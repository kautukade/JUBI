/* fetch.c — the general network-access path: parse a URL, assemble a request,
 * bound the answer, and say honestly what the peer proved.
 *
 * Everything here is pure logic over byte buffers. The sockets, the DNS
 * resolver, the TLS session and the NIC live behind fetch_backend_t and are
 * implemented once, in net/net.c, by the same connection engine that carries the
 * model protocol. That split is what makes hostile-URL handling, truncation,
 * timeouts and the credential guard testable natively — see
 * tests/host/test_net_tools.c, which supplies a synthetic backend and never
 * opens a socket.
 *
 * Read include/fetch.h first: it carries the two decisions this file implements
 * (what an HTTPS fetch is allowed to claim about the peer, and why the API key
 * cannot leave through here).
 *
 * LAYOUT
 *   1. small byte helpers (no libc assumptions beyond string.h)
 *   2. the credential guard
 *   3. URL parsing
 *   4. request assembly
 *   5. response splitting
 *   6. the public verbs: fetch, fetch_dns, fetch_tcp_probe, fetch_ifstat
 *
 * ALL INPUT IS UNTRUSTED. Every span that reaches this file came out of a
 * language model or off the network. Nothing below indexes a buffer without a
 * length, nothing assumes NUL termination on an input span, and every rejection
 * produces a sentence precise enough for the model to fix its next attempt —
 * "URL rejected" teaches nothing, "port 99999 is above 65535" teaches the fix.
 */

#include "fetch.h"

#include <stdarg.h>
#include <stdio.h>      /* vsnprintf — port/stdio.h freestanding, libc on host */
#include <string.h>

/* ====================================================================== */
/* 1. helpers                                                             */
/* ====================================================================== */

/* One rejection sentence at a time. Static because `why` outlives the call that
 * produced it and every caller reads it immediately; this kernel is
 * single-threaded and non-reentrant by construction (IF is 0, everything is
 * polled), so there is never a second message in flight. */
static char why_buf[224];

static const char *whyf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why_buf, sizeof why_buf, fmt, ap);
    va_end(ap);
    return why_buf;
}

/* Report `w` through the optional out-parameter and return `rc` in one step, so
 * no error path can return a code without also leaving a reason behind. */
