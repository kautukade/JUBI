/* chat.h — the agentic turn loop: one typed sentence -> real kernel actions.
 *
 * PURPOSE
 *   This is the machine's only interface. There is no shell, no command
 *   language, and there never will be: the operator types a sentence, and this
 *   module turns it into kernel work. It is the piece that joins the three
 *   contracts around it — tool.h (what the model may do), model.h (how bytes
 *   reach the model), json.h (how the protocol is read and written) — into a
 *   loop that keeps running until the machine has actually done the thing.
 *
 * THE LOOP
 *   One call to chat_ask() is one *turn*, and a turn is not one request. The
 *   model may answer with tool_use blocks instead of prose; those are real
 *   kernel calls, and their results have to go back so the model can carry on.
 *   So a turn is:
 *
 *       append the operator's sentence to the history
 *       repeat:
 *           build a request  = system prompt + tools[] + the whole history
 *           send it through the transport (model.h — never a named transport)
 *           print every text block                      <- the model's voice
 *           dispatch every tool_use block (tool.h)      <- the kernel acts,
 *                                                          and traces it
 *           if there were no tool_use blocks: the turn is done
 *           append the assistant turn verbatim, then a user turn carrying one
 *           tool_result per tool_use, keyed by tool_use_id, plus one kernel
 *           note telling the model how much of the round budget is left
 *       until CHAT_MAX_ROUNDS
 *
 *   The iteration cap is not a nicety. A model that keeps calling tools would
 *   otherwise own this machine forever; there is no scheduler to preempt it and
 *   no other console to kill it from.
 *
 * SOLVING, NOT ANSWERING — the properties the loop owes a working agent
 *   A cap alone makes the machine safe, not useful. Four things here are what
 *   make a multi-step job actually finish:
 *
 *     1. THE BUDGET IS VISIBLE. Every tool_result turn carries a kernel note
 *        ("tool round 3 of 16 used ..."). A model that cannot see the cap plans
 *        past it and gets cut off mid-job; one that can see it wraps up. On the
 *        last round the note says so in as many words, and the request is sent
 *        with tool calling switched off (tool_choice=none — NOT by dropping the
 *        schema, which the history still references), so the only continuation
 *        left is prose. That turns the cap from a silent death into a report.
 *     2. TRANSIENT FAILURE IS NOT FAILURE. A rate limit or a dropped TLS read
 *        used to throw away every completed action in the turn. Retryable
 *        outcomes (HTTP 429/503/529, timeouts, transport errors) are retried up
 *        to CHAT_SEND_RETRIES times per turn without spending a round, because
 *        the alternative is to discard real work that already happened.
 *     3. EVERY tool_use IS ANSWERED, ALWAYS. The API rejects a conversation in
 *        which a tool_use has no matching tool_result, so "the results did not
 *        fit" is not a truncation, it is the death of the whole exchange. The
 *        staging writer therefore gives each pending call a FAIR SHARE of the
 *        room left (see put_tool_result), and a round whose answers provably
 *        cannot fit is refused BEFORE any tool runs rather than after.
 *     4. THE KERNEL REMEMBERS WHAT IT RAN. chat_action_at() is a ring of the
 *        tool calls this machine actually dispatched, recorded by the loop from
 *        real dispatch results — not by the model, and not from the model's
 *        memory of them. It outlives history eviction and outlives an abandoned
 *        turn, which is what lets a model that was cut off pick the job back up
 *        instead of starting again. tools/agent_tools.c hands it to the model.
 *
 * CONVERSATION HISTORY — the bounded part
 *   Multi-turn tool use is unforgiving about shape. The assistant's tool_use
 *   turn must be echoed back *verbatim*, and it must be followed by a user turn
 *   whose tool_result blocks match it one-for-one by id. A history that loses
 *   half of such a pair is not "shorter", it is invalid, and the API rejects it.
 *
 *   Storage is therefore a single fixed arena of raw JSON message *contents*
 *   (CHAT_HISTORY_BYTES) plus a fixed index of at most CHAT_HISTORY_MSGS
 *   entries. Nothing grows; nothing is allocated per turn. Every entry carries
 *   the *epoch* — the operator turn — it belongs to, and three rules follow:
 *
 *     1. Eviction is whole epochs, oldest first. Dropping a complete exchange
 *        can never orphan a tool_result from its tool_use, and can never leave
 *        the history starting on an assistant message.
 *     2. The in-flight epoch is never evicted. If a single turn cannot fit on
 *        its own, that is CHAT_EHISTORY: the turn is abandoned and reported,
 *        not silently truncated into an invalid request.
 *     3. A turn that fails for *any* reason is rolled back in full. Only turns
 *        that ran to a final assistant message are remembered, so the stored
 *        history is always a valid, strictly alternating conversation.
 *
 *   The request buffer is a second, independent bound (CHAT_REQ_BYTES, sized to
 *   stay inside the TLS transport's own request framing buffer). If a request
 *   overflows it, the loop drops the oldest epoch and rebuilds rather than
 *   failing — so the machine degrades to a shorter memory instead of dying.
 *
 * WHAT THE OPERATOR SEES
 *   Prose is the model. [Brackets] are the kernel. Tool trace lines come from
 *   trace.h via tool_dispatch(), so they are emitted in C from real return
 *   values and cannot be forged by the model. This module's own bracket lines
 *   (transport failures, the iteration cap, history eviction) deliberately do
 *   NOT go through trace.h: trace_count() must stay an exact count of kernel
 *   actions the model caused, not of things the loop had to say.
 *
 * DEPENDENCIES
 *   model.h, tool.h, json.h, trace.h, kernel.h. No transport, no lwIP, no
 *   hardware — chat.c is compiled into the host test binary as-is and driven
 *   against net/model_mock.c, which is how the loop is tested with no network,
 *   no API key and no QEMU (tests/host/test_chat.c).
 *
 * FUTURE EXTENSION POINTS
 *   - Per-channel output: input_last_source() already says which console a
 *     sentence arrived on; routing this module's prose and trace lines there is
 *     a change of output function, not of shape.
 *   - Streaming: the loop reads whole response bodies today. A streaming
 *     transport would feed the same block walk incrementally.
 *   - Confirmation for TOOL_MUTATES tools slots into the dispatch step.
 *   - Spilling evicted epochs into the VFS would turn eviction into paging and
 *     give the machine a conversation that outlives its RAM budget.
 *   - The action journal is the natural place to hang a persisted audit log:
 *     the same entries appended to a VFS file would survive chat_init(), and
 *     (once the VFS has a disk behind it) a reboot.
 *   - The budget note is the only kernel voice inside the conversation. Cost
 *     accounting ("you have spent N tokens of context") would go in the same
 *     block, since it is the same class of fact: something only the loop knows.
 */
