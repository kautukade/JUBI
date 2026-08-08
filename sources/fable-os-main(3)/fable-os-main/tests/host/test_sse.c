/* test_sse.c — adversarial tests for net/sse.c, the streaming reader.
 *
 * This code faces the network directly: it is the first thing in the kernel to
 * touch a byte that came off the wire, and it runs before anything has been
 * validated. So the bar is the same as net/json.c's — malformed, truncated,
 * over-long, mis-framed and outright hostile input must all fail cleanly, never
 * crash, never write outside the buffer given, and always leave the caller with
 * something legible.
 *
 * THE CENTRAL PROPERTY, asserted everywhere below: delivery boundaries do not
 * change the result. A stream fed one byte at a time must produce byte-for-byte
 * the same events, the same reassembled body and the same echoed text as the
 * same stream fed in one lump. Real TCP segments split events mid-JSON,
 * mid-field, mid-UTF-8 and between the CR and the LF of a line ending, so
 * "works when the buffer happens to hold a whole event" is not a working
 * parser. Every structural test here runs at chunk sizes 1, 2, 3, 5, 7, 13, 64
 * and whole-stream, and compares them against each other.
 *
 * The second invariant, because net/chat.c echoes the reassembled body straight
 * back into the next request: the output is ALWAYS valid JSON. Every test that
 * produces a document parses it, and the fuzzers assert it over ~30k random and
 * mutated streams.
 *
 * NOT covered here, and it cannot be: a real streamed response from the API.
 * There is no key compiled in, so the live path answers 401 and never produces
 * an event stream. Everything below is synthetic bytes shaped exactly like the
 * Messages streaming format.
 */

#include "harness.h"
#include "sse.h"

#include <stdlib.h>

/* ====================================================================== */
/* plumbing                                                                */
/* ====================================================================== */

/* Chunk sizes every structural test is replayed at. 1 is the one that matters;
 * the rest catch state that happens to survive a particular alignment. */
static const size_t CHUNKS[] = { 1, 2, 3, 5, 7, 13, 64, 0 /* 0 = whole */ };
#define NCHUNKS ((int)(sizeof CHUNKS / sizeof CHUNKS[0]))

static void feed_chunked(sse_parser_t *p, const char *s, size_t n, size_t chunk) {
    if (chunk == 0) { sse_parser_feed(p, s, n); return; }
    for (size_t i = 0; i < n; i += chunk) {
        size_t k = n - i < chunk ? n - i : chunk;
        sse_parser_feed(p, s + i, k);
    }
}

/* ---- an event recorder, for testing layer 1 on its own ---- */

#define REC_MAX      64
#define REC_TYPE_MAX 72
#define REC_DATA_MAX 640

typedef struct {
    char   type[REC_MAX][REC_TYPE_MAX];
    char   data[REC_MAX][REC_DATA_MAX];
    char   id[REC_MAX][REC_TYPE_MAX];
    size_t dlen[REC_MAX];
    int    n;
    int    overflowed;      /* more events than the recorder can hold */
} rec_t;

static void rec_cb(void *ctx, const sse_event_t *ev) {
    rec_t *r = (rec_t *)ctx;
    if (r->n >= REC_MAX) { r->overflowed = 1; return; }
    size_t tn = strlen(ev->type);
    if (tn > REC_TYPE_MAX - 1) tn = REC_TYPE_MAX - 1;
    memcpy(r->type[r->n], ev->type, tn);
    r->type[r->n][tn] = '\0';

    size_t in = strlen(ev->id);
    if (in > REC_TYPE_MAX - 1) in = REC_TYPE_MAX - 1;
    memcpy(r->id[r->n], ev->id, in);
    r->id[r->n][in] = '\0';

    size_t dn = ev->data_len;
    if (dn > REC_DATA_MAX - 1) dn = REC_DATA_MAX - 1;
    memcpy(r->data[r->n], ev->data, dn);
    r->data[r->n][dn] = '\0';
    r->dlen[r->n] = ev->data_len;
    r->n++;
}

static sse_parser_t g_parser;

/* Run a stream through layer 1 at one chunk size; results land in `out`. */
static void frame(rec_t *out, const char *s, size_t n, size_t chunk) {
    memset(out, 0, sizeof *out);
    sse_parser_init(&g_parser, rec_cb, out);
    feed_chunked(&g_parser, s, n, chunk);
    sse_parser_finish(&g_parser);
}

/* Frame at every chunk size and assert all of them agree; return the (shared)
 * result via `out`. This is the one-byte-at-a-time proof, applied to
 * every framing test rather than to a single showcase case. */
static rec_t g_ref, g_alt;

static void frame_all(rec_t *out, const char *s, size_t n) {
    frame(&g_ref, s, n, 1);                       /* one byte at a time first */
    for (int c = 1; c < NCHUNKS; c++) {
        frame(&g_alt, s, n, CHUNKS[c]);
        CHECK_EQ(g_alt.n, g_ref.n);
        if (g_alt.n != g_ref.n) continue;
        for (int i = 0; i < g_alt.n; i++) {
            CHECK_STR(g_alt.type[i], g_ref.type[i]);
            CHECK_STR(g_alt.data[i], g_ref.data[i]);
            CHECK_EQ((long long)g_alt.dlen[i], (long long)g_ref.dlen[i]);
        }
    }
    memcpy(out, &g_ref, sizeof *out);
}

/* ---- layers 1+2 wired together, as the transport wires them ---- */

typedef struct {
    sse_parser_t p;
    sse_msg_t    m;
    char         pre[16];
    char         out[16384];
    char         post[16];
    char         echo[16384];
    size_t       echo_len;
} drv_t;

static drv_t g_drv;

static int canaries_ok(const drv_t *d) {
    for (size_t i = 0; i < sizeof d->pre; i++)
        if ((unsigned char)d->pre[i] != 0x7E) return 0;
    for (size_t i = 0; i < sizeof d->post; i++)
        if ((unsigned char)d->post[i] != 0x7E) return 0;
    return 1;
}

static void drv_event(void *ctx, const sse_event_t *ev) {
    sse_msg_event(&((drv_t *)ctx)->m, ev);
}

static void drv_text(void *ctx, const char *t, size_t n) {
    drv_t *d = (drv_t *)ctx;
    for (size_t i = 0; i < n; i++)
        if (d->echo_len + 1 < sizeof d->echo) d->echo[d->echo_len++] = t[i];
    d->echo[d->echo_len] = '\0';
}

static void drv_begin(drv_t *d, size_t cap) {
    memset(d->pre, 0x7E, sizeof d->pre);
    memset(d->post, 0x7E, sizeof d->post);
    memset(d->out, 0, sizeof d->out);
    d->echo_len = 0;
    d->echo[0]  = '\0';
    sse_msg_init(&d->m, d->out, cap ? cap : sizeof d->out, drv_text, d);
    sse_parser_init(&d->p, drv_event, d);
}

/* Reassemble a stream at one chunk size. Returns the sse_msg_finish() code. */
static int reassemble(drv_t *d, const char *s, size_t n, size_t chunk,
                      size_t cap, size_t *out_len) {
    drv_begin(d, cap);
    feed_chunked(&d->p, s, n, chunk);
    sse_parser_finish(&d->p);
    int rc = sse_msg_finish(&d->m, out_len);
    CHECK(canaries_ok(d));
    return rc;
}

/* The whole point, again: reassemble at every chunk size and prove the body and
 * the echoed text are identical every time. Leaves g_drv holding the
 * one-byte-at-a-time run, which is the one the caller then inspects. */