static int fail(const char **why, const char *w, int rc) {
    if (why) *why = w;
    return rc;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Find `needle` in [hay, hay+n). Byte-wise: network data and model output both
 * legitimately contain NUL, so nothing here may use strstr. Returns -1 if
 * absent. `ci` makes the comparison ASCII-case-insensitive. */
static long find_bytes(const char *hay, size_t n,
                       const char *needle, size_t nlen, int ci) {
    if (nlen == 0 || nlen > n) return -1;
    for (size_t i = 0; i + nlen <= n; i++) {
        size_t k = 0;
        while (k < nlen) {
            char a = hay[i + k], b = needle[k];
            if (ci) { a = (char)lc((unsigned char)a); b = (char)lc((unsigned char)b); }
            if (a != b) break;
            k++;
        }
        if (k == nlen) return (long)i;
    }
    return -1;
}

/* Copy at most cap-1 bytes and always NUL-terminate. Returns 1 if it all fit. */
static int copy_z(char *dst, size_t cap, const char *src, size_t n) {
    if (cap == 0) return 0;
    size_t k = n < cap - 1 ? n : cap - 1;
    if (k) memcpy(dst, src, k);
    dst[k] = '\0';
    return k == n;
}

/* Printable US-ASCII, excluding space and DEL. This is the only byte class
 * allowed anywhere in a URL, a header value or a method, and it is what makes
 * CR/LF request splitting impossible rather than merely unlikely. */
static int url_byte_ok(unsigned char c) { return c >= 0x21 && c <= 0x7E; }

/* A header value may carry spaces but never CR, LF, NUL or any other control
 * byte — the same rule, one class wider. */
static int hdr_byte_ok(unsigned char c) { return c >= 0x20 && c <= 0x7E; }

static int hdr_value_ok(const char *s, size_t max) {
    if (!s) return 1;
    size_t i = 0;
    for (; s[i]; i++) {
        if (i >= max) return 0;
        if (!hdr_byte_ok((unsigned char)s[i])) return 0;
    }
    return i > 0;
}

/* ====================================================================== */
/* 2. the credential guard                                                */
/* ====================================================================== */

static const char *(*secret_src)(void);

void fetch_set_secret_source(const char *(*fn)(void)) { secret_src = fn; }

/* A key shorter than this is not treated as a secret to search for. An empty or
 * one-character "key" would match somewhere in almost every request and turn the
 * guard into a denial of every fetch, which is how a safety check gets disabled
 * by whoever has to use the machine. Real Anthropic keys are ~100 characters. */
#define SECRET_MIN_LEN 12

int fetch_scan_secret(const char *buf, size_t len) {
    if (!buf || len == 0) return 0;

    /* 1. the live key, verbatim. This is the one that matters: it is the only
     *    string whose disclosure is unrecoverable. */
    if (secret_src) {
        const char *k = secret_src();
        if (k) {
            size_t kl = strlen(k);
            if (kl >= SECRET_MIN_LEN && find_bytes(buf, len, k, kl, 0) >= 0)
                return 1;
        }
    }

    /* 2. anything Anthropic-shaped, case-insensitively. Defence in depth: it
     *    catches a key this machine holds in a form the source above does not
     *    return (a different account's, one the model invented, one the operator
     *    pasted into a prompt), and no legitimate fetch from this kernel has any
     *    reason to carry such a token. */
    if (find_bytes(buf, len, "sk-ant-", 7, 1) >= 0) return 2;

    return 0;
}

/* ====================================================================== */
/* 3. URL parsing                                                         */
/* ====================================================================== */

/* Validate and normalise a bare hostname (no scheme, no port, no path).
 * Lowercases into `out`. Sets *is_ip when every label is numeric and there are
 * four of them, each 0..255 — which the backend can connect to without DNS. */
static int parse_host(const char *h, size_t n, char *out, size_t cap,
                      int *is_ip, const char **why) {
    if (n == 0) return fail(why, "the host part of the URL is empty", FETCH_EURL);

    /* A single trailing dot is a legal fully-qualified name; drop it so the
     * Host: header and the DNS query agree with everything else. */
    if (n > 1 && h[n - 1] == '.') n--;

    if (n > 253)
        return fail(why, whyf("host is %u bytes; DNS names stop at 253",
                              (unsigned)n), FETCH_EURL);
    if (n >= cap)
        return fail(why, "host does not fit this kernel's host buffer",
                    FETCH_ENOSPC);

    if (h[0] == '[')
        return fail(why, "IPv6 literals are not supported: this kernel's lwIP "
                         "build is IPv4-only. Use an IPv4 address or a name "
                         "with an A record.", FETCH_EURL);

    int label = 0, labels = 0, numeric_labels = 0, all_numeric = 1;
    unsigned octet = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)h[i];
        if (c == '.') {
            if (label == 0)
                return fail(why, "host has an empty label (\"..\" or a leading "
                                 "dot)", FETCH_EURL);
            labels++;
            if (label <= 3 && octet <= 255) numeric_labels++;
            label = 0; octet = 0;
            out[i] = '.';        /* the separator is part of the name too */
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return fail(why, whyf("host contains byte 0x%x at offset %u; only "
                                  "letters, digits, '-', '_' and '.' are allowed",
                                  (unsigned)c, (unsigned)i), FETCH_EURL);
        if (c >= '0' && c <= '9') octet = octet * 10 + (unsigned)(c - '0');
        else                      all_numeric = 0;
        if (++label > 63)
            return fail(why, "host has a label longer than 63 bytes", FETCH_EURL);
        out[i] = (char)lc(c);
    }
    if (label == 0)
        return fail(why, "host has an empty label (\"..\" or a trailing dot)",
                    FETCH_EURL);
    labels++;
    if (label <= 3 && octet <= 255) numeric_labels++;

    out[n] = '\0';
    if (is_ip) *is_ip = (all_numeric && labels == 4 && numeric_labels == 4);
    return FETCH_OK;
}

