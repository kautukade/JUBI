/* test_gui.c — the window manager and widget toolkit, host-native.
 *
 * WHAT THIS SUITE IS FOR
 *   A GUI is the subsystem where "it looked right in the screenshot" is the
 *   easiest possible lie: a window can be one pixel out, a click can land on the
 *   neighbouring key, a damage rectangle can be one row short and the bug only
 *   appears when something else happens to repaint. So everything here is
 *   asserted numerically, against a malloc'd fb_surface_t that is a real
 *   framebuffer in every respect except being on a screen:
 *
 *     1. rectangle algebra, including both exclusive edges
 *     2. gui_grid() tiling EXACTLY, at sizes that do not divide, with spans
 *     3. box splits
 *     4. window open/clamp/title sanitising/pool exhaustion
 *     5. z-order: hit order, raise, close, and the PIXELS in an overlap
 *     6. focus transitions and the hook order (blur before focus)
 *     7. damage: a superset of what changed, and nothing outside it moves
 *     8. exact pixels: border, title bar, client fill, button faces, the
 *        console text behind the windows
 *     9. hit testing at all four boundaries of a button, to the pixel
 *    10. dispatch: the right widget, press-release-inside activation, cancel by
 *        dragging off, disabled and non-hit widgets
 *    11. drag-to-move and the close box
 *    12. the one-line keyboard grab
 *    13. the calculator, driven by CLICKING PIXELS: 7 + 5 = 12, 1/3, 1/0, C
 *    14. coexistence with the console: text under a window is damage, text away
 *        from every window is not, and a scroll damages every window
 *    15. the four tools, including malformed and hostile input
 *
 *   Item 13 is the one that matters most. It is the demo — "I want a
 *   calculator" — reduced to an assertion: the clicks are at pixel coordinates
 *   derived from the layout, they go through the same dispatch path a PS/2 mouse
 *   uses, and the display is read back out of the widget afterwards.
 */

#include "harness.h"
#include "gui.h"
#include "widgets.h"
#include "fb.h"
#include "font.h"
#include "tool.h"
#include "trace.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* stand in for the linker-collected tool table                           */
/* ====================================================================== */

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

const tool_t *__start_tool_table[4] TBL_SECTION = { 0, 0, 0, 0 };
extern const tool_t *const __stop_tool_table[];

__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 32\n");
_Static_assert(sizeof(__start_tool_table) == 32,
               "tool count changed: update the __stop_tool_table byte offset");

static void install_tool_table(void) {
    __start_tool_table[0] = fableos_hosttool_gui_list_tool;
    __start_tool_table[1] = fableos_hosttool_gui_open_tool;
    __start_tool_table[2] = fableos_hosttool_gui_window_tool;
    __start_tool_table[3] = fableos_hosttool_gui_click_tool;
}

/* ====================================================================== */
/* the fixture                                                            */
/* ====================================================================== */

#define SW 1024
#define SH 768
#define GROWS (SH / 16)      /* 48 */
#define GCOLS (SW / 8)       /* 128 */

static fb_color_t   *pixels;
static fb_surface_t  screen;
static uint16_t      cells[GROWS * GCOLS];

#define BLANK_CELL 0x0720u   /* attr 0x07 (light grey on black), ' ' */

static void fixture(void) {
    if (!pixels) pixels = malloc(sizeof(fb_color_t) * SW * SH);
    gui_close_all();
    gui_detach();
    for (int i = 0; i < GROWS * GCOLS; i++) cells[i] = BLANK_CELL;
    for (int i = 0; i < SW * SH; i++) pixels[i] = 0xDEADBEEF;  /* poison */
    CHECK_EQ(fb_surface_init(&screen, pixels, SW, SH, SW), 0);
    gui_test_set_console_cursor(0, 0);
    CHECK_EQ(gui_attach(&screen, cells, GROWS, GCOLS), 0);
    gui_pointer_warp(SW - 4, SH - 4);
    kcap_reset();
}

static fb_color_t px(int32_t x, int32_t y) { return fb_get_pixel(&screen, x, y); }

/* Write a string into the console grid, as lib/base.c would. */
static void put_cells(int row, int col, const char *s, uint8_t attr) {
    for (int i = 0; s[i] && col + i < GCOLS; i++)
        cells[row * GCOLS + col + i] =
            (uint16_t)(((uint16_t)attr << 8) | (uint8_t)s[i]);
}

/* A CONTROL with this label. Buttons only, deliberately: a calculator's display
 * also reads "7" once 7 has been pressed, and a helper that matched it would
 * silently stop pressing keys — which is exactly the bug this restriction found
 * in the first version of tools/gui_tools.c. */
static gui_widget_t *by_label(gui_window_t *w, const char *s) {
    for (uint16_t i = 0; i < w->nwidgets; i++)
        if (w->widgets[i].kind == GUI_BUTTON &&
            strcmp(w->widgets[i].text, s) == 0) return &w->widgets[i];
    return NULL;
}

/* Click the centre of a widget in SCREEN coordinates, i.e. through the same
 * window-hit -> widget-hit -> dispatch path a real mouse click takes. */
static int click_label(gui_window_t *w, const char *s) {
    gui_widget_t *x = by_label(w, s);
    CHECK(x != NULL);
    if (!x) return 0;
    gui_rect_t c = gui_client_rect(w);
    return gui_click_at(c.x + x->r.x + x->r.w / 2, c.y + x->r.y + x->r.h / 2);
}

/* A synthetic hardware mouse event, exactly as drivers/input/mouse.c produces. */
static void m_move(int32_t x, int32_t y, uint8_t buttons) {
    mouse_event_t e;
    memset(&e, 0, sizeof e);
    int32_t ox, oy;
    gui_pointer_position(&ox, &oy);
    e.x = x; e.y = y;
    e.dx = (int16_t)(x - ox);
    e.dy = (int16_t)(y - oy);
    e.buttons = buttons;
    gui_post_mouse(&e);
}

static void m_down(int32_t x, int32_t y) {
    mouse_event_t e;
    memset(&e, 0, sizeof e);
    e.x = x; e.y = y;
    e.buttons = MOUSE_BTN_LEFT;
    e.pressed = MOUSE_BTN_LEFT;
    gui_post_mouse(&e);
}

static void m_up(int32_t x, int32_t y) {
    mouse_event_t e;
    memset(&e, 0, sizeof e);
    e.x = x; e.y = y;
    e.released = MOUSE_BTN_LEFT;
    gui_post_mouse(&e);
}

/* ====================================================================== */
/* 1. rectangle algebra                                                   */
/* ====================================================================== */

static void test_rect_edges_are_half_open(void) {
    gui_rect_t r = gui_rect(10, 20, 30, 40);

    CHECK(gui_rect_contains(r, 10, 20));            /* top-left is inside  */
    CHECK(gui_rect_contains(r, 39, 59));            /* bottom-right is too */
    CHECK(!gui_rect_contains(r, 9, 20));
    CHECK(!gui_rect_contains(r, 10, 19));
    CHECK(!gui_rect_contains(r, 40, 59));           /* right edge: outside */
    CHECK(!gui_rect_contains(r, 39, 60));           /* bottom edge: outside */

    CHECK(!gui_rect_contains(gui_rect(0, 0, 0, 10), 0, 0));
    CHECK(!gui_rect_contains(gui_rect(0, 0, 10, -1), 0, 0));

    /* Two adjacent rectangles partition their shared boundary: exactly one of
     * them contains it. Every hit test in the GUI inherits this. */
    gui_rect_t a = gui_rect(0, 0, 10, 10), b = gui_rect(10, 0, 10, 10);
    for (int x = 0; x < 20; x++)
        CHECK_EQ(gui_rect_contains(a, x, 5) + gui_rect_contains(b, x, 5), 1);
}

static void test_rect_intersect_union_inset(void) {
    gui_rect_t a = gui_rect(0, 0, 100, 100), b = gui_rect(50, 50, 100, 100);
    gui_rect_t i = gui_rect_intersect(a, b);
    CHECK_EQ(i.x, 50); CHECK_EQ(i.y, 50); CHECK_EQ(i.w, 50); CHECK_EQ(i.h, 50);
    CHECK(gui_rect_intersects(a, b));

    /* Touching edges do NOT intersect: (0,0,10,10) and (10,0,10,10) share a
     * boundary that neither owns as area. */
    CHECK(!gui_rect_intersects(gui_rect(0, 0, 10, 10), gui_rect(10, 0, 10, 10)));
    CHECK(gui_rect_empty(gui_rect_intersect(gui_rect(0, 0, 10, 10),
                                           gui_rect(10, 0, 10, 10))));

    gui_rect_t u = gui_rect_union(a, b);
    CHECK_EQ(u.x, 0); CHECK_EQ(u.y, 0); CHECK_EQ(u.w, 150); CHECK_EQ(u.h, 150);
    /* An empty operand is ignored rather than dragging the union to the origin. */
    u = gui_rect_union(gui_rect(0, 0, 0, 0), b);
    CHECK_EQ(u.x, 50); CHECK_EQ(u.w, 100);

    gui_rect_t in = gui_inset(gui_rect(10, 10, 20, 20), 3, 4);
    CHECK_EQ(in.x, 13); CHECK_EQ(in.y, 14);
    CHECK_EQ(in.w, 14); CHECK_EQ(in.h, 12);
    CHECK(gui_rect_empty(gui_inset(gui_rect(0, 0, 10, 10), 5, 0)));  /* w -> 0 */
    /* A negative inset grows, which is how a focus ring is drawn. */
    in = gui_inset(gui_rect(10, 10, 10, 10), -2, -2);
    CHECK_EQ(in.x, 8); CHECK_EQ(in.w, 14);
}

/* ====================================================================== */
/* 2. layout                                                              */
/* ====================================================================== */

/* The invariant that makes a keypad look like a keypad. Run over sizes that do
 * not divide by the column count, which is where naive (w/cols) fails. */
