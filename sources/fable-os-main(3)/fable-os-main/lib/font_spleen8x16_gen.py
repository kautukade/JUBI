#!/usr/bin/env python3
"""font_spleen8x16_gen.py — generate lib/font_spleen8x16.c from the Spleen BDFs.

PURPOSE
    lib/font_spleen8x16.c is a GENERATED file: ~1000 8x16 glyph bitmaps, the
    codepoint -> glyph range table, and the 256-slot console code page. It is
    committed, so the kernel build has no dependency on this script, on Python,
    or on a font file. This script exists so the provenance of those bytes is
    reproducible instead of mysterious: run it, diff the output, and you can see
    that nothing was hand-fudged.

SOURCE
    Spleen 2.1.0 by Frederic Cambus, BSD-2-Clause.
    https://github.com/fcambus/spleen/releases/download/2.1.0/spleen-2.1.0.tar.gz
        spleen-8x16.bdf                 969 glyphs, ISO10646 (Unicode) encoded
        cp437/spleen-8x16-ibm-437.bdf   256 glyphs, code page 437 order

    Both are a uniform BBX of 8 16 0 -4: every glyph is exactly 16 rows of one
    byte, MSB = leftmost pixel. That is why the kernel-side font format is
    literally "16 bytes per glyph" with no per-glyph metrics.

USAGE
    python3 lib/font_spleen8x16_gen.py <path-to-spleen-2.1.0> > lib/font_spleen8x16.c
"""

import sys
import os

# ---------------------------------------------------------------------------
# BDF reading
# ---------------------------------------------------------------------------

def parse_bdf(path):
    """-> {encoding: (name, [16 ints])}. Asserts the uniform 8x16 geometry."""
    out = {}
    enc = name = None
    rows = None
    bbx = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("STARTCHAR"):
                name, enc, rows, bbx = line[10:].strip(), None, None, None
            elif line.startswith("ENCODING"):
                enc = int(line.split()[1])
            elif line.startswith("BBX"):
                bbx = tuple(int(x) for x in line.split()[1:5])
            elif line == "BITMAP":
                rows = []
            elif line == "ENDCHAR":
                if enc is not None and enc >= 0:
                    assert bbx == (8, 16, 0, -4), (path, enc, bbx)
                    assert len(rows) == 16, (path, enc, len(rows))
                    out[enc] = (name, [int(r, 16) for r in rows])
                rows = None
            elif rows is not None:
                rows.append(line.strip())
    return out


# ---------------------------------------------------------------------------
# Hand-drawn glyphs Spleen 2.1.0 does not ship
#
# Spleen covers U+2016, U+2018-2019, U+201C-201D, U+2022 and U+2026 but has no
# dash longer than ASCII '-': U+2013 and U+2014 are simply absent, and they are
# the two characters a language model emits most often that a terminal font
# usually lacks. U+FFFD is absent too, and a replacement glyph is the whole
# point of admitting that a codepoint is unmapped. These five are drawn here,
# 8 columns wide, MSB left, 16 rows, on Spleen's baseline (row 11 is the
# baseline row, rows 12-15 are the descender).
# ---------------------------------------------------------------------------

def rows(*pattern):
    assert len(pattern) == 16, len(pattern)
    v = []
    for r in pattern:
        assert len(r) == 8, r
        v.append(int("".join("1" if c == "#" else "0" for c in r), 2))
    return v