int fetch_url_parse(const char *url, size_t len, fetch_url_t *out,
                    const char **why) {
    if (!url || !out) return fail(why, "no URL given", FETCH_EINVAL);

    memset(out, 0, sizeof *out);

    /* Trim ASCII whitespace at both ends: a model pasting a URL out of prose
     * very often brings a trailing newline with it, and refusing that teaches
     * nothing useful. Whitespace INSIDE the URL is still fatal. */
    while (len && (url[0] == ' ' || url[0] == '\t' || url[0] == '\r' ||
                   url[0] == '\n')) { url++; len--; }
    while (len && (url[len-1] == ' ' || url[len-1] == '\t' ||
                   url[len-1] == '\r' || url[len-1] == '\n')) len--;

    if (len == 0) return fail(why, "the URL is empty", FETCH_EURL);
    if (len > FETCH_URL_MAX)
        return fail(why, whyf("the URL is %u bytes; the limit is %d",
                              (unsigned)len, FETCH_URL_MAX), FETCH_EURL);

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (!url_byte_ok(c))
            return fail(why, whyf("the URL contains byte 0x%x at offset %u; a "
                                  "URL must be printable ASCII with no spaces, "
                                  "control bytes or non-ASCII. Percent-encode it.",
                                  (unsigned)c, (unsigned)i), FETCH_EURL);
    }

    /* ---- scheme ---- */
    long sep = find_bytes(url, len, "://", 3, 0);
    if (sep < 0)
        return fail(why, "no scheme: the URL must start with http:// or "
                         "https://. This kernel will not guess which, because "
                         "the difference is whether the traffic is encrypted.",
                    FETCH_ESCHEME);
    if (sep == 5 && find_bytes(url, 5, "https", 5, 1) == 0)      out->https = 1;
    else if (sep == 4 && find_bytes(url, 4, "http", 4, 1) == 0)  out->https = 0;
    else {
        char s[16];
        copy_z(s, sizeof s, url, (size_t)sep);
        return fail(why, whyf("scheme \"%s\" is not supported; only http and "
                              "https are.", s), FETCH_ESCHEME);
    }
    out->port = out->https ? 443 : 80;

    const char *rest = url + sep + 3;
    size_t      rlen = len - (size_t)sep - 3;

    /* ---- authority ---- */
    size_t alen = 0;
    while (alen < rlen && rest[alen] != '/' && rest[alen] != '?' &&
           rest[alen] != '#') alen++;

    /* Before the port split, not after: "[::1]:80" would otherwise be reported
     * as `port "1]" is not a number`, which sends the model looking for a typo
     * in a URL whose real problem is that this kernel has no IPv6. */
    if (alen && rest[0] == '[')
        return fail(why, "IPv6 literals are not supported: this kernel's lwIP "
                         "build is IPv4-only. Use an IPv4 address or a name "
                         "with an A record.", FETCH_EURL);

    if (find_bytes(rest, alen, "@", 1, 0) >= 0)
        return fail(why, "userinfo (\"user:pass@host\") in a URL is refused: it "
                         "is a credential in a field this kernel would put on "
                         "the wire and in a log. Use a header instead.",
                    FETCH_EPOLICY);

    /* Port: the LAST colon in the authority, so a malformed "a:b:c" is reported
     * as a bad port rather than silently accepted as part of the host. */
    size_t hlen = alen;
    long   colon = -1;
    for (size_t i = 0; i < alen; i++) if (rest[i] == ':') colon = (long)i;
    if (colon >= 0) {
        hlen = (size_t)colon;
        size_t plen = alen - (size_t)colon - 1;
        const char *p = rest + colon + 1;
        if (plen == 0)
            return fail(why, "the URL ends with ':' but names no port",
                        FETCH_EURL);
        if (plen > 5)
            return fail(why, "the port has more than five digits; the maximum "
                             "is 65535", FETCH_EURL);
        unsigned v = 0;
        for (size_t i = 0; i < plen; i++) {
            if (p[i] < '0' || p[i] > '9') {
                char s[8];
                copy_z(s, sizeof s, p, plen);
                return fail(why, whyf("port \"%s\" is not a number", s),
                            FETCH_EURL);
            }
            v = v * 10 + (unsigned)(p[i] - '0');
        }
        if (v == 0 || v > 65535)
            return fail(why, whyf("port %u is outside 1..65535", v), FETCH_EURL);
        out->port = (uint16_t)v;
        out->explicit_port = 1;
    }

    int rc = parse_host(rest, hlen, out->host, sizeof out->host,
                        &out->is_ip, why);
    if (rc != FETCH_OK) return rc;

    /* ---- path (origin-form: path + query, fragment dropped) ---- */
    const char *pth = rest + alen;
    size_t      plen = rlen - alen;
    for (size_t i = 0; i < plen; i++) if (pth[i] == '#') { plen = i; break; }

    if (plen == 0) { out->path[0] = '/'; out->path[1] = '\0'; return FETCH_OK; }
    if (pth[0] == '?') {
        /* "http://h?q" — legal, and the origin-form needs the slash back. */
        if (plen + 1 >= sizeof out->path)
            return fail(why, whyf("the path+query is %u bytes; the limit is %d",
                                  (unsigned)plen + 1, FETCH_PATH_MAX - 1),
                        FETCH_ENOSPC);
        out->path[0] = '/';
        memcpy(out->path + 1, pth, plen);
        out->path[plen + 1] = '\0';
        return FETCH_OK;
    }
    if (!copy_z(out->path, sizeof out->path, pth, plen))
        return fail(why, whyf("the path+query is %u bytes; the limit is %d",
                              (unsigned)plen, FETCH_PATH_MAX - 1), FETCH_ENOSPC);
    return FETCH_OK;
}

