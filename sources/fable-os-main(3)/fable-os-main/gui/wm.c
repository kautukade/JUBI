/* wm.c — the window manager: a static window pool, z-order, focus, damage
 *        tracking, a compositor over the text console, and one bounded tick.
 *
 * See include/gui.h for the four decisions this file implements (the console is
 * the desktop; damage, not frames; one cooperative loop; the pointer is off
 * until a window exists). What follows is how, and the invariants that keep it
 * honest.
 *
 * THE FOUR PIECES OF STATE, AND WHO MAY CHANGE THEM
 *   pool[]        the windows. A window's identity is its `id`, never its slot:
 *                 ids are allocated monotonically and never reused within a
 *                 boot, so a stale id from a tool call five turns ago resolves
 *                 to "no such window" instead of to whatever moved in.
 *   z[]           slot indices, bottom first. This is the ONLY z-order; there
 *                 is no `z` field to disagree with it.
 *   damage[]      rectangles that are wrong on screen. Always a SUPERSET of what
 *                 changed — over-damaging is slow, under-damaging is a visual
 *                 bug you cannot see in a screenshot taken at the wrong moment.
 *   shadow[]      this module's copy of the console's cell grid, which is how it
 *                 notices that lib/base.c drew text where a window is.
 *
 * THE COMPOSITE, TOP TO BOTTOM, FOR ONE DAMAGE RECTANGLE
 *   1. the console's cells, rasterised here from console_fb() with the same font
 *      and the same 16-colour palette lib/base.c uses;
 *   2. every visible window that intersects, bottom to top, each clipped to its
 *      own frame so a window can never paint a pixel outside itself;
 *   3. the pointer.
 *   then one fb_present_rect() of exactly that rectangle. Windows are opaque, so
 *   painting bottom-to-top is the whole of z-order — there is no occlusion
 *   arithmetic to get wrong.
 *
 * WHY CONSOLE DAMAGE IS INTERSECTED WITH THE WINDOWS
 *   When lib/base.c prints a character it draws that cell itself, correctly.
 *   Outside every window and away from the pointer, the screen is therefore
 *   already right and repainting it would be pure waste. So a console change is
 *   damage only where a window or the pointer covers it. With the prompt at the
 *   bottom of the screen and a calculator in the middle, typing costs nothing.
 */

#include "gui.h"
#include "widgets.h"
#include "fb.h"
#include "font.h"

#ifndef FABLEOS_HOSTTEST
#include "kernel.h"
#endif

/* ====================================================================== */
/* state                                                                  */
/* ====================================================================== */

/* The console grid this module can shadow, bounded by the largest mode lib/fb.c
 * will drive (160x64 cells of 8x16 at 1280x1024). */
#define GRID_MAX_COLS (FB_MAX_WIDTH  / 8)
#define GRID_MAX_ROWS (FB_MAX_HEIGHT / 16)

/* A merge that repaints this many redundant pixels is still cheaper than a
 * second walk of the window list and a second fb_present_rect(). */
#define DAMAGE_MERGE_SLACK 4096

static fb_surface_t      *dst;
static volatile uint16_t *grid;
static int                grows, gcols;
static uint16_t           shadow[GRID_MAX_ROWS * GRID_MAX_COLS];
static int                last_cur_row, last_cur_col;

static gui_window_t pool[GUI_MAX_WINDOWS];
static uint8_t      z[GUI_MAX_WINDOWS];
static int          nz;
static uint32_t     next_id = 1;
static uint32_t     focus_id;

static gui_rect_t damage[GUI_MAX_DAMAGE];
static int        ndamage;

static int32_t ptr_x, ptr_y;
static int     ptr_visible;

static int      dragging;
static uint32_t drag_id;
static int32_t  drag_dx, drag_dy;

static uint32_t press_win;        /* window whose widget is held               */
static int      press_idx = -1;   /* index into that window's widgets          */
static uint32_t press_close;      /* window whose close box is held            */

/* The one-line keyboard grab (gui.h, DECISION 3). */
static uint32_t grab_win;
static int      grab_idx = -1;

static int      pointer_announced;
static int      closing_all;      /* gui_close_all(): a veto may not stand   */
static int      consumed;         /* set by the press/release handlers         */

static gui_stats_t st;

const gui_stats_t *gui_stats(void) { return &st; }

/* ---- the console's cursor and its scroll counter -------------------------
 * In the kernel both come from lib/base.c; on the host a test supplies them,
 * which is what makes the scroll case assertable without a console at all. */
#ifdef FABLEOS_HOSTTEST
static int      host_cur_row, host_cur_col;
static uint64_t host_scrolls;
void gui_test_set_console_cursor(int row, int col) {
    host_cur_row = row;
    host_cur_col = col;
}
void gui_test_console_scrolled(void) { host_scrolls++; }
static void     con_cursor(int *r, int *c) { *r = host_cur_row; *c = host_cur_col; }
static uint64_t con_scrolls(void) { return host_scrolls; }
#else
static void     con_cursor(int *r, int *c) { console_cursor(r, c); }
static uint64_t con_scrolls(void) { return console_scrolls(); }
#endif

/* ====================================================================== */
/* small helpers                                                          */
/* ====================================================================== */

static gui_rect_t screen_rect(void) {
    if (!dst) return gui_rect(0, 0, 0, 0);
    return gui_rect(0, 0, dst->w, dst->h);
}

void gui_screen_size(int32_t *w, int32_t *h) {
    if (w) *w = dst ? dst->w : 0;
    if (h) *h = dst ? dst->h : 0;
}

int gui_attached(void) { return dst != (fb_surface_t *)0; }

static gui_window_t *slot_of(uint32_t id) {
    if (!id) return (gui_window_t *)0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (pool[i].used && pool[i].id == id) return &pool[i];
    return (gui_window_t *)0;
}

gui_window_t *gui_window(uint32_t id) { return slot_of(id); }

int gui_window_count(void) { return nz; }

gui_window_t *gui_window_at_z(int index) {
    if (index < 0 || index >= nz) return (gui_window_t *)0;
    return &pool[z[index]];
}

uint32_t gui_focused(void) { return focus_id; }

static int visible(const gui_window_t *w) {
    return w->used && !(w->flags & GUI_WIN_HIDDEN);
}

int gui_active(void) {
    for (int i = 0; i < nz; i++)
        if (visible(&pool[z[i]])) return 1;
    return 0;
}

gui_rect_t gui_frame_rect(const gui_window_t *w) {
    return w ? w->frame : gui_rect(0, 0, 0, 0);
}

static gui_rect_t title_rect(const gui_window_t *w) {
    if (!w) return gui_rect(0, 0, 0, 0);
    return gui_rect(w->frame.x + GUI_BORDER, w->frame.y + GUI_BORDER,
                    w->frame.w - 2 * GUI_BORDER, GUI_TITLE_H);
}

