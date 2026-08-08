/* app_audio_selftest.c — an app asks the kernel for a sound, on real hardware.
 *
 * ============================================================================
 * COMPILED OUT OF EVERY NORMAL BUILD. Without -DFABLEOS_APP_AUDIO_SELFTEST this
 * file compiles to nothing at all: no driver is registered, so nothing in the
 * boot path can reach it, and the null sink below cannot exist in a shipped
 * kernel.
 * ============================================================================
 *
 *     make EXTRA_CFLAGS=-DFABLEOS_APP_AUDIO_SELFTEST && ./capture.sh 14
 *
 * WHY IT HAS TO EXIST
 *   apps/cap.c is provable on the host: the statement compiles, the bounds hold,
 *   a synthetic sink receives the PCM, and a handler provably never reaches the
 *   driver. Four claims are NOT provable there:
 *
 *     1. a real click on a real window, through the PS/2 mouse path, reaches the
 *        broker at all;
 *     2. the KERNEL TRACE LINE for an app-driven sound appears in the console the
 *        operator is really reading (a host test asserts a capture buffer);
 *     3. the request is drained by the loop that waits for a sentence
 *        (kernel/main.c calls app_tick(), which calls app_service()) rather than
 *        by a pump this file wrote — so the last thing it does is leave a ticking
 *        app running and let the machine reach the prompt;
 *     4. and the operator can still type while that is happening.
 *
 *   It follows apps/app_selftest.c exactly: a DRV_LEVEL_LATE driver, because
 *   kernel/main.c is not this module's to edit and the driver table is the
 *   supported way for a subsystem to run code at boot (driver.h).
 *
 * ============================================================================
 * THE SINK HERE IS A NULL SINK, AND NOTHING IT PLAYS IS AUDIBLE
 * ============================================================================
 *   Nothing in this kernel yet calls audio_register_native() or
 *   audio_register_vm(): a model-authored driver installs itself through neither,
 *   so on every machine in this tree `audio_available()` is 0 and the honest
 *   outcome of a capability call is a refusal. That is the FIRST half of this
 *   self-test, and it is the state the operator sees today.
 *
 *   The second half needs a sink to exist, so it registers one that ACCEPTS the
 *   PCM AND DISCARDS IT, holding the machine for the length of the sound exactly
 *   as a real device would. What that proves is the path and the ground truth:
 *   the click, the queue, the drain from the idle loop, the [app_call ...] line,
 *   the rate limit, and that the prompt still answers. What it does NOT prove is
 *   that anything came out of a speaker — no PCM reaches any device, and every
 *   line below says so where it could be misread. A real sound needs a real
 *   driver registered as the sink; when one is, this same document plays through
 *   it with no edit, because this file supplies nothing but a sink.
 */

#if defined(FABLEOS_APP_AUDIO_SELFTEST) && !defined(FABLEOS_HOSTTEST)

#include "app.h"
#include "audio.h"
#include "gui.h"
#include "driver.h"
#include "kernel.h"
#include "trace.h"

#include "examples/examples_json.h"

/* The worked example itself, apps/examples/beeper.json, embedded by
 * apps/examples/gen_header.py. Not a copy: the same bytes the .json holds, which
 * tests/host/test_app_audio.c diffs against the file on disk. */
static const char beeper[] = APP_BEEPER_JSON;

/* A document with a periodic call, so that after this file returns the machine's
 * OWN loop is the only thing driving anything. */
static const char metronome[] =
    "{\"title\":\"Metronome\",\"width\":210,\"height\":110,"
    "\"grid\":{\"rows\":2,\"cols\":1},\"vars\":{\"n\":0},"
    "\"widgets\":[{\"kind\":\"field\",\"name\":\"out\",\"text\":\"waiting\","
    "\"readonly\":true,\"row\":0,\"col\":0},"
    "{\"kind\":\"label\",\"name\":\"say\",\"text\":\"tick every 1s\","
    "\"align\":\"center\",\"row\":1,\"col\":0}],"
    "\"on\":[{\"tick\":1000,\"do\":["
    "{\"set\":\"n\",\"to\":\"n + 1\"},"
    "{\"set\":\"out\",\"to\":\"cat('beat ', text(n))\"},"
    "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"660\",\"ms\":\"120\"},"
    "\"into\":\"say\"}]}]}";

/* A document that asks for something impossible, to show a bound being enforced
 * on the real machine. It is here rather than in beeper.json because a shipped
 * example should be a model of good behaviour — but a refusal has to be visible
 * outside a host test too. 5 MHz is inside what a fixed-point value can hold, so
 * it survives compilation and is refused when the HANDLER RUNS, which is the
 * interesting path; 36000000 ms is ten hours. */
