/* agent_tools.c — the tools the model needs to solve a problem, rather than to
 * touch a subsystem.
 *
 * PURPOSE
 *   Every other family in tools/ exposes a part of the machine: the VFS, the
 *   device registry, the heap, PCI config space, the driver VM. This one exposes
 *   the part of the machine that was missing, which is the *job*.
 *
 *   A multi-step request ("make a log directory, write the boot summary into it,
 *   then tell me it is there") is not hard because any single call is hard. It is
 *   hard because the state of the attempt — what the plan was, which step is
 *   done, what has actually been run — lived nowhere except in the model's own
 *   tokens. That is the one place it cannot be trusted from and the one place it
 *   does not survive: net/chat.c rolls the conversation back whenever a turn
 *   fails or runs out of rounds, so a job cut off two steps in left the next
 *   sentence with nothing to resume from. The operator then re-explains the job
 *   to a machine that has already half done it, and the half-done half is
 *   invisible.
 *
 * THE TOOL SET
 *   plan_set     write the ordered checklist for the job in hand.
 *   plan_mark    move one step to doing/done/failed, with a reason.
 *   plan_show    read the checklist back.
 *   action_log   read the KERNEL's record of the tool calls it dispatched:
 *                names, turn numbers, and whether each one actually worked.
 *   wait_ms      let bounded real time pass.
 *
 * ============================================================================
 * DECISION: the plan is kernel state, not conversation state.
 * ============================================================================
 *   It would have been cheaper to tell the model "keep your plan in your head",
 *   and that is exactly what does not survive. Three properties come from
 *   putting it in kernel memory instead:
 *
 *   1. It outlives the turn. chat.c abandons and rolls back a turn that runs out
 *      of rounds or loses the link; the plan written during that turn is still
 *      here afterwards, so "carry on" is a sentence that can work.
 *   2. It outlives eviction. The conversation arena is 16 KiB and drops whole
 *      exchanges to stay inside it (chat.h). A plan set six exchanges ago is
 *      gone from the history but not from here.
 *   3. It is attested. Writing it is a tool call, so the kernel prints a trace
 *      line for it, and the operator can see the machine commit to a plan
 *      before it starts changing things — with no shell, that bracketed line is
 *      the only place a plan could ever be visible.
 *
 *   The plan is model-authored prose and this file never interprets it: nothing
 *   dispatches on a step's text, nothing enforces the order, no step is executed
 *   because it is listed. It is a checklist, not a script — a scheduler for a
 *   machine with no scheduler would be a different and much larger thing, and it
 *   would put model text in charge of control flow. Steps are bounded, stripped
 *   to printable ASCII, and stored in fixed slots (no allocation), so a confused
 *   model can waste the twelve slots but cannot reach past them.
 *
 * ============================================================================
 * DECISION: action_log reads the kernel's record, not the model's memory.
 * ============================================================================
 *   The obvious way to answer "what have you done so far?" is for the model to
 *   summarise its own transcript. That is precisely the answer that cannot be
 *   trusted: it is the same channel as any other claim, and trace.h exists
 *   because a claim is not proof. So this reads net/chat.c's journal
 *   (chat_action_at), which the turn loop writes AFTER each dispatch returns,
 *   from the tool's own result and error flag. The model cannot write it, cannot
 *   reorder it, and cannot remove an entry it would rather not mention.
 *
 *   It is the same ground truth the operator is reading in the [bracketed] trace
 *   lines, handed to the model in a form it can reason over — which is what
 *   makes self-correction possible after a turn was cut short: the model can ask
 *   what it already did instead of guessing, and instead of doing it twice.
 *
 * ============================================================================
 * DECISION: wait_ms exists, and is capped twice.
 * ============================================================================
 *   Nothing in this kernel runs on its own: interrupts are masked, there are no
 *   timers and no scheduler, so a model asked to "check again in a second" had no
 *   way to let a second pass. mdelay() is a polled busy-wait against the TSC
 *   millisecond clock (lib/base.c), which is safe with IF=0 — it is what the
 *   drivers already use.
 *
 *   The hazard is that waiting is the one tool whose cost is wall-clock time on a
 *   machine with no way to interrupt a turn: no scheduler to preempt it, no
 *   second console, no signal. A model that called wait_ms(60000) eight times a
 *   round would hang the only interface the operator has. So there are two caps,
 *   not one: WAIT_MS_MAX per call, and WAIT_MS_PER_TURN across one operator
 *   sentence (keyed off chat_turn(), so the budget renews when the operator gets
 *   the prompt back and never mid-job). Over either, the call is refused with the
 *   remaining budget stated, which is a legible failure rather than a surprise.
 *
 * DEPENDENCIES
 *   tool.h (registration, dispatch), json.h (hostile input), trace.h (ground
 *   truth), chat.h (the action journal and the turn number), kernel.h (mdelay,
 *   millis). No hardware, no heap, no allocation: everything here is fixed
 *   storage, so the family stays usable while the heap itself is what is broken.
 *
 * FUTURE EXTENSION POINTS
 *   - A plan that survives reboot is a plan written through the VFS; the shape
 *     here (fixed steps, one state byte each) is deliberately serialisable.
 *   - action_log is one query away from "show me only the failures in turn 7",
 *     which is the shape a real audit tool would take.
 *   - A step could carry the seq of the action that satisfied it, tying the
 *     checklist to the kernel's own record instead of to the model's word.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "chat.h"
#include "kernel.h"
#include "gui.h"        /* gui_tick: keep the compositor alive during a wait  */
#include "app.h"        /* app_tick: and the periodic handlers of live apps   */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>      /* snprintf, via port/stdio.h when freestanding */

