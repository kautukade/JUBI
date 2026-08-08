/* gui_demo.c — the reference apps: a calculator and a notepad.
 *
 * PURPOSE
 *   "I want a calculator" has to produce a calculator, today, on this machine.
 *   A later phase lets the model author apps; this file is what an app looks
 *   like when a human writes one, and it is deliberately the ONLY consumer of
 *   the toolkit that ships — if a call is awkward here, it will be awkward for
 *   the model, and it was changed in widgets.h rather than worked around.
 *
 *   It is also the end-to-end test subject. tests/host/test_gui.c opens the
 *   calculator into a memory framebuffer, clicks the pixels where "7", "+", "5"
 *   and "=" actually landed, and asserts the display reads 12. That single test
 *   covers layout arithmetic, hit testing, event dispatch, focus, damage and the
 *   app's own logic at once, which is the combination a screenshot cannot prove.
 *
 * AN APP IS THREE THINGS
 *   1. gui_window_open() — a frame.
 *   2. widgets laid out with gui_split_ and gui_grid over gui_client_area().
 *   3. one hook that switches on widget ids, mutates widget text, and calls
 *      gui_invalidate_widget(). It never draws and never presents.
 *
 * ARITHMETIC WITHOUT A FLOATING-POINT UNIT
 *   This kernel is built -mno-sse -mno-mmx and has no FPU state to save, so the
 *   calculator is fixed point: every value is an int64_t scaled by 10^6, which
 *   gives six decimal places and a range of +/-9.2e12 before the type itself
 *   overflows. Multiply and divide go through __int128 (libgcc supplies the
 *   helpers; mbedTLS already relies on them) so the intermediate cannot wrap,
 *   and every operation is range-checked against CALC_MAX before it is stored.
 *   Overflow and division by zero produce the word "error" on the display and a
 *   latched state that only C clears — a calculator that shows a wrong number is
 *   worse than one that admits it stopped.
 */

#include "gui.h"
#include "widgets.h"

#ifndef FABLEOS_HOSTTEST
#include "kernel.h"
#endif

/* ====================================================================== */
/* widget ids                                                             */
/* ====================================================================== */

#define CID_DISPLAY  1
#define CID_CLEAR    2
#define CID_EQUALS   3
#define CID_DOT      4
#define CID_ADD      5
#define CID_SUB      6
#define CID_MUL      7
#define CID_DIV      8
#define CID_DIGIT    100        /* + the digit, 100..109                    */

#define NID_TEXT     1
#define NID_COUNT    2
#define NID_CLEAR    3

/* ====================================================================== */
/* the calculator engine                                                  */
/* ====================================================================== */

#define CALC_SCALE   1000000LL              /* six decimal places          */
#define CALC_MAX     9000000000000000LL     /* 9e9 as a scaled value        */
#define CALC_DIGITS  12                     /* digits the operator may type  */

typedef struct calc {
    int      used;
    int64_t  acc;        /* the left-hand side, scaled                      */
    uint8_t  op;         /* pending operator id, 0 = none                   */
    uint8_t  fresh;      /* the display holds a result: the next digit wipes it */
    uint8_t  err;        /* latched until C                                 */
} calc_t;

/* One per possible window. Static because the GUI does not allocate: an app that
 * cannot open when memory is tight is an app that vanishes exactly when the
 * operator needs to be told why. */
static calc_t calcs[GUI_MAX_WINDOWS];

static calc_t *calc_alloc(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (!calcs[i].used) {
            calcs[i].used  = 1;
            calcs[i].acc   = 0;
            calcs[i].op    = 0;
            calcs[i].fresh = 1;
            calcs[i].err   = 0;
            return &calcs[i];
        }
    return (calc_t *)0;
}

/* ---- text <-> scaled integer ----
 * Both directions are hand-rolled: there is no strtod, no %f in the kernel's
 * printf, and a calculator's formatting rules (trim trailing zeros, keep the
 * point only when it earns its place) are not sprintf's anyway. */

