/* kfmt.c — the kernel's own vsnprintf/snprintf.
 *
 * PURPOSE
 *   One bounded, freestanding string formatter, used by everything in the
 *   kernel that builds a message: mbedTLS and lwIP call snprintf directly, and
 *   every model-facing error in the tree is assembled by some wrapper that ends
 *   up here (vm/dvm.c's aerr() and sb_addf(), core/tool.c's
 *   tool_result_printf(), and so on). Note that lib/base.c's kprintf() is a
 *   SEPARATE, even smaller formatter that writes straight to the console and
 *   does NOT come through here — it supports fewer conversions, so a format
 *   string that works in one is not automatically valid in the other.
 *
 * WHY IT IS ITS OWN FILE
 *   It used to live in lib/libc_shim.c, which cannot be compiled for a host
 *   test: that file pulls in kernel.h and io.h and calls kmalloc, millis and
 *   RDRAND. So the kernel's formatter was the one piece of string handling in
 *   the tree with no test at all, while every host suite silently exercised
 *   the host libc's formatter instead. That gap hid a real defect for a long
 *   time (see the star width/precision note below), and it hid it in exactly
 *   the text a model reads to repair its own programs. This file depends on
 *   nothing but <stdint.h>, <stddef.h> and <stdarg.h>, so tests/host/test_kfmt.c
 *   links the REAL kernel formatter and compares it against the host libc
 *   conversion by conversion.
 *
 * RESPONSIBILITIES
 *   - vsnprintf/snprintf with C99 truncation semantics: never write more than
 *     `size` bytes including the NUL, always NUL-terminate a non-zero buffer,
 *     and return the length the output WOULD have had.
 *   - Refuse to spin: every count that drives a loop is clamped, so no format
 *     string and no argument can turn a message into a hang on a machine with
 *     no scheduler and no interrupts.
 *
 * PUBLIC API
 *   int vsnprintf(char *, size_t, const char *, va_list);
 *   int snprintf (char *, size_t, const char *, ...);
 *   Declared for callers by port/stdio.h, as libc headers would.
 *
 * DEPENDENCIES
 *   <stdint.h>, <stddef.h>, <stdarg.h>. Nothing else, deliberately — that is
 *   what makes it testable.
 *
 * FUTURE EXTENSION POINTS
 *   - Floating point (%f/%e/%g) is absent because nothing in the kernel has a
 *     use for it and the SSE state to support it is not saved anywhere.
 *   - '+' and ' ' sign flags and '#' are absent for the same reason: no caller.
 *     Each would be one more arm in the flag loop.
 *   - If lib/base.c's kprintf ever needs the full conversion set, it should
 *     call vsnprintf into a bounded stack buffer rather than grow a second
 *     implementation of the same grammar.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* A host test cannot define a second `vsnprintf` alongside the platform libc's
 * — the declarations collide and the linker would pick one arbitrarily. Under
 * FABLEOS_HOSTTEST the kernel formatter therefore takes a private name, so a
 * suite can link BOTH and print the same format string through each. The kernel
 * build is untouched by this: there the symbols are plain vsnprintf/snprintf,
 * which is what mbedTLS and lwIP link against. */
#ifdef FABLEOS_HOSTTEST
#define vsnprintf kfmt_vsnprintf
#define snprintf  kfmt_snprintf
#endif