/* ====================================================================== */
/* 4. request assembly                                                    */
/* ====================================================================== */

/* The methods a fetch may use. An allowlist rather than a syntax check: the
 * method is the first token on the first line, so anything unexpected there is
 * the cheapest possible request-smuggling primitive, and there is no legitimate
 * reason for this machine to invent a verb. */
static const char *const METHODS[] = {
    "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", 0
};

static const char *method_ok(const char *m) {
    if (!m || !m[0]) return "GET";
    for (int i = 0; METHODS[i]; i++) {
        const char *a = METHODS[i];
        size_t k = 0;
        while (a[k] && m[k] && lc((unsigned char)a[k]) == lc((unsigned char)m[k])) k++;
        if (!a[k] && !m[k]) return METHODS[i];
    }
    return (const char *)0;
}

/* An appender that cannot overflow and remembers that it tried to. Deliberately
 * NOT json.h's writer: nothing here is JSON, and borrowing a JSON escaper for
 * HTTP framing is how a "\r\n" ends up escaped into visibility instead of
 * refused. */
typedef struct { char *b; size_t cap, len; int ok; } app_t;

static void app_init(app_t *a, char *b, size_t cap) {
    a->b = b; a->cap = cap; a->len = 0; a->ok = cap > 0;
    if (cap) b[0] = '\0';
}
static void app_n(app_t *a, const char *s, size_t n) {
    if (!a->ok) return;
    if (a->len + n + 1 > a->cap) { a->ok = 0; return; }
    memcpy(a->b + a->len, s, n);
    a->len += n;
    a->b[a->len] = '\0';
}
static void app_s(app_t *a, const char *s) { app_n(a, s, strlen(s)); }
static void app_u(app_t *a, unsigned long v) {
    char t[24];
    int  i = (int)sizeof t;
    t[--i] = '\0';
    if (v == 0) t[--i] = '0';
    while (v) { t[--i] = (char)('0' + (v % 10)); v /= 10; }
    app_s(a, t + i);
}