/* Returns 0 on success, -1 on overflow / malformed. Accepts an optional '-',
 * digits, an optional '.', digits. */
static int calc_parse(const char *s, int64_t *out) {
    int64_t whole = 0, frac = 0, scale = CALC_SCALE;
    int     neg = 0, seen = 0;

    if (!s) return -1;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        if (whole > CALC_MAX / 10) return -1;
        whole = whole * 10 + (*s - '0');
        if (whole > CALC_MAX / CALC_SCALE) return -1;
        s++;
        seen = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            scale /= 10;
            if (scale > 0) frac += (int64_t)(*s - '0') * scale;
            s++;
            seen = 1;
        }
    }
    if (*s != '\0' || !seen) return -1;

    int64_t v = whole * CALC_SCALE + frac;
    *out = neg ? -v : v;
    return 0;
}

static void calc_format(int64_t v, char *out, size_t cap) {
    char    tmp[40];
    int     n = 0;
    int     neg = v < 0;
    /* Negate in unsigned space: -INT64_MIN is undefined, and a calculator will
     * eventually be handed the extreme. */
    uint64_t mag = neg ? (~(uint64_t)v + 1u) : (uint64_t)v;

    uint64_t whole = mag / (uint64_t)CALC_SCALE;
    uint64_t frac  = mag % (uint64_t)CALC_SCALE;

    /* Fractional part, six digits, trailing zeros trimmed. */
    char fbuf[8];
    int  fn = 0;
    for (int i = 5; i >= 0; i--) {
        uint64_t d = frac;
        for (int k = 0; k < i; k++) d /= 10;
        fbuf[fn++] = (char)('0' + (int)(d % 10));
    }
    while (fn > 0 && fbuf[fn - 1] == '0') fn--;

    if (whole == 0) tmp[n++] = '0';
    while (whole > 0 && n < (int)sizeof tmp - 12) {
        tmp[n++] = (char)('0' + (int)(whole % 10));
        whole /= 10;
    }
    /* tmp holds the integer part reversed; emit it forwards. */
    size_t o = 0;
    if (neg && o + 1 < cap) out[o++] = '-';
    for (int i = n - 1; i >= 0 && o + 1 < cap; i--) out[o++] = tmp[i];
    if (fn > 0 && o + 1 < cap) {
        out[o++] = '.';
        for (int i = 0; i < fn && o + 1 < cap; i++) out[o++] = fbuf[i];
    }
    out[o] = '\0';
}

/* Returns 0 on success, -1 on overflow or division by zero. */
static int calc_apply(int64_t a, int64_t b, uint8_t op, int64_t *out) {
    __int128 r;
    switch (op) {
    case CID_ADD: r = (__int128)a + b; break;
    case CID_SUB: r = (__int128)a - b; break;
    case CID_MUL: r = ((__int128)a * b) / CALC_SCALE; break;
    case CID_DIV:
        if (b == 0) return -1;
        r = ((__int128)a * CALC_SCALE) / b;
        break;
    default: r = b; break;
    }
    if (r > (__int128)CALC_MAX || r < -(__int128)CALC_MAX) return -1;
    *out = (int64_t)r;
    return 0;
}

/* ====================================================================== */
/* the calculator's event hook                                            */
/* ====================================================================== */

static int digits_in(const char *s) {
    int n = 0;
    for (; *s; s++) if (*s >= '0' && *s <= '9') n++;
    return n;
}

static int has_dot(const char *s) {
    for (; *s; s++) if (*s == '.') return 1;
    return 0;
}

static void calc_error(gui_window_t *win, calc_t *c, gui_widget_t *d) {
    c->err   = 1;
    c->op    = 0;
    c->fresh = 1;
    gui_set_text(d, "error");
    gui_invalidate_widget(win->id, d);
}

