/* power.c — the half that actually ends the machine.
 *
 * drivers/acpi/acpi.c found the tables and turned them into a plan. This file
 * runs it: the clean shutdown path, the legacy-to-ACPI mode switch, the ordered
 * attempts, and the driver registration that gets acpi_init() called at boot.
 * See include/acpi.h for the contract.
 *
 * ============================================================================
 * A MACHINE THAT VANISHES WITHOUT A WORD IS A BUG, NOT A FEATURE
 * ============================================================================
 *   There is no shell here, so the serial log is the entire audit trail. If the
 *   machine simply stops mid-transcript, the operator cannot tell a deliberate
 *   shutdown from a triple fault, a hang, or QEMU being killed — and those have
 *   completely different causes. So a power transition is loud, in this order:
 *
 *     [power_off stage=commit ... -> ok]        the kernel's trace line, emitted
 *                                              by tools/power_tools.c BEFORE any
 *                                              of the below (trace.h)
 *     power: shutdown requested: <reason>       what and why
 *     power: filesystem is RAM-backed ...       what is being lost
 *     power: bringing drivers down ...          the existing lifecycle
 *     e1000: suspend: ...                       the drivers' own accounts
 *     power: drivers suspended. ... going down  the last promise
 *     power: attempt 1/2 acpi-pm1a: io 0x604<-0x2000/16
 *                                              the exact write, named, before
 *                                              it is issued
 *     <silence>                                 the machine is off
 *
 *   The "attempt" line is the load-bearing one, and it is why each failed step
 *   is followed by "power: <method> did not take effect - still running". A log
 *   ending in "attempt 1/2 acpi-pm1a" with no such line after it says the ACPI
 *   path did the work; a log with two attempts says the fallback did. Without
 *   that, "we powered off" and "the QEMU port happened to be right" look
 *   identical, which would make the whole ACPI implementation unfalsifiable.
 *
 * FLUSHING
 *   The console is a polled 16550 (drivers/serial/serial.c): serial_putc waits
 *   for the holding register to empty BEFORE writing, so when kputs returns, up
 *   to one character may still be in the transmitter. Powering off in that
 *   window truncates the last line — usually the most important one. So the UART
 *   is drained (THRE and TEMT both set) before the plan runs and after every
 *   attempt line. It is read through the platform backend rather than
 *   drivers/serial/, because this file must stay host-compilable and because a
 *   test needs to be able to make the drain terminate.
 *
 *   Nothing else needs flushing and it is worth saying why rather than leaving
 *   it silent: the only filesystem is ramfs (fs/native/ramfs.c), which has no
 *   backing store, so there is nothing to sync and everything written this boot
 *   is lost by design. When a block device exists, its writeback belongs here,
 *   in prepare_default(), before drivers_suspend_all().
 */

#include "acpi.h"

#include <stdio.h>          /* snprintf */

#ifdef FABLEOS_HOSTTEST
void kputs(const char *s);
void kprintf(const char *fmt, ...);
#else
#  include "kernel.h"
#  include "driver.h"
#  include "device.h"
#endif

/* COM1's line-status register, and the two bits that together mean "the
 * transmitter is completely idle": THRE (holding register empty) and TEMT
 * (shift register empty too). Duplicating the port number rather than pulling
 * in drivers/serial/ keeps this file compilable on the host; it is the same
 * 0x3F8 base the serial driver announces at boot. */
#define UART_COM1_LSR  0x3FD
#define UART_LSR_IDLE  0x60

/* PM1_CNT is 16-bit on every real board (PM1_CNT_LEN == 2). A board claiming 4
 * gets a 32-bit access; an 8-bit one would drop SLP_EN (bit 13) on the floor and
 * look like a write that worked. Mirrors pm1_width() in acpi.c. */
static uint8_t pm1_access_width(const acpi_info_t *in) {
    return (in && in->pm1_cnt_len == 4) ? 32 : 16;
}

/* Wait, bounded, for the UART to go completely idle. Bounded because this runs
 * on the way to powering off: a wedged transmitter must cost a few thousand
 * port reads, not the shutdown. */
static void flush_console(void) {
    const acpi_platform_t *p = acpi_platform();
    if (!p || !p->io_read) return;
    for (int i = 0; i < 8192; i++)
        if ((p->io_read(UART_COM1_LSR, 8) & UART_LSR_IDLE) == UART_LSR_IDLE)
            return;
}

/* ====================================================================== */
/* the reason string: model-supplied, therefore hostile                   */
/* ====================================================================== */

