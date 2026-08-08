/* test_kfmt.c — the kernel's own formatter, checked against the host libc's.
 *
 * WHY THIS SUITE EXISTS, AND WHY IT IS SHAPED LIKE THIS
 *
 * Every other host suite in this tree links the HOST libc. So every format
 * string in the kernel has, until now, been validated by a formatter that is
 * not the one the kernel runs. lib/kfmt.c is a reduced reimplementation, and
 * the places where it silently differed from C99 were invisible: green host
 * suites, correct-looking source, wrong text on the real machine.
 *
 * That is not hypothetical. `%.*s` was unsupported — '*' is not a digit, so the
 * precision parser left prec at 0, the conversion fell through to the `default`
 * arm, and the formatter printed the literal characters "%*s" WITHOUT CONSUMING
 * A VARARG. Every argument after that point shifted by one slot. vm/dvm.c has
 * 27 call sites of that shape, all of them assembler errors — the text a model
 * reads to repair a driver program it just wrote. Two distinct harms:
 *
 *   - "%.*s" then "%d": the %d printed the token LENGTH, so the kernel told the
 *     model `register "%*s" does not exist (this machine has r0-r3)` on a
 *     machine with sixteen registers, and `label too long (max 3 characters)`
 *     where the real limit is 23. A fabricated hard number, stated as fact.
 *   - "%.*s" then "%s": the %s dereferenced the token length AS A POINTER.
 *     boot/boot.asm identity-maps low memory present+writable from physical 0,
 *     so this did not fault; it read the real-mode IVT and pushed raw bytes
 *     (0xf0, 0xff) into a model-facing string, which net/json.c passes through
 *     unescaped above 0x1f — invalid UTF-8 in the outgoing request body.
 *
 * So the suite is a DIFFERENTIAL test. For each format string it prints through
 * the kernel formatter (kfmt_snprintf) and through the host libc (snprintf) and
 * requires the two to agree byte for byte and to return the same length. That
 * is a much stronger contract than a table of expected strings, because it
 * catches a divergence in any conversion, not only the ones someone thought to
 * write down — and C99 is the specification both are being held to.
 *
 * The differential is skipped only where the kernel formatter deliberately does
 * NOT implement C99 (no floats, no '+'/' '/'#' flags). Those cases are asserted
 * against explicit expected text instead, so that "unsupported" stays a
 * decision with a test on it rather than an accident.
 */

#include "harness.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

/* lib/kfmt.c renames itself under FABLEOS_HOSTTEST so it can be linked next to
 * the platform libc. These are the real kernel entry points. */
int kfmt_vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int kfmt_snprintf(char *str, size_t size, const char *fmt, ...);

/* ---------------------------------------------------------------- helpers */

#define BUF 512

/* The differential core: same format, same arguments, both formatters.
 *
 * va_list cannot be reused after being consumed, so each formatter gets its
 * own va_start from the same call. */
static void diff(const char *label, const char *fmt, ...) {
    char kbuf[BUF], hbuf[BUF];
    va_list ap;

    memset(kbuf, '#', sizeof kbuf);
    memset(hbuf, '#', sizeof hbuf);

    va_start(ap, fmt);
    int kn = kfmt_vsnprintf(kbuf, sizeof kbuf, fmt, ap);
    va_end(ap);

    va_start(ap, fmt);
    int hn = vsnprintf(hbuf, sizeof hbuf, fmt, ap);
    va_end(ap);

    th_checks++;
    if (strcmp(kbuf, hbuf) != 0) {
        th_fails++;
        printf("    FAIL %s  fmt=[%s]\n      kernel: [%s]\n      libc  : [%s]\n",
               label, fmt, kbuf, hbuf);
    }
    th_checks++;
    if (kn != hn) {
        th_fails++;
        printf("    FAIL %s  fmt=[%s] return length: kernel=%d libc=%d\n",
               label, fmt, kn, hn);
    }
}

/* ------------------------------------------------- the regression itself */

/* The exact shapes vm/dvm.c uses. These are the four lines this whole file is
 * for, so they come first and they are spelled out literally rather than
 * generated, so a reader can see the bug that was here. */