#ifndef CHAT_H
#define CHAT_H

#include <stdint.h>
#include <stddef.h>

#include "model.h"
#include "tool.h"       /* TOOL_NAME_MAX, for the action journal below */

/* ---- result codes (errno-style negatives; 0 = the turn completed) ---- */
#define CHAT_OK           0
#define CHAT_EINVAL     -22    /* bad arguments                                */
#define CHAT_ENOSPC     -28    /* a fixed buffer could not hold the turn       */
#define CHAT_ETRANSPORT -70    /* no transport, or the exchange failed         */
#define CHAT_EPROTO     -72    /* the response was not an intelligible turn    */
#define CHAT_EHTTP     -100    /* the API answered, but not with 200           */
#define CHAT_ELIMIT    -101    /* CHAT_MAX_ROUNDS tool rounds without an answer*/
#define CHAT_EHISTORY  -102    /* one turn does not fit the history at all     */
#define CHAT_EREENTER  -103    /* chat_ask() called from inside a chat_ask()   */

/* ---- bounds. Every one of these is a hard, static limit. ---- */

/* Model round-trips per operator sentence. The last one is still allowed to
 * call tools; hitting the cap aborts the turn rather than answering blind.
 *
 * WHY 16 AND NOT 8. The cap has to be measured against the shape of real work
 * on this machine, which is inspect -> mutate -> read back, per step, because
 * the model is told not to assume a mutation landed. One device brought up
 * through the driver VM already costs four rounds (driver_targets,
 * driver_assemble, driver_run, answer — vm/transcripts/dvm_bringup.h, replayed
 * in tests/host/test_dvm_tools.c). A two-part job with verification —
 * "make /var/log, write the boot summary into it, then tell me it is there" —
 * costs make_dir, write_file, read_file, stat_path and an answer, and one
 * mistake anywhere in it costs two more rounds to notice and repair. Eight
 * rounds fits the job only when nothing goes wrong, which is exactly the case
 * that did not need a cap. Sixteen fits the job plus a recovery.
 *
 * The cost of the higher cap is bounded and paid only by a stuck model: rounds
 * are strictly sequential HTTP requests, so the worst case is 16 round-trips of
 * wall clock, after which the machine comes back to the prompt regardless. */
