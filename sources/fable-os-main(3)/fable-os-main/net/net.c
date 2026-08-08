/* net.c — lwIP netif driver for the e1000, plus the TLS transport that carries
 * the model protocol to api.anthropic.com.
 *
 * The kernel does TLS itself (mbedTLS via lwIP's altcp_tls layer), so there is
 * no host proxy: we DNS-resolve api.anthropic.com, open a TLS connection on 443
 * (with SNI), POST /v1/messages, and hand the response body back up.
 *
 * CERTIFICATE VERIFICATION is a build-time choice, default OFF.
 *
 *   plain `make`                         the historical behaviour: no CA chain,
 *                                        MBEDTLS_SSL_VERIFY_NONE. The server's
 *                                        certificate is parsed and then
 *                                        believed. Encrypted, not
 *                                        authenticated, and MITM-able by
 *                                        anything that can answer on 443.
 *   `make EXTRA_CFLAGS=-DFABLEOS_VERIFY_CERTS`
 *                                        the chain must build to one of the
 *                                        roots pinned in net/tls_ca.c, the
 *                                        certificate must name the host we
 *                                        asked for, and notBefore/notAfter must
 *                                        contain the CMOS clock's idea of now.
 *
 * Every FABLEOS_VERIFY_CERTS block below is additive; with the flag absent this
 * file compiles to exactly what it did before. See include/tls_ca.h for which
 * roots are trusted, why those, and what breaks when they rotate.
 *
 * The request is HTTP/1.0 so the response is close-delimited (no chunked
 * encoding to parse). We buffer the whole response, split off the headers, and
 * return the body.
 *
 * LAYERING — this file is deliberately the *only* place that knows about lwIP:
 *
 *     net_ask()            prompt -> JSON -> print       (protocol, no sockets)
 *       model_build_request()  net/model.c   builds & escapes the request body
 *       model_send()           net/model.c   dispatches through a transport
 *         tls_send()           here          HTTP/1.0 over altcp_tls
 *       json_msg_text()        net/json.c    reads the reply
 *
 * Swap model_tls_transport() for model_mock_transport() and the exact same
 * net_ask() logic runs on the host with no network — which is how the tool-use
 * loop will be tested.
 *
 * ============================================================================
 * ONE CONNECTION ENGINE, TWO USERS.
 * ============================================================================
 * There is a second user of everything below: tools/net_tools.c, the model's
 * general "fetch a URL" verb, through the seam in include/fetch.h. It is NOT a
 * second copy of any of this. http_exchange() below is the whole
 * connect/handshake/pump/receive/teardown machine, parameterised by destination,
 * SNI and whether TLS is used at all, and both the model transport and a general
 * fetch call it:
 *
 *     tls_send()        Anthropic framing (x-api-key!)  -> http_exchange()
 *     fetch_exchange()  arbitrary framing, no secrets   -> http_exchange()
 *
 * That matters for more than duplication. The generation-stamped callbacks, the
 * single teardown, the ERR_MEM-aware tx_pump and the "a stale connection must
 * never write this request's body" rule below were all learned the hard way, and
 * a second implementation would have had to learn them again. It also matters
 * for the secret: the engine takes an ALREADY-ASSEMBLED request buffer, so the
 * x-api-key header exists in exactly one function in this tree, and a fetch has
 * no code path that could grow one. See DECISION 2 in include/fetch.h.
 */

#include "kernel.h"
#include "net.h"
#include "e1000.h"
#include "json.h"
#include "model.h"
#include "fiber.h"
#include "fetch.h"
#include "tls_ca.h"
/* Only for CHAT_REQ_BYTES, and only so the _Static_assert on TLS_REQ_BYTES below
 * can compare the two numbers instead of a comment claiming they agree. */
#include "chat.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dns.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "mbedtls/ssl.h"

#include <stdio.h>      /* snprintf, for the one-line TLS verdict */

#ifdef FABLEOS_VERIFY_CERTS
#include "rtc.h"
#include "mbedtls/x509_crt.h"

/* The verify mode is not set here — it is set in port/lwipopts.h and consumed
 * by lwIP's altcp_tls_mbedtls.c. Refuse to build a kernel that claims to verify
 * while that knob says otherwise, so the two cannot drift apart quietly. */
#if !defined(ALTCP_MBEDTLS_AUTHMODE) || ALTCP_MBEDTLS_AUTHMODE != MBEDTLS_SSL_VERIFY_REQUIRED
#error "FABLEOS_VERIFY_CERTS requires ALTCP_MBEDTLS_AUTHMODE == MBEDTLS_SSL_VERIFY_REQUIRED (see port/lwipopts.h)"
#endif
#endif

/* Streaming is opt-in at build time (`make EXTRA_CFLAGS=-DFABLEOS_STREAM`).
 * Without it this file behaves exactly as it always has: one buffered response,
 * printed when it is complete. With it, the request carries "stream":true and
 * the response is a text/event-stream that net/sse.c turns back into the same
 * response body while printing the model's text deltas as they land. Every
 * FABLEOS_STREAM block below is additive; the default path is untouched (proven
 * by compiling this file with and without the streaming source and diffing the
 * generated code — identical).
 *
 * KNOWN GAP under the flag: net_ask() below suppresses its own second print of
 * text the transport already streamed, but net/chat.c cannot — it needs the
 * same one-line sse_http_streamed() guard in print_text_block(), and chat.c is
 * not this change's to edit. See the SCOPE section of include/sse.h. */
#ifdef FABLEOS_STREAM
#include "sse.h"
#endif

/* THE API KEY IS NOT COMPILED IN. There is no -DFABLEOS_API_KEY and there is no
 * KEY= in the Makefile, on purpose: a key that reaches the compiler reaches
 * net.o, kernel.elf, kernel.bin, fableos.iso and the .build-flags stamp, and a
 * .gitignore is the wrong place to keep a secret out of five build artifacts.
 *
 * The key arrives at RUN time over QEMU's fw_cfg channel (drivers/fwcfg/, read
 * at boot from opt/fableos/apikey) and lives only in RAM. With no key,
 * fwcfg_apikey() is "" — the header goes out empty, Anthropic answers 401, and
 * that is the documented default: it still proves DNS, TLS and HTTP worked.
 *
 * NOTHING HERE PRINTS THE REQUEST. The `req` buffer below holds the x-api-key
 * header, so any future debug dump of it is a key disclosure; if one is ever
 * added it must skip the header block. See include/fwcfg.h, THE SECRET RULE. */
#include "fwcfg.h"

/* The endpoint. Both names are overridable, but ONLY in a verification build,
 * because their only purpose is to prove that the verifier rejects things:
 *
 *   -DFABLEOS_TLS_HOST='"example.com"'    resolve and connect somewhere else, so
 *                                        the presented chain is anchored in a
 *                                        root this kernel does not trust
 *                                        -> MBEDTLS_X509_BADCERT_NOT_TRUSTED
 *   -DFABLEOS_TLS_SNI='"wrong.invalid"'   ask the verifier for a name the
 *                                        certificate does not carry
 *                                        -> MBEDTLS_X509_BADCERT_CN_MISMATCH
 *   -DFABLEOS_TLS_PORT=8443               with FABLEOS_TLS_HOST='"10.0.2.2"',
 *                                        point the kernel at a TLS server on
 *                                        the QEMU host — the offline way to
 *                                        present a self-signed certificate and
 *                                        watch it be refused. lwIP's
 *                                        dns_gethostbyname() short-circuits a
 *                                        dotted quad, so no DNS is involved.
 *
 * A verifier that has never been watched saying no is not known to work, and
 * these three lines are how that gets watched. In the default build none of the
 * macros can have any effect: the #ifdefs collapse to the original literals. */
#if defined(FABLEOS_VERIFY_CERTS) && defined(FABLEOS_TLS_HOST)
#define API_HOST   FABLEOS_TLS_HOST
#else
#define API_HOST   "api.anthropic.com"
#endif

/* The name the certificate must carry. Sent as SNI in every build; under
 * FABLEOS_VERIFY_CERTS it is additionally the name mbedTLS matches the
 * certificate's CN/subjectAltName against, which is the half of verification
 * that a CA bundle alone does not give you — a valid certificate for some
 * other host is still the wrong certificate. */
