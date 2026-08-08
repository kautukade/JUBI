/* font.h — bitmap fonts, UTF-8 decoding, and the console's 256-slot code page.
 *
 * PURPOSE
 *   Everything between "the kernel emitted some bytes" and "these pixels are
 *   lit" that is not itself drawing. Three separable jobs live here because they
 *   are the same job seen from three distances:
 *
 *     1. A FONT is a block of 1-bit glyph bitmaps plus a codepoint -> glyph
 *        index map. lib/fb.c asks it "which 16 bytes draw U+2014?" and gets an
 *        answer or an explicit miss. Nothing else in the kernel needs to know
 *        the bitmaps exist.
 *
 *     2. UTF-8 DECODING, in two shapes. utf8_decode() reads a code point out of
 *        a counted buffer, which is what a drawing call over a string wants.
 *        utf8_feed() is a one-byte-at-a-time state machine, which is what
 *        kputc(char) has no choice but to want: the console's entire interface
 *        to the rest of the kernel is a single byte, so a multi-byte character
 *        necessarily arrives across several calls. Both reject the same things
 *        the same way (see below).
 *
 *     3. The CONSOLE CODE PAGE. lib/base.c's text grid is one uint16_t per cell,
 *        attribute<<8 | byte, because tools/screen_tools.c reads and paints that
 *        grid through console_fb() and that contract predates this file. A byte
 *        cannot index a ~1000-glyph font, so the console renders through a
 *        256-slot page: font_page_slot() turns a code point into a slot, and
 *        font_page_glyph() turns a slot into a glyph.
 *
 * WHAT "CORRECT" MEANS FOR UNTRUSTED BYTES
 *   The byte stream reaching kputc() includes model output, so it includes every
 *   malformed sequence there is. The rules, identical in both decoders:
 *
 *     - a continuation byte with no lead (0x80-0xBF)          -> U+FFFD
 *     - a lead byte that cannot start a sequence (0xC0, 0xC1,
 *       0xF5-0xFF)                                            -> U+FFFD
 *     - an overlong encoding (0xE0 0x80.., 0xF0 0x80..)       -> U+FFFD
 *     - a surrogate, U+D800-U+DFFF (CESU-8 / WTF-8 input)     -> U+FFFD
 *     - a code point above U+10FFFF                           -> U+FFFD
 *     - a truncated sequence, i.e. a lead followed by anything
 *       that is not a continuation byte                       -> U+FFFD, and the
 *       offending byte is then RE-EXAMINED as a fresh lead
 *
 *   That last rule is the one that matters: it means a stream never loses
 *   synchronisation. "\xE2\x80" followed by 'A' prints one replacement box and
 *   then an 'A', not a swallowed 'A' and not a hang.
 *
 *   ONE BAD RUN IS ONE REPLACEMENT. Both decoders consume the "maximal subpart"
 *   — the lead byte plus every following byte that could still belong to that
 *   sequence — and emit a single U+FFFD for it. So "\xE2\x80" 'A' is two
 *   characters, not three, and a complete-but-illegal sequence (an overlong, a
 *   surrogate, a value above U+10FFFF) is one character, not one per byte. That
 *   equivalence is what lets a host test assert that utf8_decode() over a buffer
 *   and utf8_feed() over the same bytes produce the same thing.
 *
 *   U+0000 is a valid code point and decodes as one. It is the console's problem
 *   what to do with it, not the decoder's.
 *
 * PUBLIC API
 *   const font_t font_8x16;                    the embedded font
 *   int  font_glyph_index(const font_t*, uint32_t cp);   -1 if unmapped
 *   const uint8_t *font_glyph_bits(const font_t*, int glyph);
 *
 *   size_t utf8_decode(const char *s, size_t n, uint32_t *cp);
 *   void   utf8_stream_reset(utf8_stream_t *);
 *   int    utf8_feed(utf8_stream_t *, uint8_t byte, uint32_t *cp);
 *
 *   const font_page_t console_page;
 *   int      font_page_slot(uint32_t cp);      always 0x00-0xFF
 *   uint16_t font_page_glyph(uint8_t slot);
 *   uint32_t font_page_codepoint(uint8_t slot);
 *
 * DEPENDENCIES
 *   stdint.h, stddef.h. Nothing else — this file is pure logic and compiles and
 *   runs unchanged on the host, which is how tests/host/test_fb.c can assert
 *   exact glyph bitmaps and exact decoder verdicts.
 *
 * FUTURE EXTENSION POINTS
 *   - font_t is deliberately a value, not a singleton: a second font (a 16x32
 *     for a title bar, Spleen ships one) drops in as another font_t with no
 *     change to lib/fb.c.
 *   - stride is bytes per glyph row, so a wider font than 8 pixels needs no
 *     format change.
 *   - the page is a value too (font_page_t), so a GUI that wants a different
 *     256 characters in the text console can supply one.
 */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stddef.h>

