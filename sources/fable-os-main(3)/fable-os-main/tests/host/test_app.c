/* test_app.c — the app runtime: documents, expressions, a calculator, the tool,
 *              and the whole "build me a calculator" loop against the mock.
 *
 * WHAT THIS SUITE IS FOR
 *   This is the phase where the machine starts running code the MODEL wrote, so
 *   the two questions that matter are "does a good document really work?" and
 *   "does a bad one fail safely and legibly?". Both are answered numerically here
 *   rather than by looking at a screenshot:
 *
 *     1. fixed-point numbers: parsing, formatting, the range limit
 *     2. the expression language: precedence, strings, every function, and the
 *        ERR value propagating instead of trapping (1/0, overflow, num("x"))
 *     3. the compiler's refusals, with the byte offset and the known-name list
 *     4. document validation: ~35 malformed, hostile and oversized documents,
 *        each asserted to leave NO window, NO app slot and a message that names
 *        the JSON path
 *     5. layout: grid cells that tile exactly, spans, explicit rects
 *     6. THE SHIPPED CALCULATOR (apps/examples/calculator.json), byte-identical
 *        to the artifact on disk, launched and then driven by CLICKING SCREEN
 *        PIXELS through the same dispatch path a PS/2 mouse uses:
 *        7+8=15, 123*2-46=200, 1/3, 1.5*4, 3-8, 9/0 latched, overflow, the
 *        twelve-digit cap
 *     7. instance limits and teardown, including the window being closed from
 *        underneath the app
 *     8. the `app` tool: every action, ~25 malformed inputs, exactly one trace
 *        line per call, and its schema cost against the shared budget
 *     9. THE GOLDEN TRANSCRIPT: chat_ask() driving the real turn loop against
 *        net/model_mock.c — format, a REJECTED document, a corrected one, four
 *        clicks, a state read-back and prose saying 15 — with no network and no
 *        API key (apps/transcripts/app_calculator.h)
 *
 *   Item 6 is the headline: "I want a calculator" reduced to an assertion, with
 *   the document coming from the shipped artifact and the clicks landing on
 *   pixels computed from the layout. Item 9 is the same thing with the model in
 *   the loop, as far as a machine with no API key can put it there.
 */

#include "harness.h"
#include "app.h"
#include "../../apps/app_impl.h"
#include "gui.h"
#include "widgets.h"
#include "fb.h"
#include "tool.h"
#include "trace.h"
#include "json.h"
#include "chat.h"
#include "model.h"

#include "../../apps/examples/calculator_json.h"
#include "../../apps/transcripts/app_calculator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* stand in for the linker-collected tool table                           */
/* ====================================================================== */

extern const tool_t *const fableos_hosttool_app_tool;
extern const tool_t *const fableos_hosttool_gui_list_tool;
extern const tool_t *const fableos_hosttool_gui_open_tool;
extern const tool_t *const fableos_hosttool_gui_window_tool;
extern const tool_t *const fableos_hosttool_gui_click_tool;

#if defined(__APPLE__)
#  define SYMPFX "_"
#  define TBL_SECTION __attribute__((section("__DATA,__data")))
#else
#  define SYMPFX ""
#  define TBL_SECTION
#endif

const tool_t *__start_tool_table[5] TBL_SECTION = { 0, 0, 0, 0, 0 };
extern const tool_t *const __stop_tool_table[];

__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 40\n");
_Static_assert(sizeof(__start_tool_table) == 40,
               "tool count changed: update the __stop_tool_table byte offset");

/* gui_click is linked in so the transcript can press the app's keys exactly as
 * the model would; the other GUI tools come with it in one file. */
static void install_tool_table(void) {
    __start_tool_table[0] = fableos_hosttool_app_tool;
    __start_tool_table[1] = fableos_hosttool_gui_list_tool;
    __start_tool_table[2] = fableos_hosttool_gui_open_tool;
    __start_tool_table[3] = fableos_hosttool_gui_window_tool;
    __start_tool_table[4] = fableos_hosttool_gui_click_tool;
}

/* ====================================================================== */
/* the fixture: a real framebuffer that is not on a screen                 */
/* ====================================================================== */

#define SW 1024
#define SH 768
#define GROWS (SH / 16)
#define GCOLS (SW / 8)
#define BLANK_CELL 0x0720u

static fb_color_t   *pixels;
static fb_surface_t  screen;
static uint16_t      cells[GROWS * GCOLS];

static void fixture(void) {
    if (!pixels) pixels = malloc(sizeof(fb_color_t) * SW * SH);
    app_close_all();
    gui_close_all();
    gui_detach();
    for (int i = 0; i < GROWS * GCOLS; i++) cells[i] = BLANK_CELL;
    for (int i = 0; i < SW * SH; i++) pixels[i] = 0xDEADBEEF;
    CHECK_EQ(fb_surface_init(&screen, pixels, SW, SH, SW), 0);
    gui_test_set_console_cursor(0, 0);
    CHECK_EQ(gui_attach(&screen, cells, GROWS, GCOLS), 0);
    gui_pointer_warp(SW - 4, SH - 4);
    kcap_reset();
}

/* ---- launching ---- */
static app_error_t g_err;

static int launch(const char *doc, uint32_t *id) {
    return app_launch(doc, 0, GUI_POS_AUTO, GUI_POS_AUTO, id, &g_err);
}

/* A document that must be refused: assert the code, the path, a phrase from the
 * reason, and that NOTHING was created. */
static void reject(const char *doc, const char *path, const char *phrase) {
    uint32_t id = 12345;
    int      apps_before = app_count(), wins_before = gui_window_count();
    int      rc = launch(doc, &id);

    CHECK(rc != APP_OK);
    CHECK_EQ(id, 0);
    CHECK_EQ(app_count(), apps_before);
    CHECK_EQ(gui_window_count(), wins_before);
    if (path) CHECK_STR(g_err.path, path);
    if (phrase) CHECK_CONTAINS(g_err.msg, phrase);
    if (rc == APP_OK)
        printf("    (document was accepted but should not have been: %.80s)\n",
               doc);
}

/* ---- clicking ---- */
static gui_widget_t *button(gui_window_t *w, const char *label) {
    for (uint16_t i = 0; i < w->nwidgets; i++)
        if (w->widgets[i].kind == GUI_BUTTON &&
            strcmp(w->widgets[i].text, label) == 0) return &w->widgets[i];
    return NULL;
}

/* Press a key by its visible text, at the SCREEN pixel its layout put it at, so
 * this goes window-hit -> widget-hit -> dispatch exactly as a mouse does. */
static int press(uint32_t id, const char *label) {
    gui_window_t *w = gui_window(id);
    CHECK(w != NULL);
    if (!w) return 0;
    gui_widget_t *b = button(w, label);
    CHECK(b != NULL);
    if (!b) return 0;
    gui_rect_t c = gui_client_rect(w);
    return gui_click_at(c.x + b->r.x + b->r.w / 2, c.y + b->r.y + b->r.h / 2);
}

static const char *display_of(uint32_t id) {
    gui_window_t *w = gui_window(id);
    if (!w) return "";
    for (uint16_t i = 0; i < w->nwidgets; i++)
        if (w->widgets[i].kind == GUI_FIELD) return w->widgets[i].text;
    return "";
}

/* Type a sequence of key labels: "7+8=" presses four keys. */
static void keys(uint32_t id, const char *seq) {
    char one[2] = { 0, 0 };
    for (size_t i = 0; seq[i]; i++) { one[0] = seq[i]; press(id, one); }
}

/* ====================================================================== */
/* 1. fixed-point numbers                                                 */
/* ====================================================================== */

static void test_number_parsing(void) {
    int64_t v = 0;

    CHECK_EQ(app_num_parse("0", &v), 0);        CHECK_EQ(v, 0);
    CHECK_EQ(app_num_parse("1", &v), 0);        CHECK_EQ(v, 1000000);
    CHECK_EQ(app_num_parse("-1", &v), 0);       CHECK_EQ(v, -1000000);
    CHECK_EQ(app_num_parse("+2", &v), 0);       CHECK_EQ(v, 2000000);
    CHECK_EQ(app_num_parse("1.5", &v), 0);      CHECK_EQ(v, 1500000);
    CHECK_EQ(app_num_parse("0.000001", &v), 0); CHECK_EQ(v, 1);
    CHECK_EQ(app_num_parse(" 12 ", &v), 0);     CHECK_EQ(v, 12000000);
    /* The seventh decimal is below the scale's resolution and is dropped, not
     * rounded — stated rather than discovered. */
    CHECK_EQ(app_num_parse("0.0000019", &v), 0); CHECK_EQ(v, 1);

    /* Everything a calculator must refuse rather than half-read. */
    CHECK(app_num_parse("", &v) != 0);
    CHECK(app_num_parse(".", &v) != 0);
    CHECK(app_num_parse("-", &v) != 0);
    CHECK(app_num_parse("12x", &v) != 0);
    CHECK(app_num_parse("1.2.3", &v) != 0);
    CHECK(app_num_parse("1e6", &v) != 0);
    CHECK(app_num_parse("0x10", &v) != 0);
    CHECK(app_num_parse("error", &v) != 0);
    CHECK(app_num_parse("99999999999", &v) != 0);   /* past APP_NUM_MAX */
    CHECK(app_num_parse(NULL, &v) != 0);
}

static void test_number_formatting(void) {
    char b[APP_TEXT_MAX];

    app_num_format(0, b, sizeof b);            CHECK_STR(b, "0");
    app_num_format(1000000, b, sizeof b);      CHECK_STR(b, "1");
    app_num_format(-1000000, b, sizeof b);     CHECK_STR(b, "-1");
    app_num_format(1500000, b, sizeof b);      CHECK_STR(b, "1.5");
    app_num_format(333333, b, sizeof b);       CHECK_STR(b, "0.333333");
    app_num_format(-500000, b, sizeof b);      CHECK_STR(b, "-0.5");
    app_num_format(APP_NUM_MAX, b, sizeof b);  CHECK_STR(b, "9000000000");
    app_num_format(1, b, sizeof b);            CHECK_STR(b, "0.000001");

    /* INT64_MIN: negating it is undefined, and a calculator is eventually handed
     * the extreme. It must produce text, not a crash. */
    app_num_format((int64_t)0x8000000000000000LL, b, sizeof b);
    CHECK(b[0] == '-');

    /* Round trip over every value the formatter can express exactly. */
    static const int64_t vals[] = { 0, 1, -1, 999999, 1000000, -1234567,
                                    APP_NUM_MAX, -APP_NUM_MAX, 500000 };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        int64_t back = 0;
        app_num_format(vals[i], b, sizeof b);
        CHECK_EQ(app_num_parse(b, &back), 0);
        CHECK_EQ(back, vals[i]);
    }
}

/* ====================================================================== */
/* 2 + 3. the expression language, on its own                              */
/* ====================================================================== */

static app_inst_t ex;

/* Two vars and one widget name, so name resolution has something to resolve. */
static void ex_reset(void) {
    memset(&ex, 0, sizeof ex);
    strcpy(ex.vname[0], "a");   ex.var[0] = app_val_num(3 * APP_NUM_SCALE);
    strcpy(ex.vname[1], "s");   ex.var[1] = app_val_str("12");
    ex.nvar = 2;
    strcpy(ex.wname[0], "disp");
    ex.nwidget = 1;
}

/* Compile and evaluate; `win` supplies the widget text when one is named. */
static app_val_t ev_win(const char *src, const app_val_t *key,
                        const gui_window_t *win) {
    app_error_t  err;
    uint16_t     idx = 0;
    memset(&err, 0, sizeof err);
    if (app_expr_compile(&ex, src, &idx, &err) != APP_OK) {
        printf("    FAIL compile \"%s\": %s\n", src, err.msg);
        th_fails++; th_checks++;
        return app_val_err();
    }
    return app_expr_eval(&ex, idx, key, win);
}

