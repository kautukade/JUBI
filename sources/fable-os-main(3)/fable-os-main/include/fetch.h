/* fetch.h — reaching an arbitrary host on the network, from inside the kernel.
 *
 * PURPOSE
 *   This machine has a full TCP/IP stack, DNS, and a TLS client with pinned
 *   roots, and until this file existed every byte of that was reserved for one
 *   destination: api.anthropic.com. The model could think, allocate, drive PCI
 *   devices and write drivers, but it could not answer "is the network up?" or
 *   "what does that URL say?". On a machine whose existence depends on a network
 *   round trip, that was the largest hole in the syscall surface.
 *
 *   fetch.h is the general form of what net/net.c already did for one host: pick
 *   a host, resolve it, open a connection (TLS or plain), send an HTTP request,
 *   bring a bounded response back, and say honestly what was and was not proven
 *   about the peer.
 *
 * ARCHITECTURE — WHY THERE IS A BACKEND VTABLE
 *   Everything in net/fetch.c is pure logic: URL parsing, request assembly,
 *   response splitting, size bounds, and the secret guard below. Everything that
 *   needs lwIP and mbedTLS lives behind fetch_backend_t, and net/net.c is the
 *   one implementation of it — the SAME callbacks, the same tx_pump/sent_cb
 *   pair, the same generation-stamped teardown and the same certificate
 *   reporting that carry the model protocol. There is exactly one connection
 *   engine in this kernel and both users go through it.
 *
 *   The seam is not decoration. Host tests supply a synthetic backend and drive
 *   every path — hostile URLs, truncation, timeouts, refused connections, a
 *   response with no headers — with no network, no QEMU and no API key
 *   (tests/host/test_net_tools.c).
 *
 *       tools/net_tools.c   the model's four verbs
 *         net/fetch.c       parse, assemble, bound, split, guard   (host-testable)
 *           fetch_backend_t
 *             net/net.c     lwIP + altcp + altcp_tls               (kernel only)
 *
 * ============================================================================
 * DECISION 1: WHAT AN HTTPS FETCH PROVES ABOUT THE PEER, AND HOW THE MODEL IS
 * TOLD.
 * ============================================================================
 *   Certificate verification in this kernel is flag-gated to a handful of roots
 *   pinned for api.anthropic.com (net/tls_ca.c, FABLEOS_VERIFY_CERTS). Neither
 *   available state is right for an arbitrary host: the default build trusts
 *   nothing and verifies nothing, and the verifying build trusts only Anthropic's
 *   issuers, so a fetch of example.com fails there with BADCERT_NOT_TRUSTED.
 *
 *   Three options were on the table. Refusing HTTPS to unpinned hosts makes the
 *   tool useless in the build people actually run. Carrying a full CA bundle is
 *   ~200 KiB of roots this project cannot maintain, cannot revoke, and cannot
 *   check against a clock it only half trusts — a trust store nobody audits is
 *   worse than an admitted absence of one, because it converts "unknown" into a
 *   confident "verified".
 *
 *   SO: fetch never refuses on trust grounds, and NEVER reports a connection as
 *   authenticated unless it was. Every result carries a fetch_result_t.tls state
 *   read from the LIVE mbedTLS session's authmode and verify result — not from
 *   the flag this file was compiled with, because those are different objects and
 *   have drifted before (see net/net.c's tls_report_verify_success). The three
 *   states are FETCH_TLS_NONE, FETCH_TLS_UNVERIFIED and FETCH_TLS_VERIFIED, and
 *   tools/net_tools.c prints the corresponding sentence at the TOP of every
 *   result, before the body, in words a model will not skim past. The model may
 *   act on fetched content, so it has to know whether the content came from the
 *   host it asked for or from anything that could answer on port 443.
 *
 * ============================================================================
 * DECISION 2: THE API KEY CANNOT LEAVE THROUGH HERE.
 * ============================================================================
 *   The Anthropic key lives in RAM, delivered over fw_cfg (include/fwcfg.h, THE
 *   SECRET RULE). A general "send these bytes to a host of your choosing" verb is
 *   the textbook exfiltration primitive, so three independent things stop it:
 *
 *   1. STRUCTURAL. Nothing on this path can read the key. fwcfg_apikey() has one
 *      caller in the whole tree — the x-api-key header in net/net.c's tls_send()
 *      — and the connection engine those two share takes an ALREADY-ASSEMBLED
 *      request buffer. fetch_build_request() writes every byte of a fetch's
 *      request itself and has no access to a credential of any kind. There is no
 *      code path on which a fetch grows an Anthropic header.
 *      Verify:  grep -rn 'fwcfg_apikey' --include=*.c .   (expect net/net.c and
 *      drivers/fwcfg/fwcfg.c only)
 *
 *   2. CONTENT. The model can still put bytes in a URL or a request body, and
 *      this kernel has a memory-peek tool, so "the model found the key in RAM and
 *      typed it into a fetch" is a real path. fetch_scan_secret() is run over the
 *      COMPLETE assembled request — request line, every header, and the body — as
 *      the last step before it reaches the wire, and a hit aborts the fetch with
 *      FETCH_ESECRET. It looks for two things: the live key verbatim (via the
 *      source installed by fetch_set_secret_source(), which is fwcfg_apikey in
 *      the kernel), and any "sk-ant-" token at all, because no legitimate fetch
 *      this machine makes needs to carry an Anthropic credential. The error names
 *      the offence and echoes NONE of the matched bytes.
 *
 *   3. DESTINATION. A request is never sent to a host the fetch did not resolve
 *      itself, and the Host: header is derived from the parsed URL rather than
 *      supplied. URL parsing rejects CR, LF, NUL and every other C0 control byte
 *      in the host and path, so a model cannot splice a second request (or a
 *      second header) into the first — the classic way one connection is made to
 *      carry a header it was never given.
 *
 *   WHAT THIS DOES NOT STOP, stated plainly because a security note that only
 *   lists its wins is a lie: a model that can read the key can also send it one
 *   character per fetch, or base64 it, or hide it in a path. Nothing short of not
 *   having a general fetch verb stops a covert channel, and the honest mitigation
 *   is upstream — the key should not be readable by the model at all. That is a
 *   memory-tool and privilege question, not a fetch question, and it is reported
 *   rather than solved here.
 *
 * PUBLIC API
 *   fetch_url_parse       "https://h:443/p?q" -> host/port/path, or a reason.
 *   fetch                 one bounded HTTP/HTTPS exchange.
 *   fetch_dns             resolve a hostname to a dotted quad.
 *   fetch_tcp_probe       is anything listening on host:port?
 *   fetch_ifstat          link, addresses, and what TLS can prove today.
 *   fetch_strerror        a FETCH_* code as a sentence.
 *   fetch_scan_secret     does this byte range carry a credential? (exported so
 *                         the host suite can prove the guard, not just its use)
 *   fetch_set_backend / fetch_set_secret_source — the two injection points.
 *
 * DEPENDENCIES
 *   <stdint.h>, <stddef.h> and nothing else. Deliberately not kernel.h, not
 *   lwIP, not mbedTLS: net/fetch.c and tools/net_tools.c both compile on the
 *   host, which is the whole reason the backend exists.
 *
 * FUTURE EXTENSION POINTS
 *   - A real trust store belongs behind fetch_backend_t.exchange as a per-request
 *     choice of altcp_tls config, so a fetch could opt into a bundle without
 *     changing the model transport's pinned one. FETCH_TLS_VERIFIED already
 *     carries the vocabulary.
 *   - Chunked transfer-encoding: requests go out as HTTP/1.0 so the response is
 *     close-delimited and there is nothing to de-chunk. A server that ignores
 *     that and chunks anyway is reported as-is; fetch_split_response() is where a
 *     de-chunker would go.
 *   - Redirects are reported, never followed. Following one means re-running the
 *     whole policy (scheme, secret scan, host) against a server-chosen URL, which
 *     is a decision the model should make out loud rather than one the kernel
 *     makes silently.
 */
