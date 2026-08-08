/* power_tools.c — the two irreversible syscalls, and the one that explains them.
 *
 * PURPOSE
 *   "Shut down" and "reboot yourself" are things any operating system does, and
 *   on a machine whose only interface is a sentence they have to be reachable
 *   from a sentence. These three tools are that reach:
 *
 *     power_info     what this platform can actually do, and by which
 *                    mechanism, read from the firmware tables. Free of
 *                    consequence, and the tool to call first.
 *     power_off      ACPI soft-off (\_S5_ into PM1a_CNT), with the QEMU ports as
 *                    a last resort. Does not return when it works.
 *     power_reboot   the FADT's reset register, then the 8042 pulse, then a
 *                    triple fault. Never returns at all.
 *
 *   The mechanism lives in drivers/acpi/ (include/acpi.h). This file is only the
 *   model-facing edge: argument validation, the ground-truth line, and result
 *   text that cannot be misread.
 *
 * ============================================================================
 * DECISION: a "kill yourself" verb IS exposed to the model. Why that is safe
 * enough, and what makes it recoverable when it is not.
 * ============================================================================
 *   arch/x86_64/fault_inject.c argues the opposite for deliberate faults: a
 *   machine driven by natural language must not have a way to die sitting one
 *   hallucination away. The difference is not the outcome — both stop the
 *   machine — it is who asked and whether the record survives:
 *
 *   a. An operator can ASK for this, in the words anyone would use, and there is
 *      no other way to do it. A crash injector answers no request at all.
 *   b. It is not silent. tools/ cannot forge the audit trail: the kernel's trace
 *      line is emitted before the machine goes, and drivers/acpi/power.c prints
 *      the exact write it is about to issue. A shutdown always leaves a reason
 *      and a mechanism behind in the log.
 *   c. It is not free. Both mutating tools REQUIRE "confirm": true. A model that
 *      merely mentions shutting down, or that mis-fires a tool call while
 *      reasoning about power management, gets a legible refusal and the machine
 *      stays up. The argument exists precisely because a single confused token
 *      should not be able to end a session, and it costs the deliberate case one
 *      extra field.
 *   d. It is reversible in the only sense that matters here: nothing is written
 *      to persistent storage, because there is no persistent storage. The cost
 *      of a wrong shutdown is the session, not any data.
 *
 *   Both are flagged TOOL_MUTATES. They mutate nothing in memory — they end the
 *   machine — but TOOL_MUTATES is tool.h's documented hook for "a future
 *   confirmation prompt or read-only mode keys off this", and an irreversible
 *   power transition is the single best reason for that hook to exist.
 *
 * ============================================================================
 * THE TWO-LINE TRACE PROTOCOL, AND WHY THE FIRST LINE IS NOT A LIE
 * ============================================================================
 *   trace.h's contract is that a kernel line is printed AFTER the call, from the
 *   real return value. A power-off has no after. Emitting nothing would be worse
 *   than useless — a log that stops mid-transcript is exactly the failure this
 *   project exists to prevent, since it is indistinguishable from a fault, a
 *   hang, or someone killing QEMU. So the line is emitted first, and it is
 *   worded to report only what has genuinely already happened:
 *
 *     [power_off stage=commit method=acpi-pm1a io 0x604<-0x2000/16 reason=... -> ok]
 *
 *   What succeeded, at that instant, is the *commitment*: the arguments were
 *   accepted, the confirmation was present, the firmware tables were read, and
 *   this exact write is about to be issued. `stage=commit` is there so the line
 *   can never be read as "the machine is off". Then one of two things is true:
 *
 *     - the log ends there, and the last thing in it is the kernel naming the
 *       write that stopped the machine; or
 *     - a second line follows:
 *         [power_off stage=failed issued=2 -> -19]
 *       and the machine is still running, with the model told so in its result.
 *
 *   Note -19 rather than ENODEV: lib/trace.c's symbol table does not carry that
 *   code and this work does not own that file. trace.h guarantees an unknown
 *   code renders as the signed integer rather than being swallowed.
 *
 * UNTRUSTED INPUT
 *   Both mutating tools read exactly two fields through json.h: "confirm", which
 *   must be a JSON boolean that is true (a string "true", the integer 1, or a
 *   missing key are all refusals), and "reason", an optional string that is
 *   length-bounded and stripped of control characters and brackets by
 *   acpi_sanitize_reason() before it reaches either the console or a trace
 *   argument. power_info ignores its input entirely. A 10 KB reason, embedded
 *   NULs, truncated JSON and wrong types all produce a refusal or a bounded
 *   string, never a fault — see tests/host/test_acpi.c.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "acpi.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>      /* snprintf, via port/stdio.h when freestanding */

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand. Export a named pointer per tool instead; tests/host/test_acpi.c stands
 * in for the linker and builds the table from those. Kernel builds are
 * untouched. Same pattern as tools/mem_tools.c. */
