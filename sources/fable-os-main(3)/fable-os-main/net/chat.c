/* chat.c — the agentic turn loop. See chat.h for the contract and for why the
 * conversation history is shaped the way it is.
 *
 * Everything here is fixed storage: one history arena, one request buffer, one
 * response buffer, one staging buffer for tool_result blocks. Nothing is
 * allocated per turn, so a turn cannot fail halfway through for want of memory
 * and the worst case is knowable by reading the constants in chat.h.
 *
 * The file knows nothing about lwIP, TLS, the e1000 or the console device — it
 * talks to model.h's transport seam and kernel.h's kputs, which is what lets
 * tests/host/test_chat.c compile it natively and drive the whole loop against
 * net/model_mock.c.
 */

#include "chat.h"
#include "kernel.h"
#include "json.h"
#include "tool.h"
#include "trace.h"

#include <stdio.h>      /* snprintf, via port/stdio.h when freestanding */

/* ====================================================================== */
/* the output budget — how much the model is allowed to SAY in one round    */
/* ====================================================================== */

/* THIS NUMBER DECIDES WHETHER THE MODEL CAN AUTHOR AN APP AT ALL, and it was
 * 1024 tokens, which is why it could not.
 *
 * A live session found it the only way it can be found. "I want a window that
 * says hello only" worked first try (a 128-byte document). "I want a calculator"
 * then failed FIFTEEN rounds in a row, every one of them
 * `[app action=launch -> EINVAL]`, with the model saying "I keep failing to
 * attach the document" and attaching it every time. It was not lying and the
 * tool was not wrong: a calculator document is ~4 KiB of dense JSON (see
 * apps/examples/calculator.json, 4058 bytes), the API stopped generating at
 * max_tokens while still inside the tool_use block, and a tool_use cut off
 * mid-arguments comes back with `"input":{}`. The kernel then correctly reported
 * that "document" was missing — an error the model could not act on, because
 * from where it stood it had sent one. That is a money-burning infinite loop
 * whose cause is invisible from both ends, and no mock can reproduce it.
 *
 * WHY 3072 AND NOT MORE. The reply lands in three fixed buffers:
 *   - CHAT_RESP_BYTES (65536) holds the whole HTTP response: never the binding
 *     constraint here.
 *   - CHAT_HISTORY_BYTES (16384) must hold the assistant turn VERBATIM, because
 *     a tool_use block has to be echoed back byte for byte next round. This is
 *     the real ceiling. 3072 tokens cannot exceed 12288 bytes even at a
 *     pessimistic 4 bytes per token, which still fits one turn in the history
 *     with room for the tool_result that answers it. 4096 tokens does not carry
 *     that guarantee.
 *   - CHAT_REQ_BYTES (61440) is unaffected: what goes out is bounded by the
 *     history arena, not by this number.
 * It is also 3x the room the largest documented app document needs, so the
 * headline case has slack rather than sitting on the edge.
 *
 * Raising it further is not free: it is the one constant that lets a single
 * assistant turn stop fitting the history at all, which aborts the round
 * (CHAT_EHISTORY) instead of merely truncating it. If you raise it, raise
 * CHAT_HISTORY_BYTES in the same commit and re-check chat.h's request
 * arithmetic. The paired safety net is in run_turn(): a turn that DOES get cut
 * off mid-call is now named as such to the model instead of being reported as
 * missing arguments. */
#define CHAT_MAX_OUTPUT_TOKENS  3072u

/* ====================================================================== */
/* what the machine tells the model it is                                  */
/* ====================================================================== */

/* THE SYSTEM PROMPT IS PART OF THE OPERATING SYSTEM.
 *
 * It is the only place the model is told what machine it is running, and the
 * operator's whole experience turns on whether it is specific. "Be helpful and
 * use your tools" produces a model that narrates plausible kernel work; a prompt
 * that states this kernel's actual constraints produces one that inspects,
 * mutates, reads the result back and reports what really happened. So every
 * sentence below is a fact about THIS build, checkable against the source:
 *
 *   - single-threaded, ring 0, IF=0, all PIC lines masked, everything polled:
 *     boot/boot.asm, arch/x86_64/idt.c, kernel/main.c's loop. Nothing preempts
 *     anything, so nobody else is ever to blame.
 *   - TWO filesystems, and the model must not confuse them. "/" is RAM-backed
 *     (fs/native/ramfs.c) and is gone on the next boot; "/disk" is FAT32 on a
 *     real disk (fs/fat/, drivers/block/) and survives a power cycle, and it is
 *     where core/capability.c and core/agenda.c keep their stores when a disk is
 *     attached. Whether one IS attached is a run-time fact, so the prompt states
 *     the RULE and points at the tool result that states the FACT - a capability
 *     or agenda tool always names its store and says whether it is durable.
 *
 *     THIS PARAGRAPH IS LOAD-BEARING AND IT WAS WRONG FOR A WHOLE BRANCH. The
 *     prompt used to say, flatly, "the filesystem is real but lives in RAM, so
 *     anything you write is gone on the next boot". Asked live what would still
 *     be there after a power cycle, the model called capability_list, was told
 *     "store /disk/cap (survives a reboot)", and then CORRECTED THE KERNEL: it
 *     invented a distinction between a reboot and a power-off and told the
 *     operator that nothing persists and that an agenda item would not help
 *     either. Every word of that came from this comment's own stale sentence.
 *     A prompt that contradicts a tool result does not merely fail to help; the
 *     model believes the prompt, so the capability is DENIED TO THE OPERATOR
 *     while the machine has it. If a subsystem's contract changes, this string
 *     is part of that subsystem's contract.
 *   - the machine is no longer idle between sentences: core/fiber.c's ui fiber
 *     paints during a model turn and core/agenda.c runs scheduled work with
 *     nobody typing, so "nothing happens in the background" is no longer true
 *     and the prompt says what does.
 *   - still true, and the model must not pretend otherwise: no package manager
 *     and no loader, so nothing built off this machine can be brought in.
 *
 *     THIS ONE WENT STALE TOO, AND IT IS THE SAME DEFECT AS THE PARAGRAPH ABOVE.
 *     It used to end "no C compiler ... new code is authored in the driver VM's
 *     ISA and nowhere else", which stopped being true the moment compiler/ and
 *     tools/cc_tools.c landed: cc_compile builds real C into native x86-64 in
 *     this kernel and runs it. Asked live "can you compile real C, or only the
 *     little VM instruction set?", the model happened to trust the tool list
 *     over the prompt and proved it by compiling something (2/2 observed) - but
 *     that is luck, not design, and it is exactly the shape of the failure that
 *     denied GUI apps to the operator for a branch. The prompt now names both
 *     authoring paths and says which is for what: the VM reaches hardware and
 *     is bounded to a few hundred instructions (DVM_MAX_INSNS), the compiler is
 *     for arithmetic, strings, arrays and loops. Both clauses are conditional
 *     on the tool being offered, so this stays true in a build without one.
 *   - the tool list is the whole syscall surface: tools[] in this request IS
 *     tool.h's registry, assembled from it (core/tool.c), so "I do not have a
 *     tool for that" is a checkable statement and inventing a name is not.
 *   - trace lines are emitted by the kernel in C after each call returns
 *     (lib/trace.c) and a '[' in column zero cannot be produced by prose
 *     (print_model_prose below). Telling the model this is not a threat, it is
 *     information: claiming an action it did not take is not merely wrong, it is
 *     visibly wrong to the operator, and honest failure is strictly better.
 *   - the round budget is real and finite, and the loop reports how much is
 *     left after each round (see the kernel note in blocks_buf).
 *
 * The one thing this prompt must keep verbatim is its opening clause, which
 * tests/host/test_chat.c pins as "the machine tells the model what it is".
 */
