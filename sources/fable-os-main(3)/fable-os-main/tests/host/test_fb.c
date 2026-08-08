/* test_fb.c — the display, asserted pixel by pixel on the host.
 *
 * WHY THIS CAN BE A HOST TEST AT ALL
 *   lib/fb.c's drawing half never touches hardware: it writes 32-bit pixels into
 *   a surface the caller supplies. So the surface can be a plain array with
 *   0xDEADBEEF canaries either side of it, and every claim the console rests on
 *   becomes a checkable arithmetic fact:
 *
 *     - a glyph is rasterised from exactly these bytes into exactly these pixels;
 *     - UTF-8 decoding accepts what is valid and replaces what is not, without
 *       ever losing the byte after a bad sequence;
 *     - a draw that hangs off any of the four edges paints the part that fits and
 *       NOTHING outside the clip rectangle — verified by the canaries and by
 *       reading every pixel of the surface, not by trusting the return value;
 *     - the console's scroll moves the right pixels and blanks the right band.
 *
 *   The hardware half (multiboot tag, Bochs VBE, present) is compiled out under
 *   FABLEOS_HOSTTEST and is covered by booting: tests/qemu and ./capture.sh.
 *
 * THE FONT IS PINNED BY VALUE, NOT BY REFERENCE
 *   Several checks hardcode glyph bitmaps (em dash, box vertical, 'A'). Asking
 *   the font for a glyph and then comparing it against the font would assert
 *   nothing. Hardcoding the bytes means a regenerated lib/font_spleen8x16.c that
 *   moved a glyph, dropped one, or shifted a baseline fails here.
 */

#include "harness.h"
#include "fb.h"
#include "font.h"

#include <string.h>

/* ====================================================================== */
/* a canary-guarded surface                                               */
/* ====================================================================== */

#define SW 24            /* surface width                                */
#define SH 18            /* surface height                              */
#define STRIDE 29        /* deliberately > SW: catches stride mistakes    */
#define PAD 16           /* canary pixels before and after the pixels     */
#define CANARY 0xDEADBEEFu

static fb_color_t   buf[PAD + STRIDE * SH + PAD];
static fb_surface_t surf;

static void surface_reset(void) {
    for (size_t i = 0; i < sizeof buf / sizeof buf[0]; i++) buf[i] = CANARY;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < STRIDE; x++) buf[PAD + y * STRIDE + x] = 0;
    CHECK_EQ(fb_surface_init(&surf, buf + PAD, SW, SH, STRIDE), 0);
}

/* Canaries, plus the columns between SW and STRIDE that no drawing call may
 * ever touch: a stride bug shows up there before it shows up anywhere else. */
static int untouched_outside(void) {
    for (int i = 0; i < PAD; i++)
        if (buf[i] != CANARY || buf[PAD + STRIDE * SH + i] != CANARY) return 0;
    for (int y = 0; y < SH; y++)
        for (int x = SW; x < STRIDE; x++)
            if (buf[PAD + y * STRIDE + x] != 0) return 0;
    return 1;
}

static fb_color_t at(int x, int y) { return buf[PAD + y * STRIDE + x]; }

/* Count pixels of a given colour anywhere in the visible surface. */
static int count_of(fb_color_t c) {
    int n = 0;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) if (at(x, y) == c) n++;
    return n;
}

#define RED   FB_RGB(0xFF, 0x00, 0x00)
#define GREEN FB_RGB(0x00, 0xFF, 0x00)
#define BLUE  FB_RGB(0x00, 0x00, 0xFF)
#define WHITE FB_RGB(0xFF, 0xFF, 0xFF)

/* ====================================================================== */
/* surfaces                                                               */
/* ====================================================================== */

static void test_surface_init_rejects_nonsense(void) {
    fb_surface_t s;

    CHECK_EQ(fb_surface_init(&s, (fb_color_t *)0, 8, 8, 8), -1);
    CHECK(s.px == (fb_color_t *)0);

    fb_color_t px[64];
    CHECK_EQ(fb_surface_init(&s, px, 0, 8, 8), -1);
    CHECK_EQ(fb_surface_init(&s, px, 8, 0, 8), -1);
    CHECK_EQ(fb_surface_init(&s, px, -8, 8, 8), -1);
    CHECK_EQ(fb_surface_init(&s, px, 8, -8, 8), -1);
    /* A stride narrower than the width would let row y+1 start inside row y. */
    CHECK_EQ(fb_surface_init(&s, px, 8, 8, 7), -1);
    CHECK_EQ(fb_surface_init((fb_surface_t *)0, px, 8, 8, 8), -1);

    /* A rejected surface must be inert, not half-built. */
    CHECK_EQ(fb_surface_init(&s, px, 8, 8, 7), -1);
    fb_fill_rect(&s, 0, 0, 8, 8, RED);
    fb_put_pixel(&s, 0, 0, RED);
    fb_clear(&s, RED);
    CHECK_EQ(fb_draw_utf8(&s, &font_8x16, 0, 0, "x", 1, RED, 0, 1), 8);

    CHECK_EQ(fb_surface_init(&s, px, 8, 8, 8), 0);
    CHECK_EQ(s.w, 8);
    CHECK_EQ(s.cw, 8);
    CHECK_EQ(s.ch, 8);
}

static void test_clip_intersects_and_never_widens(void) {
    surface_reset();

    int32_t x, y, w, h;
    fb_clip_get(&surf, &x, &y, &w, &h);
    CHECK_EQ(x, 0); CHECK_EQ(y, 0); CHECK_EQ(w, SW); CHECK_EQ(h, SH);

    CHECK_EQ(fb_clip_set(&surf, 4, 5, 6, 7), 0);
    fb_clip_get(&surf, &x, &y, &w, &h);
    CHECK_EQ(x, 4); CHECK_EQ(y, 5); CHECK_EQ(w, 6); CHECK_EQ(h, 7);

    /* Narrowing works; widening does not, because it intersects. */
    CHECK_EQ(fb_clip_set(&surf, 0, 0, SW, SH), 0);
    fb_clip_get(&surf, &x, &y, &w, &h);
    CHECK_EQ(x, 4); CHECK_EQ(y, 5); CHECK_EQ(w, 6); CHECK_EQ(h, 7);

    /* A clip that cannot intersect goes empty, and drawing becomes a no-op. */
    CHECK_EQ(fb_clip_set(&surf, 100, 100, 5, 5), -1);
    fb_clip_get(&surf, &x, &y, &w, &h);
    CHECK_EQ(w, 0); CHECK_EQ(h, 0);
    fb_fill_rect(&surf, 0, 0, SW, SH, RED);
    CHECK_EQ(count_of(RED), 0);

    fb_clip_reset(&surf);
    fb_clip_get(&surf, &x, &y, &w, &h);
    CHECK_EQ(w, SW); CHECK_EQ(h, SH);

    /* Absurd rectangles: a clip whose width overflows int32 must not wrap into
     * a valid-looking small rectangle somewhere else. */
    fb_clip_reset(&surf);
    CHECK_EQ(fb_clip_set(&surf, 2, 2, 0x7FFFFFFF, 0x7FFFFFFF), 0);
    fb_clip_get(&surf, &x, &y, &w, &h);
    CHECK_EQ(x, 2); CHECK_EQ(y, 2); CHECK_EQ(w, SW - 2); CHECK_EQ(h, SH - 2);

    fb_clip_reset(&surf);
    CHECK_EQ(fb_clip_set(&surf, -0x7FFFFFFF, -0x7FFFFFFF, 4, 4), -1);
    CHECK(untouched_outside());
}

