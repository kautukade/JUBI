/* test_app_format.c — the additions that closed the gap between "a calculator
 *                     works" and "I want <the thing I actually asked for>".
 *
 * WHAT THIS SUITE IS FOR
 *   tests/host/test_app.c proves the runtime and the calculator. This suite
 *   proves the five additions that were made after enumerating what a person asks
 *   a computer for in one sentence (see DECISION 1b in include/app.h): a periodic
 *   "tick" event, the time snapshot (now / clock / today / hour / minute /
 *   second), rand(), at() and round(). Every one of them is either a new way for
 *   an app to be driven or a new way for it to read something outside itself, so
 *   each is tested three ways:
 *
 *     1. IT WORKS. The op produces the value, the tick fires, the clock reads.
 *     2. ITS BOUND HOLDS, with the number in the message the model will read: a
 *        period under APP_TICK_MIN_MS, a fifth tick handler, rand(0),
 *        rand(APP_RAND_MAX+1), at() past the end, round() overflowing.
 *     3. IT CANNOT RUN AWAY OR BE ABUSED. A tick handler cannot accumulate a
 *        backlog (the property that keeps a single-threaded kernel responsive), a
 *        clock that jumps in either direction cannot wedge or flood it, a hostile
 *        document cannot make rand() loop, and a tick handler is still cut off by
 *        APP_MAX_STEPS.
 *
 *   THE TIME AND THE DICE ARE PINNED, not observed. app_set_time_source() and
 *   app_set_random_source() exist for this: "the stopwatch counts up" is asserted
 *   as a value here and only illustrated by a screenshot on real hardware
 *   (apps/app_format_selftest.c). Nothing in this file depends on how fast it
 *   runs.
 *
 *   AND THE EXAMPLES ARE THE SPEC. Every shipped document under apps/examples/ is
 *   read off the disk, diffed against the copy the kernel embeds, then launched
 *   and driven — because those five files are what a model pattern-matches
 *   against, so a broken one is worse than a missing feature.
 */

#include "harness.h"
#include "app.h"
#include "../../apps/app_impl.h"
#include "gui.h"
#include "widgets.h"
#include "fb.h"

#include "../../apps/examples/examples_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* the fixture: a framebuffer, a pinned clock and pinned dice              */
/* ====================================================================== */

#define SW 1024
#define SH 768
#define GROWS (SH / 16)
#define GCOLS (SW / 8)
#define BLANK_CELL 0x0720u

static fb_color_t   *pixels;
static fb_surface_t  screen;
static uint16_t      cells[GROWS * GCOLS];

/* The pinned monotonic clock. Tests move it by hand, so a tick either fired
 * because it was due or the test is wrong; nothing here waits for real time. */
static uint64_t fake_ms;
static uint64_t ms_source(void) { return fake_ms; }

/* The pinned wall clock, and a switch for "this machine has no readable one" —
 * which is the state of every machine with no RTC, and must make the wall-clock
 * leaves ERR rather than 1970. */
static app_wall_t fake_wall;
static int        wall_ok = 1;
static int        wall_calls;

static int wall_source(app_wall_t *out) {
    wall_calls++;
    if (!wall_ok) return -1;
    *out = fake_wall;
    return 0;
}

/* The pinned dice: a queue, then a counter, so a value can be demanded exactly
 * and a distribution can still be measured. */
static uint64_t rand_queue[8];
static int      rand_n, rand_i, rand_calls;

static uint64_t rand_source(void) {
    rand_calls++;
    if (rand_n) return rand_queue[rand_i++ % rand_n];
    /* A cheap deterministic sequence for distribution checks. */
    static uint64_t st;
    st = st * 6364136223846793005ull + 1442695040888963407ull;
    return st ^ (st >> 29);
}

static void set_wall(int y, int mo, int d, int h, int mi, int s) {
    fake_wall.year   = y;
    fake_wall.month  = (uint8_t)mo;
    fake_wall.day    = (uint8_t)d;
    fake_wall.hour   = (uint8_t)h;
    fake_wall.minute = (uint8_t)mi;
    fake_wall.second = (uint8_t)s;
    wall_ok = 1;
    /* Installing a source takes a fresh snapshot (app_set_time_source), which is
     * what an event boundary does in a running app. Without it these direct
     * evaluations would all see the reading the first one cached — correct
     * behaviour for one event, useless for a test of many. */
    app_set_time_source(ms_source, wall_source);
}

static void fixture(void) {
    if (!pixels) pixels = malloc(sizeof(fb_color_t) * SW * SH);
    app_close_all();
    gui_close_all();
    gui_detach();
    for (int i = 0; i < GROWS * GCOLS; i++) cells[i] = BLANK_CELL;
    CHECK_EQ(fb_surface_init(&screen, pixels, SW, SH, SW), 0);
    gui_test_set_console_cursor(0, 0);
    CHECK_EQ(gui_attach(&screen, cells, GROWS, GCOLS), 0);
    gui_pointer_warp(SW - 4, SH - 4);

    fake_ms = 100000;                     /* not zero: due=0 must still fire */
    set_wall(2026, 7, 29, 14, 3, 7);
    wall_calls = rand_calls = 0;
    rand_n = rand_i = 0;
    app_set_time_source(ms_source, wall_source);
    app_set_random_source(rand_source);
}

static app_error_t g_err;

static int launch(const char *doc, uint32_t *id) {
    return app_launch(doc, 0, GUI_POS_AUTO, GUI_POS_AUTO, id, &g_err);
}

/* A document that must be refused: the code, the path, a phrase from the reason,
 * and — the property the whole format rests on — that NOTHING was created. */
static void reject(const char *doc, const char *path, const char *phrase) {
    uint32_t id = 12345;
    int      apps_before = app_count(), wins_before = gui_window_count();
    int      rc = launch(doc, &id);

    CHECK(rc != APP_OK);
    CHECK_EQ(id, 0);
    CHECK_EQ(app_count(), apps_before);
    CHECK_EQ(gui_window_count(), wins_before);
    if (path)   CHECK_STR(g_err.path, path);
    if (phrase) CHECK_CONTAINS(g_err.msg, phrase);
    if (rc == APP_OK)
        printf("    (document was accepted but should not have been: %.90s)\n",
               doc);
}

/* ---- reading an app back ----
 * Through the public queries only, exactly as the `app` tool's action=state does:
 * app_describe() renders one line per widget ("field id=1 name=out text=..."), so
 * asserting on a substring of it asserts on what the model would be told. */
static const char *describe(uint32_t id) {
    static char buf[2048];
    app_describe(id, buf, sizeof buf);
    return buf;
}

static const char *var_of(uint32_t id, const char *name) {
    static char val[APP_TEXT_MAX];
    int         n = app_var_count(id);
    for (int i = 0; i < n; i++) {
        const char *nm = NULL;
        if (app_var_at(id, i, &nm, val, sizeof val) == 0 && nm && !strcmp(nm, name))
            return val;
    }
    return "(no such var)";
}

/* gui_click_widget() presses and releases at the widget's screen centre and
 * returns 0 when the click was delivered; negative means the widget was scenery,
 * disabled or missing, which in this suite is always a failure. */