static const char SYSTEM_PROMPT[] =
    "You are the resident intelligence of fable-os, a bare-metal x86-64 kernel. "
    "You are not an assistant running on the machine: you ARE its user "
    "interface. There is no shell, no commands, no filesystem browser and no "
    "other way for the operator to drive this computer. A sentence typed at the "
    "prompt is the only input; your tool calls are the only way anything "
    "happens.\n"

    "\nTHE MACHINE. One CPU, ring 0, single-threaded, no userspace, no "
    "processes, no memory protection: every tool call runs in the kernel's own "
    "address space and finishes before the next one starts. Interrupts are "
    "masked and all hardware is polled, so nothing preempts anything - but two "
    "things do run while nobody is asking: a cooperative fiber keeps the screen "
    "and the GUI apps painted, so an app's periodic \"tick\" handler keeps "
    "running between turns and even during your own API call, and the kernel's "
    "agenda runs scheduled work with nobody typing. Memory is one heap in a "
    "fixed arena. There is no package manager and no loader, so you cannot bring "
    "in a program built somewhere else - but you can WRITE code here and run it, "
    "and that is how this machine gains an ability it was not built with. Two "
    "ways: the driver VM's own instruction set, which is what reaches hardware "
    "(ports, MMIO, DMA) and is bounded to a few hundred instructions; and, if a "
    "compile tool is offered, real C compiled inside this kernel to native "
    "x86-64 and executed here, which is the one to reach for when the job is "
    "arithmetic, strings, arrays or loops rather than a device.\n"

    "\nWHERE THINGS LIVE, AND WHAT SURVIVES A POWER CYCLE. There are two "
    "filesystems and the difference is the difference between work you keep and "
    "work you lose. Everything under \"/\" is RAM-backed and is gone on the next "
    "boot; /tmp is scratch. \"/disk\" is a real FAT32 volume on a real disk: a "
    "file written there is still there after the machine is switched off and on, "
    "and so are saved capabilities and the agenda. Not every machine has a disk, "
    "so this is a fact you READ rather than assume - a capability or agenda tool "
    "always names its store and says whether it is durable, and that result is "
    "the truth. Never contradict it from memory, and never explain it away: if a "
    "tool says the store survives a reboot, it survives being switched off too. "
    "Put anything worth keeping under /disk, and when there is no disk say "
    "plainly that the work will be lost.\n"

    "\nWHAT YOU CAN DO. Exactly the tools listed in this request, and nothing "
    "else. That list is the kernel's tool registry, assembled from it "
    "mechanically - it is not a summary, so if a capability is not in it, this "
    "machine genuinely does not have it. Never invent a tool name; if you get "
    "one wrong the kernel replies with the real list, so read it and correct "
    "yourself. Say plainly when a request is outside the surface, and name the "
    "closest thing you do have.\n"

    "\nYOU CAN BUILD GUI APPS, AND THAT IS EASY TO MISS. If an app tool is "
    "offered, this machine has no fixed catalogue of applications: you WRITE one "
    "as a JSON document - labels, buttons, fields, and handlers with a small "
    "expression language - and launch it into a real clickable window, so "
    "\"I want a stopwatch\", \"a tip calculator\" or \"a window that just says "
    "hello\" is something you author in one tool call rather than something this "
    "machine lacks. Any built-in app list you see elsewhere is a couple of "
    "prebuilt demos, NOT the limit; so offer to build the thing asked for "
    "instead of substituting the nearest demo, and if the first document is "
    "rejected the error carries a working skeleton - fix it and launch again.\n"

    "\nYOU CAN KEEP WHAT YOU BUILD, AND THAT IS THE POINT OF THIS MACHINE. If a "
    "capability tool is offered, a program you write can be SAVED under a name "
    "with a one-line description and then CALLED by name - later in this "
    "conversation, and after a reboot, because the store is on the disk. So look "
    "before you build: the list of what this machine has already been taught is "
    "carried in the capability tool's own description and there is a tool that "
    "lists it in full, so call what is there instead of writing it again. "
    "Rebuilding something this machine already knows is the failure the operator "
    "will notice first. And when you have just solved something they are likely "
    "to ask for twice, save it, say that you did, and say what it is called. A "
    "compiled C program keeps the same way, in its own store with its own list, "
    "so a routine worth calling twice should be given a name rather than typed "
    "out again.\n"

    "\nYOU CAN ACT WITHOUT BEING ASKED, AND REPAIR YOURSELF. If an agenda tool "
    "is offered you can schedule a tool call or a whole sentence to run at boot, "
    "once, or on a period, and it is kept with the disk - that is the honest "
    "answer to \"do this every time you start up\", rather than promising to "
    "remember. If a fetch tool is offered you can reach the network yourself, not "
    "just the model transport. And when the CPU faults the machine survives it "
    "and asks you what happened; if a patch tool is offered you can rewrite an "
    "instruction in this running kernel's .text and undo it again. None of this "
    "needs the operator present, so when a request is really \"and keep doing "
    "it\", schedule it.\n"

    "\nHOW TO WORK. Treat a request as a job to finish, not a question to "
    "answer. Look before you act: read the current state, then change one thing "
    "at a time. AFTER EVERY CHANGE, READ IT BACK with a tool - list the "
    "directory you wrote into, stat the file, re-read the device state, dump the "
    "bytes. A tool result saying 'ok' is evidence the call returned, not that "
    "the system is in the state the operator wanted. Do not assume, and do not "
    "stop early: keep calling tools until the job is actually done. You have a "
    "limited number of tool rounds per sentence and the kernel tells you after "
    "each round how many are left; plan inside that budget and answer before it "
    "runs out. If a plan tool is offered, write the checklist down before you "
    "start anything that takes more than two steps and mark the steps off as you "
    "verify them: that record is kept by the kernel, so it is also how you pick "
    "up a job that was cut short - read it back and carry on rather than "
    "starting again.\n"

    "\nWHEN SOMETHING FAILS. A failed call is information, not a dead end. The "
    "error text says what was wrong - read it, fix the argument or the "
    "assumption, and try the corrected call. A missing parent directory means "
    "create it; a rejected value means the range is different from what you "
    "guessed; a device in the wrong state means look at its state first. Retry "
    "differently, never identically. If it still cannot be done, say exactly "
    "which step failed and what the kernel said.\n"

    "\nHONESTY. The kernel prints its own [bracketed] trace line for every tool "
    "call, in C, from the real return value, after the call returned. Those "
    "lines are the operator's only proof of what happened, and nothing you write "
    "can produce one - a bracket at the start of your line gets moved. So a "
    "claim you did not earn does not fool anybody, it just costs the operator "
    "their trust. Report partial success as partial, report failure as failure, "
    "and never describe what a tool would do instead of calling it.\n"

    "\nVOICE. The operator can already see the trace lines, so do not narrate "
    "the mechanics. Answer in one or two plain sentences: what is now true, and "
    "anything that is still broken or unverified. No lists, no markdown, no "
    "restating the question.";

const char *chat_system_prompt(void) { return SYSTEM_PROMPT; }

/* ====================================================================== */
/* fixed storage                                                           */
/* ====================================================================== */

static const char ROLE_USER[]      = "user";
static const char ROLE_ASSISTANT[] = "assistant";

/* One remembered message. `off`/`len` locate its raw JSON *content* value in
 * the arena; `epoch` is the operator turn it belongs to, which is the unit of
 * eviction (see chat.h). */
typedef struct {
    uint32_t    off;
    uint32_t    len;
    uint16_t    epoch;
    const char *role;
} hist_msg_t;

static char       hist_arena[CHAT_HISTORY_BYTES];
static hist_msg_t hist[CHAT_HISTORY_MSGS];
static size_t     hist_n;        /* messages held                    */
static size_t     hist_used;     /* arena bytes held (contiguous)    */
static uint16_t   hist_epoch;    /* the turn currently being run     */

static char   req_buf[CHAT_REQ_BYTES];
static char   resp_buf[CHAT_RESP_BYTES];
static char   tools_buf[CHAT_TOOLS_BYTES];
static size_t tools_len;
static int    tools_ready;

/* The user turn that carries this round's tool_result blocks and the kernel's
 * budget note, assembled before it is copied into the history arena. */
static char blocks_buf[CHAT_BLOCKS_BYTES];

/* One tool's answer, and one decoded text block. */
static char result_buf[CHAT_TOOL_RESULT_CAP];
static char text_buf[8192];

static model_transport_t *transport;