static app_val_t ev(const char *src) { return ev_win(src, NULL, NULL); }

/* The value as text — the form an app actually shows. */
static const char *evt(const char *src) {
    static char b[APP_TEXT_MAX];
    app_val_t   v = ev(src);
    app_val_text(&v, b, sizeof b);
    return b;
}

static void test_expr_arithmetic(void) {
    ex_reset();
    CHECK_STR(evt("1 + 2"), "3");
    CHECK_STR(evt("7-9"), "-2");
    CHECK_STR(evt("6 * 7"), "42");
    CHECK_STR(evt("1 / 3"), "0.333333");
    CHECK_STR(evt("1.5 * 4"), "6");
    CHECK_STR(evt("10 % 3"), "1");
    CHECK_STR(evt("-5 + 1"), "-4");
    CHECK_STR(evt("a"), "3");
    CHECK_STR(evt("a * a"), "9");
    CHECK_STR(evt("2 + 3 * 4"), "14");          /* precedence */
    CHECK_STR(evt("(2 + 3) * 4"), "20");
    CHECK_STR(evt("100 - 10 - 1"), "89");       /* left associative */
    CHECK_STR(evt("8 / 4 / 2"), "1");
    CHECK_STR(evt("!0"), "1");
    CHECK_STR(evt("!7"), "0");
    CHECK_STR(evt("- -4"), "4");
    CHECK_STR(evt("0.1 + 0.2"), "0.3");         /* exact: no binary floats */
}

static void test_expr_comparisons_and_logic(void) {
    ex_reset();
    CHECK_STR(evt("1 == 1"), "1");
    CHECK_STR(evt("1 == 2"), "0");
    CHECK_STR(evt("1 != 2"), "1");
    CHECK_STR(evt("1 < 2"), "1");
    CHECK_STR(evt("2 <= 2"), "1");
    CHECK_STR(evt("3 > 2"), "1");
    CHECK_STR(evt("3 >= 4"), "0");
    CHECK_STR(evt("'ab' == 'ab'"), "1");
    CHECK_STR(evt("'ab' == 'b'"), "0");
    CHECK_STR(evt("'a' < 'b'"), "1");
    CHECK_STR(evt("1 && 1"), "1");
    CHECK_STR(evt("1 && 0"), "0");
    CHECK_STR(evt("0 || 'x'"), "1");            /* a non-empty string is true */
    CHECK_STR(evt("0 || ''"), "0");
    CHECK_STR(evt("2 > 1 && 3 > 2"), "1");

    /* Types that cannot be ordered are an error value, but equality across kinds
     * is simply false: `op == ''` on a numeric var must not become an error. */
    CHECK_STR(evt("1 == 'x'"), "0");
    CHECK_STR(evt("1 != 'x'"), "1");
    CHECK_STR(evt("1 < 'x'"), "error");
    CHECK_STR(evt("s == '12'"), "1");
    CHECK_STR(evt("num(s) == 12"), "1");
}

static void test_expr_functions(void) {
    ex_reset();
    CHECK_STR(evt("num('42')"), "42");
    CHECK_STR(evt("num('4.25')"), "4.25");
    CHECK_STR(evt("num('nope')"), "error");
    CHECK_STR(evt("text(7)"), "7");
    CHECK_STR(evt("text(0.5)"), "0.5");
    CHECK_STR(evt("cat('1', '2')"), "12");
    CHECK_STR(evt("cat(s, '3')"), "123");
    CHECK_STR(evt("cat('n=', text(a))"), "n=3");
    CHECK_STR(evt("len('abc')"), "3");
    CHECK_STR(evt("len('')"), "0");
    CHECK_STR(evt("digits('12.5x')"), "3");
    CHECK_STR(evt("digits('')"), "0");
    CHECK_STR(evt("has('1.5', '.')"), "1");
    CHECK_STR(evt("has('15', '.')"), "0");
    CHECK_STR(evt("has('abc', '')"), "0");
    CHECK_STR(evt("iserr(1/0)"), "1");
    CHECK_STR(evt("iserr(1)"), "0");
    CHECK_STR(evt("abs(-4)"), "4");
    CHECK_STR(evt("min(3, 5)"), "3");
    CHECK_STR(evt("max(3, 5)"), "5");
    CHECK_STR(evt("min(-1, max(2, 3))"), "-1");

    /* A string in an arithmetic slot is an error rather than a coercion: a
     * calculator that adds "12" to 3 and gets 15 by accident is worse than one
     * that says so. */
    CHECK_STR(evt("s + 1"), "error");
    CHECK_STR(evt("cat('a', 1)"), "a1");        /* cat DOES render numbers */

    /* cat() past the widget text limit is an error, not a silent truncation. */
    CHECK_STR(evt("cat('123456789012345678901234567890123456789',"
                  "'12345678901234567890')"), "error");
}

static void test_expr_errors_are_values_not_traps(void) {
    ex_reset();
    CHECK_STR(evt("1 / 0"), "error");
    CHECK_STR(evt("1 % 0"), "error");
    CHECK_STR(evt("(1/0) + 1"), "error");       /* propagates */
    CHECK_STR(evt("(1/0) * 0"), "error");
    CHECK_STR(evt("text(1/0)"), "error");
    CHECK_STR(evt("iserr((1/0) - 5)"), "1");
    CHECK_STR(evt("!(1/0)"), "error");
    CHECK_STR(evt("(1/0) == 1"), "error");

    /* Overflow: never a wrapped number. */
    CHECK_STR(evt("9000000000 * 2"), "error");
    CHECK_STR(evt("9000000000 + 9000000000"), "error");
    CHECK_STR(evt("-9000000000 - 9000000000"), "error");
    CHECK_STR(evt("999999999 * 999999999"), "error");
    CHECK_STR(evt("9000000000 * 0.5"), "4500000000");   /* in range, exact */
    CHECK_STR(evt("1 / 0.000001"), "1000000");

    /* An error value is counted, which is how a tool can report that something
     * went wrong in a handler nobody was watching. */
    uint32_t before = app_stats()->err_values;
    (void)ev("1/0");
    CHECK(app_stats()->err_values > before);
}

static void test_expr_reads_widget_text(void) {
    uint32_t id = 0;
    fixture();
    /* One-widget app so there is a real gui_window_t to read text out of. */
    CHECK_EQ(launch("{\"title\":\"T\",\"widgets\":[{\"kind\":\"field\","
                    "\"name\":\"disp\",\"text\":\"41\",\"rect\":[0,0,80,20]}]}",
                    &id), APP_OK);
    gui_window_t *w = gui_window(id);
    ex_reset();

    app_val_t v = ev_win("num(disp) + 1", NULL, w);
    char b[APP_TEXT_MAX];
    CHECK_STR(app_val_text(&v, b, sizeof b), "42");

    v = ev_win("disp", NULL, w);
    CHECK_EQ(v.kind, AV_STR);
    CHECK_STR(v.str, "41");

    /* `key` is the text of the widget that fired; absent, it is empty. */
    app_val_t key = app_val_str("9");
    v = ev_win("cat(disp, key)", &key, w);
    CHECK_STR(app_val_text(&v, b, sizeof b), "419");
    v = ev_win("len(key)", NULL, w);
    CHECK_STR(app_val_text(&v, b, sizeof b), "0");

    /* No window at all: a widget read is an error value, never a wild pointer. */
    v = ev_win("disp", NULL, NULL);
    CHECK_EQ(v.kind, AV_ERR);
}

/* A compile that must fail, with the reason and the offset. */
static void bad_expr(const char *src, const char *phrase, int offset) {
    app_error_t err;
    uint16_t    idx = 0;
    memset(&err, 0, sizeof err);
    err.offset = -1;
    int rc = app_expr_compile(&ex, src, &idx, &err);
    CHECK(rc != APP_OK);
    if (phrase) CHECK_CONTAINS(err.msg, phrase);
    if (offset >= 0) CHECK_EQ(err.offset, offset);
}

static void test_expr_refusals_say_where(void) {
    ex_reset();
    bad_expr("", "empty", 0);
    bad_expr("1 +", "a value was expected", -1);
    bad_expr("(1 + 2", "expected ')'", -1);
    bad_expr("1 + 2)", "trailing text", 5);
    bad_expr("nope", "unknown name \"nope\"", 0);
    bad_expr("1 + nope", "unknown name", 4);
    bad_expr("nope(1)", "unknown function", 0);
    bad_expr("cat('a')", "cat() takes 2 arguments", -1);
    bad_expr("min(1)", "min() takes 2 arguments", -1);
    bad_expr("abs(1, 2)", "expected ')'", -1);
    bad_expr("'unterminated", "unterminated string", 0);
    bad_expr("1 $ 2", "trailing text", -1);
    bad_expr("@", "unexpected character", 0);
    bad_expr("99999999999", "not a number this machine can hold", 0);

    /* The known-name list is what turns a typo into a one-turn fix. */
    memset(&ex.stmt, 0, 0);
    app_error_t err;
    uint16_t    idx = 0;
    memset(&err, 0, sizeof err);
    CHECK(app_expr_compile(&ex, "displya", &idx, &err) != APP_OK);
    CHECK_CONTAINS(err.msg, "a, s, disp, key");
}

static void test_expr_bounds_are_refused_not_survived(void) {
    ex_reset();
    /* Nested parentheses past APP_PAREN_DEPTH, and stacked unary operators, are
     * the two ways to recurse in the compiler. Both are refused with a message
     * rather than by running out of stack. */
    char deep[256];
    size_t o = 0;
    for (int i = 0; i < 40; i++) deep[o++] = '(';
    deep[o++] = '1';
    for (int i = 0; i < 40; i++) deep[o++] = ')';
    deep[o] = '\0';
    bad_expr(deep, "nested parentheses", -1);

    for (o = 0; o < 40; o++) deep[o] = '!';
    deep[o++] = '1';
    deep[o] = '\0';
    bad_expr(deep, "unary", -1);

    /* The op pool is shared by the whole app: exhausting it is a refusal too. */
    ex_reset();
    char sum[APP_EXPR_SRC_MAX];
    o = 0;
    sum[o++] = '1';
    while (o + 4 < sizeof sum) { sum[o++] = '+'; sum[o++] = '1'; }
    sum[o] = '\0';
    int refusals = 0;
    for (int i = 0; i < 40; i++) {
        app_error_t err;
        uint16_t    idx = 0;
        memset(&err, 0, sizeof err);
        if (app_expr_compile(&ex, sum, &idx, &err) != APP_OK) {
            CHECK(err.msg[0] != '\0');
            refusals++;
            break;
        }
    }
    CHECK(refusals == 1);
    CHECK(ex.nop <= APP_MAX_OPS);
}

/* ====================================================================== */
/* 4. document validation                                                 */
/* ====================================================================== */

/* The smallest document that works, as a base for mutations. */
#define MIN_DOC "{\"title\":\"T\",\"width\":120,\"height\":80," \
                "\"grid\":{\"rows\":1,\"cols\":1}," \
                "\"widgets\":[{\"kind\":\"button\",\"text\":\"go\"," \
                "\"name\":\"b\",\"row\":0,\"col\":0}]}"

static void test_minimal_document_launches(void) {
    uint32_t id = 0;
    fixture();
    CHECK_EQ(launch(MIN_DOC, &id), APP_OK);
    CHECK(id != 0);
    CHECK_EQ(app_count(), 1);
    CHECK_EQ(gui_window_count(), 1);
    CHECK_EQ(app_is_app(id), 1);
    CHECK_STR(app_title(id), "T");
    CHECK_EQ(gui_focused(), id);

    gui_window_t *w = gui_window(id);
    CHECK_EQ(w->nwidgets, 1);
    CHECK_EQ(w->widgets[0].kind, GUI_BUTTON);
    CHECK_STR(w->widgets[0].text, "go");
    CHECK_EQ(w->widgets[0].id, 1u);             /* index + 1 */
    /* A document with no "on" is a legal static picture: clicking does nothing
     * and nothing crashes. */
    press(id, "go");
    CHECK_EQ(app_count(), 1);
}

