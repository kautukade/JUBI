/* fb.c — the linear framebuffer. Contract and design rationale: include/fb.h.
 *
 * Two halves, split by FABLEOS_HOSTTEST:
 *
 *   DRAWING is pure arithmetic over a caller-supplied pixel array. It compiles
 *   and runs on the host, which is the only way to assert exact pixels — every
 *   clip edge, every glyph row, every scroll — without a VM. tests/host/test_fb.c
 *   does exactly that.
 *
 *   HARDWARE (multiboot tag, Bochs VBE, present) touches ports, a 5 MiB static
 *   back buffer and a physical address, so it is compiled out on the host.
 *
 * ARITHMETIC RULE FOR THE WHOLE FILE
 *   Coordinates are int32_t and come from callers that may be wrong (a GUI
 *   computing a window position, eventually a model choosing one). Every
 *   x+w / y+h is evaluated in int64_t before it is compared or truncated, so no
 *   clip decision can be made on an overflowed sum. That is the one thing that
 *   would turn "draws in the wrong place" into "writes outside the buffer".
 */

#include "fb.h"

/* ====================================================================== */
/* colour                                                                 */
/* ====================================================================== */

/* The IBM VGA 16-colour text palette, so an attribute byte written into the
 * console grid means the same colour whether the console is painting pixels or
 * driving real VGA text mode. */
const fb_color_t fb_vga_palette[16] = {
    FB_RGB(0x00, 0x00, 0x00), FB_RGB(0x00, 0x00, 0xAA),
    FB_RGB(0x00, 0xAA, 0x00), FB_RGB(0x00, 0xAA, 0xAA),
    FB_RGB(0xAA, 0x00, 0x00), FB_RGB(0xAA, 0x00, 0xAA),
    FB_RGB(0xAA, 0x55, 0x00), FB_RGB(0xAA, 0xAA, 0xAA),
    FB_RGB(0x55, 0x55, 0x55), FB_RGB(0x55, 0x55, 0xFF),
    FB_RGB(0x55, 0xFF, 0x55), FB_RGB(0x55, 0xFF, 0xFF),
    FB_RGB(0xFF, 0x55, 0x55), FB_RGB(0xFF, 0x55, 0xFF),
    FB_RGB(0xFF, 0xFF, 0x55), FB_RGB(0xFF, 0xFF, 0xFF),
};

/* ====================================================================== */
/* surfaces and clipping                                                  */
/* ====================================================================== */

int fb_surface_init(fb_surface_t *s, fb_color_t *px,
                    int32_t w, int32_t h, int32_t stride) {
    if (!s) return -1;
    /* Zero first: a rejected surface must be inert, not half-configured. */
    s->px = (fb_color_t *)0;
    s->w = s->h = s->stride = 0;
    s->cx = s->cy = s->cw = s->ch = 0;

    if (!px || w <= 0 || h <= 0 || stride < w) return -1;
    /* The last pixel is (h-1)*stride + w-1; make sure that fits in the range a
     * size_t index can address without wrapping. */
    if ((int64_t)h * (int64_t)stride > (int64_t)(1u << 30)) return -1;

    s->px     = px;
    s->w      = w;
    s->h      = h;
    s->stride = stride;
    fb_clip_reset(s);
    return 0;
}

void fb_clip_reset(fb_surface_t *s) {
    if (!s) return;
    s->cx = 0;
    s->cy = 0;
    s->cw = s->w;
    s->ch = s->h;
}

int fb_clip_set(fb_surface_t *s, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!s) return -1;

    int64_t ax = x, ay = y;
    int64_t bx = (int64_t)x + (w > 0 ? w : 0);
    int64_t by = (int64_t)y + (h > 0 ? h : 0);

    int64_t lx = s->cx, ly = s->cy;
    int64_t hx = (int64_t)s->cx + s->cw, hy = (int64_t)s->cy + s->ch;

    if (ax > lx) lx = ax;
    if (ay > ly) ly = ay;
    if (bx < hx) hx = bx;
    if (by < hy) hy = by;

    if (hx <= lx || hy <= ly) {
        s->cx = s->cy = s->cw = s->ch = 0;
        return -1;
    }
    s->cx = (int32_t)lx;
    s->cy = (int32_t)ly;
    s->cw = (int32_t)(hx - lx);
    s->ch = (int32_t)(hy - ly);
    return 0;
}