static unsigned max_rounds = CHAT_MAX_ROUNDS;
static unsigned stat_rounds;
static unsigned stat_tool_calls;
static unsigned stat_evictions;
static unsigned stat_retries;      /* link failures retried in this turn */

/* ====================================================================== */
/* the action journal — what the kernel actually ran                       */
/* ====================================================================== */

/* A ring, so a long session keeps the most recent actions rather than the first
 * ones. Deliberately NOT rolled back with the conversation: the conversation is
 * a record of what was said and must stay valid to be resent, while this is a
 * record of what the machine DID, and a turn that was abandoned still did it.
 * That asymmetry is the whole value — see chat.h. */
static chat_action_t journal[CHAT_JOURNAL_ENTRIES];
static unsigned      journal_total;          /* dispatched since boot */

unsigned chat_turn(void)         { return hist_epoch; }
unsigned chat_action_total(void) { return journal_total; }

unsigned chat_action_count(void) {
    return journal_total < CHAT_JOURNAL_ENTRIES ? journal_total
                                                : CHAT_JOURNAL_ENTRIES;
}

const chat_action_t *chat_action_at(unsigned i) {
    unsigned n = chat_action_count();
    if (i >= n) return (const chat_action_t *)0;
    /* Oldest kept entry first. Once the ring has wrapped, that is the slot
     * immediately after the newest. */
    unsigned base = (journal_total <= CHAT_JOURNAL_ENTRIES)
                        ? 0 : journal_total % CHAT_JOURNAL_ENTRIES;
    return &journal[(base + i) % CHAT_JOURNAL_ENTRIES];
}

/* Copy a bounded, single-line, printable-only summary. The name and the result
 * text are both reachable from model output (an unknown tool name is echoed, and
 * a tool may quote its arguments), and this text is handed back to the model
 * later, so it is sanitised at the point of record rather than at every reader:
 * no control bytes, no second line, no brackets that could be mistaken for the
 * kernel's own voice if a reader ever prints it. */
static void journal_copy(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    if (cap == 0) return;
    for (size_t i = 0; src && src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r') break;             /* first line only */
        if (c < 0x20 || c == 0x7F) continue;           /* not text */
        if (c == '[') c = '{';
        if (c == ']') c = '}';
        dst[j++] = (char)c;
    }
    /* Trailing spaces carry nothing and make an equality test surprising. */
    while (j && dst[j - 1] == ' ') j--;
    dst[j] = '\0';
}

/* Record one dispatched call, from the dispatch result — never from the model's
 * account of it. */
static void journal_record(const char *name, const char *detail, int failed) {
    chat_action_t *e = &journal[journal_total % CHAT_JOURNAL_ENTRIES];

    journal_total++;
    e->seq    = journal_total;
    e->turn   = hist_epoch;
    e->failed = failed ? 1 : 0;
    journal_copy(e->name, sizeof e->name, (name && name[0]) ? name : "(no name)");
    journal_copy(e->detail, sizeof e->detail, detail);
}

/* ====================================================================== */
/* history                                                                 */
/* ====================================================================== */

/* IN A TURN. See the re-entrancy paragraph on chat_ask() in include/chat.h for
 * what a nested call used to do; this is the whole of the defence. A flag and
 * not a counter, because there is no sane meaning for "two levels of turn": the
 * buffers are singletons. Cleared on every exit path, including the early ones,
 * which is why chat_ask() is a thin wrapper around chat_ask_body(). */
static int in_turn;

void chat_reset(void) {
    in_turn    = 0;
    hist_n     = 0;
    hist_used  = 0;
    hist_epoch = 0;
}

static void tools_load(void);

void chat_init(model_transport_t *t) {
    transport = t;
    chat_reset();
    stat_rounds = stat_tool_calls = stat_evictions = stat_retries = 0;
    max_rounds  = CHAT_MAX_ROUNDS;
    tools_ready = 0;
    journal_total = 0;
    memset(journal, 0, sizeof journal);
    /* Assemble the schema now rather than on the first sentence: its size is a
     * fixed property of this build, and the operator should learn at boot — not
     * mid-turn — if the machine's own syscall surface does not fit. */
    tools_load();
}

size_t   chat_history_messages(void) { return hist_n; }
size_t   chat_history_bytes(void)    { return hist_used; }
unsigned chat_last_rounds(void)      { return stat_rounds; }
unsigned chat_last_tool_calls(void)  { return stat_tool_calls; }
unsigned chat_evictions(void)        { return stat_evictions; }
size_t   chat_tools_bytes(void)      { return tools_len; }

void chat_set_max_rounds(unsigned n) {
    max_rounds = (n == 0 || n > CHAT_MAX_ROUNDS) ? CHAT_MAX_ROUNDS : n;
}

/* Drop every message of the oldest epoch present and compact the arena.
 * Returns 1 if anything was dropped. The in-flight epoch is never touched:
 * a turn is only valid as a whole, and half of one is worse than none. */
static int hist_drop_oldest_epoch(void) {
    if (hist_n == 0) return 0;

    uint16_t victim = hist[0].epoch;
    if (victim == hist_epoch) return 0;      /* only the in-flight turn is left */

    size_t k = 0;
    while (k < hist_n && hist[k].epoch == victim) k++;

    size_t bytes = hist[k - 1].off + hist[k - 1].len;   /* entries are in order */
    memmove(hist_arena, hist_arena + bytes, hist_used - bytes);
    hist_used -= bytes;

    for (size_t i = k; i < hist_n; i++) {
        hist[i - k]      = hist[i];
        hist[i - k].off -= (uint32_t)bytes;
    }
    hist_n -= k;
    stat_evictions++;
    return 1;
}

/* Forget the OLDEST FAILED ATTEMPT OF THE TURN IN FLIGHT, when there is nothing
 * else left to forget. Returns 1 if anything was dropped.
 *
 * WHY THIS EXISTS. Raising the output budget so the model can write a real app
 * document moved the wall rather than removing it: a live session authoring a
 * calculator sent five documents of 1.5-4 KiB, each with its rejection, and
 * CHAT_HISTORY_BYTES (16 KiB) ran out mid-turn. hist_drop_oldest_epoch() could
 * not help — by then the in-flight turn was the only epoch left — so the turn
 * died with "no room left to remember this turn". Twice that happened AFTER a
 * successful launch: the window was on screen and the conversation that could
 * say so was abandoned. That is the worst outcome this file can produce.
 *
 * WHAT IS SAFE TO DROP, exactly. The turn's messages are [the operator's
 * sentence, then (assistant with tool_use, user with its tool_result) pairs].
 * Dropping one WHOLE ADJACENT PAIR keeps every invariant the API needs: the
 * first message is still the user's sentence, roles still alternate, and no
 * tool_use is left without its tool_result (a dangling one is a 400 from the
 * API, which would cost the operator the turn as surely as running out of room).
 * The pair dropped is the oldest, which is the least useful: it is the attempt
 * the model has already moved on from, while the errors it is actually working
 * from are the most recent ones.
 *
 * It never touches hist[0], never crosses into an older epoch (those are
 * cheaper to drop whole, and hist_drop_oldest_epoch runs first), and does
 * nothing at all unless the two entries really are an assistant/user pair — so
 * a history shaped in some way this reasoning did not anticipate is left alone
 * and the old CHAT_EHISTORY path still catches it. */
static int hist_drop_oldest_attempt(void) {
    if (hist_n < 3)                     return 0;
    if (hist[0].epoch != hist_epoch)    return 0;   /* older exchanges first */
    if (hist[1].epoch != hist_epoch ||
        hist[2].epoch != hist_epoch)    return 0;
    if (hist[1].role != ROLE_ASSISTANT ||
        hist[2].role != ROLE_USER)      return 0;

    size_t off   = hist[1].off;
    size_t bytes = (size_t)hist[1].len + hist[2].len;
    memmove(hist_arena + off, hist_arena + off + bytes,
            hist_used - off - bytes);
    hist_used -= bytes;

    for (size_t i = 3; i < hist_n; i++) {
        hist[i - 2]      = hist[i];
        hist[i - 2].off -= (uint32_t)bytes;
    }
    hist_n -= 2;
    stat_evictions++;
    /* The operator is watching a turn take many rounds; this says why it is
     * still going and what it cost. No model-chosen bytes (include/trace.h). */
    kprintf("[chat: this turn no longer fits its memory - forgot its oldest "
            "failed attempt (%u bytes) and carried on]\n", (unsigned)bytes);
    return 1;
}

