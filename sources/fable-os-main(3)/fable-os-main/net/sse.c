/* sse.c — the streaming reader described in include/sse.h.
 *
 * Pure code: no allocation, no globals, no kernel services, no libc, no lwIP.
 * The only inputs are the caller's bytes and the only outputs are the caller's
 * buffers and one callback — which is why this file compiles unchanged into the
 * kernel and into tests/host/test_sse.c, and why the parser that faces the
 * network is literally the parser the tests hammer.
 *
 * Structure mirrors the header: framing, then reassembly, then the HTTP driver.
 * All three are fed one byte at a time in the tests, because that is the only
 * delivery pattern the wire actually guarantees.
 *
 * THE ONE INVARIANT worth stating twice: the reassembled document is always
 * valid JSON. Not "usually", not "unless the reply was long" — always. It is
 * echoed straight back into the next request by net/chat.c, so a document that
 * is half-written is not a smaller answer, it is a dead conversation. That is
 * what SSE_TAIL_RESERVE and the freeze() path below are for: the closing
 * brackets are paid for before the first byte of content is written, and once
 * the buffer runs low the writer stops emitting *structure* entirely rather
 * than emitting an unbalanced tail.
 */

#include "sse.h"

/* ====================================================================== */
/* tiny local helpers (no libc, no port/ shims — same rule as json.c)      */
/* ====================================================================== */

static size_t zlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void bcopy_n(char *dst, const char *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

/* Compare a counted span against a NUL-terminated literal. */
static int span_eq(const char *p, size_t n, const char *lit) {
    size_t i = 0;
    if (!p) return lit[0] == '\0' && n == 0;
    for (; i < n; i++)
        if (lit[i] == '\0' || p[i] != lit[i]) return 0;
    return lit[i] == '\0';
}

static int streq(const char *a, const char *b) { return span_eq(a, zlen(a), b); }

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive search for a NUL-terminated needle in [p, p+n). */
static int ifind(const char *p, size_t n, const char *needle) {
    size_t m = zlen(needle);
    if (m == 0 || m > n) return -1;
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && lower(p[i + k]) == lower(needle[k])) k++;
        if (k == m) return (int)i;
    }
    return -1;
}

/* Find a header FIELD NAME: `needle` at the start of a header line, i.e. after
 * a line ending, never inside a value and never on the status line. Returns the
 * offset of the field name, or -1. Case-insensitive, per RFC 7230. */
static int hdr_field(const char *p, size_t n, const char *needle) {
    size_t m = zlen(needle);
    if (m == 0 || m > n) return -1;
    for (size_t i = 0; i + m <= n; i++) {
        /* i must begin a line, and it must not be the first line (the status
         * line has no field name). */
        if (i == 0) continue;
        if (p[i - 1] != '\n') continue;
        size_t k = 0;
        while (k < m && lower(p[i + k]) == lower(needle[k])) k++;
        if (k == m) return (int)i;
    }
    return -1;
}

/* Copy a bounded, NUL-terminated string; silently clamps. */
static void copy_clamped(char *dst, size_t cap, const char *src, size_t n) {
    if (cap == 0) return;
    if (n > cap - 1) n = cap - 1;
    if (src) bcopy_n(dst, src, n); else n = 0;
    dst[n] = '\0';
}

const char *sse_strerror(int err) {
    switch (err) {
        case SSE_OK:      return "ok";
        case SSE_EINVAL:  return "bad arguments";
        case SSE_ENOSPC:  return "streamed reply did not fit the response buffer";
        case SSE_EPROTO:  return "response was not an intelligible event stream";
        case SSE_ESTREAM: return "the stream reported an error";
        default:          return "unknown streaming error";
    }
}

/* ====================================================================== */
/* layer 1 — framing                                                       */
/* ====================================================================== */

static const char SSE_BOM[3] = { (char)0xEF, (char)0xBB, (char)0xBF };

void sse_parser_init(sse_parser_t *p, sse_event_fn fn, void *ctx) {
    if (!p) return;
    p->line_len    = 0;
    p->line_over   = 0;
    p->after_cr    = 0;
    p->bom         = 0;
    p->name[0]     = '\0';
    p->id[0]       = '\0';
    p->data_len    = 0;
    p->data[0]     = '\0';
    p->ev_bad      = 0;
    p->n_events    = 0;
    p->n_dropped   = 0;
    p->n_comments  = 0;
    p->n_bad_lines = 0;
    p->n_bytes     = 0;
    p->on_event    = fn;
    p->ctx         = ctx;
}

