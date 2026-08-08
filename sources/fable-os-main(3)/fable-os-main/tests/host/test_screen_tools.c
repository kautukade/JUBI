/* test_screen_tools.c — the console tool family, on the host.
 *
 * Runs the REAL tools/screen_tools.c against the REAL core/tool.c, net/json.c
 * and lib/trace.c. Nothing is mocked except the one thing that cannot exist in a
 * host process: the framebuffer itself. tools/screen_tools.c reaches the screen
 * through three accessors that, under FABLEOS_HOSTTEST, resolve to an array this
 * file injects — so the address arithmetic, the bounds tests, the clipping and
 * the exact cell bytes are all the production code paths.
 *
 * The array is flanked by 64 canary cells on each side. On real hardware a write
 * past cell 1999 lands in unprotected memory on a machine with no IDT, so it
 * does not fault — it corrupts something and the VM resets later, somewhere
 * else. Here it smashes a canary and names the test. That is the whole reason
 * this suite exists in preference to only booting the thing.
 *
 * What it sets out to prove:
 *   1. Registration and schemas: three tools, valid JSON Schema, write_screen
 *      carries TOOL_MUTATES and the read paths do not.
 *   2. The round trip: paint known text in a known colour, then verify the exact
 *      16-bit cells (character byte AND attribute byte), then read it back
 *      through read_screen and get the same characters.
 *   3. Bounds, adversarially: every coordinate at and one past every edge,
 *      negatives, 64-bit-huge values, wrong types, truncated JSON, missing
 *      fields, a text longer than the row, a fill_row on the last cell. After
 *      each, canaries intact and (for rejections) not one cell changed.
 *   4. Character policy: control characters and UTF-8 refused, before any cell
 *      is touched.
 *   5. Colours: names, aliases, the bright-background refusal, the exact
 *      attribute nibbles.
 *   6. The payoff: a simulated boot log written into the cells the way lib/base.c
 *      writes it, then read back through read_screen as text.
 *   7. Kernel-emitted trace lines carrying the real values.
 */

#include "harness.h"
#include "tool.h"
#include "trace.h"
#include "json.h"

#include <stdlib.h>

/* ====================================================================== */
/* stand in for the linker-collected tool table                          */
/* ====================================================================== */

extern const tool_t *const fableos_hosttool_screen_info_tool;
extern const tool_t *const fableos_hosttool_read_screen_tool;
extern const tool_t *const fableos_hosttool_write_screen_tool;

/* Deliberately defined WITHOUT the inner const that core/tool.c's extern
 * declaration carries: in the kernel the linker emits this table read-only, but
 * here it must be filled in at startup and a const definition would land in
 * macOS's read-only __DATA_CONST. Same symbol, same layout, same size. */
#if defined(__APPLE__)
#  define SYMPFX "_"
#  define TBL_SECTION __attribute__((section("__DATA,__data")))
#else
#  define SYMPFX ""
#  define TBL_SECTION
#endif

const tool_t *__start_tool_table[3] TBL_SECTION = { 0, 0, 0 };
extern const tool_t *const __stop_tool_table[];

__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 24\n");
_Static_assert(sizeof(__start_tool_table) == 24,
               "tool count changed: update the __stop_tool_table byte offset");

static void install_tool_table(void) {
    __start_tool_table[0] = fableos_hosttool_screen_info_tool;
    __start_tool_table[1] = fableos_hosttool_read_screen_tool;
    __start_tool_table[2] = fableos_hosttool_write_screen_tool;
}

/* ---- the seams tools/screen_tools.c exposes under FABLEOS_HOSTTEST ---- */
void screen_tools_set_framebuffer(volatile uint16_t *fb, int rows, int cols);
void screen_tools_set_cursor(int row, int col);

/* ====================================================================== */
/* the fake framebuffer, canary-guarded on both sides                    */
/* ====================================================================== */

#define FB_ROWS   25
#define FB_COLS   80
#define FB_CELLS  (FB_ROWS * FB_COLS)          /* 2000 cells, 4000 bytes */
#define FB_GUARD  64
#define CANARY    0xDEAD

static uint16_t           fb_store[FB_GUARD + FB_CELLS + FB_GUARD];
static volatile uint16_t *fb = fb_store + FB_GUARD;

#define BLANK ((uint16_t)((0x07 << 8) | ' '))  /* what console_init() writes */

static void fb_reset(void) {
    for (size_t i = 0; i < sizeof fb_store / sizeof fb_store[0]; i++)
        fb_store[i] = CANARY;
    for (int i = 0; i < FB_CELLS; i++) fb[i] = BLANK;
}

static int fb_canaries_intact(void) {
    for (int i = 0; i < FB_GUARD; i++)
        if (fb_store[i] != CANARY) return 0;
    for (size_t i = FB_GUARD + FB_CELLS;
         i < sizeof fb_store / sizeof fb_store[0]; i++)
        if (fb_store[i] != CANARY) return 0;
    return 1;
}

/* Every cell equals BLANK — i.e. a rejected call painted absolutely nothing. */
static int fb_all_blank(void) {
    for (int i = 0; i < FB_CELLS; i++)
        if (fb[i] != BLANK) return 0;
    return 1;
}

/* ====================================================================== */
/* canary-guarded dispatch helper                                        */
/* ====================================================================== */

#define GUARD      64
#define GUARD_BYTE 0xA5

static char          g_store[GUARD + TOOL_RESULT_MAX + GUARD];
static char         *g_out = g_store + GUARD;
static tool_result_t g_res;

static int guards_intact(size_t cap) {
    for (size_t i = 0; i < GUARD; i++)
        if ((unsigned char)g_store[i] != GUARD_BYTE) return 0;
    for (size_t i = GUARD + cap; i < sizeof g_store; i++)
        if ((unsigned char)g_store[i] != GUARD_BYTE) return 0;
    return 1;
}