static int reassemble_all(const char *s, size_t n) {
    static char ref_out[sizeof g_drv.out];
    static char ref_echo[sizeof g_drv.echo];
    size_t      ref_len = 0;

    int rc = reassemble(&g_drv, s, n, 1, 0, &ref_len);
    memcpy(ref_out, g_drv.out, sizeof ref_out);
    memcpy(ref_echo, g_drv.echo, sizeof ref_echo);

    for (int c = 1; c < NCHUNKS; c++) {
        size_t len = 0;
        int    rc2 = reassemble(&g_drv, s, n, CHUNKS[c], 0, &len);
        CHECK_EQ(rc2, rc);
        CHECK_EQ((long long)len, (long long)ref_len);
        CHECK_STR(g_drv.out, ref_out);
        CHECK_STR(g_drv.echo, ref_echo);
    }

    /* leave the byte-at-a-time run in place for the caller to inspect */
    size_t len = 0;
    reassemble(&g_drv, s, n, 1, 0, &len);
    return rc;
}

/* Decode all text blocks of the reassembled body. */
static int body_text(const char *body, size_t len, char *dst, size_t cap) {
    json_value_t root;
    if (json_parse(body, len, &root) != JSON_OK) return -1;
    return json_msg_text(&root, dst, cap, (size_t *)0);
}

/* ====================================================================== */
/* sample streams                                                          */
/* ====================================================================== */

/* The Messages streaming format, exactly as the API emits it. */
static const char STREAM_TEXT[] =
"event: message_start\n"
"data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_01Xyz\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-opus-4-8\",\"content\":[],\"stop_reason\":null,\"stop_sequence\":null,\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}\n"
"\n"
"event: content_block_start\n"
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
"\n"
"event: ping\n"
"data: {\"type\": \"ping\"}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"fable\"}}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"-os \"}}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"online\"}}\n"
"\n"
"event: content_block_stop\n"
"data: {\"type\":\"content_block_stop\",\"index\":0}\n"
"\n"
"event: message_delta\n"
"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":15}}\n"
"\n"
"event: message_stop\n"
"data: {\"type\":\"message_stop\"}\n"
"\n";

/* A tool_use turn: the input object arrives as five partial_json fragments,
 * none of which is valid JSON on its own. */
static const char STREAM_TOOL[] =
"event: message_start\n"
"data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_02\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-opus-4-8\",\"content\":[]}}\n"
"\n"
"event: content_block_start\n"
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Reading it.\"}}\n"
"\n"
"event: content_block_stop\n"
"data: {\"type\":\"content_block_stop\",\"index\":0}\n"
"\n"
"event: content_block_start\n"
"data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01A\",\"name\":\"vfs_read\",\"input\":{}}}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"pa\"}}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"th\\\": \\\"/e\"}}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"tc/mo\"}}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"td\\\", \\\"max\\\":1\"}}\n"
"\n"
"event: content_block_delta\n"
"data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"28}\"}}\n"
"\n"
"event: content_block_stop\n"
"data: {\"type\":\"content_block_stop\",\"index\":1}\n"
"\n"
"event: message_delta\n"
"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null}}\n"
"\n"
"event: message_stop\n"
"data: {\"type\":\"message_stop\"}\n"
"\n";

/* ====================================================================== */
/* layer 1 — framing                                                       */
/* ====================================================================== */

static void test_basic_framing(void) {
    static rec_t r;
    static const char s[] = "event: hello\ndata: world\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.type[0], "hello");
    CHECK_STR(r.data[0], "world");
    CHECK_EQ((long long)r.dlen[0], 5);
}

static void test_default_event_name(void) {
    static rec_t r;
    static const char s[] = "data: bare\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.type[0], "message");         /* the spec's default */
    CHECK_STR(r.data[0], "bare");
}

static void test_line_endings(void) {
    static rec_t r;
    /* LF, CRLF and a bare CR must all frame identically. */
    static const char lf[]   = "data: a\n\ndata: b\n\n";
    static const char crlf[] = "data: a\r\n\r\ndata: b\r\n\r\n";
    static const char cr[]   = "data: a\r\rdata: b\r\r";

    frame_all(&r, lf, sizeof lf - 1);
    CHECK_EQ(r.n, 2); CHECK_STR(r.data[0], "a"); CHECK_STR(r.data[1], "b");

    frame_all(&r, crlf, sizeof crlf - 1);
    CHECK_EQ(r.n, 2); CHECK_STR(r.data[0], "a"); CHECK_STR(r.data[1], "b");

    frame_all(&r, cr, sizeof cr - 1);
    CHECK_EQ(r.n, 2); CHECK_STR(r.data[0], "a"); CHECK_STR(r.data[1], "b");

    /* All three endings mixed inside one event. frame_all() feeds one byte at a
     * time, so the CRLF pair is split across feeds every single time — the case
     * that breaks a splitter which only looks at one buffer. */
    static const char mixed[] = "data: a\r\ndata: b\rdata: c\n\n";
    frame_all(&r, mixed, sizeof mixed - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "a\nb\nc");

    /* LF followed by CR is TWO terminators, not one: the CR ends an empty line,
     * which dispatches. Only CRLF collapses. */
    static const char lfcr[] = "data: a\n\rdata: b\n\n";
    frame_all(&r, lfcr, sizeof lfcr - 1);
    CHECK_EQ(r.n, 2);
    CHECK_STR(r.data[0], "a");
    CHECK_STR(r.data[1], "b");
}

static void test_optional_leading_space(void) {
    static rec_t r;
    static const char s[] = "data:x\n\ndata: y\n\ndata:  z\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 3);
    CHECK_STR(r.data[0], "x");
    CHECK_STR(r.data[1], "y");
    CHECK_STR(r.data[2], " z");              /* exactly ONE space is removed */
}

static void test_multiple_data_lines(void) {
    static rec_t r;
    static const char s[] = "data: one\ndata: two\ndata: three\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "one\ntwo\nthree");  /* joined with '\n' */
}

static void test_comments_and_keepalives(void) {
    static rec_t r;
    static const char s[] = ": keepalive\n\n:\n\ndata: real\n\n: another\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "real");
    CHECK_EQ((long long)sse_parser_comments(&g_parser), 3);
    CHECK_EQ((long long)sse_parser_events(&g_parser), 1);
    CHECK_EQ((long long)sse_parser_dropped(&g_parser), 0);
}

static void test_event_without_data_is_not_dispatched(void) {
    static rec_t r;
    /* An `event:` line with no data must stay silent — otherwise every ping
     * with a missing body would look like a real event. */
    static const char s[] = "event: ping\n\nevent: x\nid: 7\n\ndata: yes\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "yes");
    CHECK_STR(r.type[0], "message");         /* the earlier names were reset */
}

static void test_bare_and_empty_fields(void) {
    static rec_t r;
    /* `data` with no colon at all is a field with an empty value; the spec then
     * dispatches an event whose data is the empty string. */
    static const char s[] = "data\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "");
    CHECK_EQ((long long)r.dlen[0], 0);

    static const char s2[] = "data:\n\n";
    frame_all(&r, s2, sizeof s2 - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "");
}

static void test_unknown_fields_ignored(void) {
    static rec_t r;
    static const char s[] =
        "retry: 3000\nfoo: bar\n: c\nDATA: wrongcase\ndata: ok\nxyzzy\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "ok");              /* field names are case-sensitive */
}

static void test_id_is_sticky(void) {
    static rec_t r;
    static const char s[] = "id: 1\ndata: a\n\ndata: b\n\nid: 2\ndata: c\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 3);
    CHECK_STR(r.id[0], "1");
    CHECK_STR(r.id[1], "1");                 /* the last id persists */
    CHECK_STR(r.id[2], "2");
}