/* A blank line ends the event: hand it over, then start a fresh one.
 *
 * WHATWG: an event whose data buffer is empty is not dispatched at all (that is
 * how a bare `event: ping` with no data stays silent), and exactly one trailing
 * '\n' is removed. Both matter for the Anthropic stream. */
static void dispatch(sse_parser_t *p) {
    if (p->data_len == 0) {              /* nothing to deliver */
        /* Nothing to deliver — but if something overran while this event was
         * being assembled then a real event WAS lost, and saying so in the
         * counters is the difference between "quiet" and "silently dropping
         * every reply". */
        if (p->ev_bad) p->n_dropped++;
        p->name[0] = '\0';
        p->ev_bad  = 0;
        return;
    }
    if (p->data[p->data_len - 1] == '\n') p->data_len--;
    p->data[p->data_len] = '\0';

    if (p->ev_bad) {
        p->n_dropped++;                  /* something overran: drop it whole */
    } else {
        p->n_events++;
        if (p->on_event) {
            sse_event_t ev;
            ev.type     = p->name[0] ? p->name : "message";
            ev.data     = p->data;
            ev.data_len = p->data_len;
            ev.id       = p->id;
            p->on_event(p->ctx, &ev);
        }
    }
    p->name[0]  = '\0';
    p->data_len = 0;
    p->data[0]  = '\0';
    p->ev_bad   = 0;
}

/* Append one data line's value plus the '\n' the spec joins data lines with. */
static void data_append(sse_parser_t *p, const char *v, size_t n) {
    /* Clamp rather than wrap, and mark the whole event unusable: half an event
     * is not a short event, it is a different one. */
    size_t room = (p->data_len + 1 < SSE_DATA_MAX)
                ? SSE_DATA_MAX - p->data_len - 1 : 0;
    if (n > room) { n = room; p->ev_bad = 1; }
    bcopy_n(p->data + p->data_len, v, n);
    p->data_len += n;

    if (p->data_len + 1 < SSE_DATA_MAX) p->data[p->data_len++] = '\n';
    else                                p->ev_bad = 1;
}

/* One complete line is sitting in p->line. Classify and apply it. */
static void end_line(sse_parser_t *p) {
    size_t      len = p->line_len;
    const char *l   = p->line;

    p->line_len = 0;                     /* reset first: every path below returns */

    if (p->line_over) {
        /* An over-long line cannot be interpreted: the field name may be whole
         * but the value is not, and guessing is how parsers get owned. */
        p->line_over = 0;
        p->n_bad_lines++;
        p->ev_bad = 1;
        return;
    }

    if (len == 0)    { dispatch(p); return; }        /* blank: end of event  */
    if (l[0] == ':') { p->n_comments++; return; }    /* ": keepalive"        */

    size_t colon = 0;
    while (colon < len && l[colon] != ':') colon++;

    const char *field = l;
    size_t      flen  = colon;
    const char *val   = l + (colon < len ? colon + 1 : len);
    size_t      vlen  = (colon < len) ? len - colon - 1 : 0;
    if (vlen && val[0] == ' ') { val++; vlen--; }    /* one optional space   */

    if (span_eq(field, flen, "data")) {
        data_append(p, val, vlen);
    } else if (span_eq(field, flen, "event")) {
        if (vlen > SSE_NAME_MAX - 1) p->ev_bad = 1;
        copy_clamped(p->name, sizeof p->name, val, vlen);
    } else if (span_eq(field, flen, "id")) {
        int has_nul = 0;                 /* spec: an id containing NUL is ignored */
        for (size_t i = 0; i < vlen; i++) if (val[i] == '\0') has_nul = 1;
        if (!has_nul) copy_clamped(p->id, sizeof p->id, val, vlen);
    }
    /* "retry" and every unknown field: ignored, per spec. */
}