/* REGISTER_TOOL puts a pointer in the linker-collected "tool_table" section.
 * Mach-O — the host-test target on macOS — rejects a bare section name, so the
 * host build uses the "__DATA,tool_table" spelling and tests/host/test_agency.c
 * supplies the matching boundary symbols. Same shim as tools/vfs_tools.c; the
 * kernel build takes the normal path untouched. */
#if defined(FABLEOS_HOSTTEST) && defined(__APPLE__)
#  define REGISTER_AGENT_TOOL(v)                                               \
      static const tool_t *const __toolptr_##v                                 \
          __attribute__((used, section("__DATA,tool_table"))) = &(v)
#else
#  define REGISTER_AGENT_TOOL(v) REGISTER_TOOL(v)
#endif

/* ---------------------------------------------------------------------- */
/* bounds — every one of these is a fixed, static limit                    */
/* ---------------------------------------------------------------------- */

#define PLAN_STEPS      12      /* steps in one plan                        */
#define PLAN_TEXT       96      /* bytes of one step's text, NUL included   */
#define PLAN_NOTE       80      /* bytes of one step's note                 */
#define PLAN_GOAL      128      /* bytes of the plan's one-line goal        */

#define WAIT_MS_MAX     2000    /* one wait_ms call                         */
#define WAIT_MS_PER_TURN 5000   /* all wait_ms calls in one operator turn   */

#define LOG_ROWS_MAX    CHAT_JOURNAL_ENTRIES
#define ERR_MAX         320     /* one model-readable failure sentence      */

/* ---------------------------------------------------------------------- */
/* failure reporting — one place, so every error is both spoken and traced  */
/* ---------------------------------------------------------------------- */

/* Turns a recoverable failure into (a) a sentence the model can act on and
 * (b) the kernel's ground-truth trace line. Always returns `err`. */
static int fail(tool_result_t *r, const char *op, const char *targs, int err,
                const char *fmt, ...) {
    va_list ap;
    char    msg[ERR_MAX];

    r->is_error = 1;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    tool_result_printf(r, "%s", msg);
    trace_err(err, op, "%s", targs ? targs : "");
    return err;
}

/* An argument was rejected before anything was identified, so the trace line
 * carries the reason instead: `[plan_mark bad-args: ... -> EINVAL]`. */