#ifdef FABLEOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const fableos_hosttool_##var = &(var)
#endif

/* Bounds. The reason is a human sentence, not a payload: 96 characters is more
 * than enough for "operator asked at the prompt" and keeps the trace line
 * comfortably inside TRACE_LINE_MAX alongside the method and the write. */
#define REASON_MAX 96

/* ====================================================================== */
/* shared argument handling                                               */
/* ====================================================================== */

/* Read and validate the arguments of a mutating power tool.
 *
 * Returns 0 when the call is confirmed and may proceed. On any refusal it
 * writes the explanation into `r`, sets is_error, emits the refusal trace line,
 * and returns non-zero — the machine keeps running and the model has everything
 * it needs to retry correctly on its next turn. */
static int power_args(const tool_call_t *call, tool_result_t *r, const char *op,
                      char *reason_out, size_t reason_cap) {
    json_value_t root, v;

    if (reason_out && reason_cap) reason_out[0] = '\0';

    if (!call || !call->input ||
        json_parse(call->input, call->input_len, &root) != JSON_OK ||
        root.type != JSON_OBJECT) {
        r->is_error = 1;
        tool_result_printf(r,
            "REFUSED: the arguments were not a JSON object. %s requires "
            "{\"confirm\":true} and optionally {\"reason\":\"...\"}. "
            "Nothing happened; the machine is still running.\n", op);
        trace_err(TOOL_EINVAL, op, "refused=unparsable-input");
        return -1;
    }

    /* Strictly a boolean true. "true", 1 and "yes" are all refusals: this is the
     * one guard between a confused tool call and the end of the session, so it
     * does not do type coercion. */
    int confirmed = 0;
    if (json_get(&root, "confirm", &v) != JSON_OK ||
        json_bool(&v, &confirmed) != JSON_OK || !confirmed) {
        r->is_error = 1;
        tool_result_printf(r,
            "REFUSED: %s is irreversible and needs explicit confirmation. Call "
            "it again with \"confirm\": true (the JSON boolean, not the string) "
            "if that is really what was asked for. Nothing happened; the machine "
            "is still running.\n", op);
        trace_err(TOOL_EINVAL, op, "refused=unconfirmed");
        return -1;
    }

    /* Optional, and a bad one is not worth refusing a confirmed request over:
     * an absent or unreadable reason simply becomes no reason. */
    if (reason_out && reason_cap) {
        /* JSON_ENOSPC is accepted as well as JSON_OK: json.h leaves a valid
         * NUL-terminated prefix in that case, and a 10 KB reason is more
         * usefully recorded as its first few words than dropped entirely. It
         * would be cut to REASON_MAX by the sanitiser regardless. */
        char raw[REASON_MAX * 2];
        int  rc = json_get_str(&root, "reason", raw, sizeof raw);
        if (rc == JSON_OK || rc == JSON_ENOSPC)
            acpi_sanitize_reason(reason_out, reason_cap, raw);
    }
    return 0;
}