int fetch_build_request(char *dst, size_t cap, const fetch_url_t *u,
                        const fetch_options_t *opt) {
    static const fetch_options_t none;
    if (!dst || !u) return FETCH_EINVAL;
    if (!opt) opt = &none;

    const char *m = method_ok(opt->method);
    if (!m) return FETCH_EINVAL;
    if (opt->body_len && !opt->body) return FETCH_EINVAL;
    if (opt->body_len > FETCH_BODY_MAX) return FETCH_ENOSPC;

    app_t a;
    app_init(&a, dst, cap);

    app_s(&a, m);
    app_s(&a, " ");
    app_s(&a, u->path);
    /* HTTP/1.0 so the response is close-delimited: there is no chunked decoder
     * in this kernel, and asking for 1.1 would invite one. Same reasoning, and
     * the same "connection: close", as the model transport in net/net.c. */
    app_s(&a, " HTTP/1.0\r\nHost: ");
    app_s(&a, u->host);
    if (u->explicit_port &&
        u->port != (uint16_t)(u->https ? 443 : 80)) {
        app_s(&a, ":");
        app_u(&a, u->port);
    }
    app_s(&a, "\r\nUser-Agent: fable-1 (bare-metal kernel)\r\nAccept: ");
    app_s(&a, (opt->accept && opt->accept[0]) ? opt->accept : "*/*");
    app_s(&a, "\r\nConnection: close\r\n");

    if (opt->body_len) {
        app_s(&a, "Content-Type: ");
        app_s(&a, (opt->content_type && opt->content_type[0])
                      ? opt->content_type : "application/json");
        app_s(&a, "\r\nContent-Length: ");
        app_u(&a, (unsigned long)opt->body_len);
        app_s(&a, "\r\n");
    }
    app_s(&a, "\r\n");
    if (opt->body_len) app_n(&a, opt->body, opt->body_len);

    if (!a.ok) return FETCH_ENOSPC;
    return (int)a.len;
}

/* ====================================================================== */
/* 5. response splitting                                                  */
/* ====================================================================== */

int fetch_split_response(char *buf, size_t len, fetch_result_t *out) {
    if (!buf || !out) return FETCH_EINVAL;

    long eol = find_bytes(buf, len, "\r\n", 2, 0);
    if (eol < 0)
        return FETCH_EPROTO;    /* not even one complete line */

    copy_z(out->status_line, sizeof out->status_line, buf, (size_t)eol);

    /* Status code: exactly three digits after the first space. An unbounded run
     * is signed overflow on network-controlled input, and a long enough one
     * wraps to a value that would pass `status == 200`. This is the same
     * hardening as net/net.c's split_http(), for the same reason. */
    long sp = 0;
    while (sp < eol && buf[sp] != ' ') sp++;
    if (sp >= eol) return FETCH_EPROTO;
    int code = 0, digits = 0;
    long i = sp + 1;
    while (i < eol && buf[i] >= '0' && buf[i] <= '9' && digits < 3) {
        code = code * 10 + (buf[i] - '0');
        i++; digits++;
    }
    if (digits != 3) return FETCH_EPROTO;
    if (i < eol && buf[i] >= '0' && buf[i] <= '9') return FETCH_EPROTO;
    out->http_status = code;

    long hdr  = find_bytes(buf, len, "\r\n\r\n", 4, 0);
    size_t hs = (size_t)eol + 2;                       /* first header line */
    size_t he = hdr >= 0 ? (size_t)hdr + 2 : len;      /* one past the last */
    if (he < hs) he = hs;

    out->headers_truncated =
        !copy_z(out->headers, sizeof out->headers, buf + hs, he - hs);

    /* A 3xx target, reported and never followed — see fetch.h. Searched in the
     * headers copy, so a Location beyond FETCH_HDRS_MAX is simply absent rather
     * than half-read. */
    {
        size_t hn = strlen(out->headers);
        long   at = find_bytes(out->headers, hn, "\nlocation:", 10, 1);
        size_t vs;
        if (at >= 0)                              vs = (size_t)at + 10;
        else if (find_bytes(out->headers, hn, "location:", 9, 1) == 0) vs = 9;
        else                                      vs = (size_t)-1;
        if (vs != (size_t)-1) {
            while (vs < hn && (out->headers[vs] == ' ' ||
                               out->headers[vs] == '\t')) vs++;
            size_t ve = vs;
            while (ve < hn && out->headers[ve] != '\r' &&
                   out->headers[ve] != '\n') ve++;
            copy_z(out->location, sizeof out->location,
                   out->headers + vs, ve - vs);
        }
    }

    size_t off  = hdr >= 0 ? (size_t)hdr + 4 : len;    /* no body is not an error */
    size_t blen = len - off;
    if (blen > 0) memmove(buf, buf + off, blen);
    buf[blen] = '\0';
    out->body     = buf;
    out->body_len = blen;
    return FETCH_OK;
}