/* ====================================================================== */
/* pixels and rectangles                                                  */
/* ====================================================================== */

static void test_pixels(void) {
    surface_reset();

    fb_put_pixel(&surf, 0, 0, RED);
    fb_put_pixel(&surf, SW - 1, SH - 1, GREEN);
    CHECK_EQ(at(0, 0), RED);
    CHECK_EQ(at(SW - 1, SH - 1), GREEN);
    CHECK_EQ(fb_get_pixel(&surf, 0, 0), RED);
    CHECK_EQ(fb_get_pixel(&surf, SW - 1, SH - 1), GREEN);

    /* Every kind of out-of-range coordinate, including the ones that would
     * index a valid pixel on the previous or next row if unchecked. */
    fb_put_pixel(&surf, -1, 0, BLUE);
    fb_put_pixel(&surf, 0, -1, BLUE);
    fb_put_pixel(&surf, SW, 0, BLUE);
    fb_put_pixel(&surf, 0, SH, BLUE);
    fb_put_pixel(&surf, SW, SH, BLUE);
    fb_put_pixel(&surf, -1000000, -1000000, BLUE);
    fb_put_pixel(&surf, 0x7FFFFFFF, 0x7FFFFFFF, BLUE);
    fb_put_pixel(&surf, -0x7FFFFFFF - 1, 3, BLUE);
    CHECK_EQ(count_of(BLUE), 0);
    CHECK(untouched_outside());

    /* Reads outside are 0 and do not fault. */
    CHECK_EQ(fb_get_pixel(&surf, -1, -1), 0u);
    CHECK_EQ(fb_get_pixel(&surf, SW, SH), 0u);
    CHECK_EQ(fb_get_pixel((fb_surface_t *)0, 0, 0), 0u);

    /* A clip bounds writes but NOT reads: fb_get_pixel is the test's own
     * instrument and must not lie about a pixel that exists. */
    fb_clip_reset(&surf);
    fb_clip_set(&surf, 10, 10, 2, 2);
    fb_put_pixel(&surf, 5, 5, BLUE);
    CHECK_EQ(at(5, 5), 0u);
    fb_put_pixel(&surf, 10, 10, BLUE);
    CHECK_EQ(at(10, 10), BLUE);
    CHECK_EQ(fb_get_pixel(&surf, 0, 0), RED);
    fb_clip_reset(&surf);
}

static void test_fill_rect_clips_at_every_edge(void) {
    /* Left, right, top, bottom, and all four corners, each hanging off by 3. */
    struct { int32_t x, y, w, h; } cases[] = {
        { -3, 5, 6, 4 },  { SW - 3, 5, 6, 4 },
        { 5, -3, 4, 6 },  { 5, SH - 3, 4, 6 },
        { -3, -3, 6, 6 }, { SW - 3, -3, 6, 6 },
        { -3, SH - 3, 6, 6 }, { SW - 3, SH - 3, 6, 6 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        surface_reset();
        fb_fill_rect(&surf, cases[i].x, cases[i].y, cases[i].w, cases[i].h, RED);

        /* Recompute the expected intersection independently, then compare every
         * pixel of the surface against it. */
        int n = 0;
        for (int y = 0; y < SH; y++) {
            for (int x = 0; x < SW; x++) {
                int inside = x >= cases[i].x && x < cases[i].x + cases[i].w &&
                             y >= cases[i].y && y < cases[i].y + cases[i].h;
                CHECK_EQ(at(x, y), inside ? RED : 0u);
                n += inside;
            }
        }
        CHECK(n > 0);
        CHECK(untouched_outside());
    }

    /* Entirely outside, zero-sized, and negative-sized all paint nothing. */
    surface_reset();
    fb_fill_rect(&surf, 100, 100, 5, 5, RED);
    fb_fill_rect(&surf, -100, -100, 5, 5, RED);
    fb_fill_rect(&surf, 2, 2, 0, 5, RED);
    fb_fill_rect(&surf, 2, 2, 5, 0, RED);
    fb_fill_rect(&surf, 2, 2, -5, -5, RED);
    fb_fill_rect(&surf, 0, 0, 0x7FFFFFFF, 0x7FFFFFFF, RED);
    /* ...except the last one, which is the whole surface. */
    CHECK_EQ(count_of(RED), SW * SH);
    CHECK(untouched_outside());
}

static void test_frame_and_lines(void) {
    surface_reset();
    fb_frame_rect(&surf, 2, 2, 10, 8, 1, RED);
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            int on_frame = (x >= 2 && x < 12 && y >= 2 && y < 10) &&
                           (x == 2 || x == 11 || y == 2 || y == 9);
            CHECK_EQ(at(x, y), on_frame ? RED : 0u);
        }

    /* A border at least half the box is a filled box, not four bars with a
     * negative-height gap between them. */
    surface_reset();
    fb_frame_rect(&surf, 0, 0, 6, 6, 3, RED);
    CHECK_EQ(count_of(RED), 36);
    surface_reset();
    fb_frame_rect(&surf, 0, 0, 6, 6, 99, RED);
    CHECK_EQ(count_of(RED), 36);
    surface_reset();
    fb_frame_rect(&surf, 0, 0, 6, 6, 0, RED);
    CHECK_EQ(count_of(RED), 0);

    surface_reset();
    fb_hline(&surf, -4, 3, 100, GREEN);
    fb_vline(&surf, 7, -4, 100, BLUE);
    for (int x = 0; x < SW; x++) CHECK_EQ(at(x, 3), x == 7 ? BLUE : GREEN);
    for (int y = 0; y < SH; y++) if (y != 3) CHECK_EQ(at(7, y), BLUE);
    CHECK(untouched_outside());
}

/* ====================================================================== */
/* blits                                                                  */
/* ====================================================================== */

