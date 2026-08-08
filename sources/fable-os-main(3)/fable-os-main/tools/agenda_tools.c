/* agenda_tools.c — the model scheduling the machine's own unprompted work.
 *
 * PURPOSE
 *   include/agenda.h holds the argument for why an unattended action is
 *   acceptable and what bounds it. This file is the surface the model reaches it
 *   through, and it has exactly one job of its own: turn an untrusted JSON object
 *   into an agenda_item_t, or refuse it with a sentence precise enough that the
 *   next turn fixes it without another guess.
 *
 * THE TOOL SET
 *   agenda_save     Schedule something, or replace it. The only mutating tool
 *                   here, and the one that makes this machine able to act when
 *                   nobody is present. Validates through agenda_save(), which
 *                   checks the named tool IS REGISTERED and that the argument
 *                   parses — so an item that would fail silently at three in the
 *                   morning is refused now, while somebody is there to be told.
 *   agenda_list     What is scheduled, whether it is persistent, how many times
 *                   each item has run this boot, and what the last run said.
 *   agenda_control  delete / enable / disable / run_now, and the master switch.
 *                   run_now is the important one: it proves an item works at the
 *                   moment it is saved rather than at the next boot.
 *
 * WHY THE TOOL RESULT ALWAYS NAMES THE STORE
 *   "Saved" means two different things depending on whether a disk is attached,
 *   and only one of them is what the operator wanted. An agenda in a RAM
 *   filesystem is gone at the next power cycle — which is precisely the problem
 *   the agenda exists to solve — so every result here says which path it used and
 *   whether that path survives. The same discipline core/capability.c applies to
 *   its store.
 *
 * DEPENDENCIES
 *   agenda.h, tool.h, trace.h, json.h, kernel.h. No hardware, no allocation.
 */

#include "agenda.h"
#include "json.h"
#include "kernel.h"
#include "tool.h"
#include "trace.h"

#include <stdint.h>
#include <stddef.h>

/* Same Mach-O shim every host-tested tool file carries: on the host a bare
 * section name is rejected, so export a named pointer and let the test stand in
 * for the linker. Kernel builds are untouched. */
#ifdef FABLEOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const fableos_hosttool_##var = &(var)
#endif

#define AG_WORD_MAX 24

static int ag_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* Read an optional string field into `dst`. 1 present, 0 absent, -1 malformed or
 * too long. ENOSPC is a refusal rather than an absence: a clipped sentence is a
 * different sentence, and this one is going to be executed. */
static int ag_str(const json_value_t *root, const char *key, char *dst,
                  size_t cap) {
    json_value_t v;
    int rc = json_get(root, key, &v);
    dst[0] = '\0';
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type != JSON_STRING) return -1;
    if (json_str(&v, dst, cap, NULL) != JSON_OK) return -1;
    return 1;
}

/* Read an optional non-negative integer. 1 present, 0 absent, -1 malformed. */
static int ag_int(const json_value_t *root, const char *key, uint32_t *out) {
    json_value_t v;
    int64_t      n;
    int rc = json_get(root, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type != JSON_NUMBER) return -1;
    if (json_int(&v, &n) != JSON_OK) return -1;
    if (n < 0 || n > 0x7FFFFFFF) return -1;
    *out = (uint32_t)n;
    return 1;
}

/* The value of "arg" may be a string (a sentence, or JSON the model wrote as
 * text) OR an object (the natural way to write a tool's arguments). Accepting
 * both is not laxness: a model told "the arguments, as an object" writes an
 * object, and a model told "a sentence" writes a string, and refusing either
 * would cost a round trip to learn a syntax rule with no meaning behind it. An
 * object is copied out verbatim — it is already exactly the bytes a tool
 * dispatch wants. */
static int ag_arg(const json_value_t *root, char *dst, size_t cap) {
    json_value_t v;
    int rc = json_get(root, "arg", &v);
    dst[0] = '\0';
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_STRING)
        return (json_str(&v, dst, cap, NULL) == JSON_OK) ? 1 : -1;
    if (v.type == JSON_OBJECT) {
        if (v.len == 0 || v.len >= cap) return -1;
        for (size_t i = 0; i < v.len; i++) dst[i] = v.start[i];
        dst[v.len] = '\0';
        return 1;
    }
    return -1;
}

