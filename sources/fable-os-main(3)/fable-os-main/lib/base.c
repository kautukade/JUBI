/* base.c — kernel runtime: the console, string/mem helpers, PIT-calibrated
 * millisecond clock, and panic.
 *
 * THE CONSOLE HAS TWO BACKENDS AND ONE CONTRACT
 *   kputc/kputs/kprintf behave identically either way. Underneath:
 *
 *     FRAMEBUFFER (preferred). lib/fb.c found a linear framebuffer, so a cell is
 *       8x16 pixels of Spleen drawn out of include/font.h and the console is as
 *       wide as the mode allows (128x48 at 1024x768). This is the path that can
 *       render an em-dash, a curly quote or a box-drawing character, which VGA
 *       text mode cannot.
 *
 *     VGA TEXT (fallback). No framebuffer: 0xB8000, 80x25, the adapter's own
 *       code page 437 font, the CRTC's own blinking cursor. Exactly what this
 *       console was before. A machine can never end up with no console at all,
 *       and `console=vga` on the kernel command line forces this path so a
 *       suspected console regression can be bisected without a rebuild.
 *
 * THE GRID IS THE SAME SHAPE IN BOTH, AND THAT IS DELIBERATE
 *   One uint16_t per cell, attribute<<8 | byte, because tools/screen_tools.c
 *   reads the screen back and paints into it through console_fb() and that
 *   contract is older than this file's ability to draw. In VGA text mode the grid
 *   IS the hardware's 0xB8000. In framebuffer mode it is an array here, and the
 *   renderer's job is to keep pixels equal to it.
 *
 *   A byte cannot index a ~1000-glyph font, so the low byte is a slot in a
 *   256-entry CODE PAGE (include/font.h). Slots 0x10-0xFF are code page 437
 *   unchanged — which is why the VGA fallback renders 240 of the 256 slots
 *   identically instead of approximately — and CP437's dingbat slots are re-used
 *   for the punctuation a language model actually emits. The ten re-used slots
 *   are uploaded into the VGA adapter's own font RAM in the fallback path, so
 *   even there an em-dash is an em-dash.
 *
 * WHY THE RENDERER DIFFS THE GRID
 *   tools/screen_tools.c pokes cells directly and knows nothing about pixels. In
 *   VGA text mode that just works; with a framebuffer, someone has to notice.
 *   So every character printed first reconciles the grid against a shadow copy
 *   and redraws whatever changed behind the console's back. It costs one compare
 *   per cell per character and it means a tool that paints does not have to know
 *   the console exists. console_refresh() is the same reconciliation on demand.
 */

#include "kernel.h"
#include "io.h"
#include "fb.h"
#include "font.h"
#include <stdarg.h>

/* ====================================================================== */
/* String / memory                                                        */
/* ====================================================================== */

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ====================================================================== */
/* Console                                                                */
/* ====================================================================== */

#define VGA_TEXT ((volatile uint16_t *)0xB8000)
#define VGA_W 80
#define VGA_H 25
#define ATTR 0x07   /* light grey on black */

/* Bound on the framebuffer text grid, derived from the largest mode lib/fb.c
 * will drive. 160x64 at 8x16; tools/screen_tools.c tolerates up to 200x100. */
#define GRID_MAX_COLS (FB_MAX_WIDTH  / 8)
#define GRID_MAX_ROWS (FB_MAX_HEIGHT / 16)

static volatile uint16_t *cells = VGA_TEXT;   /* the grid, either backend  */
static int con_rows = VGA_H, con_cols = VGA_W;
static int cur_row, cur_col;
static uint8_t con_attr = ATTR;

static int fb_mode;                  /* 1 = drawing pixels, 0 = VGA text    */
static int glyph_w = 8, glyph_h = 16;
static int cursor_drawn;

/* The framebuffer backend's grid, plus a shadow of what has actually been
 * rasterised. Aligned so the reconciliation scan can be vectorised. */