static void click(uint32_t id, uint32_t widget_id) {
    CHECK_EQ(gui_click_widget(id, widget_id), 0);
}

/* ====================================================================== */
/* 1. the expression additions, as values                                  */
/* ====================================================================== */

static app_inst_t ex;

static void ex_reset(void) {
    memset(&ex, 0, sizeof ex);
    strcpy(ex.vname[0], "s");   ex.var[0] = app_val_str("abcdef");
    strcpy(ex.vname[1], "n");   ex.var[1] = app_val_num(3 * APP_NUM_SCALE);
    ex.nvar = 2;
}

static app_val_t ev(const char *src) {
    app_error_t err;
    uint16_t    idx = 0;
    memset(&err, 0, sizeof err);
    if (app_expr_compile(&ex, src, &idx, &err) != APP_OK) {
        printf("    FAIL compile \"%s\": %s\n", src, err.msg);
        th_fails++; th_checks++;
        return app_val_err();
    }
    return app_expr_eval(&ex, idx, NULL, NULL);
}

static const char *evt(const char *src) {
    static char b[APP_TEXT_MAX];
    app_val_t   v = ev(src);
    app_val_text(&v, b, sizeof b);
    return b;
}

/* Compile once, evaluate many times — which is also how a running app works: the
 * document is compiled at launch and a handler only ever evaluates. */
static uint16_t compile_once(const char *src) {
    app_error_t err;
    uint16_t    idx = 0;
    memset(&err, 0, sizeof err);
    if (app_expr_compile(&ex, src, &idx, &err) != APP_OK) {
        printf("    FAIL compile \"%s\": %s\n", src, err.msg);
        th_fails++; th_checks++;
    }
    return idx;
}

static void test_round(void) {
    fixture();
    ex_reset();

    CHECK_STR(evt("round(0)"), "0");
    CHECK_STR(evt("round(1.4)"), "1");
    CHECK_STR(evt("round(1.5)"), "2");
    CHECK_STR(evt("round(1.6)"), "2");
    CHECK_STR(evt("round(2.5)"), "3");            /* halves away from zero */
    CHECK_STR(evt("round(-1.5)"), "-2");
    CHECK_STR(evt("round(-0.4)"), "0");
    CHECK_STR(evt("round(0.499999)"), "0");
    CHECK_STR(evt("round(0.5)"), "1");

    /* The money idiom, which is why this exists: 15% of 47.53 to the penny. */
    CHECK_STR(evt("round(47.53 * 0.15 * 100) / 100"), "7.13");
    /* And one decimal place, for a BMI or a stopwatch. */
    CHECK_STR(evt("round(23.456 * 10) / 10"), "23.5");

    /* Errors are values, still: rounding a string or past the range is ERR, not
     * a trap and not a wrapped number. */
    CHECK_STR(evt("round(s)"), "error");
    CHECK_STR(evt("round(1/0)"), "error");
    CHECK_STR(evt("round(9000000000)"), "9000000000");     /* exactly the limit */
    /* Just past it: rounding up would leave the range, so it is ERR rather than a
     * wrapped number, exactly like * and + overflowing. */
    CHECK_STR(evt("round(9000000000.6)"), "error");
    CHECK_STR(evt("iserr(round('x'))"), "1");
}

static void test_at(void) {
    fixture();
    ex_reset();

    CHECK_STR(evt("at('abc', 0)"), "a");
    CHECK_STR(evt("at('abc', 2)"), "c");
    CHECK_STR(evt("at(s, 5)"), "f");
    CHECK_STR(evt("at(s, n)"), "d");                 /* index from a var */
    CHECK_STR(evt("at('abc', 1.9)"), "b");           /* truncated toward zero */

    /* Out of range is ERR, so a document walking off its own alphabet says so. */
    CHECK_STR(evt("at('abc', 3)"), "error");
    CHECK_STR(evt("at('abc', -1)"), "error");
    CHECK_STR(evt("at('', 0)"), "error");
    CHECK_STR(evt("at('abc', 1/0)"), "error");
    CHECK_STR(evt("at(1/0, 0)"), "error");
    CHECK_STR(evt("at('abc', 'x')"), "error");       /* index must be a number */

    /* A number is text first, so at() over one is defined rather than surprising. */
    CHECK_STR(evt("at(12.5, 2)"), ".");

    /* The password idiom: a character out of an alphabet, concatenated. */
    rand_n = 2; rand_queue[0] = 0; rand_queue[1] = ~0ull;
    CHECK_STR(evt("cat(at('abcd', rand(4)), at('abcd', rand(4)))"), "ad");
}

static void test_rand_bounds_and_spread(void) {
    fixture();
    ex_reset();

    /* The two ends of the draw, exactly: multiply-high of 0 is 0, of ~0 is n-1. */
    rand_n = 1; rand_queue[0] = 0;
    CHECK_STR(evt("rand(6)"), "0");
    rand_queue[0] = ~0ull;
    CHECK_STR(evt("rand(6)"), "5");
    CHECK_STR(evt("rand(6) + 1"), "6");              /* the die idiom */
    rand_queue[0] = ~0ull / 2;
    CHECK_STR(evt("rand(2)"), "0");
    rand_queue[0] = ~0ull / 2 + 1;
    CHECK_STR(evt("rand(2)"), "1");

    /* n out of range is ERR — an app that asked for the impossible is told. */
    rand_n = 0;
    CHECK_STR(evt("rand(0)"), "error");
    CHECK_STR(evt("rand(-1)"), "error");
    CHECK_STR(evt("rand(0.5)"), "error");            /* truncates to 0 */
    CHECK_STR(evt("rand(1000001)"), "error");        /* APP_RAND_MAX + 1 */
    CHECK_STR(evt("rand(9000000000)"), "error");
    CHECK_STR(evt("rand('x')"), "error");
    CHECK_STR(evt("rand(1/0)"), "error");
    CHECK_STR(evt("rand(1)"), "0");                  /* the degenerate legal n */
    CHECK_STR(evt("rand(1000000)") [0] != 'e' ? "ok" : "error", "ok");

    /* ONE DRAW PER EVALUATION, NO LOOP. 3000 evaluations of rand(6) must consume
     * exactly 3000 draws — this is the property that makes the op safe on a
     * single-threaded kernel, and it is measured rather than reasoned about. */
    uint16_t die = compile_once("rand(6)");
    rand_calls = 0;
    int seen[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 3000; i++) {
        app_val_t v = app_expr_eval(&ex, die, NULL, NULL);
        CHECK_EQ(v.kind, AV_NUM);
        int64_t k = v.num / APP_NUM_SCALE;
        CHECK(k >= 0 && k < 6);
        CHECK_EQ(v.num % APP_NUM_SCALE, 0);          /* always a whole number */
        if (k >= 0 && k < 6) seen[k]++;
    }
    CHECK_EQ(rand_calls, 3000);
    for (int k = 0; k < 6; k++) CHECK(seen[k] > 200);   /* every face appears */

    /* Two rand()s in one expression are two independent draws. */
    rand_calls = 0;
    (void)ev("rand(6) + rand(6)");
    CHECK_EQ(rand_calls, 2);
}