static void test_hostile_documents_are_refused(void) {
    fixture();

    /* ---- not a document at all ---- */
    reject("", NULL, "not valid JSON");
    reject("   ", NULL, "not valid JSON");
    reject("nonsense", NULL, "not valid JSON");
    reject("{", NULL, "not valid JSON");
    reject("{\"widgets\":[", NULL, "not valid JSON");
    reject("[]", NULL, "must be a JSON object");
    reject("42", NULL, "must be a JSON object");
    reject("\"a string\"", NULL, "must be a JSON object");
    reject("null", NULL, "must be a JSON object");
    reject("{\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"row\":0,"
           "\"col\":0}],}", NULL, "not valid JSON");

    /* ---- top-level shape ---- */
    reject("{}", "widgets", "required");
    reject("{\"widgets\":{}}", "widgets", "required");
    reject("{\"widgets\":[]}", "widgets", "empty window");
    reject("{\"widgts\":[]}", "", "unknown key \"widgts\"");
    reject("{\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"rect\":"
           "[0,0,9,9]}],\"hight\":100}", "", "unknown key \"hight\"");
    reject("{\"title\":7,\"widgets\":[]}", "title", "must be a string");
    reject("{\"width\":\"wide\",\"widgets\":[]}", "width", "must be an integer");
    reject("{\"width\":0,\"widgets\":[]}", "width", "between 32 and 4096");
    reject("{\"width\":999999,\"widgets\":[]}", "width", "between 32 and 4096");
    reject("{\"height\":1.5,\"widgets\":[]}", "height", "must be an integer");

    /* ---- grid ---- */
    reject("{\"grid\":5,\"widgets\":[]}", "grid", "must be an object");
    reject("{\"grid\":{\"cols\":2},\"widgets\":[]}", "grid.rows", "required");
    reject("{\"grid\":{\"rows\":2},\"widgets\":[]}", "grid.cols", "required");
    reject("{\"grid\":{\"rows\":0,\"cols\":1},\"widgets\":[]}", "grid.rows",
           "1 to 64");
    reject("{\"grid\":{\"rows\":1,\"cols\":1,\"gaps\":2},\"widgets\":[]}",
           "grid", "unknown key \"gaps\"");

    /* ---- widgets ---- */
    reject("{\"widgets\":[3]}", "widgets[0]", "must be a JSON object");
    reject("{\"widgets\":[{}]}", "widgets[0].kind", "one of: button");
    reject("{\"widgets\":[{\"kind\":\"buton\",\"row\":0,\"col\":0}]}",
           "widgets[0].kind", "one of: button");
    reject("{\"widgets\":[{\"kind\":\"button\",\"colSpan\":2,\"row\":0,"
           "\"col\":0}]}", "widgets[0]", "unknown key \"colSpan\"");
    reject("{\"widgets\":[{\"kind\":\"button\"}]}", "widgets[0].row",
           "required");
    reject("{\"widgets\":[{\"kind\":\"button\",\"row\":0}]}",
           "widgets[0].col", "required");
    reject("{\"widgets\":[{\"kind\":\"button\",\"row\":0,\"col\":0}]}",
           "widgets[0]", "needs \"grid\"");
    reject("{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":\"button\","
           "\"row\":1,\"col\":0}]}", "widgets[0]", "does not fit the 1x1 grid");
    reject("{\"grid\":{\"rows\":2,\"cols\":2},\"widgets\":[{\"kind\":\"button\","
           "\"row\":0,\"col\":0,\"colspan\":3}]}", "widgets[0]",
           "does not fit the 2x2 grid");
    reject("{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":\"button\","
           "\"row\":0,\"col\":0,\"rowspan\":0}]}", "widgets[0].rowspan",
           "positive");
    reject("{\"widgets\":[{\"kind\":\"button\",\"rect\":[0,0]}]}",
           "widgets[0].rect", "[x,y,width,height]");
    reject("{\"widgets\":[{\"kind\":\"button\",\"rect\":[0,0,0,10]}]}",
           "widgets[0].rect", "positive");
    reject("{\"widgets\":[{\"kind\":\"button\",\"rect\":[0,0,10,999999]}]}",
           "widgets[0].rect", "within +/-4096");
    reject("{\"widgets\":[{\"kind\":\"button\",\"rect\":\"big\"}]}",
           "widgets[0].rect", "[x,y,width,height]");
    reject("{\"widgets\":[{\"kind\":\"button\",\"align\":\"middle\",\"rect\":"
           "[0,0,9,9]}]}", "widgets[0].align", "left, center or right");
    reject("{\"widgets\":[{\"kind\":\"button\",\"readonly\":\"yes\",\"rect\":"
           "[0,0,9,9]}]}", "widgets[0]", "must be a boolean");
    reject("{\"widgets\":[{\"kind\":\"button\",\"name\":\"9lives\",\"rect\":"
           "[0,0,9,9]}]}", "widgets[0].name", "not a usable name");
    reject("{\"widgets\":[{\"kind\":\"button\",\"name\":\"key\",\"rect\":"
           "[0,0,9,9]}]}", "widgets[0].name", "reserved word");
    reject("{\"widgets\":[{\"kind\":\"button\",\"name\":\"text\",\"rect\":"
           "[0,0,9,9]}]}", "widgets[0].name", "reserved word");
    reject("{\"widgets\":[{\"kind\":\"button\",\"name\":\"aVeryLongWidgetName\","
           "\"rect\":[0,0,9,9]}]}", "widgets[0].name", "longer than");
    /* Two widgets with one name: an expression would be ambiguous. */
    reject("{\"widgets\":[{\"kind\":\"button\",\"name\":\"b\",\"rect\":"
           "[0,0,9,9]},{\"kind\":\"button\",\"name\":\"b\",\"rect\":"
           "[0,9,9,9]}]}", "widgets[1].name", "already the name");
    /* A var and a widget with one name: same reason. */
    reject("{\"vars\":{\"b\":0},\"widgets\":[{\"kind\":\"button\",\"name\":\"b\","
           "\"rect\":[0,0,9,9]}]}", "widgets[0].name", "already the name");

    /* ---- vars ---- */
    reject("{\"vars\":[],\"widgets\":[]}", "vars", "name -> initial value");
    reject("{\"vars\":{\"a\":{}},\"widgets\":[]}", "vars.a",
           "number, a string or a boolean");
    reject("{\"vars\":{\"a\":[]},\"widgets\":[]}", "vars.a", "must be a number");
    reject("{\"vars\":{\"9\":0},\"widgets\":[]}", "vars.9", "not a usable name");
    reject("{\"vars\":{\"iserr\":0},\"widgets\":[]}", "vars.iserr",
           "reserved word");
    reject("{\"vars\":{\"a\":99999999999},\"widgets\":[]}", "vars.a",
           "outside what this machine can hold");
    /* A NUL inside a KEY: without a check, this would declare a var named "a"
     * that no expression in the document could ever name. */
    reject("{\"vars\":{\"a\\u0000b\":0},\"widgets\":[]}", "vars",
           "embedded NUL");

    /* ---- handlers ---- */
    reject("{\"on\":{},\"widgets\":[{\"kind\":\"button\",\"rect\":[0,0,9,9]}]}",
           "on", "array of handlers");
    reject("{\"on\":[7],\"widgets\":[{\"kind\":\"button\",\"rect\":[0,0,9,9]}]}",
           "on[0]", "must be a JSON object");
    reject("{\"on\":[{}],\"widgets\":[{\"kind\":\"button\",\"rect\":[0,0,9,9]}]}",
           "on[0]", "needs an event key");
    reject("{\"on\":[{\"onclick\":\"b\",\"do\":[]}],\"widgets\":[{\"kind\":"
           "\"button\",\"name\":\"b\",\"rect\":[0,0,9,9]}]}", "on[0]",
           "unknown key \"onclick\"");
    reject("{\"on\":[{\"click\":\"b\",\"submit\":\"b\",\"do\":[]}],\"widgets\":"
           "[{\"kind\":\"button\",\"name\":\"b\",\"rect\":[0,0,9,9]}]}", "on[0]",
           "exactly one event");
    reject("{\"on\":[{\"click\":\"b\"}],\"widgets\":[{\"kind\":\"button\","
           "\"name\":\"b\",\"rect\":[0,0,9,9]}]}", "on[0].do", "required");
    reject("{\"on\":[{\"click\":\"nope\",\"do\":[]}],\"widgets\":[{\"kind\":"
           "\"button\",\"name\":\"b\",\"tag\":\"t\",\"rect\":[0,0,9,9]}]}",
           "on[0].click", "no widget has the name or tag \"nope\"");
    /* ...and the message lists what DOES exist, which is the whole point. */
    CHECK_CONTAINS(g_err.msg, "Available: b");
    reject("{\"on\":[{\"click\":7,\"do\":[]}],\"widgets\":[{\"kind\":\"button\","
           "\"name\":\"b\",\"rect\":[0,0,9,9]}]}", "on[0].click",
           "name\", a \"tag\"");
    reject("{\"on\":[{\"click\":[],\"do\":[]}],\"widgets\":[{\"kind\":\"button\","
           "\"name\":\"b\",\"rect\":[0,0,9,9]}]}", "on[0].click",
           "between 1 and");

    /* ---- statements ---- */
#define WITH_DO(stmts) "{\"widgets\":[{\"kind\":\"field\",\"name\":\"d\"," \
        "\"rect\":[0,0,90,20]},{\"kind\":\"button\",\"name\":\"b\"," \
        "\"text\":\"x\",\"rect\":[0,20,90,20]}]," \
        "\"vars\":{\"n\":0},\"on\":[{\"click\":\"b\",\"do\":[" stmts "]}]}"

    reject(WITH_DO("3"), "on[0].do[0]", "must be a JSON object");
    reject(WITH_DO("{}"), "on[0].do[0]", "no known verb");
    reject(WITH_DO("{\"sett\":\"n\",\"to\":\"1\"}"), "on[0].do[0]",
           "unknown key \"sett\"");
    reject(WITH_DO("{\"set\":\"n\",\"to\":\"1\",\"if\":\"1\"}"), "on[0].do[0]",
           "exactly one verb");
    reject(WITH_DO("{\"set\":\"n\"}"), "on[0].do[0].to", "missing");
    reject(WITH_DO("{\"set\":\"n\",\"to\":1}"), "on[0].do[0].to",
           "must be a STRING");
    reject(WITH_DO("{\"set\":\"nope\",\"to\":\"1\"}"), "on[0].do[0].set",
           "not a var or a named widget");
    reject(WITH_DO("{\"set\":7,\"to\":\"1\"}"), "on[0].do[0].set",
           "name of a var");
    reject(WITH_DO("{\"set\":\"n\",\"to\":\"1 +\"}"), "on[0].do[0].to",
           "a value was expected");
    reject(WITH_DO("{\"set\":\"n\",\"to\":\"nope\"}"), "on[0].do[0].to",
           "unknown name");
    /* The offset into the expression is reported, and quoted back. */
    CHECK_CONTAINS(g_err.msg, "at offset 0 of \"nope\"");
    reject(WITH_DO("{\"if\":\"1\"}"), "on[0].do[0].then", "needs \"then\"");
    reject(WITH_DO("{\"if\":\"1\",\"then\":{}}"), "on[0].do[0].then",
           "needs \"then\"");
    reject(WITH_DO("{\"if\":\"1\",\"then\":[],\"else\":{}}"),
           "on[0].do[0].else", "must be an array");
    reject(WITH_DO("{\"if\":\"1\",\"then\":[{\"set\":\"n\",\"to\":\"bad\"}]}"),
           "on[0].do[0].then[0].to", "unknown name");
    reject(WITH_DO("{\"stop\":false}"), "on[0].do[0]", "must be the boolean");
    reject(WITH_DO("{\"stop\":1}"), "on[0].do[0]", "must be the boolean");
}