static void test_blit_clips_both_sides(void) {
    /* A 4x4 source with a distinct colour per pixel, so a misplaced copy is
     * visible rather than merely plausible. */
    fb_color_t   spx[16];
    fb_surface_t src;
    for (int i = 0; i < 16; i++) spx[i] = 0x010000u * (fb_color_t)(i + 1);
    CHECK_EQ(fb_surface_init(&src, spx, 4, 4, 4), 0);

    surface_reset();
    fb_blit(&surf, 5, 6, &src, 0, 0, 4, 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            CHECK_EQ(at(5 + x, 6 + y), spx[y * 4 + x]);
    CHECK(untouched_outside());

    /* Destination hanging off the left: the source origin must advance by the
     * same amount, or the copy is the right size in the wrong place. */
    surface_reset();
    fb_blit(&surf, -2, 0, &src, 0, 0, 4, 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 2; x++)
            CHECK_EQ(at(x, y), spx[y * 4 + x + 2]);
    CHECK_EQ(count_of(0u), SW * SH - 8);

    /* Destination hanging off the bottom-right. */
    surface_reset();
    fb_blit(&surf, SW - 2, SH - 3, &src, 0, 0, 4, 4);
    CHECK_EQ(at(SW - 2, SH - 3), spx[0]);
    CHECK_EQ(at(SW - 1, SH - 1), spx[2 * 4 + 1]);
    CHECK(untouched_outside());

    /* A source rectangle bigger than the source: copy only what exists. */
    surface_reset();
    fb_blit(&surf, 0, 0, &src, 2, 2, 10, 10);
    CHECK_EQ(at(0, 0), spx[2 * 4 + 2]);
    CHECK_EQ(at(1, 1), spx[3 * 4 + 3]);
    CHECK_EQ(count_of(0u), SW * SH - 4);

    /* Negative source coordinates shift the destination instead of reading
     * before the buffer. */
    surface_reset();
    fb_blit(&surf, 0, 0, &src, -2, -2, 4, 4);
    CHECK_EQ(at(2, 2), spx[0]);
    CHECK_EQ(count_of(0u), SW * SH - 4);

    /* Nothing at all. */
    surface_reset();
    fb_blit(&surf, 100, 100, &src, 0, 0, 4, 4);
    fb_blit(&surf, 0, 0, &src, 99, 99, 4, 4);
    fb_blit(&surf, 0, 0, &src, 0, 0, 0, 4);
    fb_blit(&surf, 0, 0, &src, 0, 0, -4, -4);
    fb_blit(&surf, 0, 0, (fb_surface_t *)0, 0, 0, 4, 4);
    CHECK_EQ(count_of(0u), SW * SH);
    CHECK(untouched_outside());

    /* The clip rectangle bounds a blit exactly as it bounds a fill. */
    surface_reset();
    fb_clip_set(&surf, 6, 7, 2, 2);
    fb_blit(&surf, 5, 6, &src, 0, 0, 4, 4);
    fb_clip_reset(&surf);
    CHECK_EQ(count_of(0u), SW * SH - 4);
    CHECK_EQ(at(6, 7), spx[1 * 4 + 1]);
    CHECK(untouched_outside());
}

static void test_blit_overlapping_same_surface(void) {
    surface_reset();
    for (int x = 0; x < SW; x++) fb_put_pixel(&surf, x, 0, 0x100u + (fb_color_t)x);

    /* Shift the row two pixels right, in place. Forward row order would smear
     * the first pixel across the whole row. */
    fb_blit(&surf, 2, 0, &surf, 0, 0, SW - 2, 1);
    for (int x = 2; x < SW; x++) CHECK_EQ(at(x, 0), 0x100u + (fb_color_t)(x - 2));

    /* And two pixels left. */
    surface_reset();
    for (int x = 0; x < SW; x++) fb_put_pixel(&surf, x, 0, 0x100u + (fb_color_t)x);
    fb_blit(&surf, 0, 0, &surf, 2, 0, SW - 2, 1);
    for (int x = 0; x < SW - 2; x++) CHECK_EQ(at(x, 0), 0x100u + (fb_color_t)(x + 2));

    /* Rows overlapping downwards: row order must be reversed. */
    surface_reset();
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) fb_put_pixel(&surf, x, y, 0x200u + (fb_color_t)y);
    fb_blit(&surf, 0, 1, &surf, 0, 0, SW, SH - 1);
    for (int y = 1; y < SH; y++) CHECK_EQ(at(0, y), 0x200u + (fb_color_t)(y - 1));
    CHECK(untouched_outside());
}

static void test_blit_keyed(void) {
    fb_color_t   spx[4] = { RED, GREEN, RED, BLUE };
    fb_surface_t src;
    CHECK_EQ(fb_surface_init(&src, spx, 2, 2, 2), 0);

    surface_reset();
    fb_fill_rect(&surf, 0, 0, 2, 2, WHITE);
    fb_blit_keyed(&surf, 0, 0, &src, 0, 0, 2, 2, RED);
    CHECK_EQ(at(0, 0), WHITE);      /* keyed out */
    CHECK_EQ(at(1, 0), GREEN);
    CHECK_EQ(at(0, 1), WHITE);      /* keyed out */
    CHECK_EQ(at(1, 1), BLUE);
    CHECK(untouched_outside());
}

static void test_scroll(void) {
    surface_reset();
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) fb_put_pixel(&surf, x, y, 0x300u + (fb_color_t)y);

    fb_scroll(&surf, 3, RED);
    for (int y = 0; y < SH - 3; y++) CHECK_EQ(at(0, y), 0x300u + (fb_color_t)(y + 3));
    for (int y = SH - 3; y < SH; y++)
        for (int x = 0; x < SW; x++) CHECK_EQ(at(x, y), RED);

    /* Downwards. */
    surface_reset();
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) fb_put_pixel(&surf, x, y, 0x300u + (fb_color_t)y);
    fb_scroll(&surf, -2, GREEN);
    for (int y = 2; y < SH; y++) CHECK_EQ(at(0, y), 0x300u + (fb_color_t)(y - 2));
    for (int y = 0; y < 2; y++) CHECK_EQ(at(0, y), GREEN);

    /* Scrolling by the full height (or more) is a clear, not a copy from
     * outside the surface. */
    surface_reset();
    fb_fill_rect(&surf, 0, 0, SW, SH, WHITE);
    fb_scroll(&surf, SH, BLUE);
    CHECK_EQ(count_of(BLUE), SW * SH);
    surface_reset();
    fb_fill_rect(&surf, 0, 0, SW, SH, WHITE);
    fb_scroll(&surf, 100000, BLUE);
    CHECK_EQ(count_of(BLUE), SW * SH);
    surface_reset();
    fb_fill_rect(&surf, 0, 0, SW, SH, WHITE);
    fb_scroll(&surf, 0, BLUE);
    CHECK_EQ(count_of(WHITE), SW * SH);
    CHECK(untouched_outside());

    /* Scrolling respects the clip: this is exactly what the console needs when
     * the mode's height is not a whole number of character rows. */
    surface_reset();
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) fb_put_pixel(&surf, x, y, 0x400u + (fb_color_t)y);
    fb_clip_set(&surf, 0, 0, SW, SH - 2);      /* two rows of margin */
    fb_scroll(&surf, 1, RED);
    fb_clip_reset(&surf);
    CHECK_EQ(at(0, SH - 1), 0x400u + (fb_color_t)(SH - 1));   /* margin intact */
    CHECK_EQ(at(0, SH - 2), 0x400u + (fb_color_t)(SH - 2));
    CHECK_EQ(at(0, SH - 3), RED);                             /* vacated band  */
    CHECK_EQ(at(0, 0), 0x401u);
    CHECK(untouched_outside());
}