static void store_line(tool_result_t *r) {
    tool_result_printf(r, "store %s (%s)\n", agenda_store(),
                       agenda_persistent()
                           ? "survives a reboot"
                           : "RAM ONLY - nothing scheduled here survives a "
                             "reboot, so this agenda is for this boot only");
}

static void list_body(tool_result_t *r) {
    int n = agenda_count();
    store_line(r);
    tool_result_printf(r,
        "the agenda as a whole is %s; %u autonomous action(s) this boot, %u "
        "failed, %u abandoned by a CPU fault\n",
        agenda_enabled() ? "ON" : "OFF",
        (unsigned)agenda_runs(), (unsigned)agenda_failures(),
        (unsigned)agenda_escapes());
    if (n == 0) {
        tool_result_printf(r,
            "Nothing is scheduled. This machine currently does nothing unless "
            "somebody speaks to it.\n");
        return;
    }
    tool_result_printf(r, "%d of %u slot(s) used:\n", n,
                       (unsigned)AGENDA_MAX_ITEMS);
    for (int i = 0; i < n; i++) {
        const agenda_item_t *it = agenda_at(i);
        if (!it) continue;
        tool_result_printf(r, "%s: %s", it->name, agenda_when_name(it->when));
        if (it->when == AGENDA_WHEN_EVERY)
            tool_result_printf(r, " %u ms", (unsigned)it->period_ms);
        if (it->act == AGENDA_DO_TOOL)
            tool_result_printf(r, " -> tool %s %s", it->tool, it->arg);
        else
            tool_result_printf(r, " -> say \"%s\"", it->arg);
        tool_result_printf(r, " [%s, %u/%u run(s)]",
                           it->enabled ? "enabled" : "DISABLED",
                           (unsigned)it->runs, (unsigned)it->max_runs);
        if (it->note[0]) tool_result_printf(r, " %s", it->note);
        if (it->runs)
            tool_result_printf(r, "\n    last run: %s%s",
                               it->last_rc ? "FAILED - " : "",
                               it->last_why[0] ? it->last_why : "(no detail)");
        if (it->escapes)
            tool_result_printf(r,
                "\n    %u run(s) took a fatal CPU exception; the machine "
                "survived each one because an autonomous action is always run "
                "inside a fault guard",
                (unsigned)it->escapes);
        tool_result_printf(r, "\n");
    }
}

/* ====================================================================== */
/* agenda_save                                                            */
/* ====================================================================== */

static const char SAVE_USAGE[] =
    "Expected {\"name\":\"...\",\"when\":\"boot|every|once\",\"do\":\"tool|say\","
    "\"tool\":\"<a registered tool>\",\"arg\":{...} or \"a sentence\","
    "\"period_ms\":N,\"max_runs\":N,\"note\":\"why\"}.\n";

static int save_fail(tool_result_t *r, const char *why, const char *extra) {
    r->is_error = 1;
    tool_result_printf(r, "agenda_save refused: %s%s%s%s.\n%s",
                       why,
                       extra ? " (\"" : "", extra ? extra : "",
                       extra ? "\")"  : "",
                       SAVE_USAGE);
    trace_err(TOOL_EINVAL, "agenda_save", "%s%s%s", why,
              extra ? " " : "", extra ? extra : "");
    return TOOL_OK;
}

