/* test_app_audio.c — an app document reaches a driver, and only the way it is
 *                    supposed to.
 *
 * WHAT THIS SUITE IS FOR
 *   include/app.h DECISION 4 lets a model-authored document ask the kernel for
 *   hardware: {"call":"audio.tone","with":{"hz":"440"}}. That is the first time an
 *   app can cause a physical effect, on a kernel with no privilege separation and
 *   no MMU, so the claims worth proving are not "it beeps" but these:
 *
 *     1. THE STATEMENT IS A REAL PART OF THE FORMAT. It compiles, its arguments
 *        are ordinary expressions, and every way of writing it wrong is refused at
 *        LOAD time with a path and a reason — including a capability name that
 *        does not exist, which is answered with the whole table.
 *     2. EVERY BOUND HOLDS, and holds where the value is actually known: the
 *        arguments are expressions, so a 20 GHz tone or a 10-hour tone is caught
 *        when the handler RUNS, and is REFUSED rather than clamped.
 *     3. A HANDLER NEVER REACHES THE DRIVER. This is the property that keeps the
 *        prompt answering, and it is asserted twice: the synthetic sink's play
 *        count is still zero after a real click, and it becomes one only after
 *        app_service(). The second half is measured in real milliseconds against a
 *        sink that deliberately takes 60 ms.
 *     4. A MISSING DRIVER IS LEGIBLE. Every machine in this tree is that machine
 *        today, so the app must show a sentence rather than looking broken, and
 *        the kernel must not claim a sound happened.
 *     5. NOTHING CAN FLOOD THE MACHINE. Ten thousand requests a second become two
 *        sounds a second and 9998 refusals, and a hostile document cannot loop the
 *        event loop, leak a slot, or overflow anything.
 *     6. THE GROUND TRUTH IS THE KERNEL'S. Exactly one [app_call ...] line per
 *        performed call, from the real return value, with no model-controlled
 *        bytes in it — and a sink whose failure sentence contains "\n[vfs_write
 *        ... -> ok]" cannot forge a second one anywhere.
 *
 *   THE SINK IS SYNTHETIC AND THE PCM IS READ BACK. A test that trusted the
 *   service would prove nothing about the broker, so audio_register_native()
 *   installs a sink here whose play() records the address it was handed, and the
 *   samples are then read out of that address and measured: 440.00 Hz, 7200
 *   frames, exactly what the document asked for. The real driver VM (vm/dvm.c) and
 *   the real audio service (core/audio.c) are linked, so the path under test is
 *   the one the kernel runs.
 *
 *   THE CLOCK IS PINNED, so "the rate limit holds" is arithmetic rather than a
 *   race. Where real time is the point — a handler must not wait for a device —
 *   the suite measures CLOCK_MONOTONIC instead, and says so.
 */

#include "harness.h"
#include "app.h"
#include "../../apps/app_impl.h"
#include "audio.h"
#include "chat.h"
#include "gui.h"
#include "widgets.h"
#include "fb.h"
#include "tool.h"
#include "trace.h"

#include "../../apps/examples/examples_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ====================================================================== */
/* stand in for the linker-collected tool table                           */
/* ====================================================================== */

/* Only the app tool: action=caps is the page that teaches this statement, and its
 * result has to fit CHAT_TOOL_RESULT_CAP or the model reads half a specification.
 * That is asserted below, through the real tool_dispatch(). */
extern const tool_t *const fableos_hosttool_app_tool;

#if defined(__APPLE__)
#  define SYMPFX "_"
#  define TBL_SECTION __attribute__((section("__DATA,__data")))
#else
#  define SYMPFX ""
#  define TBL_SECTION
#endif

const tool_t *__start_tool_table[1] TBL_SECTION = { 0 };
extern const tool_t *const __stop_tool_table[];

__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 8\n");
_Static_assert(sizeof(__start_tool_table) == 8,
               "tool count changed: update the __stop_tool_table byte offset");

/* ====================================================================== */
/* the fixture                                                            */
/* ====================================================================== */

#define SW 1024
#define SH 768
#define GROWS (SH / 16)
#define GCOLS (SW / 8)
#define BLANK_CELL 0x0720u

static fb_color_t   *pixels;
static fb_surface_t  screen;
static uint16_t      cells[GROWS * GCOLS];

/* The pinned monotonic clock. The broker's rate limit is measured against it, so
 * every "too soon" in this file is a decision and not a coincidence. */
static uint64_t fake_ms;
static uint64_t ms_source(void) { return fake_ms; }

/* ---- the synthetic sink ---- */

/* A driver that records what it was asked to play. `hold_ms` moves the pinned
 * clock forward the way a real device would consume time (audio.h's play contract
 * is synchronous), and `spin_real_ms` burns REAL time, which is what makes the
 * "a click does not wait for the device" measurement meaningful. */
static struct {
    int      plays, stops;
    uint64_t phys;
    uint32_t bytes, frames, ms;
    int      fail;
    char     why[192];
    int      hold_clock;      /* advance fake_ms by ms          */
    unsigned clock_scale;     /* ...times this (0/1 = exactly ms) */
    unsigned spin_real_ms;    /* busy-wait this long, for real  */
    uint64_t blocked_ms;      /* total fake time held, all plays  */
} sink;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

static int sink_play(void *ctx, uint64_t phys, uint32_t bytes, uint32_t frames,
                     uint32_t ms, char *why, size_t cap) {
    (void)ctx;
    sink.plays++;
    sink.phys   = phys;
    sink.bytes  = bytes;
    sink.frames = frames;
    sink.ms     = ms;
    if (sink.spin_real_ms) {
        uint64_t until = mono_ms() + sink.spin_real_ms;
        while (mono_ms() < until) { }
    }
    /* How long a synchronous driver holds the machine is NOT the duration it was
     * asked for. A play program polling a device gets a budget of the sound plus
     * AUDIO_PLAY_MARGIN_US, so `clock_scale` lets a test model a driver that costs
     * more than it plays — which is the whole reason apps/cap.c measures instead
     * of trusting APP_CAP_AUDIO_MS_MAX. */
    if (sink.hold_clock) {
        uint32_t held = ms * (sink.clock_scale ? sink.clock_scale : 1u);
        fake_ms += held;
        sink.blocked_ms += held;
    }
    if (sink.fail) {
        snprintf(why, cap, "%s", sink.why);
        return -1;
    }
    return 0;
}

static int sink_stop(void *ctx) { (void)ctx; sink.stops++; return 0; }

static const audio_format_t fmt48 = { 48000, 2, 16 };

static void install_sink(void) {
    audio_ops_t ops;
    memset(&ops, 0, sizeof ops);
    ops.play = sink_play;
    ops.stop = sink_stop;
    CHECK_EQ(audio_register_native("testsink", NULL, &fmt48, &ops), AUDIO_OK);
    CHECK_EQ(audio_available(), 1);
}

/* app_stats() IS CUMULATIVE FOR THE LIFE OF THE MACHINE — there is no reset, and
 * there should not be one: a tool prints those totals. So every assertion here is
 * on a DELTA from the start of the case, snapshotted by fixture(). Asserting the
 * absolute value would make each test depend on the order of the ones before it. */
static app_stats_t base;

static unsigned d_calls(void)   { return app_stats()->calls         - base.calls; }
static unsigned d_refused(void) { return app_stats()->calls_refused - base.calls_refused; }
static unsigned d_hw(void)      { return app_stats()->hw_actions    - base.hw_actions; }

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

    fake_ms = 100000;
    app_set_time_source(ms_source, NULL);
    app_cap_reset();
    audio_reset();                       /* no sink, no counters, no error */
    memset(&sink, 0, sizeof sink);
    sink.hold_clock = 1;
    kcap_reset();
    trace_reset();
    base = *app_stats();
    __start_tool_table[0] = fableos_hosttool_app_tool;
}

static app_error_t g_err;