/* ====================================================================== */
/* the font                                                              */
/* ====================================================================== */

/* Bitmaps pinned by value. See the file header for why. */
static const uint8_t kEmDash[16] = {
    0, 0, 0, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t kBoxVertical[16] = {
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18
};
static const uint8_t kCapitalA[16] = {
    0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xFE, 0xC6,
    0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
};

static void check_glyph_bits(uint32_t cp, const uint8_t *want, const char *what) {
    int g = font_glyph_index(&font_8x16, cp);
    CHECK(g >= 0);
    const uint8_t *got = font_glyph_bits(&font_8x16, g);
    CHECK(got != (const uint8_t *)0);
    if (g < 0 || !got) { printf("    (no glyph for %s)\n", what); return; }
    for (int r = 0; r < 16; r++) CHECK_EQ(got[r], want[r]);
}

static void test_font_geometry_and_lookup(void) {
    CHECK_EQ(font_8x16.width, 8);
    CHECK_EQ(font_8x16.height, 16);
    CHECK_EQ(font_8x16.stride, 1);
    CHECK(font_8x16.nglyphs > 900);
    CHECK(font_8x16.name != (const char *)0);

    check_glyph_bits(0x2014, kEmDash, "U+2014 EM DASH");
    check_glyph_bits(0x2502, kBoxVertical, "U+2502 BOX DRAWINGS LIGHT VERTICAL");
    check_glyph_bits('A', kCapitalA, "U+0041");

    /* Glyph 0 is the replacement character, by construction, so a lookup that
     * escapes its bounds and is clamped still draws "unmapped". */
    CHECK_EQ(font_glyph_index(&font_8x16, UNICODE_REPLACEMENT),
             FONT_GLYPH_REPLACEMENT);

    /* Space must be blank; if it were not, every screen would be a grid. */
    const uint8_t *sp = font_glyph_bits(&font_8x16,
                                        font_glyph_index(&font_8x16, ' '));
    CHECK(sp != (const uint8_t *)0);
    for (int r = 0; r < 16; r++) CHECK_EQ(sp[r], 0);

    /* The characters the display exists for. */
    static const uint32_t must_have[] = {
        0x2013, 0x2014,                       /* en dash, em dash            */
        0x2018, 0x2019, 0x201C, 0x201D,       /* curly quotes                */
        0x2026, 0x2022,                       /* ellipsis, bullet            */
        0x2190, 0x2191, 0x2192, 0x2193, 0x2194, 0x2195,   /* arrows          */
        0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,   /* light box       */
        0x251C, 0x2524, 0x252C, 0x2534, 0x253C,
        0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D,   /* double box      */
        0x256D, 0x256E, 0x256F, 0x2570,                   /* rounded corners */
        0x2588, 0x2591, 0x2592, 0x2593,                   /* blocks          */
        0x00E9, 0x00FC, 0x00F1, 0x00DF, 0x00B0, 0x00B5,   /* latin-1         */
        0x03C0, 0x03A9, 0x0416, 0x0100,       /* greek, cyrillic, latin ext  */
        0x2248, 0x2264, 0x2265, 0x221E,       /* maths                       */
        0x2713, 0x2717,                       /* check, cross                */
    };
    for (unsigned i = 0; i < sizeof must_have / sizeof must_have[0]; i++) {
        int g = font_glyph_index(&font_8x16, must_have[i]);
        CHECK(g > 0);           /* > 0: present AND not the replacement glyph */
        if (g <= 0) printf("    (missing U+%04X)\n", must_have[i]);
    }

    /* The four curly quotes must be distinct from each other and from ASCII.
     * Spleen ships them as pixel-identical copies of ' and "; the generator
     * overrides them, and this is the check that keeps that override. */
    static const uint32_t quotes[] = { '\'', '"', 0x2018, 0x2019, 0x201C, 0x201D };
    for (unsigned i = 0; i < 6; i++)
        for (unsigned j = i + 1; j < 6; j++) {
            const uint8_t *a = font_glyph_bits(&font_8x16,
                                   font_glyph_index(&font_8x16, quotes[i]));
            const uint8_t *b = font_glyph_bits(&font_8x16,
                                   font_glyph_index(&font_8x16, quotes[j]));
            CHECK(a && b && memcmp(a, b, 16) != 0);
        }

    /* Absent code points report absent — never a nearby glyph. */
    CHECK_EQ(font_glyph_index(&font_8x16, 0x1F600), -1);   /* emoji           */
    CHECK_EQ(font_glyph_index(&font_8x16, 0x4E00), -1);    /* CJK             */
    CHECK_EQ(font_glyph_index(&font_8x16, 0x10FFFF), -1);
    CHECK_EQ(font_glyph_index(&font_8x16, 0x0000), -1);
    CHECK_EQ(font_glyph_index(&font_8x16, 0x001F), -1);
    CHECK_EQ(font_glyph_index((const font_t *)0, 'A'), -1);
    CHECK(font_glyph_bits(&font_8x16, -1) == (const uint8_t *)0);
    CHECK(font_glyph_bits(&font_8x16, font_8x16.nglyphs) == (const uint8_t *)0);
    CHECK(font_glyph_bits((const font_t *)0, 0) == (const uint8_t *)0);

    /* Every glyph index the ranges advertise must exist. Cheap, and it is the
     * one invariant a regenerated table could break silently. */
    for (uint16_t i = 0; i < font_8x16.nranges; i++) {
        const font_range_t *r = &font_8x16.ranges[i];
        CHECK(r->first <= r->last);
        CHECK(r->glyph + (r->last - r->first) < font_8x16.nglyphs);
        if (i) CHECK(font_8x16.ranges[i - 1].last < r->first);   /* sorted    */
        /* Both ends resolve, and to the right indices. */
        CHECK_EQ(font_glyph_index(&font_8x16, r->first), (int)r->glyph);
        CHECK_EQ(font_glyph_index(&font_8x16, r->last),
                 (int)(r->glyph + (r->last - r->first)));
    }
}

/* ====================================================================== */
/* the console code page                                                  */
/* ====================================================================== */

static void test_code_page(void) {
    /* ASCII is the identity, which is the whole reason tools/screen_tools.c
     * keeps working unchanged. */
    for (uint32_t cp = 0x20; cp < 0x7F; cp++) CHECK_EQ(font_page_slot(cp), (int)cp);

    /* The re-used slots: this is the mapping lib/base.c and the VGA font upload
     * both depend on, so it is pinned by value. */
    CHECK_EQ(font_page_slot(0x2014), 0x01);   /* em dash        */
    CHECK_EQ(font_page_slot(0x2013), 0x02);   /* en dash        */
    CHECK_EQ(font_page_slot(0x2018), 0x03);
    CHECK_EQ(font_page_slot(0x2019), 0x04);
    CHECK_EQ(font_page_slot(0x201C), 0x05);
    CHECK_EQ(font_page_slot(0x201D), 0x06);
    CHECK_EQ(font_page_slot(0x2022), 0x07);   /* bullet (CP437) */
    CHECK_EQ(font_page_slot(0x2026), 0x0B);   /* ellipsis       */
    CHECK_EQ(font_page_slot(0x2713), 0x0C);
    CHECK_EQ(font_page_slot(0x2717), 0x0E);

    /* Code page 437 survives from 0x10 up, which is what makes the VGA text
     * fallback faithful rather than approximate. */
    CHECK_EQ(font_page_slot(0x2191), 0x18);   /* up arrow       */
    CHECK_EQ(font_page_slot(0x2193), 0x19);
    CHECK_EQ(font_page_slot(0x2192), 0x1A);
    CHECK_EQ(font_page_slot(0x2190), 0x1B);
    CHECK_EQ(font_page_slot(0x2500), 0xC4);   /* box light horizontal */
    CHECK_EQ(font_page_slot(0x2502), 0xB3);
    CHECK_EQ(font_page_slot(0x2588), 0xDB);   /* full block     */
    CHECK_EQ(font_page_slot(0x00E9), 0x82);   /* e-acute        */
    CHECK_EQ(font_page_slot(0x00DF), 0xE1);   /* sharp s        */

    /* Folds: an honest equivalent, not a random glyph. */
    CHECK_EQ(font_page_slot(0x2212), '-');    /* MINUS SIGN     */
    CHECK_EQ(font_page_slot(0x2010), '-');    /* HYPHEN         */
    CHECK_EQ(font_page_slot(0x00A0), 0xFF);   /* NBSP: CP437's own blank slot */
    CHECK_EQ(font_page_slot(0x2003), ' ');    /* EM SPACE       */
    CHECK_EQ(font_page_slot(0x00D7), 'x');    /* MULTIPLICATION SIGN */
    CHECK_EQ(font_page_slot(0x256D), 0xDA);   /* rounded corner -> square */
    CHECK_EQ(font_page_slot(0x2570), 0xC0);
    CHECK_EQ(font_page_slot(0x250F), 0xDA);   /* heavy corner -> light    */
    CHECK_EQ(font_page_slot(0x21D2), 0x1A);   /* double arrow -> plain    */
    CHECK_EQ(font_page_slot(0x25CF), 0x07);   /* black circle -> bullet   */

    /* Everything else is the replacement slot: visible, and never a stray. */
    CHECK_EQ(font_page_slot(0x1F600), FONT_PAGE_REPLACEMENT);
    CHECK_EQ(font_page_slot(0x4E00), FONT_PAGE_REPLACEMENT);
    CHECK_EQ(font_page_slot(0x0000), FONT_PAGE_REPLACEMENT);
    CHECK_EQ(font_page_slot(0x0001), FONT_PAGE_REPLACEMENT);
    CHECK_EQ(font_page_slot(0x007F), FONT_PAGE_REPLACEMENT);
    CHECK_EQ(font_page_slot(0x10FFFF), FONT_PAGE_REPLACEMENT);
    CHECK_EQ(font_page_slot(0xFFFFFFFFu), FONT_PAGE_REPLACEMENT);
    CHECK_EQ(font_page_codepoint(FONT_PAGE_REPLACEMENT), UNICODE_REPLACEMENT);

    /* Every slot resolves to a real glyph — no hole can render as garbage. */
    for (int slot = 0; slot < 256; slot++) {
        uint16_t g = font_page_glyph((uint8_t)slot);
        CHECK(g < font_8x16.nglyphs);
        CHECK(font_glyph_bits(&font_8x16, (int)g) != (const uint8_t *)0);
    }
    /* Slot 0 is a blank, not U+0000: an untouched cell must not be a box. */
    const uint8_t *z = font_glyph_bits(&font_8x16, (int)font_page_glyph(0));
    for (int r = 0; r < 16; r++) CHECK_EQ(z[r], 0);

    /* Round trip: a slot's own code point maps back to that slot. The dozen
     * exceptions are the slots whose code point is a duplicate (space, NBSP). */
    for (int slot = 0; slot < 256; slot++) {
        uint32_t cp = font_page_codepoint((uint8_t)slot);
        int      back = font_page_slot(cp);
        CHECK(back == slot || font_page_codepoint((uint8_t)back) == cp);
    }
}

/* ====================================================================== */
/* UTF-8: the counted decoder                                             */
/* ====================================================================== */

static void expect_decode(const char *bytes, size_t n, size_t want_used,
                          uint32_t want_cp) {
    uint32_t cp = 0;
    size_t   used = utf8_decode(bytes, n, &cp);
    CHECK_EQ(used, want_used);
    CHECK_EQ(cp, want_cp);
}

static void test_utf8_decode_valid(void) {
    expect_decode("A", 1, 1, 'A');
    expect_decode("\x00", 1, 1, 0);                       /* NUL is valid     */
    expect_decode("\x7F", 1, 1, 0x7F);
    expect_decode("\xC2\x80", 2, 2, 0x80);                /* smallest 2-byte  */
    expect_decode("\xDF\xBF", 2, 2, 0x7FF);               /* largest 2-byte   */
    expect_decode("\xE0\xA0\x80", 3, 3, 0x800);           /* smallest 3-byte  */
    expect_decode("\xEF\xBF\xBF", 3, 3, 0xFFFF);          /* largest 3-byte   */
    expect_decode("\xF0\x90\x80\x80", 4, 4, 0x10000);     /* smallest 4-byte  */
    expect_decode("\xF4\x8F\xBF\xBF", 4, 4, 0x10FFFF);    /* largest legal    */

    expect_decode("\xE2\x80\x94", 3, 3, 0x2014);          /* em dash          */
    expect_decode("\xE2\x80\xA6", 3, 3, 0x2026);          /* ellipsis         */
    expect_decode("\xE2\x94\x80", 3, 3, 0x2500);          /* box horizontal   */
    expect_decode("\xF0\x9F\x98\x80", 4, 4, 0x1F600);     /* emoji            */

    /* Extra bytes after a complete character are not consumed. */
    expect_decode("\xE2\x80\x94" "AB", 5, 3, 0x2014);
    expect_decode("AB", 2, 1, 'A');
}

static void test_utf8_decode_malformed(void) {
    /* A lone continuation byte. One byte consumed, so a loop resynchronises. */
    for (int b = 0x80; b <= 0xBF; b++) {
        char s[1] = { (char)b };
        expect_decode(s, 1, 1, UNICODE_REPLACEMENT);
    }
    /* Lead bytes that cannot start anything. */
    for (int b = 0xC0; b <= 0xC1; b++) {
        char s[2] = { (char)b, (char)0x80 };
        expect_decode(s, 2, 1, UNICODE_REPLACEMENT);
    }
    for (int b = 0xF5; b <= 0xFF; b++) {
        char s[4] = { (char)b, (char)0x80, (char)0x80, (char)0x80 };
        expect_decode(s, 4, 1, UNICODE_REPLACEMENT);
    }

    /* Overlong encodings of characters that have a shorter form. A complete
     * sequence with an illegal value is ONE bad character, so it consumes all
     * of itself — see the maximal-subpart rule in font.h. */
    expect_decode("\xC0\xAF", 2, 1, UNICODE_REPLACEMENT);        /* C0 is not
                                                                    a lead at
                                                                    all       */
    expect_decode("\xE0\x80\xAF", 3, 3, UNICODE_REPLACEMENT);
    expect_decode("\xE0\x9F\xBF", 3, 3, UNICODE_REPLACEMENT);    /* U+07FF   */
    expect_decode("\xF0\x80\x80\xAF", 4, 4, UNICODE_REPLACEMENT);
    expect_decode("\xF0\x8F\xBF\xBF", 4, 4, UNICODE_REPLACEMENT); /* U+FFFF  */

    /* Surrogate halves — what a naive UTF-16-to-UTF-8 converter emits. */
    expect_decode("\xED\xA0\x80", 3, 3, UNICODE_REPLACEMENT);    /* U+D800   */
    expect_decode("\xED\xBF\xBF", 3, 3, UNICODE_REPLACEMENT);    /* U+DFFF   */
    expect_decode("\xED\xA0\xBD\xED\xB8\x80", 6, 3, UNICODE_REPLACEMENT);

    /* Above the last plane. */
    expect_decode("\xF4\x90\x80\x80", 4, 4, UNICODE_REPLACEMENT);
    expect_decode("\xF4\xBF\xBF\xBF", 4, 4, UNICODE_REPLACEMENT);

    /* Truncated tails, at every length and in every position: the lead and the
     * continuation bytes that did arrive are consumed together. */
    expect_decode("\xC2", 1, 1, UNICODE_REPLACEMENT);
    expect_decode("\xE2", 1, 1, UNICODE_REPLACEMENT);
    expect_decode("\xE2\x80", 2, 2, UNICODE_REPLACEMENT);
    expect_decode("\xF0", 1, 1, UNICODE_REPLACEMENT);
    expect_decode("\xF0\x9F", 2, 2, UNICODE_REPLACEMENT);
    expect_decode("\xF0\x9F\x98", 3, 3, UNICODE_REPLACEMENT);

    /* A lead byte followed by a non-continuation: one byte consumed, so the
     * NEXT call sees the 'A' rather than swallowing it. */
    {
        uint32_t cp;
        CHECK_EQ(utf8_decode("\xE2\x80" "A", 3, &cp), 2u);
        CHECK_EQ(cp, UNICODE_REPLACEMENT);
        CHECK_EQ(utf8_decode("A", 1, &cp), 1u);
        CHECK_EQ(cp, 'A');
        CHECK_EQ(utf8_decode("\x80" "A", 2, &cp), 1u);
        CHECK_EQ(cp, UNICODE_REPLACEMENT);
        CHECK_EQ(utf8_decode("A", 1, &cp), 1u);
        CHECK_EQ(cp, 'A');
    }

    /* Degenerate arguments. */
    {
        uint32_t cp = 0x1234;
        CHECK_EQ(utf8_decode("A", 0, &cp), 0u);
        CHECK_EQ(cp, UNICODE_REPLACEMENT);
        CHECK_EQ(utf8_decode((const char *)0, 4, &cp), 0u);
        CHECK_EQ(utf8_decode("A", 1, (uint32_t *)0), 0u);
    }
}

/* Every 1..3 byte string, decoded. The only invariants asserted are the ones
 * that keep the console alive: progress is always made, and a decoded value is
 * always a legal code point. */
static void test_utf8_decode_exhaustive(void) {
    unsigned char s[3];
    long          replaced = 0, ok = 0;
    for (int a = 0; a < 256; a++) {
        s[0] = (unsigned char)a;
        for (int b = 0; b < 256; b++) {
            s[1] = (unsigned char)b;
            for (int c = 0; c < 256; c += 17) {     /* every 17th: 45k triples */
                s[2] = (unsigned char)c;
                uint32_t cp   = 0;
                size_t   used = utf8_decode((const char *)s, 3, &cp);
                CHECK(used >= 1 && used <= 3);
                CHECK(cp <= 0x10FFFF);
                CHECK(!(cp >= 0xD800 && cp <= 0xDFFF));
                if (cp == UNICODE_REPLACEMENT) replaced++;
                else ok++;
            }
        }
    }
    CHECK(replaced > 0);
    CHECK(ok > 0);
}

/* ====================================================================== */
/* UTF-8: the byte-at-a-time decoder kputc() uses                          */
/* ====================================================================== */

/* Feed a byte string through utf8_feed exactly as kputc does — including the
 * single retry — and collect the code points it yields. */
static int feed_string(const char *bytes, size_t n, uint32_t *out, int cap) {
    utf8_stream_t st;
    utf8_stream_reset(&st);
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)bytes[i];
        for (;;) {
            uint32_t cp;
            int      rc = utf8_feed(&st, b, &cp);
            if (rc == UTF8_MORE) break;
            if (k < cap) out[k] = cp;
            k++;
            if (rc != UTF8_BAD_RETRY) break;
        }
    }
    return k;
}