static gui_rect_t close_rect(const gui_window_t *w) {
    if (!w || (w->flags & GUI_WIN_NOCLOSE)) return gui_rect(0, 0, 0, 0);
    gui_rect_t t = title_rect(w);
    if (t.w < GUI_CLOSE_W + 6 || t.h < GUI_CLOSE_W) return gui_rect(0, 0, 0, 0);
    return gui_rect(t.x + t.w - GUI_CLOSE_W - 3,
                    t.y + (t.h - GUI_CLOSE_W) / 2, GUI_CLOSE_W, GUI_CLOSE_W);
}

gui_rect_t gui_client_rect(const gui_window_t *w) {
    if (!w) return gui_rect(0, 0, 0, 0);
    int32_t x = w->frame.x + GUI_BORDER;
    int32_t y = w->frame.y + GUI_BORDER + GUI_TITLE_H;
    int32_t cw = w->frame.w - 2 * GUI_BORDER;
    int32_t chh = w->frame.h - 2 * GUI_BORDER - GUI_TITLE_H;
    if (cw < 0) cw = 0;
    if (chh < 0) chh = 0;
    return gui_rect(x, y, cw, chh);
}

gui_rect_t gui_client_area(const gui_window_t *w) {
    gui_rect_t c = gui_client_rect(w);
    return gui_rect(0, 0, c.w, c.h);
}

/* ====================================================================== */
/* damage                                                                 */
/* ====================================================================== */

static int64_t area_of(gui_rect_t r) {
    if (gui_rect_empty(r)) return 0;
    return (int64_t)r.w * (int64_t)r.h;
}

static void damage_rect(gui_rect_t r) {
    if (!dst) return;
    r = gui_rect_intersect(r, screen_rect());
    if (gui_rect_empty(r)) return;

    for (int i = 0; i < ndamage; i++) {
        gui_rect_t u = gui_rect_union(damage[i], r);
        if (area_of(u) <= area_of(damage[i]) + area_of(r) + DAMAGE_MERGE_SLACK) {
            damage[i] = u;
            st.merges++;
            return;
        }
    }
    if (ndamage < GUI_MAX_DAMAGE) { damage[ndamage++] = r; return; }

    /* Full: collapse to the bounding box. Still a superset, which is the only
     * property anything downstream relies on. */
    gui_rect_t u = r;
    for (int i = 0; i < ndamage; i++) u = gui_rect_union(u, damage[i]);
    damage[0] = u;
    ndamage   = 1;
    st.collapses++;
}

void gui_invalidate(int32_t x, int32_t y, int32_t w, int32_t h) {
    damage_rect(gui_rect(x, y, w, h));
}

void gui_invalidate_window(uint32_t id) {
    gui_window_t *w = slot_of(id);
    if (w) damage_rect(w->frame);
}

void gui_invalidate_widget(uint32_t id, const gui_widget_t *wd) {
    gui_window_t *w = slot_of(id);
    if (!w || !wd) return;
    gui_rect_t c = gui_client_rect(w);
    damage_rect(gui_rect_intersect(
        gui_rect(c.x + wd->r.x, c.y + wd->r.y, wd->r.w, wd->r.h), c));
}

void gui_invalidate_all(void) {
    ndamage = 0;
    damage_rect(screen_rect());
}

int gui_damage_count(void) { return ndamage; }

gui_rect_t gui_damage_rect(int index) {
    if (index < 0 || index >= ndamage) return gui_rect(0, 0, 0, 0);
    return damage[index];
}

/* ====================================================================== */
/* the pointer                                                            */
/* ====================================================================== */

/* 11x17, hotspot at (0,0). 'X' outline, '#' fill, ' ' transparent. Written as
 * text so it can be read and edited; the cost is one strcmp-free character
 * compare per pixel, once per damaged cursor rectangle. */
#define CURSOR_W 11
#define CURSOR_H 17
static const char *const cursor_bits[CURSOR_H] = {
    "X          ",
    "XX         ",
    "X#X        ",
    "X##X       ",
    "X###X      ",
    "X####X     ",
    "X#####X    ",
    "X######X   ",
    "X#######X  ",
    "X########X ",
    "X#####XXXXX",
    "X##X###X   ",
    "X#X X##X   ",
    "XX  X##X   ",
    "X    X##X  ",
    "     X##X  ",
    "      XX   ",
};

static gui_rect_t cursor_rect(void) {
    return gui_rect(ptr_x, ptr_y, CURSOR_W, CURSOR_H);
}

/* Hiding the pointer also STOPS THE HARDWARE STREAM, and that is the point of
 * routing every change through here rather than assigning ptr_visible directly.
 *
 * mouse_poll() enables data reporting on first use and nothing ever disabled it
 * again outside driver suspend, so the aux device kept streaming for the rest of
 * the boot even with every window closed and the pointer hidden. That is bytes
 * arriving at a shared i8042 output buffer that nobody is draining — harmless now
 * that drivers/input/kbd.c forwards aux bytes to the decoder instead of reading
 * them as scancodes, but "harmless because the other driver is careful" is not
 * where a bound belongs. A device nobody is listening to should be quiet. */
static void set_pointer_visible(int v) {
    v = !!v;
    if (v == !!ptr_visible) return;
    ptr_visible = v;
#ifndef FABLEOS_HOSTTEST
    if (!v && mouse_present()) (void)mouse_stop();
#endif
    damage_rect(cursor_rect());
}

void gui_pointer_show(int v) { set_pointer_visible(v); }

int  gui_pointer_visible(void) { return ptr_visible; }

void gui_pointer_position(int32_t *x, int32_t *y) {
    if (x) *x = ptr_x;
    if (y) *y = ptr_y;
}

void gui_pointer_warp(int32_t x, int32_t y) {
    if (x == ptr_x && y == ptr_y) return;
    if (ptr_visible) damage_rect(cursor_rect());
    ptr_x = x;
    ptr_y = y;
    if (ptr_visible) damage_rect(cursor_rect());
}

/* ====================================================================== */
/* painting                                                               */
/* ====================================================================== */

static void clip_to(gui_rect_t r) {
    fb_clip_reset(dst);
    fb_clip_set(dst, r.x, r.y, r.w, r.h);
}

/* The desktop: the console's own cells, drawn with the console's own font and
 * palette. Only the cells intersecting `r` are rasterised. */