size_t acpi_sanitize_reason(char *dst, size_t cap, const char *reason) {
    if (!dst || cap == 0) return 0;
    size_t o = 0;
    if (reason) {
        for (size_t i = 0; reason[i] && o + 1 < cap; i++) {
            unsigned char c = (unsigned char)reason[i];
            /* Drop, rather than escape: control characters could forge a line
             * break in the console transcript, and '[' / ']' could forge the
             * shape of a kernel trace line (trace.h). This string is printed by
             * kputs as well as passed to trace.h, and only the latter escapes. */
            if (c < 0x20 || c == 0x7F || c == '[' || c == ']') continue;
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

/* ====================================================================== */
/* the clean path                                                         */
/* ====================================================================== */

static void prepare_default(acpi_power_op_t op, const char *reason) {
    char r[128];
    acpi_sanitize_reason(r, sizeof r, reason);

    kputs("\n");
    kprintf("power: %s requested%s%s\n",
            op == ACPI_POWER_REBOOT ? "restart" : "shutdown",
            r[0] ? ": " : "", r);
    kputs("power: the filesystem is RAM-backed - nothing to sync, and "
          "everything written this boot is lost\n");
    kputs("power: bringing drivers down through the lifecycle "
          "(drivers_suspend_all)\n");
#ifndef FABLEOS_HOSTTEST
    drivers_suspend_all();
#endif
    kprintf("power: drivers suspended. This machine is going %s now.\n",
            op == ACPI_POWER_REBOOT ? "down and coming back" : "down");
    flush_console();
}

/* The clean path's inverse, for the one case where the machine survives its own
 * shutdown: every mechanism in the plan was refused and we are still executing.
 *
 * Without this the failure return is worse than doing nothing at all. prepare
 * has already run drivers_suspend_all(), so the e1000's receive and transmit
 * engines are off — and the model reached over that NIC is the ONLY human
 * interface this machine has. The tool would report its own failure to something
 * it can no longer talk to, and the only tool that could bring the NIC back
 * (device_resume) is on the far side of the dead NIC. There is no shell to
 * escape to. So: undo the clean path before reporting, and say so, because a
 * silent resume would make the log claim drivers are down when they are up. */
static void unprepare(void) {
    kputs("power: nothing took effect, so this machine is still running with "
          "every driver suspended. Bringing them back up (drivers_resume_all) "
          "- otherwise the model could not be reached to be told.\n");
#ifndef FABLEOS_HOSTTEST
    drivers_resume_all();
#endif
}

static acpi_prepare_fn g_prepare = prepare_default;

void acpi_set_prepare_hook(acpi_prepare_fn fn) {
    g_prepare = fn ? fn : prepare_default;
}

/* ====================================================================== */
/* legacy -> ACPI mode                                                    */
/* ====================================================================== */

int acpi_enable_hw(void) {
    const acpi_info_t     *in = acpi_info();
    const acpi_platform_t *p  = acpi_platform();

    if (!in->present || !p || !p->io_read || !p->io_write) return ACPI_OK;
    /* No SMI command port, or no enable value: this board has no legacy mode to
     * leave, so there is nothing to do and nothing has failed. */
    if (!in->smi_cmd || !in->acpi_enable) return ACPI_OK;
    /* Without a control block there is nothing to poll, and issuing an SMI we
     * cannot confirm is worse than not issuing it. */
    if (!in->pm1a_cnt) return ACPI_OK;

    uint8_t w = pm1_access_width(in);
    if (p->io_read(in->pm1a_cnt, w) & ACPI_PM1_CNT_SCI_EN) return ACPI_OK;

    kputs("power: board is in legacy mode - switching it to ACPI via the SMI "
          "command port\n");
    p->io_write(in->smi_cmd, in->acpi_enable, 8);

    /* 3 seconds is the conventional allowance for an SMI handler to flip
     * SCI_EN; the spec does not name a number. */
    for (int i = 0; i < 300; i++) {
        if (p->io_read(in->pm1a_cnt, w) & ACPI_PM1_CNT_SCI_EN) {
            kputs("power: ACPI mode is on\n");
            return ACPI_OK;
        }
        if (p->delay_ms) p->delay_ms(10);
    }
    kputs("power: the board never acknowledged ACPI mode - attempting soft-off "
          "anyway (many boards honour SLP_EN in legacy mode)\n");
    return ACPI_ENODEV;
}

/* ====================================================================== */
/* execution                                                              */
/* ====================================================================== */

int acpi_plan_execute(const acpi_plan_t *p) {
    const acpi_platform_t *pf = acpi_platform();
    int                    done = 0;

    if (!p || !pf) return 0;

    for (int i = 0; i < p->n && i < ACPI_PLAN_MAX; i++) {
        const acpi_action_t *s = &p->step[i];
        char                 what[64];
        acpi_action_describe(s, what, sizeof what);

        /* Printed BEFORE the write, and flushed, because after the write there
         * may be no machine left to print with. */
        if (acpi_action_has_operand(s->kind))
            kprintf("power: attempt %d/%d %s: %s\n", i + 1, p->n, s->method, what);
        else
            kprintf("power: attempt %d/%d %s\n", i + 1, p->n, s->method);
        flush_console();

        switch (s->kind) {
            case ACPI_ACT_IO_WRITE:
                if (pf->io_write) {
                    pf->io_write((uint32_t)s->address, s->value, s->bit_width);
                    done++;
                }
                break;
            case ACPI_ACT_MEM_WRITE8:
                if (pf->mem_write8) {
                    pf->mem_write8(s->address, (uint8_t)s->value);
                    done++;
                }
                break;
            case ACPI_ACT_KBD_PULSE:
                if (pf->kbd_pulse) { pf->kbd_pulse(); done++; }
                break;
            case ACPI_ACT_TRIPLE_FAULT:
                if (pf->triple_fault) { done++; pf->triple_fault(); }
                break;
            case ACPI_ACT_NONE:
                break;
        }

        /* Give the hardware time to act on it before concluding it did not. */
        if (s->settle_ms && pf->delay_ms) pf->delay_ms(s->settle_ms);

        /* Reaching here means the machine survived the step. Say so: this line
         * is how the log distinguishes "ACPI powered it off" from "the QEMU
         * fallback did". */
        kprintf("power: %s did not take effect - still running\n", s->method);
    }
    return done;
}

/* ====================================================================== */
/* the two verbs                                                          */
/* ====================================================================== */

static int power_go(acpi_power_op_t op, const char *reason) {
    acpi_plan_t plan;

    g_prepare(op, reason);

    /* Best effort, and deliberately not fatal: the return value is reported by
     * acpi_enable_hw() itself and soft-off is worth attempting either way. */
    if (op == ACPI_POWER_OFF) (void)acpi_enable_hw();

    int n = acpi_power_plan(acpi_info(), op, &plan);
    if (n < 0) {
        kputs("power: could not build a power plan (internal error) - the "
              "machine is still running\n");
        unprepare();
        return ACPI_EINVAL;
    }
    if (n == 0) {
        kputs("power: this platform exposes no way to turn itself off. No ACPI "
              "tables, no PM1 control block, and no known fallback port. The "
              "machine is still running.\n");
        unprepare();
        return ACPI_ENODEV;
    }

    char desc[192];
    acpi_plan_describe(&plan, desc, sizeof desc);
    kprintf("power: plan (%d step(s), in order): %s\n", n, desc);

    int done = acpi_plan_execute(&plan);

    /* Only reachable when every mechanism was refused. */
    kprintf("power: %d of %d mechanism(s) were issued and none took effect. "
            "This machine cannot %s.\n", done, n,
            op == ACPI_POWER_REBOOT ? "restart itself" : "turn itself off");
    unprepare();
    return ACPI_ENODEV;
}

int acpi_power_off(const char *reason) { return power_go(ACPI_POWER_OFF, reason); }
int acpi_reboot(const char *reason)    { return power_go(ACPI_POWER_REBOOT, reason); }

/* ====================================================================== */
/* the driver                                                             */
/* ====================================================================== */

#ifndef FABLEOS_HOSTTEST
/* Table discovery at BUS level: it needs the serial console (EARLY) to report
 * anything and nothing else at all. No device_create() call — ACPI is not a
 * device, and inventing a node for it would change the device tree every boot
 * test asserts on for no gain.
 *
 * A machine with no ACPI is not a failed init: it boots, it prompts, and
 * power_info tells the operator which mechanisms remain. Returning non-zero
 * would print "driver acpi: init failed" on hardware that is merely old. */
static int acpi_driver_init(void) {
    (void)acpi_init();
    return 0;
}

static const driver_t acpi_driver = {
    .name  = "acpi",
    .level = DRV_LEVEL_BUS,
    .cls   = DEV_CLASS_NONE,
    .init  = acpi_driver_init,
};
REGISTER_DRIVER(acpi_driver);
#endif