static void test_utf8_stream(void) {
    uint32_t got[16];

    /* The two decoders must agree on well-formed input. */
    CHECK_EQ(feed_string("A\xE2\x80\x94" "B", 5, got, 16), 3);
    CHECK_EQ(got[0], 'A');
    CHECK_EQ(got[1], 0x2014);
    CHECK_EQ(got[2], 'B');

    CHECK_EQ(feed_string("\xF0\x9F\x98\x80", 4, got, 16), 1);
    CHECK_EQ(got[0], 0x1F600);

    /* An incomplete character at the end of the stream yields nothing at all —
     * the console has no way to know whether the rest is coming. */
    CHECK_EQ(feed_string("\xE2\x80", 2, got, 16), 0);

    /* THE RESYNC CASE. A truncated sequence followed by an ASCII byte must
     * produce a replacement AND the ASCII byte. Losing the 'A' here is the
     * classic streaming-decoder bug. */
    CHECK_EQ(feed_string("\xE2\x80" "A", 3, got, 16), 2);
    CHECK_EQ(got[0], UNICODE_REPLACEMENT);
    CHECK_EQ(got[1], 'A');

    /* Same, where the interrupting byte starts a valid multi-byte character. */
    CHECK_EQ(feed_string("\xE2\xE2\x80\x94", 4, got, 16), 2);
    CHECK_EQ(got[0], UNICODE_REPLACEMENT);
    CHECK_EQ(got[1], 0x2014);

    /* Newline must survive an interrupted sequence: if it did not, the console
     * would keep printing on the same line. */
    CHECK_EQ(feed_string("\xE2\x80\n", 3, got, 16), 2);
    CHECK_EQ(got[0], UNICODE_REPLACEMENT);
    CHECK_EQ(got[1], '\n');

    /* Stray continuation bytes are one replacement each, not a swallowed run. */
    CHECK_EQ(feed_string("\x80\x81\x82", 3, got, 16), 3);
    for (int i = 0; i < 3; i++) CHECK_EQ(got[i], UNICODE_REPLACEMENT);

    /* Overlongs and surrogates are rejected at the point the value is known,
     * i.e. after the last continuation byte, and consume the whole sequence. */
    CHECK_EQ(feed_string("\xE0\x80\xAF" "Z", 4, got, 16), 2);
    CHECK_EQ(got[0], UNICODE_REPLACEMENT);
    CHECK_EQ(got[1], 'Z');
    CHECK_EQ(feed_string("\xED\xA0\x80" "Z", 4, got, 16), 2);
    CHECK_EQ(got[0], UNICODE_REPLACEMENT);
    CHECK_EQ(got[1], 'Z');
    CHECK_EQ(feed_string("\xF4\x90\x80\x80" "Z", 5, got, 16), 2);
    CHECK_EQ(got[0], UNICODE_REPLACEMENT);
    CHECK_EQ(got[1], 'Z');

    /* utf8_feed tolerates a NULL state and a NULL output. */
    CHECK_EQ(utf8_feed((utf8_stream_t *)0, 'A', got), UTF8_BAD);
    {
        utf8_stream_t st;
        utf8_stream_reset(&st);
        CHECK_EQ(utf8_feed(&st, 'A', (uint32_t *)0), UTF8_OK);
        utf8_stream_reset((utf8_stream_t *)0);      /* must not fault */
    }

    /* Fuzz: every byte pair, fed as a stream. The invariant is that a stream
     * never emits an illegal code point and never emits more characters than
     * bytes — the two ways a decoder inside kputc could corrupt a console. */
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b++) {
            char s[2] = { (char)a, (char)b };
            int  k    = feed_string(s, 2, got, 16);
            CHECK(k >= 0 && k <= 2);
            for (int i = 0; i < k && i < 16; i++) {
                CHECK(got[i] <= 0x10FFFF);
                CHECK(!(got[i] >= 0xD800 && got[i] <= 0xDFFF));
            }
        }
}