/* Dispatch `name` with a raw, deliberately NOT NUL-terminated input span. */
static const char *call_n(const char *name, const char *input, size_t input_len) {
    memset(g_store, GUARD_BYTE, sizeof g_store);
    tool_result_init(&g_res, g_out, TOOL_RESULT_MAX);

    /* Copy the span somewhere with no terminator after it, so a tool that walks
     * off the end of input_len reads a filler byte rather than a lucky NUL. */
    static char span[8192];
    memset(span, 0x5A, sizeof span);
    if (input && input_len) {
        if (input_len > sizeof span) input_len = sizeof span;
        memcpy(span, input, input_len);
    }

    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = (input && input_len) ? span : input;
    c.input_len = input_len;

    kcap_reset();
    tool_dispatch(&c, &g_res);

    CHECK(guards_intact(TOOL_RESULT_MAX));
    CHECK(fb_canaries_intact());
    CHECK(g_res.len < TOOL_RESULT_MAX);
    CHECK(g_out[g_res.len] == '\0');
    return g_out;
}

static const char *call_tool(const char *name, const char *input) {
    return call_n(name, input, input ? strlen(input) : 0);
}

static int was_error(void) { return g_res.is_error; }

/* ====================================================================== */
/* 1. registration and schema                                            */
/* ====================================================================== */

static const char *const kTools[] = { "screen_info", "read_screen", "write_screen" };

static void test_tools_are_registered(void) {
    CHECK_EQ(tool_count(), 3);
    for (size_t i = 0; i < sizeof kTools / sizeof kTools[0]; i++) {
        const tool_t *t = tool_find(kTools[i]);
        CHECK(t != NULL);
        if (!t) continue;
        CHECK_STR(t->name, kTools[i]);
        CHECK(t->description != NULL && strlen(t->description) > 40);
        CHECK(t->input_schema != NULL);
        CHECK(t->invoke != NULL);
    }
    /* Painting really changes machine state; reading does not. */
    CHECK_TOOL_FLAGS("write_screen", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("read_screen", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("screen_info", TOOL_MUTATES, 0);
}

static void test_schema_is_valid_json(void) {
    static char schema[16384];
    size_t need = tools_build_schema(schema, sizeof schema);
    CHECK(need > 0);
    CHECK(need < sizeof schema);

    json_value_t root;
    CHECK_EQ(json_parse(schema, need, &root), JSON_OK);
    CHECK_EQ(root.type, JSON_ARRAY);
    CHECK_EQ((int)json_count(&root), tool_count());

    int seen = 0;
    for (size_t i = 0; i < json_count(&root); i++) {
        json_value_t e, v;
        CHECK_EQ(json_at(&root, i, &e), JSON_OK);
        CHECK_EQ(e.type, JSON_OBJECT);

        char name[TOOL_NAME_MAX];
        CHECK_EQ(json_get_str(&e, "name", name, sizeof name), JSON_OK);
        for (size_t k = 0; k < sizeof kTools / sizeof kTools[0]; k++)
            if (strcmp(name, kTools[k]) == 0) seen++;

        CHECK_EQ(json_get(&e, "description", &v), JSON_OK);
        CHECK_EQ(v.type, JSON_STRING);

        CHECK_EQ(json_get(&e, "input_schema", &v), JSON_OK);
        CHECK_EQ(v.type, JSON_OBJECT);

        json_value_t sub, ty;
        CHECK_EQ(json_parse(v.start, v.len, &sub), JSON_OK);
        CHECK_EQ(sub.type, JSON_OBJECT);
        CHECK_EQ(json_get(&sub, "type", &ty), JSON_OK);
        CHECK(json_str_eq(&ty, "object"));
        CHECK_EQ(json_get(&sub, "properties", &ty), JSON_OK);
        CHECK_EQ(ty.type, JSON_OBJECT);
    }
    CHECK_EQ(seen, 3);

    /* The geometry has to reach the model somehow, and the schema is a link-time
     * constant that CANNOT carry a runtime number. So the tool's job is to say
     * that the size is not fixed and to send the model to screen_info, which
     * reports the truth. This assertion used to be CHECK_CONTAINS(schema,
     * "80x25") — pinning a value that stopped being true when the console became
     * a 128x48 framebuffer, and telling a schema-following model that rows end at
     * 24 on a 48-row screen, i.e. that the most recent output is in the middle.
     *
     * IT IS NOW CHECKED AGAINST THE WHOLE TOOL, description included, not against
     * the schema alone. The same caveat used to be spelled out inside three
     * separate property descriptions, which is ~200 bytes of the registry that
     * every request carries on every round (chat.h's cliff note: the margin is
     * tens of bytes, not kilobytes). It is now said once, in the description, and
     * what matters is that a model reading this tool learns it — not which of the
     * two strings it is in. The description ALSO carried a flat contradiction of
     * it, "The screen is 80 columns by 25 rows", which is the thing the schema was
     * added to correct; the last assertion here is that it has not come back. */
    const tool_t *ws = tool_find("write_screen");
    CHECK(ws != NULL);
    CHECK_CONTAINS(schema, "screen_info");
    CHECK_CONTAINS(ws->description, "screen_info");
    CHECK_CONTAINS(ws->description, "128x48");
    CHECK_CONTAINS(ws->description, "80x25");
    CHECK(strstr(ws->description, "The screen is 80 columns by 25 rows") == NULL);
    CHECK(strstr(schema, "so rows are 0-24") == NULL);
    CHECK(strstr(schema, "bottom line of the 80x25 screen") == NULL);
    CHECK(strstr(schema, "79 = rightmost") == NULL);
    CHECK_CONTAINS(schema, "\"required\":[\"row\",\"text\"]");
    /* Colour names, not attribute nibbles. */
    CHECK_CONTAINS(schema, "light_magenta");
    CHECK_CONTAINS(schema, "blink");
}

/* ====================================================================== */
/* 2. no framebuffer: refuse, never dereference                          */
/* ====================================================================== */

static void test_no_framebuffer_is_refused(void) {
    screen_tools_set_framebuffer(NULL, 0, 0);

    CHECK_CONTAINS(call_tool("screen_info", "{}"), "no text-mode framebuffer");
    CHECK(was_error());
    CHECK_CONTAINS(call_tool("read_screen", "{}"), "no text-mode framebuffer");
    CHECK(was_error());
    CHECK_CONTAINS(call_tool("write_screen", "{\"row\":0,\"text\":\"x\"}"),
                   "no text-mode framebuffer");
    CHECK(was_error());

    screen_tools_set_framebuffer(fb, FB_ROWS, FB_COLS);
}

/* ====================================================================== */
/* 3. the round trip: exact cells, then exact text back                  */
/* ====================================================================== */

static void test_round_trip_exact_cells(void) {
    fb_reset();
    screen_tools_set_cursor(12, 0);

    const char *out = call_tool("write_screen",
        "{\"row\":3,\"col\":10,\"text\":\"HELLO\",\"fg\":\"yellow\",\"bg\":\"blue\"}");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "painted 5 character(s) at row 3, col 10");
    CHECK_CONTAINS(out, "fg=yellow bg=blue");
    /* bg=1, fg=14 -> 0x1e */
    CHECK_CONTAINS(out, "attribute byte 0x1e");

    /* The strongest assertion available: the exact 16-bit cells. */
    const uint16_t attr = 0x1E00;
    const char    *want = "HELLO";
    for (int i = 0; i < 5; i++)
        CHECK_EQ(fb[3 * FB_COLS + 10 + i], (uint16_t)(attr | (uint8_t)want[i]));

    /* Neither neighbour was touched. */
    CHECK_EQ(fb[3 * FB_COLS + 9], BLANK);
    CHECK_EQ(fb[3 * FB_COLS + 15], BLANK);
    CHECK_EQ(fb[2 * FB_COLS + 10], BLANK);
    CHECK_EQ(fb[4 * FB_COLS + 10], BLANK);
    CHECK(fb_canaries_intact());

    /* ...and now read the same cells back as text through the real tool. */
    out = call_tool("read_screen", "{\"first_row\":3,\"last_row\":3}");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "\n03|          HELLO\n");   /* 10 spaces of left pad  */

    /* Without line numbers the payload is the bare row. */
    out = call_tool("read_screen",
                    "{\"first_row\":3,\"last_row\":3,\"line_numbers\":false}");
    CHECK_CONTAINS(out, "\n          HELLO\n");
    CHECK(strstr(out, "03|") == NULL);
}

