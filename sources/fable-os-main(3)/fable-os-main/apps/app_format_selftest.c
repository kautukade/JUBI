/* app_format_selftest.c — launch every worked example on real hardware and let a
 *                         periodic app actually tick, at boot.
 *
 * ============================================================================
 * COMPILED OUT OF EVERY NORMAL BUILD. Without -DFABLEOS_APP_FORMAT_SELFTEST this
 * file compiles to nothing at all: no driver is registered, so nothing in the
 * boot path can reach it.
 * ============================================================================
 *
 * WHY IT HAS TO EXIST
 *   tests/host/test_app_format.c proves every one of the format additions
 *   numerically, with the clock and the dice pinned — which is the right way to
 *   test them and says nothing about two claims that only a boot can settle:
 *
 *     1. these documents RENDER. A window that lays out wrong is worse than one
 *        that refuses to open, and only a framebuffer can say which happened.
 *     2. a tick handler really runs, on this machine's own clock, repeatedly,
 *        while the kernel is otherwise idle. The host test moves time by hand on
 *        purpose; here nobody does, so a screenshot six seconds after another one
 *        is the honest evidence.
 *
 *   So this runs the whole set: hello, clock, stopwatch and dice at once (the
 *   instance pool is four), the stopwatch started and the dice rolled through the
 *   real click path, then a pump loop that calls app_tick() for a bounded number
 *   of milliseconds — and finally hello is closed and the password generator takes
 *   its place, so all five examples appear in one boot.
 *
 *   THE SERIAL LOG IS THE OTHER HALF OF THE PROOF. Every widget's text is printed
 *   at intervals, so "the clock advanced" is checkable from the log as well as
 *   from two PNGs, and a wrong value can be attributed without reading pixels.
 *
 *       make EXTRA_CFLAGS=-DFABLEOS_APP_FORMAT_SELFTEST
 *       FABLEOS_ENV=/nonexistent gui/gui_probe.sh apps/probe/format_probe.plan \
 *           /tmp/appfmt
 *
 * WHY IT IS A DRIVER AT DRV_LEVEL_LATE
 *   The same reason apps/app_selftest.c is: kernel/main.c is not this module's to
 *   edit, and the driver table is the machine's supported way for a subsystem to
 *   run code at boot (driver.h). LATE means every device is up — which here also
 *   means the RTC is up, so `clock` reads a real chip.
 *
 * WHAT IT DOES NOT PROVE, AND SAYS SO
 *   app_tick() is called by THIS FILE's pump loop. In a live session the caller
 *   has to be the loop that waits for a sentence (kernel/main.c's
 *   wait_for_sentence, next to gui_tick()) — one line, in a file this module does
 *   not own. Until that line exists, a clock launched by the model is correct but
 *   frozen, so the boot log below says which is which rather than implying the
 *   integration is done.
 */

#if defined(FABLEOS_APP_FORMAT_SELFTEST) && !defined(FABLEOS_HOSTTEST)

#include "app.h"
#include "gui.h"
#include "driver.h"
#include "kernel.h"

/* Milliseconds of ticking to give the periodic apps. Long enough that two
 * screenshots several seconds apart must differ, short enough that the boot still
 * reaches the prompt. */
#ifndef FABLEOS_APP_FORMAT_MS
#  define FABLEOS_APP_FORMAT_MS 14000u
#endif

/* Where each window goes. Side by side rather than cascaded, because a screenshot
 * of four overlapping windows proves the top one. The x of the last one plus its
 * width has to stay inside 1024: gui_window_open() does not push a window back
 * on screen, and the first run of this file put the dice half off the edge —
 * which is exactly the kind of thing only a screenshot catches. */
static const struct { const char *name; int32_t x, y; } layout[] = {
    { "hello",     40, 40 },
    { "clock",    300, 40 },
    { "stopwatch",560, 40 },
    { "dice",     818, 40 },
};
#define NLAYOUT ((int)(sizeof layout / sizeof layout[0]))

static uint32_t ids[NLAYOUT];

static uint32_t launch_example(const char *name, int32_t x, int32_t y) {
    app_error_t err;
    uint32_t    id  = 0;
    const char *doc = app_example_doc(name);

    if (!doc) {
        kprintf("app-format: no example called \"%s\"\n", name);
        return 0;
    }
    int rc = app_launch(doc, 0, x, y, &id, &err);
    if (rc != APP_OK) {
        kprintf("app-format: \"%s\" was REFUSED (rc=%d) at %s: %s\n",
                name, rc, err.path[0] ? err.path : "(document)", err.msg);
        return 0;
    }
    kprintf("app-format: launched \"%s\" as id=%u \"%s\" at %d,%d\n",
            name, (unsigned)id, app_title(id), (int)x, (int)y);
    return id;
}

/* One line per widget, straight out of the runtime — the same text the `app`
 * tool's action=state would show the model. */
static void dump(uint32_t id) {
    static char buf[1024];
    if (!id) return;
    app_describe(id, buf, sizeof buf);
    kprintf("app-format: id=%u \"%s\"\n%s", (unsigned)id, app_title(id), buf);
}

/* Press a control by its visible text, through gui_click_widget() — the same path
 * a PS/2 mouse takes, so this is a real click and not a call into the handler. */