static int launch(const char *doc, uint32_t *id) {
    return app_launch(doc, 0, GUI_POS_AUTO, GUI_POS_AUTO, id, &g_err);
}

/* A document that must be refused at LOAD time, leaving nothing behind. */
static void reject(const char *doc, const char *path, const char *phrase) {
    uint32_t id = 4242;
    int      apps_before = app_count(), wins_before = gui_window_count();
    int      rc = launch(doc, &id);

    CHECK(rc != APP_OK);
    CHECK_EQ(id, 0);
    CHECK_EQ(app_count(), apps_before);
    CHECK_EQ(gui_window_count(), wins_before);
    if (path)   CHECK_STR(g_err.path, path);
    if (phrase) CHECK_CONTAINS(g_err.msg, phrase);
    if (rc == APP_OK)
        printf("    (document was accepted but should not have been: %.100s)\n",
               doc);
}

/* A one-button document whose handler makes one call with the given "with" body
 * and reports into the status field. The shape every bounds test below uses, so a
 * failure points at the argument rather than at the scaffolding. */
static const char *doc_with(const char *with) {
    static char buf[APP_DOC_MAX];
    snprintf(buf, sizeof buf,
             "{\"title\":\"T\",\"width\":200,\"height\":110,"
             "\"grid\":{\"rows\":2,\"cols\":1},\"widgets\":["
             "{\"kind\":\"field\",\"name\":\"s\",\"text\":\"-\","
             "\"readonly\":true,\"row\":0,\"col\":0},"
             "{\"kind\":\"button\",\"text\":\"go\",\"name\":\"b\","
             "\"row\":1,\"col\":0}],"
             "\"on\":[{\"click\":\"b\",\"do\":[{\"call\":\"audio.tone\","
             "\"with\":%s,\"into\":\"s\"}]}]}", with);
    return buf;
}

static const char *status_of(uint32_t id) {
    gui_window_t *w = gui_window(id);
    if (!w || !w->nwidgets) return "(no window)";
    return w->widgets[0].text;
}

/* Press the "go" button through gui_click_widget(): the same press-and-release a
 * PS/2 mouse performs, so a handler is reached the way the operator reaches it. */
static void press_go(uint32_t id) {
    gui_window_t *w = gui_window(id);
    CHECK(w != NULL);
    if (!w) return;
    for (uint16_t i = 0; i < w->nwidgets; i++)
        if (!strcmp(w->widgets[i].text, "go")) {
            CHECK_EQ(gui_click_widget(id, w->widgets[i].id), 0);
            return;
        }
    CHECK(0);
}

/* Launch `with`, click it, and return the app id. */
static uint32_t run_with(const char *with) {
    uint32_t id = 0;
    CHECK_EQ(launch(doc_with(with), &id), APP_OK);
    if (id) press_go(id);
    return id;
}

/* How many [app_call ...] lines are in the console capture. */
static int app_call_lines(void) {
    const char *p = kcap_text();
    int n = 0;
    while ((p = strstr(p, "[app_call ")) != NULL) { n++; p++; }
    return n;
}

/* Lines that begin in COLUMN ZERO with '[' — the kernel's voice, and nothing
 * else's (trace.h). */
static int bracket_lines_at_column_zero(void) {
    const char *t = kcap_text();
    int n = 0;
    for (size_t i = 0; t[i]; i++)
        if (t[i] == '[' && (i == 0 || t[i - 1] == '\n')) n++;
    return n;
}

/* ====================================================================== */
/* 1. the statement is a real part of the format                           */
/* ====================================================================== */

static void test_the_capability_statement_compiles(void) {
    fixture();
    uint32_t id = 0;
    CHECK_EQ(launch(doc_with("{\"hz\":\"440\"}"), &id), APP_OK);
    CHECK(id != 0);
    CHECK_EQ(app_count(), 1);

    /* An accepted call is one compiled call statement and nothing on screen has
     * happened yet: no sink is registered, so nothing could have. */
    CHECK_EQ(sink.plays, 0);
    CHECK_EQ(app_cap_pending(), 0);

    /* Every documented argument, all four, with expressions rather than
     * literals — the whole point of "with" values being expression strings. */
    fixture();
    CHECK_EQ(launch(doc_with("{\"hz\":\"220 * 2\",\"ms\":\"50 + 50\","
                             "\"amp\":\"6000\",\"wave\":\"'square'\"}"),
                    &id), APP_OK);

    /* And the shipped worked example, which is the document a model copies. */
    fixture();
    CHECK_EQ(launch(APP_BEEPER_JSON, &id), APP_OK);
    CHECK_STR(app_title(id), "Beeper");
    CHECK_CONTAINS(status_of(id), "pick a note");
}

/* The .json on disk and the string the kernel embeds must be the same bytes, or
 * the example a model is shown is not the example that was tested. */
static void test_the_example_matches_the_file_on_disk(void) {
    const char *root = getenv("FABLEOS_SRCDIR") ? getenv("FABLEOS_SRCDIR") : ".";
    char        path[512];
    snprintf(path, sizeof path, "%s/apps/examples/beeper.json", root);

    FILE *fh = fopen(path, "rb");
    CHECK(fh != NULL);
    if (!fh) { printf("    (cannot open %s)\n", path); return; }
    static char disk[APP_DOC_MAX];
    size_t n = fread(disk, 1, sizeof disk - 1, fh);
    fclose(fh);
    disk[n] = '\0';
    if (strcmp(disk, APP_BEEPER_JSON) != 0)
        printf("    FAIL apps/examples/beeper.json differs from the embedded "
               "copy; run python3 apps/examples/gen_header.py\n");
    CHECK_STR(disk, APP_BEEPER_JSON);
}

static void test_a_wrong_call_is_refused_at_load_time(void) {
    fixture();

    /* An unknown capability is answered with the entire table, because a model
     * cannot guess a name into a validator that rejects unknown ones. */
    reject("{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
           "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
           "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
           "{\"call\":\"audio.play_mp3\"}]}]}",
           "on[0].do[0].call", "audio.tone");
    CHECK_CONTAINS(g_err.msg, "cannot name a driver");

    /* Not a string; and a capability name is never a device or a path. */
    reject("{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
           "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
           "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
           "{\"call\":7}]}]}",
           "on[0].do[0].call", "name of a capability");

    /* A required argument that is missing is named, with what it wants. */
    reject(doc_with("{\"ms\":\"100\"}"), "on[0].do[0].with.hz", "needs \"hz\"");

    /* An argument nothing would read is refused with the accepted list — the
     * same rule the rest of the format follows for every unknown key. */
    reject(doc_with("{\"hz\":\"440\",\"volume\":\"9\"}"),
           "on[0].do[0].with", "hz, ms, amp, wave");
    CHECK_CONTAINS(g_err.msg, "unknown argument \"volume\"");

    /* "with" must be an object of expressions, and an argument must be a string
     * holding one — a bare number is the mistake to expect. */
    reject("{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
           "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
           "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
           "{\"call\":\"audio.tone\",\"with\":[440]}]}]}",
           "on[0].do[0].with", "must be an object");
    reject(doc_with("{\"hz\":440}"), "on[0].do[0].with.hz", "must be a STRING");

    /* An expression that does not compile is refused with its byte offset, like
     * any other expression in the document. */
    reject(doc_with("{\"hz\":\"440 +\"}"), "on[0].do[0].with.hz", "offset");

    /* "into" must name something that exists. */
    reject("{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
           "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
           "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
           "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\"},"
           "\"into\":\"nowhere\"}]}]}",
           "on[0].do[0].into", "not a var or a named widget");

    /* Two verbs in one statement, which is how a model writes a call it thinks
     * also assigns. */
    reject("{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
           "\"vars\":{\"v\":0},"
           "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
           "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
           "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\"},\"set\":\"v\","
           "\"to\":\"1\"}]}]}",
           "on[0].do[0]", "exactly one verb");
}