/* Make room for `need` bytes of content, evicting whole old exchanges as
 * required. Returns where to write, or NULL when even an empty history cannot
 * hold this turn (CHAT_EHISTORY territory). */
static char *hist_reserve(size_t need) {
    for (;;) {
        if (hist_n < CHAT_HISTORY_MSGS && hist_used + need <= CHAT_HISTORY_BYTES)
            return hist_arena + hist_used;
        if (hist_drop_oldest_epoch())  continue;
        if (hist_drop_oldest_attempt()) continue;
        return (char *)0;
    }
}

static void hist_commit(const char *role, size_t len) {
    hist[hist_n].off   = (uint32_t)hist_used;
    hist[hist_n].len   = (uint32_t)len;
    hist[hist_n].epoch = hist_epoch;
    hist[hist_n].role  = role;
    hist_used += len;
    hist_n++;
}

/* Append a raw JSON content value (an array of blocks, typically). */
static int hist_append_raw(const char *role, const char *json, size_t len) {
    char *dst = hist_reserve(len);
    if (!dst) return CHAT_EHISTORY;
    memcpy(dst, json, len);
    hist_commit(role, len);
    return CHAT_OK;
}

/* Append plain text as a JSON string, escaped on the way in. The escaped size
 * is measured first (json_escape with a zero capacity is a pure length query),
 * so no scratch buffer is needed and an over-long sentence is refused before
 * anything is written. */
static int hist_append_text(const char *role, const char *text) {
    size_t need = json_escape((char *)0, 0, text) + 2;   /* + the two quotes */

    /* One byte over: the writer keeps its buffer NUL-terminated, and that NUL
     * lives past the content the history actually keeps. */
    char *dst = hist_reserve(need + 1);
    if (!dst) return CHAT_EHISTORY;

    json_writer_t w;
    json_writer_init(&w, dst, need + 1);
    json_put_string(&w, text);
    if (!json_writer_ok(&w)) return CHAT_EHISTORY;

    hist_commit(role, json_writer_len(&w));
    return CHAT_OK;
}

/* Undo everything the in-flight turn appended. A turn that did not reach a
 * final assistant message must leave no trace in the history, or the next
 * request would carry a dangling tool_use, or two user turns in a row. */
static void hist_rollback_epoch(void) {
    while (hist_n && hist[hist_n - 1].epoch == hist_epoch) {
        hist_n--;
        hist_used = hist[hist_n].off;
    }
}

/* ====================================================================== */
/* request assembly                                                        */
/* ====================================================================== */

/* Reassemble the tool schema. Called at the start of every turn, not once.
 *
 * IT USED TO BE `if (tools_ready) return;` AND THAT MADE THE LIVE PANELS DEAD.
 * Two tools carry a mutable fixed-width panel at the tail of their description —
 * capability_call's list of taught capabilities (tools/capability_tools.c) and
 * cc_call's list of kept programs (tools/cc_tools.c) — and both are faithfully
 * re-rendered into their description buffers whenever their registry changes.
 * tools_build_schema() COPIES those descriptions into tools_buf, so a one-shot
 * snapshot froze them at boot. Nothing saved during a session was ever visible
 * to the model again.
 *
 * The consequence was worse than a stale list: with an empty store at boot the
 * panel reads "NONE SAVED YET - this machine has not been taught anything
 * beyond its built-in tools", and that sentence stayed in the model's own
 * instructions for the whole session, immediately after it had saved something.
 * An actively false statement in the tool schema, contradicted by the
 * tool_result still sitting in the history. Only a power cycle fixed it, which
 * is exactly why a boot-2 observation made the mechanism look like it worked.
 *
 * REBUILDING IS SAFE AND CHEAP, and both halves are load-bearing. Safe because
 * the panels are FIXED WIDTH by construction — space-padded, no quote, no
 * backslash, no control byte — so the serialised schema is byte-for-byte the
 * same size with 0, 1 or 16 entries saved, which
 * tests/host/test_capability.c and tests/host/test_cc.c both assert. Cheap
 * because this runs once per operator sentence, not once per round, against a
 * network round trip that costs three orders of magnitude more.
 *
 * AND A FAILED REBUILD KEEPS THE LAST GOOD SCHEMA. Zeroing tools_len offers the
 * model NO TOOLS AT ALL — a machine that boots, talks, and can do nothing —
 * and turning a mid-session panel change into that is a far worse outcome than
 * a stale list. Only a build that has never succeeded is allowed to end up
 * empty. */
static void tools_load(void) {
    /* Measure before writing. tools_build_schema(NULL, 0) returns the length it
     * WOULD need and touches nothing, which is what makes "keep the last good
     * schema" implementable without a second 56 KiB buffer: a rebuild that
     * cannot fit is never started, so the bytes already in tools_buf survive. */
    if (tools_ready && tools_build_schema((char *)0, 0) >= sizeof tools_buf) {
        kprintf("[chat: the tool schema has grown past %u bytes since boot - "
                "the model keeps the schema from the start of this boot, so any "
                "tool added or renamed since is invisible to it]\n",
                (unsigned)sizeof tools_buf);
        return;
    }

    size_t need = tools_build_schema(tools_buf, sizeof tools_buf);

    /* Validate rather than trust the length. A schema clipped mid-array is not
     * a short schema, it is a corrupt request body, and the model would be told
     * about tools whose contract it cannot read. Parsing is also the only
     * reliable overflow signal here: tools_build_schema() returns the bytes it
     * actually wrote, so a chunk the writer refused whole leaves a length that
     * looks like it fitted. (Worth fixing in core/tool.c; not this file's.) */
    json_value_t v;
    if (need >= sizeof tools_buf || json_parse(tools_buf, need, &v) != JSON_OK) {
        kprintf("[chat: the tool schema does not fit %u bytes - the model will "
                "be offered %s]\n", (unsigned)sizeof tools_buf,
                tools_ready ? "no tools at all (the schema became invalid "
                              "during this boot)"
                            : "no tools at all");
        /* Getting here after a successful boot build means the schema FITS but
         * does not PARSE, i.e. some description or input_schema became invalid
         * during the session. There is nothing good left in the buffer to keep,
         * so this is the one case that really does end with no tools — and it
         * says which of the two situations it is. */
        tools_buf[0] = '\0';
        tools_len    = 0;
        return;
    }
    tools_len   = need;
    tools_ready = 1;
}

/* On the final permitted round the request is sent with tool calling turned OFF.
 * That is what converts the round cap from a silent abort into an answer: the
 * model was told in the previous round's budget note that this round is its
 * last, and with no tool available the only continuation left is the report the
 * operator actually needs.
 *
 * WHY THIS IS `tool_choice` AND NOT "OMIT THE tools ARRAY". Dropping the schema
 * looks like the obvious way to withhold the tools, and it is the version that
 * quietly breaks: by the final round the history necessarily contains tool_use
 * and tool_result blocks, and the API's documented behaviour for a conversation
 * whose history references a tool no longer present in `tools` is a 400 (it is
 * spelled out for the advisor tool, and a whole beta exists for removing a tool
 * mid-conversation properly). A 400 on the last round is strictly worse than
 * today's abort — the operator would get an HTTP error instead of a report.
 * `{"type":"none"}` is the supported, documented way to say "answer, do not
 * call anything", and it leaves the schema in place so every tool_use in the
 * history still resolves.
 *
 * It is spliced in here rather than passed through model.h because net/model.c
 * has no tool_choice field and is not this agent's to change — see the report.
 * The splice is the same shape net/net.c already uses for "stream":true: insert
 * one member immediately after the opening brace of an object we just built,
 * then re-parse the result and fall back to the untouched body if it did not
 * come out valid. Nothing model-controlled reaches this text. */
static const char TOOL_CHOICE_NONE[] = "{\"tool_choice\":{\"type\":\"none\"},";

