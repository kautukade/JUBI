/* harness.h — minimal host-native unit test harness for fable-os.
 *
 * Kernel modules that are pure logic (heap, JSON parsing, trace formatting, the
 * driver VM) are compiled against the HOST compiler and run natively, so the
 * inner development loop is milliseconds instead of a QEMU boot. Hardware-
 * dependent behaviour belongs in tests/qemu/ instead.
 *
 * Usage:
 *     #include "harness.h"
 *     static void test_thing(void) { CHECK(1 + 1 == 2); }
 *     int main(void) { RUN(test_thing); return th_report("thing"); }
 *
 * Exit code is 0 when every check passed, 1 otherwise, so `make test-host`
 * fails loudly in CI and for any agent iterating on a change.
 */
#ifndef HARNESS_H
#define HARNESS_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern int th_checks;
extern int th_fails;

/* Report a failure without aborting: one run surfaces every broken check. */
#define CHECK(cond)                                                           \
    do {                                                                      \
        th_checks++;                                                          \
        if (!(cond)) {                                                        \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

#define CHECK_EQ(got, want)                                                   \
    do {                                                                      \
        th_checks++;                                                          \
        long long g_ = (long long)(got), w_ = (long long)(want);               \
        if (g_ != w_) {                                                       \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  %s == %s (got %lld, want %lld)\n",         \
                   __FILE__, __LINE__, #got, #want, g_, w_);                  \
        }                                                                     \
    } while (0)

#define CHECK_STR(got, want)                                                  \
    do {                                                                      \
        th_checks++;                                                          \
        const char *g_ = (got), *w_ = (want);                                 \
        if (!g_ || !w_ || strcmp(g_, w_) != 0) {                              \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  %s == \"%s\" (got \"%s\")\n",             \
                   __FILE__, __LINE__, #got, w_ ? w_ : "(null)",              \
                   g_ ? g_ : "(null)");                                       \
        }                                                                     \
    } while (0)

/* Assert a substring appears — the workhorse for trace lines and JSON. */
#define CHECK_CONTAINS(haystack, needle)                                      \
    do {                                                                      \
        th_checks++;                                                          \
        const char *h_ = (haystack), *n_ = (needle);                          \
        if (!h_ || !n_ || strstr(h_, n_) == NULL) {                           \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  %s contains \"%s\"\n  in: %s\n",          \
                   __FILE__, __LINE__, #haystack, n_ ? n_ : "(null)",         \
                   h_ ? h_ : "(null)");                                       \
        }                                                                     \
    } while (0)

/* Assert a registered tool's flag bits — `mask` selects them, `want` is what
 * they must be (use ~0u to test the whole word).
 *
 * Every suite used to write `tool_find("x")->flags & TOOL_MUTATES` directly, and
 * tool_find() returns NULL for a tool that failed validation — a one-character
 * typo in an input_schema is enough. The deref then SIGSEGVs the suite: no FAIL
 * line, no file, no line number, and `make test-host` dying on a signal with
 * nothing for the reader (or the agent loop) to go on. Only expand this in a TU
 * that includes tool.h. */
#define CHECK_TOOL_FLAGS(name, mask, want)                                    \
    do {                                                                      \
        const tool_t *t_ = tool_find(name);                                   \
        th_checks++;                                                          \
        if (!t_) {                                                            \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  tool_find(\"%s\") is NULL — not "         \
                   "registered, or its descriptor failed validation\n",       \
                   __FILE__, __LINE__, (name));                               \
        } else if ((unsigned)(t_->flags & (unsigned)(mask)) !=                \
                   (unsigned)(want)) {                                        \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  %s flags & %s == %s (got 0x%x)\n",        \
                   __FILE__, __LINE__, (name), #mask, #want,                  \
                   (unsigned)(t_->flags & (unsigned)(mask)));                 \
        }                                                                     \
    } while (0)

#define RUN(fn)                                                               \
    do {                                                                      \
        printf("  - %s\n", #fn);                                              \
        fn();                                                                 \
    } while (0)

int th_report(const char *suite);

/* Provided by kshim.c: the kernel.h console surface, host-side. Declared here so
 * a test only needs harness.h plus the header of the module under test. */
void console_init(void);
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);

/* ---- host-side capture of kernel console output ----
 * kshim.c routes kprintf/kputs/kputc into a buffer so tests can assert on what
 * the kernel *printed* — which is exactly how kernel-emitted trace lines get
 * verified without booting. */
void        kcap_reset(void);
const char *kcap_text(void);

/* Set when the code under test calls panic(); lets a test assert that a bad
 * input panics without killing the test process.
 *
 * MIND THE DIVERGENCE: the real panic() (lib/base.c) never returns — it is
 * `for(;;) { cli; hlt; }`. The shim records and RETURNS, so a test keeps
 * executing the code that follows a panic, with the state that caused it. That
 * is what makes "prove bad input panics" testable at all, but it means a green
 * check after a captured panic is not evidence about the machine: on hardware
 * nothing after that call ever runs. Assert kpanic_hit and stop. */
extern int         kpanic_hit;
extern const char *kpanic_msg;

/* Set by kshim.c when the 1 MiB console capture overflowed, and counted when a
 * kprintf format used a conversion lib/base.c cannot render (see kshim.c). Both
 * are reported by th_report(); a format violation is also a failed check. */
extern int kcap_overflowed;
extern int kfmt_violations;

#endif /* HARNESS_H */