#ifndef FETCH_H
#define FETCH_H

#include <stdint.h>
#include <stddef.h>

/* ---- error codes (errno-flavoured negatives; a space apart from MODEL_ and
 * VFS_ so a code can never be mistaken for one of theirs) ---- */
#define FETCH_OK            0
#define FETCH_EINVAL      -22   /* bad arguments                                */
#define FETCH_ENOSPC      -28   /* a caller buffer was too small                */
#define FETCH_EURL       -140   /* the URL did not parse                        */
#define FETCH_ESCHEME    -141   /* scheme is not http or https                  */
#define FETCH_EDNS       -142   /* the hostname did not resolve                 */
#define FETCH_ECONNECT   -143   /* TCP refused / unreachable / reset            */
#define FETCH_ETLS       -144   /* the TLS handshake failed                     */
#define FETCH_ETIMEOUT   -145   /* no complete response before the deadline     */
#define FETCH_EPROTO     -146   /* the reply was not intelligible HTTP          */
#define FETCH_ENOBACKEND -147   /* no network backend bound (no NIC / host test)*/
#define FETCH_ESECRET    -148   /* the request carried a credential — refused   */
#define FETCH_EPOLICY    -149   /* refused by policy before any packet was sent */

/* ---- bounds. All of these are hard caps, not hints. ---- */
#define FETCH_HOST_MAX      256   /* DNS names top out at 253                   */
#define FETCH_PATH_MAX     1024   /* path + query, as sent                      */
#define FETCH_URL_MAX      2048   /* the whole URL the model may hand us        */
#define FETCH_IP_MAX         46   /* room for an IPv6 text form, one day        */
#define FETCH_NOTE_MAX      192   /* one sentence about the peer                */
#define FETCH_STATUS_MAX    128   /* "HTTP/1.1 404 Not Found"                   */
#define FETCH_HDRS_MAX      512   /* the response headers we keep, truncated    */