#if defined(FABLEOS_VERIFY_CERTS) && defined(FABLEOS_TLS_SNI)
#define API_SNI    FABLEOS_TLS_SNI
#else
#define API_SNI    API_HOST
#endif

#if defined(FABLEOS_VERIFY_CERTS) && defined(FABLEOS_TLS_PORT)
#define API_PORT   FABLEOS_TLS_PORT
#else
#define API_PORT   443
#endif
#define API_MODEL  "claude-opus-4-8"
#define API_TOKENS 1024

/* ====================================================================== */
/* netif driver                                                            */
/* ====================================================================== */

static struct netif e1000_netif;

static err_t e1000_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    static uint8_t buf[1600];
    if (p->tot_len > sizeof buf) return ERR_IF;
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    if (e1000_send(buf, p->tot_len) != 0) return ERR_IF;
    return ERR_OK;
}

static err_t e1000_netif_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = e1000_linkoutput;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    e1000_get_mac(netif->hwaddr);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void net_poll_rx(void) {
    static uint8_t frame[2048];
    uint16_t len;
    while (e1000_receive(frame, &len)) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        if (!p) continue;
        pbuf_take(p, frame, len);
        if (e1000_netif.input(p, &e1000_netif) != ERR_OK)
            pbuf_free(p);
    }
}

/* THE YIELD POINT THAT MATTERS.
 *
 * Every wait in this file is `while (!done && millis() < deadline)
 * net_service();`, and a model turn spends seconds in one of them. Before
 * fibers, those seconds were the whole machine: the pointer froze, windows
 * stopped repainting and an app's tick handler stopped running. Adding
 * fiber_yield() here is what makes an API call something the machine does
 * rather than something it suffers.
 *
 * WHAT IT DOES NOT FIX, so nobody reads more into it than is there: the
 * KEYBOARD is still unserviced during a turn. Only input_poll() drains it and
 * that is called from kernel/main.c's wait_for_sentence(), i.e. from the fiber
 * that is sitting in this loop. Characters typed mid-turn still queue in the
 * 8042 and appear when the turn ends, exactly as before. Moving the drain into
 * the ui fiber would give one device two owners racing for individual
 * keystrokes, which is a worse bug than a late echo; the fix is a keyboard
 * fiber that owns the port outright, and that is not this change.
 *
 * WHY *HERE* AND NOT INSIDE THE CALLERS' LOOPS. This is the one place where
 * every network wait passes through a point at which lwIP is quiescent:
 * net_poll_rx() has returned every pbuf it took to netif->input, and
 * sys_check_timeouts() has finished running the timer list. Nothing is
 * half-modified, no pbuf is held, no pcb is mid-callback. Yield one line
 * earlier — between those two calls, or from inside a callback — and another
 * fiber could observe lwIP in a state NO_SYS=1 says cannot exist.
 *
 * WHAT THE OTHER FIBER MAY NOT DO: touch the network, and — learned the hard way
 * — touch HARDWARE OF ANY KIND that can block. Nothing in this kernel calls into
 * lwIP except this file, and the ui fiber's whole body is app_tick_handlers() +
 * gui_tick(), neither of which can reach a socket (include/app.h limits an app to
 * the widget expression language plus one brokered capability call).
 *
 * It used to call app_tick(), which ends with app_service() — the one function in
 * apps/ that reaches a driver — so a model-authored document with a periodic
 * handler could make the machine play a sound from inside this loop. audio.h's
 * play path is synchronous, so lwIP went unpolled for the length of it, in the
 * middle of a TLS round trip; and apps/cap.c's stated safety argument ("nothing
 * is started during a model turn") quietly stopped being true. Splitting the pump
 * out of the handlers fixed it, in apps/runtime.c and kernel/main.c.
 *
 * If a future fiber ever wants the network, it needs its own transport state, not
 * a second caller of these statics — see the cur_pcb comment below for what
 * sharing them costs.
 *
 * COST WHEN NOTHING ELSE IS RUNNABLE: one compare and a return. */
static void net_service(void) {
    net_poll_rx();
    sys_check_timeouts();
    fiber_yield();
}

/* ====================================================================== */
/* setup: netif, TLS config, DNS                                           */
/* ====================================================================== */

static struct altcp_tls_config *tls_conf;
static ip_addr_t server_ip;
static volatile int dns_done, dns_ok;

/* Set once the netif, the default route, the DNS server and the TLS config are
 * all up, i.e. once the only thing that can still fail is the lookup itself.
 * Gates net_retry_resolve(); see the comment on resolve_api_host(). */
static int stack_up;

/* Did the API host's name last resolve? Reported by net_status (fetch_ifstat)
 * so "the model is unreachable" can be told apart from "there is no link". */
static int api_resolved;

/* A retry is paid for by an operator waiting at a prompt, so it is bounded far
 * more tightly than the boot attempt: long enough for a lost query and one
 * lwIP retransmit, short enough that a machine with no network answers quickly
 * instead of appearing hung. */
#define RETRY_TIMEOUT_MS 3000

static int resolve_api_host(uint32_t timeout_ms);

static ip_addr_t dns_result;

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; (void)arg;
    if (ipaddr) { dns_result = *ipaddr; dns_ok = 1; }
    dns_done = 1;
}

/* Resolve ANY name, bounded. Factored out of resolve_api_host() so the general
 * fetch path (include/fetch.h) uses the same resolver, the same service loop and
 * the same deadline discipline as the model transport rather than a second copy
 * that would drift. `host` must be NUL-terminated; lwIP short-circuits a dotted
 * quad without a query, which is why fetch.c can pass literals straight in.
 * Returns 0 on success. */
