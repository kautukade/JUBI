/* test_json.c — adversarial tests for net/json.c, the parser that faces the
 * network. Everything the model sends the kernel goes through this code, so the
 * bar is: malformed, truncated, over-nested, over-long, and hostile input must
 * all fail cleanly, never crash, and never write outside the buffer given.
 *
 * Buffers under test are wrapped in canaries so an off-by-one is a visible test
 * failure rather than a silent corruption.
 */

#include "harness.h"
#include "json.h"

#include <stdlib.h>

/* ---- a canary-guarded destination buffer ---- */
typedef struct {
    char pre[8];
    char buf[256];
    char post[8];
} guarded_t;

static void guard_init(guarded_t *g) {
    memset(g, 0x7E, sizeof *g);
    memset(g->buf, 0, sizeof g->buf);
}
static int guard_ok(const guarded_t *g) {
    for (size_t i = 0; i < sizeof g->pre; i++)  if ((unsigned char)g->pre[i]  != 0x7E) return 0;
    for (size_t i = 0; i < sizeof g->post; i++) if ((unsigned char)g->post[i] != 0x7E) return 0;
    return 1;
}

/* A realistic Messages API success response, with two text blocks and a
 * tool_use block carrying an arbitrary nested input object. */
static const char *SAMPLE =
    "{\"id\":\"msg_01ABC\",\"type\":\"message\",\"role\":\"assistant\","
    "\"model\":\"claude-opus-4-8\","
    "\"content\":["
      "{\"type\":\"text\",\"text\":\"fable-os online\"},"
      "{\"type\":\"tool_use\",\"id\":\"toolu_01\",\"name\":\"vfs_read\","
       "\"input\":{\"path\":\"/etc/motd\",\"max\":128,\"flags\":{\"raw\":true}}},"
      "{\"type\":\"text\",\"text\":\" and ready\"}"
    "],"
    "\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
    "\"usage\":{\"input_tokens\":42,\"output_tokens\":7}}";

/* ====================================================================== */
/* basics                                                                  */
/* ====================================================================== */

static void test_scalars(void) {
    json_value_t v;
    int64_t i;
    int b;

    CHECK_EQ(json_parse_str("42", &v), JSON_OK);
    CHECK_EQ(v.type, JSON_NUMBER);
    CHECK_EQ(json_int(&v, &i), JSON_OK);
    CHECK_EQ(i, 42);

    CHECK_EQ(json_parse_str("-1234567890123", &v), JSON_OK);
    CHECK_EQ(json_int(&v, &i), JSON_OK);
    CHECK_EQ(i, -1234567890123LL);

    /* fractions truncate toward zero, exponents are refused (documented) */
    CHECK_EQ(json_parse_str("3.99", &v), JSON_OK);
    CHECK_EQ(json_int(&v, &i), JSON_OK);
    CHECK_EQ(i, 3);
    CHECK_EQ(json_parse_str("1e3", &v), JSON_OK);
    CHECK_EQ(json_int(&v, &i), JSON_EINVAL);

    /* overflow must not wrap */
    CHECK_EQ(json_parse_str("99999999999999999999999", &v), JSON_OK);
    CHECK_EQ(json_int(&v, &i), JSON_EINVAL);

    CHECK_EQ(json_parse_str("true", &v), JSON_OK);
    CHECK_EQ(v.type, JSON_BOOL);
    CHECK_EQ(json_bool(&v, &b), JSON_OK);
    CHECK_EQ(b, 1);

    CHECK_EQ(json_parse_str("false", &v), JSON_OK);
    CHECK_EQ(json_bool(&v, &b), JSON_OK);
    CHECK_EQ(b, 0);

    CHECK_EQ(json_parse_str("null", &v), JSON_OK);
    CHECK_EQ(v.type, JSON_NULL);
    CHECK_EQ(json_bool(&v, &b), JSON_ETYPE);

    /* whitespace around the document is fine; trailing junk is not */
    CHECK_EQ(json_parse_str("  \r\n\t {}  \n", &v), JSON_OK);
    CHECK_EQ(json_parse_str("{} x", &v), JSON_EINVAL);
    CHECK_EQ(json_parse_str("{}{}", &v), JSON_EINVAL);
    CHECK_EQ(json_parse_str("", &v), JSON_EINVAL);
    CHECK_EQ(json_parse_str("   ", &v), JSON_EINVAL);
}