/* ====================================================================== */
/* drawing text                                                           */
/* ====================================================================== */

/* Assert that the 8x16 cell at (x,y) is exactly glyph `bits` in fg/bg. */
static void check_cell(int32_t x, int32_t y, const uint8_t *bits,
                       fb_color_t fg, fb_color_t bg) {
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 8; c++) {
            int on = (bits[r] >> (7 - c)) & 1;
            CHECK_EQ(at((int)x + c, (int)y + r), on ? fg : bg);
        }
}

static void test_draw_glyph_exact_pixels(void) {
    /* A surface exactly one cell wide and tall, with canaries around it. */
    static fb_color_t cell[PAD + 8 * 16 + PAD];
    fb_surface_t      s;
    for (size_t i = 0; i < sizeof cell / sizeof cell[0]; i++) cell[i] = CANARY;
    for (int i = 0; i < 8 * 16; i++) cell[PAD + i] = 0;
    CHECK_EQ(fb_surface_init(&s, cell + PAD, 8, 16, 8), 0);

    int adv = fb_draw_codepoint(&s, &font_8x16, 0, 0, 0x2014, WHITE, BLUE, 1);
    CHECK_EQ(adv, 8);
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 8; c++) {
            int on = (kEmDash[r] >> (7 - c)) & 1;
            CHECK_EQ(cell[PAD + r * 8 + c], on ? WHITE : BLUE);
        }
    for (int i = 0; i < PAD; i++) {
        CHECK_EQ(cell[i], CANARY);
        CHECK_EQ(cell[PAD + 8 * 16 + i], CANARY);
    }

    /* Transparent: background pixels are left as they were. */
    for (int i = 0; i < 8 * 16; i++) cell[PAD + i] = GREEN;
    fb_draw_codepoint(&s, &font_8x16, 0, 0, 0x2014, WHITE, BLUE, 0);
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 8; c++) {
            int on = (kEmDash[r] >> (7 - c)) & 1;
            CHECK_EQ(cell[PAD + r * 8 + c], on ? WHITE : GREEN);
        }
}