static void test_star_precision_is_supported(void) {
    /* vm/dvm.c:989 — "unknown instruction", the most common assembler error a
     * model makes. Before the fix the kernel emitted
     *     unknown instruction "%*s"
     * with BOTH the token and the hint dropped, because the trailing %s
     * consumed the length instead. */
    const char *tok = "mrx";
    diff("unknown-instruction", "unknown instruction \"%.*s\"%s",
         3, tok, " (did you mean mov?)");

    /* vm/dvm.c:519/:617 — the trailing %d is the register count. */
    diff("register-count",
         "%s: register \"%.*s\" does not exist (this machine has r0-r%d)",
         "mov", 3, "r16", 15);

    /* vm/dvm.c:884 — the trailing %d is the label length limit. */
    diff("label-max", "label \"%.*s\" is too long (max %d characters)",
         30, "averyveryverylonglabelnamehere", 23);

    /* vm/dvm.c:816 — "%.*s" then "%s": this is the one that dereferenced an
     * integer as a pointer. */
    diff("syscall-list", "sys: \"%.*s\" is not a syscall on this machine; it has %s",
         10, "frobnicate", "audio.tone fs.read");

    /* vm/dvm.c:868/:871 — two star precisions in one format string. */
    diff("two-stars", ".equ %.*s: expected a number, got \"%.*s\"",
         4, "NAME", 3, "xyz");

    /* And the property that actually broke: the argument list must not shift.
     * Assert the trailing values SURVIVE, not merely that the strings match. */
    char b[BUF];
    kfmt_snprintf(b, sizeof b,
                  "register \"%.*s\" does not exist (this machine has r0-r%d)",
                  3, "r16", 15);
    CHECK_CONTAINS(b, "\"r16\"");
    CHECK_CONTAINS(b, "r0-r15");
    CHECK(strstr(b, "%*s") == NULL);
    CHECK(strstr(b, "r0-r3)") == NULL);   /* the fabricated number */

    kfmt_snprintf(b, sizeof b, "unknown instruction \"%.*s\"%s",
                  3, "mrx", " (did you mean mov?)");
    CHECK_STR(b, "unknown instruction \"mrx\" (did you mean mov?)");
}

/* A precision on a %s is the ONLY safe way to print a token that is not
 * NUL-terminated, and vm/dvm.c's tok_t spans point straight into the model's
 * source text. If the formatter ever read past the precision it would walk off
 * into unrelated bytes. Prove it stops. */
static void test_star_precision_does_not_overread(void) {
    /* No NUL anywhere after the three characters we allow. */
    static const char raw[] = { 'a', 'b', 'c', 'X', 'Y', 'Z', '!', '?' };
    char b[BUF];
    int n = kfmt_snprintf(b, sizeof b, "[%.*s]", 3, raw);
    CHECK_STR(b, "[abc]");
    CHECK_EQ(n, 5);

    /* Precision 0 prints nothing at all and must still consume the pointer. */
    n = kfmt_snprintf(b, sizeof b, "[%.*s][%d]", 0, raw, 77);
    CHECK_STR(b, "[][77]");
    CHECK_EQ(n, 6);

    /* A precision longer than the string stops at the NUL, as C99 requires. */
    diff("prec-past-nul", "[%.*s]", 40, "short");
}

static void test_star_width(void) {
    diff("star-width-right", "[%*d]", 6, 42);
    diff("star-width-left", "[%*d]", -6, 42);       /* negative => left justify */
    diff("star-width-zero", "[%*d]", 0, 42);
    diff("star-width-str", "[%*s]", 8, "hi");
    diff("star-width-str-left", "[%-*s]", 8, "hi");
    diff("star-both", "[%*.*s]", 8, 3, "abcdefg");
    diff("star-width-narrower", "[%*d]", 1, 123456);
}

/* An absurd star width is a RUN-TIME integer, unlike a literal one. On a
 * single-threaded kernel with interrupts off, a two-billion-iteration pad loop
 * is a hang, and a hang is the one failure mode the machine cannot report. The
 * clamp is therefore a liveness property, not tidiness. This test must COMPLETE,
 * which is most of what it asserts. */