static void test_nested_navigation(void) {
    json_value_t root, content, blk, in, tmp;
    char sbuf[128];

    CHECK_EQ(json_parse_str(SAMPLE, &root), JSON_OK);
    CHECK_EQ(root.type, JSON_OBJECT);

    CHECK_EQ(json_get_str(&root, "type", sbuf, sizeof sbuf), JSON_OK);
    CHECK_STR(sbuf, "message");

    CHECK_EQ(json_msg_content(&root, &content), JSON_OK);
    CHECK_EQ(json_count(&content), 3);

    /* walk content[] generically, exactly as the tool-use loop will */
    CHECK_EQ(json_at(&content, 1, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "type", &tmp), JSON_OK);
    CHECK(json_str_eq(&tmp, "tool_use"));
    CHECK(!json_str_eq(&tmp, "text"));
    CHECK_EQ(json_get_str(&blk, "id", sbuf, sizeof sbuf), JSON_OK);
    CHECK_STR(sbuf, "toolu_01");
    CHECK_EQ(json_get_str(&blk, "name", sbuf, sizeof sbuf), JSON_OK);
    CHECK_STR(sbuf, "vfs_read");

    /* the arbitrary input object: readable member by member ... */
    CHECK_EQ(json_get(&blk, "input", &in), JSON_OK);
    CHECK_EQ(in.type, JSON_OBJECT);
    CHECK_EQ(json_count(&in), 3);
    CHECK_EQ(json_get_str(&in, "path", sbuf, sizeof sbuf), JSON_OK);
    CHECK_STR(sbuf, "/etc/motd");
    int64_t max = 0;
    CHECK_EQ(json_get(&in, "max", &tmp), JSON_OK);
    CHECK_EQ(json_int(&tmp, &max), JSON_OK);
    CHECK_EQ(max, 128);

    /* ... and available verbatim as a raw span, for echoing back */
    CHECK_EQ(in.len, strlen("{\"path\":\"/etc/motd\",\"max\":128,\"flags\":{\"raw\":true}}"));
    CHECK_EQ(memcmp(in.start, "{\"path\"", 7), 0);

    /* member-by-index walking of an unknown object */
    json_value_t k, val;
    CHECK_EQ(json_member(&in, 2, &k, &val), JSON_OK);
    CHECK(json_str_eq(&k, "flags"));
    CHECK_EQ(val.type, JSON_OBJECT);
    int raw = 0;
    CHECK_EQ(json_get(&val, "raw", &tmp), JSON_OK);
    CHECK_EQ(json_bool(&tmp, &raw), JSON_OK);
    CHECK_EQ(raw, 1);

    /* nested three deep through the usage object */
    CHECK_EQ(json_get(&root, "usage", &tmp), JSON_OK);
    json_value_t tok;
    CHECK_EQ(json_get(&tmp, "output_tokens", &tok), JSON_OK);
    int64_t n = 0;
    CHECK_EQ(json_int(&tok, &n), JSON_OK);
    CHECK_EQ(n, 7);
}