/* ====================================================================== */
/* 6. the public verbs                                                    */
/* ====================================================================== */

static const fetch_backend_t *backend;

void fetch_set_backend(const fetch_backend_t *b) { backend = b; }
const fetch_backend_t *fetch_backend(void) { return backend; }

static const char *NO_BACKEND =
    "this machine has no network backend bound: either no supported NIC was "
    "found at boot or net_init() failed. Call net_status for which.";

/* Room for the request line, the headers this file writes, and a model-supplied
 * body. Asserted against its own parts below so it cannot go stale. */
#define FETCH_REQ_MAX 8192
_Static_assert(FETCH_REQ_MAX > FETCH_PATH_MAX + FETCH_HOST_MAX +
                               FETCH_BODY_MAX + 512,
               "FETCH_REQ_MAX must hold a maximal path, host, body and headers");

static uint32_t clamp_timeout(uint32_t ms) {
    if (ms == 0) return FETCH_TIMEOUT_DEFAULT_MS;
    if (ms > FETCH_TIMEOUT_MAX_MS) return FETCH_TIMEOUT_MAX_MS;
    return ms;
}

/* Resolve, or pass a literal address straight through. Shared by all three
 * verbs so "what does this name mean" behaves identically everywhere. */
static int resolve(const fetch_url_t *u, char *ip, size_t ipcap,
                   uint32_t timeout_ms, const char **why) {
    if (u->is_ip) {
        copy_z(ip, ipcap, u->host, strlen(u->host));
        return FETCH_OK;
    }
    if (!backend || !backend->resolve)
        return fail(why, NO_BACKEND, FETCH_ENOBACKEND);
    int rc = backend->resolve(u->host, ip, ipcap, timeout_ms);
    /* Do not report a dead link as a bad name. The backend answers ECONNECT
     * when there is no link at all, and calling that "the hostname did not
     * resolve" sends the model checking its spelling for the rest of the
     * conversation instead of asking why the machine is offline. */
    if (rc == FETCH_ECONNECT)
        return fail(why, "there is no usable network link on this machine, so "
                         "no name can be looked up at all. Call net_status.",
                    FETCH_ECONNECT);
    if (rc != FETCH_OK)
        return fail(why, whyf("could not resolve \"%s\": no answer from the DNS "
                              "server within %u ms. The name may not exist, or "
                              "the resolver may be unreachable — net_status "
                              "shows which server is configured.",
                              u->host, (unsigned)timeout_ms), FETCH_EDNS);
    return FETCH_OK;
}

