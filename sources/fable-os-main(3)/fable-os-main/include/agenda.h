/* agenda.h — what this machine does when nobody has asked it to do anything.
 *
 * PURPOSE
 *   Everything else in this kernel is reactive. A tool runs because the model
 *   called it; the model ran because a human typed a sentence. Pull the keyboard
 *   out and the machine is a very capable statue. That is the last thing standing
 *   between "an operating system you talk to" and "an operating system that is
 *   awake": it cannot act on its own, and it cannot pick up where it left off,
 *   because the only thing that ever starts anything is a person.
 *
 *   An agenda is a small, bounded, persistent list of things the machine does
 *   without being asked — at boot, once, or on a period. Each item names an
 *   action the machine already knows how to perform; the agenda supplies only the
 *   WHEN, the bounds, and the evidence.
 *
 *   The use the operator actually hit: a driver the model wrote at run time is
 *   gone after a power cycle, and re-teaching it means saying so again every
 *   time. With the capability store keeping the driver and an agenda item calling
 *   it at boot, the machine brings its own sound card up before anyone logs in.
 *   Nothing in this file knows anything about audio; it knows how to call a tool
 *   at boot, and "reinstall my audio driver" is a row in a table.
 *
 * ============================================================================
 * THE FOUR THINGS THAT MAKE AN UNATTENDED ACTION ACCEPTABLE
 * ============================================================================
 *
 *   1. IT CANNOT HALT THE MACHINE. Every action runs inside a fault guard (see
 *      include/fault.h). A fatal CPU exception inside an autonomous action
 *      abandons that action's call chain and returns here, where it is recorded
 *      as a failure, counted against the item, and — if there is a transport —
 *      handed to net/faultchat.c to be diagnosed. An unattended machine that
 *      halts is an unattended machine that is dead until somebody notices, which
 *      may be days. This is the single most important property here, and it is
 *      also what closes the self-repair loop: something has to be running the
 *      code that faults, in a context that can survive it, for a diagnosis to be
 *      possible at all.
 *
 *   2. IT IS BOUNDED IN EVERY DIRECTION THAT COSTS ANYTHING. Not one bound but
 *      six, because an autonomous loop can be paid for in six different
 *      currencies and any single bound leaves the other five open:
 *        AGENDA_MAX_ITEMS         how many things may be scheduled at all
 *        AGENDA_MIN_PERIOD_MS     how often the fastest of them may fire
 *        item->max_runs           how many times one item may run per boot
 *        AGENDA_MAX_RUNS_TOTAL    how many autonomous actions a boot may take
 *        AGENDA_MAX_FAILURES      consecutive failures before an item is retired
 *        one action per tick      so a due backlog cannot starve the operator
 *      An item that hits its own bound is DISABLED with the reason recorded, not
 *      silently skipped: a schedule that quietly stopped working is worse than
 *      one that says it has stopped. Two of those disables are PERSISTED and two
 *      deliberately are not — see agenda_save()'s note on durable retirement.
 *
 *      A SEVENTH BOUND, added after the first six turned out to share a blind
 *      spot: an item may not name a tool whose SUCCESS ends the boot (today
 *      power_off and power_reboot). All six bounds above limit repetition or
 *      cost, and none of them looks at one action that works exactly as asked and
 *      leaves no machine running to be told about it. A when=boot power_off runs
 *      before the prompt is printed, and the store survives the power cycle, so
 *      the machine switches itself off during every boot with no interface left
 *      to countermand it. Refused in validate(), which the store loader also
 *      calls, so a store written by hand or by another build is disarmed on the
 *      way in. See core/agenda.c's ends_the_boot() for why this is a name list
 *      and what should replace it.
 *
 *   3. IT LEAVES EVIDENCE, MORE THAN A HUMAN-DRIVEN ACTION DOES. Every run emits
 *      a kernel trace line before and after — [agenda.run ...] and the tool's own
 *      [<tool> ... -> ...] — so the console record of an unattended action is
 *      strictly larger than the record of one somebody watched. There is no shell
 *      on this machine and nobody was present; the log is the entire account of
 *      what happened while the operator was away, and per include/trace.h these
 *      lines are emitted by the kernel in C from real return values and cannot be
 *      authored by the model.
 *
 *   4. IT SURVIVES A REBOOT, OR IT IS NOT WORTH HAVING. An agenda that has to be
 *      re-dictated after every power cycle solves nothing. Items are stored as
 *      one JSON document in the filesystem — on the disk when there is one, in
 *      RAM when there is not, and the tools say which so nobody is misled about
 *      what will still be there tomorrow.
 *
 * WHAT AN ACTION CAN BE, AND WHY IT IS EXACTLY THESE TWO
 *   AGENDA_DO_TOOL   dispatch a registered tool with a literal JSON input.
 *                    This is the general case and deliberately so: the tool table
 *                    IS this machine's syscall surface, so anything the model can
 *                    do in a conversation can be scheduled, including calling a
 *                    saved capability, installing a driver, writing a file or
 *                    reverting a code patch. No new mechanism, no second
 *                    permission model, and the same validation and the same trace
 *                    line as a model-issued call — because it IS a
 *                    tool_dispatch(), through the one door.
 *   AGENDA_DO_SAY    hand a sentence to the turn loop, as if it had been typed.
 *                    The open-ended case: the machine asks itself to do something
 *                    described in words, and the model works out how. This is the
 *                    one that makes "I should be able to ask it anything" true
 *                    when there is nobody there to ask. It costs an API round trip
 *                    every time it fires, which is why the period floor exists and
 *                    why the tools state the cost when such an item is saved.
 *
 *   Reached through a bound callback, not a direct call, so this module links
 *   against neither net/chat.c nor the transport and can be host-tested with a
 *   recording stub. core/tool.c IS called directly: a tool dispatch is the thing
 *   an agenda item fundamentally is, and there is no version of this module worth
 *   testing that fakes it.
 *
 * WHERE THE TICK COMES FROM
 *   Nowhere in here. agenda_tick(now_ms) is called by kernel/main.c from the idle
 *   path — the point where the machine is waiting for a sentence that may never
 *   come, and is therefore both genuinely idle and, crucially, still running. It
 *   is cheap when nothing is due (one compare per item) and it never blocks:
 *   an item that is due is run to completion, one per tick, and the loop goes
 *   back to polling the keyboard.
 *
 *   Deliberately NOT a timer interrupt. IF is 0 on this machine and every device
 *   is polled; a preemptive agenda would need locking around every structure an
 *   action touches, in a kernel that has none. Cooperative and late beats
 *   preemptive and racy — the same argument core/fiber.c makes.
 *
 * ORDERING AT BOOT
 *   agenda_bringup() loads the store and runs the boot items immediately, in the
 *   order they were saved. It must be called after the filesystem (the store is a
 *   file in it) and after the transport is bound (a boot item may be a sentence),
 *   and its actions are the first things this machine does on its own.
 *
 * PUBLIC API
 *   Lifecycle     agenda_bringup, agenda_bind_say, agenda_reset, agenda_enable.
 *   Editing       agenda_save, agenda_remove, agenda_set_enabled, agenda_run_now.
 *   Running       agenda_tick, agenda_run_boot, agenda_due.
 *   Reading       agenda_count, agenda_at, agenda_find, agenda_report,
 *                 agenda_runs, agenda_failures, agenda_escapes, agenda_store,
 *                 agenda_persistent, agenda_strerror, agenda_when_name,
 *                 agenda_act_name.
 *   Persistence   agenda_load, agenda_flush. Both are called for you; they are
 *                 exposed because a test needs to prove a reboot.
 *
 * DEPENDENCIES
 *   fault.h (the guard), tool.h (the one door), trace.h (the evidence), json.h
 *   (all parsing — no new parser is written here), vfs.h + fs.h (the store),
 *   kernel.h (kputs/kprintf/millis). NOT net/chat.c and NOT any transport: see
 *   agenda_bind_say.
 *
 * FUTURE EXTENSION POINTS
 *   A trigger keyed on an event rather than a clock — "when a disk appears",
 *   "when the model becomes reachable" — which is a predicate function pointer
 *   and a row in the when table. A per-item spend cap once model calls are
 *   metered. Backoff instead of retirement after a failure, which needs a notion
 *   of a transient failure that tool results do not currently carry.
 */