static void test_missing_and_wrong_types(void) {
    json_value_t root, v;
    char sbuf[32];

    CHECK_EQ(json_parse_str(SAMPLE, &root), JSON_OK);

    CHECK_EQ(json_get(&root, "nope", &v), JSON_ENOENT);
    CHECK_EQ(v.type, JSON_INVALID);
    CHECK_EQ(json_get_str(&root, "nope", sbuf, sizeof sbuf), JSON_ENOENT);
    CHECK_STR(sbuf, "");

    /* prefix of a real key must not match */
    CHECK_EQ(json_get(&root, "conten", &v), JSON_ENOENT);
    CHECK_EQ(json_get(&root, "contents", &v), JSON_ENOENT);
    CHECK_EQ(json_get(&root, "", &v), JSON_ENOENT);

    /* type mismatches are reported, not guessed */
    CHECK_EQ(json_get(&root, "content", &v), JSON_OK);
    CHECK_EQ(json_get(&v, "type", &v), JSON_ETYPE);   /* array, not object */

    json_value_t arr;
    CHECK_EQ(json_msg_content(&root, &arr), JSON_OK);
    CHECK_EQ(json_at(&arr, 3, &v), JSON_ENOENT);      /* one past the end */
    CHECK_EQ(json_at(&arr, 100000, &v), JSON_ENOENT);
    CHECK_EQ(json_at(&root, 0, &v), JSON_ETYPE);      /* object, not array */

    CHECK_EQ(json_str(&root, sbuf, sizeof sbuf, NULL), JSON_ETYPE);
    CHECK_EQ(json_count(&root), 8);

    /* NULL-safety on every public entry point */
    CHECK_EQ(json_parse(NULL, 10, &v), JSON_EINVAL);
    CHECK_EQ(json_parse_str("{}", NULL), JSON_EINVAL);
    CHECK_EQ(json_get(NULL, "a", &v), JSON_EINVAL);
    CHECK_EQ(json_get(&root, NULL, &v), JSON_EINVAL);
    CHECK_EQ(json_at(NULL, 0, &v), JSON_EINVAL);
    CHECK_EQ(json_int(NULL, NULL), JSON_EINVAL);
    CHECK_EQ(json_str(&root, NULL, 0, NULL), JSON_EINVAL);
    CHECK_EQ(json_str_eq(NULL, "x"), 0);
    CHECK_EQ(json_str_eq(&root, NULL), 0);
    CHECK_EQ(json_count(NULL), 0);
}

/* ====================================================================== */
/* string decoding                                                         */
/* ====================================================================== */