static void test_time_leaves(void) {
    fixture();
    ex_reset();

    /* `now` is the snapshot in monotonic seconds. Nothing has begun an event, so
     * the snapshot is whatever the last env_begin() left; drive one through the
     * public door instead — a tick — further down. Here: the source reading. */
    app_val_t v = ev("now");
    CHECK_EQ(v.kind, AV_NUM);

    set_wall(2026, 7, 29, 14, 3, 7);
    CHECK_STR(evt("clock"), "14:03:07");
    CHECK_STR(evt("today"), "2026-07-29");
    CHECK_STR(evt("hour"), "14");
    CHECK_STR(evt("minute"), "3");
    CHECK_STR(evt("second"), "7");
    CHECK_STR(evt("hour * 60 + minute"), "843");

    /* Zero padding is the reason these are pre-rendered strings and not
     * something the document assembles. */
    set_wall(2026, 1, 2, 3, 4, 5);
    CHECK_STR(evt("clock"), "03:04:05");
    CHECK_STR(evt("today"), "2026-01-02");

    set_wall(2000, 12, 31, 23, 59, 59);
    CHECK_STR(evt("clock"), "23:59:59");
    CHECK_STR(evt("today"), "2000-12-31");

    /* NO READABLE CLOCK is a state, not a lie: every wall-clock leaf is ERR, and
     * `now` still works, so a stopwatch runs on a machine where a clock cannot. */
    wall_ok = 0;
    app_set_time_source(ms_source, wall_source);
    CHECK_STR(evt("clock"), "error");
    CHECK_STR(evt("today"), "error");
    CHECK_STR(evt("hour"), "error");
    CHECK_STR(evt("iserr(clock)"), "1");
    CHECK_EQ(ev("now").kind, AV_NUM);

    /* A BACKEND THAT LIES is treated like any other untrusted input: a reading
     * outside the calendar is ERR rather than a string with garbage in it. */
    set_wall(2026, 13, 1, 0, 0, 0);   CHECK_STR(evt("clock"), "error");
    set_wall(2026, 1, 32, 0, 0, 0);   CHECK_STR(evt("today"), "error");
    set_wall(2026, 1, 1, 24, 0, 0);   CHECK_STR(evt("clock"), "error");
    set_wall(2026, 1, 1, 0, 60, 0);   CHECK_STR(evt("clock"), "error");
    set_wall(2026, 1, 1, 0, 0, 60);   CHECK_STR(evt("clock"), "error");
    set_wall(1969, 1, 1, 0, 0, 0);    CHECK_STR(evt("today"), "error");
    set_wall(99999, 1, 1, 0, 0, 0);   CHECK_STR(evt("today"), "error");
    set_wall(2026, 0, 1, 0, 0, 0);    CHECK_STR(evt("today"), "error");

    /* THE WALL CLOCK IS READ AT MOST ONCE PER EVENT. Reading it per operand would
     * put device I/O in the middle of the evaluator. */
    set_wall(2026, 7, 29, 14, 3, 7);
    wall_calls = 0;
    CHECK_STR(evt("cat(cat(clock, ' '), today)"), "14:03:07 2026-07-29");
    CHECK_EQ(wall_calls, 1);

    /* AND `now` READS IT ZERO TIMES. This is the assertion the suite was
     * missing, and its absence hid a real cost: `now` is monotonic and needs
     * nothing from the CMOS, but time_value() asked app_env_now() for both and
     * threw the wall reading away, so the stopwatch idiom — the one document
     * whose only time leaf IS `now` — drove an rtc_read() on every event. On a
     * chip whose UIP bit never clears that read burns RTC_UIP_WAIT_MS = 20 ms.
     * Every header in apps/ claims this laziness; only `clock` had it.
     *
     * set_wall() re-arms the snapshot (it goes through app_set_time_source),
     * so each measurement below starts from "unsampled" rather than from a
     * cached reading — without that these would pass for the wrong reason. */
    set_wall(2026, 7, 29, 14, 3, 7);
    wall_calls = 0;
    (void)evt("text(now)");
    (void)evt("now + now * 2");
    CHECK_EQ(wall_calls, 0);

    /* Mixing the two still costs exactly one. */
    set_wall(2026, 7, 29, 14, 3, 7);
    wall_calls = 0;
    (void)evt("cat(text(now), clock)");
    CHECK_EQ(wall_calls, 1);
}

/* A WINDOW TOO SMALL FOR ITS OWN GRID IS REFUSED, WITH THE NUMBERS.
 *
 * gui_inset() and gui_grid() both return an empty rect when the area cannot
 * hold the padding or the cells, and place()/instantiate() used to accept that
 * without comment: every widget was added with rect 0,0 0x0. The launch then
 * answered "with 12 widget(s) ... It is live: a real mouse can click it",
 * action=state listed all twelve with no geometry, gui_click reported pressing
 * one — and the operator saw an EMPTY WINDOW. That is the exact failure
 * app.h's DECISIONS single out as the most confusing a model can be left to
 * debug, and it was the only bound in runtime.c that was not checked. */
