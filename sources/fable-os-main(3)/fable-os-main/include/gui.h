/* gui.h — the window manager: windows, z-order, focus, damage, and the one
 *         event loop this machine has.
 *
 * PURPOSE
 *   The operator says "I want a calculator" and a calculator has to appear, be
 *   clickable, and not take the machine away from the sentence that asked for it.
 *   This header is the contract for that: a bounded pool of windows composited
 *   over the text console, a widget toolkit (widgets.h) to fill them with, and a
 *   cooperative tick that shares one single-threaded, interrupt-free CPU between
 *   the pointer, the keyboard, the windows and the natural-language turn loop.
 *
 *   Everything here is state and policy. All arithmetic on rectangles and all
 *   pixels are in widgets.h and lib/fb.c, which is why the whole of this module
 *   runs on the host against a malloc'd surface (tests/host/test_gui.c).
 *
 * ============================================================================
 * DECISION 1 — HOW THE CONSOLE AND THE WINDOWS SHARE THE DISPLAY
 * ============================================================================
 *   The operator talks to this machine in prose while apps are on screen, so the
 *   console cannot be replaced, hidden, or reduced to a window. It is the only
 *   place trace lines — the kernel's ground truth (trace.h) — can appear, and it
 *   is the only place the model's replies appear. Whatever else happens, `> `
 *   must stay visible and typeable.
 *
 *   Four designs were on the table:
 *
 *     (a) SPLIT THE SCREEN: console in rows [0,N), windows below. Requires
 *         teaching lib/base.c a smaller grid, i.e. changing the console's scroll
 *         and geometry — the one piece of code every subsystem prints through —
 *         and the console's geometry is what tools/screen_tools.c reads. Wrong
 *         blast radius, and it permanently spends half the screen on whichever
 *         half is idle.
 *
 *     (b) THE CONSOLE BECOMES A WINDOW. The most "correct" answer and the most
 *         dangerous one: the console would then depend on the WM being alive to
 *         be seen at all. A panic, a fault before gui_init(), or a bug in this
 *         file would leave a machine that cannot say what went wrong, on a
 *         machine whose only interface is words. The console must work when the
 *         GUI does not.
 *
 *     (c) WINDOWS ONLY WHILE THE CONSOLE IS IDLE (alternate full screens). No
 *         coexistence at all, and the demo requires coexistence.
 *
 *     (d) THE CONSOLE IS THE DESKTOP. Chosen. Windows float above the text, and
 *         the WM composites: for any damaged region it repaints the console's
 *         own cells first (it can, from console_fb() — the grid is public and the
 *         font is shared), then every window that intersects, bottom to top, then
 *         the pointer. The console keeps drawing exactly as it always has and
 *         does not know the GUI exists; the GUI needs no cooperation from
 *         lib/base.c and no change to it.
 *
 *   WHY (d) IS SAFE, in terms of what lib/base.c actually does. Its renderer
 *   keeps a shadow of the cell grid and redraws a cell only when the cell's
 *   VALUE changes. A window's pixels sit on top of cells whose values nobody
 *   touched, so printing a line of prose does not erase a window — it repaints
 *   only the cells it wrote and its own cursor cell. Three things do disturb a
 *   window, and each has an answer:
 *
 *     - text printed under the window: the cell value changed, so the console
 *       draws a glyph over the window. gui_tick() sees the changed cell in its
 *       own copy of the grid, damages it, and the compositor puts the window
 *       back on the next tick.
 *     - the console cursor block, which is redrawn every kputc: same mechanism,
 *       and it is one 8x16 cell.
 *     - a SCROLL, which shifts every pixel up one text row and presents the
 *       whole screen. Cell values change wherever there was text, but a window's
 *       pixels MOVE without any cell it covers changing, so cell-diffing cannot
 *       see it: the window is left with a copy of its own top rows smeared above
 *       it. So gui_tick() ASKS how far the screen moved rather than inferring it:
 *       lib/base.c counts its own scrolls (console_scrolls()), scroll() is the only
 *       thing in the kernel that shifts console pixels, and the difference between
 *       two readings is therefore exact. Each window is damaged together with the
 *       band above it; a gap deeper than the screen repaints once.
 *
 *       This used to be inferred by matching the grid against a shifted shadow,
 *       gated on "the console's cursor is on its last row" as a proxy for "a scroll
 *       put it there". The proxy is FALSE: the prompt lives on the last row for the
 *       entire life of the machine, so the gate was open forever, and a shadow
 *       match of 0 (which meant both "did not scroll" and "scrolled deeper than the
 *       probe looks") fell back to repainting all 786,432 pixels. Every keystroke
 *       at the prompt cost a full-screen composite: 31.7 ms per character with two
 *       windows open, against 0.08 ms with none. Asking is both exact and cheap,
 *       and it also sees a scroll of blank lines, which no amount of cell-diffing
 *       can.
 *
 *   THE HONEST COST, stated rather than hidden: between the console drawing a
 *   glyph and the next gui_tick() there is a window with a character on it. The
 *   tick runs on every iteration of the idle loop, so the gap is microseconds
 *   while the machine is waiting for input — but it is a real gap, and during a
 *   model round-trip (see DECISION 3) it is as long as the round-trip. This is
 *   the price of not owning the console, and it is the right price.
 *
 * ============================================================================
 * DECISION 2 — DAMAGE, NOT FRAMES
 * ============================================================================
 *   There is no vblank, no timer interrupt and no compositor thread. A full
 *   1024x768 repaint means ~800k pixel writes plus 6144 glyph rasterisations
 *   plus a 3 MiB present, at -O0, on the same CPU that has to answer the
 *   keyboard: unusable as a per-frame cost. So nothing is ever repainted
 *   because time passed — only because a specific rectangle became wrong.
 *
 *   Every state change names the rectangle it invalidated: opening a window
 *   damages its frame; moving one damages the union of where it was and where it
 *   is; pressing a button damages the button; the pointer damages the two cursor
 *   rectangles it left and entered; a console write damages the cells it wrote.
 *   gui_flush() paints the accumulated list and presents each rectangle
 *   separately, so a keypress on a calculator costs about 40x20 pixels.
 *
 *   The list is fixed at GUI_MAX_DAMAGE entries and never allocates. Adding a
 *   rectangle merges it into an existing one when the merged area is not much
 *   larger than the two separately (cheaper to repaint one region than to walk
 *   the window list twice); when the list is full, everything collapses into its
 *   bounding box. Over-damaging is always safe and never wrong — the invariant
 *   is only that damage is a SUPERSET of what changed, which is what makes it
 *   testable: tests/host/test_gui.c asserts pixels outside the damage did not
 *   move and pixels inside are correct.
 *
 * ============================================================================
 * DECISION 3 — THE EVENT LOOP ON A MACHINE WITH NO SCHEDULER
 * ============================================================================
 *   IF is 0, all 16 PIC lines are masked, there is one thread, and nothing runs
 *   unless something calls it. There is therefore no such thing here as "the GUI
 *   event loop": there is ONE loop, in kernel/main.c, and the GUI is a function
 *   it calls.
 *
 *       for (;;) {
 *           gui_tick();                     <- pointer, widgets, repaint
 *           n = input_poll(line, sizeof line);   <- keyboard/serial, one line
 *           if (n == INPUT_NONE) continue;
 *           if (gui_take_line(line, n)) continue;   <- typed into a field
 *           chat_ask(line);                 <- the model acts on the machine
 *       }
 *
 *   Four properties make that fair, and each one is a deliberate choice:
 *
 *     1. gui_tick() IS BOUNDED AND NEVER BLOCKS. It drains at most
 *        GUI_EVENT_BUDGET mouse events, diffs the console grid once, paints the
 *        damage list, and returns. No waiting, no retry loop, no "until quiet".
 *        Its worst case is one full-screen composite; its normal case, with
 *        nothing moving, is a grid diff and an immediate return.
 *
 *     2. IT DOES NOTHING AT ALL WITH NO WINDOWS OPEN. Not a repaint, not a mouse
 *        poll, not a grid diff. A normal boot is therefore byte-for-byte and
 *        pixel-for-pixel what it was before this module existed, and the PS/2
 *        mouse stream stays muted (see DECISION 4). The GUI costs nothing until
 *        somebody asks for a window.
 *
 *     3. THE KEYBOARD BELONGS TO THE PROMPT, and the GUI can borrow it for
 *        exactly one line. This machine's only interface is language; a widget
 *        that captures the keyboard is a widget that can strand the operator with
 *        no shell to escape to. So clicking an editable field ARMS a one-line
 *        grab: the next completed line goes into the field, the grab is released,
 *        and the line after it goes to the model. gui_wants_keys() reports the
 *        armed state and gui_take_line() consumes it. There is no key sequence to
 *        learn and no state to get stuck in.
 *
 *     4. THE POINTER BELONGS TO THE GUI, always. Mouse motion never reaches the
 *        turn loop and typing never moves the cursor, so the two interfaces
 *        cannot fight over an event. Both drivers check the i8042 AUX bit, so they
 *        cannot steal each other's bytes either — see DECISION 4.
 *
 *   WHAT IS NOT FAIR, AND EXACTLY HOW MUCH. chat_ask() blocks for as long as the
 *   model takes (net/net.c spins on net_service() until a deadline), and this
 *   module cannot get a tick in edgeways from outside. During a turn the GUI is
 *   frozen: no repaint, and mouse bytes are lost because the i8042 output buffer
 *   holds one byte. Nothing is corrupted — the decoder resynchronises on the
 *   always-one bit and the partial-packet timeout (mouse.h) — but a click during
 *   a turn does not happen. The fix is one line in a file this module does not
 *   own: net/net.c's net_service() should call gui_tick(), which is bounded,
 *   re-entrant-safe with respect to the console, and a no-op with no windows.
 *   Until then, gui_tick() is called from every loop that DOES belong to the GUI,
 *   and the freeze is bounded by the transport's own deadline.
 *
 * ============================================================================
 * DECISION 4 — THE POINTER IS OFF UNTIL A WINDOW EXISTS
 * ============================================================================
 *   The keyboard and the mouse share ONE one-byte i8042 output buffer and are told
 *   apart only by AUX status bit 5. Whoever polls first gets the byte and there is
 *   no way to put it back, so both drivers have to check that bit. mouse.h always
 *   did; drivers/input/kbd.c did not, and the consequence was not cosmetic: a
 *   mouse packet's first byte is 0x08|buttons|signs (0x08 = '7', 0x18 = 'o' in the
 *   scancode table) and a delta of 0x1C is Enter, so with any window open, moving
 *   the mouse typed garbage at the natural-language prompt and SUBMITTED it to the
 *   model — while the pointer itself received nothing, because the keyboard had
 *   eaten the stream. Confirmed on real QEMU: dozens of unintended model turns
 *   from mouse motion alone, and it persisted for the rest of the boot even after
 *   every window was closed.
 *
 *   THAT IS NOW FIXED at the source: kbd_getchar_nb() tests the AUX bit and hands
 *   the byte to mouse_accept_byte() rather than decoding it, so the mouse keeps its
 *   framing even when the keyboard wins the race. Two bounds remain by design:
 *
 *     - the hardware stream still starts MUTED and is armed only by the first
 *       consumer poll, so a machine that never opens a window is byte-for-byte the
 *       machine that existed before the GUI;
 *     - hiding the pointer (including automatically, when the last window closes)
 *       now calls mouse_stop(), so a device nobody is listening to is quiet
 *       instead of merely harmless.
 *
 * ============================================================================
 * WHAT AN APP IS
 * ============================================================================
 *   A window id, some widgets, and one hook:
 *
 *       uint32_t id = gui_window_open("Calculator", 40, 60, 240, 300, 0);
 *       gui_window_t *w = gui_window(id);
 *       gui_rect_t area = gui_inset(gui_client_area(w), 6, 6);
 *       gui_rect_t keys, disp = gui_split_top(area, 28, 6, &keys);
 *       gui_add_field(w, disp, "0", CALC_DISPLAY, GUI_W_READONLY);
 *       for (...) gui_add_button(w, gui_grid(keys, 5, 4, r, c, 1, 1, 4), "7", ...);
 *       gui_set_hook(id, calc_event);
 *
 *   The hook is called with the widget that fired and a gui_event_t; it mutates
 *   widget text and calls gui_invalidate_widget(). It never draws and never
 *   presents. gui/gui_demo.c is exactly that, in 200 lines, and is the worked
 *   example the app-authoring phase generalises: everything the model will need
 *   to emit is a call in this header.
 *
 * PUBLIC API
 *   lifecycle   gui_init gui_attach gui_detach gui_attached gui_active
 *   windows     gui_window_open gui_window_close gui_close_all gui_window
 *               gui_window_move gui_window_resize gui_window_raise
 *               gui_window_focus gui_window_show gui_set_hook gui_set_title
 *   queries     gui_window_count gui_window_at_z gui_window_at gui_focused
 *               gui_client_rect gui_client_area gui_frame_rect gui_screen_size
 *   events      gui_post_mouse gui_post_key gui_click_at gui_click_widget
 *               gui_tick gui_wants_keys gui_take_line
 *   damage      gui_invalidate gui_invalidate_window gui_invalidate_widget
 *               gui_invalidate_all gui_flush gui_damage_count gui_damage_rect
 *   pointer     gui_pointer_show gui_pointer_position gui_pointer_warp
 *   stats       gui_stats
 *
 * DEPENDENCIES
 *   widgets.h (and through it fb.h + font.h) for everything drawn; mouse.h for
 *   the event shape; kernel.h for the console grid and kprintf. No heap: the
 *   window pool is static, because a GUI that cannot open a window when memory
 *   is tight is a GUI that vanishes exactly when the operator needs to be told
 *   why. No hardware: gui_attach() takes the surface and the console grid as
 *   arguments, which is how the host tests drive the whole module.
 *
 * FUTURE EXTENSION POINTS
 *   - Per-window backing stores: gui_paint_window() already draws through a
 *     clipped fb_surface_t, so pointing it at a window-sized surface and
 *     fb_blit()ing the result is a change in one function.
 *   - Resize handles, minimise, and a task bar are policy on top of the same
 *     damage and hit-test primitives; gui_window_resize() already exists and
 *     re-lays nothing (widgets are in client coordinates and the app owns them).
 *   - A window that shows console text (a scrollback viewer) needs no new
 *     mechanism: the compositor already renders console cells.
 *   - Once an IDT services devices, mouse events arrive from an ISR calling
 *     mouse_feed_byte(); gui_tick() keeps draining the same queue.
 */