static void test_escapes(void) {
    json_value_t v;
    char buf[128];
    size_t n = 0;

    CHECK_EQ(json_parse_str("\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_STR(buf, "a\"b\\c/d\be\ff\ng\rh\ti");
    CHECK_EQ(n, strlen("a\"b\\c/d\be\ff\ng\rh\ti"));

    /* the empty string */
    CHECK_EQ(json_parse_str("\"\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 0);
    CHECK_STR(buf, "");

    /* json_str_eq walks escapes too */
    CHECK_EQ(json_parse_str("\"line\\none\"", &v), JSON_OK);
    CHECK(json_str_eq(&v, "line\none"));
    CHECK(!json_str_eq(&v, "line"));           /* prefix must not match */
    CHECK(!json_str_eq(&v, "line\nonex"));     /* extension must not match */
    CHECK(!json_str_eq(&v, ""));

    /* an embedded NUL via \u0000 truncates the C string but reports the true
     * length, so a caller that cares can still see it */
    CHECK_EQ(json_parse_str("\"a\\u0000b\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 3);
    CHECK_EQ(buf[0], 'a');
    CHECK_EQ(buf[1], '\0');
    CHECK_EQ(buf[2], 'b');
}

static void test_unicode_escapes(void) {
    json_value_t v;
    char buf[64];
    size_t n = 0;

    /* ASCII */
    CHECK_EQ(json_parse_str("\"\\u0041\\u007a\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_STR(buf, "Az");

    /* 2-byte UTF-8: U+00E9 e-acute */
    CHECK_EQ(json_parse_str("\"caf\\u00e9\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 5);
    CHECK_EQ((unsigned char)buf[3], 0xC3);
    CHECK_EQ((unsigned char)buf[4], 0xA9);

    /* 3-byte UTF-8: U+20AC euro */
    CHECK_EQ(json_parse_str("\"\\u20AC\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 3);
    CHECK_EQ((unsigned char)buf[0], 0xE2);
    CHECK_EQ((unsigned char)buf[1], 0x82);
    CHECK_EQ((unsigned char)buf[2], 0xAC);

    /* surrogate pair: U+1F600 grinning face -> 4 bytes */
    CHECK_EQ(json_parse_str("\"\\ud83d\\ude00\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 4);
    CHECK_EQ((unsigned char)buf[0], 0xF0);
    CHECK_EQ((unsigned char)buf[1], 0x9F);
    CHECK_EQ((unsigned char)buf[2], 0x98);
    CHECK_EQ((unsigned char)buf[3], 0x80);

    /* unpaired high surrogate -> U+FFFD, not a read past the end */
    CHECK_EQ(json_parse_str("\"\\ud83dX\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 4);
    CHECK_EQ((unsigned char)buf[0], 0xEF);
    CHECK_EQ((unsigned char)buf[1], 0xBF);
    CHECK_EQ((unsigned char)buf[2], 0xBD);
    CHECK_EQ(buf[3], 'X');

    /* high surrogate at the very end of the string */
    CHECK_EQ(json_parse_str("\"\\ud83d\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 3);

    /* stray low surrogate */
    CHECK_EQ(json_parse_str("\"\\udc00\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 3);
    CHECK_EQ((unsigned char)buf[0], 0xEF);

    /* high surrogate followed by a non-surrogate \u escape */
    CHECK_EQ(json_parse_str("\"\\ud83d\\u0041\"", &v), JSON_OK);
    CHECK_EQ(json_str(&v, buf, sizeof buf, &n), JSON_OK);
    CHECK_EQ(n, 4);
    CHECK_EQ(buf[3], 'A');

    /* bad hex is a parse error, not a decode surprise */
    CHECK_EQ(json_parse_str("\"\\u12g4\"", &v), JSON_EINVAL);
    CHECK_EQ(json_parse_str("\"\\u12\"", &v), JSON_EINVAL);
    CHECK_EQ(json_parse_str("\"\\u\"", &v), JSON_EINVAL);
}

static void test_decode_bounds(void) {
    json_value_t v;
    guarded_t g;

    CHECK_EQ(json_parse_str("\"0123456789abcdef\"", &v), JSON_OK);

    /* every capacity from 1 up must either fit or report ENOSPC, always
     * NUL-terminated, never touching the canaries */
    for (size_t cap = 1; cap <= 20; cap++) {
        guard_init(&g);
        size_t n = 0;
        int rc = json_str(&v, g.buf, cap, &n);
        CHECK(guard_ok(&g));
        CHECK(n < cap);
        CHECK_EQ(g.buf[n], '\0');
        CHECK_EQ(rc, cap > 16 ? JSON_OK : JSON_ENOSPC);
        if (rc == JSON_OK) CHECK_STR(g.buf, "0123456789abcdef");
    }

    /* a multi-byte character must not be half-written at the boundary */
    CHECK_EQ(json_parse_str("\"\\u20AC\\u20AC\"", &v), JSON_OK);
    for (size_t cap = 1; cap <= 8; cap++) {
        guard_init(&g);
        size_t n = 0;
        json_str(&v, g.buf, cap, &n);
        CHECK(guard_ok(&g));
        CHECK_EQ(n % 3, 0);            /* only whole code points written */
        CHECK_EQ(g.buf[n], '\0');
    }

    /* json_get_str with a tiny buffer */
    json_value_t root;
    CHECK_EQ(json_parse_str(SAMPLE, &root), JSON_OK);
    guard_init(&g);
    CHECK_EQ(json_get_str(&root, "model", g.buf, 4), JSON_ENOSPC);
    CHECK(guard_ok(&g));
    CHECK_STR(g.buf, "cla");
}

/* ====================================================================== */
/* hostile input                                                           */
/* ====================================================================== */

static void test_malformed(void) {
    static const char *bad[] = {
        "{",  "}",  "[",  "]",  ",",  ":",  "\"",
        "{\"a\"}", "{\"a\":}", "{\"a\" 1}", "{a:1}", "{'a':1}",
        "{\"a\":1,}", "[1,]", "[,1]", "[1 2]", "{\"a\":1 \"b\":2}",
        "{\"a\":1}}", "[[]", "[]]",
        "tru", "truex", "nul", "nulll", "fals",
        "01", "-", "-.", ".5", "+1", "1.", "1.e5", "1e", "1e+", "0x10",
        "\"unterminated", "\"bad\\escape\"", "\"\\x41\"",
        "\"raw\ncontrol\"", "\"raw\tcontrol\"",
        "{\"k\":\"v\"", "[{\"k\":\"v\"}", "{\"\\u00\":1}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        json_value_t v;
        int rc = json_parse_str(bad[i], &v);
        CHECK(rc != JSON_OK);
        CHECK_EQ(v.type, JSON_INVALID);
        if (rc == JSON_OK) printf("      (accepted: %s)\n", bad[i]);
    }
}

/* Every prefix of a valid document must be rejected, and none may read past the
 * end. The buffer is exact-sized and un-terminated so a stray read is a real
 * out-of-bounds access (visible under ASan). */
static void test_truncation_never_overruns(void) {
    size_t full = strlen(SAMPLE);
    for (size_t n = 0; n < full; n++) {
        char *chunk = malloc(n ? n : 1);
        memcpy(chunk, SAMPLE, n);
        json_value_t v;
        int rc = json_parse(chunk, n, &v);
        if (rc == JSON_OK) {
            th_fails++;
            printf("    FAIL prefix of length %zu parsed as valid\n", n);
        }
        th_checks++;
        free(chunk);
    }
    /* the whole thing, un-terminated, still parses and navigates */
    char *exact = malloc(full);
    memcpy(exact, SAMPLE, full);
    json_value_t root, c;
    CHECK_EQ(json_parse(exact, full, &root), JSON_OK);
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ(json_count(&c), 3);
    char text[64];
    CHECK_EQ(json_msg_text(&root, text, sizeof text, NULL), JSON_OK);
    CHECK_STR(text, "fable-os online and ready");
    free(exact);
}

/* Deep nesting must hit the depth limit, not the stack guard page. */
static void test_deep_nesting(void) {
    json_value_t v;

    /* exactly at the limit: fine */
    char ok[2 * JSON_MAX_DEPTH + 1];
    for (int i = 0; i < JSON_MAX_DEPTH; i++) {
        ok[i] = '[';
        ok[2 * JSON_MAX_DEPTH - 1 - i] = ']';
    }
    ok[2 * JSON_MAX_DEPTH] = '\0';
    CHECK_EQ(json_parse_str(ok, &v), JSON_OK);

    /* one past: refused, with the specific error */
    char over[2 * (JSON_MAX_DEPTH + 1) + 1];
    for (int i = 0; i < JSON_MAX_DEPTH + 1; i++) {
        over[i] = '[';
        over[2 * (JSON_MAX_DEPTH + 1) - 1 - i] = ']';
    }
    over[2 * (JSON_MAX_DEPTH + 1)] = '\0';
    CHECK_EQ(json_parse_str(over, &v), JSON_EDEPTH);

    /* a hostile 100k-deep opener: must return, not recurse to death */
    size_t n = 100000;
    char *bomb = malloc(n + 1);
    memset(bomb, '[', n);
    bomb[n] = '\0';
    CHECK_EQ(json_parse_str(bomb, &v), JSON_EDEPTH);
    memset(bomb, '{', n);
    CHECK(json_parse_str(bomb, &v) != JSON_OK);
    free(bomb);
}

/* Byte-level mutation: flip every byte of a valid document to a few nasty
 * values and make sure the parser survives and stays self-consistent. */
static void test_mutation_fuzz(void) {
    static const char nasty[] = { '"', '\\', '{', '}', '[', ']', ',', ':', 0x01, 0x7F, (char)0xFF };
    size_t full = strlen(SAMPLE);
    char  *buf  = malloc(full);
    int    survived = 1;

    for (size_t i = 0; i < full; i++) {
        for (size_t k = 0; k < sizeof nasty; k++) {
            memcpy(buf, SAMPLE, full);
            buf[i] = nasty[k];
            json_value_t root;
            if (json_parse(buf, full, &root) != JSON_OK) continue;
            /* If it still parses, navigation must remain bounded and sane. */
            char tmp[512];
            json_value_t c, blk, t;
            if (json_msg_content(&root, &c) == JSON_OK) {
                size_t cnt = json_count(&c);
                if (cnt > 64) { survived = 0; }
                for (size_t j = 0; j < cnt; j++) {
                    if (json_at(&c, j, &blk) != JSON_OK) continue;
                    if (json_get(&blk, "type", &t) == JSON_OK)
                        json_str(&t, tmp, sizeof tmp, NULL);
                }
            }
            json_msg_text(&root, tmp, sizeof tmp, NULL);
            json_msg_stop_reason(&root, tmp, sizeof tmp);
        }
    }
    free(buf);
    CHECK(survived);
}

/* ====================================================================== */
/* encoding + round-trip                                                   */
/* ====================================================================== */

static void test_escape_output(void) {
    char buf[128];

    CHECK_EQ(json_escape(buf, sizeof buf, "plain"), 5);
    CHECK_STR(buf, "plain");

    size_t need = json_escape(buf, sizeof buf, "say \"hi\"\nbye\\");
    CHECK_STR(buf, "say \\\"hi\\\"\\nbye\\\\");
    CHECK_EQ(need, strlen(buf));

    /* control characters become \u00XX */
    char ctl[4] = { 0x01, 0x1F, 0x7F, 0 };
    json_escape(buf, sizeof buf, ctl);
    CHECK_STR(buf, "\\u0001\\u001f\x7f");

    /* UTF-8 passes through untouched */
    json_escape(buf, sizeof buf, "caf\xc3\xa9");
    CHECK_STR(buf, "caf\xc3\xa9");

    /* snprintf-style truncation reporting, with canaries */
    guarded_t g;
    for (size_t cap = 1; cap <= 12; cap++) {
        guard_init(&g);
        size_t r = json_escape(g.buf, cap, "a\"b\"c");
        CHECK(guard_ok(&g));
        CHECK_EQ(r, 7);                       /* a\"b\"c */
        CHECK(strlen(g.buf) < cap);
    }
    CHECK_EQ(json_escape(NULL, 0, "anything"), 8);   /* sizing query */
    CHECK_EQ(json_escape(buf, sizeof buf, NULL), 0);
}

static void test_writer(void) {
    char b[64];
    json_writer_t w;

    json_writer_init(&w, b, sizeof b);
    json_put(&w, "{\"k\":");
    json_put_string(&w, "v\"1");
    json_put(&w, ",\"n\":");
    json_put_int(&w, -17);
    json_put(&w, ",\"u\":");
    json_put_uint(&w, 4200000000u);
    json_put_char(&w, '}');
    CHECK(json_writer_ok(&w));
    CHECK_STR(b, "{\"k\":\"v\\\"1\",\"n\":-17,\"u\":4200000000}");
    CHECK_EQ(json_writer_len(&w), strlen(b));

    json_value_t v;
    CHECK_EQ(json_parse_str(b, &v), JSON_OK);

    /* overflow is sticky, the buffer stays NUL-terminated and in bounds */
    guarded_t g;
    for (size_t cap = 1; cap <= 24; cap++) {
        guard_init(&g);
        json_writer_init(&w, g.buf, cap);
        json_put(&w, "{\"key\":");
        json_put_string(&w, "a long-ish value");
        json_put(&w, "}");
        CHECK(guard_ok(&g));
        CHECK(strlen(g.buf) < cap);
        CHECK(!json_writer_ok(&w));
        /* whatever survived must never be a half-written escape at the end */
        CHECK(strlen(g.buf) == 0 || g.buf[strlen(g.buf) - 1] != '\\');
    }

    /* zero capacity is not a crash */
    json_writer_init(&w, b, 0);
    json_put(&w, "x");
    json_put_string(&w, "y");
    CHECK(!json_writer_ok(&w));
}

/* The property the request path depends on: whatever the user types, escaping
 * it and parsing it back returns exactly the original bytes. */
static void test_escape_parse_roundtrip(void) {
    static const char *inputs[] = {
        "hello",
        "quote \" inside",
        "newline\nand\ttab",
        "back\\slash and \\\" both",
        "\x01\x02\x1f control soup",
        "}{ \"role\": \"system\" } injection attempt",
        "unicode caf\xc3\xa9 \xf0\x9f\x98\x80",
        "trailing backslash \\",
        "",
    };

    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        char doc[512];
        json_writer_t w;
        json_writer_init(&w, doc, sizeof doc);
        json_put(&w, "{\"p\":");
        json_put_string(&w, inputs[i]);
        json_put(&w, "}");
        CHECK(json_writer_ok(&w));

        json_value_t root;
        CHECK_EQ(json_parse_str(doc, &root), JSON_OK);

        char back[512];
        size_t n = 0;
        json_value_t p;
        CHECK_EQ(json_get(&root, "p", &p), JSON_OK);
        CHECK_EQ(json_str(&p, back, sizeof back, &n), JSON_OK);
        CHECK_EQ(n, strlen(inputs[i]));
        CHECK_EQ(memcmp(back, inputs[i], n), 0);
        CHECK(json_str_eq(&p, inputs[i]));
    }
}

/* ====================================================================== */
/* Anthropic sugar                                                         */
/* ====================================================================== */

static void test_message_helpers(void) {
    json_value_t root;
    char buf[256];
    size_t n = 0;

    CHECK_EQ(json_parse_str(SAMPLE, &root), JSON_OK);

    /* text blocks are concatenated, tool_use blocks skipped */
    CHECK_EQ(json_msg_text(&root, buf, sizeof buf, &n), JSON_OK);
    CHECK_STR(buf, "fable-os online and ready");
    CHECK_EQ(n, strlen("fable-os online and ready"));

    CHECK_EQ(json_msg_stop_reason(&root, buf, sizeof buf), JSON_OK);
    CHECK_STR(buf, "tool_use");

    /* an API error body has no content[]: the caller must be told so it can
     * fall back to printing the raw JSON (this is the live 401 path) */
    const char *err =
        "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
        "\"message\":\"x-api-key header is required\"}}";
    CHECK_EQ(json_parse_str(err, &root), JSON_OK);
    CHECK_EQ(json_msg_text(&root, buf, sizeof buf, &n), JSON_ENOENT);
    CHECK_STR(buf, "");
    CHECK_EQ(json_msg_stop_reason(&root, buf, sizeof buf), JSON_ENOENT);
    CHECK_EQ(json_get_str(&root, "type", buf, sizeof buf), JSON_OK);
    CHECK_STR(buf, "error");
    json_value_t e;
    CHECK_EQ(json_get(&root, "error", &e), JSON_OK);
    CHECK_EQ(json_get_str(&e, "message", buf, sizeof buf), JSON_OK);
    CHECK_STR(buf, "x-api-key header is required");

    /* a turn that is only tool_use: no text, still not an error for the caller */
    const char *only_tool =
        "{\"content\":[{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"heap_stats\","
        "\"input\":{}}],\"stop_reason\":\"tool_use\"}";
    CHECK_EQ(json_parse_str(only_tool, &root), JSON_OK);
    CHECK_EQ(json_msg_text(&root, buf, sizeof buf, &n), JSON_ENOENT);
    json_value_t c, blk, in;
    CHECK_EQ(json_msg_content(&root, &c), JSON_OK);
    CHECK_EQ(json_at(&c, 0, &blk), JSON_OK);
    CHECK_EQ(json_get(&blk, "input", &in), JSON_OK);
    CHECK_EQ(json_count(&in), 0);               /* empty input object */
    CHECK_EQ(json_member(&in, 0, NULL, NULL), JSON_ENOENT);

    /* stop_reason may be null while streaming */
    CHECK_EQ(json_parse_str("{\"content\":[],\"stop_reason\":null}", &root), JSON_OK);
    CHECK_EQ(json_msg_stop_reason(&root, buf, sizeof buf), JSON_OK);
    CHECK_STR(buf, "");
    CHECK_EQ(json_msg_text(&root, buf, sizeof buf, &n), JSON_ENOENT);

    /* content of the wrong type must not be walked as an array */
    CHECK_EQ(json_parse_str("{\"content\":\"oops\"}", &root), JSON_OK);
    CHECK_EQ(json_msg_content(&root, &c), JSON_ETYPE);
    CHECK_EQ(json_msg_text(&root, buf, sizeof buf, &n), JSON_ETYPE);

    /* a text block whose text overflows the destination: partial + ENOSPC */
    const char *big =
        "{\"content\":[{\"type\":\"text\",\"text\":\"0123456789\"},"
        "{\"type\":\"text\",\"text\":\"abcdefghij\"}]}";
    CHECK_EQ(json_parse_str(big, &root), JSON_OK);
    guarded_t g;
    guard_init(&g);
    CHECK_EQ(json_msg_text(&root, g.buf, 15, &n), JSON_ENOSPC);
    CHECK(guard_ok(&g));
    CHECK_EQ(n, 14);
    CHECK_STR(g.buf, "0123456789abcd");
}

int main(void) {
    console_init();
    RUN(test_scalars);
    RUN(test_nested_navigation);
    RUN(test_missing_and_wrong_types);
    RUN(test_escapes);
    RUN(test_unicode_escapes);
    RUN(test_decode_bounds);
    RUN(test_malformed);
    RUN(test_truncation_never_overruns);
    RUN(test_deep_nesting);
    RUN(test_mutation_fuzz);
    RUN(test_escape_output);
    RUN(test_writer);
    RUN(test_escape_parse_roundtrip);
    RUN(test_message_helpers);
    return th_report("json");
}