void fb_clip_get(const fb_surface_t *s, int32_t *x, int32_t *y,
                 int32_t *w, int32_t *h) {
    if (x) *x = s ? s->cx : 0;
    if (y) *y = s ? s->cy : 0;
    if (w) *w = s ? s->cw : 0;
    if (h) *h = s ? s->ch : 0;
}

/* Usable? A zeroed surface answers "no" and every drawing call returns early. */
static int live(const fb_surface_t *s) {
    return s && s->px && s->w > 0 && s->h > 0 && s->cw > 0 && s->ch > 0;
}

/* Intersect (x,y,w,h) with the clip rectangle, in place. 0 if anything is left. */
static int clip_to(const fb_surface_t *s, int32_t *x, int32_t *y,
                   int32_t *w, int32_t *h) {
    if (*w <= 0 || *h <= 0) return -1;

    int64_t x0 = *x, y0 = *y;
    int64_t x1 = x0 + *w, y1 = y0 + *h;

    if (x0 < s->cx) x0 = s->cx;
    if (y0 < s->cy) y0 = s->cy;
    if (x1 > (int64_t)s->cx + s->cw) x1 = (int64_t)s->cx + s->cw;
    if (y1 > (int64_t)s->cy + s->ch) y1 = (int64_t)s->cy + s->ch;

    if (x1 <= x0 || y1 <= y0) return -1;
    *x = (int32_t)x0;
    *y = (int32_t)y0;
    *w = (int32_t)(x1 - x0);
    *h = (int32_t)(y1 - y0);
    return 0;
}

/* ====================================================================== */
/* pixels and rectangles                                                  */
/* ====================================================================== */

void fb_put_pixel(fb_surface_t *s, int32_t x, int32_t y, fb_color_t c) {
    if (!live(s)) return;
    if (x < s->cx || y < s->cy) return;
    if (x >= s->cx + s->cw || y >= s->cy + s->ch) return;
    s->px[(size_t)y * (size_t)s->stride + (size_t)x] = c;
}

fb_color_t fb_get_pixel(const fb_surface_t *s, int32_t x, int32_t y) {
    if (!s || !s->px) return 0;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    return s->px[(size_t)y * (size_t)s->stride + (size_t)x];
}

void fb_fill_rect(fb_surface_t *s, int32_t x, int32_t y,
                  int32_t w, int32_t h, fb_color_t c) {
    if (!live(s)) return;
    if (clip_to(s, &x, &y, &w, &h) != 0) return;

    for (int32_t row = 0; row < h; row++) {
        fb_color_t *p = s->px + (size_t)(y + row) * (size_t)s->stride + (size_t)x;
        for (int32_t col = 0; col < w; col++) p[col] = c;
    }
}

void fb_clear(fb_surface_t *s, fb_color_t c) {
    if (!live(s)) return;
    fb_fill_rect(s, s->cx, s->cy, s->cw, s->ch, c);
}

void fb_hline(fb_surface_t *s, int32_t x, int32_t y, int32_t w, fb_color_t c) {
    fb_fill_rect(s, x, y, w, 1, c);
}

void fb_vline(fb_surface_t *s, int32_t x, int32_t y, int32_t h, fb_color_t c) {
    fb_fill_rect(s, x, y, 1, h, c);
}

void fb_frame_rect(fb_surface_t *s, int32_t x, int32_t y,
                   int32_t w, int32_t h, int32_t t, fb_color_t c) {
    if (t <= 0 || w <= 0 || h <= 0) return;
    /* A border thicker than half the box IS a filled box. Saying so here keeps
     * the four bars below from being computed with a negative height. */
    if ((int64_t)t * 2 >= w || (int64_t)t * 2 >= h) {
        fb_fill_rect(s, x, y, w, h, c);
        return;
    }
    fb_fill_rect(s, x, y, w, t, c);                       /* top    */
    fb_fill_rect(s, x, y + h - t, w, t, c);               /* bottom */
    fb_fill_rect(s, x, y + t, t, h - 2 * t, c);           /* left   */
    fb_fill_rect(s, x + w - t, y + t, t, h - 2 * t, c);   /* right  */
}