static void paint_backdrop(gui_rect_t r) {
    clip_to(r);
    fb_fill_rect(dst, r.x, r.y, r.w, r.h, fb_vga_palette[0]);
    if (!grid || grows <= 0 || gcols <= 0) return;

    int gw = font_8x16.width, gh = font_8x16.height;
    int c0 = r.x / gw, c1 = (r.x + r.w - 1) / gw;
    int r0 = r.y / gh, r1 = (r.y + r.h - 1) / gh;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 > gcols - 1) c1 = gcols - 1;
    if (r1 > grows - 1) r1 = grows - 1;

    for (int row = r0; row <= r1; row++) {
        for (int col = c0; col <= c1; col++) {
            uint16_t cell = grid[(size_t)row * (size_t)gcols + (size_t)col];
            uint8_t  slot = (uint8_t)(cell & 0xFF);
            uint8_t  attr = (uint8_t)(cell >> 8);
            fb_draw_glyph(dst, &font_8x16, col * gw, row * gh,
                          (int)font_page_glyph(slot),
                          fb_vga_palette[attr & 0x0F],
                          fb_vga_palette[(attr >> 4) & 0x0F], 1);
        }
    }
}

static void paint_close_box(gui_rect_t b, int focused) {
    const gui_theme_t *t = gui_theme();
    fb_frame_rect(dst, b.x, b.y, b.w, b.h, 1,
                  focused ? t->title_fg_focus : t->title_fg);
    /* Two diagonals inside the box, drawn by hand: an 'x' glyph in an 8x16 cell
     * is not square and reads as a letter. */
    fb_color_t c = focused ? t->title_fg_focus : t->title_fg;
    for (int32_t i = 3; i < b.w - 3; i++) {
        fb_put_pixel(dst, b.x + i, b.y + i, c);
        fb_put_pixel(dst, b.x + i, b.y + b.h - 1 - i, c);
    }
}

static void paint_window(gui_window_t *w, gui_rect_t area) {
    const gui_theme_t *t = gui_theme();
    int focused = (w->id == focus_id);

    clip_to(area);
    if (fb_clip_set(dst, w->frame.x, w->frame.y, w->frame.w, w->frame.h) != 0)
        return;                       /* nothing of this window is in `area` */

    /* Frame and border. */
    fb_fill_rect(dst, w->frame.x, w->frame.y, w->frame.w, w->frame.h, t->client);
    fb_frame_rect(dst, w->frame.x, w->frame.y, w->frame.w, w->frame.h,
                  GUI_BORDER, focused ? t->frame_focus : t->frame);

    /* Title bar. */
    gui_rect_t tb = title_rect(w);
    fb_fill_rect(dst, tb.x, tb.y, tb.w, tb.h,
                 focused ? t->title_bg_focus : t->title_bg);
    gui_rect_t cb = close_rect(w);
    gui_rect_t tt = tb;
    if (!gui_rect_empty(cb)) tt.w = cb.x - tb.x - 2;
    gui_paint_text(dst, gui_inset(tt, 5, 0), w->title, GUI_ALIGN_LEFT,
                   focused ? t->title_fg_focus : t->title_fg);
    if (!gui_rect_empty(cb)) paint_close_box(cb, focused);

    /* Client area and widgets. Narrowing the clip here is what makes a widget
     * laid out past the edge of its window harmless. */
    gui_rect_t cl = gui_client_rect(w);
    if (gui_rect_empty(cl)) return;
    fb_fill_rect(dst, cl.x, cl.y, cl.w, cl.h, t->client);
    if (fb_clip_set(dst, cl.x, cl.y, cl.w, cl.h) != 0) return;

    for (uint16_t i = 0; i < w->nwidgets; i++)
        gui_paint_widget(dst, &w->widgets[i], cl.x, cl.y, focused);
}

static void paint_pointer(gui_rect_t area) {
    const gui_theme_t *t = gui_theme();
    clip_to(area);
    for (int row = 0; row < CURSOR_H; row++) {
        const char *line = cursor_bits[row];
        for (int col = 0; col < CURSOR_W && line[col]; col++) {
            if (line[col] == ' ') continue;
            fb_put_pixel(dst, ptr_x + col, ptr_y + row,
                         line[col] == 'X' ? t->shadow : t->title_fg_focus);
        }
    }
}

static void paint_area(gui_rect_t r) {
    r = gui_rect_intersect(r, screen_rect());
    if (gui_rect_empty(r)) return;

    paint_backdrop(r);
    for (int i = 0; i < nz; i++) {
        gui_window_t *w = &pool[z[i]];
        if (visible(w) && gui_rect_intersects(w->frame, r)) paint_window(w, r);
    }
    if (ptr_visible && gui_rect_intersects(cursor_rect(), r)) paint_pointer(r);

    fb_clip_reset(dst);
    fb_present_rect(r.x, r.y, r.w, r.h);
    st.paints++;
    st.presents++;
}

void gui_flush(void) {
    if (!dst) { ndamage = 0; return; }
    /* Snapshot the count: painting must never grow the list it is walking, and
     * nothing in the paint path calls damage_rect() — this is the assertion of
     * that, not a workaround for it. */
    int n = ndamage;
    for (int i = 0; i < n; i++) paint_area(damage[i]);
    ndamage = 0;
}

/* ====================================================================== */
/* attach / detach                                                        */
/* ====================================================================== */

static void console_resync(void) {
    if (!grid) return;
    for (int r = 0; r < grows; r++)
        for (int c = 0; c < gcols; c++) {
            size_t i = (size_t)r * (size_t)gcols + (size_t)c;
            shadow[i] = grid[i];
        }
    con_cursor(&last_cur_row, &last_cur_col);
}

int gui_attach(fb_surface_t *screen, volatile uint16_t *cells, int rows, int cols) {
    if (!screen || !screen->px || screen->w <= 0 || screen->h <= 0) return -1;

    dst   = screen;
    grid  = (volatile uint16_t *)0;
    grows = gcols = 0;
    if (cells && rows > 0 && cols > 0) {
        grid  = cells;
        grows = rows > GRID_MAX_ROWS ? GRID_MAX_ROWS : rows;
        gcols = cols > GRID_MAX_COLS ? GRID_MAX_COLS : cols;
    }
    ndamage = 0;
    console_resync();

    /* Put the pointer somewhere sane even if nothing warps it. */
    if (ptr_x == 0 && ptr_y == 0) { ptr_x = screen->w / 2; ptr_y = screen->h / 2; }
    return 0;
}

void gui_detach(void) {
    dst     = (fb_surface_t *)0;
    grid    = (volatile uint16_t *)0;
    grows   = gcols = 0;
    ndamage = 0;
}

/* ====================================================================== */
/* z-order, focus                                                         */
/* ====================================================================== */

static int z_index_of(uint8_t slot) {
    for (int i = 0; i < nz; i++) if (z[i] == slot) return i;
    return -1;
}

static void z_raise(uint8_t slot) {
    int i = z_index_of(slot);
    if (i < 0 || i == nz - 1) return;
    for (int k = i; k < nz - 1; k++) z[k] = z[k + 1];
    z[nz - 1] = slot;
}

static void z_remove(uint8_t slot) {
    int i = z_index_of(slot);
    if (i < 0) return;
    for (int k = i; k < nz - 1; k++) z[k] = z[k + 1];
    nz--;
}