static void test_grid_tiles_exactly(void) {
    static const int widths[]  = { 40, 41, 42, 43, 100, 101, 102, 103, 199, 200,
                                   201, 202, 203, 640, 767, 999, 1000, 1023 };
    static const int gaps[]     = { 0, 1, 2, 3, 4, 5, 6, 7, 11 };
    static const int counts[]   = { 1, 2, 3, 4, 5, 6, 7, 8, 12, 16 };

    for (unsigned wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
        for (unsigned gi = 0; gi < sizeof gaps / sizeof gaps[0]; gi++)
            for (unsigned ci = 0; ci < sizeof counts / sizeof counts[0]; ci++) {
                int W = widths[wi], G = gaps[gi], N = counts[ci];
                gui_rect_t area = gui_rect(7, 13, W, W);
                gui_rect_t prev = gui_rect(0, 0, 0, 0);
                /* Skip grids where a cell cannot even be one pixel wide: with
                 * less than N pixels to share, gui_grid() returns EMPTY, which
                 * is asserted separately below. */
                if (W - G * (N - 1) < N) continue;

                for (int c = 0; c < N; c++) {
                    gui_rect_t cell = gui_grid(area, N, N, 0, c, 1, 1, G);
                    CHECK(!gui_rect_empty(cell));
                    if (c == 0) CHECK_EQ(cell.x, area.x);
                    else        CHECK_EQ(prev.x + prev.w + G, cell.x);
                    if (c == N - 1) CHECK_EQ(cell.x + cell.w, area.x + area.w);
                    /* No cell may be wider than another by more than 1px. */
                    if (c > 0) {
                        int d = cell.w - prev.w;
                        CHECK(d >= -1 && d <= 1);
                    }
                    prev = cell;
                }
            }
}

static void test_grid_spans_and_bad_input(void) {
    gui_rect_t area = gui_rect(0, 0, 202, 194);

    /* A colspan is exactly the union of the cells it covers plus their gaps. */
    gui_rect_t c0 = gui_grid(area, 5, 4, 4, 0, 1, 1, 5);
    gui_rect_t c3 = gui_grid(area, 5, 4, 4, 3, 1, 1, 5);
    gui_rect_t sp = gui_grid(area, 5, 4, 4, 0, 1, 4, 5);
    CHECK_EQ(sp.x, c0.x);
    CHECK_EQ(sp.x + sp.w, c3.x + c3.w);
    CHECK_EQ(sp.h, c0.h);

    gui_rect_t r2 = gui_grid(area, 5, 4, 0, 0, 2, 1, 5);
    gui_rect_t r1 = gui_grid(area, 5, 4, 1, 0, 1, 1, 5);
    CHECK_EQ(r2.y + r2.h, r1.y + r1.h);

    /* Everything nonsensical is empty, not "nearly right". */
    CHECK(gui_rect_empty(gui_grid(area, 0, 4, 0, 0, 1, 1, 0)));
    CHECK(gui_rect_empty(gui_grid(area, 5, 4, 5, 0, 1, 1, 0)));
    CHECK(gui_rect_empty(gui_grid(area, 5, 4, 0, 4, 1, 1, 0)));
    CHECK(gui_rect_empty(gui_grid(area, 5, 4, 0, 3, 1, 2, 0)));   /* span off  */
    CHECK(gui_rect_empty(gui_grid(area, 5, 4, 0, 0, 0, 1, 0)));   /* span 0    */
    CHECK(gui_rect_empty(gui_grid(area, 5, 4, 0, 0, 1, 1, 300))); /* gap > area */
    /* Too little room to give every cell a pixel: empty, not a zero-width
     * rectangle that a widget would then be laid out into. */
    CHECK(gui_rect_empty(gui_grid(gui_rect(0, 0, 3, 100), 1, 8, 0, 0, 1, 1, 0)));
    CHECK(gui_rect_empty(gui_grid(gui_rect(0, 0, 11, 100), 1, 16, 0, 3, 1, 1, 2)));
    CHECK(gui_rect_empty(gui_grid(area, 5, 4, -1, 0, 1, 1, 0)));
    CHECK(gui_rect_empty(gui_grid(gui_rect(0, 0, 0, 0), 2, 2, 0, 0, 1, 1, 0)));
}

static void test_splits(void) {
    gui_rect_t area = gui_rect(10, 20, 200, 100), rest;

    gui_rect_t top = gui_split_top(area, 30, 6, &rest);
    CHECK_EQ(top.y, 20); CHECK_EQ(top.h, 30); CHECK_EQ(top.w, 200);
    CHECK_EQ(rest.y, 56); CHECK_EQ(rest.h, 64);
    CHECK_EQ(top.h + 6 + rest.h, area.h);

    gui_rect_t bot = gui_split_bottom(area, 30, 6, &rest);
    CHECK_EQ(bot.y, 90); CHECK_EQ(bot.h, 30);
    CHECK_EQ(rest.y, 20); CHECK_EQ(rest.h, 64);

    gui_rect_t left = gui_split_left(area, 50, 4, &rest);
    CHECK_EQ(left.x, 10); CHECK_EQ(left.w, 50);
    CHECK_EQ(rest.x, 64); CHECK_EQ(rest.w, 146);

    gui_rect_t right = gui_split_right(area, 50, 4, &rest);
    CHECK_EQ(right.x, 160); CHECK_EQ(right.w, 50);
    CHECK_EQ(rest.x, 10);   CHECK_EQ(rest.w, 146);

    /* Asking for more than there is yields everything and an empty rest, so a
     * caller cannot end up laying out into a negative rectangle. */
    gui_rect_t all = gui_split_top(area, 500, 6, &rest);
    CHECK_EQ(all.h, 100);
    CHECK(gui_rect_empty(rest));
    gui_rect_t none = gui_split_top(area, 0, 6, &rest);
    CHECK(gui_rect_empty(none));
    CHECK_EQ(rest.h, 100);

    gui_rect_t c = gui_center(gui_rect(0, 0, 100, 100), 40, 20);
    CHECK_EQ(c.x, 30); CHECK_EQ(c.y, 40); CHECK_EQ(c.w, 40);
}

/* ====================================================================== */
/* 3. windows: open, clamp, ids                                            */
/* ====================================================================== */

static void test_window_open_and_clamp(void) {
    fixture();
    CHECK(gui_attached());
    int32_t w, h;
    gui_screen_size(&w, &h);
    CHECK_EQ(w, SW); CHECK_EQ(h, SH);
    CHECK_EQ(gui_active(), 0);

    uint32_t a = gui_window_open("First", 100, 60, 200, 150, 0);
    CHECK(a != 0);
    CHECK_EQ(gui_active(), 1);
    CHECK_EQ(gui_window_count(), 1);
    CHECK_EQ(gui_focused(), a);

    gui_window_t *W = gui_window(a);
    CHECK(W != NULL);
    CHECK_EQ(W->frame.x, 100); CHECK_EQ(W->frame.w, 200);
    CHECK_STR(W->title, "First");

    /* The client area is inside the border and below the title bar. */
    gui_rect_t cl = gui_client_rect(W);
    CHECK_EQ(cl.x, 100 + GUI_BORDER);
    CHECK_EQ(cl.y, 60 + GUI_BORDER + GUI_TITLE_H);
    CHECK_EQ(cl.w, 200 - 2 * GUI_BORDER);
    CHECK_EQ(cl.h, 150 - 2 * GUI_BORDER - GUI_TITLE_H);
    CHECK_EQ(gui_client_area(W).x, 0);
    CHECK_EQ(gui_client_area(W).w, cl.w);

    /* Absurd geometry is clamped, never refused: a model will send 1x1 and
     * 9999x9999 eventually, and a window nobody can grab is unusable. */
    uint32_t tiny = gui_window_open("t", 0, 0, 1, 1, 0);
    CHECK_EQ(gui_window(tiny)->frame.w, GUI_MIN_W);
    CHECK_EQ(gui_window(tiny)->frame.h, GUI_MIN_H);

    uint32_t huge = gui_window_open("h", -5000, -5000, 99999, 99999, 0);
    gui_window_t *H = gui_window(huge);
    CHECK_EQ(H->frame.w, SW);
    CHECK_EQ(H->frame.h, SH - 16);              /* the prompt row is reserved */
    CHECK(H->frame.y >= 0);
    CHECK(H->frame.x + H->frame.w >= 48);       /* still grabbable */

    uint32_t off = gui_window_open("o", 5000, 5000, 100, 100, 0);
    gui_window_t *O = gui_window(off);
    CHECK(O->frame.x <= SW - 48);
    CHECK(O->frame.y <= SH - (GUI_TITLE_H + 2 * GUI_BORDER));

    /* DECISION 1: THE BOTTOM TEXT ROW IS NEVER COVERED, whatever a document
     * asks for. The console scrolls, so the prompt lives on the last row for
     * the life of the machine; a window over it hides `> ` and, worse, hides
     * the "[typing into ...]" warning that says the operator's next sentence is
     * going into a text field rather than to the model. Both a full-screen
     * window and a window merely placed low must clear it. */
    for (int k = 0; k < gui_window_count(); k++) {
        gui_window_t *W2 = gui_window_at_z(k);
        CHECK(W2->frame.y + W2->frame.h <= SH - 16);
    }
    uint32_t low = gui_window_open("low", 0, SH - 40, 300, 200, 0);
    gui_window_t *L = gui_window(low);
    CHECK(L->frame.y + L->frame.h <= SH - 16);
    CHECK_EQ(L->frame.h, 200);                  /* slid up, not shortened */
    CHECK_EQ(gui_window_at(10, SH - 1), 0u);    /* nothing owns the last row */
    CHECK(gui_window_close(low) == 0);

    /* Ids are monotonic and never reused, so a stale id from an earlier turn
     * resolves to "no such window" instead of to whatever moved in. */
    CHECK(gui_window_close(a) == 0);
    CHECK(gui_window(a) == NULL);
    uint32_t next = gui_window_open("n", 10, 10, 100, 100, 0);
    CHECK(next > a);
    CHECK(gui_window(a) == NULL);
    CHECK_EQ(gui_window_close(99999), -1);
}

static void test_title_is_sanitised(void) {
    fixture();
    uint32_t id = gui_window_open("a\nb\tc\x01", 10, 10, 200, 100, 0);
    CHECK_STR(gui_window(id)->title, "a?b?c?");

    id = gui_window_open(NULL, 10, 10, 200, 100, 0);
    CHECK_STR(gui_window(id)->title, "?");

    char long_title[300];
    memset(long_title, 'x', sizeof long_title - 1);
    long_title[sizeof long_title - 1] = '\0';
    id = gui_window_open(long_title, 10, 10, 200, 100, 0);
    CHECK_EQ((int)strlen(gui_window(id)->title), GUI_TITLE_MAX - 1);
}

static void test_pool_exhaustion(void) {
    fixture();
    uint32_t ids[GUI_MAX_WINDOWS];
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        ids[i] = gui_window_open("w", 10, 10, 100, 80, 0);
        CHECK(ids[i] != 0);
    }
    CHECK_EQ(gui_window_open("over", 10, 10, 100, 80, 0), 0u);
    CHECK_EQ(gui_window_count(), GUI_MAX_WINDOWS);
    CHECK_EQ(gui_window_close(ids[3]), 0);
    CHECK(gui_window_open("again", 10, 10, 100, 80, 0) != 0);
    gui_close_all();
    CHECK_EQ(gui_window_count(), 0);
    CHECK_EQ(gui_active(), 0);
}

/* ====================================================================== */
/* 4. z-order and focus                                                    */
/* ====================================================================== */

static void test_zorder(void) {
    fixture();
    uint32_t a = gui_window_open("A", 100, 100, 200, 200, 0);
    uint32_t b = gui_window_open("B", 150, 150, 200, 200, 0);
    uint32_t c = gui_window_open("C", 200, 200, 200, 200, 0);

    /* Bottom to top in creation order. */
    CHECK_EQ(gui_window_at_z(0)->id, a);
    CHECK_EQ(gui_window_at_z(2)->id, c);
    CHECK(gui_window_at_z(3) == NULL);
    CHECK(gui_window_at_z(-1) == NULL);

    /* The topmost window wins a point all three contain. */
    CHECK_EQ(gui_window_at(250, 250), c);
    CHECK_EQ(gui_window_at(120, 120), a);       /* only A is there */
    CHECK_EQ(gui_window_at(160, 160), b);       /* A and B; B is above */
    CHECK_EQ(gui_window_at(5, 5), 0u);          /* the desktop */

    gui_window_raise(a);
    CHECK_EQ(gui_window_at_z(2)->id, a);
    CHECK_EQ(gui_window_at(250, 250), a);
    /* Raising does not change focus. */
    CHECK_EQ(gui_focused(), c);

    gui_window_close(a);
    CHECK_EQ(gui_window_count(), 2);
    CHECK_EQ(gui_window_at(250, 250), c);

    /* Hidden windows are not hit and do not keep the GUI active. */
    gui_window_show(b, 0);
    gui_window_show(c, 0);
    CHECK_EQ(gui_window_at(250, 250), 0u);
    CHECK_EQ(gui_active(), 0);
    gui_window_show(c, 1);
    CHECK_EQ(gui_window_at(250, 250), c);
}

