/* fb.h — the linear framebuffer: pixels, rectangles, blits, clipped text, and
 *        double buffering.
 *
 * PURPOSE
 *   Everything this machine can draw. The console (lib/base.c) is the first
 *   consumer and the GUI phase is the intended one, so the API is shaped for the
 *   GUI and the console is expressed in terms of it, not the other way round.
 *   Concretely, that means:
 *
 *     - Drawing is against a SURFACE, not against "the screen". A surface is a
 *       pointer, a size, a stride and a clip rectangle. The hardware framebuffer
 *       is one surface; a window's backing store is another; a host test's
 *       malloc'd array is a third, which is the only reason exact-pixel tests are
 *       possible at all (tests/host/test_fb.c).
 *     - Every drawing call is CLIPPED, never bounds-checked-and-refused. A GUI
 *       draws things that hang off the edge constantly — a window at negative x,
 *       a label longer than its box — and a widget toolkit that has to
 *       pre-intersect every rectangle itself is a toolkit nobody will use
 *       correctly. Coordinates are signed for the same reason.
 *     - Colour is one format, everywhere: 32-bit 0x00RRGGBB. The hardware's
 *       actual pixel layout (32bpp XRGB, 24bpp packed, 16bpp 565) is a detail of
 *       fb_present() and appears nowhere else.
 *     - Text is a drawing operation at a PIXEL position, with the full font
 *       (~1000 glyphs) behind it. The 80-column text grid is the console's
 *       problem; a GUI wants a string at (x,y) inside a clip rect.
 *
 * DOUBLE BUFFERING, AND WHY THE CONSOLE STILL FEELS FAST
 *   fb_back() is a RAM surface; the hardware framebuffer is write-only as far as
 *   this code is concerned. Everything draws into the back buffer and
 *   fb_present_rect() copies a region forward, converting pixel format on the
 *   way. That gives a GUI tear-free compositing for free, and it makes scrolling
 *   possible at all (reading back 3 MiB of video memory to shift it up 16 rows
 *   would be miserable; a memmove in RAM is not).
 *
 *   The console presents one 8x16 cell per character — 512 bytes — rather than
 *   the whole screen, so the cost of double buffering per printed character is
 *   about the same as writing the character. A full-screen present happens only
 *   when the console scrolls.
 *
 * WHERE THE FRAMEBUFFER COMES FROM
 *   fb_hw_init() tries, in order:
 *
 *     1. The MULTIBOOT FRAMEBUFFER TAG. boot/boot.asm asks for a linear mode in
 *        its Multiboot v1 header (flags bit 2) and the loader answers in the
 *        info structure (flags bit 12) with an address, a pitch, a depth and the
 *        red/green/blue field positions. GRUB does this. It is the only portable
 *        way to get a framebuffer on a machine that has already left real mode,
 *        because setting a VBE mode needs the BIOS and the BIOS needs real mode.
 *
 *     2. The BOCHS/QEMU VBE EXTENSIONS (ports 0x1CE/0x1CF), if the machine has
 *        the QEMU standard VGA (PCI 1234:1111). QEMU's built-in `-kernel` loader
 *        does NOT implement the framebuffer tag, so without this tier the
 *        project's primary run target would never exercise the framebuffer at
 *        all — the display would be tested only on the ISO path and would rot.
 *        The probe is narrow on purpose: the PCI ID must match AND the DISPI ID
 *        register must read back 0xB0Cx, so it cannot fire on hardware that
 *        merely happens to answer on those ports.
 *
 *     3. Nothing. fb_hw_init() fails, fb_back() stays NULL, and lib/base.c falls
 *        back to VGA text mode. A machine can never end up with no console.
 *
 *   `console=vga` on the kernel command line skips 1 and 2, which is how a
 *   suspected console regression is bisected without a rebuild.
 *
 * PUBLIC API
 *   surfaces   fb_surface_init fb_clip_reset fb_clip_set fb_clip_get
 *   pixels     fb_put_pixel fb_get_pixel fb_clear
 *   rectangles fb_fill_rect fb_frame_rect fb_hline fb_vline
 *   copying    fb_blit fb_blit_keyed fb_scroll
 *   text       fb_draw_glyph fb_draw_codepoint fb_draw_utf8 fb_utf8_columns
 *   hardware   fb_hw_init fb_hw_info fb_back fb_present fb_present_rect
 *   boot data  multiboot_cmdline fb_forced_off
 *
 * DEPENDENCIES
 *   font.h for glyphs and UTF-8. io.h and the multiboot info structure only
 *   inside the hardware half, which is compiled out under FABLEOS_HOSTTEST so the
 *   drawing half is testable natively.
 *
 * FUTURE EXTENSION POINTS
 *   - fb_blit takes two surfaces, so off-screen composition (a window redrawing
 *     itself, then appearing at once) needs nothing new.
 *   - fb_blit_keyed's colour key is the cheap stand-in for an alpha channel; an
 *     fb_blit_alpha() would slot in beside it without changing fb_surface_t.
 *   - fb_surface_t carries a stride, so a sub-surface (a window's rectangle of
 *     the screen, drawn into directly with its own clip) is just a pointer into
 *     someone else's pixels.
 */