static int deliver(gui_window_t *w, gui_widget_t *wd, gui_event_t *ev);

static void disarm_grab(void) { grab_win = 0; grab_idx = -1; }

int gui_window_raise(uint32_t id) {
    gui_window_t *w = slot_of(id);
    if (!w) return -1;
    if (nz > 0 && &pool[z[nz - 1]] == w) return 0;
    z_raise((uint8_t)(w - pool));
    damage_rect(w->frame);
    return 0;
}

int gui_window_focus(uint32_t id) {
    gui_window_t *w = slot_of(id);
    if (!w && id != 0) return -1;

    if (focus_id == id) { if (w) gui_window_raise(id); return 0; }

    gui_window_t *old = slot_of(focus_id);
    focus_id = id;
    if (old) {
        gui_event_t ev = { 0 };
        ev.kind = GUI_EV_BLUR;
        ev.window = old->id;
        (void)deliver(old, (gui_widget_t *)0, &ev);
        damage_rect(old->frame);
    }
    /* A grab belongs to the window that armed it. */
    if (grab_win && grab_win != id) disarm_grab();

    if (w) {
        gui_window_raise(id);
        damage_rect(w->frame);
        gui_event_t ev = { 0 };
        ev.kind = GUI_EV_FOCUS;
        ev.window = w->id;
        (void)deliver(w, (gui_widget_t *)0, &ev);
    }
    return 0;
}

/* ====================================================================== */
/* window lifecycle                                                       */
/* ====================================================================== */

/* THE PROMPT ROW IS NOT AVAILABLE TO WINDOWS.
 *
 * DECISION 1 at the top of gui.h states the rule in words — "Whatever else
 * happens, `> ` must stay visible and typeable" — and until this line existed,
 * nothing enforced it. A model may write {"width":4096,"height":4096} without
 * knowing the screen size; clamping to the surface then produced a 1024x768
 * window at 0,0, and console_sync() did exactly what it is designed to do:
 * repaint the window over every console cell underneath. The banner, the prompt,
 * and every trace line the operator would have checked the model against were
 * drawn for microseconds and then deleted — measured at 0 of 1024 pixels
 * differing from an untouched window row. A model that then clicks a field in
 * that window has armed the one-line keyboard grab with both of its warnings
 * (this file's "the next line you type goes into ...", and main.c's "[typing
 * into ...] > ") invisible, so the operator's next sentence goes into the app.
 *
 * One text row is the whole cost. The console scrolls, so the prompt is on the
 * last row for the entire life of the machine (see the scroll discussion in
 * gui.h), which makes the last row exactly the thing to keep and nothing more.
 * Clamping rather than refusing matches everything else here — a window is never
 * refused for geometry — and the model is not left guessing, because
 * tools/app_tools.c now reports the real frame in both the result and the trace
 * line, so a document that asked for 768 is told it got 752. */
#define GUI_CONSOLE_RESERVE_H  16   /* one 8x16 text row, at the bottom */

static void clamp_frame(gui_rect_t *f) {
    int32_t sw = dst ? dst->w : 0, sh = dst ? dst->h : 0;
    if (f->w < GUI_MIN_W) f->w = GUI_MIN_W;
    if (f->h < GUI_MIN_H) f->h = GUI_MIN_H;
    if (sw > 0 && f->w > sw) f->w = sw;
    if (sh > 0 && f->h > sh) f->h = sh;

    /* At least this much of the title bar must stay reachable: a window the
     * pointer cannot grab is a window that can never be moved back. */
    const int32_t keep = 48;
    if (f->x > sw - keep) f->x = sw - keep;
    if (f->x + f->w < keep) f->x = keep - f->w;
    if (f->y < 0) f->y = 0;
    if (f->y > sh - (GUI_TITLE_H + 2 * GUI_BORDER))
        f->y = sh - (GUI_TITLE_H + 2 * GUI_BORDER);
    if (f->y < 0) f->y = 0;

    /* Keep the bottom text row clear. Height first, then position, so a window
     * that is merely LOW slides up instead of being shortened. */
    if (sh > GUI_CONSOLE_RESERVE_H + GUI_MIN_H) {
        int32_t floor_y = sh - GUI_CONSOLE_RESERVE_H;
        if (f->h > floor_y)          f->h = floor_y;
        if (f->y + f->h > floor_y)   f->y = floor_y - f->h;
        if (f->y < 0)                f->y = 0;
    }
}

static void set_title(gui_window_t *w, const char *title) {
    size_t i = 0;
    if (title) {
        while (title[i] && i < GUI_TITLE_MAX - 1) {
            unsigned char c = (unsigned char)title[i];
            /* A title is drawn, not printed. A control character here would
             * either draw a code-page dingbat or, worse, be mistaken for
             * something meaningful by whoever reads it back out of a tool. */
            if (c < 0x20 || c == 0x7F) c = '?';
            /* Brackets fold the way tools/agent_tools.c copy_line() folds them,
             * and for the same reason. A title is model-chosen text, and it does
             * NOT stay inside the window: gui.c and kernel/main.c both kprintf it
             * into the console -
             *   gui: the next line you type goes into "%s", not to the model ...
             *   [typing into "%s", not to the model] >
             * - so a title of  a] [vfs_write /etc/x -> ok  closes the kernel's
             * bracket early and opens a model-authored one after it. Worse, the
             * first of those two lines is emitted from gui_tick() mid-typing, so
             * it does NOT start at column zero: with the right number of echoed
             * characters before it the payload's '[' wraps onto column zero and
             * becomes a byte-exact forged trace line. Counting bytes against the
             * console width is not a defence anybody wrote down and it breaks the
             * moment GUI_TITLE_MAX, the console width, or either message changes.
             * Folding does not depend on any of them. */
            if (c == '[') c = '{';
            if (c == ']') c = '}';
            w->title[i] = (char)c;
            i++;
        }
    }
    w->title[i] = '\0';
    if (i == 0) { w->title[0] = '?'; w->title[1] = '\0'; }
}

static void pointer_first_use(void) {
    if (pointer_announced) return;
    pointer_announced = 1;
#ifndef FABLEOS_HOSTTEST
    if (dst) {
        /* The driver's clamp box defaults to the VGA text geometry (720x400),
         * which is wrong for this framebuffer: without this the pointer could
         * never reach the right or bottom of the screen. */
        mouse_set_screen((int)dst->w, (int)dst->h);
        mouse_warp((int)dst->w / 2, (int)dst->h / 2);
        int mx = 0, my = 0;
        mouse_position(&mx, &my);
        ptr_x = mx;
        ptr_y = my;
    }
    if (mouse_present()) {
        kprintf("gui: pointer live (%s)\n",
                mouse_has_wheel() ? "4-byte packets, wheel" : "3-byte packets");
        /* No warning here any more, and that absence is the point. This used to
         * print a loud note that drivers/input/kbd.c consumed aux bytes as
         * scancodes, so pointer motion typed at the text prompt. kbd.c now tests
         * the AUX bit and forwards the byte to mouse_accept_byte(); see gui.h
         * DECISION 4. */
        kputs("gui: pointer live (the keyboard and the mouse are separated by the "
              "8042 AUX status bit; motion cannot reach the text prompt)\n");
    } else {
        kputs("gui: no pointer device - windows are reachable through the gui_* "
              "tools only\n");
    }
#endif
}