static void test_a_window_too_small_for_its_grid_is_refused(void) {
    fixture();

    /* The measured worst case: 32x32 with a 4x3 grid used to launch clean with
     * twelve zero-area widgets. */
    reject("{\"title\":\"C\",\"width\":32,\"height\":32,"
           "\"grid\":{\"rows\":4,\"cols\":3},\"widgets\":["
           "{\"kind\":\"button\",\"text\":\"0\",\"row\":0,\"col\":0}]}",
           "", "too small for its own grid");

    /* The message has to be arithmetic the model can act on, not a complaint. */
    CHECK_CONTAINS(g_err.msg, "4x3 grid");
    CHECK_CONTAINS(g_err.msg, "\"width\":");
    CHECK_CONTAINS(g_err.msg, "\"height\":");

    /* The stopwatch shape that started this: 200x44, four rows. */
    reject("{\"title\":\"Stopwatch\",\"width\":200,\"height\":44,"
           "\"grid\":{\"rows\":4,\"cols\":1},\"widgets\":["
           "{\"kind\":\"field\",\"name\":\"out\",\"text\":\"0.0 s\","
           "\"row\":0,\"col\":0}]}", "", "too small");

    /* AND THE NUMBER IT QUOTES IS THE ONE THAT WORKS. Parse the "width" the
     * rejection demands back out of it, build that document, and launch it —
     * the same trick test_app.c uses on the retry skeleton, so this is a
     * promise rather than a paraphrase. */
    const char *pw = strstr(g_err.msg, "\"width\":");
    const char *ph = strstr(g_err.msg, "\"height\":");
    CHECK(pw != NULL && ph != NULL);
    int need_w = pw ? atoi(pw + 8) : 0;
    int need_h = ph ? atoi(ph + 9) : 0;
    CHECK(need_w > 0 && need_h > 0);
    char fixed[320];
    snprintf(fixed, sizeof fixed,
             "{\"title\":\"Stopwatch\",\"width\":%d,\"height\":%d,"
             "\"grid\":{\"rows\":4,\"cols\":1},\"widgets\":["
             "{\"kind\":\"field\",\"name\":\"out\",\"text\":\"0.0 s\","
             "\"row\":0,\"col\":0}]}", need_w, need_h);
    uint32_t id = 0;
    CHECK_EQ(launch(fixed, &id), APP_OK);
    CHECK(id != 0);

    /* And what launched is genuinely clickable, not merely non-zero. */
    gui_window_t *W = gui_window(id);
    CHECK(W != NULL);
    if (W) for (uint16_t i = 0; i < W->nwidgets; i++) {
        CHECK(W->widgets[i].r.w >= APP_MIN_CELL);
        CHECK(W->widgets[i].r.h >= 1);
    }

    /* Comfortable windows are untouched: every shipped example still launches
     * (test_examples_match_disk and the per-example tests cover the rest). */
    fixture();
    uint32_t ok = 0;
    CHECK_EQ(launch("{\"title\":\"Hi\",\"width\":200,\"height\":90,"
                    "\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":["
                    "{\"kind\":\"label\",\"text\":\"hello\",\"row\":0,"
                    "\"col\":0}]}", &ok), APP_OK);
}

/* A SELECTOR NAMES A "name" OR A "tag", NEVER THE VISIBLE TEXT — and that was
 * the single most repeated model mistake in the live transcripts. The rejection
 * now says so, and lists tags as well as names: put_selectors() used to prefer
 * a widget's name and fall back to its tag, so a keypad whose buttons each have
 * a name AND the shared tag the tool description recommends listed k1, k2, k3
 * and never mentioned the tag at all. A tag is compile-time-only, so that error
 * was the only place it could ever have been seen. */
static void test_a_mistyped_selector_names_the_fix(void) {
    fixture();

    /* The visible text, which is what a model reaches for first. */
    reject("{\"title\":\"P\",\"width\":200,\"height\":140,"
           "\"grid\":{\"rows\":2,\"cols\":2},\"widgets\":["
           "{\"kind\":\"field\",\"name\":\"out\",\"text\":\"\",\"row\":0,"
           "\"col\":0,\"colspan\":2},"
           "{\"kind\":\"button\",\"name\":\"k1\",\"tag\":\"digit\","
           "\"text\":\"7\",\"row\":1,\"col\":0},"
           "{\"kind\":\"button\",\"name\":\"k2\",\"tag\":\"digit\","
           "\"text\":\"8\",\"row\":1,\"col\":1}],"
           "\"on\":[{\"click\":\"7\",\"do\":[{\"set\":\"out\","
           "\"to\":\"cat(out,key)\"}]}]}",
           "on[0].click", "VISIBLE TEXT");
    CHECK_CONTAINS(g_err.msg, "\"name\"");
    CHECK_CONTAINS(g_err.msg, "\"tag\"");

    /* A tag that exists must appear in the list, even when every widget that
     * carries it also has a name. */
    reject("{\"title\":\"P\",\"width\":200,\"height\":140,"
           "\"grid\":{\"rows\":2,\"cols\":2},\"widgets\":["
           "{\"kind\":\"field\",\"name\":\"out\",\"text\":\"\",\"row\":0,"
           "\"col\":0,\"colspan\":2},"
           "{\"kind\":\"button\",\"name\":\"k1\",\"tag\":\"digit\","
           "\"text\":\"7\",\"row\":1,\"col\":0},"
           "{\"kind\":\"button\",\"name\":\"k2\",\"tag\":\"digit\","
           "\"text\":\"8\",\"row\":1,\"col\":1}],"
           "\"on\":[{\"click\":\"digits\",\"do\":[{\"set\":\"out\","
           "\"to\":\"cat(out,key)\"}]}]}",
           "on[0].click", "digit");
    CHECK_CONTAINS(g_err.msg, "Available:");
    CHECK_CONTAINS(g_err.msg, "k1");
    CHECK_CONTAINS(g_err.msg, "digit");
    /* Listed once, not once per widget that carries it. */
    {
        const char *s = strstr(g_err.msg, "Available:");
        int n = 0;
        for (const char *q = s; q && (q = strstr(q, "digit")); q += 5) n++;
        CHECK_EQ(n, 1);
    }
}

static void test_new_names_are_reserved_and_listed(void) {
    fixture();

    /* A var called `clock` would be unreachable, because the leaf resolves
     * first — so it is refused by name rather than silently shadowed. */
    reject("{\"vars\":{\"clock\":0},\"grid\":{\"rows\":1,\"cols\":1},"
           "\"widgets\":[{\"kind\":\"label\",\"text\":\"x\",\"row\":0,\"col\":0}]}",
           "vars.clock", "reserved word");
    reject("{\"vars\":{\"now\":0},\"grid\":{\"rows\":1,\"cols\":1},"
           "\"widgets\":[{\"kind\":\"label\",\"text\":\"x\",\"row\":0,\"col\":0}]}",
           "vars.now", "reserved word");
    reject("{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":\"label\","
           "\"name\":\"rand\",\"text\":\"x\",\"row\":0,\"col\":0}]}",
           "widgets[0].name", "reserved word");

    /* And an unknown function still names every function that DOES exist,
     * including the new ones: one turn to fix a wrong guess. */
    ex_reset();
    app_error_t err;
    uint16_t    idx = 0;
    memset(&err, 0, sizeof err);
    CHECK(app_expr_compile(&ex, "random(6)", &idx, &err) != APP_OK);
    CHECK_CONTAINS(err.msg, "rand");
    CHECK_CONTAINS(err.msg, "round");
    CHECK_CONTAINS(err.msg, "at");

    memset(&err, 0, sizeof err);
    CHECK(app_expr_compile(&ex, "tyme", &idx, &err) != APP_OK);
    CHECK_CONTAINS(err.msg, "now");
    CHECK_CONTAINS(err.msg, "clock");

    /* Arity is checked, with the count in the message. */
    memset(&err, 0, sizeof err);
    CHECK(app_expr_compile(&ex, "at('abc')", &idx, &err) != APP_OK);
    CHECK_CONTAINS(err.msg, "at()");
    memset(&err, 0, sizeof err);
    CHECK(app_expr_compile(&ex, "rand(1,2)", &idx, &err) != APP_OK);
    memset(&err, 0, sizeof err);
    CHECK(app_expr_compile(&ex, "now(1)", &idx, &err) != APP_OK);
    CHECK_CONTAINS(err.msg, "unknown function");
}

/* ====================================================================== */
/* 2. the tick event                                                       */
/* ====================================================================== */

/* One counter app: a var incremented by a tick, mirrored into a label. */
static const char *tick_doc(int period) {
    static char doc[512];
    snprintf(doc, sizeof doc,
             "{\"title\":\"Ticker\",\"width\":200,\"height\":80,"
             "\"grid\":{\"rows\":1,\"cols\":1},\"vars\":{\"n\":0},"
             "\"widgets\":[{\"kind\":\"label\",\"name\":\"out\",\"text\":\"0\","
             "\"row\":0,\"col\":0}],"
             "\"on\":[{\"tick\":%d,\"do\":["
             "{\"set\":\"n\",\"to\":\"n + 1\"},"
             "{\"set\":\"out\",\"to\":\"text(n)\"}]}]}", period);
    return doc;
}

static void test_tick_runs_on_period(void) {
    fixture();
    uint32_t id = 0;
    CHECK_EQ(launch(tick_doc(1000), &id), APP_OK);

    /* The first call fires: a clock that showed nothing for its first second
     * would look broken. */
    CHECK_EQ(app_tick(), 1);
    CHECK_STR(var_of(id, "n"), "1");
    CHECK_CONTAINS(describe(id), "name=out text=\"1\"");

    /* Then nothing until the period has passed, however many times it is
     * called — the idle loop calls this thousands of times a second. */
    for (int i = 0; i < 1000; i++) CHECK_EQ(app_tick(), 0);
    CHECK_STR(var_of(id, "n"), "1");

    fake_ms += 999;
    CHECK_EQ(app_tick(), 0);
    fake_ms += 1;
    CHECK_EQ(app_tick(), 1);
    CHECK_STR(var_of(id, "n"), "2");

    /* NO CATCH-UP. Ten seconds pass in one jump (the machine was talking to the
     * model): the handler runs ONCE, not ten times. This is the property that
     * keeps a single-threaded kernel from wedging after any long pause. */
    fake_ms += 10000;
    CHECK_EQ(app_tick(), 1);
    CHECK_STR(var_of(id, "n"), "3");
    CHECK_EQ(app_tick(), 0);
    fake_ms += 1000;
    CHECK_EQ(app_tick(), 1);
    CHECK_STR(var_of(id, "n"), "4");

    /* A CLOCK THAT JUMPS BACKWARDS must not silence the app for hours. */
    fake_ms = 5;
    CHECK_EQ(app_tick(), 1);
    CHECK_STR(var_of(id, "n"), "5");

    /* Closing the app stops the ticks; nothing else is running. */
    CHECK_EQ(app_close(id), APP_OK);
    fake_ms += 100000;
    CHECK_EQ(app_tick(), 0);
    CHECK_EQ(app_count(), 0);
}

static void test_tick_costs_nothing_when_idle(void) {
    fixture();

    /* No apps at all: app_tick() has nothing to scan and nothing to read. */
    CHECK_EQ(app_tick(), 0);

    /* An app with no tick handler: still no periodic work, so a document that
     * never asked for one pays nothing. */
    uint32_t id = 0;
    uint32_t ticks_before = app_stats()->ticks;
    CHECK_EQ(launch("{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":"
                    "\"button\",\"name\":\"b\",\"text\":\"x\",\"row\":0,"
                    "\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":[]}]}", &id),
             APP_OK);
    for (int i = 0; i < 100; i++) CHECK_EQ(app_tick(), 0);
    CHECK_EQ(app_stats()->ticks, ticks_before);
}

