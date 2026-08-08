/* widgets.h — the widget toolkit: rectangles, layout, and the four controls a
 *             window is made of.
 *
 * PURPOSE
 *   An app on this machine is a window plus a handful of widgets plus one event
 *   hook. This header is the "plus a handful of widgets" part, and it is
 *   deliberately the SMALLER half of the GUI: gui.h owns windows, z-order, focus
 *   and the event loop; everything here is pure arithmetic over rectangles and
 *   pixels, with no static state at all beyond the theme.
 *
 *   That split is what makes the toolkit testable. A widget is a value in a
 *   window's fixed array; laying one out is a function from a rectangle to a
 *   rectangle; painting one is a function from a widget and a surface to pixels;
 *   hit-testing one is a comparison. tests/host/test_gui.c calls every one of
 *   those directly, against a malloc'd fb_surface_t, and asserts exact pixels
 *   and exact boundaries. Nothing here needs a framebuffer, a mouse or a boot.
 *
 * DESIGNED FOR A CALCULATOR FIRST
 *   The brief is "the operator says 'I want a calculator' and a working
 *   calculator appears", so the calculator drove the design and the general case
 *   was extracted from it rather than the other way round. Three consequences
 *   are visible in the API:
 *
 *     1. gui_grid() is exact, not approximate. A 4x5 keypad has to tile its area
 *        with no seams and no overlaps whatever the window size, including sizes
 *        that do not divide by four. So cell edges are computed as
 *        (avail * i) / n and each cell ends where the next one begins: adjacent
 *        cells share an edge to the pixel, widths differ by at most one, and the
 *        last cell ends exactly on the area's right edge. A layout that is one
 *        pixel out shows up as a visible seam and as a dead column between two
 *        buttons, which is a bug a model authoring an app cannot debug.
 *
 *     2. Spans exist (colspan/rowspan), because "=" is a double-width key and a
 *        display is a full-width one. A span is defined as "from the left edge of
 *        cell (col) to the right edge of cell (col+colspan-1)", so a spanning
 *        cell is exactly the union of the cells it covers plus the gaps between
 *        them — never a re-division of the area.
 *
 *     3. A widget's text is a fixed char array, not a pointer. A calculator's
 *        display changes on every keypress and the alternative is either an
 *        allocation per keystroke or a lifetime rule an app author will get
 *        wrong. GUI_TEXT_MAX is 40, which holds a 12-digit result with a sign, a
 *        decimal point and room to be wrong in.
 *
 * COORDINATE SPACES — there are exactly two, and mixing them is the classic bug
 *   SCREEN coordinates are pixels from the top-left of the framebuffer. A
 *   window's frame rectangle is in screen space.
 *   CLIENT coordinates are pixels from the top-left of a window's client area,
 *   i.e. inside the border and below the title bar. Every widget rectangle is in
 *   client space, so moving a window does not touch its widgets, and a widget can
 *   be laid out before its window is placed.
 *   gui.h converts between them in exactly one place (gui_client_rect()), and
 *   event delivery hands a widget hook coordinates relative to THE WIDGET.
 *
 * WHAT A WIDGET IS
 *   Four kinds, chosen as the smallest set that makes a real app:
 *     GUI_PANEL   a filled rectangle: a background, a group box, a separator.
 *                 Not hit-testable by default, so a panel behind a keypad cannot
 *                 swallow the keypad's clicks.
 *     GUI_LABEL   text, aligned left/centre/right, optionally over a fill.
 *     GUI_BUTTON  text in a raised box that sinks while held. Activation is
 *                 press-then-release-inside, like every other GUI: pressing and
 *                 dragging off cancels, which is the behaviour that makes a
 *                 misclick recoverable.
 *     GUI_FIELD   a sunken box holding editable text with a caret. Read-only
 *                 fields are the honest way to render a value the app owns (a
 *                 calculator display), and they are still selectable so a click
 *                 can focus the window without arming the keyboard.
 *
 *   Each carries `id` (app-assigned, 0 = anonymous), a state byte, colours, and
 *   an optional per-widget event hook. The hook is tried first and the window's
 *   hook second, so an app can either write one switch on widget ids (what the
 *   calculator does) or attach behaviour per control, without the toolkit having
 *   an opinion.
 *
 * PUBLIC API
 *   rectangles  gui_rect gui_rect_empty gui_rect_contains gui_rect_intersect
 *               gui_rect_intersects gui_rect_union gui_inset
 *   layout      gui_grid gui_split_top gui_split_bottom gui_split_left
 *               gui_split_right gui_center
 *   widgets     gui_add_panel gui_add_label gui_add_button gui_add_field
 *               gui_widget_by_id gui_widget_at gui_widget_index
 *               gui_set_text gui_text gui_set_state gui_enable
 *   painting    gui_paint_widget gui_paint_text (both take any surface)
 *   editing     gui_field_key gui_field_clear
 *   theme       gui_theme gui_set_theme gui_metrics
 *
 * DEPENDENCIES
 *   fb.h (surfaces, clipped drawing, the 0x00RRGGBB colour format) and font.h
 *   (the 8x16 font and UTF-8 measurement). Nothing else — no heap, no hardware,
 *   no console. gui.h includes this header, not the other way round.
 *
 * FUTURE EXTENSION POINTS
 *   - A new widget kind is a case in gui_paint_widget() plus a hit rule; the
 *     window's array does not change shape.
 *   - Hover state: the WM already computes the widget under the pointer for
 *     every motion event, so a GUI_W_HOT bit and one extra colour in the theme
 *     would light buttons up under the cursor with no new plumbing.
 *   - Scrolling and clipping to a viewport: fb_clip_set() intersects rather than
 *     replaces, so a container widget that narrows the clip before painting its
 *     children needs nothing from this header.
 *   - Proportional fonts: every measurement goes through gui_text_width(), which
 *     is the only place that assumes a fixed advance.
 */