#define CHAT_MAX_ROUNDS        16

/* tool_use blocks honoured in a single assistant turn. Excess blocks are still
 * answered — with a refusal — because every tool_use needs a matching result. */
#define CHAT_MAX_TOOL_CALLS    8

/* Retries per turn that do NOT spend a round: rate limits, overload and
 * transport failures, which say nothing about the work and everything about the
 * link. Bounded per turn (not per round) so a permanently rate-limited API
 * cannot hold the prompt hostage. */
#define CHAT_SEND_RETRIES      3

/* Bytes of raw JSON message content held across the whole conversation. */
#define CHAT_HISTORY_BYTES     16384

/* Messages held across the whole conversation (user + assistant turns). */
#define CHAT_HISTORY_MSGS      32

/* Result text budget handed to one tool. Below tool.h's TOOL_RESULT_MAX on
 * purpose: eight 4 KiB results cannot fit in one request body. */
#define CHAT_TOOL_RESULT_CAP   1024

/* The assembled "tools":[...] schema. This is the model's whole syscall surface
 * written out longhand, and it is the fastest-growing number in this header:
 * 18 tools cost ~8 KiB, 39 cost 29.7 KiB, 46 cost ~34 KiB — call it 750 bytes
 * of description and JSON Schema per tool. Overflowing it offers the model NO
 * tools at all, loudly, rather than advertising a contract it cannot read; that
 * is the right failure but it is a total one, so the bound is kept ahead of the
 * registry deliberately.
 *
 * THE CLIFF THIS WAS ON, and how it was paid for rather than shaved.
 * One request must hold this schema, the system prompt and the history (up to
 * CHAT_HISTORY_BYTES), inside CHAT_REQ_BYTES, which in turn must stay inside
 * net/net.c's HTTP framing buffer. At 49 tools those numbers were
 *
 *     39125 + 16384 + 4506 + 1400 = 61415 of CHAT_REQ_BYTES = 61440
 *
 * i.e. TWENTY-FIVE bytes of slack in the worst-case request, against a framing
 * buffer of 64 KiB. The paragraph that used to live here said, correctly, that
 * tool 50 could not be paid for by cutting prose again and that the STRUCTURE
 * had to change first: "send the schema once per conversation rather than once
 * per round, or page it, or grow net/net.c's framing buffer so CHAT_REQ_BYTES
 * can move."
 *
 * The network tool family (tools/net_tools.c, four verbs, ~3 KiB of schema) is
 * the change that needed it, and the third option is the one that was taken,
 * because it is the only one of the three that is a size change rather than a
 * protocol change:
 *
 *     net/net.c   TLS_REQ_BYTES   65536 -> 81920   (the framing buffer)
 *     here        CHAT_REQ_BYTES  61440 -> 77824
 *     here        CHAT_TOOLS_BYTES 40960 -> 49152
 *
 * AND IT HAD TO BE PAID AGAIN, ONCE, WITH THE MACHINE DEAD IN BETWEEN. Read this
 * before adding a tool family, because it is what the failure actually looks like.
 * A capability family (core/capability.c + tools/capability_tools.c, four verbs)
 * took the registry from 42909 to 49238 bytes — 86 bytes OVER CHAT_TOOLS_BYTES.
 * The overflow is not a truncation: net/chat.c's tools_load() sets tools_len = 0
 * and the model is then offered NO TOOLS AT ALL, so the machine boots, talks, and
 * can do nothing whatever. The boot banner read "57 registered (0 bytes of
 * schema)" and three QEMU cases failed on it. Eighty-six bytes is the whole
 * distance between a working agent and an inert one, which is the argument for
 * keeping real slack rather than the minimum that fits:
 *
 *     net/net.c   TLS_REQ_BYTES   81920 -> 90112
 *     here        CHAT_REQ_BYTES  77824 -> 86016
 *     here        CHAT_TOOLS_BYTES 49152 -> 57344
 *
 * The worst case is now 57344 + 16384 + 4506 + 1400 = 79634 of 86016, i.e. 6382
 * bytes of real slack in the request, with 8172 bytes spare inside
 * CHAT_TOOLS_BYTES itself; net/net.c carries a _Static_assert that its buffer
 * still holds CHAT_REQ_BYTES plus headers, so the two numbers cannot drift apart
 * silently the way this comment's predecessor did. The cost is ~24 KiB more BSS on
 * a machine with 128 MiB.
 *
 * WHAT THAT DOES NOT BUY. It buys ROOM, not a different design. The schema is
 * still retransmitted in full on every round of every turn, so the real cost of
 * tool 57 is paid in tokens and latency on every request, and the structural fix
 * (send it once, or page it) is still the right one. This is the SECOND time the
 * buffers have been grown to pay for a tool family, and growing them a third time
 * would be an admission that nobody intends to do the structural work: 57 tools
 * averaging 863 bytes of schema is already ~13k tokens on every single request, on
 * a machine whose whole point is a conversation. Read the two numbers below
 * together before adding a family: the binding limit is whichever is closer.
 *
 * WHERE THE REGISTRY ACTUALLY IS, so nobody re-derives it from a stale comment.
 * The boot banner prints the truth ("tools : N registered (M bytes of schema)"),
 * and that number lives in exactly one place in this tree — CHAT_REGISTRY_BYTES
 * below — which tests/host/test_app.c, tests/host/test_agency.c and
 * tests/qemu/harness.py all read, and which `make test-qemu` FAILS on if the
 * kernel disagrees with it.
 *
 * The version of this paragraph before last said 4008 bytes of slack, computed
 * from a CHAT_REGISTRY_BYTES that was 4 KiB stale — so the slack it advertised
 * had already been spent. Do not "fix" a red guard by raising a constant without
 * also raising the one below it in the chain; that is what the static assert in
 * net/net.c is there to stop. */