#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include <stddef.h>

#include "widgets.h"
#include "mouse.h"

/* ====================================================================== */
/* bounds — every one of these is static and hard                          */
/* ====================================================================== */

/* Windows that can exist at once. Twelve is far past what a 1024x768 screen can
 * show usefully and keeps the pool (widgets included) around 60 KiB of .bss. */
#define GUI_MAX_WINDOWS   12

/* Pass as x and/or y to gui_window_open() to let the WM cascade the window.
 * A distinguished value rather than "negative means auto", because a negative
 * position is legal: a window can be dragged half off the left edge. */
#define GUI_POS_AUTO      (-2147483647 - 1)

/* Widgets per window. A 4x5 keypad plus a display plus labels is 22; 48 leaves
 * room for a settings panel without inviting an app to be a whole desktop. */
#define GUI_MAX_WIDGETS   48

/* Title bytes, including the NUL. */
#define GUI_TITLE_MAX     40

/* Pending damage rectangles before the list collapses to its bounding box. */
#define GUI_MAX_DAMAGE    12

/* Mouse events consumed per gui_tick(). At 200 Hz a mouse produces ~3 events
 * per 16 ms, so this drains many frames' worth while still being a bound: a
 * controller stuck asserting OBF must not own the loop. */
#define GUI_EVENT_BUDGET  32