static void test_tick_bounds(void) {
    fixture();

    /* The period is a bound with its numbers in the message. */
    reject(tick_doc(49), "on[0].tick", "50");
    reject(tick_doc(0), "on[0].tick", "milliseconds");
    reject(tick_doc(-1000), "on[0].tick", "milliseconds");
    reject(tick_doc(3600001), "on[0].tick", "3600000");

    /* Wrong type, and the fractional case json_int would truncate. */
    reject("{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":\"label\","
           "\"text\":\"x\",\"row\":0,\"col\":0}],"
           "\"on\":[{\"tick\":\"1000\",\"do\":[]}]}",
           "on[0].tick", "whole number of milliseconds");
    reject("{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":\"label\","
           "\"text\":\"x\",\"row\":0,\"col\":0}],"
           "\"on\":[{\"tick\":100.5,\"do\":[]}]}",
           "on[0].tick", "whole number of milliseconds");

    /* One event per handler, still. */
    reject("{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":\"button\","
           "\"name\":\"b\",\"text\":\"x\",\"row\":0,\"col\":0}],"
           "\"on\":[{\"tick\":100,\"click\":\"b\",\"do\":[]}]}",
           "on[0]", "exactly one event");

    /* At most APP_MAX_TICKS periodic handlers: the bound on how much work a
     * document can demand per period. */
    char doc[1024];
    int  o = snprintf(doc, sizeof doc,
                      "{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":"
                      "\"label\",\"name\":\"out\",\"text\":\"x\",\"row\":0,"
                      "\"col\":0}],\"on\":[");
    for (int i = 0; i < APP_MAX_TICKS + 1; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o,
                      "%s{\"tick\":100,\"do\":[{\"set\":\"out\",\"to\":\"'x'\"}]}",
                      i ? "," : "");
    snprintf(doc + o, sizeof doc - (size_t)o, "]}");
    reject(doc, "on[4].tick", "at most 4");

    /* Exactly APP_MAX_TICKS is accepted, and all of them fire. */
    o = snprintf(doc, sizeof doc,
                 "{\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":"
                 "\"label\",\"name\":\"out\",\"text\":\"x\",\"row\":0,"
                 "\"col\":0}],\"on\":[");
    for (int i = 0; i < APP_MAX_TICKS; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o,
                      "%s{\"tick\":100,\"do\":[{\"set\":\"out\",\"to\":\"'x'\"}]}",
                      i ? "," : "");
    snprintf(doc + o, sizeof doc - (size_t)o, "]}");
    uint32_t id = 0;
    CHECK_EQ(launch(doc, &id), APP_OK);
    CHECK_EQ(app_tick(), APP_MAX_TICKS);
}

static void test_tick_is_still_bounded_by_steps(void) {
    fixture();

    /* A tick handler is the same bounded statement list a click runs: the step
     * budget cuts it off and says so, and the app stays alive. */
    char doc[8192];
    int  o = snprintf(doc, sizeof doc,
                      "{\"grid\":{\"rows\":1,\"cols\":1},\"vars\":{\"n\":0},"
                      "\"widgets\":[{\"kind\":\"label\",\"name\":\"out\","
                      "\"text\":\"x\",\"row\":0,\"col\":0}],"
                      "\"on\":[{\"tick\":50,\"do\":[");
    for (int i = 0; i < APP_MAX_EXPRS - 6; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o,
                      "%s{\"set\":\"n\",\"to\":\"n + 1\"}", i ? "," : "");
    snprintf(doc + o, sizeof doc - (size_t)o, "]}]}");

    uint32_t id = 0;
    CHECK_EQ(launch(doc, &id), APP_OK);
    uint32_t steps_before = app_stats()->steps;
    CHECK_EQ(app_tick(), 1);
    /* 90 statements, so well inside APP_MAX_STEPS: the point is that the count is
     * bounded by the DOCUMENT's length, which was checked before it ran — a
     * document cannot ask for more work per tick than it spells out. */
    CHECK(app_stats()->steps - steps_before <= APP_MAX_STEPS);
    CHECK_STR(var_of(id, "n"), "90");

    /* And a hostile clock cannot multiply that: one run per call, whatever the
     * clock does. */
    uint32_t s2 = app_stats()->steps;
    fake_ms += 1000000;
    CHECK_EQ(app_tick(), 1);
    CHECK(app_stats()->steps - s2 <= APP_MAX_STEPS);
}