static int forbid_tool_calls(char *body, size_t len, size_t cap, size_t *out_len) {
    size_t add = sizeof TOOL_CHOICE_NONE - 2;        /* without the '{' and NUL */

    /* Only ever edit something that really is the object model_build promises. */
    if (len < 2 || body[0] != '{' || body[1] != '"') return -1;
    if (len + add + 1 > cap)                         return -1;

    memmove(body + add + 1, body + 1, len - 1);
    memcpy(body, TOOL_CHOICE_NONE, add + 1);

    size_t n = len + add;
    body[n] = '\0';

    /* Re-read our own output with the parser that faces the network, exactly as
     * model_build does: a body we cannot parse is never worth sending. */
    json_value_t v;
    if (json_parse(body, n, &v) != JSON_OK || v.type != JSON_OBJECT) return -1;

    *out_len = n;
    return 0;
}

/* Build the next request from the whole history. On overflow, forget the
 * oldest exchange and try again — a shorter memory beats a dead machine. */
static int build_request(size_t *out_len, int offer_tools) {
    static model_msg_t msgs[CHAT_HISTORY_MSGS];

    tools_load();

    size_t offer_len = tools_len;

    for (;;) {
        for (size_t i = 0; i < hist_n; i++) {
            msgs[i].role        = hist[i].role;
            msgs[i].content     = hist_arena + hist[i].off;
            msgs[i].content_len = hist[i].len;
            msgs[i].raw         = 1;
        }

        model_request_t rq;
        rq.model         = (const char *)0;      /* model.c's compiled default */
        rq.max_tokens    = CHAT_MAX_OUTPUT_TOKENS;
        rq.system        = SYSTEM_PROMPT;
        rq.tools         = offer_len ? tools_buf : (const char *)0;
        rq.tools_len     = offer_len;
        rq.messages      = msgs;
        rq.message_count = hist_n;

        int n = model_build(req_buf, sizeof req_buf, &rq);
        if (n >= 0) {
            size_t len = (size_t)n;
            /* A failed splice is not a failed turn: the body is still the valid
             * request model_build produced, and a model that answers with a tool
             * call on its last round is handled by the cap as before. */
            if (!offer_tools)
                forbid_tool_calls(req_buf, len, sizeof req_buf, &len);
            *out_len = len;
            return CHAT_OK;
        }

        if (n == MODEL_ENOSPC && hist_drop_oldest_epoch()) {
            kputs("[chat: request over budget - forgot the oldest exchange]\n");
            continue;
        }
        if (n == MODEL_ENOSPC) {
            kputs("[chat: this turn alone does not fit one request]\n");
            return CHAT_ENOSPC;
        }
        kputs("[chat: could not build a valid request]\n");
        return CHAT_EINVAL;
    }
}

/* ====================================================================== */
/* printing                                                                */
/* ====================================================================== */

/* The model's voice: plain prose, no prefix. Always ends on a fresh line so a
 * kernel trace line never starts mid-sentence.
 *
 * FORGERY RESISTANCE — this is the model's largest uncontrolled output channel.
 *   trace.c escapes brackets and control bytes in the argument and op fields of
 *   a trace line, so a hostile *path* cannot fabricate kernel output. But the
 *   reply text printed here is entirely model-chosen, and a bare kputs() of it
 *   would let the model simply TYPE
 *
 *       [vfs_write /etc/shadow bytes=9999 -> ok]
 *
 *   producing bytes indistinguishable from genuine ground truth. That would
 *   defeat the whole point of the trace mechanism, since the operator has no
 *   `ls` with which to check.
 *
 *   The invariant this function enforces is the cheapest one that cannot be
 *   talked around: A KERNEL TRACE LINE IS A '[' IN COLUMN ZERO. Model prose is
 *   never allowed to put one there — a '[' that would land in column zero gets a
 *   single leading space. Prose is otherwise untouched, so the model can still
 *   write about brackets, quote a trace line back, or use them mid-sentence;
 *   it just cannot occupy the one position that means "the kernel says so".
 *
 *   TWO WAYS THAT INVARIANT WAS ESCAPED, AND WHY IT IS NOW WRITTEN LIKE THIS.
 *   The first version of this function tracked line starts itself, with
 *   `at_line_start = (c == '\n')`. That is a MODEL of the console, and the
 *   console disagreed with it in three places (all reproduced against this
 *   function, all now regression-tested in tests/host/test_chat.c):
 *
 *     1. '\r' — kputc() resets the column on a carriage return, and json.c
 *        decodes "\r" into a raw 0x0D. `x\r[vfs_write ... -> ok]` therefore put
 *        a byte-exact forgery in column zero.
 *     2. '\b' — kputc() steps the cursor left, and from column zero it steps UP
 *        a row. So backspaces reach column zero, and enough of them walk back
 *        into a GENUINE trace line printed earlier and overwrite it. That is
 *        worse than forgery: it is erasure.
 *     3. wrap — printing in the last column returns the cursor to column zero
 *        with no newline involved at all. Exactly 80 printable characters of
 *        prose, then '[', and the forgery lands.
 *
 *   Both halves of the fix follow from those. First, prose does not get to move
 *   the cursor: every C0 control byte except '\n' and '\t' is escaped as \xNN,
 *   the same way lib/trace.c sanitizes an argument. Prose is text, not
 *   cursor control, and a reply has no legitimate reason to emit a carriage
 *   return. Second, the column is not modelled, it is ASKED FOR —
 *   console_cursor() is the console's own count, so wrap, scroll, tabs and any
 *   future console change are covered by construction rather than by a rule
 *   here that has to be kept in sync. lib/base.c owns the console; it owns the
 *   column too.
 */
/* The length-counted core. It exists because one caller — the raw HTTP body
 * fallback in print_http_error() — holds a span, not a C string: the body is
 * whatever answered on 443 and may contain embedded NULs, so stopping at the
 * first one would leave the rest of an attacker's payload unexamined while
 * kputc('\0') went to the framebuffer. Everything else calls the string form. */
static void print_untrusted_n(const char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        /* Cursor control is not prose. '\n' and '\t' are the two controls that
         * only ever move forward, so they are the two that survive. */
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) {
            kputc('\\');
            kputc('x');
            kputc(hex[(c >> 4) & 0xF]);
            kputc(hex[c & 0xF]);
            continue;
        }

        if (c == '[') {
            int col = 0;
            console_cursor((int *)0, &col);
            if (col == 0) kputc(' ');
        }
        kputc((char)c);
    }
}

static void print_model_prose(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    print_untrusted_n(s, n);
}

static void print_text_block(const json_value_t *v) {
    size_t n  = 0;
    int    rc = json_str(v, text_buf, sizeof text_buf, &n);
    if (rc != JSON_OK && rc != JSON_ENOSPC) return;
    if (n == 0) return;

    print_model_prose(text_buf);
    if (rc == JSON_ENOSPC) kputs("\n[chat: reply clipped to the print buffer]");
    if (text_buf[n - 1] != '\n') kputc('\n');
}

/* A reply the model did not finish saying. `stop_reason` is the API's own word
 * for why generation ended, and "max_tokens" on a turn with no tool calls means
 * the answer the operator is reading stops mid-thought — which looks exactly
 * like a complete answer on a console with no scrollback and no shell to check
 * with. Anything else (end_turn, tool_use, a reason this build has not heard of)
 * is silent: the operator should only be interrupted by facts they can act on. */
static int hit_output_limit(const json_value_t *root) {
    char why[32];
    if (json_msg_stop_reason(root, why, sizeof why) != JSON_OK) return 0;
    /* memcmp over the NUL, not strcmp: kernel.h declares the former and this
     * file must compile freestanding as well as against host libc. */
    return memcmp(why, "max_tokens", sizeof "max_tokens") == 0;
}

static void print_stop_warning(const json_value_t *root) {
    if (!hit_output_limit(root)) return;
    kputs("[chat: the model ran out of output budget - that answer is "
          "incomplete. Ask it to continue.]\n");
}