#ifndef AGENDA_H
#define AGENDA_H

#include <stdint.h>
#include <stddef.h>

/* For TOOL_NAME_MAX, which AGENDA_TOOL_MAX is derived from rather than mirroring.
 * The field below holds a tool name, so the tool layer is where its size comes
 * from; see AGENDA_TOOL_MAX. */
#include "tool.h"

/* ---- result codes (0 = ok) ---- */
#define AGENDA_OK          0
#define AGENDA_EINVAL    -22    /* malformed item                            */
#define AGENDA_ENOENT     -2    /* no item by that name                      */
#define AGENDA_ENOSPC    -28    /* the table is full                         */
#define AGENDA_EEXIST    -17    /* refused rather than silently replaced      */
#define AGENDA_EIO        -5    /* the store could not be read or written     */
#define AGENDA_EBUSY     -16    /* an action, or a model turn, is already running */
#define AGENDA_EOFF      -1     /* the whole agenda is switched off           */
#define AGENDA_ENOTOOL   -70    /* the named tool is not registered           */
#define AGENDA_ENOSAY    -71    /* a sentence item, and nothing to say it to   */
/* A "say" item's turn was started and did not complete. Distinct from
 * AGENDA_EIO on purpose: this failure is in the MODEL TRANSPORT and has nothing
 * to do with the store. Folding the two together told a live model the disk was
 * broken when it was not, and it reasoned correctly from that false fact for six
 * rounds and two paid API calls before delivering a degraded workaround. An
 * error code a model reads is part of the interface. */
