/* capability_tools.c — the model's four verbs for teaching this machine.
 *
 * See include/capability.h for the mechanism. This file is only the model's view
 * of it: it holds no state, decides nothing, and every rule it appears to
 * enforce is enforced in core/capability.c so that the store and the tool cannot
 * disagree about what a capability is.
 *
 * FOUR TOOLS AND WHY NOT MORE
 *   The obvious design — one tool per saved capability — is impossible here, and
 *   not by a little: include/chat.h's CHAT_TOOLS_BYTES is the whole request's
 *   tool schema, the registry already fills most of it, and the schema is
 *   retransmitted on every round of every turn. So capabilities are DATA behind
 *   one dispatcher (capability_call) rather than entries in the tool table, and
 *   the four verbs are save / call / list / revert.
 *
 * THE DISCOVERY PROBLEM, and the shape of the answer
 *   A dispatcher tool alone is not enough. The system prompt tells the model that
 *   the tool list is the complete, mechanically assembled truth about this
 *   machine — so a capability that is not visible IN THE SCHEMA is a capability
 *   the model will apologise for lacking while holding it. That has happened
 *   here before.
 *
 *   So capability_call's description ends with a LIVE list, read off the store,
 *   rebuilt whenever the registry changes: g_call_desc below is the prose plus
 *   exactly CAP_PANEL_CHARS characters that core/capability.c overwrites in
 *   place. The model therefore sees the machine's learned vocabulary in the same
 *   place it sees its built-in one, without calling anything first.
 *
 *   THE PANEL IS A FIXED WIDTH, and the padding below is why this file has a
 *   _Static_assert in it. The assembled schema size is a compile-time constant in
 *   this tree (CHAT_REGISTRY_BYTES, which `make test-qemu` fails on if the boot
 *   banner disagrees), so a description that grew as the operator saved things
 *   would make that constant depend on the contents of a disk — a red test on a
 *   machine where nothing is wrong. Padding to a constant width costs ~1 KiB of
 *   schema whether anything is saved or not, and buys a schema size that does not
 *   move. The assert pins the pad literal to CAP_PANEL_CHARS, and
 *   capability_panel_check() re-checks the rendered length at run time and prints
 *   both numbers if they ever differ.
 */

#include "capability.h"
#include "tool.h"
#include "trace.h"
#include "json.h"
#include "kernel.h"

#include <stdio.h>

/* Longest error sentence this file builds. */
#define CT_ERR_MAX 256

/* ====================================================================== */
/* the live panel, and the padding that keeps the schema a fixed size      */
/* ====================================================================== */

#define CT_PAD64  "                                                                "
#define CT_PAD256 CT_PAD64  CT_PAD64  CT_PAD64  CT_PAD64
#define CT_PAD768 CT_PAD256 CT_PAD256 CT_PAD256

_Static_assert(sizeof(CT_PAD768) - 1 == CAP_PANEL_CHARS,
               "the reserved panel padding must be exactly CAP_PANEL_CHARS "
               "characters, or the assembled tool schema changes size when a "
               "capability is saved and CHAT_REGISTRY_BYTES goes stale");

#define CT_CALL_HEAD                                                           \
    "Run a capability this machine has been taught, by name. These are not "    \
    "built-in tools: each was authored here and kept, and calling one is a "    \
    "first-class action - prefer it over writing the same program again. "      \
    "\"args\" must hold exactly as many whole numbers as the capability "       \
    "declares, in order; a wrong count is refused, not padded. You get one "    \
    "number, an optional text output, and what the call cost. A capability may " \
    "call others, so one call can run a chain, and a failure names the "        \
    "capability, its source line and the reason.\n"                            \
    "SAVED HERE (live, off the store; a name not in this list does not "        \
    "exist): "

static char g_call_desc[] = CT_CALL_HEAD CT_PAD768;

/* Where the panel starts inside that buffer: immediately after the prose. */
#define CT_PANEL_OFF (sizeof(CT_CALL_HEAD) - 1)

_Static_assert(sizeof g_call_desc == CT_PANEL_OFF + CAP_PANEL_CHARS + 1,
               "g_call_desc must be exactly the prose plus the panel plus a NUL");

void capability_bringup(void) {
    /* Bind first, so that the load below renders straight into the schema. */
    capability_panel_bind(g_call_desc + CT_PANEL_OFF, CAP_PANEL_CHARS);
    capability_init();

    char why[CT_ERR_MAX];
    if (!capability_panel_check(why, sizeof why))
        kprintf("capability: PANEL SIZE MISMATCH - %s\n", why);
}