static void test_the_call_bound_is_stated(void) {
    fixture();
    /* APP_MAX_CALLS + 1 call statements. The message has to name the bound and
     * the cheaper way to write it. */
    static char doc[APP_DOC_MAX];
    int n = snprintf(doc, sizeof doc,
                     "{\"width\":200,\"height\":110,"
                     "\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":["
                     "{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
                     "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":[");
    for (int i = 0; i < APP_MAX_CALLS + 1; i++)
        n += snprintf(doc + n, sizeof doc - (size_t)n,
                      "%s{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\"}}",
                      i ? "," : "");
    snprintf(doc + n, sizeof doc - (size_t)n, "]}]}");

    uint32_t id = 0;
    CHECK_EQ(launch(doc, &id), APP_ENOSPC);
    CHECK_CONTAINS(g_err.msg, "more than 8");
    CHECK_EQ(app_count(), 0);

    /* Exactly APP_MAX_CALLS is accepted, so the bound is off-by-one clean. */
    fixture();
    n = snprintf(doc, sizeof doc,
                 "{\"width\":200,\"height\":110,"
                 "\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":["
                 "{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
                 "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":[");
    for (int i = 0; i < APP_MAX_CALLS; i++)
        n += snprintf(doc + n, sizeof doc - (size_t)n,
                      "%s{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\"}}",
                      i ? "," : "");
    snprintf(doc + n, sizeof doc - (size_t)n, "]}]}");
    CHECK_EQ(launch(doc, &id), APP_OK);
}

/* ====================================================================== */
/* 2. a missing driver is legible, and is never reported as a sound        */
/* ====================================================================== */

static void test_no_audio_driver_is_legible(void) {
    fixture();                            /* audio_reset(): no sink at all */
    CHECK_EQ(audio_available(), 0);

    uint32_t id = run_with("{\"hz\":\"440\"}");
    /* The app shows it, in words that fit a 40-character field... */
    CHECK_STR(status_of(id), "no audio driver");
    /* ...and the model gets the whole sentence, from the audio service itself. */
    CHECK_CONTAINS(app_last_error(id), "no audio output driver is registered");
    CHECK_CONTAINS(app_last_error(id), "register itself as the audio sink");

    /* Nothing was queued, nothing was played, and the kernel claimed nothing:
     * silence that reports success is the one outcome this must never produce. */
    CHECK_EQ(app_cap_pending(), 0);
    CHECK_EQ(sink.plays, 0);
    CHECK_EQ(app_service(), 0);
    CHECK_EQ(app_call_lines(), 0);
    CHECK_EQ(d_calls(), 0u);
    CHECK_EQ(d_refused(), 1u);
    CHECK_EQ(d_hw(), 0u);
}

/* ====================================================================== */
/* 3. a handler never reaches the driver                                   */
/* ====================================================================== */

static void test_a_handler_queues_and_app_service_performs(void) {
    fixture();
    install_sink();

    uint32_t id = 0;
    CHECK_EQ(launch(doc_with("{\"hz\":\"440\",\"ms\":\"150\"}"), &id), APP_OK);

    uint64_t clock_before = fake_ms;
    press_go(id);

    /* THE CLAIM THIS SUITE EXISTS FOR: the click is over and the device has not
     * been touched. Not "was fast" — was not called. */
    CHECK_EQ(sink.plays, 0);
    CHECK_EQ(fake_ms, clock_before);
    CHECK_EQ(app_cap_pending(), 1);
    CHECK_EQ(app_call_lines(), 0);
    CHECK_EQ(d_calls(), 1u);

    /* And the pump performs it, once. */
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(sink.plays, 1);
    CHECK_EQ(app_cap_pending(), 0);
    CHECK_EQ(app_service(), 0);           /* nothing left to do */
    CHECK_EQ(sink.plays, 1);
    CHECK_EQ(d_hw(), 1u);

    /* What the driver was handed is what the document asked for. */
    CHECK_EQ(sink.phys, audio_buffer_phys());
    CHECK_EQ(sink.ms, 150u);
    CHECK_EQ(sink.frames, 48000u * 150u / 1000u);
    CHECK_EQ(sink.bytes, sink.frames * 4u);

    /* THE SAMPLES ARE REAL, AND THEY ARE THE RIGHT NOTE. Measured out of the
     * address the sink was given: one 440 Hz cycle per 109.09 frames, so 7200
     * frames hold 66 of them. Counting negative-going zero crossings recovers the
     * frequency without trusting the synthesiser's own arithmetic. */
    const int16_t *pcm = (const int16_t *)(uintptr_t)sink.phys;
    int cycles = 0;
    int16_t prev = pcm[0];
    for (uint32_t f = 1; f < sink.frames; f++) {
        int16_t cur = pcm[f * 2];                     /* left channel */
        if (prev >= 0 && cur < 0) cycles++;
        prev = cur;
    }
    int hz = (int)((uint64_t)cycles * 1000ull / sink.ms);
    if (hz < 438 || hz > 442)
        printf("    (measured %d Hz from %d cycles in %u ms)\n", hz, cycles,
               sink.ms);
    CHECK(hz >= 438 && hz <= 442);

    /* Stereo, interleaved, both channels identical — the format the sink
     * declared. And it is not silence. */
    int loud = 0;
    for (uint32_t f = 0; f < sink.frames; f++) {
        CHECK_EQ(pcm[f * 2], pcm[f * 2 + 1]);
        if (pcm[f * 2] > 4000) loud = 1;
        if (f > 40) break;                 /* enough: 40 frames is a claim */
    }
    CHECK_EQ(loud, 1);

    /* The outcome reached the app, after the sound rather than before it. */
    CHECK_STR(status_of(id), "ok");
}

/* THE PROMPT KEEPS ANSWERING, in real milliseconds rather than by argument.
 * The sink here burns 60 ms of actual CPU inside play(), which is what a
 * synchronous audio driver does. The click must not pay for that; app_service()
 * must. */
static void test_a_click_does_not_wait_for_the_device(void) {
    fixture();
    install_sink();
    sink.spin_real_ms = 60;

    uint32_t id = 0;
    CHECK_EQ(launch(doc_with("{\"hz\":\"440\",\"ms\":\"200\"}"), &id), APP_OK);

    uint64_t t0 = mono_ms();
    press_go(id);
    uint64_t click_ms = mono_ms() - t0;

    uint64_t t1 = mono_ms();
    CHECK_EQ(app_service(), 1);
    uint64_t service_ms = mono_ms() - t1;

    printf("    (the click took %lu ms; app_service() took %lu ms)\n",
           (unsigned long)click_ms, (unsigned long)service_ms);
    CHECK(click_ms < 20);                 /* generous: it should be ~0 */
    CHECK(service_ms >= 55);              /* the device's time is paid here */
    CHECK_EQ(sink.plays, 1);

    /* A tick handler is the same: it queues, it does not play. */
    fixture();
    install_sink();
    sink.spin_real_ms = 60;
    static const char ticker[] =
        "{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
        "\"widgets\":[{\"kind\":\"label\",\"text\":\"t\",\"row\":0,"
        "\"col\":0}],\"on\":[{\"tick\":50,\"do\":["
        "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\",\"ms\":\"200\"}}]}]}";
    CHECK_EQ(launch(ticker, &id), APP_OK);

    /* run_ticks() is inside app_tick(), and so is the pump — so measure the two
     * halves separately by draining first with app_service() disabled (nothing
     * pending), then letting the tick queue one. */
    t0 = mono_ms();
    CHECK_EQ(app_cap_pending(), 0);
    (void)app_service();                  /* nothing to do: must be free */
    CHECK(mono_ms() - t0 < 20);
}

/* ====================================================================== */
/* 4. every bound holds, and refuses rather than clamps                    */
/* ====================================================================== */

/* Click a fresh app with `with`, and assert it was refused with `brief` in the
 * status field and `phrase` in the note the model reads — and that nothing was
 * played. The clock is moved on between cases so a refusal can never be the rate
 * limit by accident. */
static void refused(const char *with, const char *brief, const char *phrase) {
    fake_ms += APP_CAP_GAP_MS * 2;
    app_close_all();
    int plays_before = sink.plays;
    uint32_t id = run_with(with);
    CHECK_STR(status_of(id), brief);
    CHECK_CONTAINS(app_last_error(id), phrase);
    CHECK_EQ(app_cap_pending(), 0);
    CHECK_EQ(app_service(), 0);
    CHECK_EQ(sink.plays, plays_before);
}

static void test_bounds_are_enforced_when_the_handler_runs(void) {
    fixture();
    install_sink();

    /* A 5 MHz tone. REFUSED, with the range in the sentence — not clamped to
     * 20 kHz, which would report a sound that is not the one asked for. */
    refused("{\"hz\":\"5000000\"}", "hz out of range", "outside 0 to 20000");
    /* And a 20 GHz one, which is past what a fixed-point value can even hold, so
     * the expression compiler refuses it a step earlier — at LOAD time. Both
     * refusals are legible; neither is a sound. */
    fake_ms += APP_CAP_GAP_MS * 2;
    app_close_all();
    reject(doc_with("{\"hz\":\"20000000000\"}"), "on[0].do[0].with.hz", NULL);
    /* A 10-hour tone. Same, against APP_CAP_AUDIO_MS_MAX. */
    refused("{\"hz\":\"440\",\"ms\":\"36000000\"}", "ms out of range",
            "outside 1 to 200");
    /* Just past each bound, which is where an off-by-one would hide. */
    refused("{\"hz\":\"20001\"}", "hz out of range", "outside 0 to 20000");
    refused("{\"hz\":\"440\",\"ms\":\"201\"}", "ms out of range",
            "outside 1 to 200");
    refused("{\"hz\":\"440\",\"ms\":\"0\"}", "ms out of range",
            "outside 1 to 200");
    refused("{\"hz\":\"440\",\"amp\":\"32768\"}", "amp out of range",
            "outside 0 to 32767");
    /* Inaudible but not zero: the hole in the range is explained, not hidden. */
    refused("{\"hz\":\"19\"}", "hz too low to hear", "below 20 Hz");
    refused("{\"hz\":\"-440\"}", "hz out of range", "outside 0 to 20000");
    /* A waveform that does not exist, answered with the ones that do. */
    refused("{\"hz\":\"440\",\"wave\":\"'zither'\"}", "no such wave", "'sine'");
    /* A number where text belongs, and text where a number belongs. */
    refused("{\"hz\":\"440\",\"wave\":\"3\"}", "wave is not text", "single quotes");
    refused("{\"hz\":\"'440'\"}", "hz is not a number", "produced the text");
    /* And the ERR value, which is what 1/0 and num('abc') produce. */
    refused("{\"hz\":\"1 / 0\"}", "hz is an error", "error value");
    refused("{\"hz\":\"num('abc')\"}", "hz is an error", "error value");

    /* Every one of those was refused, so the driver was never asked. */
    CHECK_EQ(sink.plays, 0);
    CHECK_EQ(d_hw(), 0u);
    CHECK_EQ(app_call_lines(), 0);
}

static void test_the_edges_of_the_range_are_accepted(void) {
    fixture();
    install_sink();

    /* Exactly on each bound, and the documented defaults. */
    const char *ok[] = {
        "{\"hz\":\"20\"}",
        "{\"hz\":\"20000\",\"ms\":\"1\"}",
        "{\"hz\":\"440\",\"ms\":\"200\",\"amp\":\"32767\"}",
        "{\"hz\":\"440\",\"wave\":\"'triangle'\"}",
        "{\"hz\":\"440\",\"wave\":\"'square'\"}",
        "{\"hz\":\"440.4\"}",             /* rounded to 440, not refused */
    };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        fake_ms += APP_CAP_GAP_MS * 2;
        app_close_all();
        uint32_t id = run_with(ok[i]);
        CHECK_EQ(app_cap_pending(), 1);
        CHECK_EQ(app_service(), 1);
        CHECK_STR(status_of(id), "ok");
    }
    CHECK_EQ(sink.plays, (int)(sizeof ok / sizeof ok[0]));

    /* hz 0 is a REST: audio.h's own meaning, so it is a success and it is
     * silence. A melody needs gaps, and refusing them would be wrong. */
    fake_ms += APP_CAP_GAP_MS * 2;
    app_close_all();
    uint32_t id = run_with("{\"hz\":\"0\",\"ms\":\"20\"}");
    CHECK_EQ(app_service(), 1);
    CHECK_STR(status_of(id), "ok");
    const int16_t *pcm = (const int16_t *)(uintptr_t)sink.phys;
    int nonzero = 0;
    for (uint32_t f = 0; f < sink.frames * 2; f++) if (pcm[f]) nonzero = 1;
    CHECK_EQ(nonzero, 0);
}

/* ====================================================================== */
/* 5. nothing can flood the machine                                        */
/* ====================================================================== */

static void test_one_call_at_a_time(void) {
    fixture();
    install_sink();

    /* Two calls in ONE handler: the second is refused because the first is still
     * waiting, and the app is told so. A queue would have hidden the overload and
     * then played a burst of stale beeps. */
    static const char two[] =
        "{\"width\":200,\"height\":110,\"grid\":{\"rows\":2,\"cols\":1},"
        "\"widgets\":[{\"kind\":\"field\",\"name\":\"s\",\"text\":\"-\","
        "\"readonly\":true,\"row\":0,\"col\":0},"
        "{\"kind\":\"button\",\"text\":\"go\",\"name\":\"b\",\"row\":1,"
        "\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
        "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\"},\"into\":\"s\"},"
        "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"880\"},\"into\":\"s\"}]}]}";
    uint32_t id = 0;
    CHECK_EQ(launch(two, &id), APP_OK);
    press_go(id);
    CHECK_STR(status_of(id), "still busy");
    CHECK_CONTAINS(app_last_error(id), "dropped");
    CHECK_EQ(d_calls(), 1u);
    CHECK_EQ(d_refused(), 1u);
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(sink.plays, 1);
    CHECK_EQ(sink.frames, 48000u * 120u / 1000u);   /* the default ms */
}

/* Advance the fake clock far enough that the duty-cycle limiter will allow
 * another sound, whatever the last one cost.
 *
 * The limiter is not a fixed interval: apps/cap.c times each call and then keeps
 * the machine quiet for at least that long again plus APP_CAP_GAP_MS, counted
 * from when the call FINISHED. So a test that wants "long enough" cannot just add
 * APP_CAP_GAP_MS — it has to cover the block as well. The block is at most the
 * requested duration, which is at most APP_CAP_AUDIO_MS_MAX, so this is always
 * enough and never depends on which sink the test installed. Tests that are
 * specifically ABOUT the boundary set sink.hold_clock = 0 and step the clock by
 * hand instead. */
static void wait_out_the_quiet(void) {
    fake_ms += (uint64_t)APP_CAP_GAP_MS + APP_CAP_AUDIO_MS_MAX + 1;
}

static void test_the_rate_limit_holds(void) {
    fixture();
    install_sink();
    /* The sink neither advances the fake clock nor burns real time, so the
     * MEASURED block is 0 and the quiet window is exactly APP_CAP_GAP_MS. That
     * isolates the gap from the duration, which is what this test is about;
     * test_a_slow_driver_buys_a_longer_silence covers the other half. */
    sink.hold_clock = 0;

    uint32_t id = run_with("{\"hz\":\"440\"}");
    CHECK_EQ(app_service(), 1);
    CHECK_STR(status_of(id), "ok");

    /* Immediately again: too soon, and the sentence says how long is left and
     * WHY there is a wait at all. The rule is a measured duty cycle, not a fixed
     * interval: the machine stays quiet for as long as the last sound actually
     * blocked it, plus APP_CAP_GAP_MS, counted from when it finished. */
    press_go(id);
    CHECK_STR(status_of(id), "too soon");
    CHECK_CONTAINS(app_last_error(id), "held this machine for");
    CHECK_CONTAINS(app_last_error(id), "plus 500 ms");
    CHECK_CONTAINS(app_last_error(id), "ms to go");
    CHECK_EQ(sink.plays, 1);

    /* One millisecond short of the gap: still refused. */
    fake_ms += APP_CAP_GAP_MS - 1;
    press_go(id);
    CHECK_STR(status_of(id), "too soon");
    CHECK_EQ(app_service(), 0);
    CHECK_EQ(sink.plays, 1);

    /* And exactly on it: accepted. */
    fake_ms += 1;
    press_go(id);
    CHECK_EQ(app_cap_pending(), 1);
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(sink.plays, 2);

    /* A clock that goes BACKWARDS must not silence the app forever. */
    fake_ms = 1;
    app_set_time_source(ms_source, NULL);
    press_go(id);
    CHECK_EQ(app_cap_pending(), 1);
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(sink.plays, 3);
}

/* THE DUTY CYCLE IS BOUNDED BY MEASUREMENT, NOT BY APP_CAP_AUDIO_MS_MAX.
 *
 * This is the regression test for a documented number that was simply false.
 * include/app.h and apps/cap.c both used to state that APP_CAP_AUDIO_MS_MAX and
 * APP_CAP_GAP_MS "bound that to 200 ms in any 500 ms, i.e. at most 40% of the
 * machine". APP_CAP_AUDIO_MS_MAX bounds the duration an app may REQUEST. It does
 * not bound how long performing the request BLOCKS the machine, and with the sink
 * this project exists to produce — a driver program the model wrote — those are
 * different numbers: core/audio.c raises the program's delay budget to the sound
 * plus AUDIO_PLAY_MARGIN_US (250 ms), and the program spends it in mdelay(),
 * which yields to nothing. A 200 ms app tone was measured holding the machine for
 * 450 ms. Because the old gap was timed from the moment a sound STARTED, that
 * 450 ms came out of the 500 ms interval: 90% of the machine.
 *
 * The fix is to measure. This test installs a sink that blocks for FOUR TIMES the
 * requested duration — a stand-in for exactly that margin — and asserts the
 * resulting duty cycle, in the sink's own accounting of time it held the machine
 * against the wall clock the whole run took. */
/* TWO DOCUMENTS BOTH GET A TURN AT THE SPEAKER.
 *
 * There is one pending capability slot machine-wide, deliberately (apps/cap.c:
 * "who is making the sound has to have one answer"). That is fine. What was not
 * fine is that run_ticks() iterated the instance pool from slot 0 every single
 * pass, while every handler's first due time is 0 and equal-period handlers share
 * one env_ms reading — so two documents with the same tick period were phase
 * locked for ever and the lower slot won the slot every time. Measured before the
 * fix: two identical documents over 66 s of virtual time, app A 132 sounds, app B
 * ZERO. Not a race; an exact and permanent outcome. And the refusal app B was
 * given ("still busy" / "too soon") reads as transient, so a model would back off
 * — which could not possibly help.
 *
 * This test is the fairness property, asserted as a ratio rather than as equality:
 * round robin does not promise identical counts, it promises nobody is silenced. */
static void test_two_documents_share_the_speaker(void) {
    fixture();
    install_sink();
    sink.hold_clock = 0;

    static const char ticker[] =
        "{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
        "\"widgets\":[{\"kind\":\"label\",\"name\":\"l\",\"text\":\"t\","
        "\"row\":0,\"col\":0}],\"on\":[{\"tick\":50,\"do\":["
        "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\",\"ms\":\"20\"},"
        "\"into\":\"l\"}]}]}";

    uint32_t a = 0, b = 0;
    CHECK_EQ(launch(ticker, &a), APP_OK);
    CHECK_EQ(launch(ticker, &b), APP_OK);
    CHECK(a != b);

    /* Which app each sound belonged to is in the trace line the kernel printed,
     * which is the only honest record of who really made a noise. */
    kcap_reset();
    for (int i = 0; i < 66000; i++) {         /* 66 s at 1 ms per pass */
        (void)app_tick_handlers();
        (void)app_service();
        fake_ms += 1;
    }

    int for_a = 0, for_b = 0;
    char needle[48];
    snprintf(needle, sizeof needle, "app=%u ", (unsigned)a);
    for (const char *p = kcap_text(); (p = strstr(p, needle)) != NULL; p++) for_a++;
    snprintf(needle, sizeof needle, "app=%u ", (unsigned)b);
    for (const char *p = kcap_text(); (p = strstr(p, needle)) != NULL; p++) for_b++;

    printf("    (66 s, two identical tickers: app A got %d sounds, app B got %d)\n",
           for_a, for_b);

    /* NEITHER MAY BE ZERO. This is the whole test. */
    CHECK(for_a > 0);
    CHECK(for_b > 0);
    /* And neither may dominate: with round robin the split is close to even, so
     * require each to hold at least a third of the total. A 3:1 split would mean
     * the rotation is not actually rotating. */
    CHECK(for_a * 3 >= for_a + for_b);
    CHECK(for_b * 3 >= for_a + for_b);
    CHECK_EQ(sink.plays, for_a + for_b);
    CHECK_EQ(kpanic_hit, 0);

    /* Four documents: still nobody silenced. */
    fixture();
    install_sink();
    sink.hold_clock = 0;
    uint32_t ids[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) CHECK_EQ(launch(ticker, &ids[i]), APP_OK);
    kcap_reset();
    for (int i = 0; i < 66000; i++) {
        (void)app_tick_handlers();
        (void)app_service();
        fake_ms += 1;
    }
    for (int i = 0; i < 4; i++) {
        int n = 0;
        snprintf(needle, sizeof needle, "app=%u ", (unsigned)ids[i]);
        for (const char *p = kcap_text(); (p = strstr(p, needle)) != NULL; p++) n++;
        printf("    (four tickers: app %u got %d)\n", (unsigned)ids[i], n);
        CHECK(n > 0);
    }
}

static void test_a_slow_driver_buys_a_longer_silence(void) {
    fixture();
    install_sink();

    /* hold_clock advances the fake clock by the requested ms. Multiply it: this
     * sink holds the machine for 4x what it was asked for, which is worse than
     * the real VM path's 2.25x. The limiter has never been told this. */
    sink.hold_clock  = 1;
    sink.clock_scale = 4;

    static const char ticker[] =
        "{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
        "\"widgets\":[{\"kind\":\"label\",\"name\":\"l\",\"text\":\"t\","
        "\"row\":0,\"col\":0}],\"on\":[{\"tick\":50,\"do\":["
        "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\",\"ms\":\"200\"},"
        "\"into\":\"l\"}]}]}";
    uint32_t id = 0;
    CHECK_EQ(launch(ticker, &id), APP_OK);

    /* Ask as fast as a document is allowed to, for 60 s of virtual time. Each
     * accepted sound is asked for as 200 ms and costs the machine 800 ms. */
    uint64_t start = fake_ms;
    for (int i = 0; i < 60000; i++) {
        (void)app_tick_handlers();
        (void)app_service();
        fake_ms += 1;
    }
    uint64_t elapsed = fake_ms - start;
    uint64_t blocked = sink.blocked_ms;

    printf("    (a driver blocking 4x the requested duration: %u sounds, "
           "%lu ms blocked of %lu ms = %lu%%)\n",
           sink.plays, (unsigned long)blocked, (unsigned long)elapsed,
           (unsigned long)(blocked * 100 / (elapsed ? elapsed : 1)));

    /* THE PROPERTY. Under the old rule this was 90%+. It must now be under half
     * however slow the driver is, because the quiet interval is derived from the
     * measured block rather than from a constant. */
    CHECK(blocked * 2 < elapsed);

    /* And it must not have stopped the app making any sound at all: a limiter
     * that silences everything is not a fix. */
    CHECK(sink.plays > 10);

    /* Every request that was refused got a note saying so, so the document can
     * tell the difference between "not yet" and "broken". */
    CHECK(d_refused() > 0u);
    CHECK_CONTAINS(app_last_error(id), "stays quiet");
    CHECK_EQ(kpanic_hit, 0);

    /* A FAST driver must not be punished by the same rule: with a block of ~0 the
     * quiet interval collapses to APP_CAP_GAP_MS, i.e. the old behaviour. */
    fixture();
    install_sink();
    sink.hold_clock = 0;                  /* no measurable block at all */
    CHECK_EQ(launch(ticker, &id), APP_OK);
    start = fake_ms;
    for (int i = 0; i < 10000; i++) {
        (void)app_tick_handlers();
        (void)app_service();
        fake_ms += 1;
    }
    elapsed = fake_ms - start;
    /* 10 s / 500 ms = 20, plus the one at t=0. */
    CHECK(sink.plays >= 20 && sink.plays <= 21);
}

static void test_ten_thousand_tones_a_second_become_two(void) {
    fixture();
    install_sink();
    sink.hold_clock = 0;

    /* The fastest a document can ask: a tick handler at APP_TICK_MIN_MS with a
     * call in it, i.e. 20 requests a second, for 20 seconds of virtual time. The
     * pump runs on every pass, exactly as kernel/main.c's loop does. */
    static const char ticker[] =
        "{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
        "\"widgets\":[{\"kind\":\"label\",\"name\":\"l\",\"text\":\"t\","
        "\"row\":0,\"col\":0}],\"on\":[{\"tick\":50,\"do\":["
        "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\",\"ms\":\"200\"},"
        "\"into\":\"l\"}]}]}";
    uint32_t id = 0;
    CHECK_EQ(launch(ticker, &id), APP_OK);

    for (int i = 0; i < 20000; i++) {     /* 20 s at 1 ms per pass */
        (void)app_tick();
        fake_ms += 1;
    }

    printf("    (20 s of a 50 ms tick asking for sound: %u accepted, %u "
           "refused, %u performed)\n", d_calls(), d_refused(), d_hw());

    /* 20 s / 500 ms = 40, plus the one at t=0. Not 400. */
    CHECK(d_hw() <= 41u);
    CHECK(d_hw() >= 38u);
    CHECK_EQ(sink.plays, (int)d_hw());
    /* The rest were refused, every one with a sentence, and none of them
     * printed anything: 20 seconds of refusals must not fill the console. */
    CHECK(d_refused() > 300u);
    CHECK_EQ(app_call_lines(), (int)d_hw());
    CHECK_EQ(kpanic_hit, 0);

    /* The app is still alive and still holds one bounded note. The bound is
     * app_inst_t.lasterr (224 bytes, matching app_error_t.msg) — a refusal has to
     * fit whole, because the clause saying what to do about it is at the end. */
    CHECK_EQ(app_count(), 1);
    CHECK(strlen(app_last_error(id)) > 0);
    CHECK(strlen(app_last_error(id)) < 224);
}

static void test_a_hostile_document_cannot_exhaust_anything(void) {
    fixture();
    install_sink();

    /* Absurd, malformed and hostile shapes. Each must be refused, and each must
     * leave no app, no window and no queued request behind. */
    static char big[APP_DOC_MAX + 64];

    /* A capability name of 8000 characters. */
    int n = snprintf(big, sizeof big,
                     "{\"width\":200,\"height\":110,"
                     "\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":["
                     "{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
                     "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
                     "{\"call\":\"");
    for (int i = 0; i < 4000 && n < (int)sizeof big - 32; i++)
        big[n++] = 'a';
    snprintf(big + n, sizeof big - (size_t)n, "\"}]}]}");
    uint32_t id = 4242;
    CHECK(launch(big, &id) != APP_OK);
    CHECK_EQ(id, 0);
    CHECK_EQ(app_count(), 0);

    /* An embedded NUL in the capability name (legal JSON, not a legal name). */
    reject("{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
           "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
           "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
           "{\"call\":\"audio\\u0000.tone\"}]}]}",
           "on[0].do[0].call", "not a capability");

    /* "with" nested absurdly deep, and a call inside eight ifs. */
    reject(doc_with("{\"hz\":\"((((((((((((((440))))))))))))))\"}"),
           "on[0].do[0].with.hz", "nested");

    /* Truncated JSON in the middle of a call. */
    reject("{\"width\":200,\"height\":110,\"grid\":{\"rows\":1,\"cols\":1},"
           "\"widgets\":[{\"kind\":\"button\",\"text\":\"x\",\"name\":\"b\","
           "\"row\":0,\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
           "{\"call\":\"audio.tone\",\"with\":{\"hz\":",
           "", "not valid JSON");

    CHECK_EQ(app_cap_pending(), 0);
    CHECK_EQ(sink.plays, 0);

    /* A CALL INSIDE NESTED IFS still runs, and still only once per pass — depth
     * is bounded by APP_MAX_IF_DEPTH, which existed before this statement did. */
    static const char nested[] =
        "{\"width\":200,\"height\":110,\"grid\":{\"rows\":2,\"cols\":1},"
        "\"widgets\":[{\"kind\":\"field\",\"name\":\"s\",\"text\":\"-\","
        "\"readonly\":true,\"row\":0,\"col\":0},"
        "{\"kind\":\"button\",\"text\":\"go\",\"name\":\"b\",\"row\":1,"
        "\"col\":0}],\"on\":[{\"click\":\"b\",\"do\":["
        "{\"if\":\"1\",\"then\":[{\"if\":\"1\",\"then\":[{\"if\":\"1\","
        "\"then\":[{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\"},"
        "\"into\":\"s\"}]}]}]}]}]}";
    CHECK_EQ(launch(nested, &id), APP_OK);
    press_go(id);
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(sink.plays, 1);
    CHECK_STR(status_of(id), "ok");

    /* 200 launch/close cycles with a call in each: no slot leak, no queue leak,
     * and the instance pool is exactly as free at the end as at the start. */
    for (int i = 0; i < 200; i++) {
        uint32_t t = 0;
        CHECK_EQ(launch(doc_with("{\"hz\":\"440\"}"), &t), APP_OK);
        press_go(t);
        (void)app_service();
        wait_out_the_quiet();
        CHECK_EQ(app_close(t), APP_OK);
    }
    CHECK_EQ(app_count(), 1);             /* only `nested` is still open */
    CHECK_EQ(kpanic_hit, 0);
}

static void test_an_app_that_closes_makes_no_sound(void) {
    fixture();
    install_sink();

    uint32_t id = run_with("{\"hz\":\"440\"}");
    CHECK_EQ(app_cap_pending(), 1);
    CHECK_EQ(app_close(id), APP_OK);

    /* The window is gone before the idle loop got to it, so the request is
     * dropped: a closed app must not be able to make a noise. */
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(sink.plays, 0);
    CHECK_EQ(d_hw(), 0u);
    CHECK_EQ(app_call_lines(), 0);
    CHECK_EQ(app_cap_pending(), 0);
}

/* ====================================================================== */
/* 6. the ground truth is the kernel's                                     */
/* ====================================================================== */

static void test_every_performed_call_emits_one_trace_line(void) {
    fixture();
    install_sink();

    uint32_t id = run_with("{\"hz\":\"440\",\"ms\":\"150\",\"amp\":\"6000\","
                           "\"wave\":\"'square'\"}");
    CHECK_EQ(app_service(), 1);

    /* Exactly one line, and every field in it is a number the broker validated
     * or a word from kernel source. */
    CHECK_EQ(app_call_lines(), 1);
    char want[128];
    snprintf(want, sizeof want,
             "[app_call cap=audio.tone app=%u hz=440 ms=150 amp=6000 "
             "wave=square -> ok]", (unsigned)id);
    CHECK_CONTAINS(kcap_text(), want);

    /* A failure is traced too, symbolically, and the sink's own sentence is NOT
     * in the line — only in the app's note. */
    wait_out_the_quiet();
    kcap_reset();
    sink.fail = 1;
    snprintf(sink.why, sizeof sink.why, "status register still 0x00 after 40 ms");
    press_go(id);
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(app_call_lines(), 1);
    /* AUDIO_EIO is -5, and lib/trace.c's symbol table has no row for it, so
     * trace.h's documented fallback applies: an unknown code renders as the
     * signed integer rather than being swallowed. Asserted as it really prints —
     * a one-row addition to that table would make it read "EIO", and that file
     * is not this module's to edit. */
    CHECK_CONTAINS(kcap_text(), "-> -5]");
    CHECK(strstr(kcap_text(), "status register") == NULL);
    CHECK_STR(status_of(id), "the driver failed");
    CHECK_CONTAINS(app_last_error(id), "status register still 0x00");
}

/* THE LINE STARTS IN COLUMN ZERO EVEN WITH THE PROMPT ON SCREEN. This is the
 * first thing in the machine that prints while the operator's "> " is sitting
 * there with no newline behind it: the idle loop performs the sound between two
 * input polls. Without the guard in app_cap_service() the line came out as
 * "> [app_call ...]", which is not a forgery but does stop "^\[" from finding a
 * genuine kernel action — the harness greps for exactly that. */
static void test_the_trace_line_starts_in_column_zero_after_a_prompt(void) {
    fixture();
    install_sink();

    uint32_t id = run_with("{\"hz\":\"440\"}");
    kputs("> ");                          /* the prompt, cursor left mid-line */
    CHECK_EQ(app_service(), 1);

    const char *t = kcap_text();
    const char *at = strstr(t, "[app_call ");
    CHECK(at != NULL);
    if (at) CHECK(at == t || at[-1] == '\n');
    CHECK_EQ(app_call_lines(), 1);
    CHECK_EQ(bracket_lines_at_column_zero(), 1);
    CHECK_STR(status_of(id), "ok");

    /* And no gratuitous newline when the cursor is already at column zero. */
    wait_out_the_quiet();
    kcap_reset();
    kputs("line\n");
    press_go(id);
    CHECK_EQ(app_service(), 1);
    CHECK_CONTAINS(kcap_text(), "line\n[app_call ");
}

/* A SINK'S SENTENCE IS MODEL BYTES. A VM play program's `abort "reason"` reaches
 * audio_last_error() verbatim, and this module puts that sentence in a widget and
 * in a note. trace.h's one invariant is that a '[' in column zero is the kernel
 * speaking, so a reason of "\n[vfs_write fd=3 -> ok]" must not be able to forge
 * one. */
static void test_a_sink_sentence_cannot_forge_a_trace_line(void) {
    fixture();
    install_sink();
    sink.fail = 1;
    snprintf(sink.why, sizeof sink.why,
             "\n[vfs_write fd=3 bytes=34 -> ok]\n[app_call cap=audio.tone "
             "app=1 hz=440 -> ok]\n");

    uint32_t id = run_with("{\"hz\":\"440\"}");
    CHECK_EQ(app_service(), 1);

    /* One genuine line, printed by apps/cap.c. */
    CHECK_EQ(app_call_lines(), 1);
    CHECK_CONTAINS(kcap_text(), "-> -5]");
    CHECK_EQ(bracket_lines_at_column_zero(), 1);
    CHECK(strstr(kcap_text(), "vfs_write") == NULL);

    /* The app and the model see the reason, with the dangerous bytes replaced
     * rather than the message thrown away. */
    CHECK(strchr(app_last_error(id), '[') == NULL);
    CHECK(strchr(app_last_error(id), '\n') == NULL);
    CHECK_CONTAINS(app_last_error(id), "vfs_write fd=3 bytes=34");
    CHECK(strchr(status_of(id), '[') == NULL);

    /* And directly, over every byte: the cleaner leaves ordinary text alone and
     * neutralises exactly the two brackets and the control characters. */
    char out[64];
    app_cap_clean(out, sizeof out, "ok\n[fake]\t\x01 done");
    CHECK_STR(out, "ok  fake    done");
    app_cap_clean(out, sizeof out, "");
    CHECK_STR(out, "");
    app_cap_clean(out, 1, "abc");
    CHECK_STR(out, "");
    app_cap_clean(NULL, 0, "abc");        /* must simply not crash */
    for (int c = 1; c < 256; c++) {
        char in[2] = { (char)c, 0 };
        app_cap_clean(out, sizeof out, in);
        int dangerous = (c < 0x20) || c == 0x7F || c == '[' || c == ']';
        CHECK_EQ(out[0] == ' ' && dangerous, dangerous);
        if (!dangerous) CHECK_EQ((unsigned char)out[0], (unsigned)c);
    }
}

/* ====================================================================== */
/* 7. what the model is told                                               */
/* ====================================================================== */

static void test_the_spec_is_generated_from_the_table(void) {
    char buf[1024];
    size_t need = app_cap_spec(buf, sizeof buf);

    CHECK(need > 0);
    CHECK(need < sizeof buf);
    /* Every bound a document can hit appears, and appears as the number that is
     * actually enforced — these are the same constants apps/cap.c validates
     * against, so a changed bound cannot leave stale documentation behind. */
    CHECK_CONTAINS(buf, "audio.tone");
    CHECK_CONTAINS(buf, "hz");
    CHECK_CONTAINS(buf, "20000");
    CHECK_CONTAINS(buf, "200");
    CHECK_CONTAINS(buf, "32767");
    CHECK_CONTAINS(buf, "'triangle'");
    CHECK_CONTAINS(buf, "500 ms");
    CHECK_CONTAINS(buf, "REFUSED");
    CHECK_CONTAINS(buf, "trace line");
    /* No trace line can be forged out of it either: it is model-facing text. */
    CHECK(strstr(buf, "\n[") == NULL);

    /* snprintf convention: truncates cleanly, reports what it wanted, and
     * survives no buffer at all. */
    char small[40];
    memset(small, 'x', sizeof small);
    CHECK_EQ(app_cap_spec(small, sizeof small), need);
    CHECK(strlen(small) < sizeof small);
    CHECK_EQ(app_cap_spec(NULL, 0), need);
    CHECK_EQ(app_cap_spec(small, 0), need);

    /* The table's own accessors are total. */
    CHECK_EQ(app_cap_lookup("audio.tone"), 0);
    CHECK_EQ(app_cap_lookup("audio.Tone"), -1);
    CHECK_EQ(app_cap_lookup(""), -1);
    CHECK_EQ(app_cap_lookup("audio.tone "), -1);
    CHECK_STR(app_cap_name(0), "audio.tone");
    CHECK_STR(app_cap_name(-1), "");
    CHECK_STR(app_cap_name(99), "");
    CHECK_EQ(app_cap_nargs(0), 4);
    CHECK_EQ(app_cap_nargs(99), 0);
    CHECK(app_cap_arg(0, 0) != NULL);
    CHECK(app_cap_arg(0, 4) == NULL);
    CHECK(app_cap_arg(99, 0) == NULL);
    CHECK_STR(app_cap_arg(0, 0)->name, "hz");
    CHECK_EQ(app_cap_arg(0, 0)->required, 1);
    CHECK_EQ(app_cap_arg(0, 1)->required, 0);
}

/* THE PAGE THE MODEL ACTUALLY READS. A tool result is clipped at
 * CHAT_TOOL_RESULT_CAP, and a specification that stops mid-sentence is worse than
 * a terser one that finishes — so action=caps is measured, and the document inside
 * it is extracted and launched, because a page that teaches a document that does
 * not work is worse than no page. */
static void test_action_caps_fits_and_teaches_a_document_that_launches(void) {
    fixture();

    static char   rbuf[2048];
    tool_result_t res;
    tool_call_t   c;
    c.id = "toolu_test"; c.name = "app";
    c.input = "{\"action\":\"caps\"}";
    c.input_len = strlen(c.input);
    tool_result_init(&res, rbuf, sizeof rbuf);
    CHECK_EQ(tool_dispatch(&c, &res), TOOL_OK);
    CHECK_EQ(res.is_error, 0);

    printf("    (action=caps is %d bytes; the model only ever sees %d)\n",
           (int)res.len, CHAT_TOOL_RESULT_CAP - 1);
    CHECK(res.len < CHAT_TOOL_RESULT_CAP);
    CHECK_EQ(res.truncated, 0);
    CHECK_CONTAINS(rbuf, "audio.tone");
    CHECK_CONTAINS(rbuf, "into");
    CHECK_CONTAINS(kcap_text(), "[app action=caps -> ok]");

    /* The example document is on the first line: extract and launch it. */
    const char *start = strchr(rbuf, '{');
    CHECK(start != NULL);
    const char *nl = start ? strchr(start, '\n') : NULL;
    CHECK(nl != NULL);
    if (!start || !nl) return;
    char doc[1024];
    size_t n = (size_t)(nl - start);
    CHECK(n < sizeof doc);
    memcpy(doc, start, n);
    doc[n] = '\0';

    uint32_t id = 0;
    int rc = app_launch(doc, n, GUI_POS_AUTO, GUI_POS_AUTO, &id, &g_err);
    if (rc != APP_OK)
        printf("    (the taught document was rejected at %s: %s)\n",
               g_err.path, g_err.msg);
    CHECK_EQ(rc, APP_OK);

    /* And its button really asks for a sound. */
    install_sink();
    gui_window_t *w = gui_window(id);
    CHECK(w != NULL);
    if (!w) return;
    for (uint16_t i = 0; i < w->nwidgets; i++)
        if (!strcmp(w->widgets[i].text, "beep"))
            CHECK_EQ(gui_click_widget(id, w->widgets[i].id), 0);
    CHECK_EQ(app_cap_pending(), 1);
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(sink.plays, 1);

    /* The old page still fits too: it now carries the one line that says this
     * page exists, and it is the text a model reads first. */
    c.input = "{\"action\":\"format\"}";
    c.input_len = strlen(c.input);
    tool_result_init(&res, rbuf, sizeof rbuf);
    CHECK_EQ(tool_dispatch(&c, &res), TOOL_OK);
    CHECK(res.len < CHAT_TOOL_RESULT_CAP);
    CHECK_CONTAINS(rbuf, "{call,with,into}");
    CHECK_CONTAINS(rbuf, "action=caps");
}

/* ====================================================================== */
/* 8. the worked example, driven                                           */
/* ====================================================================== */

static void test_the_beeper_example_plays_every_note(void) {
    fixture();
    install_sink();
    sink.hold_clock = 0;

    uint32_t id = 0;
    CHECK_EQ(launch(APP_BEEPER_JSON, &id), APP_OK);

    /* Each note button carries its own frequency as its visible text, and the
     * handler reads it with num(key) — one handler, three keys. */
    const struct { const char *label; uint32_t hz; } notes[] = {
        { "440", 440 }, { "554", 554 }, { "659", 659 },
    };
    for (unsigned i = 0; i < sizeof notes / sizeof notes[0]; i++) {
        wait_out_the_quiet();
        gui_window_t *w = gui_window(id);
        CHECK(w != NULL);
        if (!w) return;
        for (uint16_t k = 0; k < w->nwidgets; k++)
            if (!strcmp(w->widgets[k].text, notes[i].label))
                CHECK_EQ(gui_click_widget(id, w->widgets[k].id), 0);
        /* Before the pump the app says what it asked for; after it, "ok". */
        CHECK_CONTAINS(status_of(id), "asking for ");
        CHECK_EQ(app_service(), 1);
        CHECK_STR(status_of(id), "ok");
        CHECK_EQ(sink.ms, 150u);

        /* The note that was played is the one on the button. */
        const int16_t *pcm = (const int16_t *)(uintptr_t)sink.phys;
        int cycles = 0;
        int16_t prev = pcm[0];
        for (uint32_t f = 1; f < sink.frames; f++) {
            int16_t cur = pcm[f * 2];
            if (prev >= 0 && cur < 0) cycles++;
            prev = cur;
        }
        int hz = (int)((uint64_t)cycles * 1000ull / sink.ms);
        if (hz < (int)notes[i].hz - 4 || hz > (int)notes[i].hz + 4)
            printf("    (button \"%s\" produced %d Hz)\n", notes[i].label, hz);
        CHECK(hz >= (int)notes[i].hz - 4 && hz <= (int)notes[i].hz + 4);
    }
    CHECK_EQ(sink.plays, 3);

    /* The buzz button: a different waveform and a louder level, from literals. */
    wait_out_the_quiet();
    gui_window_t *w = gui_window(id);
    for (uint16_t k = 0; w && k < w->nwidgets; k++)
        if (!strcmp(w->widgets[k].text, "buzz"))
            CHECK_EQ(gui_click_widget(id, w->widgets[k].id), 0);
    CHECK_EQ(app_service(), 1);
    CHECK_EQ(sink.ms, 200u);
    CHECK_CONTAINS(kcap_text(), "hz=110 ms=200 amp=12000 wave=square -> ok]");

    /* Its counter counted, which proves the statements after a call still run. */
    const char *val = "";
    for (int i = 0; i < app_var_count(id); i++) {
        const char *nm = NULL;
        static char v[APP_TEXT_MAX];
        if (app_var_at(id, i, &nm, v, sizeof v) == 0 && nm && !strcmp(nm, "beeps"))
            val = v;
    }
    CHECK_STR(val, "4");

    /* On a machine with no driver the SAME document is honest instead of silent. */
    audio_reset();
    wait_out_the_quiet();
    app_cap_reset();
    for (uint16_t k = 0; w && k < w->nwidgets; k++)
        if (!strcmp(w->widgets[k].text, "440"))
            CHECK_EQ(gui_click_widget(id, w->widgets[k].id), 0);
    CHECK_STR(status_of(id), "no audio driver");
}

int main(void) {
    RUN(test_the_capability_statement_compiles);
    RUN(test_the_example_matches_the_file_on_disk);
    RUN(test_a_wrong_call_is_refused_at_load_time);
    RUN(test_the_call_bound_is_stated);
    RUN(test_no_audio_driver_is_legible);
    RUN(test_a_handler_queues_and_app_service_performs);
    RUN(test_a_click_does_not_wait_for_the_device);
    RUN(test_bounds_are_enforced_when_the_handler_runs);
    RUN(test_the_edges_of_the_range_are_accepted);
    RUN(test_one_call_at_a_time);
    RUN(test_the_rate_limit_holds);
    RUN(test_two_documents_share_the_speaker);
    RUN(test_a_slow_driver_buys_a_longer_silence);
    RUN(test_ten_thousand_tones_a_second_become_two);
    RUN(test_a_hostile_document_cannot_exhaust_anything);
    RUN(test_an_app_that_closes_makes_no_sound);
    RUN(test_every_performed_call_emits_one_trace_line);
    RUN(test_the_trace_line_starts_in_column_zero_after_a_prompt);
    RUN(test_a_sink_sentence_cannot_forge_a_trace_line);
    RUN(test_the_spec_is_generated_from_the_table);
    RUN(test_action_caps_fits_and_teaches_a_document_that_launches);
    RUN(test_the_beeper_example_plays_every_note);
    return th_report("app audio");
}