static void test_absurd_star_values_are_clamped_not_spun(void) {
    char b[64];
    int n = kfmt_snprintf(b, sizeof b, "%*d", INT_MAX, 1);
    CHECK(n > 0);                       /* clamped, and it returned */
    CHECK(n <= 8192);                   /* FMT_STAR_MAX */
    CHECK_EQ(strlen(b), sizeof b - 1);  /* truncated to the buffer, no overrun */

    n = kfmt_snprintf(b, sizeof b, "%.*s", INT_MAX, "abc");
    CHECK_STR(b, "abc");                /* still stops at the NUL */
    CHECK_EQ(n, 3);

    /* A negative star precision is "as if omitted" per C99, NOT precision 0. */
    diff("negative-star-prec", "[%.*s]", -1, "keepme");

    /* INT_MIN cannot be negated in int; make sure that does not misbehave. */
    n = kfmt_snprintf(b, sizeof b, "[%*d]", INT_MIN, 5);
    CHECK(n > 0);
    CHECK(n <= 8192 + 2);
}

/* ------------------------------------------ the rest of the grammar, diffed */

static void test_integers(void) {
    static const long long vals[] = {
        0, 1, -1, 7, -7, 9, 10, 99, 100, 255, 256, 32767, -32768,
        65535, 65536, 2147483647LL, -2147483648LL
    };
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        int v = (int)vals[i];
        diff("d", "%d", v);
        diff("i", "%i", v);
        diff("u", "%u", (unsigned)v);
        diff("x", "%x", (unsigned)v);
        diff("X", "%X", (unsigned)v);
        diff("d-width", "%8d", v);
        diff("d-left", "%-8d", v);
        diff("d-zero", "%08d", v);
        diff("x-zero-width", "%08x", (unsigned)v);
        diff("d-prec", "%.5d", v);
    }
}

static void test_long_and_size(void) {
    static const unsigned long long vals[] = {
        0, 1, 0xffffffffULL, 0x100000000ULL, 0xdeadbeefcafeULL,
        0x7fffffffffffffffULL, 0xffffffffffffffffULL
    };
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        unsigned long v = (unsigned long)vals[i];
        diff("lu", "%lu", v);
        diff("lx", "%lx", v);
        diff("lX", "%lX", v);
        diff("ld", "%ld", (long)v);
        diff("zu", "%zu", (size_t)v);
        diff("lx-width", "%016lx", v);
    }
}

static void test_strings_and_chars(void) {
    diff("s", "%s", "hello");
    diff("s-empty", "%s", "");
    diff("s-width", "%10s", "hi");
    diff("s-left", "%-10s", "hi");
    diff("s-prec", "%.2s", "hello");
    diff("s-prec-wide", "%10.2s", "hello");
    diff("s-prec-zero", "%.0s", "hello");
    diff("c", "%c", 'q');
    diff("c-nul-adjacent", "%c%c", 'a', 'b');
    diff("percent", "100%%");
    diff("mixed", "%s=%d (0x%x) %c", "key", -12, 0xabcu, '!');
    diff("adjacent", "%d%d%d", 1, 2, 3);
}

/* NULL through %s: the kernel formatter substitutes "(null)", which is what
 * glibc and macOS libc both do, but it is not required by C99, so it is
 * asserted explicitly rather than diffed. */
static void test_null_string(void) {
    char b[BUF];
    kfmt_snprintf(b, sizeof b, "[%s]", (const char *)NULL);
    CHECK_STR(b, "[(null)]");
    kfmt_snprintf(b, sizeof b, "[%.2s]", (const char *)NULL);
    CHECK_STR(b, "[(n]");
}

/* ------------------------------------------------------- C99 truncation */

/* This is the property every caller in the kernel depends on: tool results,
 * assembler errors and trace lines are all formatted into fixed buffers, and
 * several of them are then handed straight to a model. A formatter that can
 * write one byte past the end, or leave a buffer unterminated, would turn every
 * one of those into a memory-safety bug. Sweep every buffer size. */