static void feed_byte(sse_parser_t *p, char c) {
    if (p->after_cr) {
        p->after_cr = 0;
        if (c == '\n') return;                       /* the LF of a CRLF */
    }
    if (c == '\r') { p->after_cr = 1; end_line(p); return; }
    if (c == '\n') { end_line(p); return; }

    if (p->line_len < SSE_LINE_MAX) p->line[p->line_len++] = c;
    else                            p->line_over = 1;
}

/* A partially matched BOM turned out to be data after all: replay it. */
static void bom_replay(sse_parser_t *p) {
    uint8_t matched = p->bom;
    p->bom = 3;
    for (uint8_t k = 0; k < matched; k++) feed_byte(p, SSE_BOM[k]);
}

void sse_parser_feed(sse_parser_t *p, const char *bytes, size_t n) {
    if (!p || !bytes) return;

    for (size_t i = 0; i < n; i++) {
        char c = bytes[i];
        p->n_bytes++;

        /* One optional leading U+FEFF is stripped. The match straddles feed()
         * calls like everything else in this file. */
        if (p->bom < 3) {
            if (c == SSE_BOM[p->bom]) { p->bom++; continue; }
            bom_replay(p);
        }
        feed_byte(p, c);
    }
}

void sse_parser_finish(sse_parser_t *p) {
    if (!p) return;
    if (p->bom < 3) bom_replay(p);                   /* a truncated BOM was data */
    if (p->line_len || p->line_over) end_line(p);    /* unterminated last line   */
    if (p->data_len) dispatch(p);                    /* no final blank line      */
}

uint32_t sse_parser_events(const sse_parser_t *p)    { return p ? p->n_events : 0; }
uint32_t sse_parser_dropped(const sse_parser_t *p)   { return p ? p->n_dropped : 0; }
uint32_t sse_parser_comments(const sse_parser_t *p)  { return p ? p->n_comments : 0; }
uint32_t sse_parser_bad_lines(const sse_parser_t *p) { return p ? p->n_bad_lines : 0; }
uint32_t sse_parser_bytes(const sse_parser_t *p)     { return p ? p->n_bytes : 0; }

/* ====================================================================== */
/* layer 2 — reassembly                                                    */
/* ====================================================================== */

/* Stop emitting structure, after closing whatever block is open. Called the
 * moment an append would eat into SSE_TAIL_RESERVE. Everything written from
 * here on comes out of that reserve, and only ever once: at most one block
 * closer (3 bytes) plus the document tail (72 bytes) — see SSE_TAIL_RESERVE. */
static void freeze(sse_msg_t *m) {
    if (m->frozen) return;
    m->frozen  = 1;
    m->clipped = 1;
    if      (m->block == SSE_BLK_TEXT) json_put(&m->w, "\"}");
    else if (m->block == SSE_BLK_TOOL) json_put(&m->w, "{}}");
    /* `block` is deliberately LEFT SET. Block tracking carries on after a
     * freeze so text deltas keep reaching the live echo: running out of room to
     * *remember* the reply is no reason to stop *showing* it. close_block() and
     * start_block() are no-ops while frozen, so nothing more is written. */
}

/* Reserve room for a whole structural fragment BEFORE any of it is written. A
 * fragment is never emitted half-way: half a block is invalid JSON, a missing
 * block is merely a shorter reply. */
static int mneed(sse_msg_t *m, size_t n) {
    if (m->frozen || m->w.overflow) return 0;
    if (m->w.len + n + SSE_TAIL_RESERVE >= m->w.cap) { freeze(m); return 0; }
    return 1;
}

static int mput_n(sse_msg_t *m, const char *s, size_t n) {
    if (!mneed(m, n)) return 0;
    json_put_n(&m->w, s, n);
    return 1;
}

static int mput(sse_msg_t *m, const char *s) { return mput_n(m, s, zlen(s)); }

/* Locate a JSON string member as its RAW span — quotes and escapes exactly as
 * they arrived — or fall back to a literal. Nothing is decoded and nothing is
 * re-escaped, so no byte can change meaning on the way through. */
static void span_or(const json_value_t *obj, const char *key, const char *fb,
                    const char **p, size_t *n) {
    json_value_t v;
    if (obj && json_get(obj, key, &v) == JSON_OK &&
        v.type == JSON_STRING && v.len >= 2) {
        *p = v.start;
        *n = v.len;
        return;
    }
    *p = fb;
    *n = zlen(fb);
}