#define AGENDA_ESAY      -73
#define AGENDA_EFAULT   -104    /* the action took a fatal CPU exception       */
#define AGENDA_ERETIRED  -11    /* the item is disabled or out of runs         */
#define AGENDA_EDENIED   -13    /* the tool's success would end the boot       */

/* ---- bounds. Every one is .bss; nothing here allocates. ---- */

#define AGENDA_MAX_ITEMS        8
#define AGENDA_NAME_MAX        32     /* a name a human would say out loud    */
/* DERIVED, NOT GUESSED, and it used to be guessed.
 *
 * This was 40, with the comment "longest registered tool name + slack". The
 * longest tool name in the tree is 17 characters, so it looked generous — but
 * TOOL_NAME_MAX is 48, so a tool the tool layer considers perfectly legal could
 * not be scheduled, and the failure mode is a stored item silently refusing to
 * load with "its tool is longer than 39 characters" on a machine where the tool
 * itself works fine. A hardcoded number mirroring a computed one, of exactly the
 * kind that has turned the QEMU cases red twice.
 *
 * So the bound IS the tool layer's bound, plus the NUL this field needs and
 * tool.h's does not count. Nothing to keep in step, and the _Static_assert below
 * fails the build rather than the machine if that relationship is ever broken.
 * agenda_bounds_ok() additionally walks the live registry at boot, because
 * TOOL_NAME_MAX is a promise about names and not an enforcement of them. */
#define AGENDA_TOOL_MAX        (TOOL_NAME_MAX + 1)
#define AGENDA_ARG_MAX        288     /* a tool's JSON input, or a sentence   */
#define AGENDA_WHY_MAX        128     /* the last outcome, in words           */
#define AGENDA_NOTE_MAX       112     /* why this item exists, in the model's */

/* The floor on how often an item may fire. Ten seconds is not a performance
 * number: a "say" item costs an API round trip and some of the operator's money
 * every time it runs, and an item that fires faster than a human can read the
 * console is indistinguishable from a runaway loop. */
#define AGENDA_MIN_PERIOD_MS  10000u

/* Runs one item may take per boot, unless it says less. */
#define AGENDA_MAX_RUNS       64u

/* Autonomous actions a whole boot may take, across every item. The per-item cap
 * bounds one schedule; this bounds the machine, which is the quantity that
 * actually matters when eight items each think they are reasonable. */
#define AGENDA_MAX_RUNS_TOTAL 256u