static void test_tick_sees_a_consistent_snapshot(void) {
    fixture();

    /* Two reads of `second` in one tick must agree even though the source moves
     * between them — one snapshot per event is what makes a rendered clock
     * consistent with the numbers beside it. */
    uint32_t id = 0;
    CHECK_EQ(launch("{\"grid\":{\"rows\":2,\"cols\":1},\"vars\":{\"a\":0,\"b\":0},"
                    "\"widgets\":[{\"kind\":\"label\",\"name\":\"x\",\"text\":\"\","
                    "\"row\":0,\"col\":0},{\"kind\":\"label\",\"name\":\"y\","
                    "\"text\":\"\",\"row\":1,\"col\":0}],"
                    "\"on\":[{\"tick\":100,\"do\":["
                    "{\"set\":\"a\",\"to\":\"second\"},"
                    "{\"set\":\"b\",\"to\":\"second\"},"
                    "{\"set\":\"x\",\"to\":\"clock\"},"
                    "{\"set\":\"y\",\"to\":\"clock\"}]}]}", &id), APP_OK);
    wall_calls = 0;
    CHECK_EQ(app_tick(), 1);
    CHECK_STR(var_of(id, "a"), var_of(id, "b"));
    CHECK_EQ(wall_calls, 1);                    /* one CMOS read for the tick */
    CHECK_CONTAINS(describe(id), "name=x text=\"14:03:07\"");
    CHECK_CONTAINS(describe(id), "name=y text=\"14:03:07\"");

    /* The next tick sees the new time. */
    set_wall(2026, 7, 29, 14, 3, 8);
    fake_ms += 100;
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=x text=\"14:03:08\"");
}

static void test_now_advances_across_events(void) {
    fixture();

    /* The stopwatch shape, driven entirely by pinned time: a click records the
     * reading and the tick shows the difference. */
    uint32_t id = 0;
    CHECK_EQ(launch(app_example_doc("stopwatch"), &id), APP_OK);

    /* Before start, ticks do nothing visible. */
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=elapsed text=\"0.0\"");

    /* Widget ids are index+1 in document order: 1 display, 2 start, 3 stop,
     * 4 reset. */
    fake_ms = 200000;
    click(id, 2);                       /* start */
    CHECK_STR(var_of(id, "run"), "1");

    fake_ms += 2500;
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=elapsed text=\"2.5 s\"");

    fake_ms += 7250;
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=elapsed text=\"9.8 s\"");   /* 9.75 -> 9.8 */

    /* stop freezes it however much time passes... */
    click(id, 3);
    fake_ms += 60000;
    CHECK_EQ(app_tick(), 1);                        /* the handler still runs */
    CHECK_CONTAINS(describe(id), "name=elapsed text=\"9.8 s\"");

    /* ...and start resumes from where it stopped rather than from the boot. */
    click(id, 2);
    fake_ms += 1000;
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=elapsed text=\"10.8 s\"");

    /* reset really resets. */
    click(id, 4);
    CHECK_CONTAINS(describe(id), "name=elapsed text=\"0.0\"");
    fake_ms += 5000;
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=elapsed text=\"0.0\"");
}

static void test_ticks_across_several_apps(void) {
    fixture();

    /* Different periods in different apps, all driven from one app_tick(). */
    uint32_t a = 0, b = 0;
    CHECK_EQ(launch(tick_doc(100), &a), APP_OK);
    CHECK_EQ(launch(tick_doc(1000), &b), APP_OK);
    CHECK_EQ(app_tick(), 2);                       /* both are due at once */
    CHECK_STR(var_of(a, "n"), "1");
    CHECK_STR(var_of(b, "n"), "1");

    for (int i = 0; i < 9; i++) { fake_ms += 100; CHECK_EQ(app_tick(), 1); }
    CHECK_STR(var_of(a, "n"), "10");
    CHECK_STR(var_of(b, "n"), "1");
    fake_ms += 100;
    CHECK_EQ(app_tick(), 2);                       /* the slow one comes due */
    CHECK_STR(var_of(a, "n"), "11");
    CHECK_STR(var_of(b, "n"), "2");

    /* Closing one leaves the other ticking. */
    CHECK_EQ(app_close(a), APP_OK);
    fake_ms += 1000;
    CHECK_EQ(app_tick(), 1);
    CHECK_STR(var_of(b, "n"), "3");

    /* And a window closed from underneath the app (the close box) stops it: the
     * runtime learns it from GUI_EV_CLOSE, so there is one teardown path. */
    gui_window_close(b);
    fake_ms += 1000;
    CHECK_EQ(app_tick(), 0);
    CHECK_EQ(app_count(), 0);
}

/* ====================================================================== */
/* 3. adversarial documents that abuse the additions                       */
/* ====================================================================== */

static void test_hostile_documents(void) {
    fixture();

    /* A tick handler that writes nothing but errors: the app latches "error",
     * says why in its note, and keeps running. */
    uint32_t id = 0;
    CHECK_EQ(launch("{\"grid\":{\"rows\":1,\"cols\":1},"
                    "\"widgets\":[{\"kind\":\"label\",\"name\":\"out\","
                    "\"text\":\"-\",\"row\":0,\"col\":0}],"
                    "\"on\":[{\"tick\":50,\"do\":[{\"set\":\"out\","
                    "\"to\":\"at('abc', rand(1000000))\"}]}]}", &id), APP_OK);
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "text=\"error\"");
    CHECK_CONTAINS(app_last_error(id), "error value");
    fake_ms += 50;
    CHECK_EQ(app_tick(), 1);                        /* still alive */
    app_close(id);

    /* A tick handler nested to the limit, firing every 50 ms: bounded before it
     * ran, so it is bounded while it runs. */
    char doc[4096];
    int  o = snprintf(doc, sizeof doc,
                      "{\"grid\":{\"rows\":1,\"cols\":1},\"vars\":{\"n\":0},"
                      "\"widgets\":[{\"kind\":\"label\",\"name\":\"out\","
                      "\"text\":\"x\",\"row\":0,\"col\":0}],"
                      "\"on\":[{\"tick\":50,\"do\":[");
    for (int i = 0; i < APP_MAX_IF_DEPTH; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o,
                      "{\"if\":\"1\",\"then\":[");
    o += snprintf(doc + o, sizeof doc - (size_t)o,
                  "{\"set\":\"n\",\"to\":\"n + 1\"}");
    for (int i = 0; i < APP_MAX_IF_DEPTH; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o, "]}");
    snprintf(doc + o, sizeof doc - (size_t)o, "]}]}");
    id = 0;
    /* Either it is accepted and bounded, or it is refused with the depth named;
     * both are correct, and neither may crash. */
    if (launch(doc, &id) == APP_OK) {
        CHECK_EQ(app_tick(), 1);
        CHECK(app_count() == 1);
        app_close(id);
    } else {
        CHECK_CONTAINS(g_err.msg, "nested");
    }

    /* rand() and at() inside a condition, and a tick whose whole body is a
     * comparison against ERR: total evaluation means no trap, ever. */
    fixture();
    id = 0;
    CHECK_EQ(launch("{\"grid\":{\"rows\":1,\"cols\":1},\"vars\":{\"n\":0},"
                    "\"widgets\":[{\"kind\":\"label\",\"name\":\"out\","
                    "\"text\":\"x\",\"row\":0,\"col\":0}],"
                    "\"on\":[{\"tick\":50,\"do\":["
                    "{\"if\":\"at('abc', rand(9)) == 'z'\",\"then\":["
                    "{\"set\":\"n\",\"to\":\"n + 1\"}],\"else\":["
                    "{\"set\":\"out\",\"to\":\"text(rand(6) + now * 0)\"}]}]}]}",
                    &id), APP_OK);
    for (int i = 0; i < 50; i++) { CHECK_EQ(app_tick(), 1); fake_ms += 50; }
    CHECK_EQ(app_count(), 1);
    CHECK_EQ(kpanic_hit, 0);

    /* A tick handler on an app whose widget it writes was made unclickable
     * scenery: a tick has no widget, so nothing depends on hit testing. */
    fixture();
    id = 0;
    CHECK_EQ(launch("{\"grid\":{\"rows\":1,\"cols\":1},"
                    "\"widgets\":[{\"kind\":\"field\",\"name\":\"ro\","
                    "\"text\":\"-\",\"readonly\":true,\"row\":0,\"col\":0}],"
                    "\"on\":[{\"tick\":50,\"do\":[{\"set\":\"ro\","
                    "\"to\":\"clock\"}]}]}", &id), APP_OK);
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "text=\"14:03:07\"");
    CHECK_CONTAINS(describe(id), "not-clickable");
}