#define CHAT_TOOLS_BYTES       57344

/* WHAT THE 45-TOOL REGISTRY ASSEMBLES TO TODAY, in bytes, measured.
 *
 * WHY A CONSTANT AND NOT A COMMENT. No host test links all 45 tools — each test
 * binary links the two or three families it exercises — so the only place the
 * real total exists is the boot banner, and the last three times it was written
 * down it was written as a literal inside a printf, in three files, and drifted
 * 699 bytes in the UNSAFE direction while every suite stayed green. The suites
 * were printing it as a measured fact and this header was pointing at them as
 * the authority, so the next agent sizing a description would have believed it
 * had 699 bytes it did not have.
 *
 * This is now one number with teeth: `make test-qemu` parses the banner and
 * FAILS if the kernel disagrees with it (tests/qemu/harness.py, tool_registry's
 * expect_schema_bytes). Growing a description is therefore a two-line change —
 * the description, and this — and forgetting the second line is loud rather
 * than silent. Get the new value from the banner, never by arithmetic on the
 * old one; that is precisely how the last drift happened. */
/* Read off the banner. NOTE FOR WHOEVER GROWS A DESCRIPTION NEXT: the worst-case
 * request is CHAT_TOOLS_BYTES + CHAT_HISTORY_BYTES + the system prompt +
 * framing, and tests/host/test_agency.c and test_app.c assert it fits
 * CHAT_REQ_BYTES. When this value was stale at 35142 those two guards were
 * passing on a number 4 KiB too small, so the overflow they exist to catch had
 * already happened and was invisible. Read the banner, set this, and if the
 * guards then fail, something has to get shorter - the failure is real. */
/* 2026-07-30: read off the banner of a default `make -j8` build carrying the
 * fault_patch + agenda_save/list/control family (61 tools). The previous value
 * was 48389 at 57 tools; 52821 was this value before capability_save's
 * description gained the 200-byte paragraph saying that a program developed
 * under driver_run's /vm root is DENIED under a capability's data root, which
 * cost a live turn six wasted tool rounds hunting for where /vm mapped. */