static int hook_log_n;
static char hook_log[32][32];
static int  logging_hook(gui_window_t *w, gui_widget_t *wd, const gui_event_t *e) {
    (void)wd;
    if (hook_log_n < 32)
        snprintf(hook_log[hook_log_n++], 32, "%u:%d", (unsigned)w->id, (int)e->kind);
    return 0;
}

static void test_focus_transitions(void) {
    fixture();
    uint32_t a = gui_window_open("A", 100, 100, 200, 200, 0);
    uint32_t b = gui_window_open("B", 400, 100, 200, 200, 0);
    gui_set_hook(a, logging_hook);
    gui_set_hook(b, logging_hook);
    CHECK_EQ(gui_focused(), b);

    hook_log_n = 0;
    gui_window_focus(a);
    CHECK_EQ(gui_focused(), a);
    /* Blur the old one BEFORE focusing the new one: an app saving state on blur
     * must not see the new focus first. */
    CHECK(hook_log_n >= 2);
    char want_blur[32], want_focus[32];
    snprintf(want_blur, sizeof want_blur, "%u:%d", (unsigned)b, GUI_EV_BLUR);
    snprintf(want_focus, sizeof want_focus, "%u:%d", (unsigned)a, GUI_EV_FOCUS);
    CHECK_STR(hook_log[0], want_blur);
    CHECK_STR(hook_log[1], want_focus);

    /* Focusing the already-focused window is a no-op, not a blur/focus pair. */
    hook_log_n = 0;
    gui_window_focus(a);
    CHECK_EQ(hook_log_n, 0);

    /* Clicking the desktop drops focus entirely. */
    gui_click_at(5, 5);
    CHECK_EQ(gui_focused(), 0u);

    /* Closing the focused window hands focus to the topmost survivor rather
     * than leaving windows on screen with none of them focused. */
    gui_window_focus(a);
    gui_window_close(a);
    CHECK_EQ(gui_focused(), b);

    CHECK_EQ(gui_window_focus(4242), -1);
}

/* ====================================================================== */
/* 5. damage                                                              */
/* ====================================================================== */

/* Is `inner` covered by the union of the damage list? Damage may be merged or
 * collapsed, so the only assertable property is that it is a SUPERSET. */
static int damage_covers(gui_rect_t inner) {
    for (int y = inner.y; y < inner.y + inner.h; y += 4)
        for (int x = inner.x; x < inner.x + inner.w; x += 4) {
            int in = 0;
            for (int i = 0; i < gui_damage_count() && !in; i++)
                in = gui_rect_contains(gui_damage_rect(i), x, y);
            if (!in) return 0;
        }
    return 1;
}

static void test_damage_is_a_superset(void) {
    fixture();
    uint32_t a = gui_window_open("A", 100, 100, 200, 150, 0);
    gui_pointer_show(0);
    CHECK(gui_damage_count() > 0);
    CHECK(damage_covers(gui_window(a)->frame));

    gui_flush();
    CHECK_EQ(gui_damage_count(), 0);

    /* Moving damages where it was AND where it is. */
    gui_rect_t old = gui_window(a)->frame;
    gui_window_move(a, 400, 300);
    gui_rect_t nw = gui_window(a)->frame;
    CHECK(damage_covers(old));
    CHECK(damage_covers(nw));
    gui_flush();

    /* A widget invalidation is the widget, not the window. */
    gui_window_t *W = gui_window(a);
    gui_widget_t *b = gui_add_button(W, gui_rect(10, 10, 40, 20), "x", 1);
    gui_flush();
    gui_invalidate_widget(a, b);
    CHECK_EQ(gui_damage_count(), 1);
    gui_rect_t d = gui_damage_rect(0);
    CHECK(d.w <= 40 && d.h <= 20);
    gui_rect_t cl = gui_client_rect(W);
    CHECK_EQ(d.x, cl.x + 10);
    gui_flush();

    /* Overflowing the list collapses it to a bounding box and keeps going. */
    for (int i = 0; i < GUI_MAX_DAMAGE * 3; i++)
        gui_invalidate(i * 30, i * 20, 8, 8);
    CHECK(gui_damage_count() <= GUI_MAX_DAMAGE);
    CHECK(gui_damage_count() >= 1);
    gui_flush();

    /* Damage is clipped to the screen: nothing off-screen can be presented. */
    gui_invalidate(-500, -500, 100, 100);
    CHECK_EQ(gui_damage_count(), 0);
    gui_invalidate(SW - 10, SH - 10, 100, 100);
    CHECK_EQ(gui_damage_count(), 1);
    CHECK_EQ(gui_damage_rect(0).w, 10);
    CHECK_EQ(gui_damage_rect(0).h, 10);
    gui_flush();

    gui_invalidate_all();
    CHECK_EQ(gui_damage_count(), 1);
    CHECK_EQ(gui_damage_rect(0).w, SW);
}

/* ====================================================================== */
/* 6. exact pixels                                                        */
/* ====================================================================== */

static void test_pixels_frame_and_chrome(void) {
    fixture();
    const gui_theme_t *t = gui_theme();
    uint32_t id = gui_window_open("Hello", 100, 200, 200, 150, 0);
    gui_pointer_show(0);
    gui_flush();

    gui_window_t *W = gui_window(id);
    gui_rect_t f = W->frame;

    /* Border: the focused colour, one pixel thick, on all four sides. */
    CHECK_EQ(px(f.x, f.y), t->frame_focus);
    CHECK_EQ(px(f.x + f.w - 1, f.y), t->frame_focus);
    CHECK_EQ(px(f.x, f.y + f.h - 1), t->frame_focus);
    CHECK_EQ(px(f.x + f.w - 1, f.y + f.h - 1), t->frame_focus);

    /* Title bar, inside the border. */
    CHECK_EQ(px(f.x + 1, f.y + 1), t->title_bg_focus);
    CHECK_EQ(px(f.x + 1, f.y + GUI_TITLE_H), t->title_bg_focus);

    /* Client fill, below the title bar. */
    gui_rect_t cl = gui_client_rect(W);
    CHECK_EQ(px(cl.x + 5, cl.y + 5), t->client);
    CHECK_EQ(px(cl.x + cl.w - 1, cl.y + cl.h - 1), t->client);

    /* The title text is drawn: some pixel in the first glyph cell is neither
     * the bar colour nor the border. */
    int inked = 0;
    for (int y = f.y + 1; y < f.y + GUI_TITLE_H && !inked; y++)
        for (int x = f.x + 6; x < f.x + 6 + 8 * 5 && !inked; x++)
            if (px(x, y) == t->title_fg_focus) inked = 1;
    CHECK(inked);

    /* NOTHING outside the frame was touched: the poison is still there. */
    CHECK_EQ(px(f.x - 1, f.y), 0xDEADBEEFu);
    CHECK_EQ(px(f.x + f.w, f.y + 5), 0xDEADBEEFu);
    CHECK_EQ(px(f.x + 5, f.y - 1), 0xDEADBEEFu);
    CHECK_EQ(px(f.x + 5, f.y + f.h), 0xDEADBEEFu);

    /* Losing focus repaints the chrome in the unfocused colours. */
    uint32_t other = gui_window_open("B", 600, 600, 100, 80, 0);
    gui_pointer_show(0);
    gui_flush();
    CHECK_EQ(gui_focused(), other);
    CHECK_EQ(px(f.x, f.y), t->frame);
    CHECK_EQ(px(f.x + 1, f.y + 1), t->title_bg);
}

static void test_pixels_backdrop_is_the_console(void) {
    fixture();
    put_cells(2, 4, "TALK", 0x07);
    put_cells(3, 0, "OS", 0x1F);          /* white on blue */
    gui_invalidate_all();
    gui_flush();

    /* Row 2 col 4 is the pixel block (32,32)-(40,48). The glyph must have put
     * ink there in the foreground colour, and its background is palette 0. */
    int ink = 0;
    for (int y = 32; y < 48; y++)
        for (int x = 32; x < 40; x++)
            if (px(x, y) == fb_vga_palette[7]) ink = 1;
    CHECK(ink);
    CHECK_EQ(px(31, 32), fb_vga_palette[0]);     /* the blank cell beside it */

    /* Attribute decoding: cell (3,0) is white on blue. */
    int fg = 0, bg = 0;
    for (int y = 48; y < 64; y++)
        for (int x = 0; x < 8; x++) {
            if (px(x, y) == fb_vga_palette[15]) fg = 1;
            if (px(x, y) == fb_vga_palette[1])  bg = 1;
        }
    CHECK(fg); CHECK(bg);

    /* A window drawn over that text hides it; moving away restores it. */
    uint32_t id = gui_window_open("W", 0, 0, 200, 120, 0);
    gui_pointer_show(0);
    gui_flush();
    CHECK_EQ(px(34, 36), gui_theme()->client);
    gui_window_move(id, 500, 400);
    gui_flush();
    ink = 0;
    for (int y = 32; y < 48; y++)
        for (int x = 32; x < 40; x++)
            if (px(x, y) == fb_vga_palette[7]) ink = 1;
    CHECK(ink);
}

static void test_pixels_zorder_overlap(void) {
    fixture();
    const gui_theme_t *t = gui_theme();
    uint32_t a = gui_window_open("A", 100, 100, 200, 200, 0);
    uint32_t b = gui_window_open("B", 200, 200, 200, 200, 0);
    gui_pointer_show(0);
    /* Give A a distinctive client colour by painting a panel over it. */
    gui_add_panel(gui_window(a), gui_client_area(gui_window(a)),
                  FB_RGB(0xff, 0, 0), 0);
    gui_add_panel(gui_window(b), gui_client_area(gui_window(b)),
                  FB_RGB(0, 0xff, 0), 0);
    gui_invalidate_all();
    gui_flush();

    /* In the overlap, B (on top) wins. */
    CHECK_EQ(px(250, 250), FB_RGB(0, 0xff, 0));
    /* Outside the overlap each keeps its own. */
    CHECK_EQ(px(150, 150), FB_RGB(0xff, 0, 0));
    CHECK_EQ(px(380, 380), FB_RGB(0, 0xff, 0));

    gui_window_raise(a);
    gui_flush();
    CHECK_EQ(px(250, 250), FB_RGB(0xff, 0, 0));
    CHECK_EQ(px(380, 380), FB_RGB(0, 0xff, 0));

    /* A widget laid out past the edge of its window is clipped to the client
     * area, not drawn over the neighbour. */
    gui_add_panel(gui_window(a), gui_rect(0, 0, 5000, 5000),
                  FB_RGB(0, 0, 0xff), 0);
    gui_invalidate_all();
    gui_flush();
    gui_rect_t fa = gui_window(a)->frame;
    /* A is raised but NOT focused (raising does not steal focus), so its border
     * is the unfocused colour — and the panel did not reach it. */
    CHECK_EQ(px(fa.x + fa.w - 1, fa.y + fa.h - 1), t->frame);
    CHECK_EQ(px(fa.x + fa.w + 2, fa.y + fa.h - 4), FB_RGB(0, 0xff, 0));
}

/* ====================================================================== */
/* 7. hit testing and dispatch                                            */
/* ====================================================================== */

static int   ev_count;
static int   ev_kinds[64];
static uint32_t ev_ids[64];
static int   widget_hook_calls;

static int recording_hook(gui_window_t *w, gui_widget_t *wd, const gui_event_t *e) {
    (void)w;
    if (ev_count < 64) {
        ev_kinds[ev_count] = e->kind;
        ev_ids[ev_count]   = wd ? wd->id : 0;
        ev_count++;
    }
    return 0;
}