int fetch(const char *url, size_t url_len, const fetch_options_t *opt,
          char *buf, size_t cap, fetch_result_t *out, const char **why) {
    static const fetch_options_t none;
    static char req[FETCH_REQ_MAX];

    if (!buf || !out || cap < 256)
        return fail(why, "internal: response buffer too small", FETCH_EINVAL);
    if (!opt) opt = &none;

    memset(out, 0, sizeof *out);
    out->body = buf;
    buf[0] = '\0';

    fetch_url_t u;
    int rc = fetch_url_parse(url, url_len, &u, why);
    if (rc != FETCH_OK) return rc;

    if (!method_ok(opt->method))
        return fail(why, whyf("method \"%s\" is not allowed; use one of GET, "
                              "HEAD, POST, PUT, PATCH, DELETE, OPTIONS.",
                              opt->method ? opt->method : "?"), FETCH_EINVAL);
    if (opt->body_len > FETCH_BODY_MAX)
        return fail(why, whyf("the request body is %u bytes; the limit is %d",
                              (unsigned)opt->body_len, FETCH_BODY_MAX),
                    FETCH_ENOSPC);
    if (!hdr_value_ok(opt->accept, 128) && opt->accept)
        return fail(why, "the Accept value contains a control byte or is too "
                         "long", FETCH_EINVAL);
    if (!hdr_value_ok(opt->content_type, 128) && opt->content_type)
        return fail(why, "the Content-Type value contains a control byte or is "
                         "too long", FETCH_EINVAL);

    if (!backend || !backend->exchange)
        return fail(why, NO_BACKEND, FETCH_ENOBACKEND);

    uint32_t tmo = clamp_timeout(opt->timeout_ms);

    char ip[FETCH_IP_MAX];
    rc = resolve(&u, ip, sizeof ip, tmo, why);
    if (rc != FETCH_OK) return rc;
    copy_z(out->peer_ip, sizeof out->peer_ip, ip, strlen(ip));
    out->peer_port = u.port;

    int n = fetch_build_request(req, sizeof req, &u, opt);
    if (n < 0)
        return fail(why, "the assembled request did not fit this kernel's "
                         "request buffer — shorten the URL or the body",
                    FETCH_ENOSPC);

    /* THE LAST GATE BEFORE THE WIRE. Everything the model influenced — the
     * method, the path, the query, every header value and the whole body — is in
     * `req` now, in exactly the bytes that would be transmitted. Scanning here
     * rather than at each field means no future field can be added that skips
     * the check. See DECISION 2 in include/fetch.h. */
    int sec = fetch_scan_secret(req, (size_t)n);
    if (sec)
        return fail(why,
            sec == 1
              ? "REFUSED: this request contains this machine's Anthropic API "
                "key. The key is a secret held in RAM for the model transport "
                "only and must never be sent anywhere else. Nothing was "
                "transmitted."
              : "REFUSED: this request contains an Anthropic-shaped credential "
                "(an \"sk-ant-\" token). No fetch from this machine has any "
                "reason to carry one. Nothing was transmitted.",
            FETCH_ESECRET);

    size_t rxcap = cap;
    if (opt->max_bytes && opt->max_bytes < rxcap) rxcap = opt->max_bytes;
    if (rxcap < 256) rxcap = 256;
    if (rxcap > cap) rxcap = cap;

    fetch_conn_t c;
    memset(&c, 0, sizeof c);
    c.ip         = ip;
    c.port       = u.port;
    c.sni        = u.https ? u.host : (const char *)0;
    c.req        = req;
    c.req_len    = (size_t)n;
    c.rx         = buf;
    c.rx_cap     = rxcap;
    c.timeout_ms = tmo;

    fetch_wire_t w;
    memset(&w, 0, sizeof w);

    rc = backend->exchange(&c, &w);

    out->tls        = w.tls;
    out->elapsed_ms = w.elapsed_ms;
    out->received   = w.rx_len;
    out->truncated  = w.overflow;
    copy_z(out->tls_note, sizeof out->tls_note,
           w.tls_note, strlen(w.tls_note));

    if (rc != FETCH_OK) {
        const char *e = (w.err && w.err[0]) ? w.err : fetch_strerror(rc);
        return fail(why, whyf("%s://%s:%u (%s): %s",
                              u.https ? "https" : "http", u.host,
                              (unsigned)u.port, ip, e), rc);
    }

    if (w.rx_len >= rxcap) w.rx_len = rxcap ? rxcap - 1 : 0;
    buf[w.rx_len] = '\0';

    rc = fetch_split_response(buf, w.rx_len, out);
    if (rc != FETCH_OK)
        return fail(why, w.rx_len == 0
                        ? "the peer accepted the connection and then closed it "
                          "without sending a single byte"
                        : whyf("the peer sent %u bytes that are not an HTTP "
                               "response (no status line, or a malformed one)",
                               (unsigned)w.rx_len),
                    FETCH_EPROTO);

    return FETCH_OK;
}