/* Consecutive failures before an item retires itself. Three: enough for a
 * transient (the network was not up yet, the disk was not mounted) and few
 * enough that a permanently broken item stops costing anything by the third
 * time anyone could have noticed. */
#define AGENDA_MAX_FAILURES   3u

/* Where the agenda is kept. The disk path first when a disk is mounted, so an
 * agenda survives a power cycle; the RAM path otherwise, which does not, and
 * every tool result says which of the two this boot is using. */
#define AGENDA_STORE_DISK  "/disk/agenda.json"
#define AGENDA_STORE_RAM   "/agenda.json"

/* One rendered document must fit a static buffer. Eight items at roughly 500
 * bytes each, with room for the envelope and for a document written by a future
 * version that carries fields this one ignores. */
#define AGENDA_FILE_MAX     12288

/* AND THE TWO NUMBERS ARE NOW CHECKED AGAINST EACH OTHER, because the one above
 * is a hand-computed mirror of the field widths below it, and a constant that
 * mirrors a computed value going stale is the defect class that has turned this
 * tree red twice. It had already gone stale: this read 8192, described as
 * "eight items at roughly 500 bytes each", and the real worst case is 1058 —
 * so a full agenda of large items would have exceeded it.
 *
 * WHAT THAT FAILURE LOOKS LIKE, which is why it is worth a compile-time check
 * rather than a comment: agenda_flush() correctly REFUSES to write a document
 * that does not fit, because a clipped store is a corrupt store. So the symptom
 * is not a truncated file. It is agenda_save() reporting success and the
 * schedule silently not surviving the next power cycle — the same shape as the
 * `used` bug the loader below still carries a comment about.
 *
 * Worst case per item. The escape factor is 2, not 6, and that is sound rather
 * than optimistic: copy_text() and copy_payload() replace every byte below 0x20
 * and 0x7F with a space BEFORE it is ever stored, so no field can contain a
 * character that put_json_str() renders as \uXXXX. Only '"' and '\' expand, and
 * both expand to exactly two bytes. If either sanitiser is ever relaxed, this
 * factor has to move with it.
 *
 *     fixed skeleton: keys, punctuation, two bools and two 10-digit numbers ~160
 *     name, tool, arg, note                                    (sum) * 2 = 898 */
#define AGENDA_ITEM_RENDER_MAX                                                \
    (160u + (AGENDA_NAME_MAX + AGENDA_TOOL_MAX + AGENDA_ARG_MAX +             \
             AGENDA_NOTE_MAX) * 2u)

_Static_assert(AGENDA_FILE_MAX >=
                   32u + AGENDA_MAX_ITEMS * AGENDA_ITEM_RENDER_MAX,
               "AGENDA_FILE_MAX is smaller than the largest document "
               "AGENDA_MAX_ITEMS items can render to, so a full agenda of "
               "worst-case items would refuse to save and the schedule would "
               "silently stop persisting");

/* ---- when ---- */
typedef enum agenda_when {
    AGENDA_WHEN_BOOT = 0,   /* once, during agenda_bringup()                  */
    AGENDA_WHEN_EVERY,      /* every period_ms, from the first tick onwards    */
    AGENDA_WHEN_ONCE,       /* at the next tick, then disable itself           */
    AGENDA_WHEN_COUNT
} agenda_when_t;

/* ---- what ---- */
typedef enum agenda_act {
    AGENDA_DO_TOOL = 0,     /* tool_dispatch(tool, arg)                        */
    AGENDA_DO_SAY,          /* the bound sentence sink                         */
    AGENDA_DO_COUNT
} agenda_act_t;

const char *agenda_when_name(int when);   /* "boot" — never NULL              */
const char *agenda_act_name(int act);     /* "tool" — never NULL              */
const char *agenda_strerror(int rc);      /* one sentence — never NULL        */

/* The seventh bound, enumerable. agenda_denied_tool(0), (1), ... walks the names
 * of tools an item may not schedule because their success ends the boot, and
 * returns NULL past the end. Published for exactly one reason: a test asserts
 * every name here is a tool that really is registered, so renaming power_off
 * fails a test instead of quietly re-arming a machine that cannot boot. */
const char *agenda_denied_tool(int i);

