/* faultchat.h — the kernel asking the model to diagnose its own crash.
 *
 * PURPOSE
 *   Phase 3a made a fault legible. Phase 3b made it survivable, but only for a
 *   decision the model had already armed *before* the fault. This header closes
 *   the loop the other way round: after the machine survives an exception, the
 *   KERNEL — on its own initiative, with no operator sentence involved — hands
 *   the model everything the CPU said, asks what happened, and offers to arm the
 *   fix that comes back.
 *
 *   Nothing else in this kernel starts a conversation. chat.c runs when a human
 *   types; this runs when the hardware objects. That difference is the whole
 *   feature, and it is also the entire source of danger, which is why the next
 *   section is longer than the API.
 *
 * ============================================================================
 * THE SAFE-CONTEXT ARGUMENT — read this before changing anything below
 * ============================================================================
 *
 * The obvious implementation is to call the model from inside the exception
 * handler, where the fault is freshest. That implementation is wrong, and it is
 * wrong in a way that turns a recoverable fault into an unrecoverable one.
 *
 *   1. The transport needs the heap. net/net.c reaches the model through lwIP
 *      and mbedTLS; both call kmalloc, thousands of times per TLS handshake. If
 *      the fault happened *inside* the allocator — a corrupted header, a bad
 *      free-list link — then the allocator is mid-mutation, and the first thing
 *      the diagnosis request does is walk the very structure that just broke.
 *      Best case it faults again. Worst case it silently corrupts more.
 *   2. Neither stack is reentrant. If the fault happened inside lwIP or inside
 *      mbedTLS, re-entering them from the handler resumes a state machine that
 *      is halfway through a state transition, on the interrupted stack, with a
 *      pbuf or a TLS record half-built.
 *   3. IF is 0 in the handler and the whole machine is polled. A network round
 *      trip from there is not merely long, it is a spin with interrupts off in
 *      code that assumes it is running at task level.
 *   4. A fault raised while the handler is running is caught by idt.c's
 *      in_handler guard and halts immediately, with a deliberately minimal
 *      report. So "phone home from the handler" converts a survivable page
 *      fault into the one failure mode this whole module set exists to prevent.
 *
 *   lib/trace.c is allocation-free and hardware-free precisely so that it IS
 *   safe there. The network stack is emphatically not, and no amount of care
 *   inside this file can make it so.
 *
 * SO: THIS MODULE RUNS NO CODE INSIDE THE EXCEPTION HANDLER. NOT ONE
 * INSTRUCTION.
 *
 *   arch/x86_64/idt.c is unchanged by this phase. There is no hook, no callback,
 *   no queue push from isr_dispatch(). The handler does exactly what it did
 *   before: capture into the ring (allocation-free, already built), consult the
 *   armed plan, and either patch the frame and IRETQ or halt.
 *
 *   Discovery is by polling instead. faultchat_pump() compares fault_count()
 *   with the count it last saw. If it has moved, there is a new fault to talk
 *   about. That is the entire mechanism, and it buys three properties that a
 *   handler-side hook cannot:
 *
 *     - The handler's cost stays exactly zero, so nothing this module does can
 *       ever make a fault less survivable than it was in Phase 3b.
 *     - The pump only ever runs in normal kernel context, because it is only
 *       ever *called* from normal kernel context.
 *     - Every fault the pump can see is a fault the machine SURVIVED. A fatal,
 *       unrecovered fault halts inside the handler and never returns, so it can
 *       never reach a pump call site. There is no code path in which this module
 *       runs on top of a machine that has already given up.
 *
 * ============================================================================
 * WHAT CLOSES THE LOOP — and why this file did not have to change much to do it
 * ============================================================================
 *
 *   Everything above was true before the loop was closed, and none of it has
 *   been weakened. What was missing was not a way to ASK; it was a way to still
 *   be running when there was something worth asking about.
 *
 *   The pump can only ever see faults the machine SURVIVED. Before, there were
 *   exactly two ways to survive one: it was a continuable trap, or somebody had
 *   armed a recovery plan in advance. A plain fatal fault with no plan halted
 *   inside the handler and the machine never reached a pump call site — so the
 *   very faults most worth diagnosing were the ones that could not be diagnosed.
 *   The loop had a hole in it exactly where it mattered.
 *
 *   fault.h's fault_guard_run() / fault_escape() fill that hole and are the only
 *   new ingredient. A caller that has thought about the consequences wraps what
 *   it is doing in a guard; a fatal fault inside that call chain is caught by the
 *   handler, which — allocation-free, device-free, exactly as before — patches
 *   the frame so the IRETQ lands back in the guard's caller instead of halting.
 *   The call chain is abandoned. The machine is in normal kernel context, on an
 *   intact stack, with the fault fully recorded in the ring. THEN this module
 *   runs, on the far side of the handler, and asks.
 *
 *   So the sequence the operator never has to be present for is:
 *
 *     fault  ->  captured in the handler (no allocation, no network)
 *            ->  frame patched to the innermost guard; handler returns
 *            ->  normal kernel context; the guarded caller gets ESCAPED back
 *            ->  faultchat_pump(): build a prompt from the record, ask
 *            ->  the reply is validated by the same tools an operator's model
 *                turn would go through, four gates deep
 *            ->  a recovery plan is armed and/or .text is patched
 *            ->  the caller retries, and the retry either works or is bounded
 *
 *   Note where this module still refuses to be clever: it does NOT retry
 *   anything itself, and it does not decide what to re-run. It answers one
 *   question ("what was that, and what should be done about it") and arms what
 *   comes back. Deciding to try again is the caller's business, because only the
 *   caller knows what it was doing — see core/agenda.c, which does both.
 *
 * AND THE PART THAT MAKES A REPAIR PERMANENT
 *   A recovery plan changes what the CPU does at the NEXT fault. That is enough
 *   to survive, and not enough to fix: the same bad instruction is still there
 *   and will fault again, and the plan is consumed. So a diagnosis may also
 *   propose a "patch" — literally the input of the fault_patch tool — which
 *   rewrites the instructions. Pages here are RWX, so the bug can be edited out
 *   of the running kernel with no compiler and no reboot, and the ORIGINAL BYTES
 *   ARE ALWAYS KEPT so it can be undone.
 *
 *   That is a much sharper instrument than a recovery plan, which is why it has
 *   its own, much smaller budget (FAULTCHAT_MAX_PATCHES) and why the per-address
 *   attempt limit (FAULTCHAT_MAX_PER_RIP) exists: a wrong patch does not expire,
 *   does not fault, and costs money to keep re-diagnosing.
 *
 * WHY NOT "LAST RITES" — a diagnosis on the way down
 *   The tempting exception is the fatal case: the machine is about to halt
 *   forever, so what is there to lose by trying? Two things, and they are the
 *   two that matter. First, a fault inside the attempt hits the in_handler guard
 *   and replaces a full, formatted report with a one-line minimal one — the
 *   operator loses the only record they were ever going to get. Second, a broken
 *   heap makes lwIP *hang* at least as often as it faults, and a spin with IF
 *   clear produces no message at all, which is strictly worse than a halt that
 *   printed a report. A dead machine that explained itself beats a hung machine
 *   that did not. If this is ever revisited it needs its own stack, its own
 *   allocator, and a watchdog — i.e. it is a different project, not a flag.
 *
 * WHAT ABOUT A FAULT INSIDE THE HEAP?
 *   Two independent answers, because one is not enough.
 *
 *     Structural: this module never allocates. The prompt, the request body, the
 *     response body, the reply record and the tool-result scratch are all static
 *     .bss, sized at compile time. A completely destroyed allocator cannot stop
 *     a diagnosis request from being BUILT.
 *
 *     Bounded: it can still stop one from being SENT, because the transport
 *     allocates and this module cannot fix that. So the damage is bounded rather
 *     than prevented — see the recursion latch below. Note also what is NOT done
 *     here: heap_check() is never called as a precondition, because walking the
 *     allocator's metadata to find out whether the allocator is trustworthy is
 *     the same dereference chain that just faulted, and heap_check() panics on
 *     what it finds rather than returning a verdict.
 *
 * A FAULT WHILE A DIAGNOSIS IS IN FLIGHT
 *   This is the case that decides whether the feature is safe, so it is handled
 *   three times over:
 *
 *     1. Re-entrancy latch. `busy` is set for the whole request. faultchat_pump()
 *        returns FAULTCHAT_EBUSY immediately if it is already inside itself, so
 *        even if a future call site is added somewhere reachable from the
 *        transport, the second entry does nothing at all.
 *     2. Detection. The fault count is sampled before the request and compared
 *        after it. A fault during the exchange means the reply cannot be trusted
 *        (the transport was interrupted mid-state-machine), so the reply is
 *        DISCARDED even if it parsed, and no fix is armed from it.
 *     3. Latch-off, permanently. A fault during a diagnosis is evidence that
 *        diagnosing is what broke the machine. Asking again is how a diagnosis
 *        loop becomes a fault loop, so the module disables itself for the rest
 *        of the boot and says so on the console. It is a latch, not a counter:
 *        it never re-arms.
 *
 *   On top of that, FAULTCHAT_MAX_DIAGNOSES caps how many requests one boot may
 *   make at all, so a machine that faults in a slow storm cannot spend the rest
 *   of its life talking about it.
 *
 * WHERE THE PUMP IS CALLED FROM
 *   Anywhere in normal kernel context, as often as you like: it is cheap
 *   (an integer compare) when there is nothing to do. The intended call site is
 *   the REPL loop in kernel/main.c, between sentences — the point where the
 *   machine is provably idle, the heap is quiescent and the network is up.
 *   arch/x86_64/fault_inject.c calls it in the diagnostic builds, which is how
 *   the whole path is demonstrated on real hardware.
 *
 *   kernel/main.c belongs to nobody in particular and was not edited by this
 *   phase, so the two lines it wants are written out here rather than applied:
 *
 *       #include "faultchat.h"
 *       ...
 *       chat_init(net_up ? model_tls_transport() : (model_transport_t *)0);
 *       faultchat_bind(net_up ? model_tls_transport()             // <-- add
 *                             : (model_transport_t *)0);          // <-- add
 *       ...
 *       for (;;) {
 *           faultchat_pump();                                     // <-- add
 *           kputs("\n> ");
 *           ...
 *
 *   Before the prompt is printed, not after, so a diagnosis never lands under a
 *   "> " the operator is already typing at. Until those lines exist the module
 *   is inert in a normal build: it is bound to nothing and polled by nobody,
 *   which is a safe default — a fault still prints its full report exactly as it
 *   did in Phase 3b.
 *
 * THE EXCHANGE
 *   One request, one reply, no tool-use loop. Deliberately:
 *
 *     - A tool loop would let the model run arbitrary kernel calls immediately
 *       after a fault, which is the worst possible moment to be poking at
 *       hardware. It also needs net/chat.c's history machinery, and a crash
 *       report has no conversation to belong to.
 *     - One shot means one bounded buffer and one bounded failure. If the model
 *       says something unintelligible, the machine prints why and carries on.
 *
 *   The reply contract is a single JSON object: a "diagnosis" string and an
 *   optional "fix" object. The "fix" object's grammar is not a second language —
 *   it is byte-for-byte the input schema of the existing fault_recover tool, and
 *   it is applied by literally dispatching that tool. So a fix proposed by a
 *   diagnosis and a fix armed by the model during a conversation go through the
 *   same parser, the same validation, the same fault_plan_check(), and produce
 *   the same kernel-emitted [fault_recover ...] trace line. There is exactly one
 *   door into the CPU's future, and this module knocks on it like everyone else.
 *
 * VALIDATION, IN ORDER
 *   A proposed fix passes four independent gates before it can change what the
 *   CPU executes, and this module owns only the first:
 *     1. faultchat_parse_reply — is this even a JSON object with a bounded
 *        "fix" member? Rejects wrapping prose, markdown fences, oversize
 *        objects, wrong types, and a fix that is not an object.
 *     2. tools/fault_tools.c   — the field grammar: known keys only, known
 *        actions only, rsp refused, uses in range.
 *     3. fault_plan_check()    — effect: #DF/#MC are never recoverable, a
 *        set_rip target must be inside [code_lo, code_hi).
 *     4. fault_apply_plan()    — re-validated a third time inside the handler
 *        against the LIVE frame, at the moment of use.
 *   Nothing here is trusted. The model's reply is untrusted input arriving over
 *   a network, and it is treated exactly like the model's tool arguments are.
 *
 * PUBLIC API
 *   Lifecycle     faultchat_bind, faultchat_reset, faultchat_enable.
 *   The loop      faultchat_pump, faultchat_pending.
 *   Pure parts    faultchat_build_prompt, faultchat_parse_reply,
 *                 faultchat_apply_fix, faultchat_system_prompt,
 *                 faultchat_strerror. All host-tested against net/model_mock.c.
 *   Autonomy      faultchat_apply_patch, faultchat_patches_applied,
 *                 faultchat_rip_attempts. The pump is what makes the machine
 *                 repair itself with nobody watching; these are what bound it.
 *   Introspection faultchat_diagnoses, faultchat_fixes_armed,
 *                 faultchat_last_result, faultchat_last_seq,
 *                 faultchat_last_diagnosis, faultchat_last_prompt,
 *                 faultchat_disabled, faultchat_disabled_reason.
 *
 * DEPENDENCIES
 *   fault.h (the record and the plan), model.h (the transport seam), json.h
 *   (all parsing), tool.h (the one door), kernel.h (kputs/kprintf). Deliberately
 *   NOT heap.h, NOT net.h, NOT lwIP, NOT mbedTLS: this file names no transport,
 *   so tests/host/test_fault_diagnose.c compiles it natively and drives the
 *   whole path against the scripted mock with no network and no API key.
 *
 * WHAT AWAITS A REAL API KEY
 *   Everything below is proven against net/model_mock.c and, on hardware,
 *   against a scripted transport compiled only into the fault-injection build.
 *   With a key supplied over fw_cfg and faultchat_bind(model_tls_transport()), the same
 *   code sends the same body to api.anthropic.com. What is NOT proven, and
 *   cannot be until a key exists: whether a real model reliably answers with a
 *   bare JSON object, and whether its proposed fixes are any good. The parser
 *   assumes neither — it accepts a fenced or prose-wrapped object, and every
 *   fix is refused unless it survives all four gates above.
 *
 * FUTURE EXTENSION POINTS
 *   - A second round trip: hand the model the tool_result of its own fix and
 *     let it revise. Needs a bounded two-message history, not chat.c's.
 *   - Scoping a proposed fix to (vector, RIP, CR2) rather than vector alone —
 *     fault.h already lists this as the next increment on the plan side.
 *   - Persisting the exchange into the VFS so a crash survives a reset, which is
 *     the only way a *fatal* fault will ever get diagnosed on this machine.
 *   - A rate limiter keyed on the faulting RIP, so a slow storm at one address
 *     spends one diagnosis rather than FAULTCHAT_MAX_DIAGNOSES of them.
 */