/* ====================================================================== */
/* blits                                                                  */
/* ====================================================================== */

/* Shared setup: clamp (sx,sy,w,h) into `src`, carry the same adjustment over to
 * (dx,dy), then clip the destination. 0 if anything survives. */
static int blit_clip(fb_surface_t *dst, int32_t *dx, int32_t *dy,
                     const fb_surface_t *src, int32_t *sx, int32_t *sy,
                     int32_t *w, int32_t *h) {
    if (!live(dst) || !src || !src->px || src->w <= 0 || src->h <= 0) return -1;
    if (*w <= 0 || *h <= 0) return -1;

    /* Source side: only the part of the source rectangle that exists. */
    if (*sx < 0) { int64_t d = -(int64_t)*sx; if (d >= *w) return -1;
                   *dx = (int32_t)(*dx + d); *w = (int32_t)(*w - d); *sx = 0; }
    if (*sy < 0) { int64_t d = -(int64_t)*sy; if (d >= *h) return -1;
                   *dy = (int32_t)(*dy + d); *h = (int32_t)(*h - d); *sy = 0; }
    if (*sx >= src->w || *sy >= src->h) return -1;
    if ((int64_t)*sx + *w > src->w) *w = src->w - *sx;
    if ((int64_t)*sy + *h > src->h) *h = src->h - *sy;
    if (*w <= 0 || *h <= 0) return -1;

    /* Destination side: clip, and shift the source origin by the same amount. */
    int32_t cx = *dx, cy = *dy, cw = *w, ch = *h;
    if (clip_to(dst, &cx, &cy, &cw, &ch) != 0) return -1;
    *sx += cx - *dx;
    *sy += cy - *dy;
    *dx  = cx;
    *dy  = cy;
    *w   = cw;
    *h   = ch;
    return 0;
}

void fb_blit(fb_surface_t *dst, int32_t dx, int32_t dy,
             const fb_surface_t *src, int32_t sx, int32_t sy,
             int32_t w, int32_t h) {
    if (blit_clip(dst, &dx, &dy, src, &sx, &sy, &w, &h) != 0) return;

    /* Same pixels, overlapping: pick a row and column order that reads before it
     * writes. Copying a surface onto itself IS the console's scroll. */
    int overlap = (dst->px == src->px);
    int rev_row = overlap && dy > sy;
    int rev_col = overlap && dy == sy && dx > sx;

    for (int32_t r = 0; r < h; r++) {
        int32_t           row = rev_row ? h - 1 - r : r;
        fb_color_t       *d   = dst->px + (size_t)(dy + row) * (size_t)dst->stride + (size_t)dx;
        const fb_color_t *sp  = src->px + (size_t)(sy + row) * (size_t)src->stride + (size_t)sx;
        if (rev_col) for (int32_t c = w - 1; c >= 0; c--) d[c] = sp[c];
        else         for (int32_t c = 0; c < w; c++)      d[c] = sp[c];
    }
}

void fb_blit_keyed(fb_surface_t *dst, int32_t dx, int32_t dy,
                   const fb_surface_t *src, int32_t sx, int32_t sy,
                   int32_t w, int32_t h, fb_color_t key) {
    if (blit_clip(dst, &dx, &dy, src, &sx, &sy, &w, &h) != 0) return;

    int rev_row = (dst->px == src->px) && dy > sy;
    for (int32_t r = 0; r < h; r++) {
        int32_t           row = rev_row ? h - 1 - r : r;
        fb_color_t       *d   = dst->px + (size_t)(dy + row) * (size_t)dst->stride + (size_t)dx;
        const fb_color_t *sp  = src->px + (size_t)(sy + row) * (size_t)src->stride + (size_t)sx;
        for (int32_t c = 0; c < w; c++)
            if (sp[c] != key) d[c] = sp[c];
    }
}

void fb_scroll(fb_surface_t *s, int32_t dy, fb_color_t fill) {
    if (!live(s) || dy == 0) return;

    int32_t x = s->cx, y = s->cy, w = s->cw, h = s->ch;
    int32_t shift = dy > 0 ? dy : -dy;
    if (shift >= h) { fb_fill_rect(s, x, y, w, h, fill); return; }

    if (dy > 0) {              /* contents move up; the bottom band is vacated */
        fb_blit(s, x, y, s, x, y + shift, w, h - shift);
        fb_fill_rect(s, x, y + h - shift, w, shift, fill);
    } else {                   /* contents move down; the top band is vacated  */
        fb_blit(s, x, y + shift, s, x, y, w, h - shift);
        fb_fill_rect(s, x, y, w, shift, fill);
    }
}

