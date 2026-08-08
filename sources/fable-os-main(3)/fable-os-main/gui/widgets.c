/* widgets.c — rectangles, layout arithmetic, and the four controls.
 *
 * See include/widgets.h for the contract and the reasoning. This file is pure
 * arithmetic and pixels: no static state except the theme pointer, no heap, no
 * hardware, no console. Every function here is called directly by
 * tests/host/test_gui.c against a malloc'd surface.
 *
 * TWO RULES THAT THE WHOLE FILE OBEYS
 *   1. A rectangle with a non-positive w or h is EMPTY, and an empty rectangle is
 *      never computed with — it is returned. That is what stops a bad layout from
 *      becoming a negative width somewhere three functions away.
 *   2. Nothing here decides whether it is allowed to draw. Clipping is fb.h's
 *      job on every single call, so a widget that hangs off its window, or a
 *      window off the screen, costs a clipped blit and not a bounds check. The
 *      caller narrows the clip; this file never widens it.
 */

#include "gui.h"
#include "widgets.h"

/* ====================================================================== */
/* rectangles                                                             */
/* ====================================================================== */

int gui_rect_empty(gui_rect_t r) { return r.w <= 0 || r.h <= 0; }

int gui_rect_contains(gui_rect_t r, int32_t x, int32_t y) {
    if (gui_rect_empty(r)) return 0;
    /* Half-open: the right and bottom edges belong to the next rectangle. Done
     * in int64 so a frame at INT32_MAX-ish coordinates cannot wrap into a
     * "contains" that isn't. */
    int64_t x1 = (int64_t)r.x + r.w, y1 = (int64_t)r.y + r.h;
    return (int64_t)x >= r.x && (int64_t)x < x1 &&
           (int64_t)y >= r.y && (int64_t)y < y1;
}