#ifndef WIDGETS_H
#define WIDGETS_H

#include <stdint.h>
#include <stddef.h>

#include "fb.h"
#include "font.h"

/* Declared here so the event-hook typedef below can name them before either is
 * defined: gui.h owns gui_window_t and gui_event_t, and it includes this file. */
struct gui_window;
struct gui_event;
struct gui_widget;

/* ====================================================================== */
/* rectangles                                                             */
/* ====================================================================== */

/* Signed, like fb.h's coordinates: a window dragged off the left edge has a
 * negative x and everything downstream must cope rather than refuse. w/h are
 * never negative in a valid rectangle; a non-positive w or h means EMPTY, and
 * every function here treats it that way instead of computing with it. */
typedef struct gui_rect {
    int32_t x, y, w, h;
} gui_rect_t;

static inline gui_rect_t gui_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    gui_rect_t r = { x, y, w, h };
    return r;
}

int  gui_rect_empty(gui_rect_t r);

/* Half-open on both axes: x in [r.x, r.x+r.w). A point on the right or bottom
 * edge is OUTSIDE, which is what makes two adjacent cells partition their
 * boundary instead of both claiming it. tests/host/test_gui.c pins all four
 * edges of a button because this is the rule every hit test inherits. */
int  gui_rect_contains(gui_rect_t r, int32_t x, int32_t y);

int  gui_rect_intersects(gui_rect_t a, gui_rect_t b);
gui_rect_t gui_rect_intersect(gui_rect_t a, gui_rect_t b);

/* Smallest rectangle containing both. An empty operand returns the other. */
gui_rect_t gui_rect_union(gui_rect_t a, gui_rect_t b);

/* Shrink by dx on the left and right and dy on the top and bottom. Negative
 * values grow it. Collapses to empty rather than inverting. */
gui_rect_t gui_inset(gui_rect_t r, int32_t dx, int32_t dy);

/* ====================================================================== */
/* layout                                                                 */
/* ====================================================================== */

/* One cell of a rows x cols grid over `area`, with `gap` pixels between
 * neighbours (and none around the outside).
 *
 * EXACTNESS IS THE CONTRACT. For every valid grid:
 *     cell(0,0).x == area.x
 *     cell(r, c).x + cell(r, c).w + gap == cell(r, c+1).x
 *     cell(r, cols-1).x + w == area.x + area.w
 * and the same on the vertical axis. Cell widths differ by at most one pixel.
 * A span of n covers its n cells and the n-1 gaps between them.
 *
 * Out-of-range indices, spans that run past the edge, a gap wider than the
 * area, and non-positive rows/cols all return the empty rectangle rather than
 * something plausible-looking: a layout bug should be invisible, not subtly
 * misplaced. */