static int swallowing_widget_hook(gui_window_t *w, gui_widget_t *wd,
                                 const gui_event_t *e) {
    (void)w; (void)wd;
    if (e->kind == GUI_EV_CLICK) { widget_hook_calls++; return 1; }
    return 0;
}

static int clicks_of(uint32_t id) {
    int n = 0;
    for (int i = 0; i < ev_count; i++)
        if (ev_kinds[i] == GUI_EV_CLICK && ev_ids[i] == id) n++;
    return n;
}

static void test_hit_test_boundaries(void) {
    fixture();
    uint32_t id = gui_window_open("W", 100, 100, 300, 200, 0);
    gui_window_t *W = gui_window(id);
    gui_widget_t *b = gui_add_button(W, gui_rect(20, 30, 40, 25), "btn", 7);
    gui_add_panel(W, gui_rect(0, 0, 300, 200), gui_theme()->client, 0);
    gui_widget_t *lab = gui_add_label(W, gui_rect(100, 100, 50, 16), "l",
                                     GUI_ALIGN_LEFT);

    /* All four boundaries, to the pixel. */
    CHECK(gui_widget_at(W, 20, 30) == b);
    CHECK(gui_widget_at(W, 59, 54) == b);
    CHECK(gui_widget_at(W, 19, 30) == NULL);
    CHECK(gui_widget_at(W, 20, 29) == NULL);
    CHECK(gui_widget_at(W, 60, 40) == NULL);
    CHECK(gui_widget_at(W, 40, 55) == NULL);

    /* The panel added AFTER the button covers it in paint order, but panels are
     * never hit-testable, so the button still wins. That is the whole reason
     * GUI_W_NOHIT exists. */
    CHECK(gui_widget_at(W, 30, 40) == b);
    CHECK(gui_widget_at(W, 200, 150) == NULL);
    CHECK(gui_widget_at(W, 110, 105) == NULL);         /* labels: not hit */
    CHECK((lab->state & GUI_W_NOHIT) != 0);

    /* A disabled widget is not hit at all. */
    gui_enable(b, 0);
    CHECK(gui_widget_at(W, 30, 40) == NULL);
    gui_enable(b, 1);
    CHECK(gui_widget_at(W, 30, 40) == b);

    /* Two adjacent buttons partition their shared edge. */
    gui_widget_t *b2 = gui_add_button(W, gui_rect(60, 30, 40, 25), "b2", 8);
    CHECK(gui_widget_at(W, 59, 40) == b);
    CHECK(gui_widget_at(W, 60, 40) == b2);

    CHECK_EQ(gui_widget_index(W, b), 0);
    CHECK_EQ(gui_widget_index(W, NULL), -1);
    CHECK(gui_widget_by_id(W, 7) == b);
    CHECK(gui_widget_by_id(W, 999) == NULL);
    CHECK(gui_widget_by_id(W, 0) == NULL);           /* 0 is "anonymous" */
}

static void test_dispatch_to_the_right_widget(void) {
    fixture();
    uint32_t id = gui_window_open("W", 100, 100, 300, 200, 0);
    gui_window_t *W = gui_window(id);
    gui_add_button(W, gui_rect(10, 10, 60, 30), "one", 101);
    gui_add_button(W, gui_rect(80, 10, 60, 30), "two", 102);
    gui_set_hook(id, recording_hook);
    gui_rect_t cl = gui_client_rect(W);
    ev_count = 0;

    /* A press-then-release inside "two" activates exactly "two". */
    CHECK_EQ(click_label(W, "two"), 1);
    CHECK_EQ(clicks_of(102), 1);
    CHECK_EQ(clicks_of(101), 0);
    /* ...and the ordering is DOWN, UP, CLICK. */
    CHECK_EQ(ev_kinds[ev_count - 3], GUI_EV_MOUSE_DOWN);
    CHECK_EQ(ev_kinds[ev_count - 2], GUI_EV_MOUSE_UP);
    CHECK_EQ(ev_kinds[ev_count - 1], GUI_EV_CLICK);

    /* Coordinates handed to a widget hook are relative to THE WIDGET. */
    ev_count = 0;
    static int32_t seen_x, seen_y;
    seen_x = seen_y = -1;
    gui_widget_t *one = gui_widget_by_id(W, 101);
    one->on_event = NULL;
    gui_set_hook(id, NULL);
    struct {
        int dummy;
    } unused;
    (void)unused;
    gui_set_hook(id, recording_hook);
    gui_click_at(cl.x + 10 + 3, cl.y + 10 + 4);
    CHECK_EQ(clicks_of(101), 1);

    /* Press inside, release OUTSIDE: no activation, and the pressed look is
     * cleared. Dragging off a key must cancel it. */
    ev_count = 0;
    gui_widget_t *two = gui_widget_by_id(W, 102);
    m_down(cl.x + 80 + 5, cl.y + 10 + 5);
    CHECK((two->state & GUI_W_PRESSED) != 0);
    m_up(cl.x + 200, cl.y + 100);
    CHECK((two->state & GUI_W_PRESSED) == 0);
    CHECK_EQ(clicks_of(102), 0);
    CHECK(ev_count > 0);
    CHECK_EQ(ev_kinds[ev_count - 1], GUI_EV_MOUSE_UP);

    /* A click on empty client area reaches the window hook with no widget. */
    ev_count = 0;
    gui_click_at(cl.x + 200, cl.y + 150);
    CHECK(ev_count >= 1);
    CHECK_EQ(ev_ids[0], 0u);

    /* A widget's own hook is tried first and can swallow the event. */
    widget_hook_calls = 0;
    ev_count = 0;
    two->on_event = swallowing_widget_hook;
    click_label(W, "two");
    CHECK_EQ(widget_hook_calls, 1);
    CHECK_EQ(clicks_of(102), 0);          /* the window hook never saw it */
    two->on_event = NULL;

    /* A click on the desktop is not consumed by anything. */
    CHECK_EQ(gui_click_at(5, SH - 5), 0);

    (void)seen_x; (void)seen_y;
}

static void test_click_raises_and_focuses(void) {
    fixture();
    uint32_t a = gui_window_open("A", 100, 100, 200, 200, 0);
    uint32_t b = gui_window_open("B", 150, 150, 200, 200, 0);
    CHECK_EQ(gui_focused(), b);
    /* Clicking a window that is behind raises AND focuses it in one action. */
    gui_click_at(110, 250);
    CHECK_EQ(gui_focused(), a);
    CHECK_EQ(gui_window_at_z(1)->id, a);
}

static void test_drag_to_move(void) {
    fixture();
    uint32_t id = gui_window_open("Drag", 100, 100, 200, 150, 0);
    gui_pointer_show(0);
    gui_flush();

    /* Grab the title bar 30px in, drag right and down 60,40. */
    m_move(130, 108, 0);
    m_down(130, 108);
    m_move(190, 148, MOUSE_BTN_LEFT);
    gui_window_t *W = gui_window(id);
    CHECK_EQ(W->frame.x, 160);
    CHECK_EQ(W->frame.y, 140);
    CHECK(damage_covers(gui_rect(100, 100, 200, 150)));   /* where it was  */
    CHECK(damage_covers(W->frame));                       /* where it is   */

    /* Releasing ends the drag: further motion does not move the window. */
    m_up(190, 148);
    m_move(400, 400, 0);
    CHECK_EQ(gui_window(id)->frame.x, 160);

    /* Losing the button without an explicit release also ends it (a release
     * consumed by something else must not leave the window glued to the
     * pointer). */
    m_down(190, 148);
    m_move(200, 158, MOUSE_BTN_LEFT);
    CHECK_EQ(gui_window(id)->frame.x, 170);
    m_move(300, 300, 0);                     /* button no longer held */
    CHECK_EQ(gui_window(id)->frame.x, 170);

    /* GUI_WIN_NOMOVE refuses to drag but still focuses. */
    uint32_t fixedw = gui_window_open("Fixed", 500, 400, 150, 100,
                                      GUI_WIN_NOMOVE);
    m_down(540, 408);
    m_move(600, 500, MOUSE_BTN_LEFT);
    CHECK_EQ(gui_window(fixedw)->frame.x, 500);
    m_up(600, 500);
}

static void test_close_box(void) {
    fixture();
    uint32_t id = gui_window_open("Close me", 100, 100, 200, 150, 0);
    gui_window_t *W = gui_window(id);
    /* The close box is at the right end of the title bar. */
    int32_t cx = W->frame.x + W->frame.w - 1 - 3 - GUI_CLOSE_W / 2;
    int32_t cy = W->frame.y + GUI_BORDER + GUI_TITLE_H / 2;

    /* Press in the box and release OUTSIDE it: the window survives. */
    m_down(cx, cy);
    m_up(W->frame.x + 20, cy);
    CHECK(gui_window(id) != NULL);

    m_down(cx, cy);
    m_up(cx, cy);
    CHECK(gui_window(id) == NULL);
    CHECK_EQ(gui_window_count(), 0);

    /* GUI_WIN_NOCLOSE has no box, so a click there drags instead. */
    uint32_t k = gui_window_open("Kept", 100, 100, 200, 150, GUI_WIN_NOCLOSE);
    gui_window_t *K = gui_window(k);
    int32_t kx = K->frame.x + K->frame.w - 8;
    m_down(kx, cy);
    m_up(kx, cy);
    CHECK(gui_window(k) != NULL);
}

static int close_events;
static int refuse_close(gui_window_t *w, gui_widget_t *wd, const gui_event_t *e) {
    (void)w; (void)wd;
    if (e->kind != GUI_EV_CLOSE) return 0;
    close_events++;
    return 1;
}

static void test_app_can_refuse_to_close(void) {
    fixture();
    uint32_t id = gui_window_open("Sticky", 100, 100, 200, 150, 0);
    gui_set_hook(id, refuse_close);
    CHECK_EQ(gui_window_close(id), -2);
    CHECK(gui_window(id) != NULL);
    /* gui_close_all() is the override, because a machine must always be able to
     * clear its own screen — but the app is still TOLD, so it can release
     * whatever it allocated. Only the veto is ignored. */
    close_events = 0;
    gui_close_all();
    CHECK_EQ(gui_window_count(), 0);
    CHECK_EQ(close_events, 1);
}

/* ====================================================================== */
/* 8. the keyboard                                                        */
/* ====================================================================== */

static void test_one_line_keyboard_grab(void) {
    fixture();
    uint32_t id = gui_window_open("Form", 100, 100, 300, 120, 0);
    gui_window_t *W = gui_window(id);
    gui_widget_t *f = gui_add_field(W, gui_rect(10, 10, 200, 26), "", 1, 0);
    gui_widget_t *ro = gui_add_field(W, gui_rect(10, 50, 200, 26), "read only", 2,
                                    GUI_W_READONLY);
    gui_rect_t cl = gui_client_rect(W);

    /* Nothing is armed until a field is clicked. */
    CHECK_EQ(gui_wants_keys(), 0);
    CHECK_EQ(gui_take_line("hello", 5), 0);

    gui_click_at(cl.x + 20, cl.y + 20);
    CHECK_EQ(gui_wants_keys(), 1);
    CHECK((f->state & GUI_W_FOCUS) != 0);

    CHECK_EQ(gui_take_line("hi there", 8), 1);
    CHECK_STR(gui_text(f), "hi there");
    /* The grab lasted exactly one line: the prompt cannot be captured. */
    CHECK_EQ(gui_wants_keys(), 0);
    CHECK_EQ(gui_take_line("more", 4), 0);
    CHECK_STR(gui_text(f), "hi there");

    /* A read-only field takes focus but never the keyboard. */
    gui_click_at(cl.x + 20, cl.y + 60);
    CHECK_EQ(gui_wants_keys(), 0);
    CHECK((ro->state & GUI_W_FOCUS) != 0);
    CHECK((f->state & GUI_W_FOCUS) == 0);

    /* Focusing another window drops an armed grab. */
    gui_click_at(cl.x + 20, cl.y + 20);
    CHECK_EQ(gui_wants_keys(), 1);
    gui_window_open("Other", 600, 400, 200, 100, 0);
    CHECK_EQ(gui_wants_keys(), 0);

    /* Closing the window with the grab drops it too. */
    gui_window_focus(id);
    gui_click_at(cl.x + 20, cl.y + 20);
    CHECK_EQ(gui_wants_keys(), 1);
    gui_window_close(id);
    CHECK_EQ(gui_wants_keys(), 0);
    CHECK_EQ(gui_take_line("x", 1), 0);
}