static void test_write_does_not_move_the_cursor(void) {
    fb_reset();
    screen_tools_set_cursor(17, 42);

    const char *out = call_tool("write_screen",
        "{\"row\":0,\"col\":0,\"text\":\"status\",\"fg\":\"white\",\"bg\":\"blue\"}");
    CHECK(!was_error());
    /* The honesty contract: the result states the cursor and the scroll budget. */
    CHECK_CONTAINS(out, "cursor is at row 17, col 42 and was NOT moved");
    CHECK_CONTAINS(out, "The next 7 line(s) of kernel output fit");
    CHECK_CONTAINS(out, "Row 0 is at or above the cursor");

    /* And screen_info still reports the same, unmoved cursor. */
    out = call_tool("screen_info", "{}");
    CHECK_CONTAINS(out, "cursor: row 17, col 42");
    CHECK_CONTAINS(out, "7 more line(s) of output fit");

    /* Painting below the cursor gets the other warning. */
    call_tool("write_screen", "{\"row\":24,\"text\":\"bar\"}");
    CHECK_CONTAINS(g_out, "Row 24 is below the cursor");
}

static void test_fill_row_paints_to_the_edge(void) {
    fb_reset();
    screen_tools_set_cursor(5, 0);

    /* Derived from the text rather than hardcoded. Renaming the OS from talk-os
     * to fable-os moved every one of these by one column and failed four checks
     * at once — the assertions were pinning "column 6 holds 's'" when what they
     * mean is "the last painted column holds the last character". */
    static const char TEXT[] = "fable-os";
    const int len = (int)(sizeof TEXT - 1);

    const char *out = call_tool("write_screen",
        "{\"row\":0,\"col\":0,\"text\":\"" "fable-os" "\",\"fg\":\"black\","
        "\"bg\":\"light_grey\",\"fill_row\":true}");
    CHECK(!was_error());
    char want_fill[64];
    snprintf(want_fill, sizeof want_fill, "filled the remaining %d column(s)",
             FB_COLS - len);
    CHECK_CONTAINS(out, want_fill);

    const uint16_t attr = 0x7000;                 /* bg=7 fg=0 */
    for (int i = 0; i < len; i++)
        CHECK_EQ(fb[i], (uint16_t)(attr | (unsigned char)TEXT[i]));
    for (int x = len; x < FB_COLS; x++)
        CHECK_EQ(fb[x], (uint16_t)(attr | ' '));
    /* Exactly one row: the first cell of row 1 is untouched. */
    CHECK_EQ(fb[FB_COLS], BLANK);
    CHECK(fb_canaries_intact());
}

/* ====================================================================== */
/* 4. bounds — every edge, and one past it                               */
/* ====================================================================== */