static int fail_args(tool_result_t *r, const char *op, const char *why) {
    char targs[ERR_MAX + 16];
    snprintf(targs, sizeof targs, "bad-args: %s", why);
    return fail(r, op, targs, TOOL_EINVAL, "%s", why);
}

/* ---------------------------------------------------------------------- */
/* argument decoding — the input span is model output, i.e. hostile        */
/* ---------------------------------------------------------------------- */

/* Static storage: the span must outlive this call. Used when the model sends no
 * input at all, so a missing argument is reported as missing rather than as a
 * parse error. */
static const char empty_object[] = "{}";

static int parse_input(const tool_call_t *c, json_value_t *out,
                       char *err, size_t errcap) {
    if (!c || !c->input || c->input_len == 0) {
        out->type  = JSON_OBJECT;
        out->start = empty_object;
        out->len   = 2;
        return 0;
    }
    if (json_parse(c->input, c->input_len, out) != JSON_OK) {
        snprintf(err, errcap, "tool input is not valid JSON");
        return -1;
    }
    if (out->type != JSON_OBJECT) {
        snprintf(err, errcap, "tool input must be a JSON object");
        return -1;
    }
    return 0;
}

/* Copy model text into a fixed slot as one line of printable ASCII.
 *
 * Control bytes are dropped rather than escaped, and a newline ends the copy: a
 * step's text and a note are echoed back by plan_show and could otherwise carry
 * a '[' to the start of a line. trace.h's invariant is that a kernel trace line
 * is a '[' in column zero, and this text reaches the console (through a tool
 * result the operator never sees, but also through the trace argument, which
 * lib/trace.c escapes). Brackets are folded to braces here so that even a future
 * printer of this text cannot be talked into producing one. Returns the bytes
 * kept. */
static size_t copy_line(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    if (cap == 0) return 0;
    for (size_t i = 0; src && src[i] && j + 1 < cap; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\n' || ch == '\r') break;
        if (ch < 0x20 || ch >= 0x7F) continue;
        if (ch == '[') ch = '{';
        if (ch == ']') ch = '}';
        dst[j++] = (char)ch;
    }
    while (j && dst[j - 1] == ' ') j--;
    dst[j] = '\0';
    return j;
}

/* Decode an optional string argument into a fixed slot. Missing is not an error
 * (0 with an empty destination); present-but-wrong is. */
static int arg_opt_str(const json_value_t *in, const char *key,
                       char *dst, size_t cap, char *err, size_t errcap) {
    json_value_t v;
    /* Decoded at the family's widest slot, then measured against the caller's
     * cap. Decoding straight into `dst` would silently truncate a value that is
     * merely too long for THIS argument, and a silently shortened state name or
     * goal is a wrong answer the model has no way to notice. */
    char   raw[PLAN_GOAL > PLAN_TEXT ? PLAN_GOAL : PLAN_TEXT];
    size_t rawlen = 0;

    dst[0] = '\0';
    int rc = json_get(in, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK || v.type != JSON_STRING) {
        snprintf(err, errcap, "argument \"%s\" must be a string", key);
        return -1;
    }
    rc = json_str(&v, raw, sizeof raw, &rawlen);
    if (rc == JSON_ENOSPC || (rc == JSON_OK && rawlen >= cap)) {
        snprintf(err, errcap, "argument \"%s\" is too long (limit %d bytes)",
                 key, (int)(cap - 1));
        return -1;
    }
    if (rc != JSON_OK) {
        snprintf(err, errcap, "argument \"%s\" is not a decodable string", key);
        return -1;
    }
    copy_line(dst, cap, raw);
    return 0;
}

/* Decode an optional integer argument with an inclusive range. */
static int arg_opt_int(const json_value_t *in, const char *key, int64_t lo,
                       int64_t hi, int64_t dflt, int64_t *out,
                       char *err, size_t errcap) {
    json_value_t v;
    int64_t      n = 0;

    *out = dflt;
    int rc = json_get(in, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK || json_int(&v, &n) != JSON_OK) {
        snprintf(err, errcap, "argument \"%s\" must be a whole number", key);
        return -1;
    }
    if (n < lo || n > hi) {
        /* The kernel's vsnprintf has %d but no 64-bit conversion, so an absurd
         * value is described rather than printed: casting it to int would report
         * a number the model never sent. */
        if (n > -1000000 && n < 1000000)
            snprintf(err, errcap,
                     "argument \"%s\" is %d, outside the accepted range %d..%d",
                     key, (int)n, (int)lo, (int)hi);
        else
            snprintf(err, errcap,
                     "argument \"%s\" is far outside the accepted range %d..%d",
                     key, (int)lo, (int)hi);
        return -1;
    }
    *out = n;
    return 0;
}