/* ====================================================================== */
/* windows                                                                */
/* ====================================================================== */

/* Window flags. */
#define GUI_WIN_NOCLOSE  0x01   /* no close box; gui_window_close() still works */
#define GUI_WIN_NOMOVE   0x02   /* the title bar does not drag                  */
#define GUI_WIN_HIDDEN   0x04   /* created but not composited                   */

/* Event kinds. */
#define GUI_EV_MOUSE_DOWN 1
#define GUI_EV_MOUSE_UP   2
#define GUI_EV_MOUSE_MOVE 3
#define GUI_EV_CLICK      4   /* press and release inside the same widget      */
#define GUI_EV_WHEEL      5
#define GUI_EV_KEY        6   /* one character, to the focused widget          */
#define GUI_EV_SUBMIT     7   /* Enter in a field, or a line handed to it      */
#define GUI_EV_FOCUS      8   /* this window gained focus                     */
#define GUI_EV_BLUR       9   /* this window lost focus                       */
#define GUI_EV_CLOSE     10   /* about to be destroyed; return 1 to refuse     */

typedef struct gui_event {
    uint8_t  kind;
    uint8_t  buttons;      /* MOUSE_BTN_* levels                              */
    uint8_t  pressed;      /* edges, this event                               */
    uint8_t  released;
    int8_t   dz;           /* wheel notches                                   */
    int32_t  x, y;         /* pointer position, RELATIVE to the delivery target
                            * (widget for widget hooks, client area for a
                            * window hook with no widget). Screen coordinates
                            * are never handed to an app: an app that has to
                            * know where its window is cannot be moved. */
    int32_t  key;          /* GUI_EV_KEY: the character                        */
    uint32_t window;       /* window id                                       */
    uint32_t widget;       /* widget id, 0 when it has none / is absent        */
} gui_event_t;