/* The inner bytes of a JSON string span (no quotes), for appending into a
 * string we are building. 0 when `v` is not a usable string. */
static int str_inner(const json_value_t *v, const char **p, size_t *n) {
    if (!v || v->type != JSON_STRING || v->len < 2) return 0;
    *p = v->start + 1;
    *n = v->len - 2;
    return 1;
}

void sse_msg_init(sse_msg_t *m, char *out, size_t cap, sse_text_fn fn, void *ctx) {
    if (!m) return;
    if (!out) cap = 0;
    json_writer_init(&m->w, out, cap);
    m->on_text        = fn;
    m->ctx            = ctx;
    m->started        = 0;
    m->block          = SSE_BLK_NONE;
    m->done           = 0;
    m->closed         = 0;
    m->clipped        = 0;
    m->frozen         = 0;
    m->tool_over      = 0;
    m->tool_lost      = 0;
    m->blocks         = 0;
    m->text_bytes     = 0;
    m->deltas         = 0;
    m->bad_events     = 0;
    m->unknown_events = 0;
    m->stray_deltas   = 0;
    m->stop[0]        = '\0';
    m->err            = SSE_OK;
    m->err_msg[0]     = '\0';
    m->tool_len       = 0;
}

static void set_err(sse_msg_t *m, int err, const char *msg) {
    if (m->err != SSE_OK) return;                    /* the first error wins */
    m->err = err;
    copy_clamped(m->err_msg, sizeof m->err_msg, msg, zlen(msg));
}

/* `{"id":..,"type":"message","role":..,"model":..,"content":[` */
static void begin_message(sse_msg_t *m, const json_value_t *msg) {
    if (m->started) return;

    const char *id, *role, *model;
    size_t      idn, rolen, modeln;
    span_or(msg, "id",    "\"\"",          &id,    &idn);
    span_or(msg, "role",  "\"assistant\"", &role,  &rolen);
    span_or(msg, "model", "\"\"",          &model, &modeln);

    /* 6 + 25 + 9 + 12 = the four fixed fragments below. */
    if (!mneed(m, 52 + idn + rolen + modeln)) return;

    mput(m, "{\"id\":");
    mput_n(m, id, idn);
    mput(m, ",\"type\":\"message\",\"role\":");
    mput_n(m, role, rolen);
    mput(m, ",\"model\":");
    mput_n(m, model, modeln);
    mput(m, ",\"content\":[");
    m->started = 1;
}

static void close_block(sse_msg_t *m) {
    switch (m->block) {
        case SSE_BLK_TEXT:
            if (!m->frozen) json_put(&m->w, "\"}");  /* 2 bytes of the reserve */
            break;

        case SSE_BLK_TOOL: {
            /* The accumulated partial_json must be a complete JSON OBJECT or the
             * request that echoes it back is invalid. Being parseable is not
             * enough: fragments concatenating to `123` or `"oops"` parse fine and
             * would emit "input":123, which chat.c then hands to tool_dispatch as
             * an argument object. This module's contract is to produce a document
             * indistinguishable from a non-streamed one, so the type is checked
             * here rather than left to whatever the readers happen to reject.
             * Substitute an empty object when it is not, which the tools report as
             * missing arguments — a legible error the model can recover from. */
            json_value_t v;
            int ok = !m->tool_over && m->tool_len &&
                     json_parse(m->tool, m->tool_len, &v) == JSON_OK &&
                     v.type == JSON_OBJECT;
            if (m->frozen) break;                    /* freeze() closed it */
            if (ok && mneed(m, m->tool_len + 1)) {
                json_put_n(&m->w, m->tool, m->tool_len);
                json_put_char(&m->w, '}');
            } else if (!m->frozen) {                 /* mneed() may have frozen */
                json_put(&m->w, "{}}");
            }
            break;
        }

        case SSE_BLK_OTHER:                          /* echoed whole at start */
        default:
            break;
    }
    m->block     = SSE_BLK_NONE;
    m->tool_len  = 0;
    m->tool_over = 0;
}