/* ====================================================================== */
/* the plan                                                                */
/* ====================================================================== */

#define ST_TODO   0
#define ST_DOING  1
#define ST_DONE   2
#define ST_FAILED 3

static const char *state_name(unsigned s) {
    switch (s) {
        case ST_DOING:  return "doing";
        case ST_DONE:   return "done";
        case ST_FAILED: return "failed";
        default:        return "todo";
    }
}

static int state_from_name(const char *s) {
    for (unsigned i = ST_TODO; i <= ST_FAILED; i++) {
        const char *n = state_name(i);
        size_t      k = 0;
        while (n[k] && n[k] == s[k]) k++;
        if (n[k] == '\0' && s[k] == '\0') return (int)i;
    }
    return -1;
}

typedef struct {
    char    text[PLAN_TEXT];
    char    note[PLAN_NOTE];
    uint8_t state;
} plan_step_t;

static plan_step_t plan[PLAN_STEPS];
static unsigned    plan_n;                 /* steps in use */
static char        plan_goal[PLAN_GOAL];
static unsigned    plan_turn;              /* operator turn it was written in */
static unsigned    plan_writes;            /* plan_set calls since boot */

/* Count of steps in one state, for the summary line and the trace argument. */
static unsigned plan_count(unsigned state) {
    unsigned n = 0;
    for (unsigned i = 0; i < plan_n; i++) if (plan[i].state == state) n++;
    return n;
}

/* ---- plan_set ---- */

static int t_plan_set(const tool_call_t *c, tool_result_t *r) {
    json_value_t in, steps;
    char         err[ERR_MAX];
    char         goal[PLAN_GOAL];

    if (parse_input(c, &in, err, sizeof err) < 0)
        return fail_args(r, "plan_set", err);

    int rc = json_get(&in, "steps", &steps);
    if (rc != JSON_OK || steps.type != JSON_ARRAY)
        return fail_args(r, "plan_set",
                         "missing required argument \"steps\" (an array of short "
                         "strings, one per step, in the order you will do them)");

    size_t n = json_count(&steps);
    if (n == 0)
        return fail_args(r, "plan_set",
                         "\"steps\" is empty; a plan needs at least one step "
                         "(pass one step to record a single action)");
    if (n > PLAN_STEPS)
        return fail(r, "plan_set", "too-many-steps", TOOL_EINVAL,
                    "%d steps is more than this machine keeps (limit %d). Fold "
                    "the small ones together and plan the rest after.",
                    (int)n, PLAN_STEPS);

    if (arg_opt_str(&in, "goal", goal, sizeof goal, err, sizeof err) < 0)
        return fail_args(r, "plan_set", err);

    /* Validate every step BEFORE overwriting anything: a plan half replaced is
     * worse than a plan refused, because the model would believe the new one. */
    static char decoded[PLAN_STEPS][PLAN_TEXT];
    for (size_t i = 0; i < n; i++) {
        json_value_t s;
        char         raw[PLAN_TEXT];

        if (json_at(&steps, i, &s) != JSON_OK || s.type != JSON_STRING)
            return fail(r, "plan_set", "bad-step", TOOL_EINVAL,
                        "step %d is not a string; every step must be a short "
                        "sentence saying what you will do.", (int)i + 1);
        int srr = json_str(&s, raw, sizeof raw, (size_t *)0);
        if (srr == JSON_ENOSPC)
            return fail(r, "plan_set", "step-too-long", TOOL_EINVAL,
                        "step %d is longer than %d bytes; say it shorter or "
                        "split it in two.", (int)i + 1, (int)(PLAN_TEXT - 1));
        if (srr != JSON_OK)
            return fail(r, "plan_set", "bad-step", TOOL_EINVAL,
                        "step %d is not a decodable string.", (int)i + 1);
        if (copy_line(decoded[i], PLAN_TEXT, raw) == 0)
            return fail(r, "plan_set", "empty-step", TOOL_EINVAL,
                        "step %d is empty once control characters are removed; "
                        "every step needs readable text.", (int)i + 1);
    }

    for (size_t i = 0; i < n; i++) {
        copy_line(plan[i].text, PLAN_TEXT, decoded[i]);
        plan[i].note[0] = '\0';
        plan[i].state   = ST_TODO;
    }
    plan_n = (unsigned)n;
    copy_line(plan_goal, sizeof plan_goal, goal);
    plan_turn = chat_turn();
    plan_writes++;

    tool_result_printf(r, "plan recorded, %d steps, all todo%s%s\n",
                       (int)plan_n, plan_goal[0] ? ", goal: " : "", plan_goal);
    for (unsigned i = 0; i < plan_n; i++)
        tool_result_printf(r, "%d. [todo] %s\n", (int)i + 1, plan[i].text);
    tool_result_printf(r, "Mark each step with plan_mark as you go; plan_show "
                          "reads this back in a later turn.");

    trace_ret((long)plan_n, "plan_set", "steps=%d goal=%s",
              (int)plan_n, plan_goal[0] ? plan_goal : "(none)");
    return TOOL_OK;
}