#ifndef FAULTCHAT_H
#define FAULTCHAT_H

#include <stdint.h>
#include <stddef.h>

#include "fault.h"
#include "model.h"

/* ---- result codes (0 = a diagnosis completed) ---- */
#define FAULTCHAT_OK            0
#define FAULTCHAT_ENONE        -2    /* nothing new to diagnose                */
#define FAULTCHAT_EOFF         -1    /* latched off, or the budget is spent    */
#define FAULTCHAT_EBUSY       -16    /* re-entered while a request is in flight*/
#define FAULTCHAT_EINVAL      -22    /* bad arguments                          */
#define FAULTCHAT_ENOSPC      -28    /* a fixed buffer could not hold it       */
#define FAULTCHAT_ETRANSPORT  -70    /* no transport bound, or it failed       */
#define FAULTCHAT_EPROTO      -72    /* the reply was not an intelligible one  */
#define FAULTCHAT_EHTTP      -100    /* the API answered, but not with 200     */
#define FAULTCHAT_EFIX       -103    /* a fix was proposed and refused         */
#define FAULTCHAT_ERECURSE   -104    /* a fault occurred during the request    */
#define FAULTCHAT_EPATCH     -105    /* a code patch was proposed and refused  */

/* ---- bounds. Every one is static .bss; nothing here allocates. ---- */