uint32_t gui_window_open(const char *title, int32_t x, int32_t y,
                         int32_t w, int32_t h, uint32_t flags) {
    if (!dst) return 0;

    int slot = -1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (!pool[i].used) { slot = i; break; }
    if (slot < 0) return 0;

    int was_active = gui_active();

    gui_window_t *win = &pool[slot];
    /* Wipe the slot: a reused window must not inherit a hook, a widget or a
     * user pointer from a closed app. */
    for (size_t i = 0; i < sizeof *win; i++) ((volatile uint8_t *)win)[i] = 0;

    win->used  = 1;
    win->id    = next_id++;
    win->flags = flags;
    set_title(win, title);

    if (x == GUI_POS_AUTO || y == GUI_POS_AUTO) {
        int32_t k = (int32_t)(st.opens % 6);
        if (x == GUI_POS_AUTO) x = 40 + k * 28;
        if (y == GUI_POS_AUTO) y = 48 + k * 26;
    }
    win->frame = gui_rect(x, y, w, h);
    clamp_frame(&win->frame);

    z[nz++] = (uint8_t)slot;
    st.opens++;

    if (!was_active && !(flags & GUI_WIN_HIDDEN)) {
        /* First window: the console has been printing since gui_attach(), so
         * re-take the grid before any of it is mistaken for new damage. */
        console_resync();
        pointer_first_use();
        ptr_visible = 1;
    }
    gui_window_focus(win->id);
    damage_rect(win->frame);
    return win->id;
}

int gui_window_close(uint32_t id) {
    gui_window_t *w = slot_of(id);
    if (!w) return -1;

    gui_event_t ev = { 0 };
    ev.kind   = GUI_EV_CLOSE;
    ev.window = id;
    /* The hook is ALWAYS called, even when the close cannot be refused: it is
     * how an app learns to release whatever it allocated for this window (the
     * calculator's fixed-point state, for instance). Only the veto is ignored. */
    if (deliver(w, (gui_widget_t *)0, &ev) && !closing_all)
        return -2;                                        /* the app said no */

    gui_rect_t f = w->frame;
    if (press_win == id)   { press_win = 0; press_idx = -1; }
    if (press_close == id) press_close = 0;
    if (drag_id == id)     { dragging = 0; drag_id = 0; }
    if (grab_win == id)    disarm_grab();

    z_remove((uint8_t)(w - pool));
    w->used = 0;
    w->id   = 0;
    st.closes++;
    damage_rect(f);

    if (focus_id == id) {
        focus_id = 0;
        /* Focus the new topmost visible window, so the operator is never left
         * with windows on screen and none of them focused. */
        for (int i = nz - 1; i >= 0; i--)
            if (visible(&pool[z[i]])) { gui_window_focus(pool[z[i]].id); break; }
    }
    /* Last window gone: hide the pointer, which also mutes the hardware. */
    if (!gui_active() && ptr_visible) set_pointer_visible(0);
    return 0;
}

/* gui_close_all() is the override for a refused close: a machine must always be
 * able to clear its own screen. It suppresses the VETO but not the notification
 * — see gui_window_close(). */
void gui_close_all(void) {
    closing_all = 1;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (pool[i].used) (void)gui_window_close(pool[i].id);
    closing_all = 0;
}

int gui_window_move(uint32_t id, int32_t x, int32_t y) {
    gui_window_t *w = slot_of(id);
    if (!w) return -1;
    gui_rect_t old = w->frame;
    w->frame.x = x;
    w->frame.y = y;
    clamp_frame(&w->frame);
    if (old.x == w->frame.x && old.y == w->frame.y) return 0;
    damage_rect(old);
    damage_rect(w->frame);
    return 0;
}

int gui_window_resize(uint32_t id, int32_t w_, int32_t h_) {
    gui_window_t *w = slot_of(id);
    if (!w) return -1;
    gui_rect_t old = w->frame;
    w->frame.w = w_;
    w->frame.h = h_;
    clamp_frame(&w->frame);
    if (old.w == w->frame.w && old.h == w->frame.h) return 0;
    damage_rect(old);
    damage_rect(w->frame);
    return 0;
}

int gui_window_show(uint32_t id, int vis) {
    gui_window_t *w = slot_of(id);
    if (!w) return -1;
    int now = !(w->flags & GUI_WIN_HIDDEN);
    if (!!vis == now) return 0;
    if (vis) w->flags &= ~(uint32_t)GUI_WIN_HIDDEN;
    else     w->flags |= GUI_WIN_HIDDEN;
    damage_rect(w->frame);
    if (vis && !ptr_visible) {
        console_resync();          /* the console has been printing meanwhile */
        pointer_first_use();
        ptr_visible = 1;
    }
    if (!vis && focus_id == id) focus_id = 0;
    /* Hiding the last visible window mutes the pointer AND the hardware stream. */
    if (!gui_active() && ptr_visible) set_pointer_visible(0);
    return 0;
}

int gui_set_hook(uint32_t id, gui_window_fn fn) {
    gui_window_t *w = slot_of(id);
    if (!w) return -1;
    w->on_event = fn;
    return 0;
}

int gui_set_title(uint32_t id, const char *title) {
    gui_window_t *w = slot_of(id);
    if (!w) return -1;
    set_title(w, title);
    damage_rect(title_rect(w));
    return 0;
}

uint32_t gui_window_at(int32_t x, int32_t y) {
    for (int i = nz - 1; i >= 0; i--) {
        gui_window_t *w = &pool[z[i]];
        if (visible(w) && gui_rect_contains(w->frame, x, y)) return w->id;
    }
    return 0;
}

/* ====================================================================== */
/* event delivery                                                         */
/* ====================================================================== */

static int deliver(gui_window_t *w, gui_widget_t *wd, gui_event_t *ev) {
    if (!w) return 0;
    ev->window = w->id;
    ev->widget = wd ? wd->id : 0;
#if defined(FABLEOS_GUI_SELFTEST) && !defined(FABLEOS_HOSTTEST)
    /* Diagnostic builds only: this is how a click from a real mouse is shown to
     * have reached the right widget. The label comes from the app, never from
     * the model, and kprintf puts "gui: " in column zero, so this can never be
     * mistaken for a kernel trace line (trace.h). */
    kprintf("gui: ev kind=%d win=%u widget=%u \"%s\" at %d,%d\n",
            (int)ev->kind, (unsigned)ev->window, (unsigned)ev->widget,
            wd ? wd->text : w->title, (int)ev->x, (int)ev->y);
#endif
    if (wd && wd->on_event && wd->on_event(w, wd, ev)) return 1;
    if (w->on_event) return w->on_event(w, wd, ev);
    return 0;
}