static void test_sources_can_be_restored(void) {
    fixture();

    /* NULL restores the machine's own sources. On the host that means the wall
     * clock is unavailable (there is no CMOS here) and `now` comes from millis(),
     * which must be monotonic and must not crash. This is the path the kernel
     * takes, exercised. */
    app_set_time_source(NULL, NULL);
    app_set_random_source(NULL);

    uint32_t id = 0;
    CHECK_EQ(launch("{\"grid\":{\"rows\":1,\"cols\":1},\"vars\":{\"a\":0},"
                    "\"widgets\":[{\"kind\":\"label\",\"name\":\"out\","
                    "\"text\":\"x\",\"row\":0,\"col\":0}],"
                    "\"on\":[{\"tick\":50,\"do\":["
                    "{\"set\":\"a\",\"to\":\"now\"},"
                    "{\"set\":\"out\",\"to\":\"text(rand(1000) + 0)\"}]}]}",
                    &id), APP_OK);
    CHECK_EQ(app_tick(), 1);
    CHECK(strcmp(var_of(id, "a"), "error") != 0);      /* now always works */
    CHECK(strcmp(describe(id), "") != 0);
    CHECK_EQ(kpanic_hit, 0);

    /* Put the pinned sources back for whatever runs next. */
    app_set_time_source(ms_source, wall_source);
    app_set_random_source(rand_source);
}

/* ====================================================================== */
/* 4. the shipped examples                                                 */
/* ====================================================================== */

/* The .json on disk and the string the kernel embeds must be the same bytes, or
 * the artifact a human reads is not the app the machine runs. */
static void check_embedded(const char *file, const char *embedded) {
    char path[256];
    snprintf(path, sizeof path, "%s/apps/examples/%s",
             getenv("FABLEOS_SRCDIR") ? getenv("FABLEOS_SRCDIR") : ".", file);
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        /* Run from another directory: skip rather than fail, but say so, because
         * a silent skip of a drift check is how drift ships. */
        printf("    (note: %s not readable, skipping the byte-diff)\n", path);
        return;
    }
    static char disk[APP_DOC_MAX * 2];
    size_t n = fread(disk, 1, sizeof disk - 1, fh);
    fclose(fh);
    disk[n] = '\0';
    th_checks++;
    if (strcmp(disk, embedded) != 0) {
        th_fails++;
        printf("    FAIL apps/examples/%s differs from the embedded copy; run "
               "python3 apps/examples/gen_header.py\n", file);
    }
}

static void test_examples_match_disk(void) {
    check_embedded("hello.json",     APP_HELLO_JSON);
    check_embedded("clock.json",     APP_CLOCK_JSON);
    check_embedded("stopwatch.json", APP_STOPWATCH_JSON);
    check_embedded("dice.json",      APP_DICE_JSON);
    check_embedded("password.json",  APP_PASSWORD_JSON);

    /* And the table the kernel exposes lists every one of them by name. */
    CHECK_EQ(app_example_count(), 6);
    static const char *want[] = { "hello", "clock", "stopwatch", "dice",
                                  "password", "calculator" };
    for (int i = 0; i < 6; i++) {
        CHECK_STR(app_example_name(i), want[i]);
        CHECK(app_example_doc(want[i]) != NULL);
    }
    CHECK(app_example_name(-1) == NULL);
    CHECK(app_example_name(6) == NULL);
    CHECK(app_example_doc("nope") == NULL);
    CHECK(app_example_doc("") == NULL);
    CHECK_STR(app_example_doc("calculator"), app_example_calculator());
}

static void test_example_hello(void) {
    fixture();
    uint32_t id = 0;
    CHECK_EQ(launch(app_example_doc("hello"), &id), APP_OK);
    CHECK_STR(app_title(id), "Hello");
    CHECK_CONTAINS(describe(id), "label id=1 name=greeting text=\"hello\"");
    /* A document with no handlers is a legal app and must not tick or fault. */
    CHECK_EQ(app_tick(), 0);
    CHECK_EQ(gui_window_count(), 1);
}

static void test_example_clock(void) {
    fixture();
    uint32_t id = 0;
    CHECK_EQ(launch(app_example_doc("clock"), &id), APP_OK);
    CHECK_STR(app_title(id), "Clock");

    /* The first tick fills both labels in, from the pinned clock. */
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=time text=\"14:03:07\"");
    CHECK_CONTAINS(describe(id), "name=date text=\"2026-07-29\"");

    /* Half a second later it follows the clock. */
    set_wall(2026, 12, 31, 23, 59, 59);
    fake_ms += 500;
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=time text=\"23:59:59\"");
    CHECK_CONTAINS(describe(id), "name=date text=\"2026-12-31\"");

    /* On a machine with no readable clock it says "error" rather than lying. */
    wall_ok = 0;
    fake_ms += 500;
    CHECK_EQ(app_tick(), 1);
    CHECK_CONTAINS(describe(id), "name=time text=\"error\"");
}