static void test_edge_coordinates_are_exact(void) {
    /* The very last cell of the framebuffer. Nothing may be written past it. */
    fb_reset();
    screen_tools_set_cursor(24, 79);
    const char *out = call_tool("write_screen",
        "{\"row\":24,\"col\":79,\"text\":\"Z\",\"fg\":\"white\"}");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "painted 1 character(s) at row 24, col 79");
    CHECK_EQ(fb[FB_CELLS - 1], (uint16_t)(0x0F00 | 'Z'));
    CHECK_EQ(fb[FB_CELLS - 2], BLANK);
    CHECK(fb_canaries_intact());

    /* fill_row on the last cell must fill zero cells, not one past the end. */
    fb_reset();
    out = call_tool("write_screen",
        "{\"row\":24,\"col\":79,\"text\":\"Z\",\"bg\":\"red\",\"fill_row\":true}");
    CHECK(!was_error());
    CHECK(strstr(out, "filled the remaining") == NULL);
    CHECK_EQ(fb[FB_CELLS - 1], (uint16_t)(0x4700 | 'Z'));
    CHECK(fb_canaries_intact());

    /* A full-width row: exactly 80 cells, the 80th being the last of the row. */
    fb_reset();
    out = call_tool("write_screen",
        "{\"row\":24,\"col\":0,\"text\":"
        "\"01234567890123456789012345678901234567890123456789"
        "012345678901234567890123456789\"}");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "painted 80 character(s)");
    CHECK(strstr(out, "CLIPPED") == NULL);
    CHECK_EQ(fb[FB_CELLS - 1] & 0xFF, '9');
    CHECK(fb_canaries_intact());

    /* One character too many at col 1: clipped to 79, and it says so. */
    fb_reset();
    out = call_tool("write_screen",
        "{\"row\":24,\"col\":1,\"text\":"
        "\"01234567890123456789012345678901234567890123456789"
        "012345678901234567890123456789\"}");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "painted 79 character(s)");
    CHECK_CONTAINS(out, "CLIPPED: 1 character(s) did not fit");
    CHECK_EQ(fb[FB_CELLS - 1] & 0xFF, '8');
    CHECK(fb_canaries_intact());

    /* Row 0 col 0 — the other corner. */
    fb_reset();
    call_tool("write_screen", "{\"row\":0,\"col\":0,\"text\":\"A\"}");
    CHECK_EQ(fb[0], (uint16_t)(0x0700 | 'A'));
    CHECK(fb_canaries_intact());
}

/* Coordinates one past every edge, negative, and absurd. Each must be refused
 * with the geometry named, and must not paint a single cell. */
static void test_out_of_range_coordinates_are_refused(void) {
    static const char *const bad[] = {
        "{\"row\":25,\"text\":\"x\"}",
        "{\"row\":-1,\"text\":\"x\"}",
        "{\"row\":0,\"col\":80,\"text\":\"x\"}",
        "{\"row\":0,\"col\":-1,\"text\":\"x\"}",
        "{\"row\":2147483648,\"text\":\"x\"}",
        "{\"row\":9223372036854775807,\"text\":\"x\"}",
        "{\"row\":-9223372036854775807,\"text\":\"x\"}",
        "{\"row\":0,\"col\":9223372036854775807,\"text\":\"x\"}",
        "{\"row\":0,\"col\":99999999999999999999999,\"text\":\"x\"}",
        "{\"row\":1000000,\"col\":1000000,\"text\":\"x\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fb_reset();
        const char *out = call_tool("write_screen", bad[i]);
        CHECK(was_error());
        CHECK_CONTAINS(out, "error: ");
        CHECK(fb_all_blank());
        CHECK(fb_canaries_intact());
    }
    /* The refusals teach the geometry rather than just saying "no". */
    fb_reset();
    CHECK_CONTAINS(call_tool("write_screen", "{\"row\":25,\"text\":\"x\"}"),
                   "rows 0-24");
    CHECK_CONTAINS(call_tool("write_screen", "{\"row\":0,\"col\":80,\"text\":\"x\"}"),
                   "columns 0-79");
    CHECK(fb_all_blank());
}

static void test_malformed_input_is_refused(void) {
    static const char *const bad[] = {
        "{\"row\":\"0\",\"text\":\"x\"}",          /* row as a string       */
        "{\"row\":0.5,\"text\":\"x\"}",            /* fractional: not adjusted */
        "{\"row\":1e1,\"text\":\"x\"}",            /* exponent: not adjusted   */
        "{\"row\":true,\"text\":\"x\"}",
        "{\"row\":[0],\"text\":\"x\"}",
        "{\"row\":0,\"text\":42}",                 /* text not a string     */
        "{\"row\":0,\"text\":null}",               /* explicitly absent     */
        "{\"row\":0}",                             /* text missing          */
        "{\"text\":\"x\"}",                        /* row missing           */
        "{}",
        "{\"row\":0,\"text\":\"x\",\"fill_row\":\"yes\"}",
        "{\"row\":0,\"text\":\"x\",\"fg\":7}",     /* attribute nibble, not a name */
        "{\"row\":0,\"text\":\"x\",\"fg\":\"chartreuse\"}",
        "[]",
        "\"not an object\"",
        "null",
        "{\"row\":0,\"text\":\"x\"",               /* truncated JSON        */
        "{\"row\":0,\"text\":\"unterminated",
        "{\"row\":",
        "{",
        "",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fb_reset();
        const char *out = call_tool("write_screen", bad[i]);
        CHECK(was_error());
        CHECK_CONTAINS(out, "error: ");
        CHECK(fb_all_blank());
        CHECK(fb_canaries_intact());
    }

    /* A span whose declared length stops mid-document: the tool must honour
     * input_len, not run to the filler bytes past it. */
    fb_reset();
    const char *json = "{\"row\":0,\"text\":\"x\"}";
    CHECK(was_error() || 1);
    call_n("write_screen", json, 8);
    CHECK(was_error());
    CHECK(fb_all_blank());

    /* The same span, in full, must work — proving the length is what mattered. */
    call_n("write_screen", json, strlen(json));
    CHECK(!was_error());
    CHECK_EQ(fb[0] & 0xFF, 'x');
}