static int calc_event(gui_window_t *win, gui_widget_t *w, const gui_event_t *ev) {
    calc_t *c = (calc_t *)win->user;
    if (!c) return 0;

    if (ev->kind == GUI_EV_CLOSE) { c->used = 0; win->user = (void *)0; return 0; }
    if (ev->kind != GUI_EV_CLICK || !w) return 0;

    gui_widget_t *d = gui_widget_by_id(win, CID_DISPLAY);
    if (!d) return 0;

    uint32_t id = w->id;

    if (id == CID_CLEAR) {
        c->acc = 0; c->op = 0; c->fresh = 1; c->err = 0;
        gui_set_text(d, "0");
        gui_invalidate_widget(win->id, d);
        return 1;
    }
    if (c->err) return 1;                     /* only C clears an error */

    if (id >= CID_DIGIT && id <= CID_DIGIT + 9) {
        char ch[2] = { (char)('0' + (int)(id - CID_DIGIT)), '\0' };
        if (c->fresh) { gui_set_text(d, ""); c->fresh = 0; }
        if (digits_in(gui_text(d)) >= CALC_DIGITS) return 1;
        /* A leading zero is replaced rather than accumulated: "007" is not a
         * number anyone typed on purpose. */
        if (gui_text(d)[0] == '0' && gui_text(d)[1] == '\0') gui_set_text(d, "");
        gui_append_text(d, ch);
        if (gui_text(d)[0] == '\0') gui_set_text(d, "0");
        gui_invalidate_widget(win->id, d);
        return 1;
    }
    if (id == CID_DOT) {
        if (c->fresh) { gui_set_text(d, "0"); c->fresh = 0; }
        if (!has_dot(gui_text(d))) {
            if (gui_text(d)[0] == '\0') gui_set_text(d, "0");
            gui_append_text(d, ".");
            gui_invalidate_widget(win->id, d);
        }
        return 1;
    }
    if (id == CID_ADD || id == CID_SUB || id == CID_MUL || id == CID_DIV ||
        id == CID_EQUALS) {
        int64_t v = 0;
        if (calc_parse(gui_text(d), &v) != 0) { calc_error(win, c, d); return 1; }

        if (c->op) {
            int64_t r;
            if (calc_apply(c->acc, v, c->op, &r) != 0) {
                calc_error(win, c, d);
                return 1;
            }
            c->acc = r;
        } else {
            c->acc = v;
        }
        c->op    = (id == CID_EQUALS) ? 0 : (uint8_t)id;
        c->fresh = 1;

        char out[GUI_TEXT_MAX];
        calc_format(c->acc, out, sizeof out);
        gui_set_text(d, out);
        gui_invalidate_widget(win->id, d);
        return 1;
    }
    return 0;
}

/* ====================================================================== */
/* building the calculator                                                */
/* ====================================================================== */

/* The keypad, row-major. Reads like the thing it draws, which is the point:
 * this is the shape a model authoring an app has to produce. */
static const struct { const char *label; uint32_t id; } keypad[20] = {
    { "7", CID_DIGIT + 7 }, { "8", CID_DIGIT + 8 }, { "9", CID_DIGIT + 9 }, { "/", CID_DIV },
    { "4", CID_DIGIT + 4 }, { "5", CID_DIGIT + 5 }, { "6", CID_DIGIT + 6 }, { "*", CID_MUL },
    { "1", CID_DIGIT + 1 }, { "2", CID_DIGIT + 2 }, { "3", CID_DIGIT + 3 }, { "-", CID_SUB },
    { "0", CID_DIGIT + 0 }, { ".", CID_DOT      }, { "C", CID_CLEAR      }, { "+", CID_ADD },
    /* row 4 is one wide key; the first entry spans it and the rest are unused. */
    { "=", CID_EQUALS }, { 0, 0 }, { 0, 0 }, { 0, 0 },
};