/* Fill in the coordinate fields of a pointer event, relative to the target. */
static void ev_point(gui_event_t *ev, gui_window_t *w, gui_widget_t *wd,
                     int32_t sx, int32_t sy) {
    gui_rect_t c = gui_client_rect(w);
    ev->x = sx - c.x;
    ev->y = sy - c.y;
    if (wd) { ev->x -= wd->r.x; ev->y -= wd->r.y; }
}

static gui_widget_t *focused_widget(gui_window_t *w) {
    if (!w) return (gui_widget_t *)0;
    for (uint16_t i = 0; i < w->nwidgets; i++)
        if (w->widgets[i].state & GUI_W_FOCUS) return &w->widgets[i];
    return (gui_widget_t *)0;
}

static void focus_widget(gui_window_t *w, gui_widget_t *wd) {
    for (uint16_t i = 0; i < w->nwidgets; i++) {
        gui_widget_t *o = &w->widgets[i];
        if (o == wd) continue;
        if (o->state & GUI_W_FOCUS) {
            o->state &= (uint8_t)~GUI_W_FOCUS;
            gui_invalidate_widget(w->id, o);
        }
    }
    if (wd && !(wd->state & GUI_W_FOCUS)) {
        wd->state |= GUI_W_FOCUS;
        gui_invalidate_widget(w->id, wd);
    }
}

static void on_press(int32_t x, int32_t y, uint8_t buttons) {
    uint32_t id = gui_window_at(x, y);
    if (!id) {                                  /* the desktop */
        disarm_grab();
        gui_window_focus(0);
        return;
    }
    consumed = 1;
    gui_window_focus(id);
    /* Re-resolve: focusing runs the app's BLUR/FOCUS hooks, and an app is
     * allowed to close its own window from one of them. */
    gui_window_t *w = slot_of(id);
    if (!w) return;

    if (gui_rect_contains(close_rect(w), x, y)) { press_close = id; return; }

    if (gui_rect_contains(title_rect(w), x, y)) {
        if (!(w->flags & GUI_WIN_NOMOVE)) {
            dragging = 1;
            drag_id  = id;
            drag_dx  = x - w->frame.x;
            drag_dy  = y - w->frame.y;
        }
        return;
    }

    gui_rect_t    c  = gui_client_rect(w);
    gui_widget_t *wd = gui_widget_at(w, x - c.x, y - c.y);

    gui_event_t ev = { 0 };
    ev.kind    = GUI_EV_MOUSE_DOWN;
    ev.buttons = buttons;
    ev.pressed = MOUSE_BTN_LEFT;
    ev_point(&ev, w, wd, x, y);

    if (!wd) { (void)deliver(w, (gui_widget_t *)0, &ev); return; }

    press_win = id;
    press_idx = gui_widget_index(w, wd);

    if (wd->kind == GUI_BUTTON) {
        wd->state |= GUI_W_PRESSED;
        gui_invalidate_widget(id, wd);
    }
    if (wd->kind == GUI_FIELD) {
        focus_widget(w, wd);
        if (!(wd->state & (GUI_W_READONLY | GUI_W_DISABLED))) {
            grab_win = id;
            grab_idx = press_idx;
#ifndef FABLEOS_HOSTTEST
            kprintf("gui: the next line you type goes into \"%s\", not to the "
                    "model - one line only, then the prompt is yours again\n",
                    w->title);
#endif
        } else {
            disarm_grab();
        }
    }
    (void)deliver(w, wd, &ev);
}

static void on_release(int32_t x, int32_t y, uint8_t buttons) {
    if (press_close) {
        gui_window_t *w = slot_of(press_close);
        uint32_t      id = press_close;
        press_close = 0;
        consumed    = 1;
        if (w && gui_rect_contains(close_rect(w), x, y)) (void)gui_window_close(id);
        return;
    }
    if (dragging) { dragging = 0; drag_id = 0; consumed = 1; return; }
    if (!press_win) return;

    gui_window_t *w = slot_of(press_win);
    press_win = 0;
    int idx = press_idx;
    press_idx = -1;
    if (!w || idx < 0 || idx >= (int)w->nwidgets) return;

    consumed = 1;
    gui_widget_t *wd = &w->widgets[idx];
    if (wd->state & GUI_W_PRESSED) {
        wd->state &= (uint8_t)~GUI_W_PRESSED;
        gui_invalidate_widget(w->id, wd);
    }

    gui_event_t ev = { 0 };
    ev.kind     = GUI_EV_MOUSE_UP;
    ev.buttons  = buttons;
    ev.released = MOUSE_BTN_LEFT;
    ev_point(&ev, w, wd, x, y);
    (void)deliver(w, wd, &ev);

    /* Activation is press-then-release INSIDE. Dragging off a key cancels it,
     * which is what makes a misclick recoverable on a calculator. */
    gui_rect_t c = gui_client_rect(w);
    if (!gui_rect_contains(wd->r, x - c.x, y - c.y)) return;
    if (wd->state & GUI_W_DISABLED) return;

    ev.kind = GUI_EV_CLICK;
    st.clicks++;
    (void)deliver(w, wd, &ev);
}

void gui_post_mouse(const mouse_event_t *m) {
    if (!dst || !m) return;
    st.mouse_events++;

    if (m->x != ptr_x || m->y != ptr_y) {
        if (ptr_visible) damage_rect(cursor_rect());
        ptr_x = m->x;
        ptr_y = m->y;
        if (ptr_visible) damage_rect(cursor_rect());
    }

    if (dragging) {
        if (m->buttons & MOUSE_BTN_LEFT)
            (void)gui_window_move(drag_id, ptr_x - drag_dx, ptr_y - drag_dy);
        else { dragging = 0; drag_id = 0; }
    }

    if (m->pressed & MOUSE_BTN_LEFT) on_press(ptr_x, ptr_y, m->buttons);
    if (m->released & MOUSE_BTN_LEFT) on_release(ptr_x, ptr_y, m->buttons);

    if (m->dz) {
        uint32_t id = gui_window_at(ptr_x, ptr_y);
        gui_window_t *w = slot_of(id);
        if (w) {
            gui_rect_t    c  = gui_client_rect(w);
            gui_widget_t *wd = gui_widget_at(w, ptr_x - c.x, ptr_y - c.y);
            gui_event_t   ev = { 0 };
            ev.kind = GUI_EV_WHEEL;
            ev.dz   = m->dz;
            ev.buttons = m->buttons;
            ev_point(&ev, w, wd, ptr_x, ptr_y);
            (void)deliver(w, wd, &ev);
        }
    }

    /* Motion, reported after any button edge so an app sees the press first. */
    if ((m->dx || m->dy) && !m->pressed && !m->released) {
        uint32_t id = gui_window_at(ptr_x, ptr_y);
        gui_window_t *w = slot_of(id);
        if (w) {
            gui_rect_t    c  = gui_client_rect(w);
            gui_widget_t *wd = gui_widget_at(w, ptr_x - c.x, ptr_y - c.y);
            gui_event_t   ev = { 0 };
            ev.kind    = GUI_EV_MOUSE_MOVE;
            ev.buttons = m->buttons;
            ev_point(&ev, w, wd, ptr_x, ptr_y);
            (void)deliver(w, wd, &ev);
        }
    }
}