static void start_block(sse_msg_t *m, const json_value_t *ev) {
    json_value_t cb, type;

    if (m->block != SSE_BLK_NONE) close_block(m);    /* missing stop: recover */
    begin_message(m, (const json_value_t *)0);

    if (json_get(ev, "content_block", &cb) != JSON_OK || cb.type != JSON_OBJECT) {
        m->bad_events++;
        return;
    }

    size_t comma  = m->blocks ? 1 : 0;
    int    is_txt = json_get(&cb, "type", &type) == JSON_OK &&
                    json_str_eq(&type, "text");
    int    is_use = !is_txt && json_get(&cb, "type", &type) == JSON_OK &&
                    json_str_eq(&type, "tool_use");

    if (is_txt) {
        if (mneed(m, comma + 23)) {
            if (comma) mput(m, ",");
            mput(m, "{\"type\":\"text\",\"text\":\"");
        }
        m->block = SSE_BLK_TEXT;
    } else if (is_use) {
        const char *id, *name;
        size_t      idn, namen;
        span_or(&cb, "id",   "\"\"", &id,   &idn);
        span_or(&cb, "name", "\"\"", &name, &namen);
        /* 24 + 8 + 9 = the three fixed fragments below. */
        if (mneed(m, comma + 41 + idn + namen)) {
            if (comma) mput(m, ",");
            mput(m, "{\"type\":\"tool_use\",\"id\":");
            mput_n(m, id, idn);
            mput(m, ",\"name\":");
            mput_n(m, name, namen);
            mput(m, ",\"input\":");
        }
        m->block     = SSE_BLK_TOOL;
        m->tool_len  = 0;
        m->tool_over = 0;
    } else {
        /* Any other block type (thinking, redacted_thinking, ...) is echoed
         * from its content_block_start and its deltas are ignored. See sse.h. */
        if (mneed(m, comma + cb.len)) {
            if (comma) mput(m, ",");
            mput_n(m, cb.start, cb.len);
        }
        m->block = SSE_BLK_OTHER;
    }
    m->blocks++;
}

static void apply_delta(sse_msg_t *m, const json_value_t *ev) {
    json_value_t d, dt, v;
    const char  *p;
    size_t       n;

    if (json_get(ev, "delta", &d) != JSON_OK || d.type != JSON_OBJECT) {
        m->bad_events++;
        return;
    }
    if (m->block == SSE_BLK_NONE) { m->stray_deltas++; return; }
    if (json_get(&d, "type", &dt) != JSON_OK) { m->bad_events++; return; }

    if (json_str_eq(&dt, "text_delta") && m->block == SSE_BLK_TEXT) {
        if (json_get(&d, "text", &v) != JSON_OK || !str_inner(&v, &p, &n)) {
            m->bad_events++;
            return;
        }
        m->deltas++;
        mput_n(m, p, n);                     /* raw span: never re-escaped */

        /* The live echo is a separate decode: the console wants UTF-8 text, the
         * document wants the escapes exactly as they arrived. The decode cannot
         * overflow — decoded text is never longer than the span it came from,
         * and the span came out of a buffer one byte smaller than scratch. */
        size_t dn = 0;
        int    rc = json_str(&v, m->scratch, sizeof m->scratch, &dn);
        if ((rc == JSON_OK || rc == JSON_ENOSPC) && dn) {
            m->text_bytes += (uint32_t)dn;
            if (m->on_text) m->on_text(m->ctx, m->scratch, dn);
        }
        return;
    }

    if (json_str_eq(&dt, "input_json_delta") && m->block == SSE_BLK_TOOL) {
        /* partial_json's *decoded* value is a fragment of the tool input, so
         * this one really does have to be unescaped before it is appended. */
        if (json_get(&d, "partial_json", &v) != JSON_OK) { m->bad_events++; return; }
        size_t dn = 0;
        int    rc = json_str(&v, m->scratch, sizeof m->scratch, &dn);
        if (rc != JSON_OK && rc != JSON_ENOSPC) { m->bad_events++; return; }
        if (rc == JSON_ENOSPC) { m->tool_over = 1; m->tool_lost = 1; }
        m->deltas++;
        if (m->tool_len + dn > SSE_TOOL_INPUT_MAX) {
            m->tool_over = 1;
            m->tool_lost = 1;
            dn = (m->tool_len < SSE_TOOL_INPUT_MAX)
               ? SSE_TOOL_INPUT_MAX - m->tool_len : 0;
        }
        bcopy_n(m->tool + m->tool_len, m->scratch, dn);
        m->tool_len += dn;
        return;
    }

    /* A delta this block cannot use (a thinking_delta, or a text_delta inside a
     * tool_use). Counted, not fatal. */
    m->unknown_events++;
}