static void test_draw_unmapped_shows_replacement(void) {
    const uint8_t *repl = font_glyph_bits(&font_8x16, FONT_GLYPH_REPLACEMENT);
    CHECK(repl != (const uint8_t *)0);
    /* The replacement glyph must not be blank, or "unmapped" would be
     * indistinguishable from "nothing printed". */
    int lit = 0;
    for (int r = 0; r < 16; r++) if (repl[r]) lit++;
    CHECK(lit >= 4);

    surface_reset();
    fb_draw_codepoint(&surf, &font_8x16, 0, 0, 0x1F600, WHITE, 0, 1);
    check_cell(0, 0, repl, WHITE, 0);

    /* An explicitly-out-of-range glyph index falls back the same way. */
    surface_reset();
    fb_draw_glyph(&surf, &font_8x16, 0, 0, 99999, WHITE, 0, 1);
    check_cell(0, 0, repl, WHITE, 0);
    surface_reset();
    fb_draw_glyph(&surf, &font_8x16, 0, 0, -5, WHITE, 0, 1);
    check_cell(0, 0, repl, WHITE, 0);
    CHECK(untouched_outside());
}

static void test_draw_glyph_clips_at_every_edge(void) {
    const uint8_t *bits = font_glyph_bits(&font_8x16,
                                          font_glyph_index(&font_8x16, 'A'));
    struct { int32_t x, y; } spots[] = {
        { -3, 4 }, { SW - 5, 4 }, { 4, -5 }, { 4, SH - 5 },
        { -3, -5 }, { SW - 5, SH - 5 }, { -8, 0 }, { SW, 0 },
        { 0, -16 }, { 0, SH }, { -100000, 3 }, { 3, 100000 },
    };
    for (unsigned i = 0; i < sizeof spots / sizeof spots[0]; i++) {
        surface_reset();
        int adv = fb_draw_glyph(&surf, &font_8x16, spots[i].x, spots[i].y,
                               font_glyph_index(&font_8x16, 'A'), WHITE, BLUE, 1);
        /* The advance is the font's width whether or not anything was drawn:
         * a caller laying out a line must not have its pen moved by clipping. */
        CHECK_EQ(adv, 8);

        /* Every pixel of the surface, checked against the expected clip. */
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) {
                int gx = x - (int)spots[i].x, gy = y - (int)spots[i].y;
                if (gx < 0 || gx >= 8 || gy < 0 || gy >= 16) {
                    CHECK_EQ(at(x, y), 0u);
                } else {
                    int on = (bits[gy] >> (7 - gx)) & 1;
                    CHECK_EQ(at(x, y), on ? WHITE : BLUE);
                }
            }
        CHECK(untouched_outside());
    }
}