static void test_oversized_and_deep_documents(void) {
    static char big[APP_DOC_MAX * 2];
    fixture();

    /* Over APP_DOC_MAX: refused by size, before the parser is even asked. */
    size_t o = 0;
    o += (size_t)snprintf(big + o, sizeof big - o,
                          "{\"title\":\"x\",\"widgets\":[{\"kind\":\"button\","
                          "\"text\":\"a\",\"rect\":[0,0,9,9]}],\"vars\":{");
    for (int i = 0; o + 40 < APP_DOC_MAX + 64; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "%s\"v%d\":123456", i ? "," : "", i);
    o += (size_t)snprintf(big + o, sizeof big - o, "}}");
    reject(big, "", "the limit is 8192");

    /* Deeply nested JSON: json.h's own depth limit, surfaced as a document
     * error rather than a stack overflow. */
    o = 0;
    for (int i = 0; i < 200; i++) big[o++] = '[';
    for (int i = 0; i < 200; i++) big[o++] = ']';
    big[o] = '\0';
    reject(big, NULL, "not valid JSON");

    /* A 10,000-character widget text. */
    o = (size_t)snprintf(big, sizeof big,
                         "{\"widgets\":[{\"kind\":\"label\",\"rect\":[0,0,9,9],"
                         "\"text\":\"");
    for (int i = 0; i < 4000; i++) big[o++] = 'A';
    o += (size_t)snprintf(big + o, sizeof big - o, "\"}]}");
    reject(big, "widgets[0].text", "longer than 39 bytes");

    /* An embedded NUL inside a name the runtime compares as a C string. */
    reject("{\"widgets\":[{\"kind\":\"button\",\"rect\":[0,0,9,9],\"name\":"
           "\"b\\u0000x\"}]}", "widgets[0].name", "embedded NUL");

    /* Too many widgets, vars, handlers, statements — each named. */
    o = (size_t)snprintf(big, sizeof big, "{\"widgets\":[");
    for (int i = 0; i < APP_MAX_WIDGETS + 4; i++)
        o += (size_t)snprintf(big + o, sizeof big - o,
                              "%s{\"kind\":\"button\",\"rect\":[0,%d,20,8]}",
                              i ? "," : "", i * 8);
    o += (size_t)snprintf(big + o, sizeof big - o, "]}");
    reject(big, "widgets", "a window holds at most 48");

    o = (size_t)snprintf(big, sizeof big, "{\"widgets\":[{\"kind\":\"button\","
                                          "\"rect\":[0,0,9,9]}],\"vars\":{");
    for (int i = 0; i < APP_MAX_VARS + 2; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "%s\"v%d\":0", i ? "," : "", i);
    o += (size_t)snprintf(big + o, sizeof big - o, "}}");
    reject(big, "vars", "the limit is 16");

    o = (size_t)snprintf(big, sizeof big, "{\"widgets\":[{\"kind\":\"button\","
                                          "\"name\":\"b\",\"rect\":[0,0,9,9]}],"
                                          "\"on\":[");
    for (int i = 0; i < APP_MAX_HANDLERS + 2; i++)
        o += (size_t)snprintf(big + o, sizeof big - o,
                              "%s{\"click\":\"b\",\"do\":[]}", i ? "," : "");
    o += (size_t)snprintf(big + o, sizeof big - o, "]}");
    reject(big, "on", "the limit is 16");

    /* "if" nested past APP_MAX_IF_DEPTH. */
    o = (size_t)snprintf(big, sizeof big,
                         "{\"vars\":{\"n\":0},\"widgets\":[{\"kind\":\"button\","
                         "\"name\":\"b\",\"rect\":[0,0,9,9]}],"
                         "\"on\":[{\"click\":\"b\",\"do\":[");
    int depth = APP_MAX_IF_DEPTH + 3;
    for (int i = 0; i < depth; i++)
        o += (size_t)snprintf(big + o, sizeof big - o,
                              "{\"if\":\"1\",\"then\":[");
    o += (size_t)snprintf(big + o, sizeof big - o, "{\"stop\":true}");
    for (int i = 0; i < depth; i++)
        o += (size_t)snprintf(big + o, sizeof big - o, "]}");
    o += (size_t)snprintf(big + o, sizeof big - o, "]}]}");
    {
        uint32_t id = 12345;
        CHECK(launch(big, &id) != APP_OK);
        CHECK_EQ(id, 0);
        CHECK_CONTAINS(g_err.msg, "nested more than 8");
    }

    /* Nothing survived any of that. */
    CHECK_EQ(app_count(), 0);
    CHECK_EQ(gui_window_count(), 0);
    CHECK_EQ(kpanic_hit, 0);
}

/* ====================================================================== */
/* 5. layout                                                              */
/* ====================================================================== */

static void test_grid_layout_tiles_and_spans(void) {
    uint32_t id = 0;
    fixture();
    CHECK_EQ(launch(
        "{\"title\":\"L\",\"width\":200,\"height\":200,"
        "\"grid\":{\"rows\":2,\"cols\":3,\"gap\":4,\"pad\":6},"
        "\"widgets\":["
        "{\"kind\":\"button\",\"text\":\"a\",\"row\":0,\"col\":0},"
        "{\"kind\":\"button\",\"text\":\"b\",\"row\":0,\"col\":1},"
        "{\"kind\":\"button\",\"text\":\"c\",\"row\":0,\"col\":2},"
        "{\"kind\":\"button\",\"text\":\"wide\",\"row\":1,\"col\":0,"
        "\"colspan\":3},"
        "{\"kind\":\"label\",\"text\":\"fixed\",\"rect\":[3,3,40,12]}]}",
        &id), APP_OK);

    gui_window_t *w = gui_window(id);
    CHECK_EQ(w->nwidgets, 5);
    gui_widget_t *a = &w->widgets[0], *b = &w->widgets[1], *c = &w->widgets[2];
    gui_widget_t *wide = &w->widgets[3], *fixed = &w->widgets[4];

    /* Adjacent cells share an edge exactly, with the gap between them. */
    CHECK_EQ(a->r.x + a->r.w + 4, b->r.x);
    CHECK_EQ(b->r.x + b->r.w + 4, c->r.x);
    /* Widths differ by at most one pixel. */
    CHECK(abs((int)a->r.w - (int)b->r.w) <= 1);
    CHECK(abs((int)b->r.w - (int)c->r.w) <= 1);
    /* A span is the union of its cells plus the gaps between them. */
    CHECK_EQ(wide->r.x, a->r.x);
    CHECK_EQ(wide->r.x + wide->r.w, c->r.x + c->r.w);
    /* The rect escape hatch is honoured verbatim, in client coordinates. */
    CHECK_EQ(fixed->r.x, 3); CHECK_EQ(fixed->r.y, 3);
    CHECK_EQ(fixed->r.w, 40); CHECK_EQ(fixed->r.h, 12);

    /* Kinds and their default states. */
    CHECK_EQ(fixed->kind, GUI_LABEL);
    CHECK(fixed->state & GUI_W_NOHIT);          /* labels are scenery */
}

static void test_readonly_field_is_not_clickable(void) {
    uint32_t id = 0;
    fixture();
    /* A readout must not be hit-testable: gui_click resolves widgets by their
     * visible text, and a display reading "7" would otherwise be ambiguous with
     * the "7" key. This is enforced by the runtime, not by the document. */
    CHECK_EQ(launch("{\"widgets\":["
                    "{\"kind\":\"field\",\"name\":\"d\",\"text\":\"7\","
                    "\"readonly\":true,\"rect\":[0,0,80,20]},"
                    "{\"kind\":\"field\",\"name\":\"e\",\"text\":\"e\","
                    "\"rect\":[0,20,80,20]}]}", &id), APP_OK);
    gui_window_t *w = gui_window(id);
    CHECK(w->widgets[0].state & GUI_W_READONLY);
    CHECK(w->widgets[0].state & GUI_W_NOHIT);
    CHECK(!(w->widgets[1].state & GUI_W_NOHIT));
}

/* ...unless the document binds a handler to it. A label or a read-only field is
 * scenery by default, and an app whose handler simply never fires is the most
 * confusing thing a model could be handed, so an explicit handler wins. */
static void test_a_handler_makes_its_widget_clickable(void) {
    uint32_t id = 0;
    fixture();
    CHECK_EQ(launch(
        "{\"vars\":{\"n\":0},\"widgets\":["
        "{\"kind\":\"label\",\"name\":\"lab\",\"text\":\"press me\","
        "\"rect\":[0,0,120,20]},"
        "{\"kind\":\"label\",\"name\":\"out\",\"text\":\"0\","
        "\"rect\":[0,20,120,20]},"
        "{\"kind\":\"field\",\"name\":\"ro\",\"text\":\"x\",\"readonly\":true,"
        "\"rect\":[0,40,120,20]}],"
        "\"on\":[{\"click\":\"lab\",\"do\":[{\"set\":\"n\",\"to\":\"n + 1\"},"
        "{\"set\":\"out\",\"to\":\"text(n)\"}]}]}", &id), APP_OK);

    gui_window_t *w = gui_window(id);
    CHECK(!(w->widgets[0].state & GUI_W_NOHIT));   /* has a handler          */
    CHECK(w->widgets[1].state & GUI_W_NOHIT);      /* scenery, as declared   */
    CHECK(w->widgets[2].state & GUI_W_NOHIT);      /* read-only readout      */

    gui_rect_t c = gui_client_rect(w);
    gui_click_at(c.x + 10, c.y + 10);
    CHECK_STR(gui_text(&w->widgets[1]), "1");
    gui_click_at(c.x + 10, c.y + 10);
    CHECK_STR(gui_text(&w->widgets[1]), "2");
    /* Clicking the scenery does nothing at all. */
    gui_click_at(c.x + 10, c.y + 30);
    CHECK_STR(gui_text(&w->widgets[1]), "2");
}

/* ====================================================================== */
/* 6. THE SHIPPED CALCULATOR                                              */
/* ====================================================================== */

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf || n < 0 || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    buf[n] = '\0';
    *len = (size_t)n;
    fclose(f);
    return buf;
}

static void test_artifact_matches_the_compiled_copy(void) {
    /* `make test-host` runs from ; the second path is for running the binary
     * straight out of tests/build/. */
    static const char *paths[] = {
        "apps/examples/calculator.json",
        "../../apps/examples/calculator.json",
    };
    size_t len = 0;
    char  *disk = NULL;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0] && !disk; i++)
        disk = slurp(paths[i], &len);
    if (!disk) {
        printf("    FAIL cannot open apps/examples/calculator.json (run the "
               "suite from , as `make test-host` does)\n");
        th_fails++; th_checks++;
        return;
    }
    const char *baked = app_example_calculator();
    CHECK_EQ(len, strlen(baked));
    if (len == strlen(baked)) CHECK_EQ(memcmp(disk, baked, len), 0);
    else printf("    (run: python3 apps/examples/gen_header.py)\n");

    /* And it is a document, not just a file: json.h validates the whole thing. */
    json_value_t v;
    CHECK_EQ(json_parse(baked, strlen(baked), &v), JSON_OK);
    CHECK_EQ(v.type, JSON_OBJECT);
    free(disk);
}

static uint32_t launch_calculator_at(int32_t x, int32_t y) {
    uint32_t id = 0;
    int      rc = app_launch(app_example_calculator(), 0, x, y, &id, &g_err);
    CHECK_EQ(rc, APP_OK);
    if (rc != APP_OK) printf("    (%s: %s)\n", g_err.path, g_err.msg);
    return id;
}

static uint32_t launch_calculator(void) {
    return launch_calculator_at(GUI_POS_AUTO, GUI_POS_AUTO);
}