uint32_t gui_demo_calculator(int32_t x, int32_t y) {
    uint32_t id = gui_window_open("Calculator", x, y, 216, 266, 0);
    if (!id) return 0;

    gui_window_t *win = gui_window(id);
    calc_t       *c   = calc_alloc();
    if (!c) { (void)gui_window_close(id); return 0; }
    win->user = c;

    gui_rect_t area = gui_inset(gui_client_area(win), 6, 6);
    gui_rect_t keys;
    gui_rect_t disp = gui_split_top(area, 34, 6, &keys);

    /* The display is READONLY *and* GUI_W_NOHIT. Not being hit-testable is not
     * cosmetic: gui_click resolves a widget by its visible label, and the moment
     * the display reads "7" a click on the "7" key would be ambiguous with a
     * click on the display. A readout is scenery, so it says so. */
    gui_widget_t *d = gui_add_field(win, disp, "0", CID_DISPLAY,
                                    GUI_W_READONLY | GUI_W_NOHIT);
    if (d) d->align = GUI_ALIGN_RIGHT;

    for (int r = 0; r < 5; r++) {
        for (int col = 0; col < 4; col++) {
            const char *label = keypad[r * 4 + col].label;
            if (!label) continue;
            int span = (r == 4) ? 4 : 1;         /* "=" is a full-width key */
            gui_rect_t cell = gui_grid(keys, 5, 4, r, col, 1, span, 5);
            gui_add_button(win, cell, label, keypad[r * 4 + col].id);
            if (span == 4) break;
        }
    }

    gui_set_hook(id, calc_event);
    gui_invalidate_window(id);
    return id;
}

/* ====================================================================== */
/* the notepad — the app that uses the keyboard                            */
/* ====================================================================== */

/* Exists to exercise the one-line keyboard grab (gui.h DECISION 3) with
 * something an operator would plausibly want: click the field, type a line, and
 * the line is in the field instead of being sent to the model. */
static int notes_event(gui_window_t *win, gui_widget_t *w, const gui_event_t *ev) {
    if (!w) return 0;

    if (ev->kind == GUI_EV_CLICK && w->id == NID_CLEAR) {
        gui_widget_t *t = gui_widget_by_id(win, NID_TEXT);
        if (t) { gui_field_clear(t); gui_invalidate_widget(win->id, t); }
    } else if (ev->kind != GUI_EV_SUBMIT) {
        /* GUI_EV_KEY is deliberately NOT handled: returning 1 for it would tell
         * the WM the key was consumed and the field would never receive the
         * character. The count is refreshed on GUI_EV_SUBMIT, which the WM
         * delivers after a line lands in the field. */
        return 0;
    }

    gui_widget_t *t = gui_widget_by_id(win, NID_TEXT);
    gui_widget_t *n = gui_widget_by_id(win, NID_COUNT);
    if (t && n) {
        size_t len = 0;
        while (gui_text(t)[len]) len++;
        char buf[GUI_TEXT_MAX];
        size_t o = 0;
        const char *pre = "characters: ";
        while (*pre && o + 1 < sizeof buf) buf[o++] = *pre++;
        if (len == 0) { if (o + 1 < sizeof buf) buf[o++] = '0'; }
        else {
            char rev[8];
            int  k = 0;
            while (len > 0 && k < 7) { rev[k++] = (char)('0' + (int)(len % 10)); len /= 10; }
            while (k > 0 && o + 1 < sizeof buf) buf[o++] = rev[--k];
        }
        buf[o] = '\0';
        gui_set_text(n, buf);
        gui_invalidate_widget(win->id, n);
    }
    return 1;
}

static uint32_t gui_demo_notes(int32_t x, int32_t y) {
    uint32_t id = gui_window_open("Notes", x, y, 320, 124, 0);
    if (!id) return 0;

    gui_window_t *win  = gui_window(id);
    gui_rect_t    area = gui_inset(gui_client_area(win), 8, 8);

    gui_rect_t rest, row2, count;
    gui_rect_t hint = gui_split_top(area, 16, 4, &rest);
    gui_rect_t fld  = gui_split_top(rest, 28, 8, &row2);
    gui_rect_t btn  = gui_split_right(row2, 72, 8, &count);

    gui_add_label(win, hint, "click the box, then type a line", GUI_ALIGN_LEFT);
    gui_widget_t *t = gui_add_field(win, fld, "", NID_TEXT, 0);
    if (t) t->align = GUI_ALIGN_LEFT;
    gui_widget_t *cw = gui_add_label(win, count, "characters: 0", GUI_ALIGN_LEFT);
    if (cw) cw->id = NID_COUNT;
    gui_add_button(win, btn, "clear", NID_CLEAR);

    gui_set_hook(id, notes_event);
    gui_invalidate_window(id);
    return id;
}