HAND_DRAWN = {
    # EM DASH: a full-cell rule, so consecutive em dashes join up. Row 7 is
    # where Spleen puts the hyphen and U+2500 BOX DRAWINGS LIGHT HORIZONTAL, so
    # a dash, a hyphen and a rule all sit on the same line.
    0x2014: ("EM DASH", rows(
        "........", "........", "........", "........",
        "........", "........", "........", "########",
        "........", "........", "........", "........",
        "........", "........", "........", "........")),
    # EN DASH: the same 6-pixel rule as Spleen's hyphen, and that is the honest
    # limit of an 8-pixel cell. A hyphen and an en dash differ by about one
    # pixel at this size, so the only pair worth distinguishing is en versus em,
    # and those differ by two pixels AND by whether they join across cells.
    # What matters is that U+2013 draws a dash instead of an unmapped box.
    0x2013: ("EN DASH", rows(
        "........", "........", "........", "........",
        "........", "........", "........", ".######.",
        "........", "........", "........", "........",
        "........", "........", "........", "........")),
    # THE FOUR CURLY QUOTES.
    #   Spleen draws U+2018/2019/201C/201D as bitmap-identical copies of the
    #   ASCII ' and " — upright ticks. That is a defensible choice for a
    #   terminal font and a useless one here: a model writes "don't" with U+2019
    #   and 'quoted' with U+2018/2019 constantly, and a console that claims to
    #   render curly quotes while drawing straight ones is claiming something
    #   untrue. So these four are drawn as mirror-image diagonals, which is the
    #   most legible way to distinguish opening from closing in 8 pixels.
    #   Direction follows the typography: U+2019 is a raised comma, so its tail
    #   falls to the lower left; U+2018 is that shape rotated 180 degrees.
    0x2018: ("LEFT SINGLE QUOTATION MARK", rows(
        "........", "........", ".##.....", "..##....",
        "...##...", "........", "........", "........",
        "........", "........", "........", "........",
        "........", "........", "........", "........")),
    0x2019: ("RIGHT SINGLE QUOTATION MARK", rows(
        "........", "........", "...##...", "..##....",
        ".##.....", "........", "........", "........",
        "........", "........", "........", "........",
        "........", "........", "........", "........")),
    0x201C: ("LEFT DOUBLE QUOTATION MARK", rows(
        "........", "........", "##.##...", ".##.##..",
        "..##.##.", "........", "........", "........",
        "........", "........", "........", "........",
        "........", "........", "........", "........")),
    0x201D: ("RIGHT DOUBLE QUOTATION MARK", rows(
        "........", "........", "..##.##.", ".##.##..",
        "##.##...", "........", "........", "........",
        "........", "........", "........", "........",
        "........", "........", "........", "........")),
    # REPLACEMENT CHARACTER: a hollow box with a '?' inside is unmistakable and,
    # unlike U+FFFD's usual black diamond, still legible at 8x16.
    0xFFFD: ("REPLACEMENT CHARACTER", rows(
        "........", "........", ".######.", ".#....#.",
        ".#.##.#.", ".#...##.", ".#..##..", ".#..##..",
        ".#......", ".#..##..", ".#....#.", ".######.",
        "........", "........", "........", "........")),
    0x2713: ("CHECK MARK", rows(
        "........", "........", "......##", "......##",
        ".....##.", ".....##.", "....##..", "##..##..",
        "##.##...", ".####...", ".###....", "..#.....",
        "........", "........", "........", "........")),
    0x2717: ("BALLOT X", rows(
        "........", "........", "##....##", "##....##",
        ".##..##.", "..####..", "...##...", "..####..",
        ".##..##.", "##....##", "##....##", "........",
        "........", "........", "........", "........")),
}

# ---------------------------------------------------------------------------
# The console code page
#
# lib/base.c's text grid is one uint16_t per cell — attribute<<8 | page byte —
# because tools/screen_tools.c reads and writes that grid through console_fb().
# A byte cannot index a 969-glyph font, so the console renders through a 256-slot
# PAGE, and this is that page.
#
# 0x10-0xFF are code page 437, unchanged, for one specific reason: when there is
# no linear framebuffer the console falls back to VGA text mode, where the
# hardware font IS code page 437. Keeping the layout means the fallback renders
# those 240 slots identically instead of approximately.
#
# 0x00-0x0F and 0x7F are CP437's dingbats (smileys, card suits) and ⌂, which no
# kernel and no model has ever needed. They are re-used for the punctuation a
# language model actually emits and CP437 has no glyph for at all. Slots 0x08,
# 0x09, 0x0A and 0x0D are deliberately left as CP437 filler: kputc() consumes
# backspace, tab, newline and carriage return as cursor motion, so a glyph there
# could never be reached through the console anyway.
# ---------------------------------------------------------------------------