static void test_shipped_calculator_launches(void) {
    fixture();
    uint32_t id = launch_calculator();
    CHECK(id != 0);
    CHECK_STR(app_title(id), "Calculator");

    gui_window_t *w = gui_window(id);
    CHECK(w != NULL);
    if (!w) return;
    CHECK_EQ(w->nwidgets, 18);                  /* display + 17 keys */
    CHECK_EQ(w->frame.w, 216);
    CHECK_EQ(w->frame.h, 266);
    CHECK_STR(display_of(id), "0");
    CHECK_EQ(app_var_count(id), 6);

    /* Every key the operator expects, present and clickable. */
    static const char *labels[] = { "0","1","2","3","4","5","6","7","8","9",
                                    "+","-","*","/",".","C","=" };
    for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++)
        CHECK(button(w, labels[i]) != NULL);

    /* "=" spans the whole bottom row, and the display spans the top. */
    gui_widget_t *eq = button(w, "=");
    gui_widget_t *zero = button(w, "0");
    gui_widget_t *plus = button(w, "+");
    CHECK(eq && zero && plus);
    if (eq && zero && plus) {
        CHECK_EQ(eq->r.x, zero->r.x);
        CHECK_EQ(eq->r.x + eq->r.w, plus->r.x + plus->r.w);
    }
    CHECK(w->widgets[0].state & GUI_W_NOHIT);   /* the display is scenery */
}

/* THE DEMO AS AN ASSERTION. Every one of these presses is a click at a screen
 * pixel derived from the layout, dispatched through the same window-hit ->
 * widget-hit -> handler path a PS/2 mouse produces. */
static void test_calculator_arithmetic_by_clicking(void) {
    fixture();
    uint32_t id = launch_calculator();
    if (!id) return;

    keys(id, "7+8=");
    CHECK_STR(display_of(id), "15");            /* the headline */

    keys(id, "C");
    CHECK_STR(display_of(id), "0");

    /* A running total without pressing = each time. */
    keys(id, "C123*2-46=");
    CHECK_STR(display_of(id), "200");

    keys(id, "C1/3=");
    CHECK_STR(display_of(id), "0.333333");

    keys(id, "C1.5*4=");
    CHECK_STR(display_of(id), "6");

    keys(id, "C3-8=");
    CHECK_STR(display_of(id), "-5");

    /* Two decimal points are impossible. */
    keys(id, "C1..5");
    CHECK_STR(display_of(id), "1.5");

    /* Division by zero: the word, latched — only C clears it. */
    keys(id, "C9/0=");
    CHECK_STR(display_of(id), "error");
    keys(id, "5");
    CHECK_STR(display_of(id), "error");
    keys(id, "+2=");
    CHECK_STR(display_of(id), "error");
    keys(id, "C");
    CHECK_STR(display_of(id), "0");

    /* Overflow: never a wrapped number. */
    keys(id, "C999999999*999999999=");
    CHECK_STR(display_of(id), "error");
    keys(id, "C");

    /* The twelve-digit cap holds however long you lean on the keypad. */
    keys(id, "C");
    for (int i = 0; i < 40; i++) press(id, "9");
    CHECK_EQ((int)strlen(display_of(id)), 12);

    /* A leading zero is replaced, not accumulated. */
    keys(id, "C007");
    CHECK_STR(display_of(id), "7");

    /* Repeated = does not re-apply the operator. */
    keys(id, "C2+3===");
    CHECK_STR(display_of(id), "5");

    /* Nothing above panicked, and the app is still healthy. */
    CHECK_EQ(kpanic_hit, 0);
    CHECK_EQ(app_count(), 1);
    CHECK_EQ(app_is_app(id), 1);
}

static void test_calculator_pixels_change_when_a_key_is_pressed(void) {
    fixture();
    uint32_t id = launch_calculator();
    if (!id) return;
    gui_flush();

    /* The display area, before and after: a click must actually reach the
     * screen, not only the widget's text field. */
    gui_window_t *w = gui_window(id);
    gui_rect_t    c = gui_client_rect(w);
    gui_widget_t *d = &w->widgets[0];
    int32_t       sx = c.x + d->r.x + d->r.w - 8, sy = c.y + d->r.y + d->r.h / 2;

    fb_color_t before[16];
    for (int i = 0; i < 16; i++) before[i] = fb_get_pixel(&screen, sx + i - 8, sy);

    press(id, "7");
    gui_flush();

    int changed = 0;
    for (int i = 0; i < 16; i++)
        if (fb_get_pixel(&screen, sx + i - 8, sy) != before[i]) changed = 1;
    CHECK(changed);
    CHECK_STR(display_of(id), "7");
}

/* ====================================================================== */
/* 7. instances and teardown                                              */
/* ====================================================================== */

static void test_instance_pool_and_teardown(void) {
    uint32_t ids[APP_MAX_INSTANCES + 1];
    fixture();

    for (int i = 0; i < APP_MAX_INSTANCES; i++) {
        ids[i] = 0;
        CHECK_EQ(launch(MIN_DOC, &ids[i]), APP_OK);
    }
    CHECK_EQ(app_count(), APP_MAX_INSTANCES);

    /* One more is refused, and the message names the running ids so the model
     * can close one. */
    uint32_t extra = 12345;
    CHECK_EQ(launch(MIN_DOC, &extra), APP_EBUSY);
    CHECK_EQ(extra, 0);
    CHECK_CONTAINS(g_err.msg, "already running");
    CHECK_EQ(gui_window_count(), APP_MAX_INSTANCES);

    /* Closing frees the slot, and the window with it. */
    CHECK_EQ(app_close(ids[0]), APP_OK);
    CHECK_EQ(app_count(), APP_MAX_INSTANCES - 1);
    CHECK_EQ(gui_window_count(), APP_MAX_INSTANCES - 1);
    CHECK_EQ(app_is_app(ids[0]), 0);
    CHECK_EQ(app_close(ids[0]), APP_EINVAL);    /* and stays closed */
    CHECK_STR(app_title(ids[0]), "");

    /* The slot is reusable, so a machine can run apps all day. */
    CHECK_EQ(launch(MIN_DOC, &extra), APP_OK);
    CHECK(extra != 0);
    CHECK_EQ(app_count(), APP_MAX_INSTANCES);

    /* The window closed from underneath the app (the close box, gui_close_all)
     * must release the slot too: there is one teardown path. */
    gui_close_all();
    CHECK_EQ(gui_window_count(), 0);
    CHECK_EQ(app_count(), 0);

    /* Many launch/close cycles must not leak a slot. */
    for (int i = 0; i < APP_MAX_INSTANCES * 5; i++) {
        uint32_t id = 0;
        CHECK_EQ(launch(MIN_DOC, &id), APP_OK);
        CHECK_EQ(app_close(id), APP_OK);
    }
    CHECK_EQ(app_count(), 0);
    CHECK_EQ(gui_window_count(), 0);
}

static void test_handler_state_is_per_instance(void) {
    uint32_t a = 0, b = 0;
    fixture();
    /* Placed apart on purpose: a click goes to the TOPMOST window at that pixel
     * (which is the correct behaviour), so two cascaded calculators would have
     * one of them swallowing the other's keys. */
    a = launch_calculator_at(20, 20);
    b = launch_calculator_at(500, 400);
    CHECK(a && b && a != b);

    keys(a, "7+8=");
    keys(b, "2");
    CHECK_STR(display_of(a), "15");
    CHECK_STR(display_of(b), "2");

    /* Closing one leaves the other's state untouched. */
    CHECK_EQ(app_close(a), APP_OK);
    CHECK_STR(display_of(b), "2");
    keys(b, "+3=");
    CHECK_STR(display_of(b), "5");
}

static void test_step_budget_stops_a_runaway_handler(void) {
    static char big[APP_DOC_MAX];
    uint32_t    id = 0;
    size_t      o;
    fixture();

    /* There is no loop in the language, so the only way to spend the step budget
     * is length. Build a handler of many statements and prove it is bounded and
     * that the app survives being cut off. */
    o = (size_t)snprintf(big, sizeof big,
                         "{\"vars\":{\"n\":0},\"widgets\":["
                         "{\"kind\":\"field\",\"name\":\"d\",\"rect\":"
                         "[0,0,90,20]},"
                         "{\"kind\":\"button\",\"name\":\"b\",\"text\":\"go\","
                         "\"rect\":[0,20,90,20]}],"
                         "\"on\":[{\"click\":\"b\",\"do\":[");
    for (int i = 0; i < APP_MAX_STMTS + 1; i++)
        o += (size_t)snprintf(big + o, sizeof big - o,
                              "%s{\"stop\":true}", i ? "," : "");
    o += (size_t)snprintf(big + o, sizeof big - o, "]}]}");
    /* One statement over the pool. `stop` is used rather than `set` because a
     * `set` also costs an expression, and the expression pool (96) is reached
     * first — which is a different, equally-named refusal, asserted below. */
    reject(big, "on[0].do", "more than 128 statements");

    /* At the limit it runs, and the counter proves every statement executed. */
    o = (size_t)snprintf(big, sizeof big,
                         "{\"vars\":{\"n\":0},\"widgets\":["
                         "{\"kind\":\"field\",\"name\":\"d\",\"rect\":"
                         "[0,0,90,20]},"
                         "{\"kind\":\"button\",\"name\":\"b\",\"text\":\"go\","
                         "\"rect\":[0,20,90,20]}],"
                         "\"on\":[{\"click\":\"b\",\"do\":[");
    for (int i = 0; i < APP_MAX_EXPRS; i++)
        o += (size_t)snprintf(big + o, sizeof big - o,
                              "%s{\"set\":\"n\",\"to\":\"n + 1\"}", i ? "," : "");
    o += (size_t)snprintf(big + o, sizeof big - o,
                          ",{\"set\":\"d\",\"to\":\"text(n)\"}]}]}");
    /* One expression over that pool: also refused, also by name. */
    reject(big, "on[0].do[96].to", "too many expressions");

    /* Just inside both pools it runs, and the counter proves every statement
     * executed — the budget is a bound, not a silent truncation. */
    o = (size_t)snprintf(big, sizeof big,
                         "{\"vars\":{\"n\":0},\"widgets\":["
                         "{\"kind\":\"field\",\"name\":\"d\",\"rect\":"
                         "[0,0,90,20]},"
                         "{\"kind\":\"button\",\"name\":\"b\",\"text\":\"go\","
                         "\"rect\":[0,20,90,20]}],"
                         "\"on\":[{\"click\":\"b\",\"do\":[");
    for (int i = 0; i < 90; i++)
        o += (size_t)snprintf(big + o, sizeof big - o,
                              "%s{\"set\":\"n\",\"to\":\"n + 1\"}", i ? "," : "");
    o += (size_t)snprintf(big + o, sizeof big - o,
                          ",{\"set\":\"d\",\"to\":\"text(n)\"}]}]}");
    CHECK_EQ(launch(big, &id), APP_OK);
    uint32_t steps_before = app_stats()->steps;
    press(id, "go");
    CHECK_STR(display_of(id), "90");
    CHECK_EQ(app_stats()->steps - steps_before, 91);
    CHECK(app_stats()->step_limits == 0);
    CHECK_STR(app_last_error(id), "");
}

static void test_submit_event_and_field_editing(void) {
    uint32_t id = 0;
    fixture();
    CHECK_EQ(launch(
        "{\"title\":\"Notes\",\"width\":320,\"height\":120,"
        "\"vars\":{\"n\":0},"
        "\"widgets\":["
        "{\"kind\":\"field\",\"name\":\"txt\",\"text\":\"\",\"rect\":"
        "[8,8,290,24]},"
        "{\"kind\":\"label\",\"name\":\"count\",\"text\":\"0 chars\","
        "\"rect\":[8,40,290,16]}],"
        "\"on\":[{\"submit\":\"txt\",\"do\":["
        "{\"set\":\"count\",\"to\":\"cat(text(len(txt)), ' chars')\"}]}]}",
        &id), APP_OK);

    gui_window_t *w = gui_window(id);
    gui_rect_t    c = gui_client_rect(w);
    gui_widget_t *f = &w->widgets[0];

    /* Click the field: the WM arms its one-line keyboard grab (gui.h). */
    CHECK_EQ(gui_wants_keys(), 0);
    gui_click_at(c.x + f->r.x + 10, c.y + f->r.y + 10);
    CHECK_EQ(gui_wants_keys(), 1);

    /* The line lands in the field and the app's submit handler runs. */
    CHECK_EQ(gui_take_line("hello", 5), 1);
    CHECK_STR(gui_text(f), "hello");
    CHECK_STR(gui_text(&w->widgets[1]), "5 chars");
    /* ...and exactly one line: the prompt is never captured for longer. */
    CHECK_EQ(gui_wants_keys(), 0);
    CHECK_EQ(gui_take_line("world", 5), 0);
    CHECK_STR(gui_text(f), "hello");
}

