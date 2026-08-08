/* json.c — the JSON reader/writer described in include/json.h.
 *
 * Everything here is pure: no allocation, no globals, no kernel services, no
 * libc. The only inputs are the caller's buffers and the only outputs are the
 * caller's buffers, which is why this file compiles unchanged both freestanding
 * (into the kernel) and natively (into tests/host/test_json.c).
 *
 * The scanner is a straight recursive-descent validator. json_parse() walks the
 * whole document once and rejects anything malformed, so navigation afterwards
 * can assume well-formedness — but every navigation step re-scans with the same
 * bounds-checked routines anyway, because a json_value_t handed in by a caller
 * is untrusted input like any other.
 */

#include "json.h"

/* ====================================================================== */
/* tiny local helpers (no libc, no port/ shims)                            */
/* ====================================================================== */

static size_t zlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ====================================================================== */
/* scanner                                                                 */
/* ====================================================================== */

typedef struct {
    const char *end;
    int         err;      /* JSON_EINVAL or JSON_EDEPTH, sticky */
} pctx_t;

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && is_ws(*p)) p++;
    return p;
}

static const char *fail(pctx_t *c, int err) {
    if (!c->err) c->err = err;
    return (const char *)0;
}

/* Scan a string starting at the opening quote. Returns one past the closing
 * quote. Rejects raw control characters and bad escapes, as RFC 8259 requires —
 * a lenient parser here would be a way to smuggle bytes past the kernel. */
static const char *scan_string(pctx_t *c, const char *p) {
    const char *end = c->end;
    if (p >= end || *p != '"') return fail(c, JSON_EINVAL);
    p++;
    while (p < end) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"') return p + 1;
        if (ch < 0x20) return fail(c, JSON_EINVAL);
        if (ch == '\\') {
            p++;
            if (p >= end) return fail(c, JSON_EINVAL);
            switch (*p) {
                case '"': case '\\': case '/': case 'b':
                case 'f': case 'n':  case 'r': case 't':
                    p++;
                    break;
                case 'u':
                    if (end - p < 5) return fail(c, JSON_EINVAL);
                    for (int i = 1; i <= 4; i++)
                        if (hexval(p[i]) < 0) return fail(c, JSON_EINVAL);
                    p += 5;
                    break;
                default:
                    return fail(c, JSON_EINVAL);
            }
        } else {
            p++;
        }
    }
    return fail(c, JSON_EINVAL);          /* unterminated */
}

static const char *scan_number(pctx_t *c, const char *p) {
    const char *end = c->end;
    const char *s = p;
    if (p < end && *p == '-') p++;
    if (p >= end) return fail(c, JSON_EINVAL);
    if (*p == '0') {
        p++;
    } else if (is_digit(*p)) {
        while (p < end && is_digit(*p)) p++;
    } else {
        return fail(c, JSON_EINVAL);
    }
    if (p < end && *p == '.') {
        p++;
        if (p >= end || !is_digit(*p)) return fail(c, JSON_EINVAL);
        while (p < end && is_digit(*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || !is_digit(*p)) return fail(c, JSON_EINVAL);
        while (p < end && is_digit(*p)) p++;
    }
    return p == s ? fail(c, JSON_EINVAL) : p;
}

static const char *scan_lit(pctx_t *c, const char *p, const char *lit) {
    size_t n = zlen(lit);
    if ((size_t)(c->end - p) < n) return fail(c, JSON_EINVAL);
    for (size_t i = 0; i < n; i++)
        if (p[i] != lit[i]) return fail(c, JSON_EINVAL);
    return p + n;
}

/* Scan one value. Returns one past it, or NULL with c->err set. */
static const char *scan_value(pctx_t *c, const char *p, int depth,
                              json_value_t *out) {
    const char *end = c->end;
    if (depth > JSON_MAX_DEPTH) return fail(c, JSON_EDEPTH);
    p = skip_ws(p, end);
    if (p >= end) return fail(c, JSON_EINVAL);

    const char *start = p;
    json_type_t type;

    switch (*p) {
        case '"':
            type = JSON_STRING;
            p = scan_string(c, p);
            break;
        case '{': {
            type = JSON_OBJECT;
            p = skip_ws(p + 1, end);
            if (p < end && *p == '}') { p++; break; }
            for (;;) {
                p = skip_ws(p, end);
                p = scan_string(c, p);
                if (!p) return (const char *)0;
                p = skip_ws(p, end);
                if (p >= end || *p != ':') return fail(c, JSON_EINVAL);
                p = scan_value(c, p + 1, depth + 1, (json_value_t *)0);
                if (!p) return (const char *)0;
                p = skip_ws(p, end);
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; break; }
                return fail(c, JSON_EINVAL);
            }
            break;
        }
        case '[': {
            type = JSON_ARRAY;
            p = skip_ws(p + 1, end);
            if (p < end && *p == ']') { p++; break; }
            for (;;) {
                p = scan_value(c, p, depth + 1, (json_value_t *)0);
                if (!p) return (const char *)0;
                p = skip_ws(p, end);
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; break; }
                return fail(c, JSON_EINVAL);
            }
            break;
        }
        case 't': type = JSON_BOOL;   p = scan_lit(c, p, "true");  break;
        case 'f': type = JSON_BOOL;   p = scan_lit(c, p, "false"); break;
        case 'n': type = JSON_NULL;   p = scan_lit(c, p, "null");  break;
        default:  type = JSON_NUMBER; p = scan_number(c, p);       break;
    }

    if (!p) return (const char *)0;
    if (out) {
        out->type  = type;
        out->start = start;
        out->len   = (size_t)(p - start);
    }
    return p;
}