static int t_agenda_save(const tool_call_t *call, tool_result_t *r) {
    if (!call || !call->input || call->input_len == 0)
        return save_fail(r, "no arguments given", NULL);

    json_value_t root;
    if (json_parse(call->input, call->input_len, &root) != JSON_OK)
        return save_fail(r, "arguments are not valid JSON", NULL);
    if (root.type != JSON_OBJECT)
        return save_fail(r, "arguments are not a JSON object", NULL);

    /* Unknown keys are refused rather than ignored. An item the model believes it
     * scheduled and the kernel read half of would run at boot, unattended, doing
     * something nobody asked for. */
    static const char *const allowed[] = {
        "name", "when", "do", "tool", "arg", "period_ms", "max_runs", "note"
    };
    size_t nmemb = json_count(&root);
    for (size_t i = 0; i < nmemb; i++) {
        json_value_t k;
        char         key[AG_WORD_MAX];
        if (json_member(&root, i, &k, NULL) != JSON_OK)
            return save_fail(r, "the arguments object could not be read", NULL);
        if (json_str(&k, key, sizeof key, NULL) != JSON_OK)
            return save_fail(r, "an argument name is not one of the accepted "
                                "fields", NULL);
        int known = 0;
        for (unsigned j = 0; j < sizeof allowed / sizeof allowed[0]; j++)
            if (ag_streq(key, allowed[j])) known = 1;
        if (!known) return save_fail(r, "unknown field", key);
    }

    agenda_item_t it;
    char          word[AG_WORD_MAX];

    for (size_t i = 0; i < sizeof it; i++) ((char *)&it)[i] = 0;
    it.enabled  = 1;
    it.max_runs = AGENDA_MAX_RUNS;
    it.period_ms = AGENDA_MIN_PERIOD_MS;

    if (ag_str(&root, "name", it.name, sizeof it.name) != 1)
        return save_fail(r, "a short lowercase \"name\" is required", NULL);

    if (ag_str(&root, "when", word, sizeof word) != 1)
        return save_fail(r, "\"when\" must be \"boot\", \"every\" or \"once\"",
                         NULL);
    if      (ag_streq(word, "boot"))  it.when = AGENDA_WHEN_BOOT;
    else if (ag_streq(word, "every")) it.when = AGENDA_WHEN_EVERY;
    else if (ag_streq(word, "once"))  it.when = AGENDA_WHEN_ONCE;
    else return save_fail(r, "\"when\" must be boot, every or once", word);

    if (ag_str(&root, "do", word, sizeof word) != 1)
        return save_fail(r, "\"do\" must be \"tool\" or \"say\"", NULL);
    if      (ag_streq(word, "tool")) it.act = AGENDA_DO_TOOL;
    else if (ag_streq(word, "say"))  it.act = AGENDA_DO_SAY;
    else return save_fail(r, "\"do\" must be tool or say", word);

    if (ag_str(&root, "tool", it.tool, sizeof it.tool) < 0)
        return save_fail(r, "\"tool\" must be a registered tool's name", NULL);
    if (ag_arg(&root, it.arg, sizeof it.arg) < 0)
        return save_fail(r,
            "\"arg\" must be a JSON object (for a tool) or a string (for a "
            "sentence), and short enough to store", NULL);
    /* Clipped, not refused — the same reasoning tools/fault_tools.c spells out
     * at its own "note" field: the note is why-this-exists prose stored beside
     * the item, and refusing a correct schedule because its comment ran long
     * costs a round trip to learn a buffer size. Every field that changes what
     * RUNS ("tool", "arg") is refused rather than clipped, immediately above. */
    {
        json_value_t nv;
        if (json_get(&root, "note", &nv) == JSON_OK) {
            if (nv.type != JSON_STRING)
                return save_fail(r, "\"note\" must be a string", NULL);
            (void)json_str(&nv, it.note, sizeof it.note, NULL);
        }
    }
    if (ag_int(&root, "period_ms", &it.period_ms) < 0)
        return save_fail(r, "\"period_ms\" must be a positive integer", NULL);
    if (ag_int(&root, "max_runs", &it.max_runs) < 0)
        return save_fail(r, "\"max_runs\" must be a positive integer", NULL);

    int existed = (agenda_find(it.name) != NULL);

    char why[AGENDA_WHY_MAX];
    int  rc = agenda_save(&it, why, sizeof why);
    if (rc != AGENDA_OK) {
        r->is_error = 1;
        tool_result_printf(r, "agenda_save refused: %s.\n%s", why, SAVE_USAGE);
        trace_err(rc, "agenda_save", "%s", it.name);
        return TOOL_OK;
    }

    tool_result_printf(r, "%s \"%s\": %s",
                       existed ? "Replaced" : "Scheduled", it.name,
                       agenda_when_name(it.when));
    if (it.when == AGENDA_WHEN_EVERY)
        tool_result_printf(r, ", every %u ms", (unsigned)it.period_ms);
    if (it.act == AGENDA_DO_TOOL)
        tool_result_printf(r, ", calling the %s tool with %s", it.tool, it.arg);
    else
        tool_result_printf(r, ", saying \"%s\" to the model", it.arg);
    tool_result_printf(r, ", at most %u time(s) this boot.\n",
                       (unsigned)it.max_runs);
    store_line(r);
    if (it.act == AGENDA_DO_SAY)
        tool_result_printf(r,
            "NOTE: a \"say\" item starts a whole model turn every time it "
            "fires, which costs an API round trip and real money. That is why "
            "an \"every\" item may not fire more often than every %u ms.\n",
            (unsigned)AGENDA_MIN_PERIOD_MS);
    tool_result_printf(r,
        "Every run of this item happens inside a fault guard, so a CPU "
        "exception inside it abandons the action instead of halting the "
        "machine, and emits a kernel trace line either way. Use agenda_control "
        "with \"run_now\" to prove it works before relying on it.\n");

    trace_ret((long)agenda_count(), "agenda_save", "%s when=%s do=%s%s%s",
              it.name, agenda_when_name(it.when), agenda_act_name(it.act),
              it.act == AGENDA_DO_TOOL ? " tool=" : "",
              it.act == AGENDA_DO_TOOL ? it.tool : "");
    return TOOL_OK;
}