/* ====================================================================== */
/* 8. the tool                                                            */
/* ====================================================================== */

static char          rbuf[TOOL_RESULT_MAX];
static tool_result_t res;

/* tool_dispatch() returns TOOL_OK whenever the handler RAN — a logical failure
 * is reported in out->is_error, which is what the MODEL reads (tool.h). So this
 * helper returns the model's verdict rather than the dispatcher's, and every
 * `call(...) != TOOL_OK` below is an assertion about what the model was told. */
static int call(const char *input) {
    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = "app";
    c.input     = input;
    c.input_len = input ? strlen(input) : 0;
    tool_result_init(&res, rbuf, sizeof rbuf);
    kcap_reset();
    trace_reset();
    int rc = tool_dispatch(&c, &res);
    if (rc != TOOL_OK) return rc;
    return res.is_error ? TOOL_EINVAL : TOOL_OK;
}

/* Count the kernel trace lines in the captured console: a '[' in column zero. */
static int trace_lines(void) {
    const char *s = kcap_text();
    int         n = 0, col = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n') { col = 0; continue; }
        if (col == 0 && s[i] == '[') n++;
        col++;
    }
    return n;
}

/* Bytes one tool really spends in the assembled array, counting the JSON
 * escaping core/tool.c applies to the description. The old version of this test
 * used strlen(description), which UNDERCOUNTS every description containing a
 * quote — and the `app` description now embeds a worked JSON example, so the
 * undercount would have been ~40 bytes of invisible slack. */
static size_t wire_cost(const tool_t *t) {
    /* {"name":"","description":"","input_schema":} plus the two quoted values */
    size_t n = 41 + strlen(t->name) + 2 + strlen(t->input_schema);
    for (const char *p = t->description; p && *p; p++)
        n += (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) ? 2 : 1;
    return n + 2;
}

static void test_tool_registry_and_schema_cost(void) {
    CHECK_EQ(tool_count(), 5);
    const tool_t *t = tool_find("app");
    CHECK(t != NULL);
    if (!t) return;

    /* THE SHARED BUDGET, STATED HONESTLY.
     *
     * chat.h gives the whole machine CHAT_TOOLS_BYTES for every tool's schema
     * together, and net/chat.c's tools_load() offers the model NOTHING if that
     * is exceeded — a total failure, which is why this is pinned at all.
     *
     * THE NUMBER THIS TEST USED TO ASSERT WAS STALE AND IT MATTERED. It capped
     * this file at 437 bytes because CHAT_TOOLS_BYTES was 32768 and 44 tools
     * cost 32330. CHAT_TOOLS_BYTES is 40960 today and the full 45-tool registry
     * assembles to CHAT_REGISTRY_BYTES. That stale 437 was the reason the `app`
     * description had to stay too terse to be discoverable, which is the defect
     * this change fixes: the description now costs ~1.8 KiB and buys a worked
     * example the model can copy blind.
     *
     * The replacement literal went stale too, 699 bytes' worth, in this exact
     * printf. So it is a constant now, in chat.h, and `make test-qemu` fails if
     * the kernel's own banner disagrees with it — see CHAT_REGISTRY_BYTES.
     *
     * So the ceiling below is deliberately generous, and it is checked against
     * the REAL wire cost including escaping. It exists to catch a description
     * that runs away by kilobytes, not to shave bytes. Before raising it, get
     * the true registry total from the banner rather than from a comment.
     *
     * RAISED 1800 -> 2100 once, and this is the whole justification. A live run
     * showed the model refusing to build a ticking clock — "this machine can't
     * tick on its own" — because the description still said "a clock with a
     * Refresh button", written when app_tick() had no caller. kernel/main.c now
     * calls it, so that sentence had become false, and the correction costs
     * 150 bytes. Checked against the numbers, not against a feeling: the
     * registry is CHAT_REGISTRY_BYTES of CHAT_TOOLS_BYTES=40960, and the
     * CHAT_REQ_BYTES assertion below covers the second bound. Do not treat 2100
     * as a target either. */
    size_t cost = wire_cost(t);
    printf("    (app tool wire cost %zu bytes; ceiling 2100, and the whole "
           "45-tool registry is %d of CHAT_TOOLS_BYTES=%d)\n",
           cost, CHAT_REGISTRY_BYTES, CHAT_TOOLS_BYTES);
    CHECK(cost <= 2100);
    CHECK(CHAT_REGISTRY_BYTES < CHAT_TOOLS_BYTES);

    /* The headroom the banner reports must be real headroom, not a rounding
     * error: a full history and this schema still have to share CHAT_REQ_BYTES
     * with the system prompt (see chat.h's cliff note). */
    CHECK((size_t)CHAT_REGISTRY_BYTES + CHAT_HISTORY_BYTES +
          strlen(chat_system_prompt()) + 1400u < CHAT_REQ_BYTES);

    /* The advertised schema is valid JSON and really is an object, or
     * tool_usable() would drop the tool from the array without a word. */
    json_value_t v;
    CHECK_EQ(json_parse(t->input_schema, strlen(t->input_schema), &v), JSON_OK);
    CHECK_EQ(v.type, JSON_OBJECT);

    /* The assembled array parses, which is what chat.c requires of it. */
    static char buf[8192];
    size_t need = tools_build_schema(buf, sizeof buf);
    CHECK(need < sizeof buf);
    CHECK_EQ(json_parse(buf, need, &v), JSON_OK);
    CHECK_CONTAINS(buf, "\"name\":\"app\"");
}

/* ====================================================================== */
/* THE DEFECT THIS FILE EXISTS TO PREVENT COMING BACK                      */
/* ====================================================================== */

/* WHAT WENT WRONG, so nobody re-introduces it by "tidying" a description.
 *
 * Asked for "a window that says hello", the live model answered that the two
 * apps this kernel ships are calculator and notes, that there is no generic
 * custom-window app, and that the closest it could do was open notes. Then it
 * called gui_open app=notes. Every word of that was a faithful reading of the
 * tool surface — and the machine could already do exactly what was asked.
 *
 * The cause was two tools claiming ONE headline use case in nearly identical
 * words: gui_open said 'This is how "I want a calculator" becomes a real window'
 * and listed two apps, and `app` said 'this is how "I want a calculator" becomes
 * a real, clickable window'. gui_open read as a closed list of two, so the model
 * believed it.
 *
 * These assertions are about WORDING, which is unusual for a kernel test and is
 * the point: the wording IS the interface the model programs against, so it is
 * as load-bearing as a struct layout and it regresses just as silently. */
static void test_the_two_tools_do_not_claim_the_same_job(void) {
    const tool_t *ga = tool_find("gui_open");
    const tool_t *ap = tool_find("app");
    CHECK(ga && ap);
    if (!ga || !ap) return;

    /* 1. THE COLLIDING CLAIM IS GONE FROM gui_open. It no longer presents itself
     *    as the route from a wish to a window. */
    CHECK(strstr(ga->description, "I want a calculator") == NULL);

    /* 2. gui_open SAYS ITS LIST IS NOT THE LIMIT, and names where to go. This is
     *    the exact sentence whose absence produced the apology. */
    /* "not the limit", one needle, not two. CHECK_CONTAINS is a bare strstr, so
     * the "not" this used to look for was found inside "notes" — a word the
     * clause five lines down REQUIRES to be present. It therefore could not
     * fail, and it passed on the pre-change description this whole test exists
     * to reject. A guard that cannot fail is worse than no guard: it is a line
     * of evidence that is really a tautology. */
    CHECK_CONTAINS(ga->description, "not the limit");
    CHECK_CONTAINS(ga->description, "`app`");
    /* including in the property description, which is what a model reads when it
     * is filling the argument in and used to see a bare "calculator or notes" */
    CHECK_CONTAINS(ga->input_schema, "`app`");
    CHECK(strstr(ga->input_schema, "\"description\":\"calculator or notes\"")
          == NULL);
    /* and it is still honest about what IT can open */
    CHECK_CONTAINS(ga->description, "calculator");
    CHECK_CONTAINS(ga->description, "notes");

    /* 3. `app` READS AS THE GENERAL ANSWER, in the operator's own words rather
     *    than in the format's words. */
    CHECK_CONTAINS(ap->description, "I want <anything>");
    CHECK_CONTAINS(ap->description, "stopwatch");
    CHECK_CONTAINS(ap->description, "says hello");

    /* 4. AND IT PRE-EMPTS THE APOLOGY ITSELF. */
    CHECK_CONTAINS(ap->description, "never tell the operator");

    /* 5. NEITHER TOOL IS THE OTHER'S ORPHAN: each names the other, so a model
     *    that lands on the wrong one is one hop from the right one.
     *
     *    The needle is the BACKTICKED name. A bare "app" matched inside
     *    "application" and inside "Apps:", so this clause also passed on the
     *    old description — and it was already implied by clause 2's `app`
     *    check, which makes it doubly dead. What is actually load-bearing is
     *    the imperative next to the name, so assert that instead: gui_open must
     *    tell the model to GO to the other tool, not merely mention it. */
    CHECK_CONTAINS(ga->description, "with the `app` tool");
    CHECK(strstr(ap->description, "gui_open") != NULL);
}

/* The happy path must not require action=format first. A model under pressure
 * skips a "read the docs" call, so the description has to carry a document that
 * REALLY LAUNCHES — asserted by extracting it from the description and running
 * it, exactly as test_tool_format_teaches_the_format does for the full spec. */
static void test_description_carries_a_document_that_launches(void) {
    fixture();
    const tool_t *ap = tool_find("app");
    CHECK(ap != NULL);
    if (!ap) return;

    /* the description promises the example works as-is; hold it to that */
    CHECK_CONTAINS(ap->description, "works as-is");

    const char *s = strstr(ap->description, "{\"title\"");
    CHECK(s != NULL);
    if (!s) return;
    /* the example ends at the last '}' of the widgets array + object */
    const char *e = strstr(s, "\"col\":0}]}");
    CHECK(e != NULL);
    if (!e) return;
    size_t n = (size_t)(e - s) + strlen("\"col\":0}]}");

    char doc[512];
    CHECK(n < sizeof doc);
    memcpy(doc, s, n);
    doc[n] = '\0';

    uint32_t id = 0;
    int rc = app_launch(doc, n, GUI_POS_AUTO, GUI_POS_AUTO, &id, &g_err);
    if (rc != APP_OK)
        printf("    (the description's example was rejected at %s: %s)\n",
               g_err.path, g_err.msg);
    CHECK_EQ(rc, APP_OK);
    CHECK(id != 0);
    /* and it is the window the operator asked for */
    CHECK_STR(app_title(id), "Hi");
}

/* A model that guesses and fails must be able to fix itself on attempt two from
 * the ERROR ALONE — no extra round spent on action=format. */