/* 2026-07-30, later the same day: 56309 at 64 tools, read off the banner. The
 * +3288 is the C compiler family (cc_compile, cc_call, cc_source; see
 * include/cc.h), of which 384 bytes are the fixed-width panel that advertises
 * the saved programs and 1709 are cc_compile's description, which is the C
 * SUBSET MANUAL and is the reason that family is three tools and not five.
 *
 * READ THIS BEFORE ADDING ANYTHING: 57344 - 56309 leaves 1035 BYTES. Not 8 KiB,
 * not "a bit". The next description that grows by more than a kilobyte has to
 * either take it out of an existing one or make the case for the structural fix
 * this file has now asked for twice (send the schema once, or page it). The
 * compiler family was trimmed by 557 bytes on the way in - a verbose
 * per-property input_schema, which said what its description already said - to
 * leave that 1035 rather than 478. */
#define CHAT_REGISTRY_BYTES    56309

/* The user turn that carries a round's tool_result blocks plus the kernel's
 * budget note.
 *
 * Eight results at CHAT_TOOL_RESULT_CAP is already 8 KiB of text before any
 * escaping, and each block also carries a model-chosen tool_use_id (up to
 * TOOL_ID_MAX, six bytes per byte once escaped), so a full round of maximum-size
 * results does NOT fit here and never did. What changed is who pays for that:
 * put_tool_result() now divides the room between the calls still waiting to be
 * answered, so this number decides how much of each result survives — never
 * whether a result exists at all. Growing it buys longer results, not
 * correctness; the correctness comes from the division. */
#define CHAT_BLOCKS_BYTES      8192

/* One request body: the system prompt, every tool schema, and the history.
 * Must stay inside net/net.c's HTTP framing buffer, which also holds the
 * headers — going over would earn a MODEL_ENOSPC from the transport instead of
 * a clean, evict-and-retry one from here.
 *
 * WHY THIS GREW, TWICE. It grew to 61440 because the schema did: 39 tools
 * serialise to ~30 KiB, and 30 KiB of tools plus a full 16 KiB history plus this
 * prompt exceeded 48 KiB — at which point the loop starts evicting live
 * exchanges on every request to make a body fit, i.e. the machine silently
 * forgets the middle of the job it is doing. Forgetting is the correct behaviour
 * when the request really is too big; it is the wrong behaviour when the buffer
 * was simply undersized.
 *
 * It grew to 77824 with the network tool family, for the same reason one level
 * up: at 61440 the worst-case request had 25 bytes of slack. That was only
 * payable by growing net/net.c's framing buffer first (TLS_REQ_BYTES, now
 * 81920), which is the order the two MUST move in — and net/net.c now carries a
 * _Static_assert that refuses to build if this number ever gets ahead of it.
 * Raising this alone will not compile. */
#define CHAT_REQ_BYTES         86016

/* One response body. Whole-body buffered; the transport does not stream. */
#define CHAT_RESP_BYTES        65536

/* ---- lifecycle ---- */

/* Bind the transport the loop talks through and clear the conversation, the
 * counters and the action journal. `t` may be NULL — that is the "machine booted
 * with no network" case, and chat_ask() then reports it instead of pretending.
 *
 * kernel/main.c also calls this when a lost DNS resolve is retried and the model
 * becomes reachable mid-session. That drops the journal along with the
 * conversation, which is the consistent choice: with no history to resume from,
 * a record of earlier actions has nothing to attach to. */
void chat_init(model_transport_t *t);

/* Forget the conversation. Bounds, transport and counters are untouched. */
void chat_reset(void);

/* ---- the loop ---- */

/* Run one operator turn to completion: send, print, dispatch, repeat.
 * Returns CHAT_OK when the model finished with a normal answer, or one of the
 * negative codes above. Every failure path is also printed for the operator as
 * a [bracketed] line, because there is no other interface to report it on.
 *
 * NOT RE-ENTRANT, AND IT SAYS SO RATHER THAN MISBEHAVING. The turn loop drives
 * the whole exchange out of file-scope statics — the request buffer, the
 * response buffer, the block cursor, the round counter and the history epoch —
 * and it hands each tool handler a span pointing INTO the response buffer while
 * continuing to iterate a cursor into that same buffer afterwards. A nested call
 * overwrites all of it.
 *
 * That is reachable, not theoretical: kernel/main.c binds agenda_say() to
 * chat_ask(), and the model can call agenda_control {"action":"run_now"} on a
 * do=say item from inside a tool call. Before the guard, the observed damage was
 *   - the outer turn's REMAINING tool_use blocks were silently dropped: a model
 *     asked for two actions, one happened, one did not, and no trace line said
 *     so;
 *   - the nested request carried the outer assistant message with dangling
 *     tool_use blocks and no tool_result, which the Messages API rejects with a
 *     hard HTTP 400 (observed live, twice, in one turn);
 *   - stat_rounds was zeroed, so the only bound on paid rounds per turn reset;
 *   - hist_epoch was bumped and never restored, so the outer turn's rollback
 *     matched none of its own entries.
 *
 * A nested call now returns CHAT_EREENTER immediately, changes nothing, and
 * prints why. The caller gets a fact it can act on: a say item cannot run inside
 * a turn, but it will run when the machine is next idle or at boot. */