/* An app's hook: called after the widget's own hook declines. Return 1 to mark
 * the event handled. NULL is fine — a window with no hook is a static picture. */
typedef int (*gui_window_fn)(struct gui_window *win, gui_widget_t *w,
                             const gui_event_t *ev);

typedef struct gui_window {
    uint32_t      id;                    /* nonzero, never reused this boot   */
    char          title[GUI_TITLE_MAX];
    gui_rect_t    frame;                 /* SCREEN coordinates, incl. chrome  */
    uint32_t      flags;
    uint8_t       used;
    uint16_t      nwidgets;
    gui_widget_t  widgets[GUI_MAX_WIDGETS];
    gui_window_fn on_event;
    void         *user;                  /* app-private                       */
} gui_window_t;

/* ---- health counters, cumulative for the life of the GUI ---- */
typedef struct gui_stats {
    uint32_t opens, closes;
    uint32_t paints;          /* damage rectangles composited                 */
    uint32_t presents;        /* fb_present_rect calls                        */
    uint32_t merges;          /* damage rectangles merged into a neighbour    */
    uint32_t collapses;       /* times the damage list overflowed             */
    uint32_t mouse_events;    /* events taken from the driver                 */
    uint32_t clicks;          /* GUI_EV_CLICK delivered                      */
    uint32_t keys;            /* characters delivered to a field             */
    uint32_t console_damage;  /* ticks where the console had written          */
    uint32_t scroll_damage;   /* ticks where a console scroll was assumed     */
} gui_stats_t;