static void test_a_failed_launch_teaches_the_format(void) {
    fixture();

    /* the archetypal guess: plausible shape, wrong in three ways at once
     * (no placement, a kind that does not exist, a handler naming nothing) */
    CHECK(call("{\"action\":\"launch\",\"document\":{\"title\":\"Hello\","
               "\"widgets\":[{\"kind\":\"text\",\"text\":\"hello\"}]}}")
          != TOOL_OK);
    CHECK_EQ(res.is_error, 1);
    CHECK_EQ(app_count(), 0);
    CHECK_EQ(gui_window_count(), 0);

    /* it says nothing was made... */
    CHECK_CONTAINS(rbuf, "NOT launched");
    /* ...names the fault... */
    CHECK_CONTAINS(rbuf, "where:");
    CHECK_CONTAINS(rbuf, "problem:");
    /* ...AND hands over a document that launches, plus the rules a guess misses */
    CHECK_CONTAINS(rbuf, "DOES launch");
    CHECK_CONTAINS(rbuf, "\"kind\":\"label\"");
    CHECK_CONTAINS(rbuf, "row+col");
    CHECK_CONTAINS(rbuf, "call app action=launch again");
    /* and where the rest of the spec is, for the case it needs more */
    CHECK_CONTAINS(rbuf, "action=format");
    CHECK_EQ(trace_lines(), 1);

    /* THE WHOLE THING REACHES THE MODEL. chat.c clips a result at
     * CHAT_TOOL_RESULT_CAP, and a skeleton that stops mid-document is worse than
     * no skeleton: the model would copy a truncated example. */
    if (res.len >= CHAT_TOOL_RESULT_CAP)
        printf("    (rejection is %d bytes; the model only sees %d)\n",
               (int)res.len, CHAT_TOOL_RESULT_CAP - 1);
    CHECK(res.len < CHAT_TOOL_RESULT_CAP);
    CHECK_EQ(res.truncated, 0);

    /* ATTEMPT TWO ACTUALLY LANDS: the skeleton is lifted out of the error text
     * and launched verbatim, which is the whole claim being made. */
    const char *s = strchr(strstr(rbuf, "DOES launch"), '{');
    CHECK(s != NULL);
    if (!s) return;
    const char *nl = strchr(s, '\n');
    CHECK(nl != NULL);
    if (!nl) return;
    char doc[512];
    size_t n = (size_t)(nl - s);
    CHECK(n < sizeof doc);
    memcpy(doc, s, n);
    doc[n] = '\0';

    uint32_t id = 0;
    int rc = app_launch(doc, n, GUI_POS_AUTO, GUI_POS_AUTO, &id, &g_err);
    if (rc != APP_OK)
        printf("    (the skeleton in the error was rejected at %s: %s)\n",
               g_err.path, g_err.msg);
    CHECK_EQ(rc, APP_OK);
    CHECK(id != 0);
}

/* THE WORST CASE, because this is where a helpful skeleton would silently eat
 * the error it was supposed to explain. app_error_t carries a 72-byte path and a
 * 224-byte msg; with both at full length the reply must STILL fit
 * CHAT_TOOL_RESULT_CAP with the skeleton intact. */