gui_rect_t gui_grid(gui_rect_t area, int rows, int cols,
                    int row, int col, int rowspan, int colspan, int gap);

/* Slice `n` pixels off one edge. *rest (may be NULL) gets what is left, with
 * `gap` pixels dropped between the two. Slicing more than there is yields the
 * whole area and an empty rest. This is the box layout: a display on top and a
 * keypad below is one gui_split_top(). */
gui_rect_t gui_split_top   (gui_rect_t area, int32_t n, int32_t gap, gui_rect_t *rest);
gui_rect_t gui_split_bottom(gui_rect_t area, int32_t n, int32_t gap, gui_rect_t *rest);
gui_rect_t gui_split_left  (gui_rect_t area, int32_t n, int32_t gap, gui_rect_t *rest);
gui_rect_t gui_split_right (gui_rect_t area, int32_t n, int32_t gap, gui_rect_t *rest);

/* A w x h rectangle centred in `area` (biased up/left on an odd remainder). */
gui_rect_t gui_center(gui_rect_t area, int32_t w, int32_t h);

/* ====================================================================== */
/* widgets                                                                */
/* ====================================================================== */

/* Kinds. Kept as a small enum rather than a vtable: there are four of them, the
 * paint switch is 60 lines, and a function pointer per widget per window is
 * memory spent on flexibility nothing has asked for yet. */
#define GUI_PANEL   0
#define GUI_LABEL   1
#define GUI_BUTTON  2
#define GUI_FIELD   3

/* State bits. */
#define GUI_W_DISABLED  0x01   /* greyed; ignores hits                        */
#define GUI_W_PRESSED   0x02   /* a button held down right now                 */
#define GUI_W_FOCUS     0x04   /* the keyboard target within its window        */
#define GUI_W_READONLY  0x08   /* a field the operator may not type into       */
#define GUI_W_NOHIT     0x10   /* transparent to the pointer (panels, labels)  */
#define GUI_W_BORDER    0x20   /* panels/labels: draw a 1px edge               */
#define GUI_W_OPAQUE    0x40   /* labels: fill bg before drawing text          */

/* Text alignment. */
#define GUI_ALIGN_LEFT   0
#define GUI_ALIGN_CENTER 1
#define GUI_ALIGN_RIGHT  2

/* Longest widget text, including the NUL. A calculator result is at most 12
 * significant digits, a sign, a point and a status word. */
#define GUI_TEXT_MAX 40

/* Return value of an event hook: 1 = handled (stop here), 0 = not mine. */
typedef int (*gui_widget_fn)(struct gui_window *win, struct gui_widget *w,
                            const struct gui_event *ev);

typedef struct gui_widget {
    uint8_t       kind;
    uint8_t       state;
    uint8_t       align;
    uint8_t       pad_;
    uint32_t      id;               /* app-assigned; 0 = anonymous            */
    gui_rect_t    r;                /* CLIENT coordinates                     */
    char          text[GUI_TEXT_MAX];
    uint16_t      caret;            /* GUI_FIELD: insertion point in bytes    */
    fb_color_t    fg, bg;
    gui_widget_fn on_event;         /* tried before the window's hook          */
    void         *user;             /* app-private, never touched here        */
} gui_widget_t;

/* ---- construction ----
 * Every add returns the widget, or NULL when the window is full (WIN_MAX_WIDGETS
 * in gui.h) or the arguments are unusable. NULL is a legible failure: an app
 * that ignores it gets a window with a missing control, not corrupted memory.
 * Widgets are painted in creation order, so a later widget is ON TOP of an
 * earlier one — which is also the hit-test order, reversed. */
gui_widget_t *gui_add_panel (struct gui_window *win, gui_rect_t r, fb_color_t bg,
                             uint8_t state);
gui_widget_t *gui_add_label (struct gui_window *win, gui_rect_t r,
                             const char *text, uint8_t align);
gui_widget_t *gui_add_button(struct gui_window *win, gui_rect_t r,
                             const char *text, uint32_t id);
gui_widget_t *gui_add_field (struct gui_window *win, gui_rect_t r,
                             const char *text, uint32_t id, uint8_t state);