/* ====================================================================== */
/* snprintf / vsnprintf — small, bounded formatter for mbedTLS             */
/* Supports: flags '-' '0', width and precision (literal or '*'), length   */
/* l/ll/z, and the conversions d i u x X p c s %. Returns the C99          */
/* "would-have-written" length.                                            */
/*                                                                         */
/* STAR WIDTH AND PRECISION ARE SUPPORTED, and that is load-bearing. They  */
/* were not, until a live model was handed the assembler error             */
/*     unknown instruction "%*s"                                           */
/* instead of the name of the instruction it had got wrong. The old parser  */
/* accepted only digits after '%' and '.', so '*' fell through to the       */
/* `default` arm, which printed the two characters literally AND CONSUMED   */
/* NO VARARG. Every argument after that point shifted by one slot, so:      */
/*   - "%.*s" followed by "%s" made the next %s dereference the token       */
/*     LENGTH as a pointer. Low memory is identity-mapped present+writable  */
/*     from physical 0 (boot/boot.asm), so this did not fault — it read the  */
/*     real-mode IVT and pushed raw bytes like 0xf0 into a model-facing      */
/*     error string, and from there into the JSON request body, which       */
/*     net/json.c passes through unescaped above 0x1f. Invalid UTF-8 on the  */
/*     wire, and the actual token and "did you mean" hint silently dropped. */
/*   - "%.*s" followed by "%d" made the %d print the token length, so       */
/*     vm/dvm.c told the model `register "%*s" does not exist (this machine  */
/*     has r0-r3)` on a machine with r0-r15, and `label too long (max 3      */
/*     characters)` where the real maximum is 23. The kernel stated a       */
/*     fabricated hard number as fact in the one message a model reads to    */
/*     repair its own program.                                              */
/* 27 call sites in vm/dvm.c had this shape (tests/qemu/lint_printf.py       */
/* lists them). Fixing the parser fixes all of them at once and, more        */
/* importantly, stops the next one being written: host test suites link the  */
/* real libc, where these format strings have always worked perfectly, so no */
/* test in this tree could ever see the divergence. The two formatters now   */
/* agree, which is what makes a green host suite evidence about the kernel.  */
/* ====================================================================== */

/* Widths and precisions are spun over one character at a time by sb_pad and
 * emit_uint, so an absurd one is a busy loop, and on this machine a busy loop
 * is a hang: single-threaded, IF clear, everything polled. A literal width is
 * a compile-time constant and was always trustworthy, but a '*' width is a
 * RUN-TIME integer, so it has to be bounded. 8192 is past every buffer any
 * caller in this tree formats into, so no legitimate conversion is affected. */
#define FMT_STAR_MAX 8192

struct sbuf { char *p; size_t cap; size_t len; };

static void sb_putc(struct sbuf *s, char c) {
    if (s->len + 1 < s->cap) s->p[s->len] = c;
    s->len++;
}

static void sb_pad(struct sbuf *s, int n, char pad) {
    while (n-- > 0) sb_putc(s, pad);
}

/* One integer conversion, sign included.
 *
 * The sign is emitted HERE rather than by the caller, and that is the fix for a
 * defect the differential test in tests/host/test_kfmt.c found the moment it
 * existed. The caller used to print '-' itself and then ask for a width one
 * smaller, which puts the sign on the wrong side of space padding:
 *     "%10d" of -10320  ->  "-    10320"     (was)
 *                       ->  "    -10320"     (C99, and what libc prints)
 * Every negative number the kernel ever printed in a fixed-width column came
 * out like the first line. The two paddings genuinely differ — space padding
 * goes BEFORE the sign, zero padding goes AFTER it — so the sign and the pad
 * have to be decided together. */