static int dns_lookup(const char *host, ip_addr_t *out, uint32_t timeout_ms) {
    if (!host || !host[0] || !out) return -1;

    dns_done = dns_ok = 0;
    err_t e = dns_gethostbyname(host, &dns_result, dns_cb, NULL);
    if (e == ERR_OK)                 { dns_ok = 1; dns_done = 1; }
    else if (e != ERR_INPROGRESS)    { return -1; }

    uint64_t deadline = millis() + timeout_ms;
    while (!dns_done && millis() < deadline) net_service();
    if (!dns_ok) return -1;

    *out = dns_result;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* what the current connection is, and what it proved                      */
/* ---------------------------------------------------------------------- */

/* The ssl context of the connection currently being attempted, so a handshake
 * failure can be turned into a sentence instead of "[net error -15]".
 *
 * LIFETIME, because this is a raw pointer into lwIP's allocation: it is set
 * immediately after altcp_tls_new() and cleared the moment it is read or the
 * attempt ends. lwIP calls the error callback *before* it frees the mbedTLS
 * state (altcp_mbedtls_lower_recv_process() and altcp_mbedtls_lower_err() both
 * invoke conn->err and only then close/free), so the context is still alive
 * inside err_cb — and nowhere else. Nothing outside this file may touch it.
 *
 * Not under FABLEOS_VERIFY_CERTS: record_tls_state() below needs it in EVERY
 * build, because the honest answer to "was this peer authenticated?" is exactly
 * the question a general fetch has to answer for the model, and in the default
 * build that answer is no. */
static mbedtls_ssl_context *cur_ssl;

/* The name being asked for on the connection in progress — API_SNI for the model
 * transport, the URL's host for a fetch. Used for SNI, for the verifier's
 * hostname check, and in every message about a certificate. */
static const char *xc_sni;

/* FETCH_TLS_* plus one sentence, recorded from the LIVE session at handshake
 * time. See record_tls_state(). */
static int  xc_tls_state;
static char xc_tls_note[FETCH_NOTE_MAX];

/* WHAT THIS CONNECTION ACTUALLY PROVED — asked of mbedTLS, never inferred.
 *
 * "Compiled with FABLEOS_VERIFY_CERTS" is not the same claim as "this handshake
 * verified anything", and the two live in different object files:
 * ALTCP_MBEDTLS_AUTHMODE is consumed by lwIP's altcp_tls_mbedtls.c, and one
 * stale object is enough to separate them. Worse, under VERIFY_NONE mbedTLS
 * skips verification and leaves verify_result at 0, which is byte-identical to
 * success. So the only sound source is the config the handshake really used,
 * and both halves are checked: authmode must be VERIFY_REQUIRED *and* the
 * verify result must be clean.
 *
 * tools/net_tools.c prints the resulting sentence above the body of every fetch,
 * because a model that acts on fetched content must know whether the content
 * came from the host it named or from anything that could answer on that port. */
static void record_tls_state(mbedtls_ssl_context *ssl) {
    xc_tls_state  = FETCH_TLS_NONE;
    xc_tls_note[0] = '\0';
    if (!ssl) return;

    int      mode = ssl->conf ? (int)ssl->conf->authmode : -1;
    uint32_t vr   = mbedtls_ssl_get_verify_result(ssl);

    if (mode == MBEDTLS_SSL_VERIFY_REQUIRED && vr == 0) {
        xc_tls_state = FETCH_TLS_VERIFIED;
        snprintf(xc_tls_note, sizeof xc_tls_note,
                 "chain built to one of this kernel's %d pinned roots and the "
                 "certificate names %s",
                 tls_ca_root_count(), xc_sni ? xc_sni : "?");
    } else {
        xc_tls_state = FETCH_TLS_UNVERIFIED;
        snprintf(xc_tls_note, sizeof xc_tls_note,
                 "ENCRYPTED BUT NOT AUTHENTICATED: this kernel was built "
                 "without FABLEOS_VERIFY_CERTS (mbedTLS authmode %d), so the "
                 "certificate %s presented was parsed and then believed. "
                 "Anything able to answer on this port could be the far end.",
                 mode, xc_sni ? xc_sni : "the peer");
    }
}

#ifdef FABLEOS_VERIFY_CERTS
/* ---------------------------------------------------------------------- */
/* certificate verification: setup and reporting                           */
/* ---------------------------------------------------------------------- */

/* One line describing why a certificate was refused, pointed at by tls_err.
 * Static because tls_err outlives the call that produced it. */
static char verify_line[192];

static int tls_verify_init(void) {
    /* Refuse to come up with a trust store we cannot vouch for. A bundle that
     * silently failed to parse would leave mbedTLS with an empty CA chain and
     * VERIFY_REQUIRED, i.e. every handshake failing for a reason that looks
     * like the network. Check the shape here so the message is the truth. */
    int rc = tls_ca_bundle_selftest();
    if (rc != TLS_CA_OK) {
        kprintf("net: embedded CA bundle is malformed (%d) - refusing to run "
                "with a trust store this kernel cannot parse\n", rc);
        return -1;
    }

    tls_conf = altcp_tls_create_config_client((const u8_t *)tls_ca_bundle(),
                                              tls_ca_bundle_len());
    if (!tls_conf) {
        /* lwIP folds "out of memory" and "mbedtls_x509_crt_parse rejected the
         * bundle" into one NULL, and its own diagnostics go to LWIP_DEBUGF,
         * which is compiled out here. Say both possibilities out loud. */
        kprintf("net: TLS config init failed - the CA bundle did not parse, "
                "or the heap could not hold it\n");
        return -1;
    }

    kprintf("net: certificate verification ON (%d pinned root%s, "
            "hostname + validity enforced)\n",
            tls_ca_root_count(), tls_ca_root_count() == 1 ? "" : "s");
    for (int i = 0; i < tls_ca_root_count(); i++) {
        const tls_ca_root_t *r = tls_ca_root(i);
        kprintf("net:   trust %s (%s, pin expires %s) - %s\n",
                r->name, r->key, r->not_after, r->why);
    }

    /* PROVE the date check is compiled in, do not trust that it is. Without
     * MBEDTLS_HAVE_TIME_DATE, mbedTLS compiles mbedtls_x509_time_is_past() to
     * `return 0` — every certificate is forever in date and this banner would be
     * a lie. The defines live in include/mbedtls_config.h and reach mbedTLS
     * through a different set of object files than this one, so one stale object
     * is enough to separate the claim from the behaviour. Ask a question whose
     * answer can only be yes: is 1971 in the past?
     *
     * (Not decorative. `make EXTRA_CFLAGS=-DFABLEOS_VERIFY_CERTS` in a tree that
     * was already built does not rebuild mbedTLS, so this is the difference
     * between finding out at boot and finding out never.) */
    {
        mbedtls_x509_time past;
        past.year = 1971; past.mon = 1; past.day = 1;
        past.hour = 0;    past.min = 0; past.sec = 0;
        if (!mbedtls_x509_time_is_past(&past)) {
            kprintf("net: REFUSING to claim verification: this build's mbedTLS "
                    "does not check certificate dates (MBEDTLS_HAVE_TIME_DATE "
                    "missing, or stale objects). Rebuild from clean.\n");
            altcp_tls_free_config(tls_conf);
            tls_conf = (struct altcp_tls_config *)0;
            return -1;
        }
    }

    /* The clock is half of the check, so it gets stated, not assumed. */
    int64_t   now = tls_ca_now_unix();
    rtc_time_t t;
    char       iso[RTC_ISO8601_MAX];
    if (now != TLS_CA_NO_CLOCK && rtc_from_unix(now, &t) == RTC_OK &&
        rtc_format_iso8601(&t, iso, sizeof iso) > 0) {
        kprintf("net:   validity is checked against %s (CMOS RTC, assumed UTC)\n",
                iso);
    } else {
        kprintf("net:   WARNING: no readable wall clock. Every certificate will "
                "look not-yet-valid and every handshake will fail.\n");
    }
    return 0;
}

/* Fold mbedTLS's multi-line verify_info into one line, print the full thing,
 * and hand the caller a string it can use as the transport's last error.
 * Returns NULL when the failure was not a trust failure.
 *
 * Called only from err_cb, only before lwIP frees the mbedTLS state. */
static const char *tls_verify_failure_reason(void) {
    if (!cur_ssl) return (const char *)0;
    uint32_t flags = mbedtls_ssl_get_verify_result(cur_ssl);
    cur_ssl = (mbedtls_ssl_context *)0;   /* one look; the context dies next */

    /* 0xFFFFFFFF is mbedTLS's "there is no session", i.e. we never got as far
     * as a certificate — a TCP reset, a timeout, an alert. 0 means the chain
     * verified and something else went wrong. Neither is a trust failure and
     * neither should be reported as one. */
    if (flags == 0 || flags == 0xFFFFFFFFu) return (const char *)0;

    /* Do not blame the peer for our own broken hardware. tls_ca_now_unix()
     * fails closed to the epoch when the CMOS is unreadable, which makes every
     * certificate look not-yet-valid — so BADCERT_FUTURE plus a dead clock is a
     * clock fault, and reporting it as "the certificate validity starts in the
     * future" sends the operator hunting a certificate that is perfectly fine.
     * The boot banner only covers a clock that was already dead at boot; this
     * covers one that stops later, because mbedTLS re-reads it every
     * comparison. */
    if ((flags & MBEDTLS_X509_BADCERT_FUTURE) &&
        tls_ca_now_unix() == TLS_CA_NO_CLOCK) {
        kprintf("\n[tls] cannot judge %s: the CMOS clock is unreadable, so every "
                "certificate looks not-yet-valid. This is a clock fault, not a "
                "certificate fault.\n", xc_sni ? xc_sni : "the peer");
        return "wall clock unreadable - certificate dates cannot be checked";
    }

    char info[256];
    int  n = mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
    if (n <= 0) return "certificate rejected";
    if (n > (int)sizeof info - 1) n = (int)sizeof info - 1;
    info[n] = '\0';

    /* verify_info emits one reason per line; squash to "a; b" so the single
     * line net_ask() prints carries every reason rather than the first. */
    int w = 0;
    for (int i = 0; i < n && w < (int)sizeof verify_line - 3; i++) {
        if (info[i] == '\n') {
            if (i + 1 >= n) break;                 /* trailing newline */
            verify_line[w++] = ';';
            verify_line[w++] = ' ';
        } else {
            verify_line[w++] = info[i];
        }
    }
    verify_line[w] = '\0';

    kprintf("\n[tls] certificate REJECTED for %s (verify flags 0x%x)\n",
            xc_sni ? xc_sni : "the peer", (unsigned)flags);
    kprintf("[tls] %s\n", verify_line);
    return verify_line;
}

/* On the way up, say what was actually proven. "TLS ok" is not evidence; the
 * subject, the issuer and the expiry date are. */
static void tls_report_verify_success(mbedtls_ssl_context *ssl) {
    /* Ask the live session what mode it actually ran in, rather than inferring
     * it from the flag this file was compiled with. ALTCP_MBEDTLS_AUTHMODE is
     * consumed by lwIP's altcp_tls_mbedtls.c — a different object — and under
     * VERIFY_NONE mbedTLS skips verification and leaves verify_result at 0,
     * which is indistinguishable from success. So the only honest source for
     * "was this chain actually checked" is the config the handshake used. */
    if (!ssl || !ssl->conf || ssl->conf->authmode != MBEDTLS_SSL_VERIFY_REQUIRED) {
        kprintf("[tls] NOT VERIFIED: this connection ran with authmode %d, not "
                "VERIFY_REQUIRED. The certificate was not trust-checked.\n",
                (ssl && ssl->conf) ? (int)ssl->conf->authmode : -1);
        return;
    }

    kprintf("[tls] verified: chain trusted, hostname %s matches",
            xc_sni ? xc_sni : "?");

    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(ssl);
    if (peer) {
        kprintf(", valid %d-%d-%d..%d-%d-%d UTC",
                peer->valid_from.year, peer->valid_from.mon, peer->valid_from.day,
                peer->valid_to.year,   peer->valid_to.mon,   peer->valid_to.day);
    }
    kputc('\n');
}
#endif /* FABLEOS_VERIFY_CERTS */

int net_init(void) {
    /* ARM THE CREDENTIAL GUARD FIRST, before anything below can fail and take an
     * early return with it. fetch_scan_secret() falls back to the "sk-ant-"
     * shape rule without a source, but the rule that matters — refuse any
     * request containing THIS machine's key, whatever it looks like — needs the
     * source installed. It is one assignment and it can never be the reason a
     * fetch is allowed through unchecked. See include/fetch.h DECISION 2. */
    fetch_set_secret_source(fwcfg_apikey);

    lwip_init();

    ip4_addr_t ip, nm, gw;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&nm, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    netif_add(&e1000_netif, &ip, &nm, &gw, NULL, e1000_netif_init, netif_input);
    netif_set_default(&e1000_netif);
    netif_set_up(&e1000_netif);

    ip_addr_t dnssrv;
    IP_ADDR4(&dnssrv, 10, 0, 2, 3);
    dns_setserver(0, &dnssrv);
    kprintf("net: up at 10.0.2.15/24\n");

    /* The TLS client config. Either way this is where the CTR_DRBG is seeded
     * from mbedtls_hardware_poll() — see the entropy caveat in the READMEs. */
#ifdef FABLEOS_VERIFY_CERTS
    if (tls_verify_init() != 0) return -1;
#else
    /* No CA -> handshake but no trust check. */
    tls_conf = altcp_tls_create_config_client(NULL, 0);
    if (!tls_conf) { kprintf("net: TLS config init failed\n"); return -1; }
#endif

    stack_up = 1;

    /* Bind the general network verbs (tools/net_tools.c) HERE, before the API
     * host lookup, and deliberately not after it. Resolving api.anthropic.com has
     * nothing to do with whether this machine can reach example.com or report its
     * own link state — and a boot whose model lookup failed is exactly the boot
     * where an operator most needs the machine to be able to say why. */
    fetch_set_backend(net_fetch_backend());

    return resolve_api_host(10000);
}

/* WHY THIS IS SEPARABLE AND RE-RUNNABLE.
 * Resolution is the one step in net_init() that fails for reasons that have
 * nothing to do with this machine: a dropped UDP packet to 10.0.2.3 and the
 * whole boot is over, because net_init() returns -1, chat gets a NULL transport
 * and every sentence the operator types is refused for the life of the boot. On
 * a machine whose ONLY interface is the model there is then no way to ask for a
 * retry — asking is the thing that needs the network. Observed once in ~22 boots
 * (four queries sent, no answer, a boot 40 seconds earlier resolved in 23 ms),
 * so it is rare and it is a coin flip that turns the machine into a prompt that
 * refuses everything.
 *
 * Everything above this point in net_init() — netif, default route, DNS server,
 * TLS config — either succeeds once and stays up or is a permanent property of
 * the machine, so a retry only has to redo the lookup. `stack_up` is what stops
 * a retry from being attempted on a machine that never got that far (no NIC),
 * where it would burn the timeout per sentence and could not possibly work. */
static int resolve_api_host(uint32_t timeout_ms) {
    kprintf("net: resolving %s ...\n", API_HOST);
    if (dns_lookup(API_HOST, &server_ip, timeout_ms) != 0) {
        kprintf("net: DNS resolution failed\n");
        api_resolved = 0;
        return -1;
    }
    api_resolved = 1;
    kprintf("net: %s -> %s, TLS ready\n", API_HOST, ipaddr_ntoa(&server_ip));
    return 0;
}

int net_retry_resolve(void) {
    if (!stack_up) return -1;   /* never came up; a lookup cannot help */

    /* Ask the hardware, not the software: netif_set_up() succeeds whether or not
     * a card was ever found, so lwIP's idea of "up" says nothing. A live
     * STATUS.LU read is the difference between "one query went missing, try
     * again" and "there is no network on this machine", and only the first is
     * worth making the operator wait for. Without this a NIC-less boot spends
     * the whole retry timeout per sentence before refusing, which reads as a
     * hang — measured at 3.2 s a sentence, now 0. */
    if (!e1000_link_up()) return -1;

    return resolve_api_host(RETRY_TIMEOUT_MS);
}

/* ====================================================================== */
/* the TLS transport (implements model_transport_t)                        */
/* ====================================================================== */

/* Receive state. The caller's response buffer doubles as the receive buffer:
 * headers land in it too and the body is shifted to the front afterwards, so
 * the transport needs no 64 KiB static of its own. */
static char        *rx_buf;
static int          rx_cap;
static volatile int rx_len;
static volatile int rx_overflow;

static volatile int ask_done;
static int          ask_ok;
static const char  *req_buf;
static int          req_len;

/* How much of req_buf is still waiting to go out (see tx_pump). */
static const char  *tx_ptr;
static int          tx_left;

static const char  *tls_err = "";

/* PER-ATTEMPT MODE BITS. The engine below serves two callers with different
 * needs, and these are the only differences between them:
 *
 *   xc_stream  feed received bytes to the SSE machine instead of the raw buffer.
 *              Only the model transport ever sets it, and only under
 *              FABLEOS_STREAM; a fetch is always buffered.
 *   xc_probe   there is no request and no response — the question is only
 *              "does anything complete a TCP handshake here?", so the attempt
 *              finishes the moment connected_cb fires.
 *
 * They are set by http_exchange() before any callback can run and are never
 * touched from inside one. */
static int xc_stream;
static int xc_probe;

/* The lwIP error the last failed attempt died of, kept so http_exchange() can
 * classify it for the fetch layer instead of every caller re-deriving it. */
static err_t xc_lwip_err;

/* THE CONNECTION IN PROGRESS, and how this file knows a callback belongs to it.
 *
 * Every field above is a file static shared by all attempts, which is only safe
 * as long as at most one connection is ever wired to our callbacks. lwIP frees a
 * pcb behind our back on two paths (err_cb, and a successful close in recv_cb)
 * and on no others — so any exit from tls_send() that is not one of those two
 * leaves a LIVE pcb still holding altcp_recv/altcp_sent/altcp_err. That pcb is
 * polled by net_service() for the rest of the boot, and its callbacks would then
 * run against the NEXT request's state: its ACK would make tx_pump() write this
 * request's body — x-api-key header included — into a connection the kernel has
 * already given up on, its late data would interleave into the new response
 * buffer, and its FIN would set ask_done and return a half-read reply as real.
 *
 * So: cur_pcb is the pcb this file is still responsible for, NULL once lwIP owns
 * it, and tls_send() drops any survivor before it returns. tls_gen is belt and
 * braces — it stamps each attempt onto its pcb's arg so a callback can prove it
 * belongs to the attempt in progress rather than that being a property of having
 * read every exit path correctly. */
static struct altcp_pcb *cur_pcb;
static uintptr_t         tls_gen;

/* True for a callback arriving from a connection this file has abandoned. */
static int stale_cb(const void *arg) { return (uintptr_t)arg != tls_gen; }

/* Let go of a connection unconditionally.
 *
 * Deregister BEFORE aborting: altcp_abort() runs lwIP's error path, which calls
 * conn->err and only then frees (altcp_tls_mbedtls.c's lower_err), so an armed
 * err_cb would re-enter this file and print "[net error ...]" for a connection
 * we are deliberately discarding. With the callbacks cleared, lwIP frees it
 * silently.
 *
 * Abort, not close: altcp_close() can fail with ERR_MEM when no pbuf is
 * available for the FIN, and lwIP's contract is then that the pcb is still ours
 * with every callback re-armed (it calls setup_callbacks again on that path).
 * A connection whose request is already over has nothing left to lose by dying
 * unconditionally, and "close, maybe" is exactly the leak this exists to stop.
 *
 * Never called from inside a callback: aborting a pcb from its own callback
 * obliges the callback to return ERR_ABRT, and keeping that contract at two
 * call sites is harder to check than deferring the teardown to tls_send(). */
static void tls_drop(struct altcp_pcb *pcb) {
    altcp_arg(pcb, (void *)0);
    altcp_recv(pcb, (altcp_recv_fn)0);
    altcp_sent(pcb, (altcp_sent_fn)0);
    altcp_err(pcb, (altcp_err_fn)0);
    altcp_abort(pcb);
}

#ifdef FABLEOS_STREAM
/* ~20 KiB of parser state. Static on purpose: the kernel stack is 64 KiB. */
static sse_http_t stream;

/* The model's voice, one delta at a time — the whole point of the flag. The
 * operator watches the sentence being written instead of waiting for it. */
static void stream_text(void *ctx, const char *text, size_t len) {
    (void)ctx;
    for (size_t i = 0; i < len; i++) kputc(text[i]);
}
#endif

static err_t recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)err;
    /* Not ours: a connection some earlier attempt abandoned. Consume the data so
     * lwIP's window reopens, but let none of it reach this attempt's buffer. */
    if (stale_cb(arg)) {
        if (p) { altcp_recved(pcb, p->tot_len); pbuf_free(p); }
        return ERR_OK;
    }
    if (p == NULL) {
        /* Peer FIN: the response is complete. Whether the pcb is gone depends on
         * whether lwIP could get a pbuf for our FIN — on ERR_MEM it hands the
         * connection back with every callback re-armed, so cur_pcb stays set and
         * tls_send()'s teardown finishes the job. Discarding this return is how a
         * pcb outlives the request that opened it. */
        if (altcp_close(pcb) == ERR_OK) cur_pcb = (struct altcp_pcb *)0;
        ask_done = 1;
        return ERR_OK;
    }
    for (struct pbuf *q = p; q; q = q->next) {
        const char *d = q->payload;
#ifdef FABLEOS_STREAM
        /* Straight into the state machine: it prints text deltas as they land
         * and reassembles the response body in place. Segments split an event
         * at arbitrary byte boundaries, which is sse.c's whole job.
         *
         * Only for the model transport. A fetch's response is somebody else's
         * HTML and must never be interpreted as a model event stream, nor
         * printed to the console delta by delta — hence the mode bit rather than
         * a bare #ifdef, which is what this used to be. */
        if (xc_stream) { sse_http_feed(&stream, d, (size_t)q->len); continue; }
#endif
        for (int i = 0; i < q->len; i++) {
            if (rx_len < rx_cap - 1) rx_buf[rx_len++] = d[i];
            else                     rx_overflow = 1;
        }
    }
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