/* ====================================================================== */
/* lifecycle                                                              */
/* ====================================================================== */

/* Kernel bring-up: attach to fb_back() and the console grid, size the pointer's
 * clamp box to the framebuffer, and print one line saying what happened. Safe
 * (and a no-op that reports itself) when there is no framebuffer: the VGA text
 * fallback has no pixels to composite into, so the GUI stays unavailable and
 * every entry point below fails legibly instead of drawing into 0xB8000. */
void gui_init(void);

/* The seam gui_init() uses, and the one the host tests use. `screen` is where
 * everything is composited; `cells`/`rows`/`cols` are the text grid painted as
 * the desktop (NULL for a plain black desktop). Returns 0 on success, -1 if the
 * surface is unusable. Calling it again re-attaches and drops all damage;
 * windows survive, because a mode change should not close the operator's apps. */
int  gui_attach(fb_surface_t *screen, volatile uint16_t *cells, int rows, int cols);
void gui_detach(void);
int  gui_attached(void);

/* Non-zero when at least one window is open and visible — i.e. when gui_tick()
 * has anything to do and the pointer is live. */
int  gui_active(void);

void gui_screen_size(int32_t *w, int32_t *h);

/* ====================================================================== */
/* windows                                                                */
/* ====================================================================== */

/* Open a window and focus it on top. Returns its id, or 0 when the pool is full
 * or the GUI is not attached. Size is clamped to [GUI_MIN_*, screen] and the
 * position is nudged so at least the title bar is on screen — a window the
 * operator cannot reach is worse than a window in the wrong place. A NULL or
 * over-long title is truncated, never refused; control characters in it are
 * replaced, because a title is drawn, not printed, and must not carry a newline
 * into the compositor. */