static void test_bom(void) {
    static rec_t r;
    static const char s[] = "\xEF\xBB\xBF" "data: a\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "a");               /* the BOM was swallowed */

    /* Only ONE BOM is stripped. A second is ordinary data, so it lands in the
     * field name, the line stops being a `data` field, and no event fires —
     * which is precisely how we can see the bytes were not swallowed. */
    static const char two[] = "\xEF\xBB\xBF\xEF\xBB\xBF" "data: a\n\n";
    frame_all(&r, two, sizeof two - 1);
    CHECK_EQ(r.n, 0);
    CHECK_EQ((long long)sse_parser_bad_lines(&g_parser), 0);

    /* A partial BOM is NOT a BOM: the matched bytes have to be replayed as
     * data, and they pollute the field name the same way. Swallowing them
     * would have produced a perfectly good event here — that is the bug this
     * asserts against, and it can only be caught because the BOM match is
     * itself resumable. */
    static const char part[] = "\xEF\xBB" "data: a\n\n";
    frame_all(&r, part, sizeof part - 1);
    CHECK_EQ(r.n, 0);

    /* Same stray bytes, on their own line: framing recovers completely. */
    static const char part2[] = "\xEF\xBB" "\ndata: a\n\n";
    frame_all(&r, part2, sizeof part2 - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "a");

    /* A stream that is nothing but a truncated BOM: those bytes come back as an
     * unterminated final line, not silently vanish. */
    frame(&r, "\xEF\xBB", 2, 1);
    CHECK_EQ(r.n, 0);
    CHECK_EQ((long long)sse_parser_bytes(&g_parser), 2);
    CHECK_EQ((long long)sse_parser_events(&g_parser), 0);
}

static void test_embedded_nul_in_data(void) {
    static rec_t r;
    static const char s[] = "data: a\0b\n\n";       /* a real NUL byte */
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_EQ((long long)r.dlen[0], 3);              /* length, not strlen */
    CHECK_EQ(r.data[0][0], 'a');
    CHECK_EQ(r.data[0][1], '\0');
    CHECK_EQ(r.data[0][2], 'b');
}

static void test_unterminated_final_event(void) {
    static rec_t r;
    /* The server closed without the final blank line. Being pedantic here would
     * throw away the last delta of every truncated reply. */
    static const char s[] = "data: a\n\ndata: tail";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 2);
    CHECK_STR(r.data[1], "tail");
}

static void test_overlong_line_is_dropped(void) {
    static char s[SSE_LINE_MAX + 600];
    size_t      i = 0;

    memcpy(s + i, "data: ", 6); i += 6;
    for (size_t k = 0; k < SSE_LINE_MAX + 100; k++) s[i++] = 'A';
    s[i++] = '\n';
    s[i++] = '\n';
    memcpy(s + i, "data: after\n\n", 13); i += 13;

    static rec_t r;
    frame_all(&r, s, i);
    /* The giant line is refused whole, and framing RESYNCS: the event after it
     * arrives intact. A parser that lost its place here would lose the rest of
     * the reply, not just the bad event. */
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "after");
    CHECK_EQ((long long)sse_parser_bad_lines(&g_parser), 1);
    CHECK_EQ((long long)sse_parser_dropped(&g_parser), 1);
}

static void test_enormous_event_is_dropped(void) {
    /* Lines that each fit, but whose joined data does not. */
    static char s[SSE_DATA_MAX * 2 + 400];
    size_t      i = 0;

    for (int line = 0; line < 6; line++) {
        memcpy(s + i, "data: ", 6); i += 6;
        for (int k = 0; k < 1000; k++) s[i++] = (char)('a' + line);
        s[i++] = '\n';
    }
    s[i++] = '\n';
    memcpy(s + i, "data: after\n\n", 13); i += 13;

    static rec_t r;
    frame_all(&r, s, i);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "after");
    CHECK_EQ((long long)sse_parser_dropped(&g_parser), 1);
    CHECK_EQ((long long)sse_parser_events(&g_parser), 1);
}

static void test_overlong_event_name_drops_event(void) {
    static char s[SSE_NAME_MAX + 64];
    size_t      i = 0;
    memcpy(s + i, "event: ", 7); i += 7;
    for (size_t k = 0; k < SSE_NAME_MAX + 8; k++) s[i++] = 'n';
    s[i++] = '\n';
    memcpy(s + i, "data: x\n\ndata: ok\n\n", 19); i += 19;

    static rec_t r;
    frame_all(&r, s, i);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "ok");
    CHECK_EQ((long long)sse_parser_dropped(&g_parser), 1);
}

static void test_blank_lines_everywhere(void) {
    static rec_t r;
    static const char s[] = "\n\n\n\n\ndata: a\n\n\n\n\n";
    frame_all(&r, s, sizeof s - 1);
    CHECK_EQ(r.n, 1);
    CHECK_STR(r.data[0], "a");
}

static void test_no_callback_still_counts(void) {
    sse_parser_init(&g_parser, (sse_event_fn)0, (void *)0);
    sse_parser_feed(&g_parser, "data: a\n\ndata: b\n\n", 18);
    sse_parser_finish(&g_parser);
    CHECK_EQ((long long)sse_parser_events(&g_parser), 2);

    /* Null and zero-length feeds are no-ops, not faults. */
    sse_parser_feed(&g_parser, (const char *)0, 10);
    sse_parser_feed(&g_parser, "x", 0);
    CHECK_EQ((long long)sse_parser_events(&g_parser), 2);
    sse_parser_init((sse_parser_t *)0, (sse_event_fn)0, (void *)0);
    sse_parser_finish((sse_parser_t *)0);
    CHECK_EQ((long long)sse_parser_events((const sse_parser_t *)0), 0);
}

/* ====================================================================== */
/* layer 2 — reassembly                                                    */
/* ====================================================================== */

static void test_reassemble_text_turn(void) {
    size_t len = 0;
    int    rc  = reassemble_all(STREAM_TEXT, sizeof STREAM_TEXT - 1);
    CHECK_EQ(rc, SSE_OK);

    sse_msg_finish(&g_drv.m, &len);
    CHECK(len > 0);

    /* The reassembled body is exactly the document a non-streaming request
     * would have returned, so the rest of the kernel cannot tell the
     * difference. */
    char text[512];
    CHECK_EQ(body_text(g_drv.out, len, text, sizeof text), JSON_OK);
    CHECK_STR(text, "fable-os online");

    /* ...and the same text was echoed to the console as it arrived. */
    CHECK_STR(g_drv.echo, "fable-os online");
    CHECK_EQ((long long)sse_msg_text_bytes(&g_drv.m), (long long)strlen("fable-os online"));
    CHECK_EQ((long long)sse_msg_blocks(&g_drv.m), 1);
    CHECK_STR(sse_msg_stop_reason(&g_drv.m), "end_turn");
    CHECK(sse_msg_complete(&g_drv.m));
    CHECK(!sse_msg_clipped(&g_drv.m));

    json_value_t root, c, blk, t;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
    CHECK_EQ(json_get_str(&root, "id", text, sizeof text), JSON_OK);
    CHECK_STR(text, "msg_01Xyz");
    CHECK_EQ(json_get_str(&root, "model", text, sizeof text), JSON_OK);
    CHECK_STR(text, "claude-opus-4-8");
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ((long long)json_count(&c), 1);
    CHECK_EQ(json_at(&c, 0, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "type", &t), JSON_OK);
    CHECK(json_str_eq(&t, "text"));
    CHECK_EQ(json_msg_stop_reason(&root, text, sizeof text), JSON_OK);
    CHECK_STR(text, "end_turn");
}

