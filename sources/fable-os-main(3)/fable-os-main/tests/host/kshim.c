/* kshim.c — host implementations of the kernel services in kernel.h.
 *
 * This is what lets real kernel source files (mm/heap.c, and the JSON/trace/VM
 * modules to come) link and run in a normal host process. Console output is
 * captured into a buffer instead of a VGA framebuffer, and panic() is recorded
 * rather than halting, so a test can assert that bad input panics.
 *
 * memcpy/memset/memmove/memcmp/strlen are intentionally NOT defined here — the
 * host libc already provides them, and kernel.h's declarations are compatible.
 */

#include "harness.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- captured console ---- */

#define KCAP_CAP (1 << 20)
static char   kcap_buf[KCAP_CAP];
static size_t kcap_len = 0;

/* Set once the 1 MiB capture is full. Without it a high-volume suite's console
 * assertions would quietly start matching nothing: the text they are looking for
 * was printed, and then dropped on the floor. th_report() prints this. */
int kcap_overflowed = 0;

/* The captured console also tracks a cursor, because net/chat.c asks the console
 * where the cursor IS before it prints a '[' (see print_model_prose there). A
 * shim that always answered "column 0" would make that guard untestable, and a
 * shim that always answered "column 7" would make it vacuous. So these mirror
 * lib/base.c kputc() exactly: \n advances a row and resets the column, \r
 * resets it, \b steps left and can walk up a row, \t rounds up to a multiple of
 * 8, and printing in the last column wraps. 25x80 matches the real VGA console.
 * If lib/base.c's rules ever change, this has to change with it. */
#define KCAP_ROWS 25
#define KCAP_COLS 80
static int kcap_row = 0, kcap_col = 0;

void kcap_reset(void) {
    kcap_len = 0;
    kcap_buf[0] = '\0';
    kcap_row = kcap_col = 0;
    kcap_overflowed = 0;
}

const char *kcap_text(void) {
    kcap_buf[kcap_len] = '\0';
    return kcap_buf;
}

static void kcap_advance(char c) {
    if (c == '\n') {
        kcap_col = 0;
        if (++kcap_row >= KCAP_ROWS) kcap_row = KCAP_ROWS - 1;   /* scrolls */
        return;
    }
    if (c == '\r') { kcap_col = 0; return; }
    if (c == '\b') {
        if (kcap_col > 0) kcap_col--;
        else if (kcap_row > 0) { kcap_row--; kcap_col = KCAP_COLS - 1; }
        return;
    }
    if (c == '\t') kcap_col = (kcap_col + 8) & ~7;
    else           kcap_col++;
    if (kcap_col >= KCAP_COLS) {
        kcap_col = 0;
        if (++kcap_row >= KCAP_ROWS) kcap_row = KCAP_ROWS - 1;
    }
}

static void kcap_push(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) kcap_advance(s[i]);
    if (kcap_len + n >= KCAP_CAP) {
        n = KCAP_CAP - kcap_len - 1;
        kcap_overflowed = 1;
    }
    memcpy(kcap_buf + kcap_len, s, n);
    kcap_len += n;
}

/* ---- kernel.h console surface ---- */

void console_init(void) { kcap_reset(); }

void console_geometry(int *rows, int *cols) {
    if (rows) *rows = KCAP_ROWS;
    if (cols) *cols = KCAP_COLS;
}

void console_cursor(int *row, int *col) {
    if (row) *row = kcap_row;
    if (col) *col = kcap_col;
}

void kputc(char c) { kcap_push(&c, 1); }

void kputs(const char *s) { kcap_push(s, strlen(s)); }

/* ---- kprintf, and the grammar it is allowed to use ----
 *
 * The kernel's kprintf (lib/base.c) understands ONLY %s %c %d %u %x %p %% with
 * an optional 0 and a literal width. On anything else it prints '%' plus that
 * character and CONSUMES NO VARARG, so every remaining argument on the line is
 * read from the wrong slot and a later %s dereferences a non-pointer.
 *
 * Rendering here with the host's vsnprintf — a strict superset — is what let a
 * `%lu` ship to real hardware while this suite stayed green: the host printed
 * "walking 3 blocks", the machine printed "walking %lu blocks". So the format is
 * CHECKED against the kernel's grammar first, and a violation is a test failure
 * with the offending format named. Rendering still goes through vsnprintf,
 * because once the grammar is honoured the two agree, and an emulation of
 * lib/base.c's formatter would just be a second thing to keep in step.
 *
 * If lib/base.c's grammar grows, this list grows with it. */
int kfmt_violations = 0;

static void kfmt_fail(const char *fmt, const char *why) {
    kfmt_violations++;
    th_fails++;
    if (kfmt_violations <= 5)      /* one bad format usually repeats; say it once */
        printf("    FAIL kprintf format %s: \"%s\"\n", why, fmt);
}

static void kfmt_check(const char *fmt) {
    if (!fmt) return;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        if (*p == '0') p++;                             /* zero pad  */
        while (*p >= '0' && *p <= '9') p++;             /* width     */
        if (!*p) {
            kfmt_fail(fmt, "ends inside a conversion (a trailing bare '%')");
            return;
        }
        if (!strchr("scduxp", *p)) {
            kfmt_fail(fmt, "uses a conversion lib/base.c does not implement "
                           "(it would print the two characters and consume no "
                           "argument, shifting every later one)");
            return;
        }
    }
}

void kprintf(const char *fmt, ...) {
    char    tmp[8192];
    va_list ap;
    kfmt_check(fmt);
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) kcap_push(tmp, (size_t)n > sizeof tmp - 1 ? sizeof tmp - 1 : (size_t)n);
}

/* ---- panic: record, don't die ---- */

int         kpanic_hit = 0;
const char *kpanic_msg = NULL;

void panic(const char *msg) {
    kpanic_hit = 1;
    kpanic_msg = msg;
    printf("    [panic captured: %s]\n", msg ? msg : "(null)");
}

/* ---- time ---- */

void time_init(void) {}

uint64_t millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void mdelay(uint32_t ms) { (void)ms; }