static const tool_t agenda_save_tool = {
    .name        = "agenda_save",
    .description =
        "Schedule something for this machine to do WITHOUT being asked - at "
        "boot, once, or on a period - and keep it across reboots. This is the "
        "only way anything on this machine happens when nobody is speaking to "
        "it.\n"
        "An item either calls a registered tool with fixed arguments "
        "(\"do\":\"tool\") or hands a sentence to the model as if it had been "
        "typed (\"do\":\"say\"), which costs an API round trip every time it "
        "fires. The classic use is \"when I turn this machine on, reinstall the "
        "sound driver you wrote\": a boot item calling capability_call.\n"
        "Saving replaces any item of the same name and its statistics. A tool "
        "that is not registered in this build is refused now rather than "
        "failing at boot with nobody watching. Every run is fault-guarded and "
        "emits kernel trace lines.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"Short lowercase name "
        "(letters, digits, _ and -). Saving reuses it.\"},"
        "\"when\":{\"type\":\"string\",\"enum\":[\"boot\",\"every\",\"once\"],"
        "\"description\":\"boot: once at every power-on. every: on a period. "
        "once: at the next idle moment, then disables itself.\"},"
        "\"do\":{\"type\":\"string\",\"enum\":[\"tool\",\"say\"],"
        "\"description\":\"tool: dispatch a tool. say: run a model turn.\"},"
        "\"tool\":{\"type\":\"string\",\"description\":\"With do=tool: the "
        "registered tool to call, e.g. capability_call.\"},"
        "\"arg\":{\"description\":\"With do=tool: the tool's arguments as an "
        "object. With do=say: the sentence, as a string.\"},"
        "\"period_ms\":{\"type\":\"integer\",\"description\":\"With "
        "when=every: how often, in ms. At least 10000.\"},"
        "\"max_runs\":{\"type\":\"integer\",\"description\":\"Runs allowed per "
        "boot, 1..64. Defaults to 64.\"},"
        "\"note\":{\"type\":\"string\",\"description\":\"One short line saying "
        "why this exists.\"}},"
        "\"required\":[\"name\",\"when\",\"do\",\"arg\"]}",
    .flags        = TOOL_MUTATES,
    .invoke       = t_agenda_save,
};
REGISTER_TOOL(agenda_save_tool);

/* ====================================================================== */
/* agenda_list                                                            */
/* ====================================================================== */

static int t_agenda_list(const tool_call_t *call, tool_result_t *r) {
    (void)call;                      /* no arguments: nothing to misparse */
    list_body(r);
    trace_ret((long)agenda_count(), "agenda_list", "runs=%u",
              (unsigned)agenda_runs());
    return TOOL_OK;
}

static const tool_t agenda_list_tool = {
    .name        = "agenda_list",
    .description =
        "What this machine does when nobody asks it to: every scheduled agenda "
        "item, whether the schedule survives a reboot, how many times each has "
        "run this boot, what the last run actually said, and how many runs took "
        "a CPU fault. Read-only.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_agenda_list,
};
REGISTER_TOOL(agenda_list_tool);

/* ====================================================================== */
/* agenda_control                                                         */
/* ====================================================================== */