static const tool_t plan_set_tool = {
    .name        = "plan_set",
    .description =
        "Record the ordered checklist for the job you are doing, before you "
        "start. Steps are kept in kernel memory, so they survive a turn that "
        "runs out of tool rounds or loses the connection: a job cut off halfway "
        "can be resumed instead of restarted. Replaces any previous plan. Up to "
        "12 steps. Nothing is executed by listing it.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
        "\"description\":\"one short sentence per step, in order\"},"
        "\"goal\":{\"type\":\"string\",\"description\":"
        "\"one line saying what done looks like\"}},"
        "\"required\":[\"steps\"]}",
    .flags       = TOOL_MUTATES,
    .invoke      = t_plan_set,
};
REGISTER_AGENT_TOOL(plan_set_tool);

/* ---- plan_mark ---- */

static int t_plan_mark(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    char         err[ERR_MAX];
    char         want[16];
    char         note[PLAN_NOTE];
    int64_t      step = 0;

    if (parse_input(c, &in, err, sizeof err) < 0)
        return fail_args(r, "plan_mark", err);

    if (plan_n == 0)
        return fail(r, "plan_mark", "no-plan", TOOL_EINVAL,
                    "there is no plan to mark; call plan_set first");

    json_value_t probe;
    if (json_get(&in, "step", &probe) != JSON_OK)
        return fail_args(r, "plan_mark",
                         "missing required argument \"step\" (the 1-based step "
                         "number from plan_show)");
    if (arg_opt_int(&in, "step", 1, (int64_t)plan_n, 1, &step,
                    err, sizeof err) < 0)
        return fail_args(r, "plan_mark", err);

    if (arg_opt_str(&in, "state", want, sizeof want, err, sizeof err) < 0)
        return fail_args(r, "plan_mark", err);
    if (want[0] == '\0')
        return fail_args(r, "plan_mark",
                         "missing required argument \"state\" (todo, doing, done "
                         "or failed)");
    int st = state_from_name(want);
    if (st < 0)
        return fail(r, "plan_mark", "bad-state", TOOL_EINVAL,
                    "unknown state \"%s\"; use todo, doing, done or failed",
                    want);

    if (arg_opt_str(&in, "note", note, sizeof note, err, sizeof err) < 0)
        return fail_args(r, "plan_mark", err);

    /* A step marked done or failed with no reason is the case where the model is
     * most likely to be guessing, so the note is required for exactly those two
     * and optional otherwise. It costs one short phrase and it is what makes the
     * plan readable to the operator afterwards. */
    if ((st == ST_DONE || st == ST_FAILED) && note[0] == '\0')
        return fail(r, "plan_mark", "no-note", TOOL_EINVAL,
                    "marking a step %s needs a \"note\" saying how you know - "
                    "for done, what you read back to confirm it; for failed, "
                    "what the error said", state_name((unsigned)st));

    plan_step_t *s    = &plan[step - 1];
    const char  *from = state_name(s->state);

    s->state = (uint8_t)st;
    copy_line(s->note, sizeof s->note, note);

    tool_result_printf(r, "step %d of %d: %s -> %s (%s)\n",
                       (int)step, (int)plan_n, from, state_name((unsigned)st),
                       s->text);
    if (s->note[0]) tool_result_printf(r, "note: %s\n", s->note);
    tool_result_printf(r, "plan now: %d done, %d failed, %d doing, %d todo",
                       (int)plan_count(ST_DONE), (int)plan_count(ST_FAILED),
                       (int)plan_count(ST_DOING), (int)plan_count(ST_TODO));

    trace_ok("plan_mark", "step=%d/%d %s->%s", (int)step, (int)plan_n, from,
             state_name((unsigned)st));
    return TOOL_OK;
}