CP437 = [
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
] + list(range(0x20, 0x7F)) + [0x2302] + [
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
]
assert len(CP437) == 256, len(CP437)

PAGE_REPLACEMENT = 0x7F     # was CP437 ⌂ U+2302

# slot -> codepoint, overriding CP437 where the slot is being re-used.
PAGE = list(CP437)
PAGE[0x01] = 0x2014         # — EM DASH            (was ☺)
PAGE[0x02] = 0x2013         # – EN DASH            (was ☻)
PAGE[0x03] = 0x2018         # ' LEFT SINGLE QUOTE  (was ♥)
PAGE[0x04] = 0x2019         # ' RIGHT SINGLE QUOTE (was ♦)
PAGE[0x05] = 0x201C         # " LEFT DOUBLE QUOTE  (was ♣)
PAGE[0x06] = 0x201D         # " RIGHT DOUBLE QUOTE (was ♠)
PAGE[0x0B] = 0x2026         # … HORIZONTAL ELLIPSIS(was ♂)
PAGE[0x0C] = 0x2713         # ✓ CHECK MARK         (was ♀)
PAGE[0x0E] = 0x2717         # ✗ BALLOT X           (was ♫)
PAGE[PAGE_REPLACEMENT] = 0xFFFD

# Codepoints with no slot of their own that have an honest equivalent in the
# page. A fold is a substitution the reader would accept as the same character
# (a heavy box corner drawn light, a minus sign drawn as a hyphen); anything not
# listed here and not in the page renders as U+FFFD, never as a random glyph.
FOLD = {}


def fold(slot_cp, *cps):
    for cp in cps:
        FOLD[cp] = slot_cp


# Spaces of every width, and the ones that are not really spaces.
fold(0x20, 0x00A0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
     0x2006, 0x2007, 0x2008, 0x2009, 0x200A, 0x202F, 0x205F, 0x3000,
     0x200B, 0x200C, 0x200D, 0x2060, 0xFEFF)
# Dashes and minus signs that are not the em/en dash.
fold(0x2D, 0x00AD, 0x2010, 0x2011, 0x2012, 0x2015, 0x2043, 0x2212, 0x2796,
     0xFE58, 0xFE63, 0xFF0D)
# Apostrophes and primes.
fold(0x27, 0x02B9, 0x02BC, 0x02C8, 0x00B4, 0x2032, 0x2035, 0x201A, 0x201B,
     0xFF07)