static uint16_t fb_cells[GRID_MAX_ROWS * GRID_MAX_COLS] __attribute__((aligned(16)));
static uint16_t fb_shadow[GRID_MAX_ROWS * GRID_MAX_COLS] __attribute__((aligned(16)));

/* kputc() takes one byte, so a multi-byte character necessarily arrives across
 * several calls. This is the state between them. */
static utf8_stream_t ustream;

/* The serial driver overrides this; until it loads (or if absent) it's a
 * no-op. Lets kprintf reach the serial log once we leave VGA text mode. */
__attribute__((weak)) void serial_emit(char c) { (void)c; }

static uint16_t blank_cell(void) {
    return (uint16_t)(((uint16_t)con_attr << 8) | (uint16_t)' ');
}

/* ---- VGA text backend ---------------------------------------------------- */

/* Move the blinking hardware cursor to (cur_row, cur_col) via the VGA CRTC. */
static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(cur_row * VGA_W + cur_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

/* Replace the adapter's ROM glyphs for the ten code page slots this kernel
 * re-uses (em dash, en dash, the four curly quotes, ellipsis, check, cross, and
 * the unmapped box) with the same bitmaps the framebuffer path draws.
 *
 * The VGA font lives in plane 2 of video memory, which is only addressable while
 * the sequencer's odd/even chaining is off and the graphics controller has
 * remapped the aperture to 0xA0000. That is the whole dance below. Only those
 * ten 32-byte slots are written, so the other 246 glyphs remain the adapter's
 * own and a mistake here cannot cost more than ten characters.
 *
 * 8x16 text mode reserves 32 bytes per glyph and uses the first 16. */
static void vga_install_page_glyphs(void) {
    static const uint8_t slots[] = { 0x01, 0x02, 0x03, 0x04, 0x05,
                                     0x06, 0x0B, 0x0C, 0x0E, 0x7F };

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);   /* sequencer: synchronous reset   */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);   /* map mask: plane 2 only         */
    outb(0x3C4, 0x04); outb(0x3C5, 0x07);   /* extended mem, no odd/even      */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);   /* reset off                      */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);   /* GC read map select: plane 2    */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);   /* GC mode: write 0, no odd/even  */
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);   /* GC misc: aperture at 0xA0000   */

    volatile uint8_t *plane2 = (volatile uint8_t *)0xA0000;
    for (unsigned i = 0; i < sizeof slots; i++) {
        const uint8_t *bits = font_glyph_bits(&font_8x16,
                                              (int)font_page_glyph(slots[i]));
        volatile uint8_t *dst = plane2 + (unsigned)slots[i] * 32u;
        for (int r = 0; r < 16; r++) dst[r] = bits ? bits[r] : 0;
        for (int r = 16; r < 32; r++) dst[r] = 0;
    }

    outb(0x3C4, 0x00); outb(0x3C5, 0x01);   /* reset while we put it back     */
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);   /* map mask: planes 0 and 1       */
    outb(0x3C4, 0x04); outb(0x3C5, 0x03);   /* extended mem + odd/even        */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);   /* reset off                      */
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);   /* read map select: plane 0       */
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);   /* odd/even addressing            */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);   /* aperture back at 0xB8000, text */
}

/* ---- framebuffer backend ------------------------------------------------- */

/* The back buffer, clipped to the character grid. The clip matters: a mode whose
 * height is not a multiple of 16 leaves a margin of pixels no cell owns, and
 * scrolling must not drag it upwards. */
static fb_surface_t *grid_surface(void) {
    fb_surface_t *s = fb_back();
    if (!s) return (fb_surface_t *)0;
    fb_clip_reset(s);
    fb_clip_set(s, 0, 0, con_cols * glyph_w, con_rows * glyph_h);
    return s;
}