static const tool_t plan_mark_tool = {
    .name        = "plan_mark",
    .description =
        "Move one step of the current plan to todo, doing, done or failed. "
        "Marking a step done or failed requires a note: for done, what you read "
        "back to confirm it; for failed, what the kernel said. Use it as you go "
        "so a later turn can see where the job actually got to.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"step\":{\"type\":\"integer\",\"description\":\"1-based step number\"},"
        "\"state\":{\"type\":\"string\",\"enum\":"
        "[\"todo\",\"doing\",\"done\",\"failed\"]},"
        "\"note\":{\"type\":\"string\",\"description\":"
        "\"evidence, or the error text\"}},"
        "\"required\":[\"step\",\"state\"]}",
    .flags       = TOOL_MUTATES,
    .invoke      = t_plan_mark,
};
REGISTER_AGENT_TOOL(plan_mark_tool);

/* ---- plan_show ---- */

static int t_plan_show(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    char         err[ERR_MAX];

    if (parse_input(c, &in, err, sizeof err) < 0)
        return fail_args(r, "plan_show", err);

    if (plan_n == 0) {
        tool_result_printf(r, "no plan recorded. Use plan_set when a request "
                              "needs more than one step.");
        trace_ret(0, "plan_show", "steps=0");
        return TOOL_OK;
    }

    unsigned now = chat_turn();
    tool_result_printf(r, "plan set in turn %u (this is turn %u), %d steps%s%s\n",
                       plan_turn, now, (int)plan_n,
                       plan_goal[0] ? ", goal: " : "", plan_goal);
    for (unsigned i = 0; i < plan_n; i++) {
        tool_result_printf(r, "%d. [%s] %s", (int)i + 1,
                           state_name(plan[i].state), plan[i].text);
        if (plan[i].note[0]) tool_result_printf(r, " -- %s", plan[i].note);
        tool_result_printf(r, "\n");
    }
    tool_result_printf(r, "%d done, %d failed, %d doing, %d todo",
                       (int)plan_count(ST_DONE), (int)plan_count(ST_FAILED),
                       (int)plan_count(ST_DOING), (int)plan_count(ST_TODO));

    trace_ret((long)plan_n, "plan_show", "steps=%d done=%d turn=%u",
              (int)plan_n, (int)plan_count(ST_DONE), plan_turn);
    return TOOL_OK;
}

static const tool_t plan_show_tool = {
    .name        = "plan_show",
    .description =
        "Read the current plan back: every step, its state and its note, and "
        "which operator turn the plan was written in. This is how a turn that "
        "was cut off is resumed - read the plan, see what is still todo, and "
        "carry on from there.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_plan_show,
};
REGISTER_AGENT_TOOL(plan_show_tool);

/* ====================================================================== */
/* action_log — the kernel's record of what it dispatched                  */
/* ====================================================================== */