/* THE SAME FACT, ON A ROUND THAT CALLED TOOLS — where it is worse.
 *
 * stop_reason "max_tokens" used to be reported only on a round with no tool
 * calls, on the argument that the operator should only be interrupted by facts
 * they can act on. But generation stops wherever it stops, and the last block of
 * a truncated reply is usually the tool_use itself: the API then hands back that
 * block with `"input":{}`, because a half-written arguments object is not JSON.
 * Every tool in this kernel then answers, correctly and uselessly, that its
 * required argument is missing.
 *
 * A live session showed exactly what that costs: fifteen consecutive rounds of
 * `[app action=launch -> EINVAL]` while the model insisted it was attaching a
 * document it really was attaching, each round a paid API call. Neither end
 * could see the truncation, because the one party that knew — the kernel, which
 * had the stop_reason in its hand — was staying quiet.
 *
 * So a call whose arguments arrived empty on a truncated reply is NOT dispatched
 * (dispatching it can only produce a misleading error, and for a mutating tool
 * it could act on defaults nobody asked for). It is answered with the real
 * reason, in words that name the remedy. The kernel prints its own line too, and
 * that line contains no model-chosen bytes: the tool NAME is model-controlled
 * and a '[' in column zero must stay unforgeable (include/trace.h), so the count
 * is printed and the name is not. */
#define CUT_OFF_RESULT                                                        \
    "refused: nothing was run, and nothing on this machine changed. Your "    \
    "previous message hit this machine's per-reply output limit while you "   \
    "were still writing this call's arguments, so they never arrived - the "  \
    "kernel received an empty object where your arguments should have been. " \
    "This is not a schema error and repeating the same call unchanged will "  \
    "fail the same way. Send it again, SHORTER: for an app document that "    \
    "means fewer widgets, shorter names, and no indentation or newlines "     \
    "inside the JSON."

/* Whatever the API said when it did not say 200. The operator has no other way
 * to find out why the machine will not act.
 *
 * EVERY BYTE IN HERE IS CHOSEN BY THE FAR END, so all three of them go through
 * print_untrusted_n. That is not paranoia about Anthropic: certificate
 * verification is off unless FABLEOS_VERIFY_CERTS is set, so "the far end" is
 * anything that can answer on 443 — and this is the path the machine takes on
 * every keyless boot, since a missing key is a 401.
 *
 * Printing them raw broke the one invariant this kernel's whole output contract
 * rests on ("a '[' in column zero is the kernel speaking"): a JSON error.message
 * of "oops\n[vfs_write /etc/shadow -> ok]" decodes to a real newline, and the
 * forged line was byte-identical to lib/trace.c's own. The raw-body fallback was
 * worse — it passed '\b' through, and a backspace from column zero steps UP a
 * row in lib/base.c, so a body of backspaces could overwrite genuine ground
 * truth printed above. The brackets and the labels below are the kernel's; only
 * they are allowed in column zero. */
static void print_http_error(const model_response_t *r) {
    kputs("[model ");
    if (r->status_line[0]) print_model_prose(r->status_line);
    else                   kputs("http error");
    kputs("]\n");

    json_value_t root, err, msg;
    size_t       mlen = 0;
    if (json_parse(r->body, r->body_len, &root) == JSON_OK &&
        json_get(&root, "error", &err) == JSON_OK &&
        json_get(&err, "message", &msg) == JSON_OK &&
        json_str(&msg, text_buf, sizeof text_buf, &mlen) == JSON_OK) {
        kputs("[model said: ");
        print_untrusted_n(text_buf, mlen);
        kputs("]\n");
        return;
    }
    if (r->body_len) {
        size_t n = r->body_len < 240 ? r->body_len : 240;
        kputs("[model said: ");
        print_untrusted_n(r->body, n);
        kputs("]\n");
    }
}

/* ====================================================================== */
/* one round of tool calls                                                 */
/* ====================================================================== */

/* Fixed cost of one tool_result block that carries no text at all: the two keys
 * (36), the content key (11), the optional is_error member (16), the braces and
 * the separating comma (3), the content quotes (2) and the "...[clipped]" marker
 * (13) that a block with no room still emits. 96 rounds that up. Measured
 * generously on purpose — being wrong in this direction costs a few bytes of
 * result text, while being wrong the other way overflows the writer. */
#define RESULT_BLOCK_OVERHEAD  96

/* Room the budget note at the end of the turn needs (a 288-byte sentence plus
 * its JSON block framing). Reserved from the very first result, so the note can
 * never be the thing that does not fit. */
#define BUDGET_NOTE_RESERVE    384

/* Append one tool_result block to the staging writer.
 *
 * FAIR SHARE, NOT FIRST COME. Every tool_use MUST come back with a matching
 * tool_result or the API rejects the whole conversation — a missing result is
 * not a shorter answer, it is a dead exchange after the side effects already
 * happened. The first version of this took "whatever room is left" for each
 * block in turn, which meant one 1 KiB result could leave the remaining seven
 * calls with nothing: their clip markers then overflowed the writer and the turn
 * was abandoned *after* eight real mutations had run.
 *
 * So each call is given the room left divided by the number of calls still to be
 * answered (`pending`, including this one), and its text is clipped to fit that
 * share. Because every block is bounded by its own share and the shares sum to
 * no more than the room available, the array cannot overflow however the sizes
 * fall — the last call is as sure of its slot as the first. */
static void put_tool_result(json_writer_t *w, int first, size_t pending,
                            const char *id, const char *text, int is_error) {
    static const char MARK[] = "...[clipped]";

    /* The share is computed BEFORE this block's own bytes go in, from the same
     * writer state every other block sees. */
    size_t used = json_writer_len(w);
    size_t tail = BUDGET_NOTE_RESERVE + 2;            /* note + "]" + NUL */
    size_t avail = (w->cap > used + tail) ? w->cap - used - tail : 0;
    size_t share = avail / (pending ? pending : 1);
    size_t idcost = json_escape((char *)0, 0, id);
    size_t fixed  = RESULT_BLOCK_OVERHEAD + idcost;
    size_t room   = (share > fixed) ? share - fixed : 0;

    if (!first) json_put_char(w, ',');
    json_put(w, "{\"type\":\"tool_result\",\"tool_use_id\":");
    json_put_string(w, id);
    json_put(w, ",\"content\":");

    if (json_escape((char *)0, 0, text) <= room) {
        json_put_string(w, text);
    } else {
        /* Escaping expands a byte to at most six (""), so room/6 source
         * bytes always fit. Conservative beats clever here. */
        char   clip[CHAT_TOOL_RESULT_CAP];
        size_t limit = sizeof clip - sizeof MARK;
        /* The marker is subtracted first, so even a share with no room for text
         * still produces a block that fits: the model is told its result was
         * lost, instead of the conversation being broken by a missing one. */
        size_t keep  = (room > sizeof MARK) ? (room - sizeof MARK) / 6 : 0;
        size_t tlen  = strlen(text);

        if (keep > tlen)  keep = tlen;
        if (keep > limit) keep = limit;
        memcpy(clip, text, keep);
        memcpy(clip + keep, MARK, sizeof MARK);      /* includes the NUL */
        json_put_string(w, clip);
    }

    if (is_error) json_put(w, ",\"is_error\":true");
    json_put_char(w, '}');
}

/* Append the kernel's own block to the tool_result turn: how much of the round
 * budget is gone.
 *
 * WHY THE MODEL IS TOLD. The cap is the loop's hardest edge — reach it and the
 * turn is abandoned, the history is rolled back and the operator is told the
 * machine gave up. A model that cannot see the budget cannot plan inside it, so
 * that outcome used to arrive with no warning, in the middle of a job, and cost
 * the operator every completed step. One sentence of arithmetic per round is
 * enough for the model to triage: finish the important half, then report.
 *
 * This is the only kernel voice inside the conversation, and it is the loop's
 * fact, not the model's: written in C from stat_rounds. It is a text block, not
 * a tool_result, because it answers nothing the model asked for. It goes last so
 * the tool_result blocks it accompanies still lead the turn, as the API
 * requires. */
