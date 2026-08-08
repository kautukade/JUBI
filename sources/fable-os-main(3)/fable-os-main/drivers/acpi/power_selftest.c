/* power_selftest.c — drive a power tool from the boot path, on purpose.
 *
 * ============================================================================
 * THIS FILE EXISTS ONLY TO END THE MACHINE DURING BOOT. IT IS COMPILED OUT OF
 * EVERY NORMAL BUILD AND NOTHING IN THE BOOT PATH CAN REACH IT WITHOUT THE FLAG.
 * ============================================================================
 *
 * WHY IT HAS TO EXIST
 *   A shutdown path that has never shut anything down is a hypothesis, and this
 *   one cannot be proven the usual way. The host tests assert on the plan and
 *   the parsers; tests/qemu asserts on serial output — but a power-off produces
 *   no output at all after the fact, and the automated runs are keyless on
 *   purpose (the API key never enters the build, see the Makefile), so no model
 *   can reach a tool call in one. Without this file the only
 *   way to find out whether writing SLP_TYP|SLP_EN to a real PM1a_CNT actually
 *   turns a real machine off would be to type at the prompt by hand.
 *
 *   So the flag exists, it goes through the REAL model-facing path, and QEMU's
 *   own exit status is the assertion:
 *
 *     make EXTRA_CFLAGS=-DFABLEOS_POWER_TEST=power_off
 *     qemu-system-x86_64 -kernel kernel.bin -display none -no-reboot \
 *                        -serial stdio ; echo $?
 *
 *   QEMU exiting on its own is proof of a genuine power-off: a hang would sit
 *   there until the timeout, and a triple fault would print the boot banner a
 *   second time instead. -DFABLEOS_POWER_TEST=power_reboot proves the other one,
 *   by producing exactly that second banner in one serial log.
 *
 * HOW IT IS KEPT OUT OF REACH
 *   Mirrors arch/x86_64/fault_inject.c, which makes the same argument for
 *   deliberate faults:
 *     1. Everything here is inside #ifdef FABLEOS_POWER_TEST. Without the flag
 *        this file compiles to nothing at all — no driver is registered, so the
 *        driver manager never has anything to call.
 *     2. The flag's value is a tool name and nothing else, so the file cannot do
 *        anything the model could not already do at the prompt.
 *     3. It announces itself first, so a log can never be mistaken for a real
 *        session or an accidental shutdown.
 *
 * WHY IT GOES THROUGH tool_dispatch()
 *   Calling acpi_power_off() directly would prove the ACPI write and nothing
 *   else. Dispatching the tool by name exercises the whole chain the model uses:
 *   the registry lookup, the JSON argument parse, the "confirm" guard, the
 *   result buffer, and — the part that matters most — the ordering guarantee
 *   that the kernel's trace line reaches the console BEFORE the machine goes
 *   down. That ordering is not visible from a host test; it is only visible in a
 *   serial log that ends in the right place.
 */

#ifdef FABLEOS_POWER_TEST

#include "acpi.h"
#include "tool.h"
#include "trace.h"
#include "kernel.h"
#include "driver.h"
#include "device.h"

/* -DFABLEOS_POWER_TEST=power_off  ->  "power_off" */
#define FABLEOS_STR2(x) #x
#define FABLEOS_STR(x)  FABLEOS_STR2(x)

static int power_selftest_init(void) {
    static const char args[] =
        "{\"confirm\":true,\"reason\":\"FABLEOS_POWER_TEST boot self-test\"}";
    const char *name = FABLEOS_STR(FABLEOS_POWER_TEST);

    size_t nargs = 0;
    while (args[nargs]) nargs++;

    kputs("\n--- power self-test (FABLEOS_POWER_TEST) ---\n");
    kprintf("power-selftest: dispatching the tool \"%s\" exactly as the model "
            "would\n", name);
    if (!tool_find(name)) {
        kprintf("power-selftest: no tool named \"%s\" is registered - nothing "
                "to do\n", name);
        return 0;
    }

    char          buf[TOOL_RESULT_MAX];
    tool_result_t res;
    tool_result_init(&res, buf, sizeof buf);

    tool_call_t call = {
        .id        = "toolu_power_selftest",
        .name      = name,
        .input     = args,
        .input_len = nargs,
    };

    uint64_t before = trace_emissions();
    int      rc     = tool_dispatch(&call, &res);

    /* Only reachable when the tool did NOT end the machine. */
    kprintf("power-selftest: still running. dispatch rc=%d is_error=%d "
            "trace lines emitted=%d\n", rc, res.is_error,
            (int)(trace_emissions() - before));
    kputs("power-selftest: the result the model would have received:\n");
    kputs(buf);
    kputs("\n--- end of power self-test ---\n");
    return 0;
}

/* LATE, so every other driver is up and drivers_suspend_all() has real work to
 * do — the clean shutdown path is part of what is being proven. */
static const driver_t power_selftest_driver = {
    .name  = "powertest",
    .level = DRV_LEVEL_LATE,
    .cls   = DEV_CLASS_NONE,
    .init  = power_selftest_init,
};
REGISTER_DRIVER(power_selftest_driver);

#else

/* Keeps this translation unit non-empty in a normal build, where the whole file
 * above compiles to nothing. */
typedef int fableos_power_selftest_not_built_t;

#endif /* FABLEOS_POWER_TEST */