static void test_rejection_plus_skeleton_fits_the_result_cap(void) {
    fixture();

    /* A real rejection whose path and msg are both long: a handler naming a
     * widget that does not exist makes the runtime append the known-name list,
     * which is the wordiest message it produces. */
    CHECK(call("{\"action\":\"launch\",\"document\":{"
               "\"vars\":{\"averyveryverylongvariablename\":0},"
               "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\","
               "\"name\":\"abutton\",\"rect\":[0,0,40,20]},"
               "{\"kind\":\"field\",\"name\":\"anotherlongwidgetname\","
               "\"rect\":[0,20,40,20]}],"
               "\"on\":[{\"click\":\"aputton\",\"do\":[]}]}}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "problem:");
    CHECK_CONTAINS(rbuf, "DOES launch");
    CHECK_CONTAINS(rbuf, "action=format");
    CHECK(res.len < CHAT_TOOL_RESULT_CAP);
    CHECK_EQ(res.truncated, 0);

    /* NOW PROVE THE BOUND INSTEAD OF SAMPLING IT. Any observed rejection is one
     * point; what matters is that the LONGEST one the runtime can emit still
     * fits. The reply is a fixed preamble, then "where: <path>", then
     * "problem: <msg>", then the fixed skeleton — so the maximum is computable
     * from app_error_t's own field sizes, whatever message the runtime chooses.
     *
     * Both halves are measured out of the live reply rather than hardcoded, so
     * editing either string keeps this honest. */
    const char *where = strstr(rbuf, "where: ");
    const char *skel  = strstr(rbuf, "This document DOES launch");
    CHECK(where != NULL);
    CHECK(skel != NULL);
    if (!where || !skel) return;

    size_t preamble = (size_t)(where - rbuf);
    size_t skeleton = strlen(skel);
    size_t path_max = sizeof ((app_error_t *)0)->path - 1;   /* 72 - NUL  */
    size_t msg_max  = sizeof ((app_error_t *)0)->msg  - 1;   /* 224 - NUL */

    /* "where: " + path + "\n" + "problem: " + msg + "\n" */
    size_t worst = preamble + 7 + path_max + 1 + 9 + msg_max + 1 + skeleton;

    printf("    (observed %d bytes; computable worst case %zu of %d "
           "[preamble %zu + path %zu + msg %zu + skeleton %zu])\n",
           (int)res.len, worst, CHAT_TOOL_RESULT_CAP,
           preamble, path_max, msg_max, skeleton);
    CHECK(worst < CHAT_TOOL_RESULT_CAP);
}

/* The system prompt must volunteer this capability, or the model never looks for
 * the tool in the first place. Asserted here (not only in test_agency) because
 * this suite links chat.c and owns the app surface. */
static void test_system_prompt_offers_to_build_apps(void) {
    const char *p = chat_system_prompt();
    CHECK(p != NULL);
    CHECK_CONTAINS(p, "BUILD GUI APPS");
    /* it is specific about the mechanism, not vague encouragement */
    CHECK_CONTAINS(p, "JSON document");
    CHECK_CONTAINS(p, "no fixed catalogue");
    /* it names the requests the model previously refused */
    CHECK_CONTAINS(p, "stopwatch");
    CHECK_CONTAINS(p, "says hello");
    /* it says the built-in list is not the limit, which is what it got wrong */
    CHECK_CONTAINS(p, "NOT the limit");
    /* and it tells the model retrying is cheap */
    CHECK_CONTAINS(p, "skeleton");
}

static void test_tool_format_teaches_the_format(void) {
    fixture();
    CHECK_EQ(call("{\"action\":\"format\"}"), TOOL_OK);
    CHECK_EQ(res.is_error, 0);
    CHECK_CONTAINS(rbuf, "\"widgets\"");
    CHECK_CONTAINS(rbuf, "\"click\"");
    CHECK_CONTAINS(rbuf, "iserr");
    CHECK_CONTAINS(rbuf, "no loops");
    CHECK_CONTAINS(kcap_text(), "[app action=format -> ok]");
    CHECK_EQ(trace_lines(), 1);

    /* THE WHOLE SPEC MUST REACH THE MODEL. chat.c clips a tool result to
     * CHAT_TOOL_RESULT_CAP, and a format specification that stops mid-sentence
     * is worse than a shorter one that finishes. */
    if (res.len >= CHAT_TOOL_RESULT_CAP)
        printf("    (the format spec is %d bytes; the model only ever sees %d)\n",
               (int)res.len, CHAT_TOOL_RESULT_CAP - 1);
    CHECK(res.len < CHAT_TOOL_RESULT_CAP);
    CHECK_EQ(res.truncated, 0);

    /* The worked example in that text must actually launch, or the tool is
     * teaching a lie. It is extracted from the result and run. */
    const char *start = strchr(rbuf, '{');
    CHECK(start != NULL);
    if (!start) return;
    /* The example ends at the last '}' on its line. */
    const char *nl = strchr(start, '\n');
    CHECK(nl != NULL);
    if (!nl) return;
    char doc[1024];
    size_t n = (size_t)(nl - start);
    CHECK(n < sizeof doc);
    memcpy(doc, start, n);
    doc[n] = '\0';

    uint32_t id = 0;
    int rc = app_launch(doc, n, GUI_POS_AUTO, GUI_POS_AUTO, &id, &g_err);
    if (rc != APP_OK) printf("    (example rejected at %s: %s)\n",
                             g_err.path, g_err.msg);
    CHECK_EQ(rc, APP_OK);
    /* And it works: the counter counts. */
    press(id, "+1");
    press(id, "+1");
    CHECK_STR(display_of(id), "2");
}

static void test_tool_launch_list_state_close(void) {
    fixture();

    /* launch, with the document as a nested object (the preferred form). */
    static char in[APP_DOC_MAX + 64];
    snprintf(in, sizeof in, "{\"action\":\"launch\",\"x\":40,\"y\":60,"
                            "\"document\":%s}", app_example_calculator());
    CHECK_EQ(call(in), TOOL_OK);
    CHECK_EQ(res.is_error, 0);
    CHECK_CONTAINS(rbuf, "launched \"Calculator\"");
    CHECK_CONTAINS(rbuf, "18 widget(s)");
    CHECK_CONTAINS(rbuf, "acc=0");
    CHECK_CONTAINS(rbuf, "button id=2 text=\"7\"");
    CHECK_CONTAINS(kcap_text(), "[app action=launch title=Calculator widgets=18");
    CHECK_EQ(trace_lines(), 1);
    CHECK_EQ(app_count(), 1);
    uint32_t id = app_at(0);
    CHECK_EQ(gui_window(id)->frame.x, 40);
    CHECK_EQ(gui_window(id)->frame.y, 60);

    /* list */
    CHECK_EQ(call("{\"action\":\"list\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "1 of 4 app slots");
    CHECK_CONTAINS(rbuf, "\"Calculator\"");
    CHECK_CONTAINS(rbuf, "launched");
    CHECK_EQ(trace_lines(), 1);

    /* state, before and after a click: this is how the model checks its work. */
    char q[64];
    snprintf(q, sizeof q, "{\"action\":\"state\",\"id\":%u}", (unsigned)id);
    CHECK_EQ(call(q), TOOL_OK);
    CHECK_CONTAINS(rbuf, "field id=1 name=display text=\"0\"");
    CHECK_CONTAINS(rbuf, "op=");

    keys(id, "7+8=");
    CHECK_EQ(call(q), TOOL_OK);
    CHECK_CONTAINS(rbuf, "text=\"15\"");
    CHECK_CONTAINS(rbuf, "acc=15");
    CHECK_EQ(trace_lines(), 1);

    /* close */
    snprintf(q, sizeof q, "{\"action\":\"close\",\"id\":%u}", (unsigned)id);
    CHECK_EQ(call(q), TOOL_OK);
    CHECK_CONTAINS(rbuf, "closed app");
    CHECK_CONTAINS(kcap_text(), "[app action=close");
    CHECK_EQ(app_count(), 0);
    CHECK_EQ(gui_window_count(), 0);
    CHECK_EQ(trace_lines(), 1);

    /* The document may also arrive as a JSON string, escaping and all. */
    CHECK_EQ(call("{\"action\":\"launch\",\"document\":\"{\\\"widgets\\\":"
                  "[{\\\"kind\\\":\\\"button\\\",\\\"text\\\":\\\"hi\\\","
                  "\\\"rect\\\":[0,0,60,20]}]}\"}"), TOOL_OK);
    CHECK_EQ(res.is_error, 0);
    CHECK_CONTAINS(rbuf, "1 widget(s)");
    CHECK_EQ(app_count(), 1);
}

static void test_tool_rejects_hostile_input_legibly(void) {
    fixture();

    static const char *bad[] = {
        "",                                          /* no input at all      */
        "not json",
        "[]",
        "42",
        "{",
        "{\"action\":\"launch\"",                    /* truncated            */
        "{\"action\":7}",
        "{\"action\":null}",
        "{\"action\":\"\"}",
        "{\"action\":\"lunch\"}",                    /* near miss            */
        "{\"action\":\"launch\"}",                   /* no document          */
        "{\"action\":\"launch\",\"document\":7}",
        "{\"action\":\"launch\",\"document\":[]}",
        "{\"action\":\"launch\",\"document\":null}",
        "{\"action\":\"launch\",\"document\":true}",
        "{\"action\":\"launch\",\"document\":{}}",   /* no widgets           */
        "{\"action\":\"launch\",\"document\":\"{ nope\"}",
        "{\"action\":\"launch\",\"document\":{\"widgets\":[]},\"x\":1.5}",
        "{\"action\":\"launch\",\"document\":{\"widgets\":[]},\"x\":1e9}",
        ("{\"action\":\"launch\",\"document\":{\"widgets\":[]},"
         "\"y\":100000000000000000000}"),
        "{\"action\":\"state\"}",                    /* no id                */
        "{\"action\":\"close\"}",
        "{\"action\":\"state\",\"id\":0}",
        "{\"action\":\"state\",\"id\":-3}",
        "{\"action\":\"state\",\"id\":999}",
        "{\"action\":\"close\",\"id\":\"3\"}",
        "{\"action\":\"close\",\"id\":1.5}",
        "{\"action\":\"format\\u0000junk\"}",        /* embedded NUL         */
        "{\"action\":{\"action\":\"list\"}}",        /* nested              */
        "{\"action\":\"list\",\"action\":\"close\"}",/* duplicated key       */
    };

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int apps_before = app_count();
        int wins_before = gui_window_count();
        int rc = call(bad[i]);

        /* A duplicated or nested key may resolve to something legal; what must
         * never happen is a crash, a silent success, or a missing trace line. */
        if (rc == TOOL_OK) continue;
        CHECK(res.is_error != 0);
        CHECK_CONTAINS(rbuf, "error:");
        CHECK(res.len > 8);
        CHECK_EQ(app_count(), apps_before);
        CHECK_EQ(gui_window_count(), wins_before);
        if (trace_lines() != 1)
            printf("    (input %zu produced %d trace lines: %s)\n",
                   i, trace_lines(), bad[i]);
        CHECK_EQ(trace_lines(), 1);
        CHECK_EQ(kpanic_hit, 0);
    }
}

static void test_tool_rejection_is_actionable(void) {
    fixture();
    /* The document the model has to fix: a handler naming a tag that does not
     * exist. The reply must say WHERE, WHAT, and that nothing was launched. */
    CHECK(call("{\"action\":\"launch\",\"document\":{\"widgets\":["
               "{\"kind\":\"button\",\"text\":\"7\",\"tag\":\"digit\","
               "\"rect\":[0,0,40,20]}],"
               "\"on\":[{\"click\":\"digits\",\"do\":[]}]}}") != TOOL_OK);
    CHECK_EQ(res.is_error, 1);
    CHECK_CONTAINS(rbuf, "was NOT launched");
    CHECK_CONTAINS(rbuf, "where: on[0].click");
    CHECK_CONTAINS(rbuf, "no widget has the name or tag \"digits\"");
    CHECK_CONTAINS(rbuf, "Available: digit");
    CHECK_CONTAINS(rbuf, "call app action=launch again");
    CHECK_CONTAINS(kcap_text(), "[app action=launch rejected at on\\x5b");
    CHECK_EQ(trace_lines(), 1);
    CHECK_EQ(app_count(), 0);
    CHECK_EQ(gui_window_count(), 0);

    /* An expression typo reports the offset and quotes the expression back. */
    CHECK(call("{\"action\":\"launch\",\"document\":{\"vars\":{\"n\":0},"
               "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
               "\"rect\":[0,0,40,20]}],"
               "\"on\":[{\"click\":\"b\",\"do\":[{\"set\":\"n\",\"to\":"
               "\"n + m\"}]}]}}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "where: on[0].do[0].to");
    CHECK_CONTAINS(rbuf, "unknown name \"m\"");
    CHECK_CONTAINS(rbuf, "at offset 4");
    CHECK_CONTAINS(rbuf, "Known names here: n, b, key");
}

static void test_tools_without_a_framebuffer(void) {
    fixture();
    gui_detach();

    /* format still works: a model may read the spec on a machine that cannot
     * draw, and then be told plainly why launching is impossible. */
    CHECK_EQ(call("{\"action\":\"format\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "widgets");

    CHECK(call("{\"action\":\"list\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no window manager");
    CHECK(call("{\"action\":\"launch\",\"document\":{\"widgets\":[{\"kind\":"
               "\"button\",\"rect\":[0,0,9,9]}]}}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no window manager");
    CHECK_EQ(app_count(), 0);

    /* app_launch() says the same thing through its own error path. */
    uint32_t id = 12345;
    CHECK_EQ(launch(MIN_DOC, &id), APP_ENOGUI);
    CHECK_EQ(id, 0);
    CHECK_CONTAINS(g_err.msg, "no window manager");
}

/* ====================================================================== */
/* 9. the golden transcript                                               */
/* ====================================================================== */

static void queue(const char *body) { CHECK_EQ(model_mock_queue(200, body), MODEL_OK); }

/* RUNS FIRST, and has to: the recorded session names window id 1, because the
 * model reads that id out of the launch result and a canned transcript cannot.
 * gui.h never reuses a window id for the life of the boot, so the only way the
 * first window is id 1 is for this to be the first window opened. main() calls it
 * first for that reason, and the CHECK_EQ(id, 1) below is what fails if someone
 * reorders it. */
static void test_golden_transcript(void) {
    fixture();
    model_mock_reset();
    chat_reset();
    chat_init(model_mock_transport());
    chat_set_max_rounds(0);          /* after chat_init: it resets the cap */
    kcap_reset();

    queue(AR_ROUND1);
    queue(AR_ROUND2);
    queue(AR_ROUND3);
    queue(AR_ROUND4);
    queue(AR_ROUND5);
    queue(AR_ROUND6);

    CHECK_EQ(chat_ask(AR_SENTENCE), CHAT_OK);

    CHECK_EQ((int)model_mock_request_count(), 6);
    CHECK_EQ((int)chat_last_rounds(), 6);
    CHECK_EQ((int)chat_last_tool_calls(), 8);   /* 1+1+1+4+1 */
    CHECK_EQ((int)model_mock_pending(), 0);

    const char *console = kcap_text();

    /* ---- what the kernel did, in the kernel's own words ---- */
    CHECK_CONTAINS(console, "[app action=format -> ok]");
    /* trace.h escapes '[' and ']' inside the argument field as \xNN so a value
     * that came from the model can never forge a kernel line. The JSON path in
     * this argument is exactly such a value, so the ground truth reads
     * on\x5b0\x5d.click — asserted in the escaped form on purpose. */
    CHECK_CONTAINS(console, "[app action=launch rejected at on\\x5b0\\x5d.click");
    CHECK_CONTAINS(console, "[app action=launch title=Calculator widgets=8");
    CHECK_CONTAINS(console, "[gui_click id=1 widget=2 label=7");
    CHECK_CONTAINS(console, "[gui_click id=1 widget=4 label=+");
    CHECK_CONTAINS(console, "[gui_click id=1 widget=3 label=8");
    CHECK_CONTAINS(console, "[gui_click id=1 widget=8 label==");
    /* The model's closing prose reached the operator. */
    CHECK_CONTAINS(console, "display reads 15");

    /* ---- what the model saw between the two attempts: the reason ---- */
    const char *req3 = model_mock_request(2);
    CHECK(req3 != NULL);
    CHECK_CONTAINS(req3, "was NOT launched");
    CHECK_CONTAINS(req3, "on[0].click");
    CHECK_CONTAINS(req3, "no widget has the name or tag");
    CHECK_CONTAINS(req3, "\"is_error\":true");
    /* ...and the format spec it read in the first round. */
    const char *req2 = model_mock_request(1);
    CHECK(req2 != NULL);
    CHECK_CONTAINS(req2, "Launch this document, then adapt it");
    CHECK_CONTAINS(req2, "iserr() finds it");   /* the WHOLE spec, not a prefix */

    /* ---- and the state read-back the model used to check itself ---- */
    const char *req6 = model_mock_request(5);
    CHECK(req6 != NULL);
    CHECK_CONTAINS(req6, "text=\\\"15\\\"");

    /* ---- THE APP IS GENUINELY RUNNING AND GENUINELY SHOWS 15 ---- */
    CHECK_EQ(app_count(), 1);
    uint32_t id = app_at(0);
    CHECK_EQ(id, 1u);
    CHECK_STR(app_title(id), "Calculator");
    CHECK_EQ(gui_window_count(), 1);
    CHECK_STR(display_of(id), "15");
    CHECK_EQ(gui_window(id)->nwidgets, 8);
    CHECK_EQ(kpanic_hit, 0);

    /* One document was refused and one accepted — no half-built windows. */
    CHECK(app_stats()->launches >= 1);
    CHECK(app_stats()->rejections >= 1);

    /* The operator's whole view of the session, on request. */
    if (getenv("APP_SHOW_TRANSCRIPT"))
        printf("\n----- session as the operator saw it -----\n> %s\n%s"
               "------------------------------------------\n",
               AR_SENTENCE, console);
}

/* The other half of the loop: a model that keeps sending the same broken
 * document is refused every time, and the machine is unharmed. */
static void test_transcript_that_never_converges(void) {
    fixture();
    model_mock_reset();
    chat_reset();
    chat_init(model_mock_transport());
    chat_set_max_rounds(3);          /* after chat_init: it resets the cap */
    kcap_reset();

    for (int i = 0; i < 3; i++) queue(AR_ROUND2);
    CHECK_EQ(chat_ask(AR_SENTENCE), CHAT_ELIMIT);
    CHECK_EQ(app_count(), 0);
    CHECK_EQ(gui_window_count(), 0);
    CHECK_EQ(kpanic_hit, 0);
    CHECK_CONTAINS(kcap_text(), "rejected at on\\x5b0\\x5d.click");
    chat_set_max_rounds(0);
}

/* ====================================================================== */

int main(void) {
    console_init();
    install_tool_table();

    /* The transcripts run first: see the comment on test_golden_transcript. */
    RUN(test_golden_transcript);
    RUN(test_transcript_that_never_converges);

    RUN(test_number_parsing);
    RUN(test_number_formatting);

    RUN(test_expr_arithmetic);
    RUN(test_expr_comparisons_and_logic);
    RUN(test_expr_functions);
    RUN(test_expr_errors_are_values_not_traps);
    RUN(test_expr_reads_widget_text);
    RUN(test_expr_refusals_say_where);
    RUN(test_expr_bounds_are_refused_not_survived);

    RUN(test_minimal_document_launches);
    RUN(test_hostile_documents_are_refused);
    RUN(test_oversized_and_deep_documents);
    RUN(test_grid_layout_tiles_and_spans);
    RUN(test_readonly_field_is_not_clickable);
    RUN(test_a_handler_makes_its_widget_clickable);

    RUN(test_artifact_matches_the_compiled_copy);
    RUN(test_shipped_calculator_launches);
    RUN(test_calculator_arithmetic_by_clicking);
    RUN(test_calculator_pixels_change_when_a_key_is_pressed);

    RUN(test_instance_pool_and_teardown);
    RUN(test_handler_state_is_per_instance);
    RUN(test_step_budget_stops_a_runaway_handler);
    RUN(test_submit_event_and_field_editing);

    RUN(test_tool_registry_and_schema_cost);
    RUN(test_the_two_tools_do_not_claim_the_same_job);
    RUN(test_description_carries_a_document_that_launches);
    RUN(test_a_failed_launch_teaches_the_format);
    RUN(test_rejection_plus_skeleton_fits_the_result_cap);
    RUN(test_system_prompt_offers_to_build_apps);
    RUN(test_tool_format_teaches_the_format);
    RUN(test_tool_launch_list_state_close);
    RUN(test_tool_rejects_hostile_input_legibly);
    RUN(test_tool_rejection_is_actionable);
    RUN(test_tools_without_a_framebuffer);

    return th_report("app");
}
