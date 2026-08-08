/* net_tools.c — the model's network verbs.
 *
 * PURPOSE
 *   This kernel has had a TCP/IP stack, a DNS resolver and a TLS client since
 *   its first boot, and every byte of it was reserved for one destination. The
 *   model could allocate memory, drive PCI silicon and write a sound driver from
 *   scratch, and it could not answer "is the link up?" or "what does that page
 *   say?". These four tools close that.
 *
 * THE TOOLS
 *   net_fetch    GET/POST/... an arbitrary http:// or https:// URL and return
 *                the status, what TLS proved, the headers and the body — or
 *                write the whole body to a file and hand back a summary.
 *   net_resolve  turn a hostname into an address, with the timing.
 *   net_probe    is anything listening on host:port? (a TCP handshake, nothing
 *                sent, nothing interpreted)
 *   net_status   the link, the addresses, the resolver, the API host, and what
 *                this build's TLS can and cannot prove.
 *
 *   Only net_fetch mutates anything, and only when asked to save a body, so it
 *   is the only one carrying TOOL_MUTATES.
 *
 * WHY THE RESULTS LOOK LIKE THIS
 *   A tool result is clipped to about 1 KB by the turn loop (chat.h's
 *   CHAT_TOOL_RESULT_CAP), which is smaller than most useful documents. So every
 *   fetch leads with the facts that decide what to do next — status, peer, TLS
 *   state, how many bytes really arrived — and only then spends what is left on
 *   the body. When there is more body than room, the tool says so with the exact
 *   numbers, and `save_path` writes the WHOLE body to the filesystem so
 *   read_file(offset=...) can page through it. Truncation is never silent: a
 *   model that cannot tell a complete document from a clipped one will
 *   confidently reason from half a page.
 *
 * WHAT EVERY HTTPS RESULT SAYS ABOUT THE PEER
 *   The "tls:" line is not decoration and is not derived from a build flag. It
 *   is read from the live mbedTLS session (net/net.c, record_tls_state) and is
 *   one of three states. In the default build it says, in capitals, that the
 *   connection is encrypted but NOT authenticated. That is the truth about this
 *   kernel, the model may act on fetched content, and a tool that hides it would
 *   be lying by omission. The full argument is DECISION 1 in include/fetch.h.
 *
 * THE API KEY CANNOT LEAVE THROUGH HERE
 *   Nothing in this file can read it, and the request bytes are scanned for it
 *   immediately before transmission (include/fetch.h DECISION 2). A refused
 *   fetch says so plainly and echoes none of the offending bytes. This file adds
 *   one more rule of its own: net_fetch's result never quotes the request back,
 *   so even a refusal cannot become an oracle.
 *
 * UNTRUSTED INPUT
 *   Every argument is model output. Strings go through json.h and are rejected
 *   if they carry an embedded NUL (which would silently truncate a URL after the
 *   parser had approved the whole of it); numbers must be JSON integers inside a
 *   stated range; the URL itself is parsed by net/fetch.c, which rejects control
 *   bytes, spaces, userinfo, IPv6 literals, absurd ports and 10 KB paths with a
 *   sentence naming the problem. Response bytes are equally untrusted and are
 *   rendered printable before they are quoted back.
 *
 * DEPENDENCIES
 *   fetch.h (all of the network), vfs.h (save_path), json.h, tool.h, trace.h.
 *   No lwIP, no mbedTLS, no kernel.h — which is why tests/host/test_net_tools.c
 *   can link this file and drive all four tools against a synthetic backend.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "fetch.h"
#include "vfs.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand as it does in a kernel link. tests/host/tooltable.h supplies the same
 * table through the section$start builtins; including it here would drag the
 * test harness into the kernel, so the suite includes this .c file instead. */
#ifdef FABLEOS_HOSTTEST
#include "tooltable.h"
#endif

/* ====================================================================== */
/* buffers                                                                */
/* ====================================================================== */