int chat_ask(const char *sentence);

/* Declare that no turn is running, whatever chat_ask() may believe.
 *
 * chat_ask()'s re-entrancy guard is a flag it sets on the way in and clears on
 * the way out — and a fault guard escaping out of a turn does not return through
 * "the way out". core/agenda.c runs a say item inside fault_guard_run(), so a
 * fatal CPU exception anywhere under chat_ask() would leave the flag set and
 * every later turn would be refused for the rest of the boot: a machine that
 * boots, survives the fault exactly as designed, and can then never be spoken to
 * again. Strictly worse than the bug the guard exists to fix.
 *
 * So: call this from anywhere that is PROVABLY outside a turn. kernel/main.c's
 * idle_work() is exactly that — it is the loop chat_ask() is called FROM — and it
 * calls this on every pass. Deliberately NOT called from core/agenda.c, which
 * would give that module a link dependency on net/chat.c that include/agenda.h
 * names as something it does not have. A no-op in the normal case. */
void chat_turn_ended(void);

/* ---- the action journal: what this machine actually ran ---- */

/* Entries kept. A ring: the oldest is overwritten, never the newest, because
 * the useful question is almost always "what did you just do". */
#define CHAT_JOURNAL_ENTRIES   32

/* First line of the tool's own result text, clipped. Enough for "wrote 34 bytes
 * to /etc/motd" or the first sentence of a failure. */
#define CHAT_ACTION_DETAIL_MAX 96

/* One dispatched tool call, recorded by the loop after the call returned.
 *
 * This is the same ground truth the operator reads in the [bracketed] trace
 * lines, kept in a form the model can be handed back. It is written from the
 * dispatch result — the tool's own is_error flag and result text — so it cannot
 * be forged by the model, and unlike the conversation history it is NOT rolled
 * back when a turn is abandoned. A turn that ran out of rounds still leaves the
 * record of the four things it did behind, which is the difference between the
 * next turn resuming a job and repeating it. */
typedef struct chat_action {
    uint32_t seq;                            /* 1-based, monotonic since boot  */
    uint16_t turn;                           /* operator turn it ran in        */
    uint8_t  failed;                         /* the tool reported is_error     */
    char     name[TOOL_NAME_MAX];            /* as dispatched (may be unknown) */
    char     detail[CHAT_ACTION_DETAIL_MAX]; /* first line of the result       */
} chat_action_t;

unsigned chat_turn(void);          /* operator turns run since boot          */
unsigned chat_action_total(void);  /* tools dispatched since boot (all turns) */
unsigned chat_action_count(void);  /* entries still in the ring               */

/* Oldest kept entry is index 0. NULL when `i` is out of range. */
const chat_action_t *chat_action_at(unsigned i);

/* ---- introspection (boot banner, tests) ---- */

size_t   chat_tools_bytes(void);        /* size of the assembled tool schema  */
size_t   chat_history_messages(void);   /* messages currently remembered      */
size_t   chat_history_bytes(void);      /* arena bytes currently in use       */
unsigned chat_last_rounds(void);        /* model round-trips in the last turn */
unsigned chat_last_tool_calls(void);    /* tools dispatched in the last turn  */
unsigned chat_evictions(void);          /* exchanges forgotten to make room   */

/* Lower the round cap (never above CHAT_MAX_ROUNDS). Lets a test prove the cap
 * without queueing eight scripted responses. 0 restores the default. */
void chat_set_max_rounds(unsigned n);

/* The system prompt the loop sends. Exposed so a test can assert the machine
 * tells the model what it is, rather than leaving it to guess. */
const char *chat_system_prompt(void);

#endif /* CHAT_H */