int json_parse(const char *text, size_t len, json_value_t *out) {
    if (out) { out->type = JSON_INVALID; out->start = (const char *)0; out->len = 0; }
    if (!text || !out) return JSON_EINVAL;

    pctx_t c;
    c.end = text + len;
    c.err = 0;

    const char *p = scan_value(&c, text, 1, out);
    if (!p) {
        out->type = JSON_INVALID;
        return c.err ? c.err : JSON_EINVAL;
    }
    p = skip_ws(p, c.end);
    if (p != c.end) { out->type = JSON_INVALID; return JSON_EINVAL; }
    return JSON_OK;
}

int json_parse_str(const char *text, json_value_t *out) {
    return json_parse(text, zlen(text), out);
}

/* ====================================================================== */
/* navigation                                                              */
/* ====================================================================== */

/* Walk the members of an object or the elements of an array.
 *
 * `want_key`/`index` select the mode:
 *   - key != NULL : return the value whose member name equals `key`.
 *   - key == NULL : return element/member number `index`.
 * For objects, `key_out` receives the member name span when requested. */
static int walk(const json_value_t *v, const char *key, size_t index,
                json_value_t *key_out, json_value_t *val_out, size_t *count_out) {
    if (!v || (v->type != JSON_OBJECT && v->type != JSON_ARRAY))
        return JSON_ETYPE;

    int is_obj = (v->type == JSON_OBJECT);

    pctx_t c;
    c.end = v->start + v->len;
    c.err = 0;

    const char *p   = v->start + 1;           /* just past '{' or '[' */
    const char *end = c.end - 1;              /* just before '}' or ']' */
    size_t      n   = 0;

    p = skip_ws(p, end);
    while (p < end) {
        json_value_t k = { JSON_INVALID, (const char *)0, 0 };
        if (is_obj) {
            const char *ks = p;
            p = scan_string(&c, p);
            if (!p) return JSON_EINVAL;
            k.type = JSON_STRING; k.start = ks; k.len = (size_t)(p - ks);
            p = skip_ws(p, end);
            if (p >= end || *p != ':') return JSON_EINVAL;
            p++;
        }

        json_value_t val;
        p = scan_value(&c, p, 1, &val);
        if (!p) return JSON_EINVAL;

        int hit = key ? (is_obj && json_str_eq(&k, key)) : (n == index);
        if (hit) {
            if (key_out) *key_out = k;
            if (val_out) *val_out = val;
            if (!count_out) return JSON_OK;
        }
        n++;

        p = skip_ws(p, end);
        if (p < end && *p == ',') { p = skip_ws(p + 1, end); continue; }
        break;
    }

    if (count_out) { *count_out = n; return JSON_OK; }
    return JSON_ENOENT;
}

size_t json_count(const json_value_t *v) {
    size_t n = 0;
    if (walk(v, (const char *)0, 0, (json_value_t *)0, (json_value_t *)0, &n) != JSON_OK)
        return 0;
    return n;
}