gui_rect_t gui_rect_intersect(gui_rect_t a, gui_rect_t b) {
    if (gui_rect_empty(a) || gui_rect_empty(b)) return gui_rect(0, 0, 0, 0);
    int64_t x0 = a.x > b.x ? a.x : b.x;
    int64_t y0 = a.y > b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.w, bx1 = (int64_t)b.x + b.w;
    int64_t ay1 = (int64_t)a.y + a.h, by1 = (int64_t)b.y + b.h;
    int64_t x1 = ax1 < bx1 ? ax1 : bx1;
    int64_t y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0) return gui_rect(0, 0, 0, 0);
    return gui_rect((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

int gui_rect_intersects(gui_rect_t a, gui_rect_t b) {
    return !gui_rect_empty(gui_rect_intersect(a, b));
}

gui_rect_t gui_rect_union(gui_rect_t a, gui_rect_t b) {
    if (gui_rect_empty(a)) return gui_rect_empty(b) ? gui_rect(0, 0, 0, 0) : b;
    if (gui_rect_empty(b)) return a;
    int64_t x0 = a.x < b.x ? a.x : b.x;
    int64_t y0 = a.y < b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.w, bx1 = (int64_t)b.x + b.w;
    int64_t ay1 = (int64_t)a.y + a.h, by1 = (int64_t)b.y + b.h;
    int64_t x1 = ax1 > bx1 ? ax1 : bx1;
    int64_t y1 = ay1 > by1 ? ay1 : by1;
    return gui_rect((int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
}

gui_rect_t gui_inset(gui_rect_t r, int32_t dx, int32_t dy) {
    if (gui_rect_empty(r)) return gui_rect(0, 0, 0, 0);
    int64_t w = (int64_t)r.w - 2 * (int64_t)dx;
    int64_t h = (int64_t)r.h - 2 * (int64_t)dy;
    if (w <= 0 || h <= 0) return gui_rect(0, 0, 0, 0);
    return gui_rect(r.x + dx, r.y + dy, (int32_t)w, (int32_t)h);
}

/* ====================================================================== */
/* layout                                                                 */
/* ====================================================================== */

gui_rect_t gui_grid(gui_rect_t area, int rows, int cols,
                    int row, int col, int rowspan, int colspan, int gap) {
    gui_rect_t none = gui_rect(0, 0, 0, 0);
    if (gui_rect_empty(area)) return none;
    if (rows <= 0 || cols <= 0 || gap < 0) return none;
    if (row < 0 || col < 0 || rowspan < 1 || colspan < 1) return none;
    if (row + rowspan > rows || col + colspan > cols) return none;

    int64_t availw = (int64_t)area.w - (int64_t)gap * (cols - 1);
    int64_t availh = (int64_t)area.h - (int64_t)gap * (rows - 1);
    if (availw <= 0 || availh <= 0) return none;

    /* Edges as (avail * i) / n: adjacent cells share an edge exactly, widths
     * differ by at most one pixel, and the last edge is exactly `avail`. */
    int64_t x0 = availw * col / cols;
    int64_t x1 = availw * (col + colspan) / cols;
    int64_t y0 = availh * row / rows;
    int64_t y1 = availh * (row + rowspan) / rows;

    int64_t w = (x1 - x0) + (int64_t)gap * (colspan - 1);
    int64_t h = (y1 - y0) + (int64_t)gap * (rowspan - 1);
    if (w <= 0 || h <= 0) return none;

    return gui_rect((int32_t)(area.x + x0 + (int64_t)gap * col),
                    (int32_t)(area.y + y0 + (int64_t)gap * row),
                    (int32_t)w, (int32_t)h);
}

/* One implementation, four orientations: `horiz` splits on x, `far` takes the
 * high edge. Keeping them one function is what makes the four agree. */
static gui_rect_t split(gui_rect_t area, int32_t n, int32_t gap,
                        gui_rect_t *rest, int horiz, int far) {
    gui_rect_t none = gui_rect(0, 0, 0, 0);
    int32_t    total = horiz ? area.w : area.h;

    if (gui_rect_empty(area) || n <= 0 || gap < 0) {
        if (rest) *rest = gui_rect_empty(area) ? none : area;
        return none;
    }
    if (n >= total) { if (rest) *rest = none; return area; }

    int32_t left = total - n - gap;              /* what is left after the gap */
    gui_rect_t a = area, b = area;
    if (horiz) {
        a.w = n; b.w = left;
        if (far) { a.x = area.x + total - n; b.x = area.x; }
        else     { b.x = area.x + n + gap; }
    } else {
        a.h = n; b.h = left;
        if (far) { a.y = area.y + total - n; b.y = area.y; }
        else     { b.y = area.y + n + gap; }
    }
    if (rest) *rest = (left > 0) ? b : none;
    return a;
}

gui_rect_t gui_split_top(gui_rect_t a, int32_t n, int32_t g, gui_rect_t *rest) {
    return split(a, n, g, rest, 0, 0);
}
gui_rect_t gui_split_bottom(gui_rect_t a, int32_t n, int32_t g, gui_rect_t *rest) {
    return split(a, n, g, rest, 0, 1);
}
gui_rect_t gui_split_left(gui_rect_t a, int32_t n, int32_t g, gui_rect_t *rest) {
    return split(a, n, g, rest, 1, 0);
}
gui_rect_t gui_split_right(gui_rect_t a, int32_t n, int32_t g, gui_rect_t *rest) {
    return split(a, n, g, rest, 1, 1);
}

gui_rect_t gui_center(gui_rect_t area, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return gui_rect(0, 0, 0, 0);
    return gui_rect(area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h);
}

/* ====================================================================== */
/* theme                                                                  */
/* ====================================================================== */

/* Dark, because the console behind it is black and a light theme would make
 * every window a lamp. The field colour is deliberately the green of a desk
 * calculator's display: it reads as a readout rather than as an input box. */
static const gui_theme_t default_theme = {
    .frame           = FB_RGB(0x2a, 0x2a, 0x34),
    .frame_focus     = FB_RGB(0x4c, 0x7c, 0xd0),
    .title_bg        = FB_RGB(0x33, 0x33, 0x3e),
    .title_bg_focus  = FB_RGB(0x2c, 0x5c, 0xa8),
    .title_fg        = FB_RGB(0xa8, 0xa8, 0xb4),
    .title_fg_focus  = FB_RGB(0xff, 0xff, 0xff),
    .client          = FB_RGB(0x1e, 0x1e, 0x26),
    .close_fg        = FB_RGB(0xe4, 0xe4, 0xec),
    .btn_bg          = FB_RGB(0x36, 0x36, 0x44),
    .btn_bg_press    = FB_RGB(0x5c, 0x7c, 0xb8),
    .btn_edge        = FB_RGB(0x52, 0x52, 0x62),
    .btn_fg          = FB_RGB(0xec, 0xec, 0xf4),
    .btn_fg_disabled = FB_RGB(0x6e, 0x6e, 0x7a),
    .field_bg        = FB_RGB(0x0c, 0x14, 0x0c),
    .field_fg        = FB_RGB(0x8c, 0xe8, 0x8c),
    .field_edge      = FB_RGB(0x4a, 0x5a, 0x4a),
    .caret           = FB_RGB(0xff, 0xff, 0xff),
    .label_fg        = FB_RGB(0xc8, 0xc8, 0xd4),
    .shadow          = FB_RGB(0x00, 0x00, 0x00),
};

static const gui_theme_t *theme = &default_theme;

const gui_theme_t *gui_theme(void) { return theme; }
void gui_set_theme(const gui_theme_t *t) { theme = t ? t : &default_theme; }

/* ====================================================================== */
/* widget construction                                                    */
/* ====================================================================== */

static gui_widget_t *alloc_widget(gui_window_t *win, uint8_t kind, gui_rect_t r) {
    if (!win || !win->used) return (gui_widget_t *)0;
    if (win->nwidgets >= GUI_MAX_WIDGETS) return (gui_widget_t *)0;

    gui_widget_t *w = &win->widgets[win->nwidgets++];
    /* Zero it rather than trusting the previous tenant: a window can be closed
     * and its slot reused, and a stale on_event pointer is a jump into an app
     * that no longer exists. */
    for (size_t i = 0; i < sizeof *w; i++) ((volatile uint8_t *)w)[i] = 0;

    w->kind  = kind;
    w->r     = r;
    w->align = GUI_ALIGN_LEFT;
    w->fg    = theme->label_fg;
    w->bg    = theme->client;
    return w;
}

gui_widget_t *gui_add_panel(gui_window_t *win, gui_rect_t r, fb_color_t bg,
                            uint8_t state) {
    gui_widget_t *w = alloc_widget(win, GUI_PANEL, r);
    if (!w) return w;
    w->bg    = bg;
    w->fg    = theme->btn_edge;
    /* Panels are scenery. If one could be hit, a background panel would swallow
     * every click aimed at the buttons drawn on top of it. */
    w->state = (uint8_t)(state | GUI_W_NOHIT);
    return w;
}

gui_widget_t *gui_add_label(gui_window_t *win, gui_rect_t r, const char *text,
                            uint8_t align) {
    gui_widget_t *w = alloc_widget(win, GUI_LABEL, r);
    if (!w) return w;
    w->align = align > GUI_ALIGN_RIGHT ? GUI_ALIGN_LEFT : align;
    w->fg    = theme->label_fg;
    w->state = GUI_W_NOHIT;
    gui_set_text(w, text);
    return w;
}

gui_widget_t *gui_add_button(gui_window_t *win, gui_rect_t r, const char *text,
                             uint32_t id) {
    gui_widget_t *w = alloc_widget(win, GUI_BUTTON, r);
    if (!w) return w;
    w->id    = id;
    w->align = GUI_ALIGN_CENTER;
    w->fg    = theme->btn_fg;
    w->bg    = theme->btn_bg;
    gui_set_text(w, text);
    return w;
}

gui_widget_t *gui_add_field(gui_window_t *win, gui_rect_t r, const char *text,
                            uint32_t id, uint8_t state) {
    gui_widget_t *w = alloc_widget(win, GUI_FIELD, r);
    if (!w) return w;
    w->id    = id;
    w->align = GUI_ALIGN_LEFT;
    w->fg    = theme->field_fg;
    w->bg    = theme->field_bg;
    w->state = state;
    size_t n = gui_set_text(w, text);
    w->caret = (uint16_t)n;
    return w;
}

/* ====================================================================== */
/* lookup                                                                 */
/* ====================================================================== */

gui_widget_t *gui_widget_by_id(gui_window_t *win, uint32_t id) {
    if (!win || !win->used || id == 0) return (gui_widget_t *)0;
    for (uint16_t i = 0; i < win->nwidgets; i++)
        if (win->widgets[i].id == id) return &win->widgets[i];
    return (gui_widget_t *)0;
}

gui_widget_t *gui_widget_at(gui_window_t *win, int32_t cx, int32_t cy) {
    if (!win || !win->used) return (gui_widget_t *)0;
    /* Painted first to last, so hit last to first: the widget drawn on top is
     * the one the operator sees and therefore the one that is clicked. */
    for (int i = (int)win->nwidgets - 1; i >= 0; i--) {
        gui_widget_t *w = &win->widgets[i];
        if (w->state & (GUI_W_NOHIT | GUI_W_DISABLED)) continue;
        if (gui_rect_contains(w->r, cx, cy)) return w;
    }
    return (gui_widget_t *)0;
}

int gui_widget_index(const gui_window_t *win, const gui_widget_t *w) {
    if (!win || !w) return -1;
    for (uint16_t i = 0; i < win->nwidgets; i++)
        if (&win->widgets[i] == w) return (int)i;
    return -1;
}

/* ====================================================================== */
/* text                                                                   */
/* ====================================================================== */

size_t gui_set_text(gui_widget_t *w, const char *s) {
    if (!w) return 0;
    size_t n = 0;
    if (s) {
        while (s[n] && n < GUI_TEXT_MAX - 1) { w->text[n] = s[n]; n++; }
    }
    w->text[n] = '\0';
    if (w->caret > n) w->caret = (uint16_t)n;
    return n;
}

size_t gui_append_text(gui_widget_t *w, const char *s) {
    if (!w || !s) return 0;
    size_t n = 0;
    while (w->text[n] && n < GUI_TEXT_MAX - 1) n++;
    size_t added = 0;
    while (s[added] && n < GUI_TEXT_MAX - 1) w->text[n++] = s[added++];
    w->text[n] = '\0';
    return added;
}

const char *gui_text(const gui_widget_t *w) { return w ? w->text : ""; }

void gui_set_state(gui_widget_t *w, uint8_t set, uint8_t clear) {
    if (!w) return;
    w->state = (uint8_t)((w->state & (uint8_t)~clear) | set);
}

void gui_enable(gui_widget_t *w, int on) {
    if (!w) return;
    if (on) w->state &= (uint8_t)~GUI_W_DISABLED;
    else    w->state |= GUI_W_DISABLED;
}

/* ====================================================================== */
/* field editing                                                          */
/* ====================================================================== */

int gui_field_key(gui_widget_t *w, int ch) {
    if (!w || w->kind != GUI_FIELD) return -1;
    if (w->state & (GUI_W_READONLY | GUI_W_DISABLED)) return -1;

    size_t len = 0;
    while (w->text[len]) len++;
    if (w->caret > len) w->caret = (uint16_t)len;

    if (ch == '\b' || ch == 0x7F) {
        if (w->caret == 0) return 0;
        for (size_t i = w->caret - 1; i < len; i++) w->text[i] = w->text[i + 1];
        w->caret--;
        return 1;
    }
    /* Printable ASCII only. A field is drawn from a 256-slot code page in the
     * console and from the full font here, but accepting a raw byte >= 0x80 out
     * of a keyboard would insert half a UTF-8 sequence and render as a
     * replacement box — visibly wrong, and not what the operator typed. */
    if (ch < 0x20 || ch > 0x7E) return 0;
    if (len + 1 >= GUI_TEXT_MAX) return 0;

    for (size_t i = len + 1; i > w->caret; i--) w->text[i] = w->text[i - 1];
    w->text[w->caret++] = (char)ch;
    return 1;
}

void gui_field_clear(gui_widget_t *w) {
    if (!w) return;
    w->text[0] = '\0';
    w->caret   = 0;
}

/* ====================================================================== */
/* painting                                                               */
/* ====================================================================== */

int32_t gui_text_width(const char *utf8, size_t n) {
    if (!utf8 || n == 0) return 0;
    return (int32_t)fb_utf8_columns(utf8, n) * font_8x16.width;
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int32_t gui_paint_text(fb_surface_t *s, gui_rect_t box, const char *utf8,
                       uint8_t align, fb_color_t fg) {
    if (!s || gui_rect_empty(box) || !utf8 || !utf8[0]) return box.x;

    size_t  n = slen(utf8);
    int32_t tw = gui_text_width(utf8, n);
    int32_t x  = box.x;
    if (align == GUI_ALIGN_CENTER) x = box.x + (box.w - tw) / 2;
    else if (align == GUI_ALIGN_RIGHT) x = box.x + box.w - tw;

    int32_t y = box.y + (box.h - font_8x16.height) / 2;

    /* Narrow the clip to the box so text can overflow without escaping it, then
     * put the caller's clip back: this function must not leak a clip change. */
    int32_t cx, cy, cw, ch;
    fb_clip_get(s, &cx, &cy, &cw, &ch);
    if (fb_clip_set(s, box.x, box.y, box.w, box.h) == 0)
        fb_draw_utf8(s, &font_8x16, x, y, utf8, n, fg, 0, 0);
    fb_clip_reset(s);
    fb_clip_set(s, cx, cy, cw, ch);
    return x;
}

/* A field shows the tail of an over-long value when it is right-aligned (which
 * is what a calculator display wants) and follows the caret when it is left-
 * aligned and being typed into. Both are the same computation: an x offset. */
static int32_t field_text_x(gui_rect_t inner, const gui_widget_t *w, size_t n) {
    int32_t tw = gui_text_width(w->text, n);
    if (w->align == GUI_ALIGN_RIGHT)  return inner.x + inner.w - tw;
    if (w->align == GUI_ALIGN_CENTER) return inner.x + (inner.w - tw) / 2;

    int32_t caret_x = gui_text_width(w->text, w->caret);
    int32_t shift   = caret_x - inner.w;      /* caret past the right edge? */
    if (shift < 0) shift = 0;
    return inner.x - shift;
}

void gui_paint_widget(fb_surface_t *s, const gui_widget_t *w,
                      int32_t origin_x, int32_t origin_y, int focused_window) {
    if (!s || !w) return;

    gui_rect_t r = gui_rect(origin_x + w->r.x, origin_y + w->r.y, w->r.w, w->r.h);
    if (gui_rect_empty(r)) return;

    int disabled = (w->state & GUI_W_DISABLED) != 0;

    switch (w->kind) {
    case GUI_PANEL:
        fb_fill_rect(s, r.x, r.y, r.w, r.h, w->bg);
        if (w->state & GUI_W_BORDER)
            fb_frame_rect(s, r.x, r.y, r.w, r.h, 1, w->fg);
        break;

    case GUI_LABEL:
        if (w->state & GUI_W_OPAQUE)
            fb_fill_rect(s, r.x, r.y, r.w, r.h, w->bg);
        if (w->state & GUI_W_BORDER)
            fb_frame_rect(s, r.x, r.y, r.w, r.h, 1, theme->btn_edge);
        gui_paint_text(s, gui_inset(r, 2, 0), w->text, w->align,
                       disabled ? theme->btn_fg_disabled : w->fg);
        break;

    case GUI_BUTTON: {
        int pressed = (w->state & GUI_W_PRESSED) != 0;
        fb_fill_rect(s, r.x, r.y, r.w, r.h,
                     pressed ? theme->btn_bg_press : w->bg);
        fb_frame_rect(s, r.x, r.y, r.w, r.h, 1, theme->btn_edge);
        /* A pressed key moves its label one pixel down and right. It is the
         * cheapest possible "this went in" and it survives being photographed,
         * which a colour change alone does not. */
        gui_rect_t t = gui_inset(r, 2, 0);
        if (pressed) { t.x += 1; t.y += 1; }
        gui_paint_text(s, t, w->text, w->align,
                       disabled ? theme->btn_fg_disabled : w->fg);
        break;
    }

    case GUI_FIELD: {
        fb_fill_rect(s, r.x, r.y, r.w, r.h, w->bg);
        fb_frame_rect(s, r.x, r.y, r.w, r.h, 1, theme->field_edge);
        gui_rect_t inner = gui_inset(r, GUI_PAD, 2);
        if (gui_rect_empty(inner)) break;

        size_t  n = slen(w->text);
        int32_t x = field_text_x(inner, w, n);
        int32_t y = inner.y + (inner.h - font_8x16.height) / 2;

        int32_t cx, cy, cw, ch;
        fb_clip_get(s, &cx, &cy, &cw, &ch);
        if (fb_clip_set(s, inner.x, inner.y, inner.w, inner.h) == 0) {
            if (n) fb_draw_utf8(s, &font_8x16, x, y, w->text, n,
                                disabled ? theme->btn_fg_disabled : w->fg, 0, 0);
            if (focused_window && (w->state & GUI_W_FOCUS) &&
                !(w->state & (GUI_W_READONLY | GUI_W_DISABLED))) {
                int32_t caret_x = x + gui_text_width(w->text, w->caret);
                fb_fill_rect(s, caret_x, y, 1, font_8x16.height, theme->caret);
            }
        }
        fb_clip_reset(s);
        fb_clip_set(s, cx, cy, cw, ch);
        break;
    }

    default:
        break;
    }
}