/* ---- fonts ---------------------------------------------------------------- */

/* A run of code points mapped onto a run of glyph indices. */
typedef struct {
    uint32_t first;      /* first code point in the run                       */
    uint32_t last;       /* last code point, inclusive                        */
    uint16_t glyph;      /* glyph index of `first`; the run is contiguous      */
} font_range_t;

typedef struct {
    uint8_t             width;      /* pixels                                 */
    uint8_t             height;     /* pixels = rows per glyph                */
    uint8_t             stride;     /* bytes per glyph row (1 for 8px wide)    */
    uint16_t            nglyphs;
    const uint8_t      *bits;       /* nglyphs * height * stride bytes         */
    const font_range_t *ranges;     /* sorted by `first`, non-overlapping      */
    uint16_t            nranges;
    const char         *name;
} font_t;

extern const font_t font_8x16;

/* Glyph index for `cp`, or -1 when the font has no glyph for it. Never returns
 * a "close enough" glyph: an unmapped code point must be visibly unmapped. */
int font_glyph_index(const font_t *f, uint32_t cp);

/* First byte of glyph `g`'s bitmap (height*stride bytes), or NULL if out of
 * range. Row r, byte b is bits[(g*height + r)*stride + b], bit 7 leftmost. */
const uint8_t *font_glyph_bits(const font_t *f, int g);

/* Glyph 0 of every font is the replacement glyph, so an index that has somehow
 * escaped its bounds and been clamped to 0 still draws "unmapped". */
#define FONT_GLYPH_REPLACEMENT 0
#define UNICODE_REPLACEMENT    0xFFFDu

/* ---- UTF-8 --------------------------------------------------------------- */

/* Decode one code point from s[0..n). Returns the number of bytes consumed,
 * always >= 1 when n > 0, so a loop over a buffer always terminates. Returns 0
 * only for n == 0. *cp is UNICODE_REPLACEMENT for any malformed sequence, and
 * exactly one byte is consumed in that case so the next call resynchronises. */
size_t utf8_decode(const char *s, size_t n, uint32_t *cp);

/* The incremental decoder, for kputc(). `pending` is how many continuation
 * bytes are still expected; zero means "between characters". */
typedef struct {
    uint32_t acc;        /* code point accumulated so far                     */
    uint8_t  pending;    /* continuation bytes still expected                 */
    uint8_t  seen;       /* continuation bytes already taken                   */
    uint32_t lowest;     /* smallest value this lead byte may legally produce  */
} utf8_stream_t;

enum {
    UTF8_MORE      = 0,  /* byte absorbed, character incomplete               */
    UTF8_OK        = 1,  /* *cp is a code point; byte consumed                */
    UTF8_BAD       = 2,  /* *cp = U+FFFD; byte consumed                       */
    UTF8_BAD_RETRY = 3,  /* *cp = U+FFFD for the abandoned sequence, and this
                          * byte was NOT consumed — feed it again             */
};

void utf8_stream_reset(utf8_stream_t *st);
int  utf8_feed(utf8_stream_t *st, uint8_t byte, uint32_t *cp);

/* ---- the console code page ----------------------------------------------- */

typedef struct {
    uint32_t codepoint;
    uint8_t  slot;
} font_pageslot_t;

typedef struct {
    const uint16_t        *slot_glyph;      /* 256 glyph indices              */
    const uint32_t        *slot_codepoint;  /* 256 code points               */
    const font_pageslot_t *by_codepoint;    /* sorted by codepoint            */
    uint16_t               n_by_codepoint;
} font_page_t;

extern const font_page_t console_page;
extern const uint16_t    console_page_glyph[256];
extern const uint32_t    console_page_codepoint[256];

/* The slot for a code point: its own slot if it has one, the slot of an honest
 * equivalent if the generator declared a fold (a heavy box corner drawn light,
 * U+2212 MINUS SIGN drawn as '-'), and FONT_PAGE_REPLACEMENT otherwise. Never
 * returns an unrelated glyph and never returns "nothing". */
int font_page_slot(uint32_t cp);

#define FONT_PAGE_REPLACEMENT 0x7F   /* was CP437's ⌂; now the unmapped box */

uint16_t font_page_glyph(uint8_t slot);
uint32_t font_page_codepoint(uint8_t slot);

#endif /* FONT_H */