/* How many denied names do NOT resolve to a registered tool. Zero in a healthy
 * build. Called by agenda_bringup() on every boot, which prints a loud line per
 * unresolved name, because a name list that has drifted out of step with the
 * registry fails silently in the unsafe direction. */
int agenda_denylist_unresolved(void);

/* How many REGISTERED tool names are too long for an agenda item to store. Zero in
 * a healthy build. Called by agenda_bringup() on every boot and prints both numbers
 * per offender: a bound that mirrors something computed has to be checked against
 * the thing it mirrors, or it goes stale in silence. */
int agenda_bounds_ok(void);

typedef struct agenda_item {
    /* ---- what was asked for; this half is what gets persisted ---- */
    char     name[AGENDA_NAME_MAX];
    char     note[AGENDA_NOTE_MAX];
    uint8_t  used;            /* 1 for an occupied slot. agenda_count() is the
                               * authority; this is a redundant marker kept for
                               * legibility in a dump, and the schedulers
                               * deliberately do NOT consult it — see the note in
                               * core/agenda.c's loader for what happened the one
                               * time they did. */
    uint8_t  when;            /* agenda_when_t                               */
    uint8_t  act;             /* agenda_act_t                                */
    uint8_t  enabled;
    /* IN FLIGHT, AND DURABLE. Written to the store BEFORE a when=boot action is
     * attempted and cleared after it resolves, so that a boot which dies inside
     * the action leaves the marker set and the NEXT boot can see it.
     *
     * Without it a crashing boot item bricked the machine permanently, and the
     * mechanism is worth stating because every other safeguard sits on the wrong
     * side of it: the failure counter, the durable disable and agenda_flush()
     * all run AFTER fault_guard_run() returns, and an action that triple-faults
     * never returns. So the on-disk record still said "enabled": true, when=boot
     * items re-arm by design, and the next boot died in the same place — with no
     * shell, no recovery console and no prompt, i.e. nothing left inside the
     * machine to remove the item with. Reproduced end to end on a stock kernel
     * with a 136-byte cc_compile argument, twice, from the same disk image.
     *
     * The existing deny_tab covers a tool that ends the boot by SUCCEEDING; it
     * cannot cover one that crashes, and its own comment says so.
     *
     * Only when=boot items are marked, and that is a cost decision: they are the
     * ones that run before any interface exists, and marking every tick of an
     * every-item would write the store twice a period for ever. */
    uint8_t  attempting;
    uint32_t period_ms;       /* AGENDA_WHEN_EVERY only                       */
    uint32_t max_runs;
    char     tool[AGENDA_TOOL_MAX];   /* AGENDA_DO_TOOL only                  */
    char     arg[AGENDA_ARG_MAX];     /* the tool's JSON input, or the words  */

    /* ---- what actually happened; since-boot only, never persisted ----
     * Kept apart from the half above on purpose. A schedule and a record of a
     * schedule being kept are different things, and writing statistics into the
     * store would mean rewriting the disk on every tick. */
    uint32_t runs;
    uint32_t failures;        /* consecutive; reset by a success              */
    uint32_t total_failures;
    uint32_t escapes;         /* runs that took a fatal CPU exception         */
    uint64_t last_ms;
    uint64_t next_ms;         /* 0 when not scheduled                         */
    int      last_rc;
    char     last_why[AGENDA_WHY_MAX];
} agenda_item_t;

/* ====================================================================== */
/* lifecycle                                                              */
/* ====================================================================== */

/* Bind the sink an AGENDA_DO_SAY item hands its sentence to. In the kernel this
 * is a one-line wrapper around chat_ask(); in a test it is a recorder. NULL
 * unbinds, and a "say" item then fails with AGENDA_ENOSAY, which is reported
 * rather than papered over — a machine that cannot reach the model must not
 * pretend it ran the sentence. The callback returns 0 for success. */
void agenda_bind_say(int (*say)(const char *sentence));

/* Load the store and run every AGENDA_WHEN_BOOT item, in the order they were
 * saved. Call after the filesystem is up and after the transport is bound.
 * Returns the number of boot items that ran (whether or not they succeeded);
 * everything interesting is printed. */