static const char too_much[] =
    "{\"title\":\"Too much\",\"width\":200,\"height\":110,"
    "\"grid\":{\"rows\":2,\"cols\":1},\"widgets\":["
    "{\"kind\":\"field\",\"name\":\"s\",\"text\":\"?\",\"readonly\":true,"
    "\"row\":0,\"col\":0},"
    "{\"kind\":\"button\",\"text\":\"5 MHz for 10 h\",\"name\":\"b\","
    "\"row\":1,\"col\":0}],"
    "\"on\":[{\"click\":\"b\",\"do\":[{\"call\":\"audio.tone\","
    "\"with\":{\"hz\":\"5000000\",\"ms\":\"36000000\"},\"into\":\"s\"}]}]}";

/* ====================================================================== */
/* the null sink                                                          */
/* ====================================================================== */

static uint32_t null_plays;
static uint32_t null_bytes_last;

/* Accept the samples, hold the machine for as long as the sound lasts — audio.h's
 * play contract is synchronous, and pretending otherwise would make the timing
 * measurements below meaningless — and discard them. */
static int null_play(void *ctx, uint64_t phys, uint32_t bytes, uint32_t frames,
                     uint32_t ms, char *why, size_t cap) {
    (void)ctx; (void)phys; (void)frames; (void)why; (void)cap;
    null_plays++;
    null_bytes_last = bytes;
    mdelay(ms);
    return 0;
}

static void install_null_sink(void) {
    audio_format_t fmt;
    audio_ops_t    ops;

    fmt.rate_hz  = 48000;
    fmt.channels = 2;
    fmt.bits     = 16;
    ops.ctx  = (void *)0;
    ops.play = null_play;
    ops.stop = (int (*)(void *))0;

    int rc = audio_register_native("nullsink", (struct device *)0, &fmt, &ops);
    kprintf("app-audio: registered a NULL SINK (rc=%d). IT MAKES NO SOUND: it "
            "takes the PCM, waits as long as the sound lasts and throws it away. "
            "It is here so the path and the trace line can be proved on real "
            "hardware; nothing below is audible.\n", rc);
    if (rc != AUDIO_OK)
        kprintf("app-audio: registration failed: %s\n", audio_last_error());
}

/* ====================================================================== */
/* helpers                                                                */
/* ====================================================================== */

static uint32_t launch(const char *doc, const char *what, int32_t x, int32_t y) {
    app_error_t err;
    uint32_t    id = 0;
    int rc = app_launch(doc, 0, x, y, &id, &err);
    if (rc != APP_OK) {
        kprintf("app-audio: %s was REFUSED (rc=%d) at %s: %s\n", what, rc,
                err.path[0] ? err.path : "(document)", err.msg);
        return 0;
    }
    kprintf("app-audio: launched %s as id=%u \"%s\"\n", what, (unsigned)id,
            app_title(id));
    return id;
}

/* Press a control by its visible text, through gui_click_widget() — the same path
 * a PS/2 mouse takes, so this is a real click and not a call into the handler. */
static int press(uint32_t id, const char *text) {
    gui_window_t *win = gui_window(id);
    if (!win) return -1;
    for (uint16_t i = 0; i < win->nwidgets; i++) {
        gui_widget_t *w = &win->widgets[i];
        size_t        k = 0;
        while (text[k] && w->text[k] == text[k]) k++;
        if (text[k] || w->text[k]) continue;
        return gui_click_widget(id, w->id);
    }
    kprintf("app-audio: id=%u has no control reading \"%s\"\n", (unsigned)id,
            text);
    return -1;
}

/* What the app itself is showing, which is the only thing the operator can see.
 * The status field is widget 0 of both documents. */
static const char *status_of(uint32_t id) {
    gui_window_t *w = gui_window(id);
    if (!w || !w->nwidgets) return "(no window)";
    return w->widgets[0].text;
}

static void report(uint32_t id, const char *when) {
    const app_stats_t *s = app_stats();
    kprintf("app-audio: %s: the app shows \"%s\"; %u accepted, %u refused, %u "
            "performed\n", when, status_of(id), (unsigned)s->calls,
            (unsigned)s->calls_refused, (unsigned)s->hw_actions);
    const char *note = app_last_error(id);
    if (note && note[0]) kprintf("app-audio:   its note: %s\n", note);
}

/* ====================================================================== */
/* the self-test                                                          */
/* ====================================================================== */

