/* app_selftest.c — launch the reference app document on real hardware, at boot.
 *
 * ============================================================================
 * COMPILED OUT OF EVERY NORMAL BUILD. Without -DFABLEOS_APP_SELFTEST this file
 * compiles to nothing at all: no driver is registered, so nothing in the boot
 * path can reach it.
 * ============================================================================
 *
 * WHY IT HAS TO EXIST
 *   Everything about the app runtime is provable on the host — the parser, the
 *   evaluator, the calculator's arithmetic, the tools, the whole turn loop
 *   against net/model_mock.c. Exactly one claim is not: that a document becomes a
 *   window a REAL PS/2 MOUSE can operate on real (virtual) hardware. That needs
 *   a boot, and a boot needs somebody to ask for the app — which normally means
 *   the model, and there is no API key in this kernel (the key never enters the
 *   build; see the Makefile), so an automated run can never reach a tool call.
 *
 *   So this file asks, through the same door the model uses:
 *
 *       make EXTRA_CFLAGS='-DFABLEOS_APP_SELFTEST -DFABLEOS_GUI_SELFTEST \
 *                          -DFABLEOS_GUI_WATCH_MS=25000'
 *       apps/probe/run_calc.sh            (drives the mouse, takes screenshots)
 *
 *   FABLEOS_APP_SELFTEST launches apps/examples/calculator.json.
 *   FABLEOS_GUI_SELFTEST (gui/wm.c) is what services a real mouse at the prompt:
 *   drivers/input/kbd.c drains 8042 bytes without testing the AUX bit, so the
 *   keyboard and the mouse race unless nothing else is polling — see DECISION 4
 *   in include/gui.h. The two flags are independent and compose.
 *
 * IT GOES THROUGH tool_dispatch(), NOT app_launch()
 *   Deliberately. The thing worth proving on hardware is the model's path, so the
 *   document is handed to the registered `app` TOOL as a tool_use input object
 *   ({"action":"launch","document":{...}}), exactly as chat.c would; the trace
 *   line and the result text below are the ones the model would see. The only
 *   difference from a live session is who typed the sentence.
 *
 * WHY IT IS A DRIVER AT DRV_LEVEL_LATE
 *   There is no other hook this file may use: kernel/main.c is not this module's
 *   to edit, and the driver table is the machine's supported way for a subsystem
 *   to run code at boot (driver.h). LATE means every device is up. gui_init() has
 *   not run yet at that point, so this calls it first — gui_attach() is
 *   documented as re-attachable, and main.c's later gui_init() leaves open
 *   windows alone.
 */

#if defined(FABLEOS_APP_SELFTEST) && !defined(FABLEOS_HOSTTEST)

#include "app.h"
#include "gui.h"
#include "driver.h"
#include "kernel.h"
#include "tool.h"
#include "trace.h"

#include "examples/calculator_json.h"

/* The tool input a model would send: {"x":..,"y":..,"document":{...}}. Built by
 * the preprocessor from the same string the kernel embeds, so this cannot drift
 * from apps/examples/calculator.json either. */
static const char launch_args[] =
    "{\"action\":\"launch\",\"x\":40,\"y\":430,\"document\":"
    APP_CALCULATOR_JSON "}";

static void report_targets(uint32_t id) {
    gui_window_t *w = gui_window(id);
    if (!w) return;
    gui_rect_t cl = gui_client_rect(w);
    for (uint16_t i = 0; i < w->nwidgets; i++) {
        gui_widget_t *x = &w->widgets[i];
        if (x->state & (GUI_W_NOHIT | GUI_W_DISABLED)) continue;
        /* The screen centre of every control, computed BY THE LAYOUT, so the
         * probe script can aim at it and the proof cannot drift the day a metric
         * changes. Mirrors gui/gui_demo.c's print_hit_targets(). */
        kprintf("app: hit \"%s\" win=%u centre=%d,%d\n", x->text, (unsigned)id,
                (int)(cl.x + x->r.x + x->r.w / 2),
                (int)(cl.y + x->r.y + x->r.h / 2));
    }
}

/* A document with one realistic mistake: the keys are tagged "digit" and the
 * handler selects "digits". It must be refused with the path, the reason and the
 * list of names that DO exist — and the machine must carry on booting. This is
 * the failure half of the loop, proved on the real machine rather than only in
 * tests/host/test_app.c. */
static const char bad_args[] =
    "{\"action\":\"launch\",\"document\":{"
    "\"title\":\"Broken\",\"width\":120,\"height\":90,"
    "\"grid\":{\"rows\":1,\"cols\":1},"
    "\"widgets\":[{\"kind\":\"button\",\"text\":\"7\",\"tag\":\"digit\","
    "\"row\":0,\"col\":0}],"
    "\"on\":[{\"click\":\"digits\",\"do\":[]}]}}";

static int dispatch_app(const char *args, const char *what) {
    static char   buf[1024];
    tool_result_t res;
    tool_call_t   call;
    size_t        n = 0;

    while (args[n]) n++;
    call.id        = "toolu_app_selftest";
    call.name      = "app";
    call.input     = args;
    call.input_len = n;

    tool_result_init(&res, buf, sizeof buf);
    uint64_t before = trace_emissions();
    int      rc     = tool_dispatch(&call, &res);

    kprintf("app-selftest: %s -> dispatch rc=%d is_error=%d trace lines=%d\n",
            what, rc, res.is_error, (int)(trace_emissions() - before));
    kputs("app-selftest: the result the model would have received:\n");
    kputs(buf);
    return res.is_error;
}

static int app_selftest_init(void) {
    size_t n = 0;

    kputs("\n--- app self-test: launching apps/examples/calculator.json ---\n");
    kprintf("app-selftest: the document is %d bytes of JSON, hand-written, "
            "embedded by apps/examples/gen_header.py\n",
            (int)(sizeof launch_args - 1));

    if (!gui_attached() && (gui_init(), !gui_attached())) {
        kputs("app-selftest: no framebuffer, so there is no window manager and "
              "no app can be launched. This is the honest outcome on a VGA text "
              "console.\n--- end of app self-test ---\n");
        return 0;
    }

    (void)n;
    (void)dispatch_app(launch_args, "the hand-written calculator");

    /* Read the outcome back out of the running app rather than asserting it. */
    if (app_count() > 0) {
        uint32_t id = app_at(0);
        kprintf("app-selftest: %d app(s) running; id=%u title=\"%s\" vars=%d\n",
                app_count(), (unsigned)id, app_title(id), app_var_count(id));
        report_targets(id);
    }

    /* And the other half: a broken document, refused, with the machine unharmed. */
    int refused = dispatch_app(bad_args, "a deliberately broken document");
    kprintf("app-selftest: the broken document was %s; %d app(s) and %d window(s) "
            "are still open and the machine is still booting\n",
            refused ? "REFUSED" : "ACCEPTED (which is a bug)",
            app_count(), gui_window_count());

    kputs("--- end of app self-test ---\n");
    return 0;
}

static const driver_t app_selftest_driver = {
    .name  = "appselftest",
    .level = DRV_LEVEL_LATE,
    .cls   = DEV_CLASS_NONE,
    .init  = app_selftest_init,
};
REGISTER_DRIVER(app_selftest_driver);

#else

/* Keeps this translation unit non-empty in a normal build, where the whole file
 * above compiles to nothing. */
typedef int fableos_app_selftest_not_built_t;

#endif