/* Longest request body the model may supply. Small on purpose: this exists so a
 * POST of a JSON document works, not so the machine can be a file uploader, and
 * every byte of it is scanned for credentials before it goes out. */
#define FETCH_BODY_MAX     4096

/* ---- what was proven about the peer ---- */
#define FETCH_TLS_NONE        0   /* plain HTTP: no encryption, no identity     */
#define FETCH_TLS_UNVERIFIED  1   /* encrypted; the peer's identity was NOT
                                   * checked. Anything that can answer on that
                                   * port could be the far end.                 */
#define FETCH_TLS_VERIFIED    2   /* chain built to a root this kernel trusts
                                   * AND the certificate names this host        */

/* ---- a parsed URL ---- */
typedef struct fetch_url {
    int      https;                  /* 1 = TLS                                 */
    char     host[FETCH_HOST_MAX];   /* lowercased, no port, no userinfo        */
    uint16_t port;                   /* explicit, or the scheme default         */
    int      explicit_port;          /* did the URL say so?                     */
    char     path[FETCH_PATH_MAX];   /* origin-form; always starts with '/'     */
    int      is_ip;                  /* host is a dotted quad, so no DNS needed */
} fetch_url_t;

/* Parse `url` (a span, NOT required to be NUL-terminated) into *out.
 * On failure returns FETCH_EURL/FETCH_ESCHEME/FETCH_ENOSPC and, if `why` is
 * non-NULL, points it at a static sentence naming the exact problem — the model
 * reads that sentence and is expected to retry with a corrected URL. */
int fetch_url_parse(const char *url, size_t len, fetch_url_t *out,
                    const char **why);

/* ---- one exchange ---- */
typedef struct fetch_options {
    const char *method;      /* NULL => "GET". Uppercase token, <= 8 chars.     */
    const char *body;        /* NULL => none. Sent verbatim.                    */
    size_t      body_len;
    const char *content_type;/* NULL => "application/json" when a body is given */
    const char *accept;      /* NULL => "*<slash>*"                             */
    uint32_t    timeout_ms;  /* 0 => FETCH_TIMEOUT_DEFAULT_MS                   */
    size_t      max_bytes;   /* 0 => the whole buffer. Caps what is received.   */
} fetch_options_t;

#define FETCH_TIMEOUT_DEFAULT_MS  15000
#define FETCH_TIMEOUT_MAX_MS      60000

typedef struct fetch_result {
    int      http_status;                 /* 200, 404, ... 0 if none parsed     */
    char     status_line[FETCH_STATUS_MAX];
    char     headers[FETCH_HDRS_MAX];     /* response headers, clipped          */
    int      headers_truncated;
    char    *body;                        /* into the caller's buffer, NUL-term */
    size_t   body_len;
    int      truncated;                   /* the peer had more to say than fit  */
    size_t   received;                    /* bytes taken off the wire, capped   */
    int      tls;                         /* FETCH_TLS_*                        */
    char     tls_note[FETCH_NOTE_MAX];    /* what was, or was not, proven       */
    char     peer_ip[FETCH_IP_MAX];
    uint16_t peer_port;
    uint32_t elapsed_ms;
    char     location[FETCH_PATH_MAX];    /* a 3xx Location:, reported not taken*/
} fetch_result_t;

/* Fetch `url` into `buf`. On FETCH_OK, out->body points at buf and is
 * NUL-terminated. `why` (optional) receives a static sentence on every failure.
 * The buffer is used as scratch for headers too, so it must hold both. */
int fetch(const char *url, size_t url_len, const fetch_options_t *opt,
          char *buf, size_t cap, fetch_result_t *out, const char **why);

/* ---- DNS ---- */
int fetch_dns(const char *host, size_t hlen, uint32_t timeout_ms,
              char *ip, size_t ipcap, const char **why);

/* ---- TCP connect probe ---- */
typedef struct fetch_probe {
    char     ip[FETCH_IP_MAX];
    uint16_t port;
    int      open;             /* 1 = a peer completed the handshake            */
    uint32_t elapsed_ms;
} fetch_probe_t;