static void test_field_editing(void) {
    fixture();
    uint32_t id = gui_window_open("Form", 100, 100, 300, 120, 0);
    gui_window_t *W = gui_window(id);
    gui_widget_t *f = gui_add_field(W, gui_rect(10, 10, 200, 26), "ab", 1, 0);
    CHECK_EQ(f->caret, 2);

    CHECK_EQ(gui_field_key(f, 'c'), 1);
    CHECK_STR(gui_text(f), "abc");
    CHECK_EQ(gui_field_key(f, '\b'), 1);
    CHECK_STR(gui_text(f), "ab");
    CHECK_EQ(gui_field_key(f, 0x7F), 1);
    CHECK_STR(gui_text(f), "a");
    CHECK_EQ(gui_field_key(f, '\b'), 1);
    CHECK_EQ(gui_field_key(f, '\b'), 0);          /* empty: nothing to delete */
    CHECK_STR(gui_text(f), "");

    /* Control characters and non-ASCII are ignored, not inserted: a raw byte
     * >= 0x80 would be half a UTF-8 sequence. */
    CHECK_EQ(gui_field_key(f, '\n'), 0);
    CHECK_EQ(gui_field_key(f, '\t'), 0);
    CHECK_EQ(gui_field_key(f, 0xC3), 0);
    CHECK_STR(gui_text(f), "");

    /* The buffer is a hard bound. */
    for (int i = 0; i < GUI_TEXT_MAX * 2; i++) gui_field_key(f, 'x');
    CHECK_EQ((int)strlen(gui_text(f)), GUI_TEXT_MAX - 1);
    CHECK_EQ(gui_field_key(f, 'y'), 0);

    /* Caret in the middle inserts there. */
    gui_field_clear(f);
    gui_set_text(f, "acd");
    f->caret = 1;
    CHECK_EQ(gui_field_key(f, 'b'), 1);
    CHECK_STR(gui_text(f), "abcd");
    CHECK_EQ(f->caret, 2);

    /* Read-only and non-field widgets refuse. */
    gui_widget_t *b = gui_add_button(W, gui_rect(10, 50, 40, 20), "b", 2);
    CHECK_EQ(gui_field_key(b, 'x'), -1);
    gui_set_state(f, GUI_W_READONLY, 0);
    CHECK_EQ(gui_field_key(f, 'x'), -1);

    /* gui_set_text truncates instead of overflowing. */
    char big[GUI_TEXT_MAX * 3];
    memset(big, 'z', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    CHECK_EQ((int)gui_set_text(f, big), GUI_TEXT_MAX - 1);
    CHECK_EQ((int)strlen(gui_text(f)), GUI_TEXT_MAX - 1);
    CHECK_EQ((int)gui_set_text(f, NULL), 0);
    CHECK_STR(gui_text(f), "");
}

static void test_post_key_to_focused_field(void) {
    fixture();
    uint32_t id = gui_window_open("Form", 100, 100, 300, 120, 0);
    gui_window_t *W = gui_window(id);
    gui_widget_t *f = gui_add_field(W, gui_rect(10, 10, 200, 26), "", 1, 0);
    gui_rect_t cl = gui_client_rect(W);

    CHECK_EQ(gui_post_key('a'), 0);              /* nothing focused yet */
    gui_click_at(cl.x + 20, cl.y + 20);
    CHECK_EQ(gui_post_key('a'), 1);
    CHECK_EQ(gui_post_key('b'), 1);
    CHECK_STR(gui_text(f), "ab");
    CHECK_EQ(gui_post_key('\n'), 1);             /* submit, not a character */
    CHECK_STR(gui_text(f), "ab");
    CHECK(gui_stats()->keys >= 2);
}

/* ====================================================================== */
/* 9. THE DEMO: a calculator, driven by clicking pixels                    */
/* ====================================================================== */

static const char *calc_display(gui_window_t *W) {
    gui_widget_t *d = gui_widget_by_id(W, 1);      /* CID_DISPLAY */
    return d ? gui_text(d) : "(none)";
}

static void test_calculator_arithmetic_by_clicking(void) {
    fixture();
    uint32_t id = gui_demo_calculator(60, 80);
    CHECK(id != 0);
    gui_window_t *W = gui_window(id);
    CHECK_STR(W->title, "Calculator");
    CHECK_STR(calc_display(W), "0");

    /* Every key must exist and be laid out INSIDE the client area — a key that
     * is off the edge cannot be clicked however good the arithmetic is. */
    static const char *keys[] = { "0","1","2","3","4","5","6","7","8","9",
                                  ".","C","+","-","*","/","=" };
    gui_rect_t area = gui_client_area(W);
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        gui_widget_t *k = by_label(W, keys[i]);
        CHECK(k != NULL);
        if (!k) continue;
        CHECK(k->r.x >= 0 && k->r.y >= 0);
        CHECK(k->r.x + k->r.w <= area.w);
        CHECK(k->r.y + k->r.h <= area.h);
        CHECK(k->kind == GUI_BUTTON);
    }

    /* 7 + 5 = 12, every click a screen coordinate through the real dispatch. */
    click_label(W, "7");  CHECK_STR(calc_display(W), "7");
    click_label(W, "+");  CHECK_STR(calc_display(W), "7");
    click_label(W, "5");  CHECK_STR(calc_display(W), "5");
    click_label(W, "=");  CHECK_STR(calc_display(W), "12");

    /* Multi-digit entry, and a running total without pressing = each time. */
    click_label(W, "C");  CHECK_STR(calc_display(W), "0");
    click_label(W, "1"); click_label(W, "2"); click_label(W, "3");
    CHECK_STR(calc_display(W), "123");
    click_label(W, "*"); click_label(W, "2");
    click_label(W, "-"); CHECK_STR(calc_display(W), "246");
    click_label(W, "4"); click_label(W, "6");
    click_label(W, "="); CHECK_STR(calc_display(W), "200");

    /* Fixed point: six decimal places, trailing zeros trimmed. */
    click_label(W, "C");
    click_label(W, "1"); click_label(W, "/"); click_label(W, "3");
    click_label(W, "=");
    CHECK_STR(calc_display(W), "0.333333");

    click_label(W, "C");
    click_label(W, "1"); click_label(W, "."); click_label(W, "5");
    click_label(W, "*"); click_label(W, "4"); click_label(W, "=");
    CHECK_STR(calc_display(W), "6");

    /* A second '.' is ignored rather than producing "1..5". */
    click_label(W, "C");
    click_label(W, "1"); click_label(W, "."); click_label(W, ".");
    click_label(W, "5");
    CHECK_STR(calc_display(W), "1.5");

    /* Negative results. */
    click_label(W, "C");
    click_label(W, "3"); click_label(W, "-"); click_label(W, "8");
    click_label(W, "=");
    CHECK_STR(calc_display(W), "-5");

    /* Division by zero latches an error that only C clears. */
    click_label(W, "C");
    click_label(W, "9"); click_label(W, "/"); click_label(W, "0");
    click_label(W, "=");
    CHECK_STR(calc_display(W), "error");
    click_label(W, "5");
    CHECK_STR(calc_display(W), "error");
    click_label(W, "C");
    CHECK_STR(calc_display(W), "0");

    /* Overflow is an error too, not a wrapped number. */
    click_label(W, "C");
    for (int i = 0; i < 9; i++) click_label(W, "9");
    click_label(W, "*");
    for (int i = 0; i < 9; i++) click_label(W, "9");
    click_label(W, "=");
    CHECK_STR(calc_display(W), "error");

    /* The digit cap holds: the display never grows without bound. */
    click_label(W, "C");
    for (int i = 0; i < 40; i++) click_label(W, "9");
    CHECK_EQ((int)strlen(calc_display(W)), 12);

    /* A leading zero is replaced, not accumulated. */
    click_label(W, "C");
    click_label(W, "0"); click_label(W, "0"); click_label(W, "7");
    CHECK_STR(calc_display(W), "7");

    /* Closing the calculator releases its state slot, so it can be reopened
     * indefinitely. */
    for (int i = 0; i < GUI_MAX_WINDOWS * 3; i++) {
        uint32_t k = gui_demo_calculator(GUI_POS_AUTO, GUI_POS_AUTO);
        CHECK(k != 0);
        CHECK_EQ(gui_window_close(k), 0);
    }
    /* ...and so does gui_close_all(), which must not strand a state slot: it
     * suppresses an app's veto but still tells it that it is closing. */
    for (int i = 0; i < GUI_MAX_WINDOWS * 3; i++) {
        CHECK(gui_demo_calculator(GUI_POS_AUTO, GUI_POS_AUTO) != 0);
        gui_close_all();
    }
    CHECK(gui_demo_calculator(GUI_POS_AUTO, GUI_POS_AUTO) != 0);
    gui_close_all();
}

static void test_calculator_renders(void) {
    fixture();
    uint32_t id = gui_demo_calculator(60, 80);
    gui_pointer_show(0);
    gui_flush();
    gui_window_t *W = gui_window(id);
    const gui_theme_t *t = gui_theme();

    /* The display is a sunken box in the field colours... */
    gui_widget_t *d = gui_widget_by_id(W, 1);
    gui_rect_t cl = gui_client_rect(W);
    CHECK_EQ(px(cl.x + d->r.x + 4, cl.y + d->r.y + 4), t->field_bg);
    /* ...with a right-aligned "0" in it, i.e. ink near the right edge and none
     * near the left. */
    int right_ink = 0, left_ink = 0;
    for (int y = cl.y + d->r.y + 2; y < cl.y + d->r.y + d->r.h - 2; y++) {
        for (int x = cl.x + d->r.x + d->r.w - 16; x < cl.x + d->r.x + d->r.w - 4; x++)
            if (px(x, y) == t->field_fg) right_ink = 1;
        for (int x = cl.x + d->r.x + 4; x < cl.x + d->r.x + 20; x++)
            if (px(x, y) == t->field_fg) left_ink = 1;
    }
    CHECK(right_ink);
    CHECK(!left_ink);

    /* A key face is the button colour and turns into the pressed colour while
     * held — which is what makes a click visible in a screenshot. */
    gui_widget_t *k7 = by_label(W, "7");
    int32_t kx = cl.x + k7->r.x + k7->r.w / 2;
    int32_t ky = cl.y + k7->r.y + 2;
    CHECK_EQ(px(kx, ky), t->btn_bg);
    m_down(cl.x + k7->r.x + k7->r.w / 2, cl.y + k7->r.y + k7->r.h / 2);
    gui_flush();
    CHECK_EQ(px(kx, ky), t->btn_bg_press);
    m_up(cl.x + k7->r.x + k7->r.w / 2, cl.y + k7->r.y + k7->r.h / 2);
    gui_flush();
    CHECK_EQ(px(kx, ky), t->btn_bg);
    CHECK_STR(calc_display(W), "7");

    /* Neighbouring keys are separated by exactly the layout gap of background,
     * so no two keys touch and none overlaps. */
    gui_widget_t *k8 = by_label(W, "8");
    CHECK_EQ(k7->r.x + k7->r.w + 5, k8->r.x);
    CHECK_EQ(k7->r.y, k8->r.y);
}