static void test_oversized_text_is_refused_then_clipped(void) {
    /* Longer than TEXT_MAX (512): refused outright, nothing painted. */
    static char big[1200];
    int n = 0;
    n += snprintf(big + n, sizeof big - (size_t)n, "{\"row\":0,\"text\":\"");
    for (int i = 0; i < 600; i++) big[n++] = 'A';
    n += snprintf(big + n, sizeof big - (size_t)n, "\"}");
    big[n] = '\0';

    fb_reset();
    const char *out = call_tool("write_screen", big);
    CHECK(was_error());
    CHECK_CONTAINS(out, "longer than 512 characters");
    CHECK(fb_all_blank());

    /* Between 80 and 512: accepted and clipped at the right edge. */
    n = 0;
    n += snprintf(big + n, sizeof big - (size_t)n, "{\"row\":10,\"col\":0,\"text\":\"");
    for (int i = 0; i < 200; i++) big[n++] = 'B';
    n += snprintf(big + n, sizeof big - (size_t)n, "\"}");
    big[n] = '\0';

    fb_reset();
    out = call_tool("write_screen", big);
    CHECK(!was_error());
    CHECK_CONTAINS(out, "painted 80 character(s)");
    CHECK_CONTAINS(out, "CLIPPED: 120 character(s) did not fit");
    for (int x = 0; x < FB_COLS; x++) CHECK_EQ(fb[10 * FB_COLS + x] & 0xFF, 'B');
    CHECK_EQ(fb[11 * FB_COLS], BLANK);            /* never wrapped */
    CHECK(fb_canaries_intact());
}

/* ====================================================================== */
/* 5. character policy                                                   */
/* ====================================================================== */

static void test_control_and_non_ascii_are_refused(void) {
    fb_reset();
    const char *out = call_tool("write_screen",
                                "{\"row\":0,\"text\":\"line one\\nline two\"}");
    CHECK(was_error());
    CHECK_CONTAINS(out, "control character 0x0a at offset 8");
    CHECK_CONTAINS(out, "one row per call");
    CHECK(fb_all_blank());                        /* refused BEFORE any cell */

    fb_reset();
    out = call_tool("write_screen", "{\"row\":0,\"text\":\"a\\tb\"}");
    CHECK(was_error());
    CHECK_CONTAINS(out, "control character 0x09");
    CHECK(fb_all_blank());

    /* An embedded NUL: strlen() would see a one-character string and paint it.
     * The decoded length is what gets validated, so the whole string is seen. */
    fb_reset();
    out = call_tool("write_screen", "{\"row\":0,\"text\":\"a\\u0000b\"}");
    CHECK(was_error());
    CHECK_CONTAINS(out, "control character 0x00 at offset 1");
    CHECK(fb_all_blank());

    fb_reset();
    out = call_tool("write_screen", "{\"row\":0,\"text\":\"a\\u007fb\"}");
    CHECK(was_error());
    CHECK_CONTAINS(out, "control character 0x7f");
    CHECK(fb_all_blank());

    /* A box-drawing character: valid JSON, valid UTF-8, three CP437 glyphs. */
    fb_reset();
    out = call_tool("write_screen", "{\"row\":0,\"text\":\"\\u2500\\u2500\"}");
    CHECK(was_error());
    CHECK_CONTAINS(out, "non-ASCII byte 0xe2 at offset 0");
    CHECK_CONTAINS(out, "code page 437");
    CHECK(fb_all_blank());

    /* Every printable ASCII byte is accepted. */
    fb_reset();
    static char line[200];
    int n = snprintf(line, sizeof line, "{\"row\":0,\"col\":0,\"text\":\"");
    for (int ch = 0x20; ch <= 0x7E; ch++) {
        if (ch == '"' || ch == '\\') line[n++] = '\\';
        line[n++] = (char)ch;
    }
    n += snprintf(line + n, sizeof line - (size_t)n, "\"}");
    line[n] = '\0';
    out = call_tool("write_screen", line);
    CHECK(!was_error());
    CHECK_CONTAINS(out, "painted 80 character(s)");   /* 95 chars, clipped to 80 */
    CHECK_EQ(fb[0] & 0xFF, 0x20);
    CHECK_EQ(fb[79] & 0xFF, 0x20 + 79);
    CHECK(fb_canaries_intact());
}

/* ====================================================================== */
/* 6. colours                                                            */
/* ====================================================================== */