void sse_msg_event(sse_msg_t *m, const sse_event_t *ev) {
    if (!m || !ev || m->closed) return;

    /* OpenAI-style terminator. Anthropic does not send it; proxies and other
     * endpoints do, and it is not JSON — so it has to be recognised before the
     * parse or every one of them would be counted as a malformed event. */
    if (span_eq(ev->data, ev->data_len, "[DONE]")) { m->done = 1; return; }

    json_value_t root, type;
    if (json_parse(ev->data, ev->data_len, &root) != JSON_OK ||
        root.type != JSON_OBJECT) {
        m->bad_events++;
        return;
    }

    /* The event's own "type" is authoritative; the SSE `event:` name merely
     * duplicates it, and a server that disagrees with itself does not get to
     * choose which copy we believe. */
    char kind[40];
    if (json_get(&root, "type", &type) != JSON_OK ||
        json_str(&type, kind, sizeof kind, (size_t *)0) != JSON_OK ||
        kind[0] == '\0')
        copy_clamped(kind, sizeof kind, ev->type, zlen(ev->type));

    if (streq(kind, "content_block_delta")) {
        apply_delta(m, &root);
    } else if (streq(kind, "content_block_start")) {
        start_block(m, &root);
    } else if (streq(kind, "content_block_stop")) {
        if (m->block == SSE_BLK_NONE) m->stray_deltas++;
        close_block(m);
    } else if (streq(kind, "message_start")) {
        json_value_t msg;
        if (json_get(&root, "message", &msg) == JSON_OK && msg.type == JSON_OBJECT)
            begin_message(m, &msg);
        else
            begin_message(m, (const json_value_t *)0);
    } else if (streq(kind, "message_delta")) {
        json_value_t d, sr;
        const char  *sp;
        size_t       sn;
        /* stop_reason goes back out between two quotes in sse_msg_finish(), so
         * keep the RAW span exactly as it arrived — same rule as every other
         * model-controlled string in this file. Decoding it here and emitting
         * the decoded bytes raw is how a `"` or a trailing `\` in stop_reason
         * used to break the document, or splice extra members into it.
         * A value that does not fit whole is dropped rather than clipped: half
         * an escape sequence is not a shorter reason, it is a broken one. */
        if (json_get(&root, "delta", &d) == JSON_OK &&
            json_get(&d, "stop_reason", &sr) == JSON_OK &&
            str_inner(&sr, &sp, &sn)) {
            if (sn < sizeof m->stop) {
                bcopy_n(m->stop, sp, sn);
                m->stop[sn] = '\0';
            } else {
                m->stop[0] = '\0';
                m->unknown_events++;   /* metadata, not content: stop_reason null */
            }
        }
    } else if (streq(kind, "message_stop")) {
        m->done = 1;
    } else if (streq(kind, "ping")) {
        /* keepalive */
    } else if (streq(kind, "error")) {
        json_value_t e, msg;
        char         buf[SSE_ERR_MAX];
        buf[0] = '\0';
        if (json_get(&root, "error", &e) == JSON_OK &&
            json_get(&e, "message", &msg) == JSON_OK)
            json_str(&msg, buf, sizeof buf, (size_t *)0);
        set_err(m, SSE_ESTREAM, buf[0] ? buf : "the stream reported an error");
    } else {
        m->unknown_events++;
    }
}

/* The document an empty (or hopelessly clipped) stream still has to produce. */
static const char EMPTY_MSG[] =
    "{\"id\":\"\",\"type\":\"message\",\"role\":\"assistant\","
    "\"model\":\"\",\"content\":[";