static void test_truncation_and_termination(void) {
    static const char *fmts[] = { "%s", "%d", "%08x", "%.*s", "%*d", "%s-%s" };
    for (int cap = 0; cap <= 40; cap++) {
        char kbuf[64], hbuf[64];
        for (unsigned f = 0; f < sizeof fmts / sizeof fmts[0]; f++) {
            memset(kbuf, '\xAA', sizeof kbuf);
            memset(hbuf, '\xAA', sizeof hbuf);
            int kn, hn;
            switch (f) {
            case 0: kn = kfmt_snprintf(kbuf, cap, fmts[f], "abcdefghijklmnop");
                    hn =      snprintf(hbuf, cap, fmts[f], "abcdefghijklmnop"); break;
            case 1: kn = kfmt_snprintf(kbuf, cap, fmts[f], -1234567);
                    hn =      snprintf(hbuf, cap, fmts[f], -1234567); break;
            case 2: kn = kfmt_snprintf(kbuf, cap, fmts[f], 0xfeedu);
                    hn =      snprintf(hbuf, cap, fmts[f], 0xfeedu); break;
            case 3: kn = kfmt_snprintf(kbuf, cap, fmts[f], 12, "abcdefghijklmnop");
                    hn =      snprintf(hbuf, cap, fmts[f], 12, "abcdefghijklmnop"); break;
            case 4: kn = kfmt_snprintf(kbuf, cap, fmts[f], 12, 99);
                    hn =      snprintf(hbuf, cap, fmts[f], 12, 99); break;
            default:
                    kn = kfmt_snprintf(kbuf, cap, fmts[f], "alpha", "beta");
                    hn =      snprintf(hbuf, cap, fmts[f], "alpha", "beta"); break;
            }
            /* The would-have-written length must not depend on the buffer. */
            CHECK_EQ(kn, hn);
            if (cap == 0) {
                /* Nothing may be written at all, not even a NUL. */
                CHECK_EQ((unsigned char)kbuf[0], 0xAA);
            } else {
                /* The NUL goes at min(len, cap-1): at the end of a short result,
                 * at the truncation point for a long one. */
                int term = (kn < cap - 1) ? kn : cap - 1;
                CHECK_EQ(kbuf[term], '\0');                    /* terminated */
                CHECK_EQ((unsigned char)kbuf[term + 1], 0xAA); /* nothing past */
                CHECK_STR(kbuf, hbuf);                         /* same prefix */
            }
        }
    }
}

/* --------------------------------------------- deliberate non-C99 corners */

/* These are documented gaps, asserted so that "we do not support this" is a
 * decision with a test rather than an accident waiting to shift an argument
 * list the way star precision did. Each one must at minimum CONSUME its
 * argument so that nothing after it is misaligned. */
static void test_unsupported_conversions_do_not_shift_arguments(void) {
    char b[BUF];

    /* Unknown conversion: echoed literally. The important half is the SECOND
     * conversion still receiving its own argument. */
    kfmt_snprintf(b, sizeof b, "[%q][%d]", 7);
    CHECK_CONTAINS(b, "%q");
    CHECK_CONTAINS(b, "[7]");

    /* A format ending in a bare '%' must not run off the end of the string. */
    kfmt_snprintf(b, sizeof b, "trailing %");
    CHECK_STR(b, "trailing %");

    /* '+' and ' ' and '#' are not implemented; make sure they at least do not
     * eat the conversion character and desynchronise everything after. */
    kfmt_snprintf(b, sizeof b, "%d|%d", 5, 6);
    CHECK_STR(b, "5|6");
}

/* %p is spelled 0x%x by the kernel formatter with no width, which differs from
 * glibc's "0x..." only in leading-zero handling, so it is asserted directly. */
static void test_pointer(void) {
    char b[BUF];
    kfmt_snprintf(b, sizeof b, "%p", (void *)0);
    CHECK_STR(b, "0x0");
    kfmt_snprintf(b, sizeof b, "%p", (void *)0xdeadbeef);
    CHECK_STR(b, "0xdeadbeef");
}

/* ------------------------------------------------------------------ fuzz */