static void test_notes_app(void) {
    fixture();
    const char *why = "";
    uint32_t id = gui_demo_open("notes", &why);
    CHECK(id != 0);
    gui_window_t *W = gui_window(id);
    gui_widget_t *f = gui_widget_by_id(W, 1);      /* NID_TEXT */
    CHECK(f != NULL);
    gui_rect_t cl = gui_client_rect(W);

    gui_click_at(cl.x + f->r.x + 10, cl.y + f->r.y + 10);
    CHECK_EQ(gui_wants_keys(), 1);
    CHECK_EQ(gui_take_line("hello world", 11), 1);
    CHECK_STR(gui_text(f), "hello world");
    /* The app's GUI_EV_SUBMIT hook updated the counter label. */
    CHECK_STR(gui_text(gui_widget_by_id(W, 2)), "characters: 11");

    CHECK_EQ(gui_click_widget(id, 3), 0);          /* the "clear" button */
    CHECK_STR(gui_text(f), "");
    CHECK_STR(gui_text(gui_widget_by_id(W, 2)), "characters: 0");

    /* Unknown apps are named, not guessed at. */
    CHECK_EQ(gui_demo_open("spreadsheet", &why), 0u);
    CHECK_STR(why, "unknown app");
    CHECK_EQ(gui_demo_open("", &why), 0u);
    CHECK_EQ(gui_demo_open(NULL, &why), 0u);
    CHECK_STR(gui_demo_names()[0], "calculator");
}

static void test_click_widget_api(void) {
    fixture();
    uint32_t id = gui_demo_calculator(60, 80);
    gui_window_t *W = gui_window(id);
    /* By widget id, which is what the model's gui_click tool resolves to. */
    CHECK_EQ(gui_click_widget(id, 107), 0);        /* CID_DIGIT + 7 */
    CHECK_STR(calc_display(W), "7");
    CHECK_EQ(gui_click_widget(999, 107), -1);
    CHECK_EQ(gui_click_widget(id, 12345), -2);
    /* The display is READONLY but still hit-testable, so it is not -3; a
     * disabled widget is. */
    gui_enable(gui_widget_by_id(W, 107), 0);
    CHECK_EQ(gui_click_widget(id, 107), -3);

    /* -4: A WIDGET THAT IS NOT INSIDE ITS OWN WINDOW MAY NOT BE PRESSED.
     *
     * A rect is allowed to fall outside the client area — the compositor clips,
     * so the widget is simply not drawn — but this function projects it into
     * SCREEN space and presses that pixel, and gui_click_at() then re-resolves
     * the pixel to whatever window is topmost there. Without this check, a
     * button declared 400 wide inside a 200-wide window pressed a DIFFERENT
     * window's widget, ran that app's handler and moved the keyboard grab, and
     * the caller was told it had pressed its own button.
     *
     * The victim below is a second window sitting exactly where the escaped
     * widget's centre lands: it must be untouched. */
    uint32_t victim = gui_demo_calculator(600, 400);
    gui_window_t *V = gui_window(victim);
    gui_widget_t *v7 = gui_widget_by_id(V, 107);
    gui_rect_t    vc = gui_client_rect(V);
    int32_t       tx = vc.x + v7->r.x + v7->r.w / 2;
    int32_t       ty = vc.y + v7->r.y + v7->r.h / 2;
    CHECK_STR(calc_display(V), "0");

    gui_rect_t    cl = gui_client_rect(W);
    gui_widget_t *esc = gui_add_button(W, gui_rect(tx - cl.x - 4, ty - cl.y - 4,
                                                   8, 8), "esc", 4242);
    CHECK(esc != NULL);
    uint32_t focus_before = gui_focused();
    CHECK_EQ(gui_click_widget(id, 4242), -4);
    CHECK_STR(calc_display(V), "0");             /* the other app never fired */
    CHECK_EQ(gui_focused(), focus_before);       /* and focus did not move    */
    CHECK(gui_window_close(victim) == 0);
}

/* ====================================================================== */
/* 10. coexistence with the text console                                   */
/* ====================================================================== */

static void test_console_damage_only_under_windows(void) {
    fixture();
    /* A window in the middle; the console prints at the top and the bottom. */
    (void)gui_window_open("W", 300, 300, 200, 150, 0);
    gui_pointer_show(0);
    gui_flush();
    CHECK_EQ(gui_damage_count(), 0);

    /* Text far from the window and away from the pointer: the console drew
     * those pixels itself and they are already right, so there is no damage. */
    put_cells(1, 1, "boot log line", 0x07);
    gui_test_set_console_cursor(1, 14);
    gui_tick();
    CHECK_EQ(gui_damage_count(), 0);
    CHECK(gui_stats()->console_damage > 0);

    /* Text UNDER the window is damage, clipped to the window. */
    gui_flush();
    put_cells(20, 40, "XXXXXX", 0x07);        /* pixels y=320.., x=320.. */
    gui_test_set_console_cursor(20, 46);
    gui_tick();
    /* gui_tick() flushes, so look at what it painted: the window must be back
     * on top of the text. */
    CHECK_EQ(px(340, 326), gui_theme()->client);

    /* The console's own text outside the window is left alone. */
    put_cells(20, 4, "YY", 0x07);
    gui_test_set_console_cursor(20, 6);
    gui_tick();
    CHECK_EQ(px(35, 326), 0xDEADBEEFu);       /* never painted by the GUI */
}

static void scroll_the_console(void);

static void test_console_scroll_damages_every_window(void) {
    fixture();
    uint32_t a = gui_window_open("A", 100, 100, 200, 150, 0);
    uint32_t b = gui_window_open("B", 600, 500, 200, 150, 0);
    gui_pointer_show(0);
    gui_flush();

    /* A real scroll moves every window's pixels without changing any cell those
     * windows cover, so both must be repainted even though the changed cells
     * touch neither.
     *
     * The console MUST actually be scrolled here. This test used to set the
     * cursor to the last row and assert scroll_damage rose, on the theory that
     * output from the last row implies a scroll — and it passed, because the
     * compositor made the same inference. It was therefore asserting the bug: the
     * prompt sits on the last row permanently, so that inference fired on every
     * keystroke and repainted the whole screen each time. */
    scroll_the_console();
    put_cells(GROWS - 1, 0, "prompt output", 0x07);
    gui_test_set_console_cursor(GROWS - 1, 13);
    uint32_t before = gui_stats()->scroll_damage;

    put_cells(GROWS - 1, 20, "more", 0x07);
    /* console_sync is internal; drive it through the tick and check the stat
     * plus the resulting pixels. */
    gui_tick();
    CHECK_EQ(gui_stats()->scroll_damage, before + 1);
    CHECK_EQ(px(150, 150), gui_theme()->client);
    CHECK_EQ(px(650, 550), gui_theme()->client);

    /* And the converse, which is the whole point of the change: typing at the
     * prompt on the last row WITHOUT a scroll costs no scroll repaint at all. */
    uint32_t after = gui_stats()->scroll_damage;
    for (int i = 0; i < 30; i++) {
        put_cells(GROWS - 1, 24 + i, "x", 0x07);
        gui_test_set_console_cursor(GROWS - 1, 25 + i);
        gui_tick();
    }
    CHECK_EQ(gui_stats()->scroll_damage, after);
    (void)a; (void)b;
}

static void test_tick_is_inert_without_windows(void) {
    fixture();
    const gui_stats_t *s = gui_stats();
    uint32_t p0 = s->paints;
    put_cells(3, 3, "console output with no windows", 0x07);
    gui_test_set_console_cursor(3, 33);
    for (int i = 0; i < 10; i++) gui_tick();
    /* Nothing composited, nothing presented, no pixel touched: a machine with
     * no windows open is exactly the machine that existed before the GUI. */
    CHECK_EQ(s->paints, p0);
    CHECK_EQ(px(30, 50), 0xDEADBEEFu);
    CHECK_EQ(gui_damage_count(), 0);
}

static void test_detached_is_safe(void) {
    fixture();
    gui_window_open("W", 10, 10, 200, 100, 0);
    gui_close_all();
    gui_detach();
    CHECK_EQ(gui_attached(), 0);
    /* Every entry point has to be a safe no-op with no surface: gui_init() may
     * legitimately have failed (VGA text console) and the tools still run. */
    CHECK_EQ(gui_window_open("x", 0, 0, 100, 100, 0), 0u);
    CHECK_EQ(gui_active(), 0);
    CHECK_EQ(gui_click_at(10, 10), 0);
    CHECK_EQ(gui_post_key('a'), 0);
    CHECK_EQ(gui_take_line("x", 1), 0);
    gui_invalidate(0, 0, 100, 100);
    CHECK_EQ(gui_damage_count(), 0);
    gui_flush();
    gui_tick();
    CHECK_EQ(gui_attach(NULL, NULL, 0, 0), -1);

    fb_surface_t bad;
    memset(&bad, 0, sizeof bad);
    CHECK_EQ(gui_attach(&bad, NULL, 0, 0), -1);

    /* Attaching with no console grid gives a plain black desktop. */
    CHECK_EQ(gui_attach(&screen, NULL, 0, 0), 0);
    uint32_t id = gui_window_open("W", 100, 100, 120, 80, 0);
    CHECK(id != 0);
    gui_pointer_show(0);
    gui_invalidate_all();
    gui_flush();
    CHECK_EQ(px(50, 50), fb_vga_palette[0]);      /* desktop, no console text */
    CHECK_EQ(px(150, 150), gui_theme()->client);  /* the window is still drawn */
    gui_tick();
}

static void test_pointer(void) {
    fixture();
    CHECK_EQ(gui_pointer_visible(), 0);
    uint32_t id = gui_window_open("W", 100, 100, 200, 150, 0);
    /* Opening the first window makes the pointer live. */
    CHECK_EQ(gui_pointer_visible(), 1);
    gui_pointer_warp(400, 300);
    int32_t x, y;
    gui_pointer_position(&x, &y);
    CHECK_EQ(x, 400); CHECK_EQ(y, 300);
    gui_flush();
    /* The arrow is drawn: its top-left pixel is the outline colour. */
    CHECK_EQ(px(400, 300), gui_theme()->shadow);
    /* Moving damages both the old and the new position, so no trail is left. */
    gui_pointer_warp(500, 300);
    gui_flush();
    CHECK_EQ(px(400, 300), fb_vga_palette[0]);
    CHECK_EQ(px(500, 300), gui_theme()->shadow);
    /* Closing the last window puts the pointer away. */
    gui_window_close(id);
    CHECK_EQ(gui_pointer_visible(), 0);
    gui_flush();
    CHECK_EQ(px(500, 300), fb_vga_palette[0]);
}

/* Exactly what lib/base.c's scroll() does to the screen: shift the cells up one
 * row, blank the vacated one, shift the PIXELS up one glyph row, and present.
 * The window manager is not told, which is the whole point. */
static void scroll_the_console(void) {
    for (int i = 0; i < GCOLS * (GROWS - 1); i++) cells[i] = cells[i + GCOLS];
    for (int i = 0; i < GCOLS; i++) cells[GCOLS * (GROWS - 1) + i] = BLANK_CELL;
    fb_clip_reset(&screen);
    fb_scroll(&screen, 16, fb_vga_palette[0]);
    /* lib/base.c counts its own scrolls and the compositor reads that counter, so
     * an emulated scroll has to count too. Without this the WM would correctly
     * conclude that nothing moved. */
    gui_test_console_scrolled();
}