static int t_action_log(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    char         err[ERR_MAX];
    int64_t      limit = 0;
    int64_t      failed_only = 0;
    json_value_t v;

    if (parse_input(c, &in, err, sizeof err) < 0)
        return fail_args(r, "action_log", err);
    if (arg_opt_int(&in, "limit", 1, LOG_ROWS_MAX, LOG_ROWS_MAX, &limit,
                    err, sizeof err) < 0)
        return fail_args(r, "action_log", err);
    if (json_get(&in, "failed_only", &v) == JSON_OK) {
        int b = 0;
        if (json_bool(&v, &b) != JSON_OK)
            return fail_args(r, "action_log",
                             "argument \"failed_only\" must be true or false");
        failed_only = b;
    }

    unsigned kept  = chat_action_count();
    unsigned total = chat_action_total();

    if (kept == 0) {
        tool_result_printf(r, "no tool calls recorded since boot (or since the "
                              "link to the model was re-established)");
        trace_ret(0, "action_log", "rows=0 total=%u", total);
        return TOOL_OK;
    }

    /* Newest `limit` entries, in the order they ran. */
    unsigned first = (kept > (unsigned)limit) ? kept - (unsigned)limit : 0;
    unsigned shown = 0;

    tool_result_printf(r, "%u call(s) dispatched since boot; the kernel keeps "
                          "the last %u. Oldest first:\n", total, kept);
    for (unsigned i = first; i < kept; i++) {
        const chat_action_t *a = chat_action_at(i);
        if (!a) break;
        if (failed_only && !a->failed) continue;
        tool_result_printf(r, "#%u turn=%u %s %s: %s\n",
                           a->seq, (unsigned)a->turn, a->name,
                           a->failed ? "FAILED" : "ok",
                           a->detail[0] ? a->detail : "(no detail)");
        shown++;
    }
    if (shown == 0)
        tool_result_printf(r, "(none matched)\n");
    tool_result_printf(r, "This is the kernel's own record, written after each "
                          "call returned - not a summary of what was said.");

    trace_ret((long)shown, "action_log", "rows=%u kept=%u total=%u failed_only=%d",
              shown, kept, total, (int)failed_only);
    return TOOL_OK;
}

static const tool_t action_log_tool = {
    .name        = "action_log",
    .description =
        "Read the kernel's record of the tool calls it has dispatched: name, "
        "operator turn, whether each one actually worked, and the first line of "
        "its result. Written by the kernel after each call returned, so it is "
        "evidence rather than recollection - and it survives a turn that was "
        "abandoned. Use it to find out what was already done before retrying "
        "something, or to answer \"what have you done so far?\" truthfully.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"limit\":{\"type\":\"integer\",\"description\":"
        "\"newest N entries (default all kept)\"},"
        "\"failed_only\":{\"type\":\"boolean\",\"description\":"
        "\"only calls that did not act\"}}}",
    .flags       = 0,
    .invoke      = t_action_log,
};
REGISTER_AGENT_TOOL(action_log_tool);

/* ====================================================================== */
/* wait_ms — bounded real time                                             */
/* ====================================================================== */

static unsigned wait_turn;      /* the operator turn the budget belongs to */
static unsigned wait_spent;     /* milliseconds already waited in it       */