/* ====================================================================== */
/* text                                                                   */
/* ====================================================================== */

int fb_draw_glyph(fb_surface_t *s, const font_t *f, int32_t x, int32_t y,
                  int glyph, fb_color_t fg, fb_color_t bg, int opaque) {
    if (!f || f->width == 0 || f->height == 0) return 0;
    int adv = f->width;
    if (!live(s)) return adv;

    const uint8_t *bits = font_glyph_bits(f, glyph);
    if (!bits) bits = font_glyph_bits(f, FONT_GLYPH_REPLACEMENT);
    if (!bits) return adv;

    /* Work out which rows and columns of the glyph survive the clip once, then
     * draw only those — a glyph half off the left edge must not walk the row. */
    int32_t rx = x, ry = y, rw = f->width, rh = f->height;
    if (clip_to(s, &rx, &ry, &rw, &rh) != 0) return adv;

    for (int32_t row = ry; row < ry + rh; row++) {
        const uint8_t *r = bits + (size_t)(row - y) * (size_t)f->stride;
        fb_color_t    *d = s->px + (size_t)row * (size_t)s->stride;
        for (int32_t col = rx; col < rx + rw; col++) {
            int32_t gx  = col - x;                     /* 0..width-1          */
            uint8_t byte = r[gx >> 3];
            int     on   = (byte >> (7 - (gx & 7))) & 1;
            if (on)          d[col] = fg;
            else if (opaque) d[col] = bg;
        }
    }
    return adv;
}

int fb_draw_codepoint(fb_surface_t *s, const font_t *f, int32_t x, int32_t y,
                      uint32_t cp, fb_color_t fg, fb_color_t bg, int opaque) {
    int g = font_glyph_index(f, cp);
    if (g < 0) g = FONT_GLYPH_REPLACEMENT;
    return fb_draw_glyph(s, f, x, y, g, fg, bg, opaque);
}

int fb_draw_utf8(fb_surface_t *s, const font_t *f, int32_t x, int32_t y,
                 const char *utf8, size_t n,
                 fb_color_t fg, fb_color_t bg, int opaque) {
    if (!f || !utf8) return 0;
    int32_t pen = x;
    size_t  i   = 0;
    while (i < n) {
        uint32_t cp;
        size_t   used = utf8_decode(utf8 + i, n - i, &cp);
        if (used == 0) break;
        i += used;
        pen += fb_draw_codepoint(s, f, pen, y, cp, fg, bg, opaque);
    }
    return (int)(pen - x);
}

int fb_utf8_columns(const char *utf8, size_t n) {
    if (!utf8) return 0;
    int    cols = 0;
    size_t i    = 0;
    while (i < n) {
        uint32_t cp;
        size_t   used = utf8_decode(utf8 + i, n - i, &cp);
        if (used == 0) break;
        i += used;
        cols++;
    }
    return cols;
}

/* ====================================================================== */
/* the hardware framebuffer                                               */
/* ====================================================================== */

#ifdef FABLEOS_HOSTTEST

/* No ports, no 5 MiB of .bss, no physical addresses in a host process. The
 * drawing half above is the part worth testing and it needs none of this. */
static const fb_hwinfo_t no_hw = { .active = 0, .source = "none" };

int                 fb_hw_init(void)   { return -1; }
const fb_hwinfo_t  *fb_hw_info(void)   { return &no_hw; }
fb_surface_t       *fb_back(void)      { return (fb_surface_t *)0; }
void                fb_present(void)   { }
void fb_present_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)x; (void)y; (void)w; (void)h;
}
const char *multiboot_cmdline(void) { return ""; }
int         fb_forced_off(void)     { return 0; }

#else /* kernel */

#include "io.h"

/* --- the multiboot v1 information structure ---
 * Field offsets are fixed by the spec; `flags` says which are meaningful. Only
 * three matter here: the command line (bit 2) and the framebuffer block
 * (bit 12). boot/boot.asm stashes the pointer the loader left in EBX. */
typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    /* color_info. Bytes, not a union, because WHERE IT STARTS IS NOT AGREED.
     *
     * The Multiboot 0.6.96 document puts color_info at offset 110, immediately
     * after framebuffer_type. GRUB's own multiboot.h declares it as an unnamed
     * union whose first member begins with a uint32_t palette address — and that
     * header is NOT packed, so the union is 4-byte aligned and GRUB actually
     * writes the field at offset 112, with two bytes of padding at 110.
     *
     * Both are "correct" from where they stand, and the loaders that matter
     * disagree, so this reads bytes and rgb_channels() below tries both. It was
     * found the hard way: at offset 110 GRUB's framebuffer looked like an RGB
     * mode with a zero-width red channel, so the tag was declined and the
     * kernel fell through to the Bochs path with nothing to say about why. */
    uint8_t  color_info[10];
} multiboot_info_t;

#define MB_INFO_CMDLINE     (1u << 2)
#define MB_INFO_FRAMEBUFFER (1u << 12)

#define MB_FB_INDEXED 0
#define MB_FB_RGB     1
#define MB_FB_EGA     2

/* Set by boot/boot.asm from EBX before .bss is touched. Zero if we were not
 * multiboot-loaded at all, which is not a case that can happen (boot.asm halts
 * on a bad magic) but is still the safe reading of a null pointer. */
extern uint32_t multiboot_info_phys;

static const multiboot_info_t *mbi(void) {
    if (!multiboot_info_phys) return (const multiboot_info_t *)0;
    return (const multiboot_info_t *)(uintptr_t)multiboot_info_phys;
}

const char *multiboot_cmdline(void) {
    const multiboot_info_t *m = mbi();
    if (!m || !(m->flags & MB_INFO_CMDLINE) || !m->cmdline) return "";
    return (const char *)(uintptr_t)m->cmdline;
}

/* Whitespace-delimited token match, so `console=vganot` is not `console=vga`. */
static int cmdline_has(const char *want) {
    const char *s = multiboot_cmdline();
    size_t      n = 0;
    while (want[n]) n++;

    size_t i = 0;
    while (s[i]) {
        while (s[i] == ' ' || s[i] == '\t') i++;
        if (!s[i]) break;
        size_t k = 0;
        while (k < n && s[i + k] == want[k]) k++;
        if (k == n) {
            char a = s[i + n];
            if (a == '\0' || a == ' ' || a == '\t') return 1;
        }
        while (s[i] && s[i] != ' ' && s[i] != '\t') i++;
    }
    return 0;
}

int fb_forced_off(void) {
    return cmdline_has("console=vga") || cmdline_has("novideo");
}

/* --- the back buffer ---
 * Static because the console must exist before the heap does: console_init() is
 * the first line of kernel_main() and kmalloc is not available to it. FB_MAX_*
 * is the contract that makes this safe — a mode larger than this is declined in
 * fb_hw_init() rather than scribbled past the end of the array. */
static fb_color_t   back_px[(size_t)FB_MAX_WIDTH * (size_t)FB_MAX_HEIGHT];
static fb_surface_t back;
static fb_hwinfo_t  hw = { .active = 0, .source = "none" };
static int          hw_tried;

/* Front-buffer bytes, as a byte pointer: the pixel size may not be 4. */
static volatile uint8_t *front;

/* --- Bochs / QEMU standard-VGA VBE extensions ---------------------------- */

#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF

#define DISPI_ID          0
#define DISPI_XRES        1
#define DISPI_YRES        2
#define DISPI_BPP         3
#define DISPI_ENABLE      4
#define DISPI_BANK        5
#define DISPI_VIRT_WIDTH  6

#define DISPI_ENABLED     0x01
#define DISPI_LFB_ENABLED 0x40

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_INDEX, index);
    return inw(DISPI_DATA);
}

/* One 32-bit config-space read. drivers/pci/pci.c owns PCI properly, but it has
 * not run yet — console_init() is the first thing kernel_main() does, and a
 * console that waits for the driver framework is a console that cannot report
 * the driver framework failing. Two ports and one register is a small enough
 * duplication to accept for that. */
static uint32_t pci_cfg32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