/* One response at a time, and never on the stack: the kernel stack is 64 KiB.
 * 64 KiB of receive room means a page that does not fit is reported with a real
 * number rather than an unknown one — the peer's bytes are counted up to here
 * even when only the first kilobyte is quoted. */
static char rx[65536];

static char url_arg[FETCH_URL_MAX + 1];
static char body_arg[FETCH_BODY_MAX + 1];
static char save_arg[VFS_PATH_MAX + 1];
static char method_arg[16];
static char ctype_arg[128];
static char host_arg[FETCH_HOST_MAX + 1];

/* ====================================================================== */
/* result helpers                                                         */
/* ====================================================================== */

/* Guarantee the model is told when output was clipped, by overwriting the tail
 * of the buffer rather than appending into one that is already full. Same
 * backstop, for the same reason, as tools/mem_tools.c and tools/time_tools.c. */
static void mark_truncated(tool_result_t *r) {
    static const char note[] = "\n[output truncated]";
    if (!r || !r->truncated || !r->buf || r->cap == 0) return;
    size_t n = sizeof note - 1;
    if (r->cap < n + 2) {
        size_t k = 0;
        if (r->cap > 1) r->buf[k++] = '!';
        r->buf[k] = '\0';
        r->len    = k;
        return;
    }
    size_t at = r->cap - 1 - n;
    for (size_t i = 0; i < n; i++) r->buf[at + i] = note[i];
    r->buf[r->cap - 1] = '\0';
    r->len = r->cap - 1;
}

static int oops(tool_result_t *r, const char *tool, const char *tag,
                const char *fmt, ...) {
    va_list ap;
    char    line[320];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    r->is_error = 1;
    tool_result_printf(r, "error: %s\n", line);
    mark_truncated(r);
    trace_err(TOOL_EINVAL, tool, "%s", tag);
    return TOOL_OK;
}

/* ====================================================================== */
/* argument helpers                                                       */
/* ====================================================================== */

/* Read a string argument into `dst`.
 * Returns 1 present-and-valid, 0 absent, -1 invalid (message written to `err`).
 *
 * AN EMBEDDED NUL IS A REJECTION, not a truncation. json_str() NUL-terminates,
 * so "https://good.example\0https://evil.example" would parse, validate and
 * fetch as the first half while the model believes it asked for something else —
 * and worse, the length json.h reports and the length strlen() reports would
 * disagree everywhere downstream. Comparing them is the whole check. */
static int arg_str(const json_value_t *root, const char *key,
                   char *dst, size_t cap, char *err, size_t errcap) {
    json_value_t v;
    err[0] = '\0';
    if (json_get(root, key, &v) != JSON_OK) { dst[0] = '\0'; return 0; }
    if (v.type != JSON_STRING) {
        snprintf(err, errcap, "\"%s\" must be a string", key);
        return -1;
    }
    size_t n  = 0;
    int    rc = json_str(&v, dst, cap, &n);
    if (rc == JSON_ENOSPC) {
        snprintf(err, errcap, "\"%s\" is longer than the %u bytes this kernel "
                              "accepts", key, (unsigned)cap - 1);
        return -1;
    }
    if (rc != JSON_OK) {
        snprintf(err, errcap, "\"%s\" is not a readable string", key);
        return -1;
    }
    if (strlen(dst) != n) {
        snprintf(err, errcap, "\"%s\" contains an embedded NUL byte; the value "
                              "would be silently cut short", key);
        return -1;
    }
    return 1;
}

/* Read an integer argument, or leave `*out` at `def`. Returns 0 or -1. */
static int arg_int(const json_value_t *root, const char *key, int64_t def,
                   int64_t lo, int64_t hi, int64_t *out,
                   char *err, size_t errcap) {
    json_value_t v;
    *out = def;
    if (json_get(root, key, &v) != JSON_OK) return 0;
    if (v.type != JSON_NUMBER || json_int(&v, out) != JSON_OK) {
        snprintf(err, errcap, "\"%s\" must be an integer", key);
        return -1;
    }
    if (*out < lo || *out > hi) {
        snprintf(err, errcap, "\"%s\" is %ld, outside %ld..%ld",
                 key, (long)*out, (long)lo, (long)hi);
        return -1;
    }
    return 0;
}