/* ---- lookup ---- */
gui_widget_t *gui_widget_by_id(struct gui_window *win, uint32_t id);
/* Topmost hit-testable widget containing the CLIENT point, or NULL. */
gui_widget_t *gui_widget_at(struct gui_window *win, int32_t cx, int32_t cy);
/* Index in the window's array, or -1. Used by trace lines and tools. */
int gui_widget_index(const struct gui_window *win, const gui_widget_t *w);

/* ---- text ----
 * Always NUL-terminates and never writes past GUI_TEXT_MAX-1. Returns the number
 * of bytes stored, which is how a caller learns it was truncated. A NULL text
 * clears the widget. Truncation is at a byte boundary and may split a UTF-8
 * sequence; the font's replacement glyph makes that visible rather than silent. */
size_t      gui_set_text(gui_widget_t *w, const char *s);
size_t      gui_append_text(gui_widget_t *w, const char *s);
const char *gui_text(const gui_widget_t *w);

void gui_set_state(gui_widget_t *w, uint8_t set, uint8_t clear);
void gui_enable(gui_widget_t *w, int on);

/* ---- editing ----
 * One keystroke into a field. Handles printable ASCII, backspace (0x08/0x7F),
 * and leaves everything else alone. Returns 1 if the field changed (so the
 * caller knows whether to invalidate), 0 if the key was ignored, and -1 if the
 * widget is not an editable field. Enter is NOT handled here: submission is the
 * app's decision and gui.h delivers it as GUI_EV_SUBMIT. */
int  gui_field_key(gui_widget_t *w, int ch);
void gui_field_clear(gui_widget_t *w);

/* ====================================================================== */
/* theme and metrics                                                      */
/* ====================================================================== */

typedef struct gui_theme {
    fb_color_t frame,        frame_focus;
    fb_color_t title_bg,     title_bg_focus;
    fb_color_t title_fg,     title_fg_focus;
    fb_color_t client;
    fb_color_t close_fg;
    fb_color_t btn_bg, btn_bg_press, btn_edge, btn_fg, btn_fg_disabled;
    fb_color_t field_bg, field_fg, field_edge, caret;
    fb_color_t label_fg;
    fb_color_t shadow;
} gui_theme_t;

const gui_theme_t *gui_theme(void);
/* NULL restores the built-in theme. The pointer is stored, not copied, so it
 * must outlive the GUI — in practice a static const in an app. */
void gui_set_theme(const gui_theme_t *t);

/* Chrome geometry, in pixels. Not configurable: the title bar has to hold one
 * 16-pixel glyph row and the close box has to be big enough to hit. */
#define GUI_BORDER    1
#define GUI_TITLE_H   18
#define GUI_CLOSE_W   13     /* the close box, inside the title bar          */
#define GUI_PAD       4      /* text inset inside a field/label              */

/* The smallest window that still has a usable title bar and one row of client.
 * gui_window_open() clamps to these rather than refusing, because a model
 * choosing a size is going to choose 10x10 eventually. */
#define GUI_MIN_W     64
#define GUI_MIN_H     (GUI_TITLE_H + 2 * GUI_BORDER + 8)

/* ====================================================================== */
/* painting                                                               */
/* ====================================================================== */

/* Pixel width of utf8[0..n) in the GUI font. The one place a fixed advance is
 * assumed; everything measures through here. */
int32_t gui_text_width(const char *utf8, size_t n);

/* Draw text inside `box`, aligned, clipped to the current clip AND to the box.
 * Text longer than the box is clipped on the side the alignment implies — a
 * right-aligned number loses its most significant digits off the left, which is
 * exactly what a calculator display must NOT do silently, so the caller checks
 * gui_text_width() first. Returns the x the text actually started at. */
int32_t gui_paint_text(fb_surface_t *s, gui_rect_t box, const char *utf8,
                       uint8_t align, fb_color_t fg);

/* Paint one widget. `origin` is the widget's client-space rectangle translated
 * into the surface's coordinates (i.e. the window's client origin added), so the
 * same function paints into the screen or into a test's array with no notion of
 * which. The caller sets the clip; this never widens it. */
void gui_paint_widget(fb_surface_t *s, const gui_widget_t *w,
                      int32_t origin_x, int32_t origin_y, int focused_window);

#endif /* WIDGETS_H */