int sse_msg_finish(sse_msg_t *m, size_t *out_len) {
    if (!m) return SSE_EINVAL;
    if (out_len) *out_len = 0;
    if (!m->w.buf) return SSE_ENOSPC;

    if (!m->closed) {
        m->closed = 1;
        close_block(m);

        if (!m->started) {
            /* Not one structural byte was ever written (an empty stream, or a
             * buffer too small for even the prefix). Start clean. */
            json_writer_init(&m->w, m->w.buf, m->w.cap);
            json_put(&m->w, EMPTY_MSG);
            m->started = 1;
        }

        /* From here the SSE_TAIL_RESERVE bytes mneed() held back are spent, so
         * these appends go straight at the writer. */
        json_put(&m->w, "],\"stop_reason\":");
        if (m->stop[0]) {
            json_put_char(&m->w, '"');
            json_put(&m->w, m->stop);
            json_put_char(&m->w, '"');
        } else {
            json_put(&m->w, "null");
        }
        json_put(&m->w, ",\"stop_sequence\":null}");
    }

    if (!json_writer_ok(&m->w)) return SSE_ENOSPC;

    size_t n = json_writer_len(&m->w);
    if (out_len) *out_len = n;

    /* Belt and braces. This document is what net/chat.c echoes back into the
     * next request, so prove it parses here rather than on the wire. */
    json_value_t v;
    if (json_parse(m->w.buf, n, &v) != JSON_OK) return SSE_EPROTO;

    return m->err;
}

int         sse_msg_complete(const sse_msg_t *m)   { return m ? m->done : 0; }
int         sse_msg_clipped(const sse_msg_t *m)    { return m ? m->clipped : 0; }

int sse_msg_lost(const sse_msg_t *m) {
    if (!m) return 0;
    return (m->bad_events || m->stray_deltas || m->tool_lost) ? 1 : 0;
}
uint32_t    sse_msg_blocks(const sse_msg_t *m)     { return m ? m->blocks : 0; }
uint32_t    sse_msg_text_bytes(const sse_msg_t *m) { return m ? m->text_bytes : 0; }
const char *sse_msg_stop_reason(const sse_msg_t *m){ return m ? m->stop : ""; }
const char *sse_msg_error(const sse_msg_t *m)      { return m ? m->err_msg : ""; }

/* ====================================================================== */
/* layer 3 — the HTTP response driver                                      */
/* ====================================================================== */

static void http_event_cb(void *ctx, const sse_event_t *ev) {
    sse_http_t *h = (sse_http_t *)ctx;
    sse_msg_event(&h->m, ev);
}

void sse_http_init(sse_http_t *h, char *buf, size_t cap, sse_text_fn fn, void *ctx) {
    if (!h) return;
    h->hdr_len        = 0;
    h->hdr_over       = 0;
    h->eoh            = 0;
    h->in_body        = 0;
    h->streaming      = 0;
    h->status         = 0;
    h->status_line[0] = '\0';
    h->buf            = buf;
    h->cap            = cap;
    h->len            = 0;
    h->overflow       = 0;
    h->on_text        = fn;
    h->ctx            = ctx;
    if (buf && cap) buf[0] = '\0';
    sse_parser_init(&h->p, http_event_cb, h);
    sse_msg_init(&h->m, buf, cap, fn, ctx);
}

/* The status line and the one header that decides everything. Called once, on
 * the byte that completes the header block. */
static void headers_done(sse_http_t *h) {
    const char *p   = h->hdr;
    size_t      n   = h->hdr_len;
    size_t      eol = 0;

    while (eol < n && p[eol] != '\r' && p[eol] != '\n') eol++;
    copy_clamped(h->status_line, sizeof h->status_line, p, eol);

    /* status code = the digits after the first space of the status line. An
     * HTTP status is exactly three digits; anything longer is not a status, and
     * multiplying an int by ten until it wraps is undefined behaviour on
     * network-controlled input. Stop at three and reject a longer run. */
    size_t sp = 0;
    while (sp < eol && p[sp] != ' ') sp++;
    int    code = 0;
    size_t i = sp + 1, digits = 0;
    while (i < eol && p[i] >= '0' && p[i] <= '9') { code = code * 10 + (p[i] - '0'); i++; digits++; if (digits == 3) break; }
    int trailing = (i < eol && p[i] >= '0' && p[i] <= '9');
    h->status = (digits == 3 && !trailing) ? code : 0;

    /* Sniff; do not assume. We asked for a stream, but a proxy, an error page
     * or a cached response can still hand back something else, and reading that
     * as SSE would swallow the body the operator needs to see.
     *
     * The field name must sit at the START of a header line. An unanchored
     * substring search lets a header *value* — or the status line's reason
     * phrase — decide this, in both directions: a value naming the stream type
     * would run the reassembler over a plain JSON body (destroying it, because
     * raw bytes are never buffered on that path), and a value merely containing
     * "content-type:" would hide the real header behind it. */
    int ct = hdr_field(p, n, "content-type:");
    if (h->status == 200 && ct >= 0) {
        const char *cp   = p + ct;
        size_t      rest = n - (size_t)ct;
        size_t      line = 0;
        while (line < rest && cp[line] != '\r' && cp[line] != '\n') line++;
        if (ifind(cp, line, "text/event-stream") >= 0) h->streaming = 1;
    }
    h->in_body = 1;
}