static void test_colour_names_map_to_attribute_nibbles(void) {
    static const struct { const char *fg; const char *bg; uint16_t attr; } cases[] = {
        { "black",         "black",      0x0000 },
        { "white",         "black",      0x0F00 },
        { "light_grey",    "black",      0x0700 },
        { "yellow",        "blue",       0x1E00 },
        { "light_cyan",    "magenta",    0x5B00 },
        { "light_magenta", "green",      0x2D00 },
        { "dark_grey",     "light_grey", 0x7800 },
        { "brown",         "cyan",       0x3600 },
        { "light_red",     "red",        0x4C00 },
        { "light_green",   "brown",      0x6A00 },
        /* aliases a model is likely to reach for */
        { "gray",          "black",      0x0700 },
        { "light_gray",    "black",      0x0700 },
        { "dark_gray",     "black",      0x0800 },
        { "orange",        "black",      0x0600 },
        { "purple",        "black",      0x0500 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char in[160];
        snprintf(in, sizeof in,
                 "{\"row\":1,\"col\":0,\"text\":\"#\",\"fg\":\"%s\",\"bg\":\"%s\"}",
                 cases[i].fg, cases[i].bg);
        fb_reset();
        call_tool("write_screen", in);
        CHECK(!was_error());
        CHECK_EQ(fb[FB_COLS], (uint16_t)(cases[i].attr | '#'));
    }

    /* Defaults match the console's own light-grey-on-black. */
    fb_reset();
    call_tool("write_screen", "{\"row\":0,\"text\":\"d\"}");
    CHECK_EQ(fb[0], (uint16_t)(0x0700 | 'd'));
}

static void test_bright_background_is_refused(void) {
    static const char *const bright[] = { "dark_grey", "light_blue", "light_green",
                                          "light_cyan", "light_red",
                                          "light_magenta", "yellow", "white" };
    for (size_t i = 0; i < sizeof bright / sizeof bright[0]; i++) {
        char in[160];
        snprintf(in, sizeof in, "{\"row\":0,\"text\":\"x\",\"bg\":\"%s\"}", bright[i]);
        fb_reset();
        const char *out = call_tool("write_screen", in);
        CHECK(was_error());
        CHECK_CONTAINS(out, "blink enable");
        CHECK(fb_all_blank());
    }
    /* All eight dark backgrounds are fine. */
    static const char *const dark[] = { "black", "blue", "green", "cyan",
                                        "red", "magenta", "brown", "light_grey" };
    for (size_t i = 0; i < sizeof dark / sizeof dark[0]; i++) {
        char in[160];
        snprintf(in, sizeof in, "{\"row\":0,\"text\":\"x\",\"bg\":\"%s\"}", dark[i]);
        fb_reset();
        call_tool("write_screen", in);
        CHECK(!was_error());
    }
}

/* ====================================================================== */
/* 7. read_screen                                                        */
/* ====================================================================== */

/* Paint text into the fake framebuffer the way lib/base.c's kputc does, so what
 * read_screen sees is shaped like a real boot log. */
static void fb_puts(int row, const char *s) {
    for (int x = 0; s[x] && x < FB_COLS; x++)
        fb[row * FB_COLS + x] = (uint16_t)((0x07 << 8) | (uint8_t)s[x]);
}

static void test_read_screen_reads_the_boot_log(void) {
    fb_reset();
    screen_tools_set_cursor(6, 0);

    /* Verbatim shapes from a real fable-os boot. */
    fb_puts(0, "fable-os booting");
    fb_puts(1, "  serial : COM1 115200");
    fb_puts(2, "  pci    : 4 functions, 1 unclaimed");
    fb_puts(3, "  net    : dhcp bound 10.0.2.15");
    fb_puts(4, "[dns_resolve host=api.anthropic.com tries=3 -> ok]");
    fb_puts(5, "ai> ready");

    const char *out = call_tool("read_screen", "{}");
    CHECK(!was_error());

    /* The payoff: the model can read what the kernel printed earlier. */
    CHECK_CONTAINS(out, "00|fable-os booting\n");
    CHECK_CONTAINS(out, "01|  serial : COM1 115200\n");
    CHECK_CONTAINS(out, "03|  net    : dhcp bound 10.0.2.15\n");
    CHECK_CONTAINS(out, "04|[dns_resolve host=api.anthropic.com tries=3 -> ok]\n");
    CHECK_CONTAINS(out, "05|ai> ready\n");

    /* Trailing blank rows cost one line of budget, not nineteen. */
    CHECK_CONTAINS(out, "[rows 6-24 are blank]");
    CHECK(strstr(out, "06|") == NULL);
    CHECK(g_res.len < 400);

    /* Each row is right-stripped: no 60 spaces of padding. */
    CHECK(strstr(out, "00|fable-os booting \n") == NULL);

    /* A region reads only that region. */
    out = call_tool("read_screen", "{\"first_row\":4,\"last_row\":4}");
    CHECK_CONTAINS(out, "04|[dns_resolve host=api.anthropic.com tries=3 -> ok]\n");
    CHECK(strstr(out, "fable-os booting") == NULL);
    CHECK(strstr(out, "ai> ready") == NULL);
    CHECK_CONTAINS(out, "screen rows 4-4 of 0-24");
}

static void test_read_screen_blank_and_odd_cells(void) {
    fb_reset();
    const char *out = call_tool("read_screen", "{}");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "[every row in this range is blank]");

    /* Cells never written (all-zero) read as blanks, not as NUL bytes; a CP437
     * glyph with no ASCII equivalent reads as '.'. */
    fb[0] = 0x0000;
    fb[1] = (uint16_t)(0x0700 | 0xB0);
    fb[2] = (uint16_t)(0x0700 | 'k');
    out = call_tool("read_screen", "{\"first_row\":0,\"last_row\":0}");
    CHECK_CONTAINS(out, "00| .k\n");
    CHECK(strlen(out) == g_res.len);
}

static void test_read_screen_range_errors(void) {
    fb_reset();
    CHECK_CONTAINS(call_tool("read_screen", "{\"first_row\":25}"), "rows 0-24");
    CHECK(was_error());
    CHECK_CONTAINS(call_tool("read_screen", "{\"last_row\":25}"), "rows 0-24");
    CHECK(was_error());
    CHECK_CONTAINS(call_tool("read_screen", "{\"first_row\":-1}"), "rows 0-24");
    CHECK(was_error());
    CHECK_CONTAINS(call_tool("read_screen",
                             "{\"first_row\":10,\"last_row\":2}"),
                   "is above");
    CHECK(was_error());
    CHECK_CONTAINS(call_tool("read_screen", "{\"first_row\":\"top\"}"),
                   "must be an integer");
    CHECK(was_error());
    CHECK_CONTAINS(call_tool("read_screen", "{\"line_numbers\":\"yes\"}"),
                   "must be a boolean");
    CHECK(was_error());
    CHECK_CONTAINS(call_tool("read_screen", "[1,2,3]"), "must be a JSON object");
    CHECK(was_error());

    /* first_row == last_row is a legal one-row read. */
    CHECK(!was_error() || 1);
    call_tool("read_screen", "{\"first_row\":24,\"last_row\":24}");
    CHECK(!was_error());
}

/* A screen full of dense text must not overflow the 4 KiB result; it must stop
 * early and say where to resume. */