/* Rasterise one cell from the grid, optionally with the cursor over it, and
 * push those pixels to the screen. This is the only writer of fb_shadow: the
 * shadow means "the pixels on screen were drawn from this cell value". */
static void render_cell(int row, int col, int with_cursor) {
    fb_surface_t *s = grid_surface();
    if (!s) return;

    size_t   idx  = (size_t)row * (size_t)con_cols + (size_t)col;
    uint16_t cell = cells[idx];
    uint8_t  slot = (uint8_t)(cell & 0xFF);
    uint8_t  attr = (uint8_t)(cell >> 8);

    fb_color_t fg = fb_vga_palette[attr & 0x0F];
    fb_color_t bg = fb_vga_palette[(attr >> 4) & 0x0F];

    int32_t x = (int32_t)col * glyph_w, y = (int32_t)row * glyph_h;
    fb_draw_glyph(s, &font_8x16, x, y, (int)font_page_glyph(slot), fg, bg, 1);
    if (with_cursor) fb_fill_rect(s, x, y + glyph_h - 2, glyph_w, 2, fg);

    fb_shadow[idx] = cell;
    fb_present_rect(x, y, glyph_w, glyph_h);
}

/* Redraw any cell somebody changed without telling us — see the file header. */
static void reconcile(void) {
    if (!fb_mode) return;
    for (int r = 0; r < con_rows; r++) {
        const uint16_t *g = (const uint16_t *)cells + (size_t)r * (size_t)con_cols;
        const uint16_t *sh = fb_shadow + (size_t)r * (size_t)con_cols;
        for (int c = 0; c < con_cols; c++)
            if (g[c] != sh[c]) render_cell(r, c, 0);
    }
}

void console_refresh(void) { reconcile(); }

static void cursor_hide(void) {
    if (fb_mode && cursor_drawn) { render_cell(cur_row, cur_col, 0); cursor_drawn = 0; }
}

static void cursor_show(void) {
    if (fb_mode) { render_cell(cur_row, cur_col, 1); cursor_drawn = 1; }
    else         vga_update_cursor();
}

/* ---- shared: scrolling and cell output ---------------------------------- */

/* Every scroll this console has ever done. The window manager subtracts two
 * readings to learn EXACTLY how far the screen moved between its ticks, which is
 * the only cheap way for it to repair the band a scroll drags a window's pixels
 * into. It used to infer this by pattern-matching the cell grid against its own
 * shadow, which cannot distinguish "did not scroll" from "scrolled further than
 * the probe looks" and could not see a scroll of blank lines at all — so it
 * treated "the cursor is on the last row", which is where the prompt lives
 * permanently, as evidence of a scroll and repainted all 786,432 pixels on every
 * single keystroke. A counter is exact, costs one add, and cannot be fooled:
 * this function is the only thing in the kernel that shifts console pixels. */
static uint64_t con_scrolls;

uint64_t console_scrolls(void) { return con_scrolls; }

static void scroll(void) {
    uint16_t blank = blank_cell();

    con_scrolls++;
    for (int i = 0; i < con_cols * (con_rows - 1); i++) cells[i] = cells[i + con_cols];
    for (int i = 0; i < con_cols; i++) cells[con_cols * (con_rows - 1) + i] = blank;
    cur_row = con_rows - 1;

    if (fb_mode) {
        /* Shift the pixels and the shadow by exactly the same row, so
         * reconcile() sees a screen that already agrees with the grid instead of
         * every cell having "changed". */
        fb_surface_t *s = grid_surface();
        if (s) fb_scroll(s, glyph_h, fb_vga_palette[(con_attr >> 4) & 0x0F]);
        for (int i = 0; i < con_cols * (con_rows - 1); i++)
            fb_shadow[i] = fb_shadow[i + con_cols];
        for (int i = 0; i < con_cols; i++)
            fb_shadow[con_cols * (con_rows - 1) + i] = blank;
        fb_present();
    }
}