/* The first step of the plan for `op`, so the trace line can name the mechanism
 * that is about to be used. Writes "<method>: <write>" into `dst`. */
static void first_step(acpi_power_op_t op, char *dst, size_t cap) {
    acpi_plan_t plan;
    char        what[64];

    if (cap == 0) return;
    dst[0] = '\0';
    if (acpi_power_plan(acpi_info(), op, &plan) <= 0) {
        snprintf(dst, cap, "none");
        return;
    }
    if (!acpi_action_has_operand(plan.step[0].kind)) {
        snprintf(dst, cap, "%s", plan.step[0].method);
        return;
    }
    acpi_action_describe(&plan.step[0], what, sizeof what);
    snprintf(dst, cap, "%s %s", plan.step[0].method, what);
}

/* ====================================================================== */
/* power_off                                                              */
/* ====================================================================== */

static int t_power_off(const tool_call_t *call, tool_result_t *r) {
    char reason[REASON_MAX];
    if (power_args(call, r, "power_off", reason, sizeof reason) != 0)
        return TOOL_OK;             /* refused; legible error already in `r` */

    acpi_plan_t plan;
    int         n = acpi_power_plan(acpi_info(), ACPI_POWER_OFF, &plan);
    char        desc[192];
    acpi_plan_describe(&plan, desc, sizeof desc);

    if (n <= 0) {
        r->is_error = 1;
        tool_result_printf(r,
            "FAILED: this machine exposes no way to turn itself off. No valid "
            "ACPI tables were found (no RSDP, or no PM1 control block, or no "
            "\\_S5_ sleep type) and no fallback port applies. Nothing happened; "
            "the machine is still running. power_info has the details.\n");
        trace_err(ACPI_ENODEV, "power_off", "refused=no-mechanism");
        return TOOL_OK;
    }

    /* The result the model would read if it could. It cannot when this works —
     * which is why the text says so, and why it is written BEFORE the attempt:
     * the only circumstance in which anyone reads it is failure. */
    tool_result_printf(r,
        "POWERING OFF NOW. This is irreversible and takes effect immediately:\n"
        "  - the machine stops executing; there is no further turn\n"
        "  - RAM is lost, including the entire filesystem (it is RAM-backed)\n"
        "  - you will not receive this result if the shutdown succeeds\n"
        "reason: %s\n"
        "mechanisms, in order: %s\n"
        "\n"
        "IF YOU ARE READING THIS TEXT, THE SHUTDOWN DID NOT HAPPEN and the "
        "machine is still running.\n",
        reason[0] ? reason : "(none given)", desc);

    /* Ground truth first: see the two-line protocol in the header comment. */
    char method[96];
    first_step(ACPI_POWER_OFF, method, sizeof method);
    trace_ok("power_off", "stage=commit method=%s steps=%d reason=%s",
             method, n, reason[0] ? reason : "-");

    int rc = acpi_power_off(reason);

    /* Still here: every mechanism was refused by the hardware. */
    r->is_error = 1;
    tool_result_printf(r,
        "\nAll %d mechanism(s) were issued and the machine did not stop "
        "(rc=%d). Nothing else can be tried from here.\n", n, rc);
    trace_err(rc, "power_off", "stage=failed steps=%d", n);
    return TOOL_OK;
}

static const tool_t power_off_tool = {
    .name        = "power_off",
    .description =
        "Shut this machine down: ACPI soft-off (the \\_S5_ sleep type written to "
        "PM1a_CNT), falling back to the QEMU power ports. IRREVERSIBLE AND "
        "IMMEDIATE - execution stops, RAM and the whole RAM-backed filesystem "
        "are lost, and there is no further turn, so you will only ever see this "
        "tool's result if it FAILED. Requires \"confirm\": true. Drivers are "
        "suspended and a final line is printed first. Call power_info to see "
        "what this platform supports before using this.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"confirm\":{\"type\":\"boolean\"},"
        "\"reason\":{\"type\":\"string\"}},"
        "\"required\":[\"confirm\"]}",
    .flags       = TOOL_MUTATES,
    .invoke      = t_power_off,
};
REGISTER_TOOL(power_off_tool);