/* The assembled prompt. Must hold a whole FAULT_REPORT_MAX report (3584) plus
 * the machine-state header, the memory window around RIP and the answer
 * contract, with room to spare. */
#define FAULTCHAT_PROMPT_MAX    8192

/* One request body: the system prompt plus the escaped prompt above. */
#define FAULTCHAT_REQ_BYTES    24576

/* One response body, whole-buffered (the transport does not stream). Sized from
 * the request rather than guessed: max_tokens is 1024, so the reply cannot
 * exceed roughly 4 KiB of text, and the rest is the envelope a real API wraps
 * around it (id, model, usage, stop_reason). A longer body is reported as a
 * truncated reply, not silently half-parsed. */
#define FAULTCHAT_RESP_BYTES    8192

/* The model's prose, after sanitising. Long enough for a real explanation and
 * short enough that a runaway reply cannot fill the console. */
#define FAULTCHAT_DIAG_MAX       512

/* The raw "fix" object handed to the fault_recover tool. The largest legitimate
 * one is well under 160 bytes; anything near this is a red flag, not a plan. */
#define FAULTCHAT_FIX_MAX        512

/* The raw "patch" object handed to the fault_patch tool. Bigger than a fix
 * because it carries two hex byte strings (up to 64 characters each) and a note,
 * and still small enough that a runaway object is refused rather than parsed. */