static void put_budget_note(json_writer_t *w, unsigned used, unsigned cap,
                            unsigned calls, unsigned failed) {
    unsigned left = (cap > used) ? cap - used : 0;
    /* 288 was too small by 33 bytes, and only for the last-round variant — the
     * one round where the wording is load-bearing. The instruction the model
     * received ended mid-word at "...means the turn is abandoned a", losing "nd
     * the operator is told nothing.", i.e. exactly the consequence that makes the
     * model answer instead of calling another tool. Nothing failed and no test
     * caught it: snprintf truncates safely, and the existing assertion only
     * checks the prefix, which survives inside the first 287 bytes. Sized against
     * the longest variant (321 characters) and bounded by BUDGET_NOTE_RESERVE
     * above, which the writer already reserves for this note. */
    char     note[384];
    _Static_assert(sizeof note <= BUDGET_NOTE_RESERVE,
                   "the budget note must fit the room build_request reserves");

    /* NOTE: the kernel's vsnprintf has no star-width; literal formats only.
     *
     * left == 0 cannot reach the model — the loop aborts on the cap before
     * sending again, and the note goes with the abandoned turn — so the last note
     * the model ever reads is the one written with left == 1. That note travels
     * INSIDE the final request, the one build_request() sends with no tools
     * attached, which is why it is phrased in the present tense: by the time it
     * is read, this is already the last round. */
    if (left <= 1) {
        snprintf(note, sizeof note,
                 "kernel: tool round %u of %u used (%u call(s) this round, %u "
                 "failed). THIS IS YOUR LAST ROUND AND TOOL CALLING IS NOW OFF. "
                 "Answer it now with prose: what you changed, what you verified "
                 "by reading it back, and what is left undone. A tool call "
                 "instead of an answer means the turn is abandoned and the "
                 "operator is told nothing.",
                 used, cap, calls, failed);
    } else {
        snprintf(note, sizeof note,
                 "kernel: tool round %u of %u used, %u left (%u call(s), %u "
                 "failed). Keep going until the job is verified; answer before "
                 "the budget runs out.",
                 used, cap, left, calls, failed);
    }

    json_put(w, ",{\"type\":\"text\",\"text\":");
    json_put_string(w, note);
    json_put_char(w, '}');
}

/* ====================================================================== */
/* the turn                                                                */
/* ====================================================================== */

static int abort_turn(int rc) {
    hist_rollback_epoch();
    return rc;
}

/* Outcomes that say nothing about the work and everything about the link, so
 * retrying them is not optimism — it is the difference between a turn that lost
 * its connection and a turn that threw away four completed mutations.
 *
 * 500 is deliberately NOT here. A server error is reported to the operator at
 * once, because on this machine the likeliest cause is the request we just built
 * rather than the far end, and hammering it with the same body would only hide
 * that. 429 (rate limited), 503 (unavailable) and 529 (overloaded) are the three
 * the API uses to say "later", and later is what a retry is. */
static int http_retryable(int status) {
    return status == 429 || status == 503 || status == 529;
}

/* A transport failure worth another go: the connection, the TLS session or the
 * read deadline. Not EINVAL/ENOSPC (our bug, deterministic), not ESCRIPT (the
 * mock has simply run out of script, and retrying that is a test that hangs). */
static int send_retryable(int rc) {
    return rc == MODEL_ETRANSPORT || rc == MODEL_ETIMEOUT || rc == MODEL_EPROTO;
}

static int chat_ask_body(const char *sentence);

int chat_ask(const char *sentence) {
    if (!sentence) return CHAT_EINVAL;

    if (in_turn) {
        /* Say it in the kernel's own voice, because the caller is usually a tool
         * handler whose result the model reads, and "the store could not be read
         * or written" — which is what core/agenda.c used to flatten this into —
         * sent a live model hunting a disk fault that did not exist for six
         * rounds and two paid API calls. */
        kputs("[chat: a turn is already in progress, so this one was not "
              "started. A sentence cannot be asked from inside a tool call; it "
              "will run when the machine is next idle, or at boot]\n");
        return CHAT_EREENTER;
    }

    in_turn = 1;
    int rc = chat_ask_body(sentence);
    in_turn = 0;
    return rc;
}

void chat_turn_ended(void) { in_turn = 0; }