#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>

#include "font.h"

/* ====================================================================== */
/* colour                                                                 */
/* ====================================================================== */

/* 0x00RRGGBB. The top byte is reserved (it reads back as 0), not alpha. */
typedef uint32_t fb_color_t;

#define FB_RGB(r, g, b) ((fb_color_t)(((uint32_t)(uint8_t)(r) << 16) | \
                                      ((uint32_t)(uint8_t)(g) << 8)  | \
                                       (uint32_t)(uint8_t)(b)))

/* The console's palette, and a reasonable default for a GUI: the sixteen VGA
 * text colours, so an attribute byte written through console_fb() means the
 * same thing whether the console is drawing pixels or driving VGA text mode. */
extern const fb_color_t fb_vga_palette[16];

/* ====================================================================== */
/* surfaces                                                               */
/* ====================================================================== */

typedef struct {
    fb_color_t *px;        /* row-major; row y starts at px + y*stride       */
    int32_t     w, h;      /* pixels                                         */
    int32_t     stride;    /* pixels per row (>= w)                          */
    /* Clip rectangle, always a subset of [0,w) x [0,h). Every drawing call
     * respects it; nothing can write outside it, ever. */
    int32_t     cx, cy, cw, ch;
} fb_surface_t;

/* 0 on success. Rejects a NULL pointer, a non-positive size, or a stride
 * narrower than the width — the three shapes that would let a later draw run
 * off the end of the allocation. On failure the surface is left zeroed and every
 * other call on it is a safe no-op. */
int  fb_surface_init(fb_surface_t *s, fb_color_t *px,
                     int32_t w, int32_t h, int32_t stride);

/* Clip to the whole surface. */
void fb_clip_reset(fb_surface_t *s);

/* INTERSECT the clip rectangle with (x,y,w,h). Intersect, not replace, so a
 * caller can narrow a clip without being able to widen it past its parent's —
 * which is what a nested widget needs. Returns 0 if anything remains, -1 if the
 * result is empty (in which case the clip is set empty and drawing is a no-op).
 * Use fb_clip_reset() first to start from the whole surface. */
int  fb_clip_set(fb_surface_t *s, int32_t x, int32_t y, int32_t w, int32_t h);
void fb_clip_get(const fb_surface_t *s, int32_t *x, int32_t *y,
                 int32_t *w, int32_t *h);

/* ====================================================================== */
/* drawing — every call clips, every call tolerates absurd coordinates     */
/* ====================================================================== */

void fb_clear(fb_surface_t *s, fb_color_t c);      /* fills the clip rect    */
void fb_put_pixel(fb_surface_t *s, int32_t x, int32_t y, fb_color_t c);

/* Colour at (x,y), or 0 outside the surface. Reads ignore the clip rectangle:
 * clipping bounds what may be MODIFIED, and a read that silently returned 0 for
 * a pixel that exists would make a test of the clip logic circular. */
fb_color_t fb_get_pixel(const fb_surface_t *s, int32_t x, int32_t y);

void fb_fill_rect(fb_surface_t *s, int32_t x, int32_t y,
                  int32_t w, int32_t h, fb_color_t c);
/* Outline, `t` pixels thick, drawn INSIDE (x,y,w,h). */
void fb_frame_rect(fb_surface_t *s, int32_t x, int32_t y,
                   int32_t w, int32_t h, int32_t t, fb_color_t c);
void fb_hline(fb_surface_t *s, int32_t x, int32_t y, int32_t w, fb_color_t c);
void fb_vline(fb_surface_t *s, int32_t x, int32_t y, int32_t h, fb_color_t c);

/* Copy a w*h block from src(sx,sy) to dst(dx,dy). Clipped on both sides: a
 * source rectangle that hangs off `src` copies only the part that exists.
 * Overlapping copies within one surface are handled (memmove semantics). */