static void emit_int(struct sbuf *s, uint64_t v, int neg, int base, int upper,
                     int width, int prec, int left, int zero) {
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    /* `prec` comes from the format string and is accumulated unbounded by the
     * parser, so it must be clamped before it is used as a write count: "%.40d"
     * would otherwise run the zero-fill below straight off the end of tmp and
     * corrupt the caller's stack frame. 32 is past every representable value
     * (20 digits for a base-10 uint64, 16 for base-16), so no legitimate
     * conversion is affected by the clamp. */
    if (prec > (int)sizeof tmp) prec = (int)sizeof tmp;
    if (v == 0 && prec != 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    while (n < prec) tmp[n++] = '0';                 /* precision = min digits */

    int pad = width - n - (neg ? 1 : 0);
    /* C99 7.19.6.1: "For d, i, o, u, x, and X conversions, if a precision is
     * specified, the 0 flag is ignored." The kernel used to honour both at once
     * and print "%09.5lX" of 0x518A518A as "0518A518A" where libc gives
     * " 518A518A". */
    int zpad = zero && !left && prec < 0;

    if (!left && !zpad) sb_pad(s, pad, ' ');   /* spaces come before the sign */
    if (neg) sb_putc(s, '-');
    if (zpad) sb_pad(s, pad, '0');             /* zeros come after it */
    while (n--) sb_putc(s, tmp[n]);
    if (left) sb_pad(s, pad, ' ');
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    struct sbuf s = { str, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { sb_putc(&s, *fmt); continue; }
        fmt++;
        /* A format ending in a bare '%' leaves fmt on the terminator. Without
         * this the switch below falls to `default`, the loop's own fmt++ steps
         * PAST the NUL, and formatting continues into whatever .rodata follows —
         * consuming varargs that were never passed. */
        if (!*fmt) { sb_putc(&s, '%'); break; }
        int left = 0, zero = 0;
        for (;; fmt++) {                              /* flags */
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        int width = 0;
        if (*fmt == '*') {
            /* C99: a negative star width means left-justify with its absolute
             * value, exactly as if '-' had been given.
             *
             * The magnitude is taken in UNSIGNED arithmetic. `-w` on INT_MIN is
             * signed overflow, i.e. undefined behaviour, and it does not stay
             * theoretical: written that way this line produced a negative width
             * that survived the clamp, and whether the result was harmless
             * depended on the optimisation level. The suite caught it by passing
             * INT_MIN deliberately. */
            fmt++;
            int w = va_arg(ap, int);
            unsigned uw;
            if (w < 0) { left = 1; uw = 0u - (unsigned)w; } else uw = (unsigned)w;
            width = (uw > FMT_STAR_MAX) ? FMT_STAR_MAX : (int)uw;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                /* C99: a negative star precision is as if it were omitted. */
                fmt++;
                int p = va_arg(ap, int);
                prec = (p < 0) ? -1 : (p > FMT_STAR_MAX ? FMT_STAR_MAX : p);
            } else {
                prec = 0;
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 2; fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = (lng >= 1) ? va_arg(ap, long) : va_arg(ap, int);
            /* Negating INT64_MIN is undefined in signed arithmetic, so the
             * magnitude is taken in unsigned, where it is exact and defined. */
            uint64_t mag = (v < 0) ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            emit_int(&s, mag, v < 0, 10, 0, width, prec, left, zero);
            break;
        }
        case 'u': emit_int(&s, (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 0, 10, 0, width, prec, left, zero); break;
        case 'x': emit_int(&s, (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 0, 16, 0, width, prec, left, zero); break;
        case 'X': emit_int(&s, (lng >= 1) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 0, 16, 1, width, prec, left, zero); break;
        case 'p': sb_putc(&s, '0'); sb_putc(&s, 'x'); emit_int(&s, (uint64_t)(uintptr_t)va_arg(ap, void *), 0, 16, 0, 0, -1, 0, 0); break;
        case 'c': {
            /* %c honours width like every other conversion. It used to ignore
             * it completely, so "%3c" printed one character and reported a
             * length of 1 — a silent column-alignment bug anywhere a caller
             * padded a single character. */
            char cv = (char)va_arg(ap, int);
            if (!left) sb_pad(&s, width - 1, ' ');
            sb_putc(&s, cv);
            if (left) sb_pad(&s, width - 1, ' ');
            break;
        }
        case 's': {
            const char *str2 = va_arg(ap, const char *);
            if (!str2) str2 = "(null)";
            int n = 0; while (str2[n] && (prec < 0 || n < prec)) n++;
            /* The '0' flag on %s is undefined behaviour in C99 (7.19.6.1 limits
             * it to the numeric conversions). glibc happens to zero-pad; this
             * formatter space-pads. Neither is wrong, so test_kfmt.c excludes
             * "%0Ns" from its differential rather than either side being
             * "fixed" to match the other. */
            if (!left) sb_pad(&s, width - n, ' ');
            for (int i = 0; i < n; i++) sb_putc(&s, str2[i]);
            if (left) sb_pad(&s, width - n, ' ');
            break;
        }
        case '%': sb_putc(&s, '%'); break;
        default: sb_putc(&s, '%'); if (*fmt) sb_putc(&s, *fmt); break;
        }
    }
    if (s.cap) s.p[s.len < s.cap ? s.len : s.cap - 1] = '\0';
    return (int)s.len;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return n;
}