int fetch_dns(const char *host, size_t hlen, uint32_t timeout_ms,
              char *ip, size_t ipcap, const char **why) {
    if (!host || !ip || ipcap < 8)
        return fail(why, "no hostname given", FETCH_EINVAL);
    ip[0] = '\0';

    while (hlen && (host[0] == ' ' || host[0] == '\t')) { host++; hlen--; }
    while (hlen && (host[hlen-1] == ' ' || host[hlen-1] == '\t')) hlen--;

    /* Accept a URL as well as a bare name: a model that has just been told to
     * fetch a URL will reasonably paste the same string here, and refusing that
     * is a round trip spent on nothing. */
    long sep = find_bytes(host, hlen, "://", 3, 0);
    if (sep >= 0) {
        fetch_url_t u;
        int rc = fetch_url_parse(host, hlen, &u, why);
        if (rc != FETCH_OK) return rc;
        return fetch_dns(u.host, strlen(u.host), timeout_ms, ip, ipcap, why);
    }
    for (size_t i = 0; i < hlen; i++)
        if (!url_byte_ok((unsigned char)host[i]))
            return fail(why, whyf("the hostname contains byte 0x%x at offset "
                                  "%u; names are printable ASCII",
                                  (unsigned)(unsigned char)host[i],
                                  (unsigned)i), FETCH_EURL);

    fetch_url_t u;
    memset(&u, 0, sizeof u);
    int rc = parse_host(host, hlen, u.host, sizeof u.host, &u.is_ip, why);
    if (rc != FETCH_OK) return rc;

    return resolve(&u, ip, ipcap, clamp_timeout(timeout_ms), why);
}

int fetch_tcp_probe(const char *host, size_t hlen, uint16_t port,
                    uint32_t timeout_ms, fetch_probe_t *out, const char **why) {
    if (!out) return fail(why, "internal: no result buffer", FETCH_EINVAL);
    memset(out, 0, sizeof *out);
    out->port = port;

    if (port == 0)
        return fail(why, "port 0 is not a port; use 1..65535", FETCH_EINVAL);
    if (!backend || !backend->probe)
        return fail(why, NO_BACKEND, FETCH_ENOBACKEND);

    uint32_t tmo = clamp_timeout(timeout_ms);
    int rc = fetch_dns(host, hlen, tmo, out->ip, sizeof out->ip, why);
    if (rc != FETCH_OK) return rc;

    fetch_wire_t w;
    memset(&w, 0, sizeof w);
    rc = backend->probe(out->ip, port, tmo, &w);
    out->elapsed_ms = w.elapsed_ms;
    out->open = (rc == FETCH_OK);
    if (rc != FETCH_OK)
        return fail(why, whyf("%s:%u did not accept a connection within %u ms: "
                              "%s", out->ip, (unsigned)port, (unsigned)tmo,
                              (w.err && w.err[0]) ? w.err : fetch_strerror(rc)),
                    rc);
    return FETCH_OK;
}

int fetch_ifstat(fetch_ifstat_t *out) {
    if (!out) return FETCH_EINVAL;
    memset(out, 0, sizeof *out);
    if (!backend || !backend->ifstat) return FETCH_ENOBACKEND;
    return backend->ifstat(out);
}

const char *fetch_strerror(int rc) {
    switch (rc) {
    case FETCH_OK:         return "ok";
    case FETCH_EINVAL:     return "bad arguments";
    case FETCH_ENOSPC:     return "did not fit";
    case FETCH_EURL:       return "the URL did not parse";
    case FETCH_ESCHEME:    return "unsupported scheme";
    case FETCH_EDNS:       return "the hostname did not resolve";
    case FETCH_ECONNECT:   return "the connection was refused or never answered";
    case FETCH_ETLS:       return "the TLS handshake failed";
    case FETCH_ETIMEOUT:   return "timed out";
    case FETCH_EPROTO:     return "the reply was not intelligible HTTP";
    case FETCH_ENOBACKEND: return "no network backend";
    case FETCH_ESECRET:    return "refused: the request carried a credential";
    case FETCH_EPOLICY:    return "refused by policy";
    default:               return "unknown error";
    }
}

const char *fetch_tls_word(int state) {
    switch (state) {
    case FETCH_TLS_VERIFIED:   return "verified";
    case FETCH_TLS_UNVERIFIED: return "unverified";
    default:                   return "none";
    }
}