const char *const *gui_demo_names(void) {
    static const char *const names[] = { "calculator", "notes", 0 };
    return names;
}

uint32_t gui_demo_open(const char *name, const char **why) {
    if (why) *why = "";
    if (!name || !name[0]) {
        if (why) *why = "no app name given";
        return 0;
    }
    /* Tiny hand-rolled compare: there is no strcmp in the freestanding kernel
     * and this file must compile on the host too. */
    const char *const *n = gui_demo_names();
    for (int i = 0; n[i]; i++) {
        const char *a = n[i], *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            uint32_t id = (i == 0) ? gui_demo_calculator(GUI_POS_AUTO, GUI_POS_AUTO)
                                   : gui_demo_notes(GUI_POS_AUTO, GUI_POS_AUTO);
            if (!id && why) *why = "no free window slot";
            return id;
        }
    }
    if (why) *why = "unknown app";
    return 0;
}
/* ====================================================================== */
/* the hardware self-test (compiled out of every normal build)              */
/* ====================================================================== */

/* The one thing a host test cannot prove is that a click from a REAL PS/2 mouse
 * reaches the right widget:
 *
 *   make EXTRA_CFLAGS=-DFABLEOS_GUI_SELFTEST
 *   make EXTRA_CFLAGS='-DFABLEOS_GUI_SELFTEST -DFABLEOS_GUI_WATCH_MS=30000'
 *
 * On the first gui_tick() (i.e. at the prompt, after every line of boot output)
 * this opens the calculator and the notepad and prints the screen centre of
 * every clickable control. gui/wm.c then runs a bounded watch window that
 * services the pointer and nothing else, and logs every dispatched event with
 * the widget's own label, so driving QEMU's monitor with mouse_move and
 * mouse_button shows which widget was hit and the display changing. Afterwards
 * the normal event loop resumes, so the same build also shows the prompt
 * working with windows on screen.
 *
 * Mirrors drivers/input/mouse.c's watch mode and drivers/acpi/power_selftest.c:
 * a diagnostic that has to run against hardware lives with the code it proves,
 * behind a flag, not in a test directory that cannot reach it. */
#if defined(FABLEOS_GUI_SELFTEST) && !defined(FABLEOS_HOSTTEST)

static void print_hit_targets(uint32_t id) {
    gui_window_t *w = gui_window(id);
    if (!w) return;
    gui_rect_t cl = gui_client_rect(w);
    for (uint16_t i = 0; i < w->nwidgets; i++) {
        gui_widget_t *x = &w->widgets[i];
        if (x->state & (GUI_W_NOHIT | GUI_W_DISABLED)) continue;
        kprintf("gui: hit \"%s\" win=%u centre=%d,%d\n", x->text, (unsigned)id,
                (int)(cl.x + x->r.x + x->r.w / 2),
                (int)(cl.y + x->r.y + x->r.h / 2));
    }
}

void gui_demo_selftest(void) {
    uint32_t c = gui_demo_calculator(560, 130);
    uint32_t n = gui_demo_notes(560, 430);
    kprintf("gui: selftest opened calculator=%u notes=%u\n",
            (unsigned)c, (unsigned)n);
    /* Where every control ACTUALLY landed, in screen pixels, computed by the
     * layout rather than by hand: gui/gui_probe.sh parses these lines and aims
     * the monitor's mouse at the centres, so the proof cannot drift the day a
     * metric changes. gui/wm.c then runs the watch window. */
    print_hit_targets(c);
    print_hit_targets(n);
}

#elif defined(FABLEOS_GUI_SELFTEST)

void gui_demo_selftest(void) { }

#endif