/* Linear-framebuffer base of the QEMU standard VGA / bochs-display, or 0. */
#define QEMU_STDVGA_ID 0x11111234u   /* device 0x1111, vendor 0x1234 */

static uint32_t stdvga_lfb_base(void) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        if (pci_cfg32(0, dev, 0, 0x00) != QEMU_STDVGA_ID) continue;
        uint32_t bar0 = pci_cfg32(0, dev, 0, 0x10);
        if (bar0 & 1) continue;                 /* an I/O BAR is not the LFB  */
        uint32_t base = bar0 & 0xFFFFFFF0u;
        if (base) return base;
    }
    return 0;
}

static int try_bochs_vbe(uint16_t w, uint16_t h) {
    uint32_t base = stdvga_lfb_base();
    if (!base) return -1;

    /* The ID register is the interlock: 0xB0C0..0xB0C5 means these really are
     * the DISPI registers and not some other device answering on 0x1CE. */
    uint16_t id = dispi_read(DISPI_ID);
    if ((id & 0xFFF0u) != 0xB0C0u) return -1;

    dispi_write(DISPI_ENABLE, 0);               /* modes are set while off    */
    dispi_write(DISPI_XRES, w);
    dispi_write(DISPI_YRES, h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_BANK, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

    /* Believe the hardware, not the request. If it did not take the mode, put
     * the adapter back into text mode so the VGA fallback has a screen. */
    if (dispi_read(DISPI_XRES) != w || dispi_read(DISPI_YRES) != h ||
        dispi_read(DISPI_BPP) != 32) {
        dispi_write(DISPI_ENABLE, 0);
        return -1;
    }
    uint16_t virt_w = dispi_read(DISPI_VIRT_WIDTH);
    if (virt_w < w) virt_w = w;

    hw.phys       = base;
    hw.width      = w;
    hw.height     = h;
    hw.pitch      = (uint32_t)virt_w * 4u;
    hw.bpp        = 32;
    hw.red_pos    = 16; hw.red_size   = 8;
    hw.green_pos  = 8;  hw.green_size = 8;
    hw.blue_pos   = 0;  hw.blue_size  = 8;
    hw.source     = "bochs-vbe";
    return 0;
}

/* Decode a six-byte RGB channel descriptor, and say whether it is coherent.
 * A layout is coherent when every channel has 1..8 bits, every channel fits
 * inside the pixel, and no two channels overlap. That is a strong enough test to
 * tell the two candidate offsets apart (see color_info above) without any
 * loader-specific special-casing: the wrong offset lands on padding or on the
 * tail of framebuffer_height and fails it. */
static int rgb_channels(const uint8_t *c, uint8_t bpp) {
    struct { uint8_t pos, size; } ch[3] = {
        { c[0], c[1] }, { c[2], c[3] }, { c[4], c[5] }
    };
    for (int i = 0; i < 3; i++) {
        if (ch[i].size == 0 || ch[i].size > 8) return 0;
        if ((unsigned)ch[i].pos + ch[i].size > bpp) return 0;
    }
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++) {
            unsigned a0 = ch[i].pos, a1 = a0 + ch[i].size;
            unsigned b0 = ch[j].pos, b1 = b0 + ch[j].size;
            if (a0 < b1 && b0 < a1) return 0;
        }
    return 1;
}