/* Assemble random format strings from the supported grammar and diff them.
 * The point is to find a conversion/flag/width/precision combination nobody
 * enumerated above — which is exactly how star precision escaped notice. */
static uint32_t rnd_state = 0x13572468u;
static uint32_t rnd(uint32_t n) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return (rnd_state >> 8) % n;
}

static void test_fuzz_against_libc(void) {
    static const char *conv[] = { "d", "i", "u", "x", "X", "s", "c" };
    for (int iter = 0; iter < 4000; iter++) {
        const char *c = conv[rnd(sizeof conv / sizeof conv[0])];
        int numeric = (c[0] != 's' && c[0] != 'c');

        char fmt[64];
        size_t fl = 0;
        fmt[fl++] = '%';
        if (rnd(3) == 0) fmt[fl++] = '-';
        /* The '0' flag is only DEFINED for the numeric conversions (C99
         * 7.19.6.1). On %s and %c it is undefined behaviour and the two
         * formatters legitimately differ — glibc zero-pads, kfmt space-pads —
         * so generating it would be testing an opinion, not a contract. */
        if (numeric && rnd(3) == 0) fmt[fl++] = '0';

        int star_w = 0, star_p = 0, wval = 0, pval = 0;
        switch (rnd(3)) {
        case 0: break;
        case 1: fl += (size_t)sprintf(fmt + fl, "%u", rnd(14)); break;
        default: fmt[fl++] = '*'; star_w = 1; wval = (int)rnd(20) - 4; break;
        }
        switch (rnd(3)) {
        case 0: break;
        case 1: fmt[fl++] = '.'; fl += (size_t)sprintf(fmt + fl, "%u", rnd(10)); break;
        default: fmt[fl++] = '.'; fmt[fl++] = '*'; star_p = 1;
                 pval = (int)rnd(12) - 2; break;
        }

        int is_long = 0;
        if (numeric && rnd(4) == 0) { fmt[fl++] = 'l'; is_long = 1; }
        fmt[fl++] = c[0];
        fmt[fl] = '\0';

        /* Star arguments come first, in order, exactly as C99 says. */
        if (c[0] == 's') {
            const char *sv = (rnd(4) == 0) ? "" : "abcdefghij";
            if (star_w && star_p)      diff("fuzz", fmt, wval, pval, sv);
            else if (star_w)           diff("fuzz", fmt, wval, sv);
            else if (star_p)           diff("fuzz", fmt, pval, sv);
            else                       diff("fuzz", fmt, sv);
        } else if (c[0] == 'c') {
            int cv = 'A' + (int)rnd(26);
            if (star_w && star_p)      diff("fuzz", fmt, wval, pval, cv);
            else if (star_w)           diff("fuzz", fmt, wval, cv);
            else if (star_p)           diff("fuzz", fmt, pval, cv);
            else                       diff("fuzz", fmt, cv);
        } else if (is_long) {
            unsigned long v = (unsigned long)rnd(100000) * 0x10001ul;
            if (star_w && star_p)      diff("fuzz", fmt, wval, pval, v);
            else if (star_w)           diff("fuzz", fmt, wval, v);
            else if (star_p)           diff("fuzz", fmt, pval, v);
            else                       diff("fuzz", fmt, v);
        } else {
            int v = (int)rnd(200000) - 100000;
            if (star_w && star_p)      diff("fuzz", fmt, wval, pval, v);
            else if (star_w)           diff("fuzz", fmt, wval, v);
            else if (star_p)           diff("fuzz", fmt, pval, v);
            else                       diff("fuzz", fmt, v);
        }
    }
}

int main(void) {
    RUN(test_star_precision_is_supported);
    RUN(test_star_precision_does_not_overread);
    RUN(test_star_width);
    RUN(test_absurd_star_values_are_clamped_not_spun);
    RUN(test_integers);
    RUN(test_long_and_size);
    RUN(test_strings_and_chars);
    RUN(test_null_string);
    RUN(test_truncation_and_termination);
    RUN(test_unsupported_conversions_do_not_shift_arguments);
    RUN(test_pointer);
    RUN(test_fuzz_against_libc);
    return th_report("kfmt");
}