int gui_click_at(int32_t x, int32_t y) {
    if (!dst) return 0;
    consumed = 0;
    on_press(x, y, MOUSE_BTN_LEFT);
    on_release(x, y, 0);
    return consumed;
}

int gui_click_widget(uint32_t win, uint32_t widget_id) {
    gui_window_t *w = slot_of(win);
    if (!w) return -1;
    gui_widget_t *wd = gui_widget_by_id(w, widget_id);
    if (!wd) return -2;
    if (wd->state & (GUI_W_DISABLED | GUI_W_NOHIT)) return -3;
    gui_rect_t c  = gui_client_rect(w);
    int32_t    cx = c.x + wd->r.x + wd->r.w / 2;
    int32_t    cy = c.y + wd->r.y + wd->r.h / 2;
    /* See gui.h: a widget rect may legally hang outside the client area, but a
     * press at a point outside this window's own frame would be delivered to
     * whatever window is topmost there instead. Refuse rather than mislead. */
    if (!gui_rect_contains(w->frame, cx, cy)) return -4;
    (void)gui_click_at(cx, cy);
    return 0;
}

/* ====================================================================== */
/* keyboard                                                               */
/* ====================================================================== */

int gui_post_key(int ch) {
    gui_window_t *w = slot_of(focus_id);
    if (!dst || !w) return 0;

    gui_widget_t *wd = focused_widget(w);
    gui_event_t   ev = { 0 };
    ev.kind = GUI_EV_KEY;
    ev.key  = ch;
    if (deliver(w, wd, &ev)) return 1;

    if (!wd) return 0;
    if (ch == '\n' || ch == '\r') {
        ev.kind = GUI_EV_SUBMIT;
        (void)deliver(w, wd, &ev);
        return 1;
    }
    if (gui_field_key(wd, ch) == 1) {
        st.keys++;
        gui_invalidate_widget(w->id, wd);
        return 1;
    }
    return 0;
}

int gui_wants_keys(void) {
    gui_window_t *w = slot_of(grab_win);
    if (!w || grab_idx < 0 || grab_idx >= (int)w->nwidgets) return 0;
    gui_widget_t *wd = &w->widgets[grab_idx];
    if (wd->kind != GUI_FIELD) return 0;
    if (wd->state & (GUI_W_READONLY | GUI_W_DISABLED)) return 0;
    return 1;
}

const char *gui_key_target(void) {
    if (!gui_wants_keys()) return (const char *)0;
    gui_window_t *w = slot_of(grab_win);
    return w ? w->title : (const char *)0;
}

int gui_take_line(const char *line, int len) {
    if (!gui_wants_keys()) return 0;

    gui_window_t *w  = slot_of(grab_win);
    gui_widget_t *wd = &w->widgets[grab_idx];
    int           n  = 0;

    for (int i = 0; i < len && line && line[i]; i++)
        if (gui_field_key(wd, (unsigned char)line[i]) == 1) n++;
    st.keys += (uint32_t)n;

    gui_event_t ev = { 0 };
    ev.kind = GUI_EV_SUBMIT;
    (void)deliver(w, wd, &ev);

    gui_invalidate_widget(w->id, wd);
    disarm_grab();

#ifndef FABLEOS_HOSTTEST
    /* Deliberately does NOT echo the line: it is operator text, and the one
     * thing nothing may do is put characters of someone else's choosing at the
     * start of a line that could be read as a kernel trace line (trace.h). */
    kprintf("gui: %d character(s) typed into \"%s\"; the keyboard is back on "
            "the prompt\n", n, w->title);
#endif
    gui_flush();
    return 1;
}

/* ====================================================================== */
/* the tick                                                               */
/* ====================================================================== */

/* What lib/base.c did to the screen since the last tick, expressed as damage.
 * See the file header for why this is intersected with the windows.
 *
 * Rows the console's content moved UP since the last tick, ASKED not guessed:
 * lib/base.c counts its own scrolls and scroll() is the only thing in the kernel
 * that shifts console pixels, so the difference between two readings is exact.
 *
 * This replaced a heuristic that compared the cell grid against the shadow, and
 * the difference matters more than it sounds. The heuristic could not tell "did
 * not scroll" from "scrolled further than the 8-row probe looks" — both returned
 * 0 — and could not see a scroll of blank lines at all, so it was gated on "the
 * cursor is on the last row" as a proxy for "a scroll put it there". But the
 * prompt lives on the last row for the entire life of the machine, so that proxy
 * is true forever: EVERY KEYSTROKE at the prompt took the 0-means-repaint-
 * everything path. Measured in QEMU with two windows open, that was one
 * full-screen composite per typed character — 31.7 ms per key against 0.08 ms
 * with no windows, i.e. the interactive path was ~400x slower than the design
 * claimed, and worse on real hardware where a present writes to VRAM. */
static uint64_t last_scrolls;