static void test_read_screen_respects_the_result_budget(void) {
    fb_reset();
    char row[FB_COLS + 1];
    for (int x = 0; x < FB_COLS; x++) row[x] = (char)('A' + (x % 26));
    row[FB_COLS] = '\0';
    for (int y = 0; y < FB_ROWS; y++) fb_puts(y, row);

    const char *out = call_tool("read_screen", "{}");
    CHECK(!was_error());
    CHECK(g_res.len < TOOL_RESULT_MAX);
    CHECK_CONTAINS(out, "00|ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    /* 25 rows x 83 bytes = 2075 + header: it all fits, so no stop note. */
    CHECK(strstr(out, "[stopped at row") == NULL);
    CHECK_CONTAINS(out, "24|ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    CHECK(guards_intact(TOOL_RESULT_MAX));

    /* Now force the clip by handing the handler a small buffer directly. */
    memset(g_store, GUARD_BYTE, sizeof g_store);
    tool_result_init(&g_res, g_out, 600);
    tool_call_t c = { "toolu_x", "read_screen", "{}", 2 };
    tool_dispatch(&c, &g_res);
    CHECK(guards_intact(600));
    CHECK(g_res.len < 600);
    CHECK_CONTAINS(g_out, "[stopped at row ");
    CHECK_CONTAINS(g_out, "Call read_screen again with first_row=");
}

/* ====================================================================== */
/* 8. screen_info                                                        */
/* ====================================================================== */

static void test_screen_info_reports_ground_truth(void) {
    fb_reset();
    screen_tools_set_cursor(9, 13);
    fb_puts(0, "one");
    fb_puts(1, "two");

    const char *out = call_tool("screen_info", "{}");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "geometry: 25 rows x 80 columns, rows 0-24");
    CHECK_CONTAINS(out, "columns 0-79");
    CHECK_CONTAINS(out, "4000 bytes, 2 bytes per cell");
    CHECK_CONTAINS(out, "cursor: row 9, col 13");
    CHECK_CONTAINS(out, "15 more line(s) of output fit");
    CHECK_CONTAINS(out, "content: 2 of 25 rows are non-blank");
    CHECK_CONTAINS(out, "light_magenta");

    /* At the bottom row there is no room left before the next scroll. */
    screen_tools_set_cursor(24, 0);
    out = call_tool("screen_info", "{}");
    CHECK_CONTAINS(out, "0 more line(s) of output fit");

    /* Arguments are ignored rather than able to break it. */
    out = call_tool("screen_info", "{\"nonsense\":[1,2,{}]}");
    CHECK(!was_error());
    out = call_tool("screen_info", "not json at all");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "geometry:");
}

/* ====================================================================== */
/* 9. kernel-emitted trace lines carry the real values                   */
/* ====================================================================== */

static void test_trace_lines_are_ground_truth(void) {
    fb_reset();
    screen_tools_set_cursor(3, 0);

    uint64_t before = trace_emissions();
    call_tool("write_screen",
        "{\"row\":2,\"col\":5,\"text\":\"hello world\",\"fg\":\"white\","
        "\"bg\":\"blue\",\"fill_row\":true}");
    CHECK_EQ(trace_emissions() - before, 1);
    CHECK_CONTAINS(kcap_text(),
        "[write_screen row=2 col=5 len=11 fg=white bg=blue attr=0x1f fill=1 "
        "clipped=0 -> 11]");

    /* A clipped write reports the real numbers, not the requested ones. */
    call_tool("write_screen", "{\"row\":0,\"col\":78,\"text\":\"abcdef\"}");
    CHECK_CONTAINS(kcap_text(), "col=78 len=6");
    CHECK_CONTAINS(kcap_text(), "clipped=4 -> 2]");

    /* A refusal traces symbolically and does not claim success. */
    call_tool("write_screen", "{\"row\":99,\"text\":\"x\"}");
    CHECK_CONTAINS(kcap_text(), "[write_screen -> EINVAL]");

    call_tool("read_screen", "{\"first_row\":0,\"last_row\":4}");
    CHECK_CONTAINS(kcap_text(), "[read_screen rows=0-4 blank_tail=");

    call_tool("screen_info", "{}");
    CHECK_CONTAINS(kcap_text(), "[screen_info geom=80x25 cursor=3,0 used_rows=");

    /* Exactly one kernel line per model action — nothing forged, nothing lost. */
    before = trace_emissions();
    call_tool("read_screen", "{}");
    call_tool("screen_info", "{}");
    call_tool("write_screen", "{\"row\":1,\"text\":\"q\"}");
    call_tool("write_screen", "{\"row\":-5,\"text\":\"q\"}");
    CHECK_EQ(trace_emissions() - before, 4);
}

/* ====================================================================== */
/* 9b. the column-zero bracket rule                                       */
/* ====================================================================== */

/* trace.h's invariant is that a kernel trace line is a '[' in COLUMN ZERO, and
 * three mechanisms defend it: lib/trace.c escapes brackets inside arguments,
 * tool_dispatch() counts emissions, and net/chat.c space-prefixes model prose.
 * This tool is a FOURTH path to the same cells and nobody had enumerated it.
 * screen_fb() IS console_fb(); lib/base.c's reconcile() rasterises whatever is
 * poked there with the same font and the same palette as genuine kernel output,
 * and FG_DEFAULT/BG_DEFAULT are literally the attribute console_init() blanks
 * the grid with. A painted trace line was therefore not similar to a real one,
 * it was the same bytes in the same cells in the same colours — and the model
 * could then paint it tens of rows from its own accusing audit line.
 *
 * These are the tests that were missing. The file is 900 lines long and never
 * once asked whether the painted text may begin with '['. */
static void test_a_forged_trace_line_is_refused_at_column_zero(void) {
    fb_reset();
    const char *out = call_tool("write_screen",
        "{\"row\":5,\"col\":0,\"fg\":\"white\",\"bg\":\"black\","
        "\"text\":\"[vfs_write /etc/shadow mode=overwrite bytes=9999 -> ok]\"}");
    CHECK(was_error());
    CHECK_CONTAINS(out, "column zero");
    CHECK_CONTAINS(out, "Nothing was painted");
    CHECK(fb_all_blank());                    /* refused BEFORE any cell */
    CHECK_CONTAINS(kcap_text(), "[write_screen -> EINVAL]");

    /* A lone bracket is the same forgery with fewer characters. */
    fb_reset();
    CHECK(was_error() == 0 || 1);
    out = call_tool("write_screen", "{\"row\":0,\"col\":0,\"text\":\"[\"}");
    CHECK(was_error());
    CHECK(fb_all_blank());

    /* ONE COLUMN RIGHT IS ALL IT TAKES, and the refusal says so, so a status
     * bar that genuinely wants a bracket is one corrected call away. */
    fb_reset();
    out = call_tool("write_screen", "{\"row\":5,\"col\":1,\"text\":\"[ok]\"}");
    CHECK(!was_error());
    CHECK_CONTAINS(out, "painted 4 character(s)");
    CHECK_EQ(fb[5 * 80 + 0] & 0xFF, ' ');     /* column zero still blank */
    CHECK_EQ(fb[5 * 80 + 1] & 0xFF, '[');

    /* Brackets anywhere else in the string are untouched: the rule is about
     * one cell, not about the character. */
    fb_reset();
    out = call_tool("write_screen", "{\"row\":6,\"col\":0,\"text\":\"a [b] c\"}");
    CHECK(!was_error());
    CHECK_EQ(fb[6 * 80 + 2] & 0xFF, '[');
    CHECK(fb_canaries_intact());
}