/* Buffer a byte of a non-SSE body exactly as the buffered transport does. */
static void raw_byte(sse_http_t *h, char c) {
    if (h->buf && h->len + 1 < h->cap) {
        h->buf[h->len++] = c;
        h->buf[h->len]   = '\0';
    } else {
        h->overflow = 1;
    }
}

void sse_http_feed(sse_http_t *h, const char *bytes, size_t n) {
    if (!h || !bytes) return;

    size_t i = 0;
    while (i < n && !h->in_body) {
        char c = bytes[i++];
        if (h->hdr_len < SSE_HDR_MAX) h->hdr[h->hdr_len++] = c;
        else                          h->hdr_over = 1;

        /* Resumable CRLFCRLF (and lenient LFLF) scan.
         * 0 = mid-line, 1 = after CR, 2 = at a line start, 3 = CR at a start. */
        switch (h->eoh) {
            case 0: h->eoh = (c == '\r') ? 1 : (c == '\n') ? 2 : 0; break;
            case 1: h->eoh = (c == '\n') ? 2 : (c == '\r') ? 1 : 0; break;
            case 2:
                if      (c == '\n') headers_done(h);
                else if (c == '\r') h->eoh = 3;
                else                h->eoh = 0;
                break;
            default:
                if      (c == '\n') headers_done(h);
                else if (c == '\r') h->eoh = 1;
                else                h->eoh = 0;
                break;
        }
    }
    if (i >= n) return;

    if (h->streaming) sse_parser_feed(&h->p, bytes + i, n - i);
    else for (; i < n; i++) raw_byte(h, bytes[i]);
}

int sse_http_finish(sse_http_t *h, model_response_t *out) {
    if (!h || !out) return SSE_EINVAL;

    out->http_status = h->status;
    copy_clamped(out->status_line, sizeof out->status_line,
                 h->status_line, zlen(h->status_line));
    out->body      = h->buf;
    out->body_len  = 0;
    out->truncated = 0;

    if (!h->in_body) return SSE_EPROTO;              /* headers never completed */

    if (!h->streaming) {
        if (h->buf && h->cap) h->buf[h->len] = '\0';
        out->body_len  = h->len;
        out->truncated = h->overflow;
        return SSE_OK;
    }

    sse_parser_finish(&h->p);

    size_t len = 0;
    int    rc  = sse_msg_finish(&h->m, &len);
    out->body_len = len;

    /* A stream that stopped without message_stop is a cut-off reply, and that
     * is exactly what `truncated` means to every caller above this line.
     *
     * So is a stream that LOST something in the middle and then ended tidily.
     * The framing layer drops an over-long line or event and counts it, but a
     * drop between two input_json_delta fragments splices the surviving prefix
     * and suffix into a tool input that is still well-formed JSON and no longer
     * says what the model said. Every counter that means "a byte the model sent
     * did not make it into this document" has to reach the caller, or chat.c
     * dispatches arguments nobody wrote. */
    out->truncated = (!sse_msg_complete(&h->m) || sse_msg_clipped(&h->m) ||
                      sse_msg_lost(&h->m)      || h->hdr_over ||
                      sse_parser_dropped(&h->p) ||
                      sse_parser_bad_lines(&h->p)) ? 1 : 0;
    return rc;
}

int sse_http_streamed(const sse_http_t *h) { return h ? h->streaming : 0; }

const char *sse_http_error(const sse_http_t *h) {
    if (!h) return "";
    if (h->streaming && h->m.err_msg[0]) return h->m.err_msg;
    return "";
}