static void put_slot(uint8_t slot) {
    size_t idx = (size_t)cur_row * (size_t)con_cols + (size_t)cur_col;
    cells[idx] = (uint16_t)(((uint16_t)con_attr << 8) | slot);
    if (fb_mode) render_cell(cur_row, cur_col, 0);
    cur_col++;
}

/* One code point onto the grid. The four control characters below move the
 * cursor and nothing else — byte-for-byte the behaviour this console has always
 * had, which is what net/chat.c's column guard and every test depend on. */
static void emit_codepoint(uint32_t cp) {
    if (cp == '\n') {
        cur_col = 0;
        if (++cur_row >= con_rows) scroll();
        return;
    }
    if (cp == '\r') { cur_col = 0; return; }
    if (cp == '\b') {
        if (cur_col > 0) cur_col--;
        else if (cur_row > 0) { cur_row--; cur_col = con_cols - 1; }
        return;
    }
    if (cp == '\t') cur_col = (cur_col + 8) & ~7;
    else            put_slot((uint8_t)font_page_slot(cp));

    if (cur_col >= con_cols) {
        cur_col = 0;
        if (++cur_row >= con_rows) scroll();
    }
}

static void console_selftest(void);

void console_init(void) {
    utf8_stream_reset(&ustream);
    cur_row = cur_col = 0;
    con_attr = ATTR;
    cursor_drawn = 0;
    fb_mode = 0;

    if (fb_hw_init() == 0) {
        const fb_hwinfo_t *i = fb_hw_info();
        glyph_w = font_8x16.width;
        glyph_h = font_8x16.height;
        /* Clamped, not trusted: lib/fb.c refuses a mode larger than FB_MAX_* or
         * smaller than 40x10 characters, so both bounds are already true — but
         * fb_cells is indexed with these numbers, and a bound that matters is
         * worth stating where the indexing happens. */
        int c = i->width / glyph_w, r = i->height / glyph_h;
        if (c > GRID_MAX_COLS) c = GRID_MAX_COLS;
        if (r > GRID_MAX_ROWS) r = GRID_MAX_ROWS;
        if (c < 1) c = 1;
        if (r < 1) r = 1;
        con_cols = c;
        con_rows = r;
        cells    = fb_cells;
        fb_mode  = 1;
    }
    if (!fb_mode) {
        cells    = VGA_TEXT;
        con_rows = VGA_H;
        con_cols = VGA_W;
        glyph_w  = 8;
        glyph_h  = 16;
    }

    uint16_t blank = blank_cell();
    for (int i = 0; i < con_rows * con_cols; i++) cells[i] = blank;

    if (fb_mode) {
        fb_surface_t *s = fb_back();
        fb_clip_reset(s);
        fb_clear(s, fb_vga_palette[(con_attr >> 4) & 0x0F]);   /* margins too */
        for (int i = 0; i < con_rows * con_cols; i++) fb_shadow[i] = blank;
        fb_present();
    } else {
        vga_install_page_glyphs();
    }
    cursor_show();

    /* Say which display this is, on the screen and down the serial line. A
     * screenshot then carries its own explanation, and a console regression is
     * bisectable from the log alone. */
    const fb_hwinfo_t *i = fb_hw_info();
    if (fb_mode)
        kprintf("[display %dx%d %dbpp via %s -> %dx%d text, %s]\n",
                (int)i->width, (int)i->height, (int)i->bpp, i->source,
                con_cols, con_rows, font_8x16.name);
    else
        kprintf("[display vga text 80x25, code page 437%s]\n",
                fb_forced_off() ? " (console=vga on the command line)" : "");

    console_selftest();
}