static void test_console_scroll_repairs_the_band_above_a_window(void) {
    fixture();
    for (int r = 0; r < GROWS; r++) put_cells(r, 0, "console", 0x07);
    uint32_t id = gui_window_open("W", 200, 200, 200, 150, 0);
    gui_pointer_show(0);
    gui_window_t *W = gui_window(id);
    const fb_color_t MARK = FB_RGB(0xff, 0x00, 0xff);
    gui_add_panel(W, gui_client_area(W), MARK, 0);
    gui_invalidate_all();
    gui_flush();
    gui_rect_t f = W->frame;
    CHECK_EQ(px(f.x + 20, f.y + 40), MARK);
    /* Nothing of the window is above its own frame yet. */
    CHECK(px(f.x + 20, f.y - 8) != MARK);

    /* Now the console scrolls behind the GUI's back, dragging the window's
     * pixels up one text row and leaving a copy of its top rows above the
     * frame. Eight pixels above the frame that copy is the TITLE BAR, since the
     * title bar is what used to be 8 pixels below the top edge. */
    scroll_the_console();
    CHECK_EQ(px(f.x + 20, f.y - 8), gui_theme()->title_bg_focus);
    uint32_t before = gui_stats()->scroll_damage;

    /* One more line of output at the bottom, the cursor on the last row: this is
     * exactly the state a prompt is in. */
    put_cells(GROWS - 1, 0, "> hello", 0x07);
    gui_test_set_console_cursor(GROWS - 1, 7);
    gui_tick();

    CHECK_EQ(gui_stats()->scroll_damage, before + 1);
    /* The smear is gone and the window is intact. */
    CHECK(px(f.x + 20, f.y - 8) != gui_theme()->title_bg_focus);
    CHECK(px(f.x + 20, f.y - 8) != MARK);
    CHECK_EQ(px(f.x + 20, f.y + 40), MARK);
    CHECK_EQ(px(f.x, f.y), gui_theme()->frame_focus);
    /* ...and the repair was MINIMAL: a pixel far from every window was left to
     * the console, which had already drawn it. */
    CHECK(px(SW - 3, 4) == 0xDEADBEEFu || px(SW - 3, 4) == fb_vga_palette[0]);

    /* A scroll of several rows in one gap is measured, not guessed. */
    for (int k = 0; k < 4; k++) scroll_the_console();
    CHECK_EQ(px(f.x + 20, f.y - 40), MARK);
    put_cells(GROWS - 1, 0, "> again", 0x07);
    gui_test_set_console_cursor(GROWS - 1, 7);
    gui_tick();
    CHECK(px(f.x + 20, f.y - 40) != MARK);
    CHECK(px(f.x + 20, f.y - 8) != MARK);
    CHECK_EQ(px(f.x + 20, f.y + 40), MARK);

    /* A burst too big to measure falls back to repainting the screen, which is
     * still correct — never a smear. */
    for (int k = 0; k < 20; k++) scroll_the_console();
    put_cells(GROWS - 1, 0, "> burst", 0x07);
    gui_test_set_console_cursor(GROWS - 1, 7);
    gui_tick();
    CHECK(px(f.x + 20, f.y - 8) != MARK);
    CHECK(px(f.x + 20, f.y - 8) != gui_theme()->title_bg_focus);
    CHECK_EQ(px(f.x + 20, f.y + 40), MARK);
}

static void test_gui_sync_repaints_without_dispatching(void) {
    fixture();
    uint32_t id = gui_window_open("W", 100, 100, 200, 150, 0);
    gui_pointer_show(0);
    gui_window_t *W = gui_window(id);
    gui_widget_t *b = gui_add_button(W, gui_rect(10, 10, 60, 30), "go", 1);
    gui_sync();
    CHECK_EQ(gui_damage_count(), 0);

    /* A change plus gui_sync() is on screen immediately — this is what the
     * model's tools rely on, because chat_ask() will not tick again for
     * seconds. */
    gui_set_text(b, "done");
    gui_invalidate_widget(id, b);
    CHECK(gui_damage_count() > 0);
    gui_sync();
    CHECK_EQ(gui_damage_count(), 0);
    gui_rect_t cl = gui_client_rect(W);
    int ink = 0;
    for (int y = cl.y + 10; y < cl.y + 40; y++)
        for (int x = cl.x + 10; x < cl.x + 70; x++)
            if (px(x, y) == gui_theme()->btn_fg) ink = 1;
    CHECK(ink);

    /* It also repairs console damage, which is the difference from gui_flush(). */
    put_cells(100 / 16 + 2, 100 / 8 + 2, "XXXX", 0x07);
    gui_test_set_console_cursor(8, 20);
    gui_sync();
    CHECK_EQ(px(cl.x + 5, cl.y + 5), gui_theme()->client);
}

/* ====================================================================== */
/* 11. the tools                                                          */
/* ====================================================================== */

static char        rbuf[TOOL_RESULT_MAX];
static tool_result_t res;

static int call(const char *name, const char *input) {
    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = input;
    c.input_len = input ? strlen(input) : 0;
    tool_result_init(&res, rbuf, sizeof rbuf);
    kcap_reset();
    return tool_dispatch(&c, &res);
}

static void test_tool_registry(void) {
    CHECK_EQ(tool_count(), 4);
    CHECK(tool_find("gui_list") != NULL);
    CHECK(tool_find("gui_open") != NULL);
    CHECK(tool_find("gui_window") != NULL);
    CHECK(tool_find("gui_click") != NULL);

    /* Every schema must be valid JSON and every description non-trivial: the
     * model reads these and a broken one costs it the whole tool. */
    for (int i = 0; i < tool_count(); i++) {
        const tool_t *t = tool_at(i);
        json_value_t v;
        CHECK_EQ(json_parse(t->input_schema, strlen(t->input_schema), &v), JSON_OK);
        CHECK_EQ(v.type, JSON_OBJECT);
        CHECK(strlen(t->description) > 120);
        CHECK(strlen(t->name) < TOOL_NAME_MAX);
    }
    /* The whole family fits in a schema the request can carry. */
    char big[8192];
    size_t n = tools_build_schema(big, sizeof big);
    CHECK(n > 0 && n < sizeof big);
    json_value_t arr;
    CHECK_EQ(json_parse(big, n, &arr), JSON_OK);
    CHECK_EQ(arr.type, JSON_ARRAY);
    CHECK_EQ((int)json_count(&arr), tool_count());
}

static void test_tool_happy_path(void) {
    fixture();
    /* gui_open really opens a window and reports its id. */
    CHECK_EQ(call("gui_open", "{\"app\":\"calculator\"}"), TOOL_OK);
    CHECK_EQ(res.is_error, 0);
    CHECK_CONTAINS(rbuf, "opened \"calculator\"");
    CHECK_CONTAINS(kcap_text(), "[gui_open app=calculator");
    CHECK_EQ(gui_window_count(), 1);
    uint32_t id = gui_window_at_z(0)->id;

    char in[128];
    snprintf(in, sizeof in, "{\"id\":%u}", (unsigned)id);
    CHECK_EQ(call("gui_list", in), TOOL_OK);
    CHECK_CONTAINS(rbuf, "Calculator");
    CHECK_CONTAINS(rbuf, "button id=107");
    CHECK_CONTAINS(rbuf, "text=\"7\"");
    CHECK_CONTAINS(rbuf, "readonly");
    CHECK_CONTAINS(kcap_text(), "[gui_list windows=1");

    /* gui_click by label drives the app, and reports what changed. */
    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"7\"}", (unsigned)id);
    CHECK_EQ(call("gui_click", in), TOOL_OK);
    CHECK_CONTAINS(rbuf, "clicked the button labelled \"7\"");
    CHECK_CONTAINS(rbuf, "field=\"7\"");
    CHECK_CONTAINS(kcap_text(), "label=7");

    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"+\"}", (unsigned)id);
    call("gui_click", in);
    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"5\"}", (unsigned)id);
    call("gui_click", in);
    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"=\"}", (unsigned)id);
    CHECK_EQ(call("gui_click", in), TOOL_OK);
    CHECK_CONTAINS(rbuf, "field=\"12\"");

    /* gui_window: move, then read the new position back. */
    snprintf(in, sizeof in,
             "{\"id\":%u,\"action\":\"move\",\"x\":500,\"y\":400}", (unsigned)id);
    CHECK_EQ(call("gui_window", in), TOOL_OK);
    CHECK_EQ(gui_window(id)->frame.x, 500);
    CHECK_CONTAINS(rbuf, "now at 500,400");
    CHECK_CONTAINS(kcap_text(), "[gui_window action=move");

    /* Clamping is reported rather than hidden. */
    snprintf(in, sizeof in,
             "{\"id\":%u,\"action\":\"move\",\"x\":90000,\"y\":90000}",
             (unsigned)id);
    CHECK_EQ(call("gui_window", in), TOOL_OK);
    CHECK_CONTAINS(rbuf, "clamped");

    snprintf(in, sizeof in, "{\"id\":%u,\"action\":\"hide\"}", (unsigned)id);
    CHECK_EQ(call("gui_window", in), TOOL_OK);
    CHECK((gui_window(id)->flags & GUI_WIN_HIDDEN) != 0);
    snprintf(in, sizeof in, "{\"id\":%u,\"action\":\"show\"}", (unsigned)id);
    CHECK_EQ(call("gui_window", in), TOOL_OK);

    /* Clicking a screen position with no window id. */
    CHECK_EQ(call("gui_click", "{\"x\":2,\"y\":2}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "the desktop");

    snprintf(in, sizeof in, "{\"id\":%u,\"action\":\"close\"}", (unsigned)id);
    CHECK_EQ(call("gui_window", in), TOOL_OK);
    CHECK_CONTAINS(rbuf, "closed window");
    CHECK_EQ(gui_window_count(), 0);

    CHECK_EQ(call("gui_list", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "No windows are open");
}

/* Every rejection must be legible, must not have acted, and must still emit
 * exactly one trace line. */
static void expect_error(const char *tool, const char *input,
                         const char *needle) {
    uint64_t before = trace_emissions();
    /* tool_dispatch() returns TOOL_OK whenever the HANDLER RAN (see tool.h); a
     * handler's own refusal is reported through is_error, and that is what a
     * caller must check. */
    (void)call(tool, input);
    CHECK_EQ(res.is_error, 1);
    CHECK_CONTAINS(rbuf, "error:");
    if (needle) CHECK_CONTAINS(rbuf, needle);
    CHECK_EQ((int)(trace_emissions() - before), 1);
}

static void test_tool_untrusted_input(void) {
    fixture();
    uint32_t id = gui_demo_calculator(40, 40);
    char in[16384];

    /* Not an object, truncated, wrong types. */
    expect_error("gui_list", "[1,2,3]", "JSON object");
    expect_error("gui_list", "{\"id\":", "JSON object");
    expect_error("gui_list", "{\"id\":\"three\"}", "integer");
    expect_error("gui_list", "{\"id\":1.5}", "integer");
    expect_error("gui_list", "{\"id\":99999999999999999999}", "not a possible");
    expect_error("gui_list", "{\"id\":-4}", "not a possible");
    expect_error("gui_list", "{\"widgets\":\"yes\"}", "boolean");
    expect_error("gui_list", "{\"id\":424242}", "no window with id");

    /* Absent required fields. */
    expect_error("gui_open", "{}", "\"app\" is required");
    expect_error("gui_open", "{\"app\":123}", "\"app\" is required");
    expect_error("gui_open", "{\"app\":\"spreadsheet\"}", "unknown app");
    expect_error("gui_window", "{}", "\"id\" is required");
    expect_error("gui_window", "{\"id\":1}", "\"action\" is required");

    snprintf(in, sizeof in, "{\"id\":%u,\"action\":\"explode\"}", (unsigned)id);
    expect_error("gui_window", in, "unknown action");
    snprintf(in, sizeof in, "{\"id\":%u,\"action\":\"move\",\"x\":1e9}",
             (unsigned)id);
    expect_error("gui_window", in, "\"x\" must be an integer");
    snprintf(in, sizeof in, "{\"id\":%u,\"action\":\"resize\",\"width\":0}",
             (unsigned)id);
    expect_error("gui_window", in, "\"width\" must be");
    expect_error("gui_window", "{\"id\":777,\"action\":\"focus\"}",
                 "no window with id");

    /* gui_click: every way of naming nothing. */
    expect_error("gui_click", "{}", "give \"id\"");
    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"nope\"}", (unsigned)id);
    expect_error("gui_click", in, "nothing clickable");
    CHECK_CONTAINS(rbuf, "clickable labels");
    snprintf(in, sizeof in, "{\"id\":%u,\"widget\":424242}", (unsigned)id);
    expect_error("gui_click", in, "nothing clickable");
    snprintf(in, sizeof in, "{\"id\":%u,\"x\":9999,\"y\":9999}", (unsigned)id);
    expect_error("gui_click", in, "nothing clickable");
    expect_error("gui_click", "{\"id\":31337,\"label\":\"7\"}", "no window");
    expect_error("gui_click", "{\"id\":1,\"label\":true}", "must be a string");

    /* A 10k-character label, and one longer than any widget text. */
    {
        size_t o = 0;
        o += (size_t)snprintf(in + o, sizeof in - o, "{\"id\":%u,\"label\":\"",
                              (unsigned)id);
        for (int i = 0; i < 10000; i++) in[o++] = 'A';
        snprintf(in + o, sizeof in - o, "\"}");
        expect_error("gui_click", in, "longer than");
    }

    /* An embedded NUL inside a JSON string must not truncate the comparison
     * into a match against a real label. */
    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"7\\u0000extra\"}",
             (unsigned)id);
    expect_error("gui_click", in, "embedded NUL");

    /* Deeply nested and duplicated keys must not fault. */
    call("gui_list", "{\"id\":{\"id\":{\"id\":1}}}");
    CHECK_EQ(res.is_error, 1);
    call("gui_list", "{\"widgets\":true,\"widgets\":false}");

    /* Nothing above changed the window. */
    CHECK(gui_window(id) != NULL);
    CHECK_STR(calc_display(gui_window(id)), "0");
    CHECK_EQ(gui_window_count(), 1);

    /* Unknown tool: dispatch's own error path, not ours. */
    CHECK_EQ(call("gui_nope", "{}"), TOOL_ENOENT);
}