/* An lwIP err_t as a sentence a model can act on. */
static const char *lwip_err_text(err_t e) {
    switch (e) {
    case ERR_RST:     return "the peer reset the connection - nothing is "
                             "listening on that port, or it hung up";
    case ERR_ABRT:    return "the connection was aborted";
    case ERR_CLSD:    return "the peer closed the connection without answering "
                             "- for an https URL that is usually a TLS "
                             "handshake the server refused";
    case ERR_CONN:    return "not connected";
    case ERR_TIMEOUT: return "the connection attempt timed out";
    case ERR_RTE:     return "no route to that address";
    case ERR_MEM:     return "the network stack ran out of memory";
    case ERR_BUF:     return "the network stack ran out of buffers";
    default:          return "the connection failed";
    }
}

static void err_cb(void *arg, err_t err) {
    if (stale_cb(arg)) return;   /* an abandoned connection finally dying */

    /* lwIP frees the pcb the moment this returns, so it is no longer ours to
     * tear down — and following the pointer again would be a use-after-free. */
    cur_pcb = (struct altcp_pcb *)0;
    xc_lwip_err = err;
#ifdef FABLEOS_VERIFY_CERTS
    /* A refused certificate arrives here as a bare ERR_CLSD, which is
     * indistinguishable from the peer hanging up. Ask mbedTLS what it decided
     * before lwIP tears the context down; if it was a trust failure, that
     * becomes the transport's error and net_ask() prints it instead of "-15". */
    const char *why = tls_verify_failure_reason();
    if (why) { tls_err = why; ask_done = 1; return; }
#endif
    /* Say what lwIP meant, not just what it numbered. Before this the transport
     * reported every one of these as the generic "TLS/connect failed", which is
     * the same sentence for "nothing is listening" and "the handshake was
     * refused" — a distinction the model needs to retry differently. */
    tls_err = lwip_err_text(err);
    kprintf("\n[net error %d]\n", err);
    ask_done = 1;
}