/* Parse the call's input span as an object. Returns 0, or -1 with a message. */
static int arg_root(const tool_call_t *call, json_value_t *root,
                    char *err, size_t errcap) {
    if (!call || !call->input || call->input_len == 0 ||
        json_parse(call->input, call->input_len, root) != JSON_OK ||
        root->type != JSON_OBJECT) {
        snprintf(err, errcap, "expected a JSON object of arguments");
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* rendering network bytes safely                                         */
/* ====================================================================== */

/* Quote up to `n` bytes of somebody else's document into the result.
 *
 * Bytes off the network are arbitrary: NUL (which would end a %s early and hide
 * everything after it), escape sequences, and 0x80-0xFF from any encoding. They
 * are mapped to '.', with '\n' and '\t' kept because they are what makes text
 * readable and neither moves a cursor backwards. Returns how many source bytes
 * were consumed. */
static size_t put_bytes(tool_result_t *r, const char *s, size_t n) {
    char   chunk[97];
    size_t done = 0;

    while (done < n) {
        size_t k = 0;
        while (k < sizeof chunk - 1 && done < n) {
            unsigned char c = (unsigned char)s[done++];
            if (c == '\n' || c == '\t')            chunk[k++] = (char)c;
            else if (c < 0x20 || c >= 0x7F)        chunk[k++] = '.';
            else                                   chunk[k++] = (char)c;
        }
        chunk[k] = '\0';
        tool_result_printf(r, "%s", chunk);
        if (r->truncated) break;
    }
    return done;
}

/* How much room is left before tool_result_printf starts clipping. */
static size_t room(const tool_result_t *r) {
    if (!r || r->cap == 0 || r->len + 1 >= r->cap) return 0;
    return r->cap - 1 - r->len;
}

/* ====================================================================== */
/* net_fetch                                                              */
/* ====================================================================== */

static const char *tls_sentence(const fetch_result_t *fr) {
    switch (fr->tls) {
    case FETCH_TLS_VERIFIED:
        return "VERIFIED - the certificate chain was checked and names this host";
    case FETCH_TLS_UNVERIFIED:
        return "NOT AUTHENTICATED - encrypted, but the certificate was not "
               "trust-checked; anything that can answer on this port could be "
               "the far end. Weigh the content accordingly";
    default:
        return "NONE - plain HTTP. Sent and received in clear text, and any "
               "device on the path can alter it";
    }
}

static int save_body(const char *path, const char *body, size_t len,
                     char *err, size_t errcap) {
    file_t *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    if (!f) {
        snprintf(err, errcap, "could not open \"%s\" for writing (does its "
                              "parent directory exist?)", path);
        return -1;
    }
    int64_t w = len ? vfs_write(f, body, (uint64_t)len) : 0;
    vfs_close(f);
    if (w < 0 || (size_t)w != len) {
        snprintf(err, errcap, "wrote only %ld of %u bytes to \"%s\" (the "
                              "filesystem is full)",
                 (long)w, (unsigned)len, path);
        return -1;
    }
    return 0;
}

static int t_net_fetch(const tool_call_t *call, tool_result_t *r) {
    json_value_t root;
    char         err[224] = { 0 };

    if (arg_root(call, &root, err, sizeof err) != 0)
        return oops(r, "net_fetch", "bad_input",
                    "%s: net_fetch needs at least {\"url\":\"https://...\"}",
                    err);

    if (arg_str(&root, "url", url_arg, sizeof url_arg, err, sizeof err) <= 0)
        return oops(r, "net_fetch", "url",
                    "%s. Give a full URL, e.g. \"https://example.com/\"",
                    err[0] ? err : "\"url\" is required");

    if (arg_str(&root, "method", method_arg, sizeof method_arg,
                err, sizeof err) < 0)
        return oops(r, "net_fetch", "method", "%s", err);
    if (arg_str(&root, "body", body_arg, sizeof body_arg, err, sizeof err) < 0)
        return oops(r, "net_fetch", "body", "%s", err);
    if (arg_str(&root, "content_type", ctype_arg, sizeof ctype_arg,
                err, sizeof err) < 0)
        return oops(r, "net_fetch", "content_type", "%s", err);
    if (arg_str(&root, "save_path", save_arg, sizeof save_arg,
                err, sizeof err) < 0)
        return oops(r, "net_fetch", "save_path", "%s", err);

    int64_t max_bytes, timeout_ms;
    if (arg_int(&root, "max_bytes", (int64_t)sizeof rx - 1, 256,
                (int64_t)sizeof rx - 1, &max_bytes, err, sizeof err) != 0)
        return oops(r, "net_fetch", "max_bytes", "%s", err);
    if (arg_int(&root, "timeout_ms", 0, 0, FETCH_TIMEOUT_MAX_MS,
                &timeout_ms, err, sizeof err) != 0)
        return oops(r, "net_fetch", "timeout", "%s", err);

    fetch_options_t opt;
    memset(&opt, 0, sizeof opt);
    opt.method       = method_arg[0] ? method_arg : (const char *)0;
    opt.body         = body_arg[0] ? body_arg : (const char *)0;
    opt.body_len     = strlen(body_arg);
    opt.content_type = ctype_arg[0] ? ctype_arg : (const char *)0;
    opt.timeout_ms   = (uint32_t)timeout_ms;
    opt.max_bytes    = (size_t)max_bytes;

    fetch_result_t fr;
    const char    *why = (const char *)0;
    int rc = fetch(url_arg, strlen(url_arg), &opt, rx, sizeof rx, &fr, &why);

    if (rc != FETCH_OK) {
        r->is_error = 1;
        /* NEVER quote the request back. On the FETCH_ESECRET path the request is
         * precisely the thing that must not be repeated, and a per-path
         * exception is one refactor away from being lost. */
        tool_result_printf(r, "fetch failed (%s)\n%s\n",
                           fetch_strerror(rc), why ? why : "no further detail");
        if (rc == FETCH_ENOBACKEND || rc == FETCH_ECONNECT)
            tool_result_printf(r, "Call net_status to see the link, the "
                                  "addresses and the resolver.\n");
        mark_truncated(r);
        trace_err(rc, "net_fetch", "rc=%d", rc);
        return TOOL_OK;
    }

    tool_result_printf(r, "%s -> %s\n",
                       method_arg[0] ? method_arg : "GET", fr.status_line);
    tool_result_printf(r, "peer:  %s:%u in %u ms\n",
                       fr.peer_ip, (unsigned)fr.peer_port,
                       (unsigned)fr.elapsed_ms);
    tool_result_printf(r, "tls:   %s\n", tls_sentence(&fr));
    if (fr.tls_note[0]) tool_result_printf(r, "       (%s)\n", fr.tls_note);

    tool_result_printf(r, "bytes: %u body",
                       (unsigned)fr.body_len);
    if (fr.truncated)
        tool_result_printf(r, " -- TRUNCATED: the peer had more to send than "
                              "the %u-byte receive limit; raise max_bytes or "
                              "ask the server for a range",
                           (unsigned)max_bytes);
    tool_result_printf(r, "\n");

    if (fr.location[0])
        tool_result_printf(r, "redirect: %s  (NOT followed - fetch it "
                              "explicitly if you want it)\n", fr.location);

    /* Saving is the answer to "the document is bigger than a tool result".
     * Report the failure but keep the response: a filesystem problem is not a
     * reason to throw away bytes that already crossed the network. */
    int saved = 0;
    if (save_arg[0]) {
        if (save_body(save_arg, fr.body, fr.body_len, err, sizeof err) == 0) {
            saved = 1;
            tool_result_printf(r, "saved: %u bytes to %s -- page it with "
                                  "read_file(path, offset)\n",
                               (unsigned)fr.body_len, save_arg);
        } else {
            tool_result_printf(r, "save FAILED: %s\n", err);
        }
    }

    /* Whatever room is left goes to the body, minus enough to say how much was
     * left out. Saying "shown 900 of 41233" is what stops the model reasoning
     * from a fragment it believes is the whole thing. */
    size_t avail = room(r);
    size_t hold  = 96;
    size_t show  = avail > hold ? avail - hold : 0;
    if (show > fr.body_len) show = fr.body_len;

    if (fr.body_len == 0) {
        tool_result_printf(r, "body:  (empty)\n");
    } else if (show == 0) {
        tool_result_printf(r, "body:  not shown, no room left in this result\n");
    } else {
        tool_result_printf(r, "body (%u of %u bytes):\n",
                           (unsigned)show, (unsigned)fr.body_len);
        put_bytes(r, fr.body, show);
        if (show < fr.body_len)
            tool_result_printf(r, "\n... %u more bytes not shown%s\n",
                               (unsigned)(fr.body_len - show),
                               saved ? "; the whole body is in the file above"
                                     : "; pass save_path to keep all of it");
        else
            tool_result_printf(r, "\n");
    }

    mark_truncated(r);
    trace_ret(fr.http_status, "net_fetch", "%s %u bytes",
              fr.peer_ip, (unsigned)fr.body_len);
    return TOOL_OK;
}

static const tool_t net_fetch_tool = {
    .name        = "net_fetch",
    .description =
        "Fetch an http:// or https:// URL from this machine over its own TCP/IP "
        "and TLS stack, and return the HTTP status, the peer address, WHAT THE "
        "TLS CONNECTION PROVED, the size, and as much of the body as fits. "
        "Always read the \"tls:\" line before trusting content: unless it says "
        "VERIFIED, the certificate was not checked and the peer is not proven. "
        "A tool result is about 1 KB, so for anything bigger pass save_path to "
        "write the WHOLE body to a file and then page it with read_file(offset). "
        "Truncation is always reported with exact byte counts. Redirects are "
        "reported, never followed. HTTP/1.0 is used, so there is no chunked "
        "decoding. A request that would carry this machine's API key is refused "
        "and nothing is sent. Use net_status first if a fetch fails.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"url\":{\"type\":\"string\",\"description\":"
          "\"Full URL including the scheme, e.g. https://example.com/index.html\"},"
        "\"method\":{\"type\":\"string\",\"description\":"
          "\"GET (default), HEAD, POST, PUT, PATCH, DELETE or OPTIONS.\"},"
        "\"body\":{\"type\":\"string\",\"description\":"
          "\"Request body, up to 4096 bytes. Sets Content-Length.\"},"
        "\"content_type\":{\"type\":\"string\",\"description\":"
          "\"Content-Type for the body. Defaults to application/json.\"},"
        "\"save_path\":{\"type\":\"string\",\"description\":"
          "\"Absolute path to write the whole response body to.\"},"
        "\"max_bytes\":{\"type\":\"integer\",\"description\":"
          "\"Cap on bytes taken off the wire, 256..65535.\"},"
        "\"timeout_ms\":{\"type\":\"integer\",\"description\":"
          "\"Deadline in ms, up to 60000. Default 15000.\"}},"
        "\"required\":[\"url\"]}",
    .flags        = TOOL_MUTATES,      /* only when save_path is given */
    .invoke       = t_net_fetch,
};
REGISTER_TOOL(net_fetch_tool);

/* ====================================================================== */
/* net_resolve                                                            */
/* ====================================================================== */

static int t_net_resolve(const tool_call_t *call, tool_result_t *r) {
    json_value_t root;
    char         err[224] = { 0 };

    if (arg_root(call, &root, err, sizeof err) != 0)
        return oops(r, "net_resolve", "bad_input",
                    "%s: net_resolve needs {\"host\":\"example.com\"}", err);
    if (arg_str(&root, "host", host_arg, sizeof host_arg, err, sizeof err) <= 0)
        return oops(r, "net_resolve", "host", "%s",
                    err[0] ? err : "\"host\" is required");

    int64_t timeout_ms;
    if (arg_int(&root, "timeout_ms", 0, 0, FETCH_TIMEOUT_MAX_MS,
                &timeout_ms, err, sizeof err) != 0)
        return oops(r, "net_resolve", "timeout", "%s", err);

    char        ip[FETCH_IP_MAX];
    const char *why = (const char *)0;
    int rc = fetch_dns(host_arg, strlen(host_arg), (uint32_t)timeout_ms,
                       ip, sizeof ip, &why);
    if (rc != FETCH_OK) {
        r->is_error = 1;
        tool_result_printf(r, "%s did not resolve (%s)\n%s\n",
                           host_arg, fetch_strerror(rc),
                           why ? why : "no further detail");
        mark_truncated(r);
        trace_err(rc, "net_resolve", "%s", host_arg);
        return TOOL_OK;
    }

    tool_result_printf(r,
        "%s -> %s\n"
        "Resolved by this machine's own DNS client (net_status shows which "
        "server). This says the name has an address, not that anything is "
        "listening on it - use net_probe for that.\n",
        host_arg, ip);
    mark_truncated(r);
    trace_ret(0, "net_resolve", "%s=%s", host_arg, ip);
    return TOOL_OK;
}

static const tool_t net_resolve_tool = {
    .name        = "net_resolve",
    .description =
        "Look up a hostname with this machine's DNS client and report the "
        "address. Accepts a bare name or a full URL. Resolving proves the name "
        "exists and the resolver is reachable; it does not prove anything is "
        "listening. On failure it says whether the name is bad or the resolver "
        "never answered.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"host\":{\"type\":\"string\",\"description\":"
          "\"Hostname or URL, e.g. example.com\"},"
        "\"timeout_ms\":{\"type\":\"integer\",\"description\":"
          "\"Deadline in ms, up to 60000. Default 15000.\"}},"
        "\"required\":[\"host\"]}",
    .flags        = 0,
    .invoke       = t_net_resolve,
};
REGISTER_TOOL(net_resolve_tool);

/* ====================================================================== */
/* net_probe                                                              */
/* ====================================================================== */

static int t_net_probe(const tool_call_t *call, tool_result_t *r) {
    json_value_t root;
    char         err[224] = { 0 };

    if (arg_root(call, &root, err, sizeof err) != 0)
        return oops(r, "net_probe", "bad_input",
                    "%s: net_probe needs {\"host\":\"h\",\"port\":443}", err);
    if (arg_str(&root, "host", host_arg, sizeof host_arg, err, sizeof err) <= 0)
        return oops(r, "net_probe", "host", "%s",
                    err[0] ? err : "\"host\" is required");

    int64_t port, timeout_ms;
    if (arg_int(&root, "port", -1, 1, 65535, &port, err, sizeof err) != 0)
        return oops(r, "net_probe", "port", "%s", err);
    if (port < 0)
        return oops(r, "net_probe", "port",
                    "\"port\" is required, an integer in 1..65535");
    if (arg_int(&root, "timeout_ms", 0, 0, FETCH_TIMEOUT_MAX_MS,
                &timeout_ms, err, sizeof err) != 0)
        return oops(r, "net_probe", "timeout", "%s", err);

    fetch_probe_t p;
    const char   *why = (const char *)0;
    int rc = fetch_tcp_probe(host_arg, strlen(host_arg), (uint16_t)port,
                             (uint32_t)timeout_ms, &p, &why);
    if (rc != FETCH_OK) {
        r->is_error = 1;
        tool_result_printf(r,
            "closed: %s:%u did not accept a TCP connection (%s)\n%s\n",
            p.ip[0] ? p.ip : host_arg, (unsigned)port, fetch_strerror(rc),
            why ? why : "no further detail");
        mark_truncated(r);
        trace_err(rc, "net_probe", "%s:%u", host_arg, (unsigned)port);
        return TOOL_OK;
    }

    tool_result_printf(r,
        "open: %s (%s) port %u accepted a TCP connection in %u ms.\n"
        "Nothing was sent and nothing was read, so this says a listener exists "
        "- not what protocol it speaks.\n",
        host_arg, p.ip, (unsigned)port, (unsigned)p.elapsed_ms);
    mark_truncated(r);
    trace_ret((long)port, "net_probe", "%s open", p.ip);
    return TOOL_OK;
}

static const tool_t net_probe_tool = {
    .name        = "net_probe",
    .description =
        "Open a plain TCP connection to host:port, then hang up. Sends no bytes "
        "and reads none, so it answers exactly one question: is something "
        "listening? Use it to tell \"the host is unreachable\" apart from \"the "
        "service is broken\" after a failed net_fetch.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"host\":{\"type\":\"string\",\"description\":"
          "\"Hostname or IPv4 address.\"},"
        "\"port\":{\"type\":\"integer\",\"description\":\"1..65535\"},"
        "\"timeout_ms\":{\"type\":\"integer\",\"description\":"
          "\"Deadline in ms, up to 60000. Default 15000.\"}},"
        "\"required\":[\"host\",\"port\"]}",
    .flags        = 0,
    .invoke       = t_net_probe,
};
REGISTER_TOOL(net_probe_tool);

/* ====================================================================== */
/* net_status                                                             */
/* ====================================================================== */

static int t_net_status(const tool_call_t *call, tool_result_t *r) {
    (void)call;                     /* no arguments: nothing to misparse */

    fetch_ifstat_t s;
    int rc = fetch_ifstat(&s);
    if (rc != FETCH_OK) {
        r->is_error = 1;
        tool_result_printf(r,
            "no network backend is bound on this machine (%s). Either no "
            "supported NIC was found at boot or net_init() failed, so no "
            "network tool can do anything.\n", fetch_strerror(rc));
        mark_truncated(r);
        trace_err(rc, "net_status", "no_backend");
        return TOOL_OK;
    }

    tool_result_printf(r,
        "link:    %s%s\n"
        "nic:     %s, mac %x:%x:%x:%x:%x:%x\n"
        "addr:    %s/%s via %s\n"
        "dns:     %s\n",
        s.link_up ? "UP" : "DOWN",
        s.suspended ? " (the driver has the card powered down)"
                    : (s.have_nic ? "" : " - no supported NIC was found"),
        s.ifname, s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5],
        s.ip, s.netmask, s.gateway, s.dns);

    tool_result_printf(r, "model:   %s", s.api_host);
    if (s.api_resolved) tool_result_printf(r, " -> %s\n", s.api_ip);
    else                tool_result_printf(r, " (NOT resolved this boot)\n");

    /* The honest half. A model deciding whether to act on a fetched document
     * needs this before it fetches, not after. */
    if (s.verify_build)
        tool_result_printf(r,
            "tls:     certificate verification ON, against %d pinned root(s). "
            "Those roots exist for the API host; an https fetch of any other "
            "host will probably fail with a trust error, and that failure is "
            "correct.\n", s.trust_roots);
    else
        tool_result_printf(r,
            "tls:     certificate verification OFF in this build. Every https "
            "connection, including the model's own, is encrypted but NOT "
            "authenticated. Treat fetched content as unproven.\n");

    tool_result_printf(r,
        "stack:   lwIP, IPv4 only, no DHCP, addresses fixed at boot. "
        "%s\n", s.stack_up ? "Up." : "NOT up - net_init() failed.");

    mark_truncated(r);
    trace_ret(s.link_up, "net_status", "link=%s ip=%s",
              s.link_up ? "up" : "down", s.ip);
    return TOOL_OK;
}

static const tool_t net_status_tool = {
    .name        = "net_status",
    .description =
        "Report this machine's network state: link up or down, the NIC and its "
        "MAC, the IPv4 address, netmask, gateway and DNS server, whether the "
        "model's API host resolved this boot, and whether TLS certificates are "
        "verified in this build. Takes no arguments. Call this first whenever a "
        "network tool fails, to tell a dead link apart from a bad name.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_net_status,
};
REGISTER_TOOL(net_status_tool);