#define FAULTCHAT_PATCH_MAX      768

/* Why a diagnosis ended the way it did, in one sentence. */
#define FAULTCHAT_WHY_MAX        192

/* Requests one boot may make. A machine that faults in a slow storm must not
 * spend the rest of its life talking about it. */
#define FAULTCHAT_MAX_DIAGNOSES    4

/* Code patches one boot may accept from a diagnosis. Much smaller than the
 * diagnosis budget, and deliberately so: a diagnosis costs a round trip and
 * some of the operator's money, but a wrong code patch costs a kernel whose
 * behaviour nobody can predict, and unlike a recovery plan it does not expire.
 * Two is enough to fix a thing and then fix the fix. */
#define FAULTCHAT_MAX_PATCHES      2

/* Diagnoses spent on any ONE faulting address before that address is written
 * off for the rest of the boot.
 *
 * This is the bound that makes an autonomous loop a loop and not a spiral. The
 * per-RIP re-fault counter in fault.c already stops the CPU thrashing one
 * instruction, but it says nothing about MONEY or about progress: without this,
 * a fault that recurs at 0x10a4f3 after every attempted repair would spend the
 * whole diagnosis budget re-describing the same three bytes, and each round is a
 * paid API call. After this many tries at one address the module says so, names
 * the address, and stops asking about it - while remaining willing to diagnose a
 * fault somewhere else, which is the difference between giving up on a bug and
 * giving up on the machine. */
