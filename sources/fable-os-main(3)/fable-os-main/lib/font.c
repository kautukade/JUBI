/* font.c — glyph lookup, UTF-8 decoding, and the console code page.
 *
 * The contract is in include/font.h. This file is deliberately free of any
 * dependency beyond stdint/stddef: it is the piece that host tests can hammer
 * with malformed input a million times a second (tests/host/test_fb.c), and it
 * is also the piece that runs inside kputc() for every byte the kernel ever
 * prints, so it has no allocation, no locks and no I/O.
 */

#include "font.h"

/* ====================================================================== */
/* glyph lookup                                                           */
/* ====================================================================== */

/* Binary search over the font's sorted, non-overlapping code point runs. */
int font_glyph_index(const font_t *f, uint32_t cp) {
    if (!f || !f->ranges || !f->nranges) return -1;

    int lo = 0, hi = (int)f->nranges - 1;
    while (lo <= hi) {
        int                 mid = lo + (hi - lo) / 2;
        const font_range_t *r   = &f->ranges[mid];
        if (cp < r->first)      hi = mid - 1;
        else if (cp > r->last)  lo = mid + 1;
        else {
            uint32_t g = (uint32_t)r->glyph + (cp - r->first);
            /* A malformed table must not hand out an out-of-range glyph. */
            if (g >= f->nglyphs) return -1;
            return (int)g;
        }
    }
    return -1;
}

const uint8_t *font_glyph_bits(const font_t *f, int g) {
    if (!f || !f->bits) return (const uint8_t *)0;
    if (g < 0 || g >= (int)f->nglyphs) return (const uint8_t *)0;
    return f->bits + (size_t)g * (size_t)f->height * (size_t)f->stride;
}

/* ====================================================================== */
/* UTF-8                                                                  */
/* ====================================================================== */

/* Shared by both decoders so they cannot drift apart. */
static int lead_length(uint8_t b, uint32_t *acc, uint32_t *lowest) {
    if (b < 0x80)                { *acc = b;            *lowest = 0;       return 1; }
    if (b < 0xC2)                {                                         return 0; }
    if (b < 0xE0)                { *acc = b & 0x1Fu;    *lowest = 0x80;    return 2; }
    if (b < 0xF0)                { *acc = b & 0x0Fu;    *lowest = 0x800;   return 3; }
    if (b < 0xF5)                { *acc = b & 0x07u;    *lowest = 0x10000; return 4; }
    return 0;                    /* 0x80-0xC1 stray/overlong, 0xF5-0xFF     */
}

/* A decoded value is only legal if it is not a surrogate, not above the last
 * plane, and not shorter than its encoding claims. */
static int value_ok(uint32_t cp, uint32_t lowest) {
    if (cp < lowest)                  return 0;   /* overlong               */
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0; /* surrogate half         */
    if (cp > 0x10FFFFu)               return 0;
    return 1;
}

size_t utf8_decode(const char *s, size_t n, uint32_t *cp) {
    if (!cp) return 0;
    *cp = UNICODE_REPLACEMENT;
    if (!s || n == 0) return 0;

    uint32_t acc = 0, lowest = 0;
    int      len = lead_length((uint8_t)s[0], &acc, &lowest);
    if (len == 0) return 1;                        /* not a lead byte        */
    if (len == 1) { *cp = acc; return 1; }

    /* Consume the "maximal subpart": the lead plus every byte that could still
     * belong to this sequence. Stopping at the first byte that could not means
     * that byte is examined again as a fresh lead, which is what keeps a stream
     * synchronised — and it is exactly what utf8_feed() does, so the two
     * decoders always agree on how many replacement characters a bad run
     * produces. */
    size_t i = 1;
    for (; i < (size_t)len; i++) {
        if (i >= n) return i;                      /* truncated at the end   */
        uint8_t b = (uint8_t)s[i];
        if ((b & 0xC0u) != 0x80u) return i;        /* not part of this one   */
        acc = (acc << 6) | (uint32_t)(b & 0x3Fu);
    }
    /* A complete sequence encoding an illegal value (overlong, surrogate, above
     * the last plane) is one bad character, so it consumes all of itself. */
    if (!value_ok(acc, lowest)) return (size_t)len;
    *cp = acc;
    return (size_t)len;
}

void utf8_stream_reset(utf8_stream_t *st) {
    if (!st) return;
    st->acc = 0;
    st->pending = 0;
    st->seen = 0;
    st->lowest = 0;
}

int utf8_feed(utf8_stream_t *st, uint8_t byte, uint32_t *cp) {
    uint32_t sink;
    if (!cp) cp = &sink;
    *cp = UNICODE_REPLACEMENT;
    if (!st) return UTF8_BAD;

    if (st->pending) {
        if ((byte & 0xC0u) != 0x80u) {
            /* The sequence is over and this byte is not part of it. Report the
             * abandoned sequence and hand the byte back: dropping it here is
             * how decoders lose the character after a bad one. */
            utf8_stream_reset(st);
            return UTF8_BAD_RETRY;
        }
        st->acc = (st->acc << 6) | (uint32_t)(byte & 0x3Fu);
        st->seen++;
        if (st->seen < st->pending) return UTF8_MORE;

        uint32_t v = st->acc, lowest = st->lowest;
        utf8_stream_reset(st);
        if (!value_ok(v, lowest)) return UTF8_BAD;
        *cp = v;
        return UTF8_OK;
    }

    uint32_t acc = 0, lowest = 0;
    int      len = lead_length(byte, &acc, &lowest);
    if (len == 0) return UTF8_BAD;
    if (len == 1) { *cp = acc; return UTF8_OK; }

    st->acc     = acc;
    st->lowest  = lowest;
    st->pending = (uint8_t)(len - 1);
    st->seen    = 0;
    return UTF8_MORE;
}

/* ====================================================================== */
/* the console code page                                                  */
/* ====================================================================== */

int font_page_slot(uint32_t cp) {
    /* ASCII is the page's own identity range and by far the common case. */
    if (cp >= 0x20u && cp < 0x7Fu) return (int)cp;

    const font_pageslot_t *t = console_page.by_codepoint;
    int lo = 0, hi = (int)console_page.n_by_codepoint - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < t[mid].codepoint)      hi = mid - 1;
        else if (cp > t[mid].codepoint) lo = mid + 1;
        else                            return (int)t[mid].slot;
    }
    return FONT_PAGE_REPLACEMENT;
}

uint16_t font_page_glyph(uint8_t slot) { return console_page_glyph[slot]; }

uint32_t font_page_codepoint(uint8_t slot) { return console_page_codepoint[slot]; }