static void test_example_dice(void) {
    fixture();
    uint32_t id = 0;
    CHECK_EQ(launch(app_example_doc("dice"), &id), APP_OK);
    CHECK_STR(app_title(id), "Dice");

    /* Pinned draws: two 5s is "you rolled 12". */
    rand_n = 1; rand_queue[0] = ~0ull;
    click(id, 2);
    CHECK_CONTAINS(describe(id), "text=\"you rolled 12\"");
    rand_queue[0] = 0;
    click(id, 2);
    CHECK_CONTAINS(describe(id), "text=\"you rolled 2\"");

    /* And free-running: every total stays inside 2..12 over many rolls. */
    rand_n = 0;
    for (int i = 0; i < 500; i++) {
        click(id, 2);
        int64_t t = 0;
        CHECK_EQ(app_num_parse(var_of(id, "total"), &t), 0);
        CHECK(t >= 2 * APP_NUM_SCALE && t <= 12 * APP_NUM_SCALE);
    }
    CHECK_EQ(kpanic_hit, 0);
}

static void test_example_password(void) {
    fixture();
    uint32_t id = 0;
    CHECK_EQ(launch(app_example_doc("password"), &id), APP_OK);
    CHECK_STR(app_title(id), "Password");

    /* All draws zero: ten of the alphabet's first character. */
    rand_n = 1; rand_queue[0] = 0;
    click(id, 2);
    CHECK_CONTAINS(describe(id), "text=\"aaaaaaaaaa\"");

    /* All draws at the top: ten of the last, which is the '9' the alphabet
     * ends with — so the index really reaches len-1 and never past it. */
    rand_queue[0] = ~0ull;
    click(id, 2);
    CHECK_CONTAINS(describe(id), "text=\"9999999999\"");

    /* Free-running: always ten characters, always from the alphabet, never an
     * error value (which is what an off-by-one in at() would show). */
    rand_n = 0;
    static const char *pool = "abcdefghijkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 200; i++) {
        click(id, 2);
        const char *d = describe(id);
        const char *q = strstr(d, "name=out text=\"");
        th_checks++;
        if (!q) { th_fails++; printf("    FAIL no out widget in %s\n", d); break; }
        q += strlen("name=out text=\"");
        const char *e = strchr(q, '"');
        size_t len = e ? (size_t)(e - q) : 0;
        if (len != 10) {
            th_fails++;
            printf("    FAIL password is %d chars: %.20s\n", (int)len, q);
            break;
        }
        for (size_t k = 0; k < len; k++) {
            if (!strchr(pool, q[k])) {
                th_fails++;
                printf("    FAIL '%c' is not in the alphabet\n", q[k]);
                break;
            }
        }
    }
}

static void test_calculator_still_works(void) {
    fixture();

    /* THE REGRESSION CANARY. The calculator is the app every one of these
     * additions had to leave alone: same document, same clicks, same answer.
     * Widget ids are index+1 in document order — display 1, then the keypad. */
    uint32_t id = 0;
    CHECK_EQ(launch(app_example_calculator(), &id), APP_OK);
    CHECK_STR(app_title(id), "Calculator");

    /* Find keys by their text, so this does not encode the keypad's order. */
    gui_window_t *win = gui_window(id);
    CHECK(win != NULL);

    struct { const char *k; const char *want; } seq[] = {
        { "7", "7" }, { "+", "7" }, { "8", "8" }, { "=", "15" },
    };
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        int hit = 0;
        for (uint16_t w = 0; w < win->nwidgets; w++)
            if (!strcmp(win->widgets[w].text, seq[i].k) &&
                !(win->widgets[w].state & GUI_W_NOHIT)) {
                click(id, win->widgets[w].id);
                hit = 1;
                break;
            }
        CHECK_EQ(hit, 1);
        CHECK_STR(win->widgets[0].text, seq[i].want);
    }
    CHECK_STR(var_of(id, "acc"), "15");

    /* Division by zero still latches "error" rather than trapping. */
    for (const char *k = "9/0="; *k; k++) {
        char one[2] = { *k, 0 };
        for (uint16_t w = 0; w < win->nwidgets; w++)
            if (!strcmp(win->widgets[w].text, one) &&
                !(win->widgets[w].state & GUI_W_NOHIT)) {
                click(id, win->widgets[w].id);
                break;
            }
    }
    CHECK_STR(win->widgets[0].text, "error");

    /* And no tick handler means no periodic work for it. */
    CHECK_EQ(app_tick(), 0);
}

/* Everything at once, the way a desktop ends up: the instance pool is 4, so four
 * apps with ticks and clicks running together must stay within every bound. */
static void test_a_desktop_of_apps(void) {
    fixture();
    uint32_t ids[APP_MAX_INSTANCES];
    static const char *names[] = { "clock", "stopwatch", "dice", "hello" };

    for (int i = 0; i < APP_MAX_INSTANCES; i++) {
        ids[i] = 0;
        CHECK_EQ(launch(app_example_doc(names[i]), &ids[i]), APP_OK);
    }
    CHECK_EQ(app_count(), APP_MAX_INSTANCES);

    /* A fifth is refused with the ids of the four that are running. */
    uint32_t extra = 0;
    CHECK(launch(app_example_doc("password"), &extra) != APP_OK);
    CHECK_EQ(extra, 0);
    CHECK_CONTAINS(g_err.msg, "already running");

    /* Start the stopwatch, then run a simulated minute in 100 ms steps. Four
     * cascaded windows overlap, and a click goes to whatever is on top, so the
     * one being driven is raised first — exactly what a person does with a
     * mouse. */
    gui_window_raise(ids[1]);
    click(ids[1], 2);
    for (int i = 0; i < 600; i++) {
        fake_ms += 100;
        int ran = app_tick();
        CHECK(ran >= 1 && ran <= APP_MAX_INSTANCES * APP_MAX_TICKS);
    }
    CHECK_CONTAINS(describe(ids[0]), "text=\"14:03:07\"");
    CHECK_CONTAINS(describe(ids[1]), "name=elapsed text=\"60 s\"");
    CHECK_EQ(app_count(), APP_MAX_INSTANCES);
    CHECK_EQ(kpanic_hit, 0);

    app_close_all();
    CHECK_EQ(app_count(), 0);
    CHECK_EQ(app_tick(), 0);
}

/* ====================================================================== */

int main(void) {
    printf("test_app_format: the format additions (tick, time, rand, at, "
           "round)\n");

    RUN(test_round);
    RUN(test_at);
    RUN(test_rand_bounds_and_spread);
    RUN(test_time_leaves);
    RUN(test_a_window_too_small_for_its_grid_is_refused);
    RUN(test_a_mistyped_selector_names_the_fix);
    RUN(test_new_names_are_reserved_and_listed);

    RUN(test_tick_runs_on_period);
    RUN(test_tick_costs_nothing_when_idle);
    RUN(test_tick_bounds);
    RUN(test_tick_is_still_bounded_by_steps);
    RUN(test_tick_sees_a_consistent_snapshot);
    RUN(test_now_advances_across_events);
    RUN(test_ticks_across_several_apps);

    RUN(test_hostile_documents);
    RUN(test_sources_can_be_restored);

    RUN(test_examples_match_disk);
    RUN(test_example_hello);
    RUN(test_example_clock);
    RUN(test_example_dice);
    RUN(test_example_password);
    RUN(test_calculator_still_works);
    RUN(test_a_desktop_of_apps);

    return th_report("app format");
}