uint32_t agenda_bringup(uint64_t now_ms);

/* Re-read the store from scratch, discarding the in-memory table. Returns the
 * number of items loaded, or a negative AGENDA_E*. A malformed store is reported
 * loudly and leaves an EMPTY agenda rather than half of one: half a schedule is
 * a schedule nobody wrote. */
int agenda_load(void);

/* Write the table out. Called by every mutation, so an item that was accepted
 * is on the disk before the tool result says so. */
int agenda_flush(void);

/* Forget everything, in memory only — the store is not touched. Tests, and the
 * "start again" path. */
void agenda_reset(void);

/* Master switch. Off means no item ever runs, agenda_tick() is a return, and the
 * tools say so. On by default. Not a substitute for disabling an item: this is
 * the machine's autonomy as a whole. */
void agenda_enable(int on);
int  agenda_enabled(void);

/* ====================================================================== */
/* editing                                                                */
/* ====================================================================== */

/* Validate and store one item, replacing any item of the same name.
 *
 * Every field is checked before anything is written, and a refusal changes
 * nothing: the name (1..31 characters, lowercase letters, digits, '_' and '-',
 * so it can be said out loud and typed back), the trigger, the period against
 * AGENDA_MIN_PERIOD_MS, max_runs against AGENDA_MAX_RUNS, and — for a tool
 * action — that the tool IS REGISTERED IN THIS BUILD and that the argument
 * parses as a JSON object. That last pair is the important one: an agenda item
 * naming a tool that does not exist would fail silently at three in the morning,
 * and the whole point of validating at save time is that somebody is present to
 * be told.
 *
 * Returns AGENDA_OK, or a negative code with a sentence in `why`. */
int agenda_save(const agenda_item_t *item, char *why, size_t whycap);

/* Delete by name. Returns AGENDA_OK or AGENDA_ENOENT. */
int agenda_remove(const char *name);

/* Enable or disable by name without losing the item or its statistics. */
int agenda_set_enabled(const char *name, int on);

/* Run one item NOW, regardless of its schedule, exactly as a tick would — same
 * guard, same trace lines, same accounting. This is how an operator (or the
 * model) proves an item works at the moment it is saved, rather than finding out
 * at the next boot. Counts against every bound. */
int agenda_run_now(const char *name, uint64_t now_ms);

/* ====================================================================== */
/* running                                                                */
/* ====================================================================== */

/* Run at most ONE due item. Returns the number of actions taken (0 or 1).
 *
 * One per call, not all of them, and that is a deliberate fairness property: the
 * caller is the loop that also polls the keyboard, and an agenda with a backlog
 * must not be able to make the machine stop listening. A backlog drains one item
 * per pass through the idle loop, which on this machine is thousands of times a
 * second.
 *
 * Safe to call as often as you like: with nothing due it is one compare per
 * item. MUST be called from normal kernel context, never from an exception
 * handler — it dispatches tools and may start a model turn. */
uint32_t agenda_tick(uint64_t now_ms);

/* Run every boot item. Called by agenda_bringup(); separate so a test can drive
 * it without a filesystem. */
uint32_t agenda_run_boot(uint64_t now_ms);

/* The name of the item that would run next on a tick, or NULL. */
const char *agenda_due(uint64_t now_ms);

/* ====================================================================== */
/* reading                                                                */
/* ====================================================================== */

int                   agenda_count(void);
const agenda_item_t  *agenda_at(int index);        /* NULL past the end       */
const agenda_item_t  *agenda_find(const char *name);

uint32_t agenda_runs(void);        /* autonomous actions taken this boot      */
uint32_t agenda_failures(void);    /* how many of them failed                 */
uint32_t agenda_escapes(void);     /* how many faulted and were escaped       */

/* The path the agenda is stored at, and whether that path survives a power
 * cycle. Reported by every tool, because "saved" means two different things
 * depending on the answer and only one of them is what the operator wanted. */
const char *agenda_store(void);
int         agenda_persistent(void);

/* One block for the boot log: the store, the master switch, the totals, and one
 * line per item. */
void agenda_report(void);

#endif /* AGENDA_H */