/* Print, at every boot, the characters this console claims it can draw.
 *
 * The claim is the whole point of replacing VGA text mode, and it is exactly the
 * kind of claim that rots silently: a font table regenerated with one glyph
 * missing, a code page slot re-used twice, a UTF-8 decoder that swallows the
 * byte after a bad sequence. None of that shows up in the boot log unless the
 * boot log contains the characters. So it does — three lines, in the same spirit
 * as the VFS, ACPI and e1000 self-tests, and cheap enough to leave on.
 *
 * Read it as four assertions: the punctuation a model actually emits renders;
 * arrows, mathematics, Latin-1 and box drawing render; a code point with no slot
 * of its own but an honest equivalent is FOLDED (the rounded corners come out
 * square); and a code point with neither renders as a visible unmapped box
 * rather than as a stray glyph or as nothing. The last one is a 4-byte sequence,
 * so it also proves the decoder handles the astral planes. */
static void console_selftest(void) {
    kputs("[console utf-8: em—dash en–dash ‘curly’ "
          "“quotes” ellipsis… bullet• ✓ ✗]\n");
    kputs("[console glyphs: ←↑→↓↔↕ "
          "≈≡≤≥±÷∞√ "
          "áéíóú äöü ñ ç "
          "ß ° µ ¶ §]\n");
    kputs("[console glyphs: box ┌──┬──┐ "
          "├──┼──┤ └──┴──┘ "
          "╔══╦══╗ ╠══╬══╣ ╚══╩══╝]\n");
    kputs("[console glyphs: blocks ░▒▓█▄▀▌▐ "
          "folded ╭╮╰╯ unmapped \U0001f600]\n");
}

void kputc(char c) {
    serial_emit(c);

    /* Reconcile before anything, so a cell painted through console_fb() since
     * the last character is on screen before the cursor moves off it. */
    reconcile();
    cursor_hide();

    uint8_t b = (uint8_t)c;
    for (;;) {
        uint32_t cp;
        int      rc = utf8_feed(&ustream, b, &cp);
        if (rc == UTF8_MORE) break;              /* incomplete: nothing to draw */
        emit_codepoint(cp);
        if (rc != UTF8_BAD_RETRY) break;
        /* The byte ended someone else's sequence and starts its own. One retry
         * at most: the stream is reset, so the next pass takes the lead-byte
         * path, which never asks for a retry. */
    }

    cursor_show();
}

void kputs(const char *s) { while (*s) kputc(*s++); }

/* ---- direct access to the console's cells, for tools that paint ----
 * tools/screen_tools.c lets the model read the screen back and paint into it.
 * Rather than have it duplicate 0xB8000 and 80x25 (which would silently rot the
 * day this console changes), it asks here. The cursor accessor matters most: a
 * painted region is transient precisely because THIS cursor decides where the
 * next character lands and when scroll() fires, so a tool can only be honest
 * about that if it can see the real value.
 *
 * The shape is unchanged from the VGA-only console — one uint16_t per cell,
 * attribute<<8 | character — but two things about it are now worth knowing:
 * the geometry is whatever the display gives (128x48 at 1024x768, still 80x25
 * on the fallback), so it must be asked for and not assumed; and the low byte is
 * a slot in the console code page, which is code page 437 from 0x10 up and the
 * model's punctuation below that. Printable ASCII is identical in both, which is
 * all screen_tools writes. Writing here does not move the cursor, and the pixels
 * catch up on the next printed character (or on console_refresh()). */
volatile uint16_t *console_fb(void) { return cells; }

void console_geometry(int *rows, int *cols) {
    if (rows) *rows = con_rows;
    if (cols) *cols = con_cols;
}