static void test_draw_utf8(void) {
    surface_reset();
    /* "A—B": three cells, the middle one an em dash from three bytes. */
    int adv = fb_draw_utf8(&surf, &font_8x16, 0, 0, "A\xE2\x80\x94" "B", 5,
                           WHITE, 0, 1);
    CHECK_EQ(adv, 24);
    check_cell(0, 0, kCapitalA, WHITE, 0);
    check_cell(8, 0, kEmDash, WHITE, 0);
    check_cell(16, 0, font_glyph_bits(&font_8x16,
                                      font_glyph_index(&font_8x16, 'B')),
               WHITE, 0);

    /* A malformed byte becomes exactly one replacement cell and does not eat
     * the character after it. */
    surface_reset();
    CHECK_EQ(fb_draw_utf8(&surf, &font_8x16, 0, 0, "\xE2\x80" "A", 3, WHITE, 0, 1),
             16);   /* two cells: one replacement, then the 'A' */
    check_cell(0, 0, font_glyph_bits(&font_8x16, FONT_GLYPH_REPLACEMENT), WHITE, 0);
    check_cell(8, 0, kCapitalA, WHITE, 0);

    /* The advance does not depend on clipping. */
    CHECK_EQ(fb_utf8_columns("A\xE2\x80\x94" "B", 5), 3);
    CHECK_EQ(fb_utf8_columns("\xE2\x80" "A", 3), 2);
    CHECK_EQ(fb_utf8_columns("", 0), 0);
    CHECK_EQ(fb_utf8_columns((const char *)0, 5), 0);
    surface_reset();
    CHECK_EQ(fb_draw_utf8(&surf, &font_8x16, -1000, -1000, "hello", 5, WHITE, 0, 1),
             40);
    CHECK_EQ(count_of(0u), SW * SH);

    /* Degenerate arguments. */
    CHECK_EQ(fb_draw_utf8(&surf, (const font_t *)0, 0, 0, "x", 1, WHITE, 0, 1), 0);
    CHECK_EQ(fb_draw_utf8(&surf, &font_8x16, 0, 0, (const char *)0, 5, WHITE, 0, 1), 0);
    CHECK_EQ(fb_draw_utf8((fb_surface_t *)0, &font_8x16, 0, 0, "x", 1, WHITE, 0, 1), 8);
    CHECK(untouched_outside());

    /* Embedded NUL is a code point like any other: five cells, not one. */
    surface_reset();
    CHECK_EQ(fb_draw_utf8(&surf, &font_8x16, 0, 0, "a\0b\0c", 5, WHITE, 0, 1), 40);
    CHECK_EQ(fb_utf8_columns("a\0b\0c", 5), 5);
}

/* Text drawn inside a clip rectangle: the GUI's fundamental case (a label in a
 * box), and the one place a bug shows up as painting over someone else's chrome. */
static void test_text_inside_a_clip(void) {
    surface_reset();
    fb_clip_set(&surf, 4, 4, 10, 8);
    fb_draw_utf8(&surf, &font_8x16, 0, 0, "AAA", 3, WHITE, BLUE, 1);
    fb_clip_reset(&surf);
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            int inside = x >= 4 && x < 14 && y >= 4 && y < 12;
            if (!inside) CHECK_EQ(at(x, y), 0u);
            else CHECK(at(x, y) == WHITE || at(x, y) == BLUE);
        }
    CHECK(untouched_outside());
}

/* ====================================================================== */
/* the hardware half is absent on the host, and says so                   */
/* ====================================================================== */

static void test_hardware_stubs(void) {
    CHECK_EQ(fb_hw_init(), -1);
    CHECK(fb_back() == (fb_surface_t *)0);
    CHECK_EQ(fb_hw_info()->active, 0);
    CHECK_STR(fb_hw_info()->source, "none");
    CHECK_STR(multiboot_cmdline(), "");
    CHECK_EQ(fb_forced_off(), 0);
    fb_present();                       /* must not fault with no framebuffer */
    fb_present_rect(0, 0, 10, 10);

    /* The palette a console attribute byte indexes. */
    CHECK_EQ(fb_vga_palette[0], FB_RGB(0, 0, 0));
    CHECK_EQ(fb_vga_palette[7], FB_RGB(0xAA, 0xAA, 0xAA));
    CHECK_EQ(fb_vga_palette[15], FB_RGB(0xFF, 0xFF, 0xFF));
    for (int i = 0; i < 16; i++) CHECK_EQ(fb_vga_palette[i] >> 24, 0u);
}

/* ====================================================================== */

int main(void) {
    console_init();

    RUN(test_surface_init_rejects_nonsense);
    RUN(test_clip_intersects_and_never_widens);
    RUN(test_pixels);
    RUN(test_fill_rect_clips_at_every_edge);
    RUN(test_frame_and_lines);
    RUN(test_blit_clips_both_sides);
    RUN(test_blit_overlapping_same_surface);
    RUN(test_blit_keyed);
    RUN(test_scroll);
    RUN(test_font_geometry_and_lookup);
    RUN(test_code_page);
    RUN(test_utf8_decode_valid);
    RUN(test_utf8_decode_malformed);
    RUN(test_utf8_decode_exhaustive);
    RUN(test_utf8_stream);
    RUN(test_draw_glyph_exact_pixels);
    RUN(test_draw_unmapped_shows_replacement);
    RUN(test_draw_glyph_clips_at_every_edge);
    RUN(test_draw_utf8);
    RUN(test_text_inside_a_clip);
    RUN(test_hardware_stubs);

    return th_report("fb");
}