fold(0x3C, 0x2039, 0x27E8, 0x3008)
fold(0x3E, 0x203A, 0x27E9, 0x3009)
fold(0x22, 0x201E, 0x201F, 0x2033, 0x2036)
fold(0x2F, 0x2044, 0x2215)
fold(0x78, 0x00D7, 0x2715, 0x2A2F)     # × as a lowercase x
fold(0x21, 0x203D)
fold(0x3A, 0x2236)
fold(0x7E, 0x223C, 0x02DC, 0xFF5E)
# Arrows: the double-struck and heavy families collapse onto the plain ones.
fold(0x1B, 0x21D0, 0x2B05, 0x2B60, 0x2B70, 0x27F5)
fold(0x18, 0x21D1, 0x2B06, 0x2B61, 0x27F0)
fold(0x1A, 0x21D2, 0x27A1, 0x2794, 0x2B62, 0x279C, 0x27F6, 0x21FE, 0x2B95)
fold(0x19, 0x21D3, 0x2B07, 0x2B63, 0x27F1)
fold(0x1D, 0x21D4, 0x27F7, 0x2B64)
fold(0x12, 0x21D5, 0x2B65)
# Bullets and small filled shapes.
fold(0x07, 0x2023, 0x25CF, 0x2981, 0x26AB, 0x25E6)
fold(0xFE, 0x25A1, 0x25AA, 0x25AB, 0x2B1B, 0x2B1C)      # ■
fold(0x0C, 0x2705, 0x2714)
fold(0x0E, 0x274C, 0x2718, 0x2716)
# Box drawing: the page carries CP437's light and double sets. Heavy, dashed and
# rounded members of U+2500..U+257F fold onto their light equivalent.
fold(0xC4, 0x2501, 0x2504, 0x2505, 0x2508, 0x2509, 0x254C, 0x254D, 0x257C, 0x257E)
fold(0xB3, 0x2503, 0x2506, 0x2507, 0x250A, 0x250B, 0x254E, 0x254F, 0x257D, 0x257F)
fold(0xDA, 0x250D, 0x250E, 0x250F, 0x256D)
fold(0xBF, 0x2511, 0x2512, 0x2513, 0x256E)
fold(0xC0, 0x2515, 0x2516, 0x2517, 0x2570)
fold(0xD9, 0x2519, 0x251A, 0x251B, 0x256F)
fold(0xC3, 0x251D, 0x251E, 0x251F, 0x2520, 0x2521, 0x2522, 0x2523)
fold(0xB4, 0x2525, 0x2526, 0x2527, 0x2528, 0x2529, 0x252A, 0x252B)
fold(0xC2, 0x252D, 0x252E, 0x252F, 0x2530, 0x2531, 0x2532, 0x2533)
fold(0xC1, 0x2535, 0x2536, 0x2537, 0x2538, 0x2539, 0x253A, 0x253B)
fold(0xC5, 0x253D, 0x253E, 0x253F, 0x2540, 0x2541, 0x2542, 0x2543, 0x2544,
     0x2545, 0x2546, 0x2547, 0x2548, 0x2549, 0x254A, 0x254B)
# Block elements: the page carries █ ▄ ▌ ▐ ▀ ░ ▒ ▓. The eighths fold to the
# nearest whole.
fold(0xDC, 0x2581, 0x2582, 0x2583, 0x2585, 0x2586, 0x2587)
fold(0xDB, 0x2589, 0x258A, 0x258B)
fold(0xDD, 0x258D, 0x258E, 0x258F)
fold(0xDF, 0x2594)
fold(0xDE, 0x2595)
fold(0xB1, 0x2596, 0x2597, 0x2598, 0x2599, 0x259A, 0x259B, 0x259C, 0x259D,
     0x259E, 0x259F)
# A handful of symbols with an obvious ASCII reading.
fold(0x2A, 0x2605, 0x2606, 0x272A, 0x2B50)