int fetch_tcp_probe(const char *host, size_t hlen, uint16_t port,
                    uint32_t timeout_ms, fetch_probe_t *out, const char **why);

/* ---- interface / link status ---- */
typedef struct fetch_ifstat {
    int      have_nic;          /* a NIC was found and initialised              */
    int      link_up;           /* live read, right now                         */
    int      suspended;         /* the driver has the card powered down         */
    uint8_t  mac[6];
    char     ifname[8];
    char     ip[FETCH_IP_MAX];
    char     netmask[FETCH_IP_MAX];
    char     gateway[FETCH_IP_MAX];
    char     dns[FETCH_IP_MAX];
    int      stack_up;          /* lwIP, the route and the TLS config are up    */
    char     api_host[FETCH_HOST_MAX];
    int      api_resolved;
    char     api_ip[FETCH_IP_MAX];
    int      verify_build;      /* was this kernel built with FABLEOS_VERIFY_CERTS? */
    int      trust_roots;       /* how many roots the trust store carries       */
} fetch_ifstat_t;

int fetch_ifstat(fetch_ifstat_t *out);

/* ---- errors ---- */
const char *fetch_strerror(int rc);
const char *fetch_tls_word(int state);   /* "none" | "unverified" | "verified"  */

/* ======================================================================
 * THE SECRET GUARD
 * ====================================================================== */

/* Install the function that returns this machine's API key ("" when there is
 * none). The kernel passes fwcfg_apikey; host tests pass their own. Until it is
 * installed, fetch_scan_secret() still enforces the "sk-ant-" rule, so the guard
 * is never entirely absent. Passing NULL removes the source. */
void fetch_set_secret_source(const char *(*fn)(void));

/* Does [buf, buf+len) carry a credential that must not leave this machine?
 * Returns 0 for clean, 1 for "contains the live API key", 2 for "contains an
 * Anthropic-shaped token". Never logs, never returns any matched byte. */
int fetch_scan_secret(const char *buf, size_t len);

/* ======================================================================
 * THE BACKEND SEAM
 * ====================================================================== */

/* One connection's worth of instruction to the backend. The request is
 * ALREADY ASSEMBLED and already scanned; the backend adds nothing to it. */
typedef struct fetch_conn {
    const char *ip;          /* dotted quad; the backend does no name lookup    */
    uint16_t    port;
    const char *sni;         /* NULL => plaintext TCP; else TLS with this SNI   */
    const char *req;
    size_t      req_len;
    char       *rx;
    size_t      rx_cap;      /* never write more than this, minus the NUL       */
    uint32_t    timeout_ms;
} fetch_conn_t;

/* What the backend reports back about the bytes it moved. */
typedef struct fetch_wire {
    size_t      rx_len;
    int         overflow;               /* more arrived than rx_cap allowed     */
    int         tls;                    /* FETCH_TLS_*                          */
    char        tls_note[FETCH_NOTE_MAX];
    uint32_t    elapsed_ms;
    const char *err;                    /* static sentence, NULL on success     */
} fetch_wire_t;

typedef struct fetch_backend {
    const char *name;
    /* Resolve `host` to a dotted quad. FETCH_OK or FETCH_EDNS. */
    int (*resolve)(const char *host, char *ip, size_t ipcap, uint32_t timeout_ms);
    /* Run one request/response. FETCH_OK or FETCH_E{CONNECT,TLS,TIMEOUT}. */
    int (*exchange)(const fetch_conn_t *c, fetch_wire_t *w);
    /* Connect, then hang up. FETCH_OK when a peer answered. */
    int (*probe)(const char *ip, uint16_t port, uint32_t timeout_ms,
                 fetch_wire_t *w);
    /* Fill in what this machine's network looks like. */
    int (*ifstat)(fetch_ifstat_t *out);
} fetch_backend_t;

void                   fetch_set_backend(const fetch_backend_t *b);
const fetch_backend_t *fetch_backend(void);

/* The kernel's lwIP/mbedTLS backend, implemented in net/net.c. NULL until
 * net_init() has brought the stack up. Declared here rather than in net.h so
 * that the only header a fetch user includes is this one. */
const fetch_backend_t *net_fetch_backend(void);

/* ---- exposed for the host suite: the pure steps, individually testable ---- */

/* Assemble the request text. Returns its length, or FETCH_ENOSPC. */
int fetch_build_request(char *dst, size_t cap, const fetch_url_t *u,
                        const fetch_options_t *opt);

/* Split a raw HTTP response in place: status line, headers, body moved to the
 * front of `buf`. FETCH_OK or FETCH_EPROTO. */
int fetch_split_response(char *buf, size_t len, fetch_result_t *out);

#endif /* FETCH_H */