/* Feed as much of the request into the connection as the send buffer will take
 * right now, and stop.
 *
 * The tool-use loop's request carries every registered tool's schema, so a body
 * is tens of kilobytes where the original one-shot prompt was a few hundred
 * bytes. That is far more than TCP_SND_BUF (lwipopts.h), and a single
 * altcp_write() of the whole thing simply returns ERR_MEM and sends nothing —
 * which looks, from up here, like the server resetting the connection. So:
 * write what fits, and let the sent callback pump the rest as the peer acks. */
static void tx_pump(struct altcp_pcb *pcb) {
    while (tx_left > 0) {
        u16_t room = altcp_sndbuf(pcb);
        if (room == 0) break;

        u16_t n = (tx_left < (int)room) ? (u16_t)tx_left : room;
        u8_t  flags = TCP_WRITE_FLAG_COPY |
                      (tx_left > (int)n ? TCP_WRITE_FLAG_MORE : 0);

        err_t e = altcp_write(pcb, tx_ptr, n, flags);
        if (e == ERR_MEM) break;                 /* try again from sent_cb */
        if (e != ERR_OK) { tls_err = "write failed"; ask_done = 1; return; }

        tx_ptr  += n;
        tx_left -= n;
    }
    altcp_output(pcb);
}

static err_t sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len) {
    (void)len;
    /* The dangerous one: pumping the request into a stale connection would send
     * this attempt's body, x-api-key header and all, down a socket the kernel
     * has already reported as failed, and advance tx_ptr so the live connection
     * gets a truncated request. */
    if (stale_cb(arg)) return ERR_OK;
    tx_pump(pcb);
    return ERR_OK;
}

static err_t connected_cb(void *arg, struct altcp_pcb *pcb, err_t err) {
    if (stale_cb(arg)) return ERR_OK;
    if (err != ERR_OK) { xc_lwip_err = err; ask_done = 1; return err; }
    ask_ok = 1;                                  /* TLS handshake completed */

    /* Ask the live session what it proved, while it is still alive. Both
     * builds, because "not authenticated" is a fact a fetch has to report just
     * as loudly as "authenticated". */
    if (cur_ssl) record_tls_state(cur_ssl);
#ifdef FABLEOS_VERIFY_CERTS
    /* Reaching here under VERIFY_REQUIRED means the chain built to a pinned
     * root, the name matched, and the validity period contained now. */
    if (cur_ssl) tls_report_verify_success(cur_ssl);
#endif
    cur_ssl = (mbedtls_ssl_context *)0;

    /* A connect probe has its answer already: something completed a handshake
     * here. There is no request to send and nothing to wait for. */
    if (xc_probe) { ask_done = 1; return ERR_OK; }

    tx_ptr  = req_buf;
    tx_left = req_len;
    altcp_sent(pcb, sent_cb);
    tx_pump(pcb);
    return ERR_OK;
}

#ifndef FABLEOS_STREAM
/* Find `needle` in [p, p+n). Returns the offset, or -1. (No strstr: the buffer
 * is network data and may legitimately contain NUL bytes.) */