int json_get(const json_value_t *obj, const char *key, json_value_t *out) {
    if (out) { out->type = JSON_INVALID; out->start = (const char *)0; out->len = 0; }
    if (!obj || !key) return JSON_EINVAL;
    if (obj->type != JSON_OBJECT) return JSON_ETYPE;
    return walk(obj, key, 0, (json_value_t *)0, out, (size_t *)0);
}

int json_at(const json_value_t *arr, size_t index, json_value_t *out) {
    if (out) { out->type = JSON_INVALID; out->start = (const char *)0; out->len = 0; }
    if (!arr) return JSON_EINVAL;
    if (arr->type != JSON_ARRAY) return JSON_ETYPE;
    return walk(arr, (const char *)0, index, (json_value_t *)0, out, (size_t *)0);
}

int json_member(const json_value_t *obj, size_t index,
                json_value_t *key_out, json_value_t *val_out) {
    if (key_out) { key_out->type = JSON_INVALID; key_out->start = (const char *)0; key_out->len = 0; }
    if (val_out) { val_out->type = JSON_INVALID; val_out->start = (const char *)0; val_out->len = 0; }
    if (!obj) return JSON_EINVAL;
    if (obj->type != JSON_OBJECT) return JSON_ETYPE;
    return walk(obj, (const char *)0, index, key_out, val_out, (size_t *)0);
}

/* ====================================================================== */
/* string decoding                                                         */
/* ====================================================================== */