#define FAULTCHAT_MAX_PER_RIP      2

/* ---- what came back ---- */

typedef struct faultchat_reply {
    /* Model prose, already sanitised: control characters become spaces and
     * square brackets become parentheses, because this string is printed on the
     * same console the kernel writes its [bracketed] ground truth to and must
     * not be able to forge a line there. Always NUL-terminated. */
    char   diagnosis[FAULTCHAT_DIAG_MAX];

    /* The proposed fix, verbatim, as a NUL-terminated JSON object — exactly the
     * bytes handed to the fault_recover tool as its input. has_fix is 0 when the
     * model offered none, which is a valid and often correct answer. */
    int    has_fix;
    char   fix[FAULTCHAT_FIX_MAX];
    size_t fix_len;

    /* The proposed code patch, verbatim, as a NUL-terminated JSON object -
     * exactly the bytes handed to the fault_patch tool as its input. Same
     * discipline as `fix` above and for the same reason: this module does not
     * interpret either of them, because there must be exactly one grammar for
     * a patch and tools/fault_patch.c already owns it. */
    int    has_patch;
    char   patch[FAULTCHAT_PATCH_MAX];
    size_t patch_len;
} faultchat_reply_t;

/* ====================================================================== */
/* the pure half — no transport, no globals, no allocation                */
/* ====================================================================== */