static int chat_ask_body(const char *sentence) {
    stat_rounds     = 0;
    stat_tool_calls = 0;
    stat_retries    = 0;

    if (!transport) {
        kputs("[no link to the model - this machine cannot act]\n");
        return CHAT_ETRANSPORT;
    }

    hist_epoch++;
    if (hist_append_text(ROLE_USER, sentence) != CHAT_OK) {
        kputs("[chat: that is longer than the whole conversation buffer - "
              "nothing was sent]\n");
        return abort_turn(CHAT_EHISTORY);
    }

    for (;;) {
        if (stat_rounds >= max_rounds) {
            /* The conversation is rolled back, but the actions were real and the
             * operator has already seen their trace lines. Say so, and say that
             * the record survives, so "what did you get done?" is a question the
             * next sentence can actually answer (chat_action_at(), exposed to the
             * model as the action_log tool). */
            kprintf("[chat: stopped after %u tool rounds without an answer - "
                    "the turn was abandoned]\n", max_rounds);
            kprintf("[chat: %u tool calls did run and were traced above; the "
                    "kernel's record of them survives - ask what was done]\n",
                    stat_tool_calls);
            return abort_turn(CHAT_ELIMIT);
        }

        /* The last permitted round is sent with tool calling turned off — see
         * build_request(). The model was warned in the previous round's budget
         * note, so this is the promise being kept rather than a surprise. */
        int last_round = (stat_rounds + 1 >= max_rounds);

        size_t req_len = 0;
        int    rc      = build_request(&req_len, !last_round);
        if (rc != CHAT_OK) return abort_turn(rc);

        model_response_t r;
        rc = model_send(transport, req_buf, req_len, resp_buf, sizeof resp_buf, &r);
        if (rc != MODEL_OK) {
            const char *why = model_last_error(transport);
            kprintf("[model unreachable: %s]\n",
                    (why && why[0]) ? why : model_strerror(rc));
            /* A retry does NOT spend a round: the model never saw this request,
             * so charging it for the attempt would shorten the job for a reason
             * the model cannot see or do anything about. */
            if (send_retryable(rc) && stat_retries < CHAT_SEND_RETRIES) {
                stat_retries++;
                kprintf("[chat: retrying, attempt %u of %u - the work already "
                        "done this turn is kept]\n",
                        stat_retries, (unsigned)CHAT_SEND_RETRIES);
                continue;
            }
            stat_rounds++;
            return abort_turn(CHAT_ETRANSPORT);
        }
        stat_rounds++;
        if (r.http_status != 200) {
            print_http_error(&r);
            if (http_retryable(r.http_status) && stat_retries < CHAT_SEND_RETRIES) {
                stat_retries++;
                stat_rounds--;               /* the model never answered */
                kprintf("[chat: the API said try later; retrying, attempt %u of "
                        "%u]\n", stat_retries, (unsigned)CHAT_SEND_RETRIES);
                /* Nothing to wait on: interrupts are masked and this kernel has
                 * no timers, so the delay is a polled busy-wait against the TSC
                 * millisecond clock (lib/base.c). It is the only thing the
                 * machine can do with the time, and going straight back at a
                 * rate limiter would earn the same answer. */
                mdelay(500u * stat_retries);
                continue;
            }
            return abort_turn(CHAT_EHTTP);
        }
        if (r.truncated) {
            kprintf("[chat: reply exceeded %u bytes and was cut off]\n",
                    (unsigned)sizeof resp_buf);
            return abort_turn(CHAT_EPROTO);
        }

        json_value_t root, content;
        if (json_parse(r.body, r.body_len, &root) != JSON_OK) {
            kputs("[chat: the reply was not valid JSON]\n");
            return abort_turn(CHAT_EPROTO);
        }
        if (json_msg_content(&root, &content) != JSON_OK) {
            kputs("[chat: the reply carried no content blocks]\n");
            return abort_turn(CHAT_EPROTO);
        }

        size_t nblocks = json_count(&content);

        /* --- pass 1: how many tool calls, are they all answerable, and will the
         * answers fit? -------------------------------------------------------
         * A tool_use with no usable id can never be answered, and an assistant
         * turn containing one can never be echoed back. Find that out before
         * running anything: it is the difference between a turn that fails and
         * a machine that has acted but cannot say so.
         *
         * The same argument applies to room. put_tool_result() guarantees each
         * answer fits its fair share, but only while a share is bigger than an
         * empty block; past that many calls, the array cannot be completed at
         * all. Discovering that AFTER running eight mutations is the worst
         * outcome available — the work happened and the conversation carrying it
         * is dead — so the arithmetic is done here, from the real escaped id
         * lengths, and a round that cannot be answered is refused untouched. */
        size_t ncalls   = 0;
        size_t answer_cost = 2 + BUDGET_NOTE_RESERVE;      /* "[" "]" + the note */
        /* Which block a truncated reply can have cut off: generation stops at
         * the end, so only the LAST tool_use can be missing its arguments.
         * Anything earlier that arrived with `{}` really does mean `{}` — several
         * tools take no arguments at all — and must still be dispatched. */
        size_t last_use = nblocks;
        for (size_t i = 0; i < nblocks; i++) {
            json_value_t blk, type, id;
            if (json_at(&content, i, &blk) != JSON_OK)     continue;
            if (json_get(&blk, "type", &type) != JSON_OK)  continue;
            if (!json_str_eq(&type, "tool_use"))           continue;
            last_use = i;

            char idbuf[TOOL_ID_MAX];
            if (json_get(&blk, "id", &id) != JSON_OK ||
                json_str(&id, idbuf, sizeof idbuf, (size_t *)0) != JSON_OK ||
                idbuf[0] == '\0') {
                kputs("[chat: the model asked for a tool with no usable id - "
                      "nothing was run]\n");
                return abort_turn(CHAT_EPROTO);
            }
            ncalls++;
            answer_cost += RESULT_BLOCK_OVERHEAD +
                           json_escape((char *)0, 0, idbuf);
        }

        if (ncalls && answer_cost > sizeof blocks_buf) {
            kprintf("[chat: the model asked for %u tools at once and their "
                    "answers cannot fit one request - nothing was run]\n",
                    (unsigned)ncalls);
            return abort_turn(CHAT_ENOSPC);
        }

        /* --- the assistant turn goes into the history before anything runs,
         * so a full history can still cancel the round cleanly. -------------*/
        if (ncalls) {
            if (hist_append_raw(ROLE_ASSISTANT, content.start, content.len)
                    != CHAT_OK) {
                kputs("[chat: no room left to remember this turn - "
                      "nothing was run]\n");
                return abort_turn(CHAT_EHISTORY);
            }
        }

        /* --- pass 2: print prose and dispatch, in the order they arrived --- */
        json_writer_t w;
        json_writer_init(&w, blocks_buf, sizeof blocks_buf);
        json_put_char(&w, '[');

        size_t done   = 0;                 /* tool_use blocks answered so far */
        size_t failed = 0;                 /* of those, ones that did not act */
        size_t cut    = 0;                 /* calls whose arguments never came */
        int truncated_reply = hit_output_limit(&root);
        for (size_t i = 0; i < nblocks; i++) {
            json_value_t blk, type;
            if (json_at(&content, i, &blk) != JSON_OK)    continue;
            if (json_get(&blk, "type", &type) != JSON_OK) continue;

            if (json_str_eq(&type, "text")) {
                json_value_t txt;
                if (json_get(&blk, "text", &txt) == JSON_OK)
                    print_text_block(&txt);
                continue;
            }
            if (!json_str_eq(&type, "tool_use")) continue;

            char idbuf[TOOL_ID_MAX]   = "";
            char namebuf[TOOL_NAME_MAX] = "";
            json_value_t id, name, input;

            json_get(&blk, "id", &id);
            json_str(&id, idbuf, sizeof idbuf, (size_t *)0);
            /* A missing or over-long name is not fatal: tool_dispatch turns it
             * into a legible error the model can recover from next round. */
            if (json_get(&blk, "name", &name) != JSON_OK ||
                json_str(&name, namebuf, sizeof namebuf, (size_t *)0) != JSON_OK)
                namebuf[0] = '\0';

            if (done >= CHAT_MAX_TOOL_CALLS) {
                /* Refuse, but still answer: every tool_use needs a result. The
                 * refusal says what to do instead, because the model's next move
                 * is decided by this text and "refused" alone invites it to
                 * repeat the same batch. */
                put_tool_result(&w, done == 0, ncalls - done, idbuf,
                                "refused: too many tool calls in one turn - this "
                                "machine runs at most 8 per round and 8 already "
                                "ran. Nothing was done for this one; ask for it "
                                "again next round, in a smaller batch.", 1);
                done++;
                failed++;
                continue;
            }

            const char *in_ptr = "{}";
            size_t      in_len = 2;
            int         have_args = 0;
            if (json_get(&blk, "input", &input) == JSON_OK) {
                in_ptr = input.start;
                in_len = input.len;
                /* An object with members, or anything that is not an empty
                 * object, counts as arguments that arrived. json_count() is 0
                 * for both `{}` and a non-container, which is exactly the set
                 * that carries nothing a tool can read. */
                have_args = json_count(&input) != 0;
            }

            /* Arguments lost to the output limit: say so, do not dispatch.
             * See CUT_OFF_RESULT above for why this is not a schema error. */
            if (truncated_reply && !have_args && i == last_use) {
                put_tool_result(&w, done == 0, ncalls - done, idbuf,
                                CUT_OFF_RESULT, 1);
                journal_record(namebuf, "not run: arguments cut off by the "
                                        "output limit", 1);
                done++;
                failed++;
                cut++;
                continue;
            }

            tool_call_t call;
            call.id        = idbuf;
            call.name      = namebuf[0] ? namebuf : (const char *)0;
            call.input     = in_ptr;
            call.input_len = in_len;

            tool_result_t res;
            tool_result_init(&res, result_buf, sizeof result_buf);
            tool_dispatch(&call, &res);          /* emits the kernel trace line */
            stat_tool_calls++;

            /* The kernel's own record of the action, taken from the dispatch
             * result rather than from anything the model will later say about
             * it, and kept even if this turn is abandoned. */
            journal_record(namebuf, result_buf, res.is_error);

            put_tool_result(&w, done == 0, ncalls - done, idbuf, result_buf,
                            res.is_error);
            done++;
            if (res.is_error) failed++;
        }

        if (ncalls == 0) {
            /* A plain answer: remember it and hand the machine back. */
            if (hist_append_raw(ROLE_ASSISTANT, content.start, content.len)
                    != CHAT_OK) {
                kputs("[chat: answered, but there was no room to remember it]\n");
                hist_rollback_epoch();
            }
            if (nblocks == 0) kputs("[chat: the model said nothing]\n");
            /* An answer cut off by the output budget is half an answer, and the
             * operator cannot tell by looking. stop_reason is the API's own word
             * for it, so it is reported as the kernel's fact, not guessed at. */
            print_stop_warning(&root);
            return CHAT_OK;
        }

        /* The operator watches a window not appear and has no shell to ask why.
         * One line, printed from the kernel's own count, names the cause. No
         * model-chosen byte goes into it (see CUT_OFF_RESULT). */
        if (cut)
            kprintf("[chat: the model's reply hit its output limit while writing "
                    "arguments - %u call(s) arrived empty and were NOT run; it "
                    "was told to send a shorter one]\n", (unsigned)cut);

        put_budget_note(&w, stat_rounds, max_rounds, (unsigned)done,
                        (unsigned)failed);
        json_put_char(&w, ']');
        if (!json_writer_ok(&w)) {
            kprintf("[chat: %u tool results do not fit one turn - "
                    "conversation abandoned]\n", (unsigned)done);
            return abort_turn(CHAT_ENOSPC);
        }

        if (hist_append_raw(ROLE_USER, blocks_buf, json_writer_len(&w))
                != CHAT_OK) {
            kprintf("[chat: no room to record %u tool results - "
                    "conversation abandoned]\n", (unsigned)done);
            return abort_turn(CHAT_EHISTORY);
        }
    }
}