/* ====================================================================== */
/* argument helpers                                                       */
/* ====================================================================== */

static int fail(tool_result_t *r, int err, const char *op, const char *arg,
                const char *why) {
    r->is_error = 1;
    tool_result_printf(r, "%s", why);
    trace_err(err, op, "%s", arg ? arg : "");
    return TOOL_OK;
}

/* A required capability name. Model output: may be absent, of the wrong type,
 * enormous, or full of bytes a path may not hold. capability_name_ok() is the
 * one rule, and it lives in core. */
static int want_name(const json_value_t *in, char *dst, size_t cap,
                     char *why, size_t whycap) {
    why[0] = '\0';
    dst[0] = '\0';
    if (json_get_str(in, "name", dst, cap) != JSON_OK) {
        snprintf(why, whycap,
                 "\"name\" is required and must be a string of at most %d "
                 "characters", (int)cap - 1);
        return -1;
    }
    char sub[CAP_MSG_MAX];
    if (!capability_name_ok(dst, sub, sizeof sub)) {
        snprintf(why, whycap, "\"name\": %s", sub);
        dst[0] = '\0';
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* capability_save                                                        */
/* ====================================================================== */

/* One shared source buffer for the description being saved. Static because
 * CAP_SRC_MAX on the kernel stack (64 KiB, shared with lwIP and mbedTLS) is not
 * a trade worth making, and because exactly one tool call runs at a time. */
static char  g_src[CAP_SRC_MAX + 1];
static cap_t g_spec;

static int t_save(const tool_call_t *call, tool_result_t *r) {
    char   msg[CT_ERR_MAX];
    size_t srclen = 0;

    if (capability_parse_spec(call->input, call->input_len, &g_spec,
                              g_src, sizeof g_src, &srclen,
                              msg, sizeof msg) != CAP_OK)
        return fail(r, TOOL_EINVAL, "cap.save", g_spec.name, msg);

    int rc = capability_save(&g_spec, g_src, srclen, msg, sizeof msg);
    if (rc != CAP_OK)
        return fail(r, rc, "cap.save", g_spec.name, msg);

    const cap_t *c = capability_find(g_spec.name);
    tool_result_printf(r, "%s\n", msg);
    if (c) {
        tool_result_printf(r, "signature: %s(", c->name);
        for (int i = 0; i < c->nparam; i++)
            tool_result_printf(r, "%s%s", i ? ", " : "", c->param[i]);
        tool_result_printf(r, ") -> one number%s\n",
                           c->kind == CAP_KIND_PROGRAM
                           ? " plus optional text" : "");
        if (c->kind == CAP_KIND_PROGRAM)
            tool_result_printf(r, "assembled to %u instruction(s) from %u bytes "
                                  "of source\n",
                               (unsigned)c->ninsn, (unsigned)c->src_len);
        else
            tool_result_printf(r, "%u step(s)\n", (unsigned)c->nstep);
        if (c->version > 1)
            tool_result_printf(r,
                "version %u replaced version %u, which is now the ONE backup: "
                "capability_revert brings it back. Saving again over this name "
                "would lose it.\n", (unsigned)c->version, (unsigned)c->version - 1);
        tool_result_printf(r, "It is now in the list inside capability_call, so "
                              "call it by name - do not write it again.\n");
    }
    trace_ret((long)(c ? c->version : 0), "cap.save", "%s %s", g_spec.name,
              g_spec.kind == CAP_KIND_COMPOSE ? "compose" : "program");
    return TOOL_OK;
}

/* ====================================================================== */
/* capability_call                                                        */
/* ====================================================================== */

static int t_call(const tool_call_t *call, tool_result_t *r) {
    json_value_t in, args, e;
    char         name[CAP_NAME_MAX];
    char         why[CT_ERR_MAX];
    uint64_t     a[CAP_MAX_PARAMS] = { 0 };
    int          nargs = 0;

    if (json_parse(call->input, call->input_len, &in) != JSON_OK ||
        in.type != JSON_OBJECT)
        return fail(r, TOOL_EINVAL, "cap.call", "", "the arguments are not a "
                                                    "JSON object");
    if (want_name(&in, name, sizeof name, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cap.call", "", why);

    if (json_get(&in, "args", &args) == JSON_OK) {
        if (args.type != JSON_ARRAY)
            return fail(r, TOOL_EINVAL, "cap.call", name,
                        "\"args\" must be an array of whole numbers");
        size_t n = json_count(&args);
        if (n > CAP_MAX_PARAMS) {
            snprintf(why, sizeof why, "%d arguments; a capability takes at most "
                                      "%d", (int)n, CAP_MAX_PARAMS);
            return fail(r, TOOL_EINVAL, "cap.call", name, why);
        }
        for (size_t i = 0; i < n; i++) {
            int64_t v = 0;
            if (json_at(&args, i, &e) != JSON_OK || json_int(&e, &v) != JSON_OK) {
                snprintf(why, sizeof why,
                         "argument %d is not a whole number. Every capability "
                         "argument is a number; pass text through a file under "
                         "the data root instead", (int)i + 1);
                return fail(r, TOOL_EINVAL, "cap.call", name, why);
            }
            /* Negative is accepted and reinterpreted, because every value in
             * this machine is an unsigned 64-bit one and -1 is the only way to
             * write 0xFFFFFFFFFFFFFFFF in JSON. Said out loud in the result. */
            a[nargs++] = (uint64_t)v;
        }
    }

    cap_result_t res;
    int rc = capability_invoke(name, a, nargs, &res);

    if (rc != CAP_OK) {
        r->is_error = 1;
        tool_result_printf(r, "%s\n", res.msg);
        if (res.chain[0] && res.invocations > 1)
            tool_result_printf(r, "call chain: %s\n", res.chain);
        tool_result_printf(r, "(%u invocation(s), %lu VM instruction(s))\n",
                           (unsigned)res.invocations, (unsigned long)res.steps);
        trace_err(rc, "capability_call", "%s args=%d", name, nargs);
        return TOOL_OK;
    }

    tool_result_printf(r, "%s returned %lu", name, (unsigned long)res.value);
    if (res.value > 0x7FFFFFFFFFFFFFFFull)
        tool_result_printf(r, " (as a signed value, %ld)", (long)res.value);
    tool_result_printf(r, "\n");
    if (res.text_len)
        tool_result_printf(r, "text: %s\n", res.text);
    tool_result_printf(r, "cost: %u invocation(s)", (unsigned)res.invocations);
    if (res.depth_used) tool_result_printf(r, ", depth %u", (unsigned)res.depth_used);
    tool_result_printf(r, ", %lu VM instruction(s), %u syscall(s)\n",
                       (unsigned long)res.steps, (unsigned)res.sys_calls);
    if (res.invocations > 1)
        tool_result_printf(r, "chain: %s\n", res.chain);

    trace_ret((long)res.value, "capability_call", "%s args=%d inv=%u", name,
              nargs, (unsigned)res.invocations);
    return TOOL_OK;
}

/* ====================================================================== */
/* capability_list                                                        */
/* ====================================================================== */

static void put_signature(tool_result_t *r, const cap_t *c) {
    tool_result_printf(r, "%s(", c->name);
    for (int i = 0; i < c->nparam; i++)
        tool_result_printf(r, "%s%s", i ? "," : "", c->param[i]);
    tool_result_printf(r, ") v%u", (unsigned)c->version);
}

static void put_steps(tool_result_t *r, const cap_t *c) {
    for (int i = 0; i < c->nstep; i++) {
        const cap_step_t *s = &c->step[i];
        char t[24];
        tool_result_printf(r, "  %d. ", i + 1);
        if (s->cmp != CAP_CMP_NONE) {
            char l[24], rr[24];
            capability_arg_text(&s->lhs, l, sizeof l);
            capability_arg_text(&s->rhs, rr, sizeof rr);
            tool_result_printf(r, "when %s %s %s: ", l,
                               capability_cmp_text(s->cmp), rr);
        }
        tool_result_printf(r, "%s(", s->call);
        for (int k = 0; k < s->narg; k++) {
            capability_arg_text(&s->arg[k], t, sizeof t);
            tool_result_printf(r, "%s%s", k ? "," : "", t);
        }
        tool_result_printf(r, ")\n");
    }
}

static int t_list(const tool_call_t *call, tool_result_t *r) {
    json_value_t in, v;
    char         name[CAP_NAME_MAX];
    char         why[CT_ERR_MAX];

    if (json_parse(call->input, call->input_len, &in) != JSON_OK ||
        in.type != JSON_OBJECT)
        return fail(r, TOOL_EINVAL, "cap.list", "", "the arguments are not a "
                                                    "JSON object");

    /* Presence is tested separately from decoding: a name too long for the
     * buffer must be an error, not a silent fall-through to "list everything",
     * which is a different answer to a different question. */
    int one = (json_get(&in, "name", &v) == JSON_OK);
    if (one && want_name(&in, name, sizeof name, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cap.list", "", why);

    if (one) {
        const cap_t *c = capability_find(name);
        if (!c) {
            char why[CT_ERR_MAX];
            snprintf(why, sizeof why, "there is no capability called \"%s\". "
                                      "Call this tool with no name to see the "
                                      "whole list", name);
            return fail(r, CAP_ENOENT, "cap.list", name, why);
        }
        put_signature(r, c);
        tool_result_printf(r, " %s\n%s\n",
                           c->kind == CAP_KIND_PROGRAM ? "[program]" : "[compose]",
                           c->doc);
        tool_result_printf(r, "backup: %s | calls since boot: %u (%u failed)\n",
                           c->has_backup ? "yes, capability_revert works"
                                         : "none - one save from being lost",
                           (unsigned)c->calls, (unsigned)c->failures);
        if (c->kind == CAP_KIND_COMPOSE) put_steps(r, c);
        else tool_result_printf(r, "%u instruction(s), %u bytes of source\n",
                                (unsigned)c->ninsn, (unsigned)c->src_len);
        trace_ret(1, "cap.list", "%s", name);
        return TOOL_OK;
    }

    int n = capability_count();
    tool_result_printf(r, "store %s (%s), data root %s\n",
                       capability_store_root(),
                       capability_store_persists()
                       ? "survives a reboot"
                       : "RAM ONLY - nothing saved here survives a reboot",
                       capability_data_root());
    if (capability_rejected())
        tool_result_printf(r, "%d stored record(s) were REJECTED at boot as "
                              "damaged; the console said which\n",
                           capability_rejected());
    if (!n) {
        tool_result_printf(r, "No capabilities are saved. capability_save is how "
                              "this machine learns something it can reuse.\n");
        trace_ret(0, "cap.list", "%s", "");
        return TOOL_OK;
    }
    tool_result_printf(r, "%d saved, %d slots free:\n", n, CAP_MAX - n);
    for (int i = 0; i < n; i++) {
        const cap_t *c = capability_at(i);
        put_signature(r, c);
        tool_result_printf(r, " %s %s\n", c->kind == CAP_KIND_PROGRAM ? "prog"
                                                                     : "comp",
                           c->doc);
    }
    trace_ret(n, "cap.list", "%s", "");
    return TOOL_OK;
}

/* ====================================================================== */
/* capability_revert                                                      */
/* ====================================================================== */

static int t_revert(const tool_call_t *call, tool_result_t *r) {
    json_value_t in, v;
    char         name[CAP_NAME_MAX];
    char         why[CT_ERR_MAX];
    int          del = 0;

    if (json_parse(call->input, call->input_len, &in) != JSON_OK ||
        in.type != JSON_OBJECT)
        return fail(r, TOOL_EINVAL, "cap.revert", "", "the arguments are not a "
                                                      "JSON object");
    if (want_name(&in, name, sizeof name, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cap.revert", "", why);

    if (json_get(&in, "mode", &v) == JSON_OK) {
        if      (json_str_eq(&v, "delete"))   del = 1;
        else if (json_str_eq(&v, "rollback")) del = 0;
        else
            return fail(r, TOOL_EINVAL, "cap.revert", name,
                        "\"mode\" must be \"rollback\" (restore the backup as a "
                        "new version) or \"delete\" (remove it for good)");
    }

    char msg[CT_ERR_MAX];
    int rc = del ? capability_delete(name, msg, sizeof msg)
                 : capability_revert(name, msg, sizeof msg);
    if (rc != CAP_OK)
        return fail(r, rc, del ? "cap.delete" : "cap.revert", name, msg);

    tool_result_printf(r, "%s\n", msg);
    trace_ok(del ? "cap.delete" : "cap.revert", "%s", name);
    return TOOL_OK;
}

/* ====================================================================== */
/* registration                                                           */
/* ====================================================================== */

static const tool_t capability_save_tool = {
    .name = "capability_save",
    .description =
        "Teach this machine something and KEEP it: a named, documented program "
        "stored on disk that anything can call later by name, including after a "
        "reboot. This is how the machine grows instead of starting over.\n"
        "GIVE EITHER \"program\" (driver-VM source - the instruction set "
        "driver_run documents) OR \"steps\" (a composition of calls to "
        "capabilities that already exist). \"params\" names the arguments in "
        "order; each is one whole number. \"doc\" is one printable-ASCII line and "
        "is what a future caller reads to decide whether to call this: write it "
        "for a stranger.\n"
        "PROGRAM CONTRACT. Entry: r0..r5 = the declared params in order, other "
        "registers 0, the 65536-byte scratch arena zeroed. Halt: r0 = the "
        "result; r1,r2 = offset and LENGTH of optional text output in scratch "
        "(r2=0 for none); r3 = 0. NO DEVICE ACCESS: no ports, MMIO, PCI or DMA. "
        "Syscalls: con.write, fs.read, fs.write, fs.size, time.ms, with fs.* "
        "confined to the data root capability_list names - which is NOT "
        "driver_run's /vm. A program you developed under driver_run keeps "
        "working there and is DENIED here until every path in it is rewritten to "
        "that data root, so rewrite them before you save. Source that does not "
        "assemble is refused here and nothing is saved.\n"
        "ONE CAPABILITY CALLING ANOTHER. Mid-run calls are impossible (the VM "
        "refuses re-entry), so a program delegates as its LAST act: write the "
        "callee's name into scratch, halt with r3=0x7a11ca11, r4,r5 = that "
        "name's offset and length, r6..r11 = its arguments; the callee's result "
        "becomes yours. It costs no stack, so accumulator recursion works: "
        "fact(n,acc) tail-calling fact(n-1,acc*n).\n"
        "STEPS. Each is {\"call\":\"name\",\"args\":[..]} plus optional "
        "\"when\":[left,op,right]. An argument or guard side is a decimal "
        "number, \"$0\"..\"$5\" (this capability's parameter) or \"#1\"..\"#5\" "
        "(an EARLIER step's result); op is == != < <= > >= , unsigned. A false "
        "guard skips the step; the result is the last step that ran. A "
        "composition may call itself: nesting is capped at 8, total invocations "
        "at 64, and exceeding either reports the whole chain.\n"
        "REPLACING. Saving over a name bumps its version and keeps exactly ONE "
        "backup (capability_revert restores it), so saving twice loses the "
        "working version. Changing the parameter count is refused while a "
        "composition calls it.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"a-z, 0-9 and _, "
        "starting with a letter\"},"
        "\"doc\":{\"type\":\"string\",\"description\":\"one line: what it does, "
        "what its arguments mean, what it returns\"},"
        "\"params\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"program\":{\"type\":\"string\",\"description\":\"driver-VM source\"},"
        "\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
        "\"properties\":{\"call\":{\"type\":\"string\"},"
        "\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
        "\"when\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
        "\"required\":[\"call\"]}}},"
        "\"required\":[\"name\",\"doc\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_save,
};
REGISTER_TOOL(capability_save_tool);

static const tool_t capability_call_tool = {
    .name         = "capability_call",
    .description  = g_call_desc,
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\"},"
        "\"args\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},"
        "\"description\":\"exactly as many whole numbers as the capability "
        "declares, in order\"}},"
        "\"required\":[\"name\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_call,
};
REGISTER_TOOL(capability_call_tool);

static const tool_t capability_list_tool = {
    .name = "capability_list",
    .description =
        "Everything known about the saved capabilities: where the store is and "
        "whether it survives a reboot, where capability programs may read and "
        "write files, and per capability its doc, signature, version, whether a "
        "backup exists to roll back to, and how often it has been called and has "
        "failed since boot. With \"name\", also a composition's steps. Read this "
        "before replacing something: it says whether the version you would "
        "overwrite is the only copy.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"omit for the whole "
        "list\"}}}",
    .invoke = t_list,
};
REGISTER_TOOL(capability_list_tool);

static const tool_t capability_revert_tool = {
    .name = "capability_revert",
    .description =
        "Undo a capability_save. mode \"rollback\" (the default) restores the "
        "ONE backup kept for a name as a NEW version - rolling back a broken v4 "
        "gives v5 holding v3's code and makes v4 the backup, so doing it twice "
        "is a redo and no version number is ever reused. mode \"delete\" removes "
        "the capability and its backup for good. Either is refused, naming the "
        "caller, while a composition still calls it.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\"},"
        "\"mode\":{\"type\":\"string\",\"enum\":[\"rollback\",\"delete\"]}},"
        "\"required\":[\"name\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_revert,
};
REGISTER_TOOL(capability_revert_tool);