static int utf8_encode(uint32_t cp, unsigned char out[4]) {
    if (cp < 0x80)    { out[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (unsigned char)(0xC0 | (cp >> 6));
                        out[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (unsigned char)(0xE0 | (cp >> 12));
                        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Read four hex digits at p (caller guarantees p+4 <= end); -1 if not hex. */
static int hex4(const char *p, const char *end) {
    if (end - p < 4) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(p[i]);
        if (h < 0) return -1;
        v = (v << 4) | h;
    }
    return v;
}

/* Decode one source character of a string body into UTF-8. Advances *pp.
 * Returns the number of bytes produced (1..4), or 0 if the input is malformed
 * (which json_parse would already have rejected, but this is the network). */
static int decode_one(const char **pp, const char *end, unsigned char out[4]) {
    const char *p = *pp;
    if (p >= end) return 0;

    if (*p != '\\') { out[0] = (unsigned char)*p; *pp = p + 1; return 1; }

    p++;
    if (p >= end) { *pp = p; return 0; }
    char e = *p++;
    switch (e) {
        case '"':  out[0] = '"';  *pp = p; return 1;
        case '\\': out[0] = '\\'; *pp = p; return 1;
        case '/':  out[0] = '/';  *pp = p; return 1;
        case 'b':  out[0] = '\b'; *pp = p; return 1;
        case 'f':  out[0] = '\f'; *pp = p; return 1;
        case 'n':  out[0] = '\n'; *pp = p; return 1;
        case 'r':  out[0] = '\r'; *pp = p; return 1;
        case 't':  out[0] = '\t'; *pp = p; return 1;
        case 'u': {
            int hi = hex4(p, end);
            if (hi < 0) { *pp = end; return 0; }
            p += 4;
            uint32_t cp = (uint32_t)hi;
            if (cp >= 0xD800 && cp <= 0xDBFF) {              /* high surrogate */
                if (end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    int lo = hex4(p + 2, end);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + ((uint32_t)lo - 0xDC00u);
                        p += 6;
                    } else {
                        cp = 0xFFFD;                          /* unpaired high */
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;                                  /* stray low */
            }
            *pp = p;
            return utf8_encode(cp, out);
        }
        default:
            *pp = p;
            return 0;
    }
}

int json_str(const json_value_t *v, char *dst, size_t cap, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (cap) dst[0] = '\0';
    if (!v || !dst || cap == 0) return JSON_EINVAL;
    if (v->type != JSON_STRING || v->len < 2) return JSON_ETYPE;

    const char *p   = v->start + 1;
    const char *end = v->start + v->len - 1;
    size_t      off = 0;
    int         rc  = JSON_OK;

    while (p < end) {
        unsigned char buf[4];
        int n = decode_one(&p, end, buf);
        if (n == 0) { rc = JSON_EINVAL; break; }
        if (off + (size_t)n >= cap) { rc = JSON_ENOSPC; break; }
        for (int i = 0; i < n; i++) dst[off++] = (char)buf[i];
    }
    dst[off] = '\0';
    if (out_len) *out_len = off;
    return rc;
}

int json_get_str(const json_value_t *obj, const char *key, char *dst, size_t cap) {
    json_value_t v;
    int rc = json_get(obj, key, &v);
    if (rc != JSON_OK) { if (cap) dst[0] = '\0'; return rc; }
    return json_str(&v, dst, cap, (size_t *)0);
}

int json_str_eq(const json_value_t *v, const char *s) {
    if (!v || !s || v->type != JSON_STRING || v->len < 2) return 0;

    const char *p   = v->start + 1;
    const char *end = v->start + v->len - 1;

    while (p < end) {
        unsigned char buf[4];
        int n = decode_one(&p, end, buf);
        if (n == 0) return 0;
        for (int i = 0; i < n; i++) {
            if (*s == '\0' || (unsigned char)*s != buf[i]) return 0;
            s++;
        }
    }
    return *s == '\0';
}

/* ====================================================================== */
/* scalar decoding                                                         */
/* ====================================================================== */

int json_int(const json_value_t *v, int64_t *out) {
    if (!v || !out) return JSON_EINVAL;
    if (v->type != JSON_NUMBER) return JSON_ETYPE;

    const char *p   = v->start;
    const char *end = v->start + v->len;
    int         neg = 0;

    if (p < end && *p == '-') { neg = 1; p++; }

    /* The representable magnitude depends on the sign. Bounding both at INT64_MAX
     * made -9223372036854775808 — a value that fits int64_t exactly — come back
     * JSON_EINVAL, which the header documents as meaning overflow. INT64_MIN is
     * not overflow, and it was the one int64 this parser could not express. */
    const uint64_t limit = neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;

    uint64_t mag = 0;
    int      any = 0;
    for (; p < end && is_digit(*p); p++) {
        if (mag > limit / 10u) return JSON_EINVAL;
        mag = mag * 10u + (uint64_t)(*p - '0');
        if (mag > limit) return JSON_EINVAL;
        any = 1;
    }
    if (!any) return JSON_EINVAL;

    if (p < end && *p == '.') {              /* truncate toward zero */
        p++;
        while (p < end && is_digit(*p)) p++;
    }
    if (p < end) return JSON_EINVAL;          /* exponent (or junk): refuse */

    /* INT64_MIN spelled without ever forming +9223372036854775808 as an int64_t:
     * casting 2^63 to a signed type is implementation-defined, and negating
     * INT64_MAX+1 would be the overflow this function exists to report. */
    if (neg) *out = (mag == 0x8000000000000000ull) ? INT64_MIN : -(int64_t)mag;
    else     *out = (int64_t)mag;
    return JSON_OK;
}

int json_bool(const json_value_t *v, int *out) {
    if (!v || !out) return JSON_EINVAL;
    if (v->type != JSON_BOOL) return JSON_ETYPE;
    *out = (v->len == 4);                     /* "true" vs "false" */
    return JSON_OK;
}

/* ====================================================================== */
/* encoding                                                                */
/* ====================================================================== */

/* Append one byte to a snprintf-style bounded destination. `*need` counts what
 * a big-enough buffer would have held, so the caller can detect truncation. */
static void emit(char *dst, size_t cap, size_t *need, char c) {
    if (*need + 1 < cap) dst[*need] = c;
    (*need)++;
}

size_t json_escape_n(char *dst, size_t cap, const char *src, size_t n) {
    static const char hex[] = "0123456789abcdef";
    size_t need = 0;

    if (!src) n = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  emit(dst, cap, &need, '\\'); emit(dst, cap, &need, '"');  break;
            case '\\': emit(dst, cap, &need, '\\'); emit(dst, cap, &need, '\\'); break;
            case '\b': emit(dst, cap, &need, '\\'); emit(dst, cap, &need, 'b');  break;
            case '\f': emit(dst, cap, &need, '\\'); emit(dst, cap, &need, 'f');  break;
            case '\n': emit(dst, cap, &need, '\\'); emit(dst, cap, &need, 'n');  break;
            case '\r': emit(dst, cap, &need, '\\'); emit(dst, cap, &need, 'r');  break;
            case '\t': emit(dst, cap, &need, '\\'); emit(dst, cap, &need, 't');  break;
            default:
                if (c < 0x20) {
                    emit(dst, cap, &need, '\\'); emit(dst, cap, &need, 'u');
                    emit(dst, cap, &need, '0');  emit(dst, cap, &need, '0');
                    emit(dst, cap, &need, hex[(c >> 4) & 0xF]);
                    emit(dst, cap, &need, hex[c & 0xF]);
                } else {
                    emit(dst, cap, &need, (char)c);
                }
                break;
        }
    }
    if (cap) dst[need < cap ? need : cap - 1] = '\0';
    return need;
}

size_t json_escape(char *dst, size_t cap, const char *src) {
    return json_escape_n(dst, cap, src, zlen(src));
}

/* ---- writer ---- */

void json_writer_init(json_writer_t *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
    if (cap) buf[0] = '\0';
    else     w->overflow = 1;
}

void json_put_char(json_writer_t *w, char c) {
    if (w->overflow) return;
    if (w->len + 1 >= w->cap) { w->overflow = 1; return; }
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
}

void json_put_n(json_writer_t *w, const char *s, size_t n) {
    if (w->overflow || !s) return;
    if (w->len + n >= w->cap) { w->overflow = 1; return; }
    for (size_t i = 0; i < n; i++) w->buf[w->len + i] = s[i];
    w->len += n;
    w->buf[w->len] = '\0';
}

void json_put(json_writer_t *w, const char *s) {
    json_put_n(w, s, zlen(s));
}

void json_put_uint(json_writer_t *w, uint64_t v) {
    char t[20];
    int  n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) json_put_char(w, t[--n]);
}

void json_put_int(json_writer_t *w, int64_t v) {
    if (v < 0) {
        json_put_char(w, '-');
        json_put_uint(w, (uint64_t)(-(v + 1)) + 1u);   /* INT64_MIN-safe */
    } else {
        json_put_uint(w, (uint64_t)v);
    }
}

void json_put_string(json_writer_t *w, const char *s) {
    if (w->overflow) return;
    json_put_char(w, '"');
    if (w->overflow) return;
    /* Escape straight into the writer's tail; if it doesn't fit, mark overflow
     * and leave the buffer NUL-terminated where it was. */
    size_t room = w->cap - w->len;
    size_t need = json_escape(w->buf + w->len, room, s);
    if (need + 1 >= room) {
        w->overflow = 1;
        w->buf[w->len] = '\0';
        return;
    }
    w->len += need;
    json_put_char(w, '"');
}

int json_writer_ok(const json_writer_t *w) { return w && !w->overflow; }

size_t json_writer_len(const json_writer_t *w) { return w ? w->len : 0; }

/* ====================================================================== */
/* Anthropic Messages sugar                                                */
/* ====================================================================== */

int json_msg_content(const json_value_t *root, json_value_t *out) {
    json_value_t c;
    int rc = json_get(root, "content", &c);
    if (rc != JSON_OK) return rc;
    if (c.type != JSON_ARRAY) return JSON_ETYPE;
    if (out) *out = c;
    return JSON_OK;
}

int json_msg_text(const json_value_t *root, char *dst, size_t cap, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!dst || cap == 0) return JSON_EINVAL;
    dst[0] = '\0';

    json_value_t content;
    int rc = json_msg_content(root, &content);
    if (rc != JSON_OK) return rc;

    size_t off   = 0;
    size_t nblk  = json_count(&content);
    int    found = 0;

    for (size_t i = 0; i < nblk; i++) {
        json_value_t blk, type, text;
        if (json_at(&content, i, &blk) != JSON_OK) break;
        if (blk.type != JSON_OBJECT) continue;
        if (json_get(&blk, "type", &type) != JSON_OK) continue;
        if (!json_str_eq(&type, "text")) continue;
        if (json_get(&blk, "text", &text) != JSON_OK) continue;
        if (text.type != JSON_STRING) continue;

        found = 1;
        size_t n = 0;
        int    r = json_str(&text, dst + off, cap - off, &n);
        off += n;
        if (r == JSON_ENOSPC) { if (out_len) *out_len = off; return JSON_ENOSPC; }
    }

    if (out_len) *out_len = off;
    return found ? JSON_OK : JSON_ENOENT;
}

int json_msg_stop_reason(const json_value_t *root, char *dst, size_t cap) {
    if (!dst || cap == 0) return JSON_EINVAL;
    dst[0] = '\0';

    json_value_t v;
    int rc = json_get(root, "stop_reason", &v);
    if (rc != JSON_OK) return rc;
    if (v.type == JSON_NULL) return JSON_OK;       /* streaming/in-progress */
    return json_str(&v, dst, cap, (size_t *)0);
}