static void test_reassemble_tool_use(void) {
    size_t len = 0;
    int    rc  = reassemble_all(STREAM_TOOL, sizeof STREAM_TOOL - 1);
    CHECK_EQ(rc, SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    /* net/chat.c needs the tool_use block WHOLE: five partial_json fragments,
     * none of them valid JSON alone, have to come back as one input object. */
    json_value_t root, c, blk, t, in, path, max;
    char         buf[256];
    int64_t      n = 0;

    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ((long long)json_count(&c), 2);

    CHECK_EQ(json_at(&c, 0, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "type", &t), JSON_OK);
    CHECK(json_str_eq(&t, "text"));

    CHECK_EQ(json_at(&c, 1, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "type", &t), JSON_OK);
    CHECK(json_str_eq(&t, "tool_use"));
    CHECK_EQ(json_get_str(&blk, "id", buf, sizeof buf), JSON_OK);
    CHECK_STR(buf, "toolu_01A");
    CHECK_EQ(json_get_str(&blk, "name", buf, sizeof buf), JSON_OK);
    CHECK_STR(buf, "vfs_read");
    CHECK_EQ(json_get(&blk, "input", &in), JSON_OK);
    CHECK_EQ(in.type, JSON_OBJECT);
    CHECK_EQ(json_get(&in, "path", &path), JSON_OK);
    CHECK_EQ(json_str(&path, buf, sizeof buf, (size_t *)0), JSON_OK);
    CHECK_STR(buf, "/etc/motd");
    CHECK_EQ(json_get(&in, "max", &max), JSON_OK);
    CHECK_EQ(json_int(&max, &n), JSON_OK);
    CHECK_EQ(n, 128);

    CHECK_STR(sse_msg_stop_reason(&g_drv.m), "tool_use");
    CHECK_STR(g_drv.echo, "Reading it.");     /* only prose is echoed */
}

static void test_escapes_survive_verbatim(void) {
    /* Every escape the wire can carry, split across deltas at the nastiest
     * points: inside \uXXXX, between the two halves of a surrogate pair, and in
     * the middle of a literal UTF-8 sequence. reassemble_all() feeds one byte at
     * a time, so all of those splits happen for real. */
    static const char s[] =
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"q\\\"q\\\\q\\nq\\tq\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"\\u00e9\\u4e2d\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"\\ud83d\\ude00\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"\xF0\x9F\x9A\x80 \xC3\xA9\"}}\n"
"\n"
"data: {\"type\":\"content_block_stop\",\"index\":0}\n"
"\n"
"data: {\"type\":\"message_stop\"}\n"
"\n";
    size_t len = 0;
    CHECK_EQ(reassemble_all(s, sizeof s - 1), SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    char text[512];
    CHECK_EQ(body_text(g_drv.out, len, text, sizeof text), JSON_OK);

    /* Decoded body text and echoed console text must be byte-identical: the
     * document keeps the escapes as written, the echo decodes them, and both
     * have to mean the same string.
     *
     * KNOWN LIMIT, and this test does not cover it: the split here is of the
     * STREAM, and layer 1 buffers a whole event before layer 2 parses it, so a
     * \uXXXX surrogate PAIR split across two content_block_delta EVENTS never
     * happens above. It does diverge — the document gets the right codepoint
     * because it carries the raw span, while the echo decodes each half
     * independently and prints two U+FFFD. Closing it means carrying a pending
     * high surrogate across deltas in the echo path; until then the byte-identity
     * claim holds for splits within an event, which is the only split
     * api.anthropic.com produces (it sends raw UTF-8, not \u escapes). */
    CHECK_STR(text, g_drv.echo);
    CHECK_STR(text,
        "q\"q\\q\nq\tq"          /* the plain escapes           */
        "\xC3\xA9\xE4\xB8\xAD"    /* \u00e9 \u4e2d               */
        "\xF0\x9F\x98\x80"        /* the surrogate pair          */
        "\xF0\x9F\x9A\x80 \xC3\xA9");  /* literal UTF-8 passed through */
}

static void test_missing_message_stop_is_truncated(void) {
    /* Cut the sample off in the middle of the last delta's JSON — exactly what
     * a dropped connection looks like. */
    size_t cut = sizeof STREAM_TEXT - 1 - 120;
    size_t len = 0;
    int    rc  = reassemble_all(STREAM_TEXT, cut);
    CHECK_EQ(rc, SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    CHECK(!sse_msg_complete(&g_drv.m));      /* the caller learns it was cut */
    json_value_t root;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);   /* still valid */

    /* And every possible truncation point produces valid JSON, not just this
     * one. This is the property net/chat.c depends on. */
    for (size_t k = 1; k < sizeof STREAM_TEXT - 1; k += 7) {
        size_t l = 0;
        int    r = reassemble(&g_drv, STREAM_TEXT, k, 1, 0, &l);
        CHECK(r == SSE_OK || r == SSE_ESTREAM);
        json_value_t v;
        CHECK_EQ(json_parse(g_drv.out, l, &v), JSON_OK);
    }
}

static void test_missing_content_block_stop(void) {
    /* A second block starts while the first is still open. Recovering means
     * closing the first; not recovering means unbalanced JSON. */
    static const char s[] =
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"one\"}}\n"
"\n"
"data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"two\"}}\n"
"\n"
"data: {\"type\":\"message_stop\"}\n"
"\n";
    size_t len = 0;
    CHECK_EQ(reassemble_all(s, sizeof s - 1), SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    char text[128];
    CHECK_EQ(body_text(g_drv.out, len, text, sizeof text), JSON_OK);
    CHECK_STR(text, "onetwo");
    CHECK_EQ((long long)sse_msg_blocks(&g_drv.m), 2);
}

static void test_stray_and_mismatched_deltas(void) {
    /* Deltas with no block open, a stop with nothing to stop, a text_delta
     * inside a tool_use. All counted, none fatal, output still valid. */
    static const char s[] =
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"ghost\"}}\n"
"\n"
"data: {\"type\":\"content_block_stop\",\"index\":9}\n"
"\n"
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"n\",\"input\":{}}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"nope\"}}\n"
"\n"
"data: {\"type\":\"content_block_stop\",\"index\":0}\n"
"\n"
"data: {\"type\":\"message_stop\"}\n"
"\n";
    size_t len = 0;
    CHECK_EQ(reassemble_all(s, sizeof s - 1), SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    CHECK(g_drv.m.stray_deltas >= 2);
    CHECK_STR(g_drv.echo, "");                /* nothing was printed */

    json_value_t root, c, blk, in;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ((long long)json_count(&c), 1);
    CHECK_EQ(json_at(&c, 0, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "input", &in), JSON_OK);
    CHECK_EQ(in.type, JSON_OBJECT);
    CHECK_EQ((long long)json_count(&in), 0);  /* no fragments -> {} */
}

static void test_malformed_partial_json_becomes_empty_object(void) {
    /* The fragments never close the object. Emitting them raw would poison the
     * next request; the tool must instead see an empty input and say so. */
    static const char s[] =
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"vfs_read\",\"input\":{}}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\": \"}}\n"
"\n"
"data: {\"type\":\"content_block_stop\",\"index\":0}\n"
"\n"
"data: {\"type\":\"message_stop\"}\n"
"\n";
    size_t len = 0;
    CHECK_EQ(reassemble_all(s, sizeof s - 1), SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    json_value_t root, c, blk, in;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ(json_at(&c, 0, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "input", &in), JSON_OK);
    CHECK_EQ(in.type, JSON_OBJECT);
    CHECK_EQ((long long)json_count(&in), 0);
}

static void test_unknown_block_type_is_echoed(void) {
    static const char s[] =
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hmm\"}}\n"
"\n"
"data: {\"type\":\"content_block_stop\",\"index\":0}\n"
"\n"
"data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"answer\"}}\n"
"\n"
"data: {\"type\":\"content_block_stop\",\"index\":1}\n"
"\n"
"data: {\"type\":\"message_stop\"}\n"
"\n";
    size_t len = 0;
    CHECK_EQ(reassemble_all(s, sizeof s - 1), SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    json_value_t root, c, blk, t;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ((long long)json_count(&c), 2);
    CHECK_EQ(json_at(&c, 0, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "type", &t), JSON_OK);
    CHECK(json_str_eq(&t, "thinking"));       /* echoed from its start event */
    char text[64];
    CHECK_EQ(body_text(g_drv.out, len, text, sizeof text), JSON_OK);
    CHECK_STR(text, "answer");
    CHECK_STR(g_drv.echo, "answer");
}

static void test_error_event(void) {
    static const char s[] =
"event: error\n"
"data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n"
"\n";
    size_t len = 0;
    CHECK_EQ(reassemble_all(s, sizeof s - 1), SSE_ESTREAM);
    sse_msg_finish(&g_drv.m, &len);
    CHECK_STR(sse_msg_error(&g_drv.m), "Overloaded");

    /* Even a stream that is nothing but an error yields a parseable body. */
    json_value_t root;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
}

static void test_done_sentinel(void) {
    /* `[DONE]` is not JSON. Recognising it before the parse is the difference
     * between "stream finished" and "malformed event". */
    static const char s[] =
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n"
"\n"
"data: [DONE]\n"
"\n";
    size_t len = 0;
    CHECK_EQ(reassemble_all(s, sizeof s - 1), SSE_OK);
    sse_msg_finish(&g_drv.m, &len);
    CHECK(sse_msg_complete(&g_drv.m));
    CHECK_EQ((long long)g_drv.m.bad_events, 0);
    json_value_t root;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
}

static void test_malformed_event_json_is_counted_not_fatal(void) {
    static const char s[] =
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
"\n"
"data: {not json at all\n"
"\n"
"data: \n"
"\n"
"data: []\n"
"\n"
"data: 12345\n"
"\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"still here\"}}\n"
"\n"
"data: {\"type\":\"message_stop\"}\n"
"\n";
    size_t len = 0;
    CHECK_EQ(reassemble_all(s, sizeof s - 1), SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    char text[128];
    CHECK_EQ(body_text(g_drv.out, len, text, sizeof text), JSON_OK);
    CHECK_STR(text, "still here");            /* recovered and carried on */
    CHECK(g_drv.m.bad_events >= 3);
}

static void test_empty_stream(void) {
    size_t len = 0;
    int    rc  = reassemble(&g_drv, "", 0, 1, 0, &len);
    CHECK_EQ(rc, SSE_OK);

    json_value_t root, c;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ((long long)json_count(&c), 0);
    CHECK(!sse_msg_complete(&g_drv.m));
}

static void test_clipping_still_yields_valid_json(void) {
    /* A long reply into a small buffer. The document must still close cleanly:
     * SSE_TAIL_RESERVE exists exactly so the closers are already paid for. */
    static char s[40000];
    size_t      i = 0;
    const char *head =
"data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"role\":\"assistant\",\"model\":\"x\"}}\n\n"
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
    memcpy(s + i, head, strlen(head)); i += strlen(head);
    for (int k = 0; k < 100; k++) {
        const char *d =
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"0123456789012345678901234567890123456789\"}}\n\n";
        memcpy(s + i, d, strlen(d)); i += strlen(d);
    }
    const char *tail =
"data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
"data: {\"type\":\"message_stop\"}\n\n";
    memcpy(s + i, tail, strlen(tail)); i += strlen(tail);

    for (size_t cap = 110; cap <= 3000; cap += 37) {
        size_t len = 0;
        int    rc  = reassemble(&g_drv, s, i, 1, cap, &len);
        CHECK_EQ(rc, SSE_OK);
        CHECK(len < cap);
        json_value_t root, c;
        CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
        CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
        CHECK(sse_msg_clipped(&g_drv.m));
        /* The console still saw the whole reply — clipping bounds what is
         * remembered, not what is shown. */
        CHECK_EQ((long long)g_drv.echo_len, 4000);
    }

    /* A buffer too small for even the empty document reports ENOSPC rather
     * than emitting half a document. */
    size_t len = 0;
    CHECK_EQ(reassemble(&g_drv, s, i, 1, 64, &len), SSE_ENOSPC);
    CHECK_EQ(reassemble(&g_drv, s, i, 1, 109, &len), SSE_ENOSPC);
}

static void test_oversized_tool_input_is_refused(void) {
    static char s[SSE_TOOL_INPUT_MAX * 3];
    size_t      i = 0;
    const char *head =
"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"n\",\"input\":{}}}\n\n"
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"k\\\":\\\"\"}}\n\n";
    memcpy(s + i, head, strlen(head)); i += strlen(head);
    for (int k = 0; k < 40; k++) {
        const char *d =
"data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}\n\n";
        memcpy(s + i, d, strlen(d)); i += strlen(d);
    }
    const char *tail =
"data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
"data: {\"type\":\"message_stop\"}\n\n";
    memcpy(s + i, tail, strlen(tail)); i += strlen(tail);

    size_t len = 0;
    CHECK_EQ(reassemble_all(s, i), SSE_OK);
    sse_msg_finish(&g_drv.m, &len);

    /* Over the limit: the input becomes {} rather than a clipped, unbalanced
     * fragment that would invalidate the whole conversation. */
    json_value_t root, c, blk, in;
    CHECK_EQ(json_parse(g_drv.out, len, &root), JSON_OK);
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ(json_at(&c, 0, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "input", &in), JSON_OK);
    CHECK_EQ((long long)json_count(&in), 0);
}

static void test_msg_null_safety(void) {
    CHECK_EQ(sse_msg_finish((sse_msg_t *)0, (size_t *)0), SSE_EINVAL);
    CHECK_EQ(sse_msg_complete((const sse_msg_t *)0), 0);
    CHECK_EQ(sse_msg_clipped((const sse_msg_t *)0), 0);
    CHECK_EQ((long long)sse_msg_blocks((const sse_msg_t *)0), 0);
    CHECK_STR(sse_msg_stop_reason((const sse_msg_t *)0), "");
    CHECK_STR(sse_msg_error((const sse_msg_t *)0), "");
    sse_msg_event((sse_msg_t *)0, (const sse_event_t *)0);

    static sse_msg_t m;
    sse_msg_init(&m, (char *)0, 100, (sse_text_fn)0, (void *)0);
    CHECK_EQ(sse_msg_finish(&m, (size_t *)0), SSE_ENOSPC);
}

/* ====================================================================== */
/* layer 3 — the HTTP response driver                                      */
/* ====================================================================== */

typedef struct {
    sse_http_t h;
    char       buf[16384];
    char       echo[4096];
    size_t     echo_len;
} hdrv_t;

static hdrv_t g_h;

static void h_text(void *ctx, const char *t, size_t n) {
    hdrv_t *d = (hdrv_t *)ctx;
    for (size_t i = 0; i < n; i++)
        if (d->echo_len + 1 < sizeof d->echo) d->echo[d->echo_len++] = t[i];
    d->echo[d->echo_len] = '\0';
}

static int http_run(hdrv_t *d, const char *s, size_t n, size_t chunk,
                    model_response_t *out) {
    memset(d->buf, 0, sizeof d->buf);
    d->echo_len = 0;
    d->echo[0]  = '\0';
    sse_http_init(&d->h, d->buf, sizeof d->buf, h_text, d);
    if (chunk == 0) {
        sse_http_feed(&d->h, s, n);
    } else {
        for (size_t i = 0; i < n; i += chunk) {
            size_t k = n - i < chunk ? n - i : chunk;
            sse_http_feed(&d->h, s + i, k);
        }
    }
    return sse_http_finish(&d->h, out);
}

/* Build "<headers>\r\n\r\n<body>" into a static buffer. */
static char *http_wrap(const char *headers, const char *body, size_t *out_n) {
    static char buf[65536];
    size_t      i = 0;
    size_t      hn = strlen(headers), bn = strlen(body);
    memcpy(buf + i, headers, hn); i += hn;
    memcpy(buf + i, "\r\n\r\n", 4); i += 4;
    memcpy(buf + i, body, bn); i += bn;
    buf[i] = '\0';
    *out_n = i;
    return buf;
}

static void test_http_sse_response(void) {
    size_t n = 0;
    char  *raw = http_wrap(
        "HTTP/1.1 200 OK\r\n"
        "content-type: text/event-stream; charset=utf-8\r\n"
        "cache-control: no-cache\r\n"
        "connection: close", STREAM_TEXT, &n);

    /* Every chunking, including one byte at a time, must give the same answer
     * — headers split between the CR and the LF included. */
    static char ref_body[16384];
    static char ref_echo[4096];
    size_t      ref_len = 0;

    for (int c = 0; c < NCHUNKS; c++) {
        model_response_t r;
        int rc = http_run(&g_h, raw, n, CHUNKS[c], &r);
        CHECK_EQ(rc, SSE_OK);
        CHECK_EQ(r.http_status, 200);
        CHECK_STR(r.status_line, "HTTP/1.1 200 OK");
        CHECK_EQ(r.truncated, 0);
        CHECK(sse_http_streamed(&g_h.h));
        CHECK_STR(sse_http_error(&g_h.h), "");

        char text[256];
        CHECK_EQ(body_text(r.body, r.body_len, text, sizeof text), JSON_OK);
        CHECK_STR(text, "fable-os online");
        CHECK_STR(g_h.echo, "fable-os online");

        if (c == 0) {
            memcpy(ref_body, r.body, r.body_len + 1);
            memcpy(ref_echo, g_h.echo, g_h.echo_len + 1);
            ref_len = r.body_len;
        } else {
            CHECK_EQ((long long)r.body_len, (long long)ref_len);
            CHECK_STR(r.body, ref_body);
            CHECK_STR(g_h.echo, ref_echo);
        }
    }
}

static void test_http_non_sse_is_buffered_verbatim(void) {
    /* The 401 every keyless build gets. This is the path the DEFAULT build
     * takes too, and it must be untouched by streaming. */
    const char *body =
        "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
        "\"message\":\"invalid x-api-key\"}}";
    size_t n = 0;
    char  *raw = http_wrap(
        "HTTP/1.1 401 Unauthorized\r\n"
        "content-type: application/json\r\n"
        "connection: close", body, &n);

    for (int c = 0; c < NCHUNKS; c++) {
        model_response_t r;
        CHECK_EQ(http_run(&g_h, raw, n, CHUNKS[c], &r), SSE_OK);
        CHECK_EQ(r.http_status, 401);
        CHECK_STR(r.status_line, "HTTP/1.1 401 Unauthorized");
        CHECK(!sse_http_streamed(&g_h.h));
        CHECK_EQ((long long)r.body_len, (long long)strlen(body));
        CHECK_STR(r.body, body);
        CHECK_EQ(r.truncated, 0);
        CHECK_STR(g_h.echo, "");             /* nothing was printed */
    }
}

static void test_http_200_but_not_a_stream(void) {
    /* A 200 whose body is an ordinary Messages response — a proxy that ignored
     * "stream":true. Sniffing the content-type is what keeps this readable
     * instead of being fed to the event parser. */
    const char *body =
        "{\"id\":\"m\",\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"text\",\"text\":\"buffered\"}]}";
    size_t n = 0;
    char  *raw = http_wrap("HTTP/1.1 200 OK\r\ncontent-type: application/json",
                           body, &n);

    model_response_t r;
    CHECK_EQ(http_run(&g_h, raw, n, 1, &r), SSE_OK);
    CHECK(!sse_http_streamed(&g_h.h));
    CHECK_STR(r.body, body);

    char text[64];
    CHECK_EQ(body_text(r.body, r.body_len, text, sizeof text), JSON_OK);
    CHECK_STR(text, "buffered");
}

static void test_http_content_type_variations(void) {
    struct { const char *ct; int stream; } cases[] = {
        { "content-type: text/event-stream",                    1 },
        { "Content-Type: text/event-stream",                    1 },
        { "CONTENT-TYPE: TEXT/EVENT-STREAM",                    1 },
        { "content-type:text/event-stream;charset=utf-8",       1 },
        { "content-type: application/json",                     0 },
        { "content-type: text/plain",                           0 },
        { "x-note: text/event-stream\r\ncontent-type: text/html",0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char hdr[256];
        snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\n%s", cases[i].ct);
        size_t n = 0;
        char  *raw = http_wrap(hdr, "data: {\"type\":\"message_stop\"}\n\n", &n);
        model_response_t r;
        http_run(&g_h, raw, n, 1, &r);
        CHECK_EQ(sse_http_streamed(&g_h.h), cases[i].stream);
    }
}

/* ---------------------------------------------------------------------- */
/* regressions: four ways the transport used to hand up something the model
 * did not send. Each of these was a live defect, reproduced before it was
 * fixed; they are here so it cannot come back quietly.                     */
/* ---------------------------------------------------------------------- */

/* stop_reason used to be json_str()-DECODED into a buffer and then emitted RAW
 * between two quotes — the one model-controlled string in sse.c that was not
 * carried as its raw span. A `"` in it broke the document; a crafted value
 * spliced extra members into the top-level object that chat.c echoes back. */
static void test_stop_reason_cannot_break_the_document(void) {
    static const char *hostile[] = {
        "end\\\"turn",                       /* a bare quote                  */
        "tool_use\\\\",                      /* trailing backslash            */
        "a\\nb",                             /* escaped control char          */
        "x\\\",\\\"usage\\\":\\\"OWNED",     /* structural injection attempt   */
        "\\u0000pad",                        /* escaped NUL                   */
    };
    for (size_t i = 0; i < sizeof hostile / sizeof hostile[0]; i++) {
        char body[512];
        snprintf(body, sizeof body,
                 "data: {\"type\":\"content_block_start\",\"index\":0,"
                 "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                 "data: {\"type\":\"content_block_delta\",\"index\":0,"
                 "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
                 "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                 "data: {\"type\":\"message_delta\","
                 "\"delta\":{\"stop_reason\":\"%s\"}}\n\n"
                 "data: {\"type\":\"message_stop\"}\n\n", hostile[i]);

        size_t n = 0;
        char  *raw = http_wrap("HTTP/1.1 200 OK\r\n"
                               "content-type: text/event-stream", body, &n);
        model_response_t r;
        CHECK_EQ(http_run(&g_h, raw, n, 1, &r), SSE_OK);

        /* THE invariant: still valid JSON, whatever the server said. */
        json_value_t root, junk;
        CHECK_EQ(json_parse(r.body, r.body_len, &root), JSON_OK);

        /* And the content the model really sent is intact... */
        char text[64];
        CHECK_EQ(body_text(r.body, r.body_len, text, sizeof text), JSON_OK);
        CHECK_STR(text, "hi");

        /* ...with no member the kernel never wrote. */
        CHECK(json_get(&root, "usage", &junk) != JSON_OK);
    }
}

/* The content-type sniff used to be an unanchored substring search over the
 * whole header block, so a header VALUE (or the status line's reason phrase)
 * decided which path a response took — in both directions. */
static void test_sniff_is_anchored_to_a_header_line(void) {
    struct { const char *hdrs; const char *body; int stream; } cases[] = {
        /* a value that names the stream type must NOT enable streaming: the
         * real body is JSON and the SSE path never buffers raw bytes, so this
         * used to destroy the response outright */
        { "HTTP/1.1 200 OK\r\n"
          "x-note: rejected content-type: text/event-stream was requested\r\n"
          "content-type: application/json", "{\"real\":\"json body\"}", 0 },
        /* a value containing the field name must not hide the real header */
        { "HTTP/1.1 200 OK\r\n"
          "x-note: content-type: application/json\r\n"
          "content-type: text/event-stream",
          "data: {\"type\":\"message_stop\"}\n\n", 1 },
        /* nor may the status line's reason phrase decide it */
        { "HTTP/1.1 200 content-type: text/event-stream\r\n"
          "content-type: application/json", "{\"real\":\"json\"}", 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t n = 0;
        char  *raw = http_wrap(cases[i].hdrs, cases[i].body, &n);
        model_response_t r;
        http_run(&g_h, raw, n, 1, &r);
        CHECK_EQ(sse_http_streamed(&g_h.h), cases[i].stream);
        /* a non-stream body must arrive byte-for-byte */
        if (!cases[i].stream) CHECK_STR(r.body, cases[i].body);
    }
}

/* The status code was accumulated with an unbounded `code = code*10 + d`: signed
 * overflow on network input, and a long enough digit run wrapped to a value that
 * satisfied the `status == 200` half of the sniff. */
static void test_status_code_cannot_wrap_into_200(void) {
    struct { const char *line; int status; int stream; } cases[] = {
        { "HTTP/1.1 200 OK",              200, 1 },
        { "HTTP/1.1 401 Unauthorized",    401, 0 },
        { "HTTP/1.1 4294967496 Nope",       0, 0 },  /* wraps to 200 in int    */
        { "HTTP/1.1 2000 Nope",             0, 0 },  /* four digits            */
        { "HTTP/1.1 20 Nope",               0, 0 },  /* two digits             */
        { "HTTP/1.1 99999999999999999 No",  0, 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char hdrs[128];
        snprintf(hdrs, sizeof hdrs, "%s\r\ncontent-type: text/event-stream",
                 cases[i].line);
        size_t n = 0;
        char  *raw = http_wrap(hdrs, "data: {\"type\":\"message_stop\"}\n\n", &n);
        model_response_t r;
        http_run(&g_h, raw, n, 1, &r);
        CHECK_EQ(r.http_status, cases[i].status);
        CHECK_EQ(sse_http_streamed(&g_h.h), cases[i].stream);
    }
}

/* The framing layer drops an over-long line and counts it, but nothing above it
 * ever read those counters — so a drop BETWEEN two input_json_delta fragments
 * spliced the surviving prefix and suffix into a tool input that was still
 * well-formed JSON, said something the model never said, and came back
 * rc=SSE_OK truncated=0. chat.c would have dispatched it. */
static void test_a_dropped_event_is_reported_as_truncated(void) {
    static char body[16384];
    static char big[SSE_LINE_MAX + 128];
    memset(big, 'X', sizeof big - 1);
    big[sizeof big - 1] = '\0';

    snprintf(body, sizeof body,
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"write_file\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
        "{\"type\":\"input_json_delta\",\"partial_json\":"
        "\"{\\\"path\\\":\\\"/etc/motd\\\",\\\"content\\\":\\\"KEEP\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
        "{\"type\":\"input_json_delta\",\"partial_json\":\"%s\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
        "{\"type\":\"input_json_delta\",\"partial_json\":\"TAIL\\\"}\"}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n", big);

    size_t n = 0;
    char  *raw = http_wrap("HTTP/1.1 200 OK\r\n"
                           "content-type: text/event-stream", body, &n);
    model_response_t r;
    CHECK_EQ(http_run(&g_h, raw, n, 1, &r), SSE_OK);

    /* The framing layer noticed... */
    CHECK(sse_parser_dropped(&g_h.h.p) > 0 || sse_parser_bad_lines(&g_h.h.p) > 0);
    /* ...and, unlike before, so does the caller. This is the assertion that
     * stops chat.c acting on spliced arguments (net/chat.c aborts on
     * truncated), and the document is still valid JSON either way. */
    CHECK_EQ(r.truncated, 1);
    json_value_t root;
    CHECK_EQ(json_parse(r.body, r.body_len, &root), JSON_OK);

    /* A stream that loses nothing must still report truncated=0, or the flag
     * means nothing. */
    size_t n2 = 0;
    char  *ok = http_wrap("HTTP/1.1 200 OK\r\n"
                          "content-type: text/event-stream", STREAM_TOOL, &n2);
    model_response_t r2;
    CHECK_EQ(http_run(&g_h, ok, n2, 1, &r2), SSE_OK);
    CHECK_EQ(r2.truncated, 0);
    CHECK_EQ(sse_msg_lost(&g_h.h.m), 0);
}

static void test_http_malformed(void) {
    model_response_t r;

    /* Headers that never end: no body was ever reached. */
    CHECK_EQ(http_run(&g_h, "HTTP/1.1 200 OK\r\ncontent-type: x", 32, 1, &r),
             SSE_EPROTO);
    CHECK_EQ(http_run(&g_h, "", 0, 1, &r), SSE_EPROTO);

    /* No status code at all. */
    size_t n = 0;
    char  *raw = http_wrap("garbage", "body", &n);
    CHECK_EQ(http_run(&g_h, raw, n, 1, &r), SSE_OK);
    CHECK_EQ(r.http_status, 0);
    CHECK_STR(r.body, "body");

    /* Bare LFLF instead of CRLFCRLF. */
    static const char lf[] = "HTTP/1.1 204 No Content\ncontent-type: x\n\nbody";
    CHECK_EQ(http_run(&g_h, lf, sizeof lf - 1, 1, &r), SSE_OK);
    CHECK_EQ(r.http_status, 204);
    CHECK_STR(r.body, "body");

    CHECK_EQ(sse_http_finish((sse_http_t *)0, &r), SSE_EINVAL);
    CHECK_EQ(sse_http_finish(&g_h.h, (model_response_t *)0), SSE_EINVAL);
    CHECK_EQ(sse_http_streamed((const sse_http_t *)0), 0);
    CHECK_STR(sse_http_error((const sse_http_t *)0), "");
    sse_http_feed((sse_http_t *)0, "x", 1);
    sse_http_init((sse_http_t *)0, (char *)0, 0, (sse_text_fn)0, (void *)0);
}

static void test_http_stream_error_surfaces(void) {
    size_t n = 0;
    char  *raw = http_wrap("HTTP/1.1 200 OK\r\ncontent-type: text/event-stream",
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n"
        "\n", &n);
    model_response_t r;
    CHECK_EQ(http_run(&g_h, raw, n, 1, &r), SSE_ESTREAM);
    CHECK_STR(sse_http_error(&g_h.h), "Overloaded");
    CHECK_EQ(r.http_status, 200);
}

static void test_http_raw_body_overflow(void) {
    /* The non-stream path with a body bigger than the response buffer: same
     * `truncated` contract the buffered transport has always had. */
    static hdrv_t small;
    static char   body[8192];
    for (size_t i = 0; i < sizeof body - 1; i++) body[i] = 'x';
    body[sizeof body - 1] = '\0';

    size_t n = 0;
    char  *raw = http_wrap("HTTP/1.1 200 OK\r\ncontent-type: application/json",
                           body, &n);

    memset(small.buf, 0, sizeof small.buf);
    small.echo_len = 0;
    sse_http_init(&small.h, small.buf, 256, h_text, &small);
    for (size_t i = 0; i < n; i++) sse_http_feed(&small.h, raw + i, 1);

    model_response_t r;
    CHECK_EQ(sse_http_finish(&small.h, &r), SSE_OK);
    CHECK_EQ(r.truncated, 1);
    CHECK_EQ((long long)r.body_len, 255);
}

static void test_http_sse_truncated_connection(void) {
    /* The connection dies halfway through the stream at every possible point.
     * Each one must still yield a valid body and an honest `truncated`. */
    size_t n = 0;
    char  *raw = http_wrap("HTTP/1.1 200 OK\r\ncontent-type: text/event-stream",
                           STREAM_TEXT, &n);
    for (size_t cut = 1; cut <= n; cut += 11) {
        model_response_t r;
        int rc = http_run(&g_h, raw, cut, 1, &r);
        CHECK(rc == SSE_OK || rc == SSE_EPROTO);
        if (rc != SSE_OK) continue;
        json_value_t v;
        CHECK_EQ(json_parse(r.body, r.body_len, &v), JSON_OK);
        /* Truncated unless the cut happened to fall after message_stop — the
         * last few bytes are only the blank line that terminates it. */
        CHECK(r.truncated == 1 || sse_msg_complete(&g_h.h.m));
    }
}

/* ====================================================================== */
/* adversarial fuzzing                                                     */
/* ====================================================================== */

static uint32_t rng;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

/* Bytes that make an SSE-shaped mess more often than pure noise would. */
static const char SOUP[] = "dateven:{}\"\\ \r\n0189[],pinghmsg_lostrmx\xEF\xBB\xBF";

static void fuzz_one(const char *s, size_t n) {
    size_t chunk = (rnd() % 5 == 0) ? 0 : (rnd() % 37) + 1;
    size_t len   = 0;
    int    rc    = reassemble(&g_drv, s, n, chunk, 0, &len);

    /* The three things that must hold for ANY byte string on the wire. */
    CHECK(rc == SSE_OK || rc == SSE_ESTREAM);          /* never EPROTO/ENOSPC */
    json_value_t v;
    CHECK_EQ(json_parse(g_drv.out, len, &v), JSON_OK); /* always valid JSON   */
    CHECK(canaries_ok(&g_drv));                        /* nothing wrote out   */
}

static void test_fuzz_random_bytes(void) {
    static char s[2048];
    rng = 0x5EED1234u;
    for (int it = 0; it < 4000; it++) {
        size_t n = rnd() % sizeof s;
        for (size_t i = 0; i < n; i++) {
            uint32_t r = rnd();
            s[i] = (r % 4) ? SOUP[r % (sizeof SOUP - 1)] : (char)(r >> 3);
        }
        fuzz_one(s, n);
    }
}

static void test_fuzz_mutated_streams(void) {
    static char s[sizeof STREAM_TOOL + 64];
    const char *seeds[2] = { STREAM_TEXT, STREAM_TOOL };
    size_t      seedn[2] = { sizeof STREAM_TEXT - 1, sizeof STREAM_TOOL - 1 };

    rng = 0xC0FFEE01u;
    for (int it = 0; it < 6000; it++) {
        int    k = it & 1;
        size_t n = seedn[k];
        memcpy(s, seeds[k], n);

        int muts = 1 + (int)(rnd() % 6);
        for (int m = 0; m < muts && n > 1; m++) {
            size_t at = rnd() % n;
            switch (rnd() % 4) {
                case 0: s[at] = SOUP[rnd() % (sizeof SOUP - 1)]; break;  /* flip */
                case 1: s[at] = (char)(rnd() >> 5);              break;  /* noise */
                case 2: memmove(s + at, s + at + 1, n - at - 1); n--; break;
                default: n = at; break;                                  /* cut */
            }
        }
        fuzz_one(s, n);
    }
}

static void test_fuzz_split_points(void) {
    /* Not random: EVERY single split point of a real stream, twice over — the
     * exhaustive form of "events arrive at arbitrary byte boundaries". */
    static char two[sizeof STREAM_TOOL];
    size_t      n = sizeof STREAM_TOOL - 1;
    memcpy(two, STREAM_TOOL, n);

    size_t ref_len = 0;
    reassemble(&g_drv, two, n, 1, 0, &ref_len);
    static char ref[sizeof g_drv.out];
    memcpy(ref, g_drv.out, sizeof ref);

    for (size_t split = 0; split <= n; split++) {
        drv_begin(&g_drv, 0);
        sse_parser_feed(&g_drv.p, two, split);
        sse_parser_feed(&g_drv.p, two + split, n - split);
        sse_parser_finish(&g_drv.p);
        size_t len = 0;
        CHECK_EQ(sse_msg_finish(&g_drv.m, &len), SSE_OK);
        CHECK_EQ((long long)len, (long long)ref_len);
        CHECK_STR(g_drv.out, ref);
    }
}

static void test_fuzz_http_layer(void) {
    static char s[3072];
    rng = 0xA5A5F00Du;
    for (int it = 0; it < 3000; it++) {
        size_t n = rnd() % sizeof s;
        for (size_t i = 0; i < n; i++) {
            uint32_t r = rnd();
            s[i] = (r % 3) ? SOUP[r % (sizeof SOUP - 1)] : (char)(r >> 3);
        }
        /* Half the time, give it a plausible header block to get past. */
        if (it & 1) {
            const char *h = "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\n\r\n";
            size_t hn = strlen(h);
            if (n > hn) { memcpy(s, h, hn); }
        }
        model_response_t r;
        size_t chunk = (rnd() % 4 == 0) ? 0 : (rnd() % 29) + 1;
        int    rc    = http_run(&g_h, s, n, chunk, &r);
        CHECK(rc == SSE_OK || rc == SSE_EPROTO || rc == SSE_ESTREAM);
        if (rc == SSE_OK && sse_http_streamed(&g_h.h)) {
            json_value_t v;
            CHECK_EQ(json_parse(r.body, r.body_len, &v), JSON_OK);
        }
    }
}

/* ====================================================================== */

int main(void) {
    console_init();

    /* layer 1 — framing */
    RUN(test_basic_framing);
    RUN(test_default_event_name);
    RUN(test_line_endings);
    RUN(test_optional_leading_space);
    RUN(test_multiple_data_lines);
    RUN(test_comments_and_keepalives);
    RUN(test_event_without_data_is_not_dispatched);
    RUN(test_bare_and_empty_fields);
    RUN(test_unknown_fields_ignored);
    RUN(test_id_is_sticky);
    RUN(test_bom);
    RUN(test_embedded_nul_in_data);
    RUN(test_unterminated_final_event);
    RUN(test_overlong_line_is_dropped);
    RUN(test_enormous_event_is_dropped);
    RUN(test_overlong_event_name_drops_event);
    RUN(test_blank_lines_everywhere);
    RUN(test_no_callback_still_counts);

    /* layer 2 — reassembly */
    RUN(test_reassemble_text_turn);
    RUN(test_reassemble_tool_use);
    RUN(test_escapes_survive_verbatim);
    RUN(test_missing_message_stop_is_truncated);
    RUN(test_missing_content_block_stop);
    RUN(test_stray_and_mismatched_deltas);
    RUN(test_malformed_partial_json_becomes_empty_object);
    RUN(test_unknown_block_type_is_echoed);
    RUN(test_error_event);
    RUN(test_done_sentinel);
    RUN(test_malformed_event_json_is_counted_not_fatal);
    RUN(test_empty_stream);
    RUN(test_clipping_still_yields_valid_json);
    RUN(test_oversized_tool_input_is_refused);
    RUN(test_msg_null_safety);

    /* layer 3 — the HTTP driver */
    RUN(test_http_sse_response);
    RUN(test_http_non_sse_is_buffered_verbatim);
    RUN(test_http_200_but_not_a_stream);
    RUN(test_http_content_type_variations);
    RUN(test_http_malformed);
    RUN(test_http_stream_error_surfaces);
    RUN(test_http_raw_body_overflow);
    RUN(test_http_sse_truncated_connection);

    /* regressions for defects found by audit */
    RUN(test_stop_reason_cannot_break_the_document);
    RUN(test_sniff_is_anchored_to_a_header_line);
    RUN(test_status_code_cannot_wrap_into_200);
    RUN(test_a_dropped_event_is_reported_as_truncated);

    /* adversarial */
    RUN(test_fuzz_random_bytes);
    RUN(test_fuzz_mutated_streams);
    RUN(test_fuzz_split_points);
    RUN(test_fuzz_http_layer);

    return th_report("sse");
}