uint32_t gui_window_open(const char *title, int32_t x, int32_t y,
                         int32_t w, int32_t h, uint32_t flags);

/* Destroy it. The window's hook gets GUI_EV_CLOSE first and may refuse by
 * returning 1 (an app with unsaved state can say no). Returns 0 on success, -1
 * for an unknown id, -2 when the app refused. */
int  gui_window_close(uint32_t id);
void gui_close_all(void);

/* NULL for an unknown id. The pointer is stable for the window's lifetime. */
gui_window_t *gui_window(uint32_t id);

int  gui_window_move  (uint32_t id, int32_t x, int32_t y);
int  gui_window_resize(uint32_t id, int32_t w, int32_t h);
int  gui_window_raise (uint32_t id);
int  gui_window_focus (uint32_t id);      /* raises as well                    */
int  gui_window_show  (uint32_t id, int visible);
int  gui_set_hook     (uint32_t id, gui_window_fn fn);
int  gui_set_title    (uint32_t id, const char *title);

int  gui_window_count(void);               /* including hidden ones            */
/* Bottom (0) to top (count-1) in z-order. NULL when out of range. */
gui_window_t *gui_window_at_z(int index);
/* Topmost VISIBLE window whose frame contains the screen point, or 0. */
uint32_t gui_window_at(int32_t x, int32_t y);
uint32_t gui_focused(void);

gui_rect_t gui_frame_rect (const gui_window_t *w);   /* screen, incl. chrome  */
gui_rect_t gui_client_rect(const gui_window_t *w);   /* screen, inside chrome */
gui_rect_t gui_client_area(const gui_window_t *w);   /* 0,0,w,h client space  */

/* ====================================================================== */
/* events                                                                 */
/* ====================================================================== */

/* Feed one mouse event in, exactly as gui_tick() does from the driver. This is
 * the whole input path, so a host test drives the real dispatcher rather than a
 * copy of it. Coordinates are absolute screen pixels. */
void gui_post_mouse(const mouse_event_t *ev);

/* One character to the focused widget of the focused window. Returns 1 if
 * something consumed it. */
int  gui_post_key(int ch);

/* Synthesised interaction, used by the model's tools and by tests: a press and a
 * release at the same point, which is a click by the same rule as the hardware
 * path (release inside the pressed widget). Returns 1 if a widget or chrome
 * consumed it, 0 if it landed on the desktop. */
int  gui_click_at(int32_t x, int32_t y);
/* Click the centre of a widget by id. -1 unknown window, -2 unknown widget,
 * -3 the widget is disabled or not hit-testable, -4 the widget's centre is not
 * inside `win` at all.
 *
 * -4 exists because a widget's rect is not required to lie inside the client
 * area. That is deliberate for DRAWING (the compositor clips, so a widget may
 * hang off an edge) but it is not safe for a SYNTHESISED CLICK: this function
 * projects the rect into screen space and presses that pixel, and the pixel is
 * then re-resolved to whatever window is topmost there. Without the check, a
 * button declared 400px wide inside a 200px-wide window presses a DIFFERENT
 * window's widget, runs that app's handler, and moves the keyboard grab - while
 * the caller is told it pressed its own button. Refusing is right rather than
 * clamping: the widget genuinely is not where the caller thinks it is, and a
 * clamped press would be a different lie. */
int  gui_click_widget(uint32_t win, uint32_t widget_id);

/* Service the GUI: drain the pointer, notice what the console wrote, repaint
 * what is wrong. Bounded, never blocks, and returns immediately when no window
 * is open. Call it from any loop that would otherwise spin. */