static int t_agenda_control(const tool_call_t *call, tool_result_t *r) {
    json_value_t root;
    char         action[AG_WORD_MAX];
    char         name[AGENDA_NAME_MAX];

    if (!call || !call->input || call->input_len == 0 ||
        json_parse(call->input, call->input_len, &root) != JSON_OK ||
        root.type != JSON_OBJECT) {
        r->is_error = 1;
        tool_result_printf(r,
            "agenda_control refused: expected {\"action\":\"delete|enable|"
            "disable|run_now|all_on|all_off\",\"name\":\"...\"}.\n");
        trace_err(TOOL_EINVAL, "agenda_control", "bad arguments");
        return TOOL_OK;
    }

    if (ag_str(&root, "action", action, sizeof action) != 1) {
        r->is_error = 1;
        tool_result_printf(r, "agenda_control refused: \"action\" is required "
                              "(delete, enable, disable, run_now, all_on, "
                              "all_off).\n");
        trace_err(TOOL_EINVAL, "agenda_control", "no action");
        return TOOL_OK;
    }
    int have_name = (ag_str(&root, "name", name, sizeof name) == 1);

    /* ---- the master switch: no name, and it is deliberately blunt ----
     * "Stop doing things on your own" has to be one sentence away at all times,
     * and it must not require enumerating what is scheduled. */
    if (ag_streq(action, "all_on") || ag_streq(action, "all_off")) {
        int on = ag_streq(action, "all_on");
        agenda_enable(on);
        tool_result_printf(r,
            "The agenda as a whole is now %s. %d item(s) remain scheduled; "
            "%s.\n", on ? "ON" : "OFF", agenda_count(),
            on ? "due items will run again from the next idle moment"
               : "nothing will run on its own until it is switched back on");
        list_body(r);
        trace_ok("agenda_control", "%s items=%d", action, agenda_count());
        return TOOL_OK;
    }

    if (!have_name) {
        r->is_error = 1;
        tool_result_printf(r, "agenda_control refused: \"%s\" needs the "
                              "\"name\" of an item.\n", action);
        trace_err(TOOL_EINVAL, "agenda_control", "%s no name", action);
        return TOOL_OK;
    }

    int rc;
    if (ag_streq(action, "delete")) {
        rc = agenda_remove(name);
    } else if (ag_streq(action, "enable")) {
        rc = agenda_set_enabled(name, 1);
    } else if (ag_streq(action, "disable")) {
        rc = agenda_set_enabled(name, 0);
    } else if (ag_streq(action, "run_now")) {
        /* Runs through exactly the path a tick would use: same guard, same
         * trace lines, same accounting against every bound. Proving an item
         * works must not prove that a different code path works. */
        rc = agenda_run_now(name, millis());
    } else {
        r->is_error = 1;
        tool_result_printf(r,
            "agenda_control refused: \"action\" must be delete, enable, "
            "disable, run_now, all_on or all_off, not \"%s\".\n", action);
        trace_err(TOOL_EINVAL, "agenda_control", "bad action");
        return TOOL_OK;
    }

    if (rc == AGENDA_ENOENT) {
        r->is_error = 1;
        tool_result_printf(r,
            "agenda_control refused: there is no agenda item called \"%s\".\n",
            name);
        list_body(r);
        trace_err(rc, "agenda_control", "%s %s", action, name);
        return TOOL_OK;
    }

    if (rc != AGENDA_OK) {
        /* run_now reporting a failure is NOT a tool error: the item really did
         * run, and what it returned is the answer to the question that was
         * asked. Saying "refused" would hide a successful demonstration that the
         * item is broken, which is exactly what run_now is for. */
        r->is_error = 0;
        tool_result_printf(r,
            "\"%s\" ran and did NOT succeed: %s.\n", name,
            agenda_strerror(rc));
        if (rc == AGENDA_EFAULT)
            tool_result_printf(r,
                "That was a fatal CPU exception. The machine survived it "
                "because an autonomous action runs inside a fault guard: the "
                "action's call chain was abandoned and control came back here. "
                "Call fault_report to see what faulted.\n");
    } else if (ag_streq(action, "run_now")) {
        const agenda_item_t *it = agenda_find(name);
        tool_result_printf(r, "\"%s\" ran now and succeeded.%s%s\n", name,
                           (it && it->last_why[0]) ? " It said: " : "",
                           (it && it->last_why[0]) ? it->last_why : "");
    } else {
        tool_result_printf(r, "\"%s\": %s.\n", name, action);
    }
    list_body(r);
    trace_ret((long)rc, "agenda_control", "%s %s", action, name);
    return TOOL_OK;
}