static int t_wait_ms(const tool_call_t *c, tool_result_t *r) {
    json_value_t in, probe;
    char         err[ERR_MAX];
    int64_t      ms = 0;

    if (parse_input(c, &in, err, sizeof err) < 0)
        return fail_args(r, "wait_ms", err);
    if (json_get(&in, "ms", &probe) != JSON_OK)
        return fail_args(r, "wait_ms",
                         "missing required argument \"ms\" (milliseconds to "
                         "wait, 1 to 2000)");
    if (arg_opt_int(&in, "ms", 1, WAIT_MS_MAX, 1, &ms, err, sizeof err) < 0)
        return fail_args(r, "wait_ms", err);

    /* The budget renews when the operator turn does, never mid-job. */
    unsigned turn = chat_turn();
    if (turn != wait_turn) { wait_turn = turn; wait_spent = 0; }

    unsigned left = (WAIT_MS_PER_TURN > wait_spent)
                        ? WAIT_MS_PER_TURN - wait_spent : 0;
    if ((unsigned)ms > left)
        return fail(r, "wait_ms", "budget-spent", TOOL_EINVAL,
                    "only %u ms of the %u ms wait budget is left for this "
                    "sentence (%u ms already waited). This machine has no "
                    "scheduler, so waiting blocks the only interface there is - "
                    "read the state instead, or ask the operator to say it "
                    "again if it really needs longer.",
                    left, (unsigned)WAIT_MS_PER_TURN, wait_spent);

    /* WAIT, BUT KEEP THE MACHINE ALIVE WHILE WAITING.
     *
     * A flat mdelay() is a stopped world, and that turned out to matter the
     * first time a model used this tool to CHECK ITS OWN WORK. The system
     * prompt tells it to read every change back, so it launched a clock with a
     * periodic handler, called wait_ms(1500), read the app's state — and found
     * the clock frozen, because app_tick() is driven from the loop that waits
     * for the operator's next sentence and that loop is not running inside a
     * turn. The kernel's own verification path was reporting a working app as
     * broken, and the model (correctly, on that evidence) started rebuilding
     * it. Waiting is the one place in a turn where time is deliberately being
     * spent, so it is exactly where periodic work belongs.
     *
     * Pumped in WAIT_SLICE_MS slices. app_tick() returns immediately when no
     * app is periodic, gui_tick() when no window is open (gui.h), and both are
     * bounded — so this stays a bounded wait, and the elapsed time is still
     * MEASURED below rather than assumed.
     *
     * THE LOOP COUNTS SLICES, IT DOES NOT WATCH THE CLOCK. `while (millis() <
     * deadline)` reads better and is wrong here: on the host mdelay() is a
     * no-op shim while millis() is a real monotonic clock, so that spelling
     * turns every wait_ms in the test suite into a real busy-spin — it added
     * six seconds to `make test-host` for no coverage. A fixed slice count runs
     * ms/WAIT_SLICE_MS iterations either way: the full wait on hardware, and
     * the same number of app_tick() calls, instantly, on the host. */
#define WAIT_SLICE_MS 10
    uint64_t before = millis();
    for (uint32_t done = 0; done < (uint32_t)ms; done += WAIT_SLICE_MS) {
        (void)app_tick();
        gui_tick();
        uint32_t slice = (uint32_t)ms - done;
        mdelay(slice > WAIT_SLICE_MS ? WAIT_SLICE_MS : slice);
    }
    uint64_t after = millis();

    wait_spent += (unsigned)ms;

    /* The elapsed time is measured, not assumed: mdelay() polls a TSC-derived
     * clock and the operator's proof is the real delta, not the request. */
    tool_result_printf(r, "waited %u ms (clock moved %u ms, now %u ms since "
                          "boot). %u ms of the %u ms budget left this sentence.",
                       (unsigned)ms, (unsigned)(after - before),
                       (unsigned)after, WAIT_MS_PER_TURN - wait_spent,
                       (unsigned)WAIT_MS_PER_TURN);

    trace_ret((long)(after - before), "wait_ms", "requested=%u spent=%u",
              (unsigned)ms, wait_spent);
    return TOOL_OK;
}

static const tool_t wait_ms_tool = {
    .name        = "wait_ms",
    .description =
        "Let real time pass before reading state again - the only way to do so "
        "on a machine where nothing runs in the background. Blocks everything "
        "while it waits: 2000 ms per call, 5000 ms per operator sentence. Use it "
        "when hardware needs to settle between a write and a read-back, not to "
        "pass time for its own sake.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"ms\":{\"type\":\"integer\",\"description\":"
        "\"milliseconds, 1 to 2000\"}},\"required\":[\"ms\"]}",
    .flags       = 0,
    .invoke      = t_wait_ms,
};
REGISTER_AGENT_TOOL(wait_ms_tool);