/* ====================================================================== */
/* power_reboot                                                           */
/* ====================================================================== */

static int t_power_reboot(const tool_call_t *call, tool_result_t *r) {
    char reason[REASON_MAX];
    if (power_args(call, r, "power_reboot", reason, sizeof reason) != 0)
        return TOOL_OK;

    acpi_plan_t plan;
    int         n = acpi_power_plan(acpi_info(), ACPI_POWER_REBOOT, &plan);
    char        desc[192];
    acpi_plan_describe(&plan, desc, sizeof desc);

    tool_result_printf(r,
        "RESTARTING NOW. This is irreversible and takes effect immediately:\n"
        "  - the machine resets and boots again from the beginning\n"
        "  - RAM is lost, including the entire filesystem (it is RAM-backed)\n"
        "  - this conversation ends here; the new boot starts with no history\n"
        "reason: %s\n"
        "mechanisms, in order: %s\n"
        "\n"
        "The last mechanism is a triple fault, which the CPU cannot refuse, so "
        "this call does not return. IF YOU ARE READING THIS TEXT, SOMETHING IS "
        "VERY WRONG.\n",
        reason[0] ? reason : "(none given)", desc);

    char method[96];
    first_step(ACPI_POWER_REBOOT, method, sizeof method);
    trace_ok("power_reboot", "stage=commit method=%s steps=%d reason=%s",
             method, n, reason[0] ? reason : "-");

    int rc = acpi_reboot(reason);

    /* Unreachable in practice: acpi_power_plan() always ends a reboot plan with
     * a triple fault. Handled anyway, because "unreachable" plus a machine that
     * is somehow still running is exactly when a legible message matters. */
    r->is_error = 1;
    tool_result_printf(r,
        "\nEvery restart mechanism failed, including the triple fault (rc=%d). "
        "That should not be possible; this machine is in an unexpected state.\n",
        rc);
    trace_err(rc, "power_reboot", "stage=failed steps=%d", n);
    return TOOL_OK;
}

static const tool_t power_reboot_tool = {
    .name        = "power_reboot",
    .description =
        "Restart this machine: the ACPI reset register named by the FADT, then "
        "the 8042 keyboard-controller reset pulse, then a triple fault, tried in "
        "that order. IRREVERSIBLE AND IMMEDIATE - RAM and the whole RAM-backed "
        "filesystem are lost, this conversation ends, and the machine boots "
        "again with no history. Requires \"confirm\": true. Drivers are "
        "suspended and a final line is printed first. This call does not return.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"confirm\":{\"type\":\"boolean\"},"
        "\"reason\":{\"type\":\"string\"}},"
        "\"required\":[\"confirm\"]}",
    .flags       = TOOL_MUTATES,
    .invoke      = t_power_reboot,
};
REGISTER_TOOL(power_reboot_tool);

/* ====================================================================== */
/* power_info                                                             */
/* ====================================================================== */