static int find(const char *p, int n, const char *needle, int nlen) {
    for (int i = 0; i + nlen <= n; i++) {
        int k = 0;
        while (k < nlen && p[i + k] == needle[k]) k++;
        if (k == nlen) return i;
    }
    return -1;
}

/* Split "HTTP/1.1 401 Unauthorized\r\n..." into out->status_line and
 * out->http_status, then move the body to the front of the buffer. */
static int split_http(char *buf, int len, model_response_t *out) {
    int eol = find(buf, len, "\r\n", 2);
    if (eol < 0) return MODEL_EPROTO;

    int n = eol < (int)sizeof out->status_line - 1
          ? eol : (int)sizeof out->status_line - 1;
    for (int i = 0; i < n; i++) out->status_line[i] = buf[i];
    out->status_line[n] = '\0';

    /* status code = the digits after the first space of the status line. Exactly
     * three digits: an unbounded run is signed overflow (undefined behaviour on
     * network-controlled input) and a long enough run wraps to a value that
     * passes `http_status == 200` in chat.c. */
    int sp = 0;
    while (sp < eol && buf[sp] != ' ') sp++;
    int code = 0, digits = 0, i = sp + 1;
    while (i < eol && buf[i] >= '0' && buf[i] <= '9' && digits < 3) {
        code = code * 10 + (buf[i] - '0');
        i++; digits++;
    }
    if (digits != 3) return MODEL_EPROTO;
    if (i < eol && buf[i] >= '0' && buf[i] <= '9') return MODEL_EPROTO;
    out->http_status = code;

    int hdr = find(buf, len, "\r\n\r\n", 4);
    int off = hdr >= 0 ? hdr + 4 : len;      /* no body: empty, not an error */
    int blen = len - off;
    if (blen > 0) memmove(buf, buf + off, (size_t)blen);
    buf[blen] = '\0';

    out->body     = buf;
    out->body_len = (size_t)blen;
    return MODEL_OK;
}
#endif /* !FABLEOS_STREAM — sse.c does the splitting on the streaming path */

/* ====================================================================== */
/* the connection engine — shared by the model transport and every fetch   */
/* ====================================================================== */

/* Everything one attempt differs by. Nothing else about the machinery above
 * changes between a model request and a fetch of somebody's HTML. */
typedef struct xchg {
    const ip_addr_t *ip;
    uint16_t         port;
    const char      *sni;        /* NULL => plaintext TCP, no TLS at all      */
    const char      *req;        /* ALREADY ASSEMBLED. The engine adds nothing */
    int              req_len;
    char            *rx;
    int              rx_cap;
    uint32_t         timeout_ms;
    int              stream;     /* feed the SSE machine (model transport only)*/
    int              probe;      /* finish as soon as the handshake completes  */
} xchg_t;

/* Run one attempt to completion, servicing the stack while it runs.
 *
 * THE ENGINE TAKES BYTES, NOT INTENT. It is handed a finished request buffer and
 * appends nothing of its own — no host, no credential, no framing. That is what
 * makes it safe to point at an arbitrary host: the x-api-key header is written
 * in exactly one function (tls_send, below) and there is no path from a fetch to
 * that function. See include/fetch.h DECISION 2.
 *
 * Returns MODEL_OK when a response completed, MODEL_ETIMEOUT, or
 * MODEL_ETRANSPORT. Leaves rx_len/rx_overflow describing what was received and
 * tls_err/xc_tls_state/xc_tls_note describing the peer. */
static int http_exchange(const xchg_t *x) {
    if (!x || !x->rx || x->rx_cap < 64) {
        tls_err = "response buffer too small";
        return MODEL_EINVAL;
    }
    if (x->sni && !tls_conf) {
        tls_err = "TLS not initialised";
        return MODEL_ETRANSPORT;
    }

    req_buf      = x->req;
    req_len      = x->req_len;
    rx_buf       = x->rx;
    rx_cap       = x->rx_cap;
    rx_len       = 0;
    rx_overflow  = 0;
    ask_done     = 0;
    ask_ok       = 0;
    tx_ptr       = x->req;
    tx_left      = 0;
    tls_err      = "";
    xc_stream    = x->stream;
    xc_probe     = x->probe;
    xc_sni       = x->sni;
    xc_lwip_err  = ERR_OK;
    xc_tls_state = FETCH_TLS_NONE;
    xc_tls_note[0] = '\0';
#ifdef FABLEOS_STREAM
    if (xc_stream)
        sse_http_init(&stream, x->rx, (size_t)x->rx_cap, stream_text, (void *)0);
#endif

    struct altcp_pcb *pcb = x->sni ? altcp_tls_new(tls_conf, IPADDR_TYPE_V4)
                                   : altcp_tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        tls_err = x->sni ? "could not allocate a TLS connection (out of memory)"
                         : "could not allocate a TCP connection (out of memory)";
        return MODEL_ETRANSPORT;
    }

    /* Claim it before any callback can fire, and stamp this attempt's generation
     * on it so every callback can tell whose connection it is. */
    cur_pcb = pcb;
    tls_gen++;
    altcp_arg(pcb, (void *)tls_gen);

    /* Send SNI so CDN-fronted hosts present the right certificate. Under
     * FABLEOS_VERIFY_CERTS this same string is what mbedTLS matches the
     * certificate's CN/subjectAltName against, so a failure to set it would
     * quietly downgrade verification to "signed by someone we trust, for
     * whatever name they liked" — hence the hard failure below. */
    if (x->sni) {
        mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
#ifdef FABLEOS_VERIFY_CERTS
        if (!ssl || mbedtls_ssl_set_hostname(ssl, x->sni) != 0) {
            tls_drop(pcb);
            cur_pcb = (struct altcp_pcb *)0;
            tls_err = "could not set the hostname to verify against";
            return MODEL_ETRANSPORT;
        }
#else
        if (ssl) mbedtls_ssl_set_hostname(ssl, x->sni);
#endif
        cur_ssl = ssl;
    } else {
        cur_ssl = (mbedtls_ssl_context *)0;
    }

    altcp_recv(pcb, recv_cb);
    altcp_err(pcb, err_cb);
    altcp_connect(pcb, x->ip, x->port, connected_cb);

    uint64_t deadline = millis() + x->timeout_ms;
    while (!ask_done && millis() < deadline) net_service();

    /* Past this point the pcb (and the mbedTLS state inside it) may already be
     * gone, and on the timeout path it is still alive but about to be. Either
     * way nobody may follow this pointer again. */
    cur_ssl = (mbedtls_ssl_context *)0;

    /* THE ONE TEARDOWN. Every return below is a return from a request that is
     * over, so no connection may outlive this point. On the success path recv_cb
     * already closed it and cur_pcb is NULL; every other path — the timeout, a
     * write failure, a connect lwIP refused, a close that could not get a pbuf —
     * leaves a live pcb still wired to our callbacks. See cur_pcb's comment for
     * what such a survivor does to the next request. */
    if (cur_pcb) { tls_drop(cur_pcb); cur_pcb = (struct altcp_pcb *)0; }

    /* xc_sni points at the caller's storage (a URL host on the stack, for a
     * fetch). Nothing may read it after this call, and the sentence that needed
     * it is already copied into xc_tls_note. */
    xc_sni = (const char *)0;

    if (!ask_done) { tls_err = "timed out";           return MODEL_ETIMEOUT; }
    /* Do not overwrite a reason we already have. err_cb turns mbedTLS's verify
     * flags into a sentence and parks it here; clobbering that with a generic
     * string is how "the certificate expired" used to reach the operator as
     * "TLS/connect failed". */
    if (!ask_ok)   { if (!tls_err || !tls_err[0]) tls_err = "TLS/connect failed";
                     return MODEL_ETRANSPORT; }
    return MODEL_OK;
}

/* ====================================================================== */
/* the model transport: Anthropic framing over the engine above            */
/* ====================================================================== */

/* THE OUTERMOST BOUND IN THE REQUEST-SIZE CHAIN.
 *
 * This buffer holds the request line, the headers (including x-api-key) and the
 * whole JSON body, so it has to be strictly larger than chat.h's CHAT_REQ_BYTES.
 * It used to be 64 KiB against a CHAT_REQ_BYTES of 61440, which left 25 bytes of
 * slack in the WORST-CASE request that chat.h works out — i.e. the tool registry
 * was one description away from the machine silently forgetting the middle of
 * every long job. chat.h names this buffer as the thing that has to grow first,
 * and adding the network tool family is the change that needed it.
 *
 * The relationship is asserted rather than described, because a comment that
 * says "must hold CHAT_REQ_BYTES" has already been wrong once in this tree. If
 * CHAT_REQ_BYTES moves past this, the build stops here with both numbers in the
 * message instead of the kernel getting MODEL_ENOSPC at run time. */