void gui_tick(void);

/* Non-zero when a click has armed the one-line keyboard grab: the next line the
 * operator types goes into a field instead of to the model. */
int  gui_wants_keys(void);

/* Title of the window that holds the grab, or NULL when nothing does.
 *
 * This exists so the PROMPT ITSELF can say where the next line is going, and
 * that matters because gui_click_at() is reached by the model's gui_click tool
 * as well as by a physical click — the window manager cannot tell them apart and
 * should not, since a synthesised click is meant to be indistinguishable. So a
 * model that clicks a text field for any reason, including a mis-aimed click
 * resolved by label, takes ownership of the operator's NEXT sentence. The
 * advisory printed when the grab is armed scrolls away; a prompt cannot. On a
 * machine whose only interface is a typed sentence, "you cannot be surprised
 * about where your words went" is the property worth having. Sanitised at the
 * source (window titles are sanitised on open), so it is safe to print. */
const char *gui_key_target(void);

/* Offer a completed line to the armed field. Returns 1 when the GUI took it (the
 * caller must then NOT send it to the model), 0 when it did not. Consumes the
 * grab either way, so the loop can never be captured for more than one line. */
int  gui_take_line(const char *line, int len);

/* ====================================================================== */
/* damage and painting                                                    */
/* ====================================================================== */

void gui_invalidate(int32_t x, int32_t y, int32_t w, int32_t h);
void gui_invalidate_window(uint32_t id);
void gui_invalidate_widget(uint32_t id, const gui_widget_t *w);
void gui_invalidate_all(void);

/* Composite and present every pending rectangle. Called by gui_sync(); exposed
 * so a test (or an app that just built its widgets) can force it. */
void gui_flush(void);

/* Notice what the console wrote and repaint what is wrong — gui_tick() without
 * the input half. This is what a caller uses when it has just changed something
 * and wants it on screen NOW but must not dispatch events: the model's tools do
 * exactly that, because dispatching a mouse event from inside a tool could close
 * the window the tool is holding a pointer to. */
void gui_sync(void);

int        gui_damage_count(void);
gui_rect_t gui_damage_rect(int index);

/* ====================================================================== */
/* the pointer                                                            */
/* ====================================================================== */

void gui_pointer_show(int visible);
int  gui_pointer_visible(void);
void gui_pointer_position(int32_t *x, int32_t *y);
void gui_pointer_warp(int32_t x, int32_t y);

const gui_stats_t *gui_stats(void);

/* ====================================================================== */
/* the reference app (gui/gui_demo.c)                                     */
/* ====================================================================== */

/* Open the built-in calculator. Returns its window id, or 0. This is the worked
 * example the app-authoring phase replaces with model-authored apps; it exists
 * so that "I want a calculator" has something real behind it today, and so that
 * the toolkit is exercised end to end by tests/host/test_gui.c and by a boot. */
uint32_t gui_demo_calculator(int32_t x, int32_t y);

/* The built-in apps, by name, for the gui_open tool. Returns a window id or 0;
 * *why (may be NULL) points at a static reason string on failure. */
uint32_t gui_demo_open(const char *name, const char **why);

/* NUL-terminated list of built-in app names, for a schema and an error message. */
const char *const *gui_demo_names(void);

#ifdef FABLEOS_HOSTTEST
/* The scroll heuristic in gui_tick() needs the console's cursor row, which on a
 * host has no console behind it. A test supplies it, exactly as
 * tools/screen_tools.c lets a test supply a framebuffer. Kernel builds read
 * lib/base.c's real cursor and this declaration does not exist. */
void gui_test_set_console_cursor(int row, int col);
/* Stands in for lib/base.c's scroll counter. A test that emulates a console
 * scroll must call this once per row shifted, or the compositor will (correctly)
 * conclude nothing moved and leave the smear the test is looking for. */
void gui_test_console_scrolled(void);
#endif

#ifdef FABLEOS_GUI_SELFTEST
/* Diagnostic builds only: open the reference apps on the first tick so a real
 * mouse can be driven at them. See gui/gui_demo.c. */
void gui_demo_selftest(void);
#endif

#endif /* GUI_H */