static int app_audio_selftest_init(void) {
    kputs("\n--- app audio self-test: a document asks the kernel for a sound ---\n");

    if (!gui_attached() && (gui_init(), !gui_attached())) {
        kputs("app-audio: no framebuffer, so no window and no clicks. This is "
              "the honest outcome on a VGA text console.\n"
              "--- end of app audio self-test ---\n");
        return 0;
    }

    /* ---- 1. THE STATE OF EVERY MACHINE IN THIS TREE: no sink at all ---- */
    kprintf("app-audio: audio_available()=%d - no driver has registered as the "
            "audio sink, so every capability call must be REFUSED with a "
            "sentence the app can show.\n", audio_available());

    uint32_t id = launch(beeper, "apps/examples/beeper.json", 40, 400);
    if (!id) { kputs("--- end of app audio self-test ---\n"); return 0; }
    gui_sync();

    (void)press(id, "440");
    gui_sync();
    report(id, "with no audio driver, after a real click on \"440\"");
    kprintf("app-audio: nothing was queued and app_service() has nothing to do "
            "(%d), so the kernel printed NO trace line: an app cannot claim a "
            "sound that did not happen\n", app_service());

    /* ---- 2. WITH A SINK: the whole path, and the ground truth ---- */
    install_null_sink();

    uint64_t before_lines = trace_emissions();
    uint32_t plays_before = null_plays;
    uint64_t t0 = millis();
    int      rc = press(id, "440");
    uint64_t t1 = millis();
    kprintf("app-audio: clicked \"440\" (rc=%d). It took %u ms, the sink was "
            "asked %u time(s) and %u trace line(s) were printed - the handler "
            "QUEUED the sound and returned; it did not play it\n",
            rc, (unsigned)(t1 - t0), (unsigned)(null_plays - plays_before),
            (unsigned)(trace_emissions() - before_lines));

    kputs("app-audio: now one app_service(), which is what kernel/main.c's idle "
          "loop calls. The bracketed line under this one is the kernel's own "
          "record of the hardware action:\n");
    t0 = millis();
    int did = app_service();
    t1 = millis();
    kprintf("app-audio: app_service() performed %d call(s) in %u ms (the sink "
            "held the machine for the length of the sound, as a real device "
            "does), sink asked %u time(s), %u bytes of PCM\n",
            did, (unsigned)(t1 - t0), (unsigned)null_plays,
            (unsigned)null_bytes_last);
    gui_sync();
    report(id, "after the pump");

    /* ---- 3. THE RATE LIMIT, on the real machine ---- */
    (void)press(id, "554");
    (void)press(id, "659");
    gui_sync();
    report(id, "after two more clicks with no pause");

    /* ---- 4. A BOUND, enforced when the handler runs ----
     * Waited out first: the rate limit is a TRANSIENT refusal and the bound is a
     * permanent one, so without this pause the log would show "too soon" and
     * prove nothing about the bound. */
    uint32_t bad = launch(too_much, "a 5 MHz, ten-hour tone", 330, 400);
    if (bad) {
        mdelay(APP_CAP_GAP_MS + 20);
        (void)press(bad, "5 MHz for 10 h");
        gui_sync();
        kprintf("app-audio: that app shows \"%s\"\n", status_of(bad));
        kprintf("app-audio:   the kernel's reason: %s\n", app_last_error(bad));
        uint32_t plays = null_plays;
        (void)app_service();
        kprintf("app-audio: after the pump the sink has still been asked %u "
                "time(s) (was %u): a refused call touches no hardware and "
                "prints no trace line\n", (unsigned)null_plays,
                (unsigned)plays);
        (void)app_close(bad);
        gui_sync();
    }

    /* ---- 5. HAND IT TO THE MACHINE'S OWN LOOP ---- */
    (void)launch(metronome, "a metronome that asks every second", 330, 400);
    gui_sync();

    const app_stats_t *s = app_stats();
    kprintf("app-audio: totals so far: %u accepted, %u refused, %u performed, "
            "%u handler run(s)\n", (unsigned)s->calls,
            (unsigned)s->calls_refused, (unsigned)s->hw_actions,
            (unsigned)s->events);
    kputs("app-audio: FROM HERE NOTHING IN THIS FILE RUNS AGAIN. The metronome "
          "asks for a sound every second and the machine's own idle loop "
          "(wait_for_sentence -> app_tick -> app_service) is what performs it, so "
          "every further [app_call ...] line below was printed with no help from "
          "this self-test. Type a sentence at the prompt while they appear: the "
          "answer comes back between them.\n");
    kputs("--- end of app audio self-test ---\n");
    return 0;
}

static const driver_t app_audio_selftest_driver = {
    .name  = "appaudioselftest",
    .level = DRV_LEVEL_LATE,
    .cls   = DEV_CLASS_NONE,
    .init  = app_audio_selftest_init,
};
REGISTER_DRIVER(app_audio_selftest_driver);

#else

/* Keeps this translation unit non-empty in a normal build, where the whole file
 * above compiles to nothing. */
typedef int fableos_app_audio_selftest_not_built_t;

#endif