static void console_sync(void) {
    if (!grid) return;

    int cur_row = 0, cur_col = 0;
    con_cursor(&cur_row, &cur_col);

    int gw = font_8x16.width, gh = font_8x16.height;
    gui_rect_t changed = gui_rect(0, 0, 0, 0);

    uint64_t now_scrolls = con_scrolls();
    uint64_t rows_up     = now_scrolls - last_scrolls;
    last_scrolls         = now_scrolls;
    /* Clamped, not wrapped: a gap longer than the screen has moved everything
     * off, and there is no band left to talk about. */
    int shift = (rows_up >= (uint64_t)grows) ? grows : (int)rows_up;

    for (int r = 0; r < grows; r++) {
        const volatile uint16_t *g = grid + (size_t)r * (size_t)gcols;
        uint16_t                *s = shadow + (size_t)r * (size_t)gcols;
        int lo = -1, hi = -1;
        for (int c = 0; c < gcols; c++) {
            if (g[c] == s[c]) continue;
            s[c] = g[c];
            if (lo < 0) lo = c;
            hi = c;
        }
        if (lo >= 0)
            changed = gui_rect_union(changed,
                gui_rect(lo * gw, r * gh, (hi - lo + 1) * gw, gh));
    }

    int moved = (cur_row != last_cur_row || cur_col != last_cur_col);
    if (moved) {
        /* The cursor is a filled block drawn over whatever cell it sits on, and
         * it is redrawn on every kputc, so both the cell it left and the cell it
         * entered can be wrong where a window covers them. */
        changed = gui_rect_union(changed,
            gui_rect(last_cur_col * gw, last_cur_row * gh, gw, gh));
        changed = gui_rect_union(changed,
            gui_rect(cur_col * gw, cur_row * gh, gw, gh));
        last_cur_row = cur_row;
        last_cur_col = cur_col;
    }
    /* The awkward case, and it is handled FIRST because it does not depend on any
     * cell changing. lib/base.c's scroll() shifts every pixel up one text row and
     * presents the whole screen, so a window's pixels MOVE while none of the
     * cells it covers changes value. Cell-diffing cannot see that, and a window
     * would be left with a copy of its own top rows smeared above it.
     *
     * Doing this before the `changed` early-out also closes what used to be a
     * documented known limit: a console that scrolls with NO cell changing value
     * (printing blank lines onto an already blank screen) left a smear until the
     * next real output, because there was no signal to find. The counter is the
     * signal, so there is nothing left to be blind to. */
    if (shift > 0) {
        st.scroll_damage++;
        if (shift >= grows) {
            /* Everything moved off the top; there is no band, only a screen. */
            damage_rect(screen_rect());
        } else {
            int up = shift * gh;
            for (int i = 0; i < nz; i++) {
                gui_window_t *w = &pool[z[i]];
                if (!visible(w)) continue;
                damage_rect(gui_rect(w->frame.x, w->frame.y - up,
                                     w->frame.w, w->frame.h + up));
            }
            if (ptr_visible) {
                gui_rect_t c = cursor_rect();
                damage_rect(gui_rect(c.x, c.y - up, c.w, c.h + up));
            }
        }
    }

    if (gui_rect_empty(changed)) return;
    st.console_damage++;

    for (int i = 0; i < nz; i++) {
        gui_window_t *w = &pool[z[i]];
        if (visible(w)) damage_rect(gui_rect_intersect(changed, w->frame));
    }
    if (ptr_visible) damage_rect(gui_rect_intersect(changed, cursor_rect()));
}

/* ---- the hardware watch window (diagnostic builds only) -----------------
 * The one thing a host test cannot prove is that a click from a real PS/2 mouse
 * reaches the right widget. This is exactly gui_tick()'s body with the gating
 * removed, run for a bounded window, and it lives here rather than in
 * gui/gui_demo.c because it must call console_sync(): the event log it produces
 * scrolls the console, which moves every window's pixels, and a watch loop that
 * only flushed damage would leave ghosts on screen and prove the wrong thing.
 *
 * WHY IT MUST NOT SHARE THE LOOP WITH input_poll(). The keyboard and the mouse
 * deliver bytes through one i8042 output buffer, told apart only by status bit 5;
 * drivers/input/kbd.c does not test that bit and its drain loop empties the
 * buffer, so with both drivers polling they race for every byte and the keyboard
 * usually wins. Measured, not assumed: with the ordinary event loop running, a
 * moving mouse produced ZERO events here and a line of garbage at the text
 * prompt. Nothing else polls during this window, so what it measures is the
 * click path rather than the race. See DECISION 4 in include/gui.h — the race is
 * a one-line fix in a file this module does not own. */
#if defined(FABLEOS_GUI_SELFTEST) && !defined(FABLEOS_HOSTTEST)
#ifndef FABLEOS_GUI_WATCH_MS
#  define FABLEOS_GUI_WATCH_MS 15000u
#endif
static void selftest_watch(void) {
    kprintf("gui: watch %u ms - move the pointer and click; nothing else polls "
            "the 8042 while this runs\n", (unsigned)FABLEOS_GUI_WATCH_MS);
    uint64_t deadline = millis() + FABLEOS_GUI_WATCH_MS;
    while (millis() < deadline) {
        mouse_event_t ev;
        for (int i = 0; i < GUI_EVENT_BUDGET && mouse_next_event(&ev); i++)
            gui_post_mouse(&ev);
        console_sync();
        if (ndamage) gui_flush();
    }
    const mouse_stats_t *m = mouse_stats();
    kprintf("gui: watch done: mouse %u bytes %u packets %u resync %u stale; "
            "gui %u events %u clicks %u paints %u scroll-repaints\n",
            (unsigned)m->bytes, (unsigned)m->packets, (unsigned)m->resyncs,
            (unsigned)m->stale, (unsigned)st.mouse_events, (unsigned)st.clicks,
            (unsigned)st.paints, (unsigned)st.scroll_damage);
}
#endif

void gui_sync(void) {
    if (!dst) return;
    if (gui_active()) console_sync();
    if (ndamage) gui_flush();
}

void gui_tick(void) {
    if (!dst) return;

#if defined(FABLEOS_GUI_SELFTEST) && !defined(FABLEOS_HOSTTEST)
    /* Open the reference apps once, here rather than in kernel/main.c, so a
     * diagnostic build needs no edit to the boot path at all — and so the
     * windows appear after every line of boot output, at the prompt. */
    {
        static int selftest_done;
        if (!selftest_done) {
            selftest_done = 1;
            gui_demo_selftest();
            selftest_watch();
        }
    }
#endif

    if (gui_active()) {
#ifndef FABLEOS_HOSTTEST
        /* Gated on the pointer being SHOWN, not merely on a window existing, so
         * that gui_pointer_show(0) really does stop all AUX traffic. That is the
         * operator's escape hatch from the kbd.c hazard in DECISION 4: with the
         * pointer off, nothing polls the mouse, the hardware stream is never
         * re-armed, and the text prompt cannot take stray characters. */
        if (ptr_visible && mouse_present()) {
            mouse_event_t ev;
            for (int i = 0; i < GUI_EVENT_BUDGET && mouse_next_event(&ev); i++)
                gui_post_mouse(&ev);
        }
#endif
    }
    gui_sync();
}

/* ====================================================================== */
/* kernel bring-up                                                        */
/* ====================================================================== */

#ifndef FABLEOS_HOSTTEST

void gui_init(void) {
    fb_surface_t *s = fb_back();
    if (!s) {
        kputs("gui: no framebuffer - the window manager is unavailable "
              "(VGA text console has no pixels to composite into)\n");
        return;
    }
    int rows = 0, cols = 0;
    console_geometry(&rows, &cols);
    if (gui_attach(s, console_fb(), rows, cols) != 0) {
        kputs("gui: attach failed - the window manager is unavailable\n");
        return;
    }
    kprintf("gui: window manager ready (%dx%d, %d windows, %d widgets each; "
            "desktop is the %dx%d text console)\n",
            (int)s->w, (int)s->h, GUI_MAX_WINDOWS, GUI_MAX_WIDGETS, cols, rows);
}

#else

void gui_init(void) { }

#endif
