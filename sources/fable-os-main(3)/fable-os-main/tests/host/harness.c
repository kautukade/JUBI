/* harness.c — counters and the pass/fail report for the host test harness. */

#include "harness.h"

int th_checks = 0;
int th_fails  = 0;

int th_report(const char *suite) {
    /* Two things the shim can only report at the end. Both are silent
     * corruptions of the evidence a suite reasons about, so they are named here
     * rather than left for a reader to notice a missing line. */
    if (kcap_overflowed)
        printf("    NOTE %s: the captured console filled up (1 MiB); later "
               "output was dropped, so console assertions after that point "
               "prove nothing.\n", suite);
    if (kfmt_violations > 5)
        printf("    NOTE %s: %d kprintf grammar violations in total (only the "
               "first 5 are listed).\n", suite, kfmt_violations);

    if (th_fails == 0) {
        printf("  PASS %s (%d checks)\n", suite, th_checks);
        return 0;
    }
    printf("  FAIL %s (%d/%d checks failed)\n", suite, th_fails, th_checks);
    return 1;
}