/* The system prompt this module sends. Distinct from chat.c's: that one tells
 * the model it is a user interface, this one tells it that the kernel is asking
 * and that the answer is a JSON object. Exposed so a test can assert the machine
 * states its contract rather than hoping the model guesses it. */
const char *faultchat_system_prompt(void);

/* Render everything a model needs to diagnose `rec` into `dst`. snprintf-style
 * return: the length that WOULD have been written, with `dst` always
 * NUL-terminated when cap > 0.
 *
 * `window` may be NULL, in which case the memory dump around RIP is omitted;
 * when it is non-NULL, bytes are read ONLY from inside window->image_lo..hi, on
 * the same terms as fault_capture(). Nothing else here dereferences anything.
 *
 * Pure: no globals, no allocation, no transport. This is the function a test
 * can point at a synthetic record to prove the prompt carries the vector, the
 * decoded error, RIP, the instruction bytes, the registers and the backtrace. */
size_t faultchat_build_prompt(const fault_record_t *rec,
                              const fault_window_t *window,
                              char *dst, size_t cap);

/* Turn a Messages API response body into a faultchat_reply_t.
 *
 * Robust by construction, because a model reply is untrusted input:
 *   - the body must parse, carry content blocks, and contain text;
 *   - the text is searched for one JSON object. The fast path is the whole
 *     trimmed text; the fallback finds the first brace-balanced span, so
 *     ```json fences and "Here is my answer:" preambles are tolerated;
 *   - "diagnosis" must be a string and is sanitised on the way in;
 *   - "fix", if present, must be an OBJECT and must fit FAULTCHAT_FIX_MAX. It
 *     is copied out verbatim and NOT interpreted here — interpreting it is the
 *     fault_recover tool's job, and there is only one of those.
 *
 * Returns FAULTCHAT_OK, or FAULTCHAT_EPROTO with a sentence in `why`. `why` may
 * be NULL; when it is not, it is always NUL-terminated. */
int faultchat_parse_reply(const char *body, size_t len,
                          faultchat_reply_t *out, char *why, size_t whycap);

/* Arm the proposed fix by dispatching the real fault_recover tool with the
 * reply's `fix` object as its input. Returns FAULTCHAT_OK when the tool armed
 * something, FAULTCHAT_EFIX when it refused (with the tool's own refusal text in
 * `why`, which is what the model would have been told), or FAULTCHAT_ENONE when
 * there was no fix to apply.
 *
 * Routing through tool_dispatch() rather than fault_plan_arm() is deliberate:
 * it means a fix the kernel solicited and a fix the model volunteered are
 * validated by the same code and produce the same [fault_recover ...] trace
 * line, so the console cannot be read as if one of them happened quietly. */
int faultchat_apply_fix(const faultchat_reply_t *reply, char *why, size_t whycap);