void fb_blit(fb_surface_t *dst, int32_t dx, int32_t dy,
             const fb_surface_t *src, int32_t sx, int32_t sy,
             int32_t w, int32_t h);

/* As fb_blit, but source pixels equal to `key` are left untouched in dst. */
void fb_blit_keyed(fb_surface_t *dst, int32_t dx, int32_t dy,
                   const fb_surface_t *src, int32_t sx, int32_t sy,
                   int32_t w, int32_t h, fb_color_t key);

/* Move the clip rectangle's contents up by `dy` pixels (down if negative) and
 * fill the vacated band with `fill`. This is the console's scroll. */
void fb_scroll(fb_surface_t *s, int32_t dy, fb_color_t fill);

/* ====================================================================== */
/* text                                                                   */
/* ====================================================================== */

/* Draw one glyph with its top-left at (x,y). `opaque` fills the glyph's whole
 * cell with `bg` first; otherwise only set pixels are touched, which is what
 * drawing a label over a background needs. Returns the horizontal advance in
 * pixels (the font's width) so a caller can lay out without knowing the font. */
int fb_draw_glyph(fb_surface_t *s, const font_t *f, int32_t x, int32_t y,
                  int glyph, fb_color_t fg, fb_color_t bg, int opaque);

/* As above by code point. An unmapped code point draws the font's replacement
 * glyph — visibly wrong, never silently nothing and never a stray letter. */
int fb_draw_codepoint(fb_surface_t *s, const font_t *f, int32_t x, int32_t y,
                      uint32_t cp, fb_color_t fg, fb_color_t bg, int opaque);

/* Draw utf8[0..n) left to right from (x,y). Malformed input draws replacement
 * glyphs and keeps going (see font.h for what "malformed" covers). Newlines and
 * tabs are NOT interpreted: this is a text-drawing call, not a terminal.
 * Returns the total advance in pixels, which is the same value whether or not
 * anything was clipped away. */
int fb_draw_utf8(fb_surface_t *s, const font_t *f, int32_t x, int32_t y,
                 const char *utf8, size_t n,
                 fb_color_t fg, fb_color_t bg, int opaque);

/* How many glyph cells utf8[0..n) will occupy. Counts a malformed byte as one
 * replacement glyph, so it always agrees with fb_draw_utf8(). */
int fb_utf8_columns(const char *utf8, size_t n);

/* ====================================================================== */
/* the hardware framebuffer                                               */
/* ====================================================================== */

typedef struct {
    int         active;
    uint64_t    phys;           /* linear address of the front buffer        */
    int32_t     width, height;
    uint32_t    pitch;          /* BYTES per front-buffer row                */
    uint8_t     bpp;
    uint8_t     red_pos,  red_size;
    uint8_t     green_pos, green_size;
    uint8_t     blue_pos,  blue_size;
    const char *source;         /* "multiboot", "bochs-vbe", or "none"       */
} fb_hwinfo_t;

/* Find and claim a framebuffer, and set up the back buffer. 0 on success.
 * Idempotent: a second call returns the first call's verdict. */
int fb_hw_init(void);

const fb_hwinfo_t *fb_hw_info(void);

/* The back buffer surface, or NULL when there is no framebuffer. Its clip
 * rectangle belongs to whoever is drawing; the console resets it before use. */
fb_surface_t *fb_back(void);

void fb_present(void);
void fb_present_rect(int32_t x, int32_t y, int32_t w, int32_t h);

/* The boot loader's command line, or "" if there is none. Read straight out of
 * the multiboot info structure; never NULL. */
const char *multiboot_cmdline(void);

/* Non-zero when the command line says `console=vga` (or `novideo`), i.e. the
 * operator asked for the VGA text console even if a framebuffer is available. */
int fb_forced_off(void);

/* Largest framebuffer this kernel will drive. The back buffer is statically
 * reserved at this size (it must exist before the heap does), so a loader that
 * hands us something bigger is declined and the VGA text console is used
 * instead — a wrong-sized buffer would be a silent out-of-bounds write. */
#define FB_MAX_WIDTH   1280
#define FB_MAX_HEIGHT  1024

/* And the smallest. Below this there is no room for a console (40x10 characters
 * at 8x16), and the size test has to live down here rather than in lib/base.c:
 * once a loader has put the adapter into a graphics mode, "use the framebuffer
 * or fall back to VGA text" is no longer a choice anyone can make — 0xB8000 is
 * not on screen any more. So fb_hw_init() declines an unusable mode while it can
 * still go and set a usable one itself. */
#define FB_MIN_WIDTH   320
#define FB_MIN_HEIGHT  160

#endif /* FB_H */