/* Where the next PRINTABLE character will land — which is not always where the
 * cursor is sitting, and the difference is load-bearing.
 *
 * net/chat.c asks this before it prints a '[' so that model prose can never put
 * a bracket in column zero and forge a kernel trace line. UTF-8 decoding
 * introduced a new way for that guard to be wrong: if the previous bytes left a
 * partial multi-byte sequence pending, the cursor has NOT moved yet. Feeding an
 * ASCII byte next abandons that sequence, draws one replacement glyph at the
 * current cell, and only then places the ASCII byte — one cell further on, and
 * possibly wrapped to column zero of the next row. A caller told "column 79"
 * would print its bracket into column 0 anyway.
 *
 * So a pending sequence is accounted for here, without consuming it: the answer
 * is where the byte AFTER the replacement glyph goes. The one caller this is
 * wrong for is a caller that goes on to print a continuation byte, i.e. one
 * asking where the cursor is in the middle of a character it is still building —
 * a question with no useful answer. Nothing does that. */
void console_cursor(int *row, int *col) {
    int r = cur_row, c = cur_col;

    if (ustream.pending) {
        if (++c >= con_cols) {
            c = 0;
            if (++r >= con_rows) r = con_rows - 1;   /* the scroll clamps here */
        }
    }
    if (row) *row = r;
    if (col) *col = c;
}

static void put_uint(uint64_t v, int base, int width, char pad) {
    char buf[32];
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) {
        int d = v % base;
        buf[i++] = d < 10 ? '0' + d : 'a' + d - 10;
        v /= base;
    }
    while (i < width) buf[i++] = pad;
    while (i--) kputc(buf[i]);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { kputc(*fmt); continue; }
        fmt++;
        int width = 0; char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt) {
            case 's': kputs(va_arg(ap, const char *)); break;
            case 'c': kputc((char)va_arg(ap, int)); break;
            case 'd': {
                int v = va_arg(ap, int);
                if (v < 0) { kputc('-'); put_uint((uint64_t)(-(int64_t)v), 10, 0, ' '); }
                else put_uint((uint64_t)v, 10, width, pad);
                break;
            }
            case 'u': put_uint(va_arg(ap, unsigned), 10, width, pad); break;
            case 'x': put_uint(va_arg(ap, unsigned), 16, width, pad); break;
            case 'p': kputs("0x"); put_uint((uint64_t)va_arg(ap, void *), 16, 0, ' '); break;
            case '%': kputc('%'); break;
            default: kputc('%'); kputc(*fmt); break;
        }
    }
    va_end(ap);
}

/* The kernel heap (kmalloc/kfree/krealloc) now lives in mm/heap.c. */

/* ====================================================================== */
/* Time — calibrate the TSC against PIT channel 2                          */
/* ====================================================================== */

static uint64_t tsc_per_ms;
static uint64_t tsc_base;

void time_init(void) {
    /* Use PIT channel 2 in mode 0 to time a known interval against the TSC.
     * The PIT input clock is 1193182 Hz. Count 11932 ticks ≈ 10 ms. */
    uint8_t p = (inb(0x61) & 0xFC) | 0x01;   /* speaker off, gate on */
    outb(0x61, p);
    outb(0x43, 0xB0);                        /* ch2, lo/hi byte, mode 0, binary */
    uint16_t count = 11932;
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);

    /* Restart the count by toggling the gate. */
    p = inb(0x61) & 0xFE;
    outb(0x61, p);
    outb(0x61, p | 0x01);

    uint64_t start = rdtsc();
    while (!(inb(0x61) & 0x20)) { }          /* wait for OUT to go high */
    uint64_t end = rdtsc();

    tsc_per_ms = (end - start) / 10;
    if (tsc_per_ms == 0) tsc_per_ms = 1;     /* avoid divide-by-zero */
    tsc_base = rdtsc();
}

uint64_t millis(void) {
    return (rdtsc() - tsc_base) / tsc_per_ms;
}

void mdelay(uint32_t ms) {
    uint64_t target = millis() + ms;
    while (millis() < target) { }
}

/* ====================================================================== */
/* Panic                                                                   */
/* ====================================================================== */

void panic(const char *msg) {
    kputs("\n*** PANIC: ");
    kputs(msg);
    kputs(" ***\n");
    for (;;) __asm__ volatile("cli; hlt");
}