static int t_power_info(const tool_call_t *call, tool_result_t *r) {
    (void)call;                     /* no arguments: nothing to misparse */

    const acpi_info_t *in = acpi_info();
    acpi_plan_t        off, reb;
    char               offdesc[192], rebdesc[192];

    int noff = acpi_power_plan(in, ACPI_POWER_OFF, &off);
    int nreb = acpi_power_plan(in, ACPI_POWER_REBOOT, &reb);
    acpi_plan_describe(&off, offdesc, sizeof offdesc);
    acpi_plan_describe(&reb, rebdesc, sizeof rebdesc);

    if (!in->present) {
        tool_result_printf(r,
            "ACPI: NOT AVAILABLE. No valid RSDP was found in the EBDA or in "
            "0xE0000-0xFFFFF, so this machine published no firmware tables.\n");
    } else {
        tool_result_printf(r,
            "ACPI: available (RSDP revision %u, OEM \"%s\") at 0x%llx\n"
            "  root table:   %s at 0x%llx, %d table(s) enumerated\n"
            "  FADT:         %s\n",
            (unsigned)in->rsdp_revision, in->oem_id,
            (unsigned long long)in->rsdp_phys,
            in->used_xsdt ? "XSDT" : "RSDT",
            (unsigned long long)(in->used_xsdt ? in->xsdt_phys : in->rsdt_phys),
            in->table_count,
            in->fadt_phys ? "found" : "MISSING (soft-off has no ACPI path)");

        if (in->fadt_phys)
            tool_result_printf(r,
                "  FADT at:      0x%llx, revision %u, %u bytes\n"
                "  PM1a_CNT:     0x%x    PM1b_CNT: 0x%x    PM1_CNT_LEN: %u\n"
                "  SMI command:  0x%x (enable=0x%x)   SCI IRQ: %u\n"
                "  DSDT:         0x%llx (%u bytes)\n",
                (unsigned long long)in->fadt_phys, (unsigned)in->fadt_revision,
                (unsigned)in->fadt_len,
                (unsigned)in->pm1a_cnt, (unsigned)in->pm1b_cnt,
                (unsigned)in->pm1_cnt_len,
                (unsigned)in->smi_cmd, (unsigned)in->acpi_enable,
                (unsigned)in->sci_int,
                (unsigned long long)in->dsdt_phys, (unsigned)in->dsdt_len);

        if (in->s5_found)
            tool_result_printf(r,
                "  \\_S5_:        found in the %s: SLP_TYPa=%u SLP_TYPb=%u "
                "(soft-off writes SLP_TYP|SLP_EN to PM1a_CNT)\n",
                in->s5_from, (unsigned)in->slp_typ_a, (unsigned)in->slp_typ_b);
        else
            tool_result_printf(r,
                "  \\_S5_:        NOT FOUND in the DSDT or any SSDT, so the "
                "sleep type for soft-off is unknown and ACPI power-off cannot "
                "be attempted\n");

        if (in->reset_supported)
            tool_result_printf(r,
                "  reset reg:    %s 0x%llx, %u bits, write 0x%x\n",
                in->reset_reg.space_id == ACPI_SPACE_IO ? "I/O port" : "memory",
                (unsigned long long)in->reset_reg.address,
                (unsigned)in->reset_reg.bit_width, (unsigned)in->reset_value);
        else
            tool_result_printf(r,
                "  reset reg:    not provided by the firmware; restart falls "
                "back to the 8042 pulse and then a triple fault\n");
    }

    tool_result_printf(r,
        "\npower_off would try %d mechanism(s), in order: %s\n"
        "power_reboot would try %d, in order: %s\n"
        "\nBoth are irreversible and both require \"confirm\": true. "
        "power_off %s. power_reboot always ends the machine (the final "
        "mechanism is a triple fault, which cannot be refused).\n",
        noff, noff > 0 ? offdesc : "(nothing - this machine cannot power off)",
        nreb, rebdesc,
        acpi_available()
            ? "would use real ACPI soft-off"
            : "has no ACPI path and would rely on a platform-specific port");

    trace_ret(noff, "power_info", "acpi=%s s5=%s reset=%s",
              in->present ? "yes" : "no",
              in->s5_found ? "yes" : "no",
              in->reset_supported ? "yes" : "no");
    return TOOL_OK;
}

static const tool_t power_info_tool = {
    .name        = "power_info",
    .description =
        "Report what this machine can do about its own power: whether ACPI "
        "tables were found, the FADT's PM1 control blocks, the \\_S5_ soft-off "
        "sleep type read from the DSDT, whether the firmware provides a reset "
        "register, and the exact ordered list of mechanisms power_off and "
        "power_reboot would try. Read-only and safe to call at any time. Takes "
        "no arguments.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags       = 0,
    .invoke      = t_power_info,
};
REGISTER_TOOL(power_info_tool);