static void test_tools_without_a_framebuffer(void) {
    fixture();
    gui_close_all();
    gui_detach();
    expect_error("gui_list", "{}", "no window manager");
    expect_error("gui_open", "{\"app\":\"calculator\"}", "no window manager");
    expect_error("gui_window", "{\"id\":1,\"action\":\"focus\"}",
                 "no window manager");
    expect_error("gui_click", "{\"x\":1,\"y\":1}", "no window manager");
}

/* gui_click USED TO ASSERT A PRESS IT HAD NOT LANDED.
 *
 * It captured gui_click_at()'s return value, used it only as the trace line's
 * number, and then unconditionally printed "clicked the button labelled X ...".
 * So the two ways a document breaks its own layout — a widget outside the
 * window, and a widget with no area — both produced a tool result that told the
 * model the click happened. The model's only counter-evidence was the
 * "window now shows:" tail, which for a button-only window reads "(no text
 * widgets)", and the operator's only counter-evidence was a trailing "-> 0"
 * that looks exactly like a click on a window with no handler. */
static void test_gui_click_does_not_claim_a_press_it_did_not_land(void) {
    fixture();
    char in[256];

    /* (a) A WIDGET OUTSIDE ITS OWN WINDOW. No hostile document is needed: this
     *     is what an ordinary rect that is wider than the window produces. */
    uint32_t id = gui_window_open("Oops", 100, 100, 200, 120, 0);
    gui_window_t *W = gui_window(id);
    CHECK(gui_add_button(W, gui_rect(10, 10, 400, 40), "Press", 1) != NULL);

    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"Press\"}", (unsigned)id);
    /* tool_dispatch() returns TOOL_OK whenever the HANDLER RAN (tool.h); the
     * handler's own refusal is carried by is_error, which is what a caller
     * checks. */
    CHECK_EQ(call("gui_click", in), TOOL_OK);
    CHECK_EQ(res.is_error, 1);
    CHECK_CONTAINS(rbuf, "is not inside window");
    CHECK_CONTAINS(rbuf, "Nothing was pressed");
    CHECK_CONTAINS(rbuf, "make the window bigger");
    CHECK_CONTAINS(kcap_text(), "off-window");

    /* (b) A ZERO-AREA WIDGET. Inside the frame, so the old off-window guard
     *     would not see it, and the press lands on the window but on no widget
     *     at all — the trace read "-> 1" and the result claimed success. */
    CHECK(gui_add_button(W, gui_rect(20, 60, 0, 0), "Nothing", 2) != NULL);
    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"Nothing\"}", (unsigned)id);
    CHECK_EQ(call("gui_click", in), TOOL_OK);
    CHECK_EQ(res.is_error, 1);
    CHECK_CONTAINS(rbuf, "zero area");
    CHECK_CONTAINS(kcap_text(), "zero-area");

    /* (c) A GOOD WIDGET STILL WORKS, and still says so. */
    CHECK(gui_add_button(W, gui_rect(20, 60, 80, 24), "Fine", 3) != NULL);
    snprintf(in, sizeof in, "{\"id\":%u,\"label\":\"Fine\"}", (unsigned)id);
    CHECK_EQ(call("gui_click", in), TOOL_OK);
    CHECK_EQ(res.is_error, 0);
    CHECK_CONTAINS(rbuf, "clicked the button labelled \"Fine\"");
    CHECK(strstr(rbuf, "NOTHING CONSUMED IT") == NULL);
}

/* A CLICK THAT DESTROYS A WINDOW MUST SAY SO, in the kernel's own line.
 *
 * gui_tools.c's header promises "the transcript shows which window was really
 * closed and not which one the model said it closed". That held for
 * gui_window action=close and not for this path — and this path is the ONLY
 * route to a close box, because the id+x/y branch resolves client-relative
 * coordinates through gui_widget_at() and can never reach the title bar. The
 * whole record of closing every window the operator was using used to be
 * "[gui_click at=390,108 -> 1]", with a result saying "a window took it" about
 * a window that no longer existed. */
static void test_clicking_a_close_box_is_named_in_the_trace(void) {
    fixture();
    uint32_t id = gui_window_open("Victim", 100, 100, 220, 140, 0);
    gui_window_t *W = gui_window(id);
    /* The close box, computed the way gui/wm.c's close_rect() does. */
    int32_t cx = W->frame.x + W->frame.w - GUI_BORDER - 6 - 12 / 2;
    int32_t cy = W->frame.y + GUI_BORDER + GUI_TITLE_H / 2;

    char in[128];
    snprintf(in, sizeof in, "{\"x\":%d,\"y\":%d}", (int)cx, (int)cy);
    CHECK_EQ(call("gui_click", in), TOOL_OK);
    CHECK_EQ(gui_window(id), NULL);                  /* it really is gone */
    CHECK_CONTAINS(rbuf, "THE CLICK CLOSED IT");
    CHECK_CONTAINS(rbuf, "Victim");
    CHECK_CONTAINS(kcap_text(), "title=Victim");
    CHECK_CONTAINS(kcap_text(), "CLOSED");

    /* An ordinary click on a surviving window names it too, and does not
     * claim a close. */
    uint32_t keep = gui_window_open("Keeper", 300, 300, 200, 120, 0);
    gui_window_t *K = gui_window(keep);
    snprintf(in, sizeof in, "{\"x\":%d,\"y\":%d}",
             (int)(K->frame.x + K->frame.w / 2),
             (int)(K->frame.y + K->frame.h - 10));
    CHECK_EQ(call("gui_click", in), TOOL_OK);
    CHECK(gui_window(keep) != NULL);
    CHECK_CONTAINS(rbuf, "Keeper");
    CHECK(strstr(rbuf, "CLOSED IT") == NULL);
    CHECK_CONTAINS(kcap_text(), "title=Keeper");
    CHECK(strstr(kcap_text(), "CLOSED") == NULL);
}

static void test_result_buffer_is_never_overrun(void) {
    fixture();
    /* Twelve windows of 48 widgets is far more text than one result can hold.
     * The listing must truncate legibly and stay inside the buffer. */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        uint32_t id = gui_window_open("A window with a fairly long title", 10, 10,
                                      300, 200, 0);
        gui_window_t *W = gui_window(id);
        for (int k = 0; k < GUI_MAX_WIDGETS; k++)
            gui_add_button(W, gui_rect(k, k, 30, 20), "a label", (uint32_t)k + 1);
    }
    tool_call_t c = { "id", "gui_list", "{\"widgets\":true}", 16 };
    char small[512];
    tool_result_init(&res, small, sizeof small);
    CHECK_EQ(tool_dispatch(&c, &res), TOOL_OK);
    CHECK(res.len < sizeof small);
    CHECK_EQ(small[res.len], '\0');
    CHECK_CONTAINS(small, "truncated");
}

/* ====================================================================== */

int main(void) {
    install_tool_table();

    RUN(test_rect_edges_are_half_open);
    RUN(test_rect_intersect_union_inset);
    RUN(test_grid_tiles_exactly);
    RUN(test_grid_spans_and_bad_input);
    RUN(test_splits);

    RUN(test_window_open_and_clamp);
    RUN(test_title_is_sanitised);
    RUN(test_pool_exhaustion);
    RUN(test_zorder);
    RUN(test_focus_transitions);
    RUN(test_damage_is_a_superset);

    RUN(test_pixels_frame_and_chrome);
    RUN(test_pixels_backdrop_is_the_console);
    RUN(test_pixels_zorder_overlap);

    RUN(test_hit_test_boundaries);
    RUN(test_dispatch_to_the_right_widget);
    RUN(test_click_raises_and_focuses);
    RUN(test_drag_to_move);
    RUN(test_close_box);
    RUN(test_app_can_refuse_to_close);

    RUN(test_one_line_keyboard_grab);
    RUN(test_field_editing);
    RUN(test_post_key_to_focused_field);

    RUN(test_calculator_arithmetic_by_clicking);
    RUN(test_calculator_renders);
    RUN(test_notes_app);
    RUN(test_click_widget_api);

    RUN(test_console_damage_only_under_windows);
    RUN(test_console_scroll_damages_every_window);
    RUN(test_console_scroll_repairs_the_band_above_a_window);
    RUN(test_gui_sync_repaints_without_dispatching);
    RUN(test_tick_is_inert_without_windows);
    RUN(test_detached_is_safe);
    RUN(test_pointer);

    RUN(test_tool_registry);
    RUN(test_tool_happy_path);
    RUN(test_tool_untrusted_input);
    RUN(test_gui_click_does_not_claim_a_press_it_did_not_land);
    RUN(test_clicking_a_close_box_is_named_in_the_trace);
    RUN(test_tools_without_a_framebuffer);
    RUN(test_result_buffer_is_never_overrun);

    return th_report("gui");
}