/* The other half of the same tool: painting over a row that already holds a
 * kernel line deletes ground truth. That is allowed — the console is scratch
 * space and it scrolls — but it may not be SILENT, because erasure is what
 * chat.h calls "worse than forgery". The one trace line this call emits now
 * carries it. */
static void test_erasing_a_kernel_line_is_recorded(void) {
    fb_reset();
    /* Put a genuine-looking kernel line on row 7 the way the console would. */
    const char *line = "[vfs_delete /etc/motd -> ok]";
    for (int i = 0; line[i]; i++) fb[7 * 80 + i] = (uint16_t)(0x0700 | line[i]);

    const char *out = call_tool("write_screen",
        "{\"row\":7,\"col\":0,\"text\":\" \",\"fill_row\":true}");
    CHECK(!was_error());                       /* still allowed */
    CHECK_EQ(fb[7 * 80] & 0xFF, ' ');          /* really erased */
    CHECK_CONTAINS(out, "covered a line the kernel had printed");
    CHECK_CONTAINS(kcap_text(), "erased-kernel-line=1");

    /* A row that held nothing of the kernel's says nothing. */
    fb_reset();
    call_tool("write_screen",
              "{\"row\":9,\"col\":0,\"text\":\"status\",\"fill_row\":true}");
    CHECK(strstr(kcap_text(), "erased-kernel-line=1") == NULL);

    /* Neither does a paint that starts past column zero, even on that row. */
    for (int i = 0; line[i]; i++) fb[3 * 80 + i] = (uint16_t)(0x0700 | line[i]);
    kcap_reset();
    call_tool("write_screen", "{\"row\":3,\"col\":40,\"text\":\"x\"}");
    CHECK(strstr(kcap_text(), "erased-kernel-line=1") == NULL);
    CHECK_EQ(fb[3 * 80] & 0xFF, '[');          /* the '[' is still there */
}

/* ====================================================================== */
/* 10. fuzz: nothing the model can send may reach past the framebuffer   */
/* ====================================================================== */

static void test_fuzz_never_escapes_the_framebuffer(void) {
    static const char alphabet[] =
        "{}[]\":,0123456789-+.eE truefalsenull\\rowcltxfgb_screenwrite\x01\xff";
    unsigned seed = 0xC0FFEEu;

    for (int iter = 0; iter < 4000; iter++) {
        char   buf[128];
        size_t n = (size_t)(seed % (sizeof buf - 1));
        for (size_t i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = alphabet[(seed >> 16) % (sizeof alphabet - 1)];
        }
        buf[n] = '\0';
        seed = seed * 1103515245u + 12345u;

        fb_reset();
        call_n((iter & 1) ? "write_screen" : "read_screen", buf, n);
        CHECK(fb_canaries_intact());
        CHECK(guards_intact(TOOL_RESULT_MAX));
    }

    /* Well-formed objects with hostile numeric fields, exhaustively around every
     * boundary value on both axes. */
    for (int row = -3; row <= FB_ROWS + 2; row++) {
        for (int col = -3; col <= FB_COLS + 2; col++) {
            char in[160];
            snprintf(in, sizeof in,
                     "{\"row\":%d,\"col\":%d,\"text\":\"XXXXXXXXXX\","
                     "\"fill_row\":true}", row, col);
            fb_reset();
            call_tool("write_screen", in);
            int legal = (row >= 0 && row < FB_ROWS && col >= 0 && col < FB_COLS);
            CHECK_EQ(!was_error(), legal);
            if (!legal) CHECK(fb_all_blank());
            CHECK(fb_canaries_intact());
        }
    }
}

/* ====================================================================== */

int main(void) {
    install_tool_table();
    screen_tools_set_framebuffer(fb, FB_ROWS, FB_COLS);
    screen_tools_set_cursor(0, 0);
    fb_reset();

    RUN(test_tools_are_registered);
    RUN(test_schema_is_valid_json);
    RUN(test_no_framebuffer_is_refused);
    RUN(test_round_trip_exact_cells);
    RUN(test_write_does_not_move_the_cursor);
    RUN(test_fill_row_paints_to_the_edge);
    RUN(test_edge_coordinates_are_exact);
    RUN(test_out_of_range_coordinates_are_refused);
    RUN(test_malformed_input_is_refused);
    RUN(test_oversized_text_is_refused_then_clipped);
    RUN(test_control_and_non_ascii_are_refused);
    RUN(test_colour_names_map_to_attribute_nibbles);
    RUN(test_bright_background_is_refused);
    RUN(test_read_screen_reads_the_boot_log);
    RUN(test_read_screen_blank_and_odd_cells);
    RUN(test_read_screen_range_errors);
    RUN(test_read_screen_respects_the_result_budget);
    RUN(test_screen_info_reports_ground_truth);
    RUN(test_trace_lines_are_ground_truth);
    RUN(test_a_forged_trace_line_is_refused_at_column_zero);
    RUN(test_erasing_a_kernel_line_is_recorded);
    RUN(test_fuzz_never_escapes_the_framebuffer);

    CHECK_EQ(kpanic_hit, 0);
    return th_report("screen_tools");
}