#define TLS_REQ_BYTES 90112
#define TLS_REQ_HEADROOM 2048   /* request line + headers + the API key */
_Static_assert(TLS_REQ_BYTES >= CHAT_REQ_BYTES + TLS_REQ_HEADROOM,
               "net.c's HTTP framing buffer must hold CHAT_REQ_BYTES plus the "
               "request line and headers - raise TLS_REQ_BYTES (net/net.c) "
               "before raising CHAT_REQ_BYTES (include/chat.h)");

static int tls_send(model_transport_t *t,
                    const char *body, size_t body_len,
                    char *resp_buf, size_t resp_cap,
                    model_response_t *out) {
    (void)t;

    if (!tls_conf) { tls_err = "TLS not initialised"; return MODEL_ETRANSPORT; }
    if (resp_cap < 64) { tls_err = "response buffer too small"; return MODEL_EINVAL; }

    /* Build the HTTP/1.0 request (close-delimited response, no chunking). The
     * JSON writer is just a bounded appender, so it serves for plain text too.
     *
     * THIS IS THE ONLY PLACE IN THE KERNEL THAT READS THE API KEY. */
    static char   req[TLS_REQ_BYTES];
    json_writer_t w;
    json_writer_init(&w, req, sizeof req);
    json_put(&w,
        "POST /v1/messages HTTP/1.0\r\n"
        "Host: " API_HOST "\r\n"
        "x-api-key: ");
    /* Runtime, from fw_cfg. Already trimmed and already checked to be printable
     * ASCII by drivers/fwcfg/fwcfg.c, so it cannot inject a CRLF and split this
     * request — the concatenation below would otherwise be exactly that hole. */
    json_put(&w, fwcfg_apikey());
    json_put(&w,
        "\r\n"
        "anthropic-version: 2023-06-01\r\n"
        "content-type: application/json\r\n"
        "connection: close\r\n"
        "content-length: ");
#ifdef FABLEOS_STREAM
    /* Ask for Server-Sent Events. net/model.c owns the body and must not learn
     * that transports exist, so the flag is spliced in as the first member of
     * the object it already built — the smallest change that leaves model.c
     * untouched. Refused unless the body really is an object with at least one
     * member; and the *response* is sniffed for its content-type regardless, so
     * a request we could not mark simply comes back unstreamed and buffered. */
    static const char STREAM_FLAG[] = "{\"stream\":true,";
    if (body && body_len >= 2 && body[0] == '{' && body[1] == '"') {
        json_put_uint(&w, (uint64_t)(body_len - 1 + sizeof STREAM_FLAG - 1));
        json_put(&w, "\r\n\r\n");
        json_put(&w, STREAM_FLAG);
        json_put_n(&w, body + 1, body_len - 1);
    } else {
        json_put_uint(&w, (uint64_t)body_len);
        json_put(&w, "\r\n\r\n");
        json_put_n(&w, body, body_len);
    }
#else
    json_put_uint(&w, (uint64_t)body_len);
    json_put(&w, "\r\n\r\n");
    json_put_n(&w, body, body_len);
#endif
    if (!json_writer_ok(&w)) { tls_err = "request too large"; return MODEL_ENOSPC; }

    xchg_t x;
    memset(&x, 0, sizeof x);
    x.ip         = &server_ip;
    x.port       = API_PORT;
    x.sni        = API_SNI;
    x.req        = req;
    x.req_len    = (int)json_writer_len(&w);
    x.rx         = resp_buf;
    x.rx_cap     = (int)(resp_cap > 0x7FFFFFFF ? 0x7FFFFFFF : resp_cap);
    x.timeout_ms = 90000;                   /* handshake + model latency */
#ifdef FABLEOS_STREAM
    x.stream     = 1;
#endif

    int xrc = http_exchange(&x);
    if (xrc != MODEL_OK) return xrc;

#ifdef FABLEOS_STREAM
    /* The body was consumed as it arrived; all that is left is to close the
     * reassembled document and hand up the same model_response_t the buffered
     * path produces. A non-SSE response (a 401, an error page) was buffered
     * verbatim by sse.c and comes back here unchanged. */
    int rc = sse_http_finish(&stream, out);
    if (rc != SSE_OK) {
        const char *why = sse_http_error(&stream);
        tls_err = (why && why[0]) ? why : sse_strerror(rc);
        return rc == SSE_ENOSPC ? MODEL_ENOSPC : MODEL_EPROTO;
    }
    return MODEL_OK;
#else
    resp_buf[rx_len] = '\0';
    int rc = split_http(resp_buf, rx_len, out);
    if (rc != MODEL_OK) { tls_err = "malformed HTTP response"; return rc; }
    out->truncated = rx_overflow;
    return MODEL_OK;
#endif
}

static const char *tls_last_error(model_transport_t *t) { (void)t; return tls_err; }

static model_transport_t tls_transport = {
    "tls", tls_send, tls_last_error, NULL
};

model_transport_t *model_tls_transport(void) {
    return tls_conf ? &tls_transport : NULL;
}

/* ====================================================================== */
/* the fetch backend (implements fetch_backend_t)                          */
/* ====================================================================== */

/* This is the whole lwIP half of include/fetch.h: four functions that hand the
 * engine above a destination and hand the answer back as plain bytes. Every
 * decision about WHAT to send, and every rule about what may not be sent, lives
 * in net/fetch.c, which knows nothing about sockets and is host-tested. */

/* Ask the hardware, not the software. netif_set_up() succeeds whether or not a
 * card was ever found, so lwIP's idea of "up" says nothing; a live STATUS.LU
 * read is the difference between "try again" and "there is no network on this
 * machine". Without this a NIC-less boot burns the whole timeout on every verb
 * before refusing, which reads as a hang — the same measurement that motivated
 * the identical check in net_retry_resolve(). */
static const char *no_link(void) {
    if (!stack_up)
        return "the network stack never came up on this machine (net_init "
               "failed) - nothing can be reached";
    if (!e1000_link_up())
        return e1000_is_suspended()
             ? "the NIC is powered down (the driver suspended it); resume it "
               "before using the network"
             : "there is no network link: no supported NIC was found, or the "
               "link is down. Call net_status for the detail.";
    return (const char *)0;
}

static void put_ip(char *dst, size_t cap, const ip_addr_t *a) {
    const char *s = a ? ipaddr_ntoa(a) : "";
    size_t i = 0;
    if (!cap) return;
    for (; s && s[i] && i + 1 < cap; i++) dst[i] = s[i];
    dst[i] = '\0';
}

static int backend_resolve(const char *host, char *ip, size_t ipcap,
                           uint32_t timeout_ms) {
    if (!host || !ip || ipcap < 8) return FETCH_EINVAL;
    if (no_link()) return FETCH_ECONNECT;

    ip_addr_t a;
    if (dns_lookup(host, &a, timeout_ms) != 0) return FETCH_EDNS;
    put_ip(ip, ipcap, &a);
    return FETCH_OK;
}

/* MODEL_* out of the engine -> FETCH_* for the model to read.
 *
 * The one judgement call is which side of a failed https attempt died. lwIP
 * hands both back as a bare error on the same callback, so the discriminator is
 * the code itself: a reset or an abort is the transport refusing to carry us at
 * all, while a clean close partway through is what a server does when it will
 * not complete a handshake. Being wrong here costs the model one wasted retry;
 * saying "it failed" costs it the ability to choose a different retry at all. */
static int classify(int rc, int tls) {
    if (rc == MODEL_OK)       return FETCH_OK;
    if (rc == MODEL_ETIMEOUT) return FETCH_ETIMEOUT;
    if (rc == MODEL_EINVAL)   return FETCH_EINVAL;
    if (tls && xc_lwip_err == ERR_CLSD) return FETCH_ETLS;
    if (tls && xc_lwip_err == ERR_OK)   return FETCH_ETLS;  /* alert, no err_cb */
    return FETCH_ECONNECT;
}