static const tool_t agenda_control_tool = {
    .name        = "agenda_control",
    .description =
        "Change or test one scheduled agenda item: \"delete\", \"enable\", "
        "\"disable\", or \"run_now\" to run it immediately through exactly the "
        "path a scheduled run uses - same fault guard, same trace lines - which "
        "is how you prove an item works before relying on it at boot. "
        "\"all_off\" stops this machine acting on its own entirely, and "
        "\"all_on\" allows it again; neither needs a name.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"action\":{\"type\":\"string\",\"enum\":[\"delete\",\"enable\","
        "\"disable\",\"run_now\",\"all_on\",\"all_off\"]},"
        "\"name\":{\"type\":\"string\",\"description\":\"Which item. Not "
        "needed for all_on/all_off.\"}},"
        "\"required\":[\"action\"]}",
    .flags        = TOOL_MUTATES,
    .invoke       = t_agenda_control,
};
REGISTER_TOOL(agenda_control_tool);

/* ====================================================================== */
/* the self-repair demonstration — diagnostic builds only                 */
/* ====================================================================== */

/* WHY THIS EXISTS, AND WHY IT IS NOT IN A NORMAL BUILD
 *
 *   "This machine repairs itself" is only worth claiming if it can be watched
 *   happening end to end on real hardware: a real bug in compiled kernel code, a
 *   real CPU exception, a real diagnosis over TLS, a real edit to .text, and a
 *   retry that then succeeds. That needs a function with a real bug in it — and a
 *   machine whose only interface is natural language must not carry one. It is
 *   the same argument arch/x86_64/fault_inject.c makes about not having a "crash
 *   yourself" verb one hallucination away from the model, and it is why this is
 *   compiled in only on request:
 *
 *       make EXTRA_CFLAGS=-DFABLEOS_REPAIR_DEMO run-nox
 *
 *   THE BUG IS THE MOST ORDINARY ONE THERE IS: a divide with no zero check. gcc
 *   emits an idiv, the divisor is zero, the CPU raises #DE, and the faulting
 *   instruction is a handful of bytes inside .text whose length the decoder can
 *   measure exactly. Any same-length replacement that does not divide removes the
 *   fault, so the demonstration does not depend on the model choosing one
 *   particular encoding — only on it understanding what it was shown. A demo that
 *   works solely when the model guesses three exact bytes is a demo of luck.
 *
 *   It is reached as a TOOL, so the whole loop runs through the real mechanism:
 *   a real agenda item, a real tool_dispatch, a real fault guard, real trace
 *   lines. There is no private back door for the demonstration to succeed
 *   through.
 */
#ifdef FABLEOS_REPAIR_DEMO

static int demo_divisor;      /* 0 to begin with: that IS the bug's trigger */
static int demo_runs;
static int demo_ok;

/* noinline so the faulting instruction has a stable address worth naming, and
 * volatile so the compiler cannot fold the division away or prove it faults. */
__attribute__((noinline))
static int demo_average(int total, int count) {
    volatile int d = count;
    return total / d;         /* the bug: nothing checks that d is non-zero */
}

static int t_repair_demo(const tool_call_t *call, tool_result_t *r) {
    (void)call;
    demo_runs++;
    kprintf("[repair-demo] attempt %d: averaging 100 over %d. If the divide is "
            "still in this kernel's .text, this faults.\n",
            demo_runs, demo_divisor);

    int v = demo_average(100, demo_divisor);

    /* Reached only if the divide no longer faults, i.e. only if something
     * rewrote it. */
    demo_ok++;
    kprintf("[repair-demo] attempt %d returned %d and did NOT fault. The "
            "instruction that used to divide by zero is not there any more.\n",
            demo_runs, v);
    tool_result_printf(r,
        "The average came back as %d without faulting, on attempt %d. This "
        "kernel's own code was repaired while it was running.\n", v, demo_runs);
    trace_ret((long)v, "repair_demo", "attempt=%d ok=%d", demo_runs, demo_ok);
    return TOOL_OK;
}

static const tool_t repair_demo_tool = {
    .name        = "repair_demo",
    .description =
        "DIAGNOSTIC BUILD ONLY. Calls a kernel function that divides by zero, "
        "to demonstrate the self-repair loop end to end. Faults until the "
        "faulting instruction is patched out of .text.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = TOOL_MUTATES,
    .invoke       = t_repair_demo,
};
REGISTER_TOOL(repair_demo_tool);

int  agenda_demo_runs(void);
int  agenda_demo_runs(void) { return demo_runs; }
int  agenda_demo_ok(void);
int  agenda_demo_ok(void)   { return demo_ok; }

#endif /* FABLEOS_REPAIR_DEMO */