# ---------------------------------------------------------------------------
# Assemble
# ---------------------------------------------------------------------------

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fontwork/spleen-2.1.0"
    uni = parse_bdf(os.path.join(root, "spleen-8x16.bdf"))
    cp437 = parse_bdf(os.path.join(root, "cp437", "spleen-8x16-ibm-437.bdf"))

    # cp -> (name, bitmap). Unicode BDF first, then anything CP437 has that the
    # Unicode BDF is missing (► ◄ ∟ and friends), then the hand-drawn five.
    glyphs = {}
    for cp, (name, bits) in uni.items():
        glyphs[cp] = (name, bits)
    for slot, (name, bits) in cp437.items():
        cp = CP437[slot]
        if cp and cp not in glyphs:
            glyphs[cp] = ("CP437 0x%02X" % slot, bits)
    for cp, (name, bits) in HAND_DRAWN.items():
        glyphs[cp] = (name + " (fable-os)", bits)

    # Glyph 0 is always the replacement character: an out-of-range lookup that
    # lands on index 0 then draws something visible rather than a stray 'A'.
    order = [0xFFFD] + sorted(cp for cp in glyphs if cp != 0xFFFD)
    index = {cp: i for i, cp in enumerate(order)}

    # cp -> glyph index, as sorted contiguous runs (contiguous in BOTH cp and
    # index, which is why `order` is sorted).
    ranges = []
    for cp in order[1:]:
        i = index[cp]
        if ranges and ranges[-1][1] + 1 == cp and ranges[-1][2] + (ranges[-1][1] - ranges[-1][0]) + 1 == i:
            ranges[-1][1] = cp
        else:
            ranges.append([cp, cp, i])
    # U+FFFD is out of band above; add it as its own range.
    ranges.append([0xFFFD, 0xFFFD, 0])
    ranges.sort()

    # The page: slot -> glyph index. Slot 0 is blank (a space), not U+0000.
    page_glyph = []
    for slot in range(256):
        cp = PAGE[slot]
        if cp == 0:
            cp = 0x20
        assert cp in index, "page slot 0x%02X wants U+%04X, absent" % (slot, cp)
        page_glyph.append(index[cp])

    # cp -> slot, for the console's UTF-8 path: every page slot's own codepoint
    # plus every fold. Sorted, binary-searched in C.
    tos = {}
    for slot in range(256):
        cp = PAGE[slot]
        if cp >= 0x20 and cp not in tos:
            tos[cp] = slot
    for cp, slot_cp in FOLD.items():
        if cp not in tos:
            assert slot_cp < 0x100, hex(slot_cp)
            tos[cp] = slot_cp
    pagemap = sorted(tos.items())

    w = sys.stdout.write
    w("""/* font_spleen8x16.c — GENERATED. Do not edit; see lib/font_spleen8x16_gen.py.
 *
 * Spleen 8x16, %d glyphs, plus %d drawn for fable-os (see the generator).
 *
 *   Spleen 2.1.0, Copyright (c) 2018-2024, Frederic Cambus
 *   SPDX-License-Identifier: BSD-2-Clause
 *   https://github.com/fcambus/spleen
 *
 * Layout: 16 bytes per glyph, one byte per scanline top to bottom, bit 7 =
 * leftmost pixel. Glyph 0 is U+FFFD so that a lookup which somehow escapes its
 * bounds still draws a visible "unmapped" box.
 */

#include "font.h"

static const uint8_t spleen_bits[] = {
""" % (len(uni), len(HAND_DRAWN)))

    for cp in order:
        name, bits = glyphs[cp]
        w("    /* %4d U+%04X %s */\n    " % (index[cp], cp, name[:44]))
        w(" ".join("0x%02X," % b for b in bits))
        w("\n")
    w("};\n\n")

    w("static const font_range_t spleen_ranges[] = {\n")
    for a, b, i in ranges:
        w("    { 0x%04X, 0x%04X, %4d },\n" % (a, b, i))
    w("};\n\n")

    w("""const font_t font_8x16 = {
    .width      = 8,
    .height     = 16,
    .stride     = 1,
    .nglyphs    = %d,
    .bits       = spleen_bits,
    .ranges     = spleen_ranges,
    .nranges    = %d,
    .name       = "Spleen 8x16 (BSD-2-Clause) + fable-os additions",
};

/* --- the console's 256-slot code page --- */

const uint16_t console_page_glyph[256] = {
""" % (len(order), len(ranges)))
    for i in range(0, 256, 8):
        w("    " + " ".join("%4d," % g for g in page_glyph[i:i + 8]) + "\n")
    w("};\n\nconst uint32_t console_page_codepoint[256] = {\n")
    for i in range(0, 256, 8):
        w("    " + " ".join("0x%04X," % (PAGE[j] or 0x20) for j in range(i, i + 8)) + "\n")
    w("};\n\n")

    w("static const font_pageslot_t page_by_codepoint[] = {\n")
    for cp, slot in pagemap:
        w("    { 0x%05X, 0x%02X },\n" % (cp, slot))
    w("};\n\n")
    w("""const font_page_t console_page = {
    .slot_glyph     = console_page_glyph,
    .slot_codepoint = console_page_codepoint,
    .by_codepoint   = page_by_codepoint,
    .n_by_codepoint = %d,
};
""" % len(pagemap))

    sys.stderr.write("glyphs=%d ranges=%d pagemap=%d bytes=%d\n"
                     % (len(order), len(ranges), len(pagemap), len(order) * 16))


if __name__ == "__main__":
    main()