static void fill_wire(fetch_wire_t *w, int tls, uint64_t t0) {
    w->rx_len     = (size_t)rx_len;
    w->overflow   = rx_overflow;
    w->elapsed_ms = (uint32_t)(millis() - t0);
    w->tls        = tls ? xc_tls_state : FETCH_TLS_NONE;
    w->err        = (tls_err && tls_err[0]) ? tls_err : (const char *)0;

    size_t i = 0;
    for (; xc_tls_note[i] && i + 1 < sizeof w->tls_note; i++)
        w->tls_note[i] = xc_tls_note[i];
    w->tls_note[i] = '\0';
}

static int backend_exchange(const fetch_conn_t *c, fetch_wire_t *w) {
    if (!c || !w || !c->rx) return FETCH_EINVAL;

    const char *nl = no_link();
    if (nl) { w->err = nl; return FETCH_ECONNECT; }

    ip_addr_t a;
    if (!ipaddr_aton(c->ip, &a)) {
        w->err = "internal: the resolved address did not parse back";
        return FETCH_EINVAL;
    }

    uint64_t t0 = millis();
    xchg_t   x;
    memset(&x, 0, sizeof x);
    x.ip         = &a;
    x.port       = c->port;
    x.sni        = c->sni;
    x.req        = c->req;
    x.req_len    = (int)c->req_len;
    x.rx         = c->rx;
    x.rx_cap     = (int)(c->rx_cap > 0x7FFFFFFF ? 0x7FFFFFFF : c->rx_cap);
    x.timeout_ms = c->timeout_ms;

    int rc = http_exchange(&x);
    fill_wire(w, c->sni != (const char *)0, t0);
    return classify(rc, c->sni != (const char *)0);
}

static int backend_probe(const char *ip, uint16_t port, uint32_t timeout_ms,
                         fetch_wire_t *w) {
    if (!ip || !w) return FETCH_EINVAL;

    const char *nl = no_link();
    if (nl) { w->err = nl; return FETCH_ECONNECT; }

    ip_addr_t a;
    if (!ipaddr_aton(ip, &a)) {
        w->err = "internal: the resolved address did not parse back";
        return FETCH_EINVAL;
    }

    /* A probe is a plaintext TCP handshake and nothing else: no TLS (that would
     * answer a different question, and a plain-TCP service would refuse it) and
     * no request bytes, so there is nothing to scan and nothing to leak. One
     * byte of scratch, because the engine insists on a real buffer. */
    static char scratch[64];
    uint64_t t0 = millis();
    xchg_t   x;
    memset(&x, 0, sizeof x);
    x.ip         = &a;
    x.port       = port;
    x.sni        = (const char *)0;
    x.req        = "";
    x.req_len    = 0;
    x.rx         = scratch;
    x.rx_cap     = (int)sizeof scratch;
    x.timeout_ms = timeout_ms;
    x.probe      = 1;

    int rc = http_exchange(&x);
    fill_wire(w, 0, t0);
    return classify(rc, 0);
}

static int backend_ifstat(fetch_ifstat_t *out) {
    if (!out) return FETCH_EINVAL;

    e1000_get_mac(out->mac);
    /* There is no e1000_present(): the station address is read out of the card's
     * RAL/RAH registers during init and stays zero if no card was ever mapped,
     * so a nonzero MAC is the honest test for "a NIC was found". */
    out->have_nic  = (out->mac[0] | out->mac[1] | out->mac[2] |
                      out->mac[3] | out->mac[4] | out->mac[5]) != 0;
    out->link_up   = e1000_link_up();
    out->suspended = e1000_is_suspended();
    out->stack_up  = stack_up;

    out->ifname[0] = e1000_netif.name[0] ? e1000_netif.name[0] : 'e';
    out->ifname[1] = e1000_netif.name[1] ? e1000_netif.name[1] : 'n';
    out->ifname[2] = '0';
    out->ifname[3] = '\0';

    put_ip(out->ip,      sizeof out->ip,      netif_ip_addr4(&e1000_netif));
    put_ip(out->netmask, sizeof out->netmask, netif_ip_netmask4(&e1000_netif));
    put_ip(out->gateway, sizeof out->gateway, netif_ip_gw4(&e1000_netif));
    put_ip(out->dns,     sizeof out->dns,     dns_getserver(0));

    {
        const char *h = API_HOST;
        size_t i = 0;
        for (; h[i] && i + 1 < sizeof out->api_host; i++) out->api_host[i] = h[i];
        out->api_host[i] = '\0';
    }
    out->api_resolved = api_resolved;
    if (api_resolved) put_ip(out->api_ip, sizeof out->api_ip, &server_ip);

#ifdef FABLEOS_VERIFY_CERTS
    out->verify_build = 1;
#else
    out->verify_build = 0;
#endif
    out->trust_roots = tls_ca_root_count();
    return FETCH_OK;
}

static const fetch_backend_t lwip_backend = {
    "lwip+mbedtls",
    backend_resolve,
    backend_exchange,
    backend_probe,
    backend_ifstat,
};

const fetch_backend_t *net_fetch_backend(void) { return &lwip_backend; }

/* ====================================================================== */
/* net_ask — the protocol side: build, send, read, print                   */
/* ====================================================================== */

/* Print server-chosen bytes without letting them forge kernel ground truth.
 *
 * A KERNEL TRACE LINE IS A '[' IN COLUMN ZERO, and both things net_ask() prints
 * are chosen by the far end: the model's reply text, and — when the reply is not
 * a message at all — the raw HTTP body. With certificate verification off by
 * default, "the far end" is anything that can answer on 443. A bare kputs() of
 * either let a reply of "[heap_free 0x1000 -> ok]" put a line on the console
 * that is byte-identical to genuine kernel output, during the boot self-test,
 * on a machine with no shell to check it against.
 *
 * So: C0 control bytes other than '\n' and '\t' are escaped (a '\r' resets the
 * console column and a '\b' walks back into a real trace line printed earlier,
 * so cursor motion is not something untrusted text gets to do), and a '[' that
 * would land in column zero is given one leading space. The column is asked for
 * rather than modelled, because wrap and scroll reach column zero with no
 * newline involved.
 *
 * This is deliberately the same rule as net/chat.c's print_model_prose(), which
 * carries the full derivation and the regression tests. The duplication is not
 * wanted: net_ask() is itself a weaker copy of the turn loop and the fix for
 * both is to delete it and make the boot self-test a chat_ask() call, which
 * needs kernel/main.c and chat.c. Reported rather than done. */
static void print_untrusted(const char *s) {
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) {
            kputc('\\'); kputc('x');
            kputc(hex[(c >> 4) & 0xF]); kputc(hex[c & 0xF]);
            continue;
        }
        if (c == '[') {
            int col = 0;
            console_cursor((int *)0, &col);
            if (col == 0) kputc(' ');
        }
        kputc((char)c);
    }
}

int net_ask(const char *question) {
    static char req_body[8192];
    static char resp[65536];
    static char text[16384];

    int n = model_build_request(req_body, sizeof req_body,
                                API_MODEL, API_TOKENS, question);
    if (n < 0) { kprintf("\n[net: %s]\n", model_strerror(n)); return -1; }

    model_transport_t *t = model_tls_transport();
    model_response_t   r;

    int rc = model_send(t, req_body, (size_t)n, resp, sizeof resp, &r);
    if (rc != MODEL_OK) {
        const char *why = model_last_error(t);
        kprintf("\n[net: %s]\n", (why && why[0]) ? why : model_strerror(rc));
        return -1;
    }

    /* Show the HTTP status line, then the answer (or the raw JSON on error). */
    kprintf("[http] %s\n", r.status_line);

    json_value_t root;
    int prc = json_parse(r.body, r.body_len, &root);
    int trc = prc == JSON_OK ? json_msg_text(&root, text, sizeof text, NULL)
                             : prc;
#ifdef FABLEOS_STREAM
    /* Already on screen, delta by delta, as it arrived. */
    if (sse_http_streamed(&stream)) { kputc('\n'); return 0; }
#endif
    if (trc == JSON_OK || trc == JSON_ENOSPC) print_untrusted(text);
    else                                      print_untrusted(r.body);  /* a 401 */
    kputc('\n');
    return 0;
}