static void press(uint32_t id, const char *text) {
    gui_window_t *win = gui_window(id);
    if (!win) return;
    for (uint16_t i = 0; i < win->nwidgets; i++) {
        gui_widget_t *w = &win->widgets[i];
        size_t        k = 0;
        while (text[k] && w->text[k] == text[k]) k++;
        if (text[k] || w->text[k]) continue;
        int rc = gui_click_widget(id, w->id);
        kprintf("app-format: clicked \"%s\" on id=%u (rc=%d)\n", text,
                (unsigned)id, rc);
        return;
    }
    kprintf("app-format: id=%u has no control reading \"%s\"\n", (unsigned)id,
            text);
}

/* The pump. THIS IS THE LOOP kernel/main.c's wait_for_sentence() needs one line
 * of: app_tick() to run what is due, then the damage flushed. It is bounded by
 * `ms` and by app_tick()'s own guarantees (include/app.h), and it polls no
 * hardware — in particular not the 8042, which the keyboard driver would race
 * (DECISION 4 in include/gui.h). */
static void pump(uint32_t ms, const char *why) {
    uint64_t deadline = millis() + ms;
    uint64_t report   = millis();
    uint32_t rounds   = 0;

    kprintf("app-format: pumping app_tick() for %u ms (%s)\n", (unsigned)ms, why);
    while (millis() < deadline) {
        rounds += (uint32_t)app_tick();
        gui_sync();
        if (millis() >= report) {
            report = millis() + 2000;
            /* Ground truth in the serial log: the periodic apps' own text, so
             * "it advanced" is readable without a screenshot. */
            for (int i = 0; i < NLAYOUT; i++) {
                gui_window_t *w = ids[i] ? gui_window(ids[i]) : (gui_window_t *)0;
                if (!w || !w->nwidgets) continue;
                kprintf("app-format: t=%us %s reads \"%s\"\n",
                        (unsigned)(millis() / 1000), layout[i].name,
                        w->widgets[0].text);
            }
        }
    }
    const app_stats_t *s = app_stats();
    kprintf("app-format: %u handler run(s) in that window; totals: %u tick(s), "
            "%u event(s), %u step(s), %u err value(s), %u step limit(s)\n",
            (unsigned)rounds, (unsigned)s->ticks, (unsigned)s->events,
            (unsigned)s->steps, (unsigned)s->err_values,
            (unsigned)s->step_limits);
}

static int app_format_selftest_init(void) {
    kputs("\n--- app format self-test: the worked examples on real hardware ---\n");

    if (!gui_attached() && (gui_init(), !gui_attached())) {
        kputs("app-format: no framebuffer, so there is no window manager and no "
              "app can be launched. This is the honest outcome on a VGA text "
              "console.\n--- end of app format self-test ---\n");
        return 0;
    }

    for (int i = 0; i < NLAYOUT; i++)
        ids[i] = launch_example(layout[i].name, layout[i].x, layout[i].y);
    kprintf("app-format: %d app(s) and %d window(s) open\n", app_count(),
            gui_window_count());

    /* Start the stopwatch and roll the dice through real clicks. SIX ROLLS, each
     * printed: one roll would not distinguish real entropy from a draw that is
     * the same on every boot, and RDRAND behind a hypervisor is exactly the kind
     * of thing to check rather than assume. */
    press(ids[2], "start");
    for (int i = 0; i < 6; i++) {
        gui_window_t *w;
        press(ids[3], "roll");
        w = ids[3] ? gui_window(ids[3]) : (gui_window_t *)0;
        if (w && w->nwidgets)
            kprintf("app-format: roll %d -> \"%s\"\n", i + 1, w->widgets[0].text);
    }
    for (int i = 0; i < NLAYOUT; i++) dump(ids[i]);
    gui_sync();

    pump(FABLEOS_APP_FORMAT_MS, "a clock and a stopwatch have to be seen moving");
    for (int i = 0; i < NLAYOUT; i++) dump(ids[i]);

    /* Stop the stopwatch: the display must then hold still while the clock beside
     * it keeps moving, which is the difference between "it ticks" and "the whole
     * screen is being redrawn". */
    press(ids[2], "stop");
    pump(4000, "the stopwatch is stopped and the clock is not");
    dump(ids[2]);

    /* The fifth example needs a slot, and the instance pool is APP_MAX_INSTANCES:
     * closing hello is also the teardown path being exercised on hardware. */
    if (ids[0] && app_close(ids[0]) == APP_OK) {
        kprintf("app-format: closed hello; %d app slot(s) in use\n", app_count());
        ids[0] = 0;
    }
    uint32_t pw = launch_example("password", 40, 40);
    press(pw, "generate");
    dump(pw);
    gui_sync();
    pump(3000, "the password generator is on screen");

    kprintf("app-format: finished with %d app(s) and %d window(s) open. NOTE: "
            "app_tick() was called by this self-test's own pump loop; a live "
            "session needs it called from the loop that waits for a sentence "
            "(kernel/main.c), which is one line this module does not own.\n",
            app_count(), gui_window_count());
    kputs("--- end of app format self-test ---\n");
    return 0;
}

static const driver_t app_format_selftest_driver = {
    .name  = "appfmtselftest",
    .level = DRV_LEVEL_LATE,
    .cls   = DEV_CLASS_NONE,
    .init  = app_format_selftest_init,
};
REGISTER_DRIVER(app_format_selftest_driver);

#else

/* Keeps this translation unit non-empty in a normal build, where the whole file
 * above compiles to nothing. */
typedef int fableos_app_format_selftest_not_built_t;

#endif