/* Apply the proposed CODE PATCH by dispatching the real fault_patch tool with
 * the reply's `patch` object as its input. Same contract, same reasoning, same
 * single door as faultchat_apply_fix() - and the same refusal to interpret the
 * object here.
 *
 * Returns FAULTCHAT_OK when the patch was applied, FAULTCHAT_EPATCH when the
 * tool refused it (with the tool's own words in `why`), or FAULTCHAT_ENONE when
 * none was proposed. Refuses without asking the tool when the per-boot patch
 * budget is spent, because "how much of this kernel has been rewritten
 * unattended" is a quantity that must be bounded here, where it can be counted,
 * rather than in the tool, which cannot tell an operator's patch from the
 * kernel's own. */
int faultchat_apply_patch(const faultchat_reply_t *reply,
                          char *why, size_t whycap);

/* Static description of a FAULTCHAT_* code. Never NULL. */
const char *faultchat_strerror(int rc);

/* ====================================================================== */
/* the live half                                                          */
/* ====================================================================== */

/* Bind the transport diagnoses go out on, and clear all state. `t` may be NULL —
 * that is the "machine booted with no network" case, and the pump reports it
 * once rather than pretending. Never names a transport itself: the kernel passes
 * model_tls_transport(), a test passes model_mock_transport(). */
void faultchat_bind(model_transport_t *t);

/* Forget everything: counters, the latch, the last reply, and the high-water
 * fault count (so the next pump treats the current newest fault as new). */
void faultchat_reset(void);

/* Turn the whole feature on or off by hand. On by default. Turning it off is
 * not the same as the latch: this is a policy switch, the latch is a verdict. */
void faultchat_enable(int on);

/* 1 when there is a fault the pump has not talked about yet AND it would be
 * allowed to. Cheap; safe to call from anywhere. */
int faultchat_pending(void);

/* Run at most one diagnosis to completion: build the prompt, send it, parse the
 * reply, print it, and arm the proposed fix.
 *
 * MUST ONLY BE CALLED FROM NORMAL KERNEL CONTEXT — never from an exception
 * handler, never from anything an exception handler calls. See the argument at
 * the top of this file; the re-entrancy latch enforces it but does not excuse
 * violating it.
 *
 * Returns FAULTCHAT_OK when a diagnosis completed (whether or not it carried a
 * fix), FAULTCHAT_ENONE when there was nothing to do, or one of the negative
 * codes above. Every outcome is also printed for the operator, because on this
 * machine an unprinted event did not happen. */
int faultchat_pump(void);

/* ---- introspection (the boot banner, the tests, and the operator) ---- */

/* Diagnosis ATTEMPTS, not successes: a fault the pump took responsibility for,
 * whether or not a request reached a transport. That is the quantity
 * FAULTCHAT_MAX_DIAGNOSES caps, because a machine with no network and a fault
 * storm should not spend its boot announcing that it cannot ask. */
uint32_t    faultchat_diagnoses(void);
uint32_t    faultchat_fixes_armed(void);    /* proposed fixes that were armed  */
uint32_t    faultchat_patches_applied(void);/* proposed code patches applied   */

/* How many diagnoses this boot has already spent on `rip`, and the address the
 * module is currently counting against. A caller that wants to know whether the
 * loop is making progress reads these. */
uint32_t    faultchat_rip_attempts(uint64_t rip);
uint64_t    faultchat_worst_rip(void);
int         faultchat_last_result(void);    /* the last FAULTCHAT_* code       */
uint32_t    faultchat_last_seq(void);       /* which fault was last diagnosed  */
const char *faultchat_last_diagnosis(void); /* sanitised prose; "" if none     */
const char *faultchat_last_prompt(void);    /* the exact bytes that were sent  */
size_t      faultchat_last_prompt_len(void);
const char *faultchat_last_why(void);       /* one sentence about the outcome  */

/* 1 once the module has latched itself off for the rest of the boot, plus the
 * reason in words (never NULL). */
int         faultchat_disabled(void);
const char *faultchat_disabled_reason(void);

#endif /* FAULTCHAT_H */