static int try_multiboot_tag(void) {
    const multiboot_info_t *m = mbi();
    if (!m || !(m->flags & MB_INFO_FRAMEBUFFER)) return -1;
    if (m->framebuffer_type != MB_FB_RGB) return -1;   /* EGA text / indexed  */
    if (!m->framebuffer_addr) return -1;

    /* Anything above 4 GiB is unreachable: boot/boot.asm identity-maps the low
     * 4 GiB and nothing else. Declining beats faulting on the first pixel. */
    if (m->framebuffer_addr > 0xFFFFFFFFull) return -1;

    uint32_t w = m->framebuffer_width, h = m->framebuffer_height;
    uint8_t  bpp = m->framebuffer_bpp;
    /* Too small to host a console is the same as no framebuffer, and it has to
     * be decided HERE rather than in lib/base.c. By the time we are looking at
     * this tag the loader has already switched the adapter into graphics mode, so
     * "accept the framebuffer or fall back to VGA text" is not a choice the
     * console can make afterwards — writes to 0xB8000 would go nowhere. Declining
     * here lets fb_hw_init() go on to set a mode of its own choosing instead. */
    if (w < FB_MIN_WIDTH || h < FB_MIN_HEIGHT) return -1;
    if (w > FB_MAX_WIDTH || h > FB_MAX_HEIGHT) return -1;
    if (bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return -1;
    if (m->framebuffer_pitch < w * (uint32_t)((bpp + 7) / 8)) return -1;

    /* Spec offset 110 first, GRUB's 4-byte-aligned 112 second. */
    const uint8_t *c = m->color_info;
    if (!rgb_channels(c, bpp)) {
        c += 2;
        if (!rgb_channels(c, bpp)) return -1;
    }

    hw.phys       = m->framebuffer_addr;
    hw.width      = (int32_t)w;
    hw.height     = (int32_t)h;
    hw.pitch      = m->framebuffer_pitch;
    hw.bpp        = bpp;
    hw.red_pos    = c[0]; hw.red_size   = c[1];
    hw.green_pos  = c[2]; hw.green_size = c[3];
    hw.blue_pos   = c[4]; hw.blue_size  = c[5];
    hw.source     = "multiboot";
    return 0;
}

int fb_hw_init(void) {
    if (hw_tried) return hw.active ? 0 : -1;
    hw_tried = 1;

    if (fb_forced_off()) return -1;

    if (try_multiboot_tag() != 0) {
        hw.source = "none";
        /* 1024x768x32 is the mode every emulated VGA supports, it needs 3 MiB
         * of the adapter's 16, and 8x16 glyphs make it a 128x48 console. */
        if (try_bochs_vbe(1024, 768) != 0) { hw.source = "none"; return -1; }
    }

    if (fb_surface_init(&back, back_px, hw.width, hw.height, hw.width) != 0) {
        hw.source = "none";
        return -1;
    }
    front     = (volatile uint8_t *)(uintptr_t)hw.phys;
    hw.active = 1;
    fb_present();
    return 0;
}

const fb_hwinfo_t *fb_hw_info(void) { return &hw; }

fb_surface_t *fb_back(void) { return hw.active ? &back : (fb_surface_t *)0; }

/* Pack an 0x00RRGGBB colour into the adapter's channel layout. */
static inline uint32_t pack(fb_color_t c) {
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return ((r >> (8 - hw.red_size))   << hw.red_pos)   |
           ((g >> (8 - hw.green_size)) << hw.green_pos) |
           ((b >> (8 - hw.blue_size))  << hw.blue_pos);
}

void fb_present_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!hw.active) return;

    /* Clamp to the screen, not to the back buffer's current clip rectangle: a
     * caller that narrowed the clip to draw still wants the pixels shown. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if ((int64_t)x + w > hw.width)  w = hw.width - x;
    if ((int64_t)y + h > hw.height) h = hw.height - y;
    if (w <= 0 || h <= 0) return;

    int bytes = (hw.bpp + 7) / 8;

    /* The common case by a wide margin: 32bpp XRGB, i.e. exactly the back
     * buffer's own format, so a row is a straight copy. */
    int direct = (hw.bpp == 32 && hw.red_pos == 16 && hw.green_pos == 8 &&
                  hw.blue_pos == 0 && hw.red_size == 8 && hw.green_size == 8 &&
                  hw.blue_size == 8);

    for (int32_t row = 0; row < h; row++) {
        const fb_color_t *src = back_px + (size_t)(y + row) * (size_t)back.stride
                              + (size_t)x;
        volatile uint8_t *dst = front + (size_t)(y + row) * (size_t)hw.pitch
                              + (size_t)x * (size_t)bytes;
        if (direct) {
            volatile uint32_t *d32 = (volatile uint32_t *)dst;
            for (int32_t c = 0; c < w; c++) d32[c] = src[c];
        } else {
            for (int32_t c = 0; c < w; c++) {
                uint32_t v = pack(src[c]);
                for (int i = 0; i < bytes; i++) dst[c * bytes + i] = (uint8_t)(v >> (8 * i));
            }
        }
    }
}

void fb_present(void) { fb_present_rect(0, 0, hw.width, hw.height); }

#endif /* FABLEOS_HOSTTEST */
