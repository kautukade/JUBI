/* capability.h — the machine keeps and reuses what it builds.
 *
 * PURPOSE
 *   Everything else in this kernel is a verb the model may use once. The driver
 *   VM lets it author a program, run it, and watch it work — and then the program
 *   is gone, because dvm_run() keeps no state between calls and the assembled
 *   image lives in one caller's buffer. The next turn starts from nothing.
 *
 *   That is the difference between a machine that can do things when asked and a
 *   machine that grows. A capability is the second thing: a program the model
 *   wrote, given a NAME, a documented SIGNATURE and a home on DISK, so that it
 *   can be called later — by the model, by another capability, and (see FUTURE
 *   EXTENSION POINTS) by an app — without being written again. `driver_install`
 *   already proved the shape for exactly one case, audio sinks. This is that
 *   shape with the device-specific part taken out.
 *
 *   The audio path is worth stating precisely, because it is the thing being
 *   generalised: the model wrote an AC'97 bring-up, driver_install kept the
 *   assembled image, and a later turn played a note with no program written.
 *   The parts of that which were audio-specific were the registry it landed in
 *   and the signature it had to match. Take both out and what is left is: keep
 *   the source, keep a signature, keep a name, and let anything call it.
 *
 * WHAT A CAPABILITY IS
 *   A record on disk with four parts:
 *
 *     name       an identifier, [a-z][a-z0-9_]*, unique on the machine.
 *     doc        one line of prose. This is what the model reads to decide
 *                whether to call it, so it is part of the contract, not a
 *                comment.
 *     params     an ordered list of parameter NAMES. Every parameter is an
 *                unsigned 64-bit number: the arity is the signature.
 *     body       either a driver-VM program (source text, kept verbatim) or a
 *                COMPOSITION — an ordered list of calls to other capabilities.
 *
 *   Nothing else. In particular a capability carries no policy of its own: the
 *   sandbox it runs in is built here, in C, identically for every capability, so
 *   a saved record can never widen its own reach. What that sandbox is, and the
 *   holes in it, are enumerated under THE SANDBOX below.
 *
 * THE CALLING CONVENTION — one ABI, for every capability, forever
 *   A capability is a function from up to CAP_MAX_PARAMS numbers to one number
 *   plus an optional string. A per-capability return declaration was considered
 *   and rejected: it is one more thing for a model to get wrong, and the schema
 *   bytes are better spent on the doc line.
 *
 *     ON ENTRY   r0..r(nparam-1) hold the declared parameters, in order.
 *                Every other register is zero. The scratch arena is zeroed by
 *                dvm_run() itself, so a capability never sees another's bytes.
 *
 *     ON HALT    r0                      the result. One number.
 *                r1, r2                  offset and LENGTH of an optional text
 *                                        result in the scratch arena. r2 == 0
 *                                        means "no text". Forced to printable
 *                                        ASCII and clipped to CAP_TEXT_MAX by
 *                                        this module before anyone sees it.
 *                r3 == CAP_TAILCALL      request a TAIL CALL (see COMPOSITION).
 *                r4, r5                  offset and length of the callee's NAME,
 *                                        as bytes in the scratch arena.
 *                r6..r11                 the callee's arguments.
 *
 *   The name of a tail-call target is built by the program at run time with the
 *   string ops (`mstr`), which is why dispatch is dynamic rather than baked in.
 *
 * COMPOSITION — and the hard constraint that shapes all of it
 *   dvm_run() REFUSES TO BE RE-ENTERED (DVM_TRAP_REENTRY, and dvm.h argues the
 *   case at length: there is one scratch arena, it is zeroed on entry, and one
 *   pair of DMA guard bands). So there is no way — none — for a capability to
 *   call another one from the middle of its own execution. A `cap.call` syscall
 *   would trap on the first use. That is a contract this module does not own and
 *   should not route around, so composition here happens BETWEEN runs, in C:
 *
 *     TAIL CALLS (a program capability). The program finishes, and its last act
 *     is to name a successor and hand it arguments. Nothing of the caller needs
 *     preserving, so this is a LOOP in the dispatcher, not recursion: a chain of
 *     ten thousand tail calls costs no kernel stack at all. Accumulator-passing
 *     is therefore the natural way to write iteration and recursion here —
 *     factorial is `fact(n, acc)` tail-calling `fact(n-1, acc*n)` — and
 *     tests/host/test_capability.c runs exactly that.
 *
 *     COMPOSITIONS (a compose capability). An ordered list of steps, each one a
 *     call to another capability whose arguments are wired from this
 *     capability's own parameters, from an earlier step's result, or from a
 *     literal. A step may carry a guard, so a composition can stop calling
 *     itself. Steps run in order, each to completion, and the composition's
 *     result is the last executed step's result. This IS C recursion, one frame
 *     per nested composition, which is why it is depth-bounded.
 *
 *   WHAT COMPOSITION CANNOT DO, stated plainly: pass text. Arguments are
 *   numbers, because the arena is zeroed between runs and there is nothing this
 *   module could hand across that dvm_run() would not immediately erase. Two
 *   capabilities that need to exchange bytes do it through a file in the data
 *   root (fs.write / fs.read), which is durable, inspectable and already bounded.
 *   The text result of a composition is the text of its last executed step, for
 *   the same reason.
 *
 * WHY A CYCLE CANNOT HANG THE MACHINE
 *   A capability may name itself, and two capabilities may name each other. No
 *   attempt is made to detect that statically — a cycle is often the point, and a
 *   static check would have to be conservative about a guard whose value is only
 *   known at run time. Instead every cycle is TERMINATED by counters that only
 *   ever decrease within one top-level call, and there are four of them because
 *   a cycle can be paid for in four different currencies:
 *
 *     CAP_MAX_DEPTH        nested composition frames. This is the bound on
 *                          KERNEL STACK, and it is the reason tail calls are
 *                          deliberately not counted here — they consume none.
 *     CAP_MAX_INVOCATIONS  capability invocations of any kind in one top-level
 *                          call, tail calls included. This is the counter an
 *                          unguarded tail-call loop hits, and the one that makes
 *                          "the chain is finite" true rather than hoped for.
 *     CAP_TREE_STEPS       driver-VM instructions summed over the WHOLE tree.
 *                          Per-run budgets alone bound nothing: 64 runs of
 *                          100,000 steps is 6.4M steps of work nobody asked for.
 *     CAP_TREE_DELAY_US    microseconds spent in `delay` summed over the tree,
 *                          for exactly the same reason. This is the one that
 *                          bounds WALL CLOCK, which is what "hang" means to an
 *                          operator waiting at a prompt.
 *
 *   Each counter is charged before the work happens and never reset inside a
 *   call, so the tree is finite by construction. Exceeding any of them is
 *   CAP_EDEPTH or CAP_EBUDGET with the call CHAIN in the message
 *   ("countdown -> countdown -> countdown ..."), because "recursion limit" with
 *   no path is not something a model can fix.
 *
 * THE SANDBOX — what a capability may touch, and the holes, enumerated
 *   A capability gets NO DEVICE ACCESS AT ALL: no port I/O, no MMIO, no PCI, no
 *   DMA grant, and no bus-master bit. That is not a weakened driver sandbox, it
 *   is the absence of one. Hardware bring-up already has a home (driver_run and
 *   driver_install, which name a target and derive a window from its BARs);
 *   mixing the two would mean every saved capability carried a device grant it
 *   almost never needs. The dividend is that this whole module — dispatcher,
 *   budgets, ABI, composition — is testable on a host with no hardware at all.
 *
 *   What is left open, one line of justification each:
 *     con.write   the capability's own voice. It goes out through trace.c, so a
 *                 program still cannot put a '[' in column zero.
 *     fs.read     a capability that can keep what it computed is the entire
 *     fs.write    point of this file. CONFINED TO THE DATA ROOT, which is NOT
 *     fs.size     the capability store: a capability cannot rewrite the
 *                 machine's capabilities, and that confinement is enforced by
 *                 vm/dvm.c's portable half, so a host test proves it.
 *     time.ms     a timeout that is a real timeout rather than a loop count.
 *
 *   NOT granted: audio.tone, because the machine's audio sink may itself be a
 *   driver program and servicing that syscall would re-enter dvm_run(); and
 *   anything that reaches the network, because there is no such syscall.
 *
 * PERSISTENCE, VERSIONS, AND WHAT ROLLBACK ACTUALLY MEANS
 *   One file per capability under the store root, plus at most one backup:
 *
 *     <store>/<name>.cap     the current version
 *     <store>/<name>.bak     the version before it, if there was one
 *
 *   The store is FS_DISK_MOUNT "/cap" when a disk is mounted and "/cap" on
 *   ramfs otherwise — capability_store_persists() says which, and it is said out
 *   loud at boot and in every tool result, because a capability that will not
 *   survive the night is a different promise from one that will.
 *
 *   THE FILE FORMAT is 16 lowercase hex digits, a newline, and then a JSON
 *   object. The hex is an FNV-1a-64 hash of every byte after the newline, and it
 *   is checked before the JSON is parsed, before the source is assembled, and
 *   before anything is registered. Deliberately NOT a bespoke text format with a
 *   bespoke parser: json.h is hardened and fuzzed and this file writes no
 *   parsing of its own (the 17-byte prefix is a fixed layout, not a grammar).
 *   A record whose hash does not match, whose JSON is malformed, whose name
 *   disagrees with its filename, or whose program no longer assembles is
 *   REJECTED — it never enters the registry — and then <name>.bak is tried.
 *   That is what makes a half-written file at power-off a recoverable event.
 *
 *   SAVE ORDER IS THE CONSISTENCY GUARANTEE, and it is the one thing here that
 *   has to be got right: the backup is written and flushed FIRST, then the
 *   current file is overwritten. So an interrupted save leaves either the old
 *   version intact (the overwrite never started) or a corrupt current file with
 *   a good backup beside it (which the next boot detects and falls back to). The
 *   working version is never the thing at risk.
 *
 *   VERSIONS ARE MONOTONIC PER NAME and never reused. capability_revert() does
 *   not decrement anything: it reads the backup and SAVES it again under the
 *   next version number, so "roll back v4" produces v5 whose contents are v3's,
 *   the broken v4 becomes the new backup, and reverting twice is a redo. There
 *   is exactly ONE level of undo. A model that saves twice over a working
 *   capability has lost it, and the tool says so before it does it.
 *
 *   REPLACEMENT CANNOT BREAK A CALLER MID-FLIGHT, for a structural reason
 *   rather than a locking one: this machine is single-threaded and polled, a
 *   tool call runs to completion before the next one starts, and a capability
 *   program cannot reach the store (see THE SANDBOX). So there is no instant at
 *   which a save and a call overlap. Within one call the source of each
 *   capability is read once and pinned for the duration of the chain.
 *
 * DISCOVERABILITY — the failure this is built to avoid
 *   A capability the model cannot find is the same as no capability. It has
 *   happened on this machine: the model apologised for lacking something it was
 *   holding. The system prompt (net/chat.c) tells the model in as many words
 *   that the tool list "is the kernel's tool registry, assembled from it
 *   mechanically - it is not a summary, so if a capability is not in it, this
 *   machine genuinely does not have it". So the list of saved capabilities lives
 *   IN THE TOOL SCHEMA, inside capability_call's description, where the model
 *   reads it without having to ask: capability_panel().
 *
 *   THAT PANEL IS A FIXED NUMBER OF BYTES — exactly CAP_PANEL_CHARS characters,
 *   space-padded, no newlines, no '"' and no '\'. Not for tidiness: the
 *   assembled schema size is a compile-time constant in this tree
 *   (CHAT_REGISTRY_BYTES in include/chat.h, which `make test-qemu` fails on if
 *   the boot banner disagrees), and a description that grew with the contents of
 *   a DISK would make that constant a function of what the operator saved last
 *   night. Padding to a fixed width keeps the schema size independent of the
 *   store, and capability_panel_check() asserts the rendered length equals the
 *   reserved length and prints BOTH numbers if it ever does not.
 *
 * DEPENDENCIES
 *   dvm.h (the machine), vfs.h (the store), json.h (the record), trace.h (the
 *   ground-truth line), and the libc shim's snprintf. No heap: the registry, the
 *   one program image and the one file buffer are static, because a capability
 *   that cannot be loaded when memory is tight is a capability the machine
 *   cannot rely on. No hardware: dvm_io_t is passed with every device hook NULL
 *   and only the syscall backend set, which is why tests/host/test_capability.c
 *   drives the real dispatcher with a synthetic kernel.
 *
 * SIZE NOTE
 *   The registry, the shared program image, the source buffer and the file
 *   buffer are static and always present: see the CAP_* limits below for the
 *   arithmetic. Nothing here is allocated per call.
 *
 * FUTURE EXTENSION POINTS
 *   - AN APP CALLING A CAPABILITY is one table entry away and is deliberately
 *     not done here: apps/cap.c is the single brokered path from a document to
 *     the kernel, it validates every argument against a fixed table, and it
 *     performs the call from the idle loop rather than from inside a handler.
 *     A capability call fits that shape exactly — name plus up to
 *     CAP_MAX_PARAMS bounded numbers, queued and performed by C — and wants one
 *     new entry in that file's capability table calling capability_invoke(),
 *     with the rate limit that is already there. Reported, not written: apps/ is
 *     not this module's to edit.
 *   - TEXT ARGUMENTS across a call need somewhere for bytes to live that
 *     dvm_run() does not zero. The honest shape is a per-run scratch WINDOW in
 *     vm/dvm.c (its own header names it) rather than anything here.
 *   - A capability that wants a device is really a driver; the seam is
 *     driver_install's. If the two ever merge, the merge point is the sandbox
 *     builder in core/capability.c and nothing above it.
 *   - Static cycle detection would let a runaway composition be refused at SAVE
 *     time rather than at call time. It needs the guard evaluated symbolically,
 *     so it is a real piece of work, and the run-time counters make it an
 *     improvement in the error message rather than in safety.
 *   - Capabilities are global. Once there are users or processes, the owner
 *     field belongs next to `version`, and the store gains a subdirectory.
 */
#ifndef CAPABILITY_H
#define CAPABILITY_H

#include <stdint.h>
#include <stddef.h>

#include "dvm.h"

/* ---- how many, how big. All static; nothing here is allocated. ---- */

/* Capabilities the machine may hold. The panel that advertises them to the
 * model is a fixed CAP_PANEL_CHARS bytes of tool schema, so this number and
 * that one are spent together: 16 rows of ~64 characters is what 1 KiB buys. */
#define CAP_MAX               16

#define CAP_NAME_MAX          32     /* incl. NUL: 31 characters of name      */
#define CAP_DOC_MAX          200     /* incl. NUL: one line of prose          */
#define CAP_MAX_PARAMS         6     /* r0..r5 on entry; r6..r11 on a tailcall */
#define CAP_PARAM_NAME_MAX    20     /* incl. NUL                             */
#define CAP_MAX_STEPS          6     /* calls in one composition              */

/* Source bytes kept for a program capability. DVM_MAX_INSNS is 256, so 6 KiB is
 * 24 bytes per instruction including comments — more than the assembler will
 * ever be handed usefully, and a quarter of DVM_SRC_MAX, which sizes the file
 * buffer below. Source is validated at save time to be printable ASCII plus
 * newline and tab, which is also what bounds JSON escaping at 2x. */
#define CAP_SRC_MAX         6144

/* One record on disk, hex prefix and JSON escaping included. CAP_SRC_MAX
 * doubles in the worst case (a file of nothing but newlines), the metadata is
 * under 2 KiB, so 16 KiB has room to spare and is one buffer, used for both
 * save and load, never both at once. */
#define CAP_FILE_MAX       16384

/* ---- the four budgets that make a cycle finite. See the header comment. ---- */

/* Nested COMPOSITION frames, i.e. C recursion in the dispatcher. Each frame is
 * ~200 bytes of stack; the kernel stack is 64 KiB and shared with lwIP and
 * mbedTLS, so 8 is chosen to be obviously irrelevant to it rather than to be
 * generous. Tail calls are NOT counted here: they consume no stack. */
#define CAP_MAX_DEPTH          8

/* Invocations of any kind in one top-level call. This is the counter an
 * unguarded tail-call loop hits. */
#define CAP_MAX_INVOCATIONS   64

/* Driver-VM instructions, and microseconds of `delay`, summed across the whole
 * call tree. The per-run budgets below are what one program gets; these are what
 * the tree gets, and they are what stop 64 legal runs adding up to a hang. */
#define CAP_TREE_STEPS   2000000ull
#define CAP_TREE_DELAY_US 1000000ull   /* 1 s of wall clock, in total */

/* Per-run budgets. Everything else in dvm_policy_t keeps its default. */
#define CAP_RUN_STEPS     100000ull
#define CAP_RUN_MEM_BYTES 1048576ull   /* bytes of string/memory work         */
#define CAP_RUN_SYS           32u
#define CAP_RUN_PRINTS         8u      /* con.write lines, per run            */
#define CAP_RUN_DELAY_US  200000ull
#define CAP_RUN_SINGLE_DELAY_US 50000u

/* ---- the register ABI. See THE CALLING CONVENTION. ---- */
#define CAP_REG_RESULT         0
#define CAP_REG_TEXT_OFF       1
#define CAP_REG_TEXT_LEN       2
#define CAP_REG_TAIL           3
#define CAP_REG_TAIL_NAME_OFF  4
#define CAP_REG_TAIL_NAME_LEN  5
#define CAP_REG_TAIL_ARG0      6

/* Written into r3 to request a tail call. Wide and arbitrary on purpose: a
 * program that leaves a stale value in r3 must not be read as asking for one. */
#define CAP_TAILCALL  0x7A11CA11ull

/* ---- reported strings ---- */
#define CAP_TEXT_MAX         512     /* of a capability's text result         */
#define CAP_MSG_MAX          224     /* one precise failure sentence          */
#define CAP_CHAIN_MAX        160     /* "a -> b -> a -> ...", clipped         */

/* The live discovery panel, in characters, excluding the NUL. Every rendering is
 * EXACTLY this long — see DISCOVERABILITY — so this is schema spent on every
 * request of every turn whether anything is saved or not, and it is spent
 * against a registry that has under a kilobyte of headroom left in
 * CHAT_TOOLS_BYTES. 768 characters shows roughly eight capabilities with their
 * doc lines intact, and the renderer ends the list with "+N more
 * (capability_list)" rather than half a name, so the sixteenth capability is
 * still reachable — just not visible without one more tool call. If the schema
 * budget is ever grown, this is the first number that should get some of it. */
#define CAP_PANEL_CHARS      768

/* ---- result codes (errno-style negatives; 0 = success) ---- */
#define CAP_OK          0
#define CAP_ENOENT     -2    /* no capability of that name                    */
#define CAP_EIO        -5    /* the store could not be read or written        */
#define CAP_EEXIST    -17    /* name collision where one was not allowed      */
#define CAP_EINVAL    -22    /* the description is not a usable capability    */
#define CAP_ENOSPC    -28    /* the registry or a fixed buffer is full        */
#define CAP_ECORRUPT  -84    /* a stored record failed its own checksum       */
#define CAP_EDEPTH    -85    /* composition nested deeper than CAP_MAX_DEPTH  */
#define CAP_EBUDGET   -86    /* invocations, steps or delay exhausted         */
#define CAP_ETRAP     -87    /* the program itself trapped; msg says how      */

typedef enum {
    CAP_KIND_PROGRAM = 1,    /* a driver-VM program, source kept verbatim */
    CAP_KIND_COMPOSE = 2     /* an ordered list of calls to other capabilities */
} cap_kind_t;

/* ---- where a step's argument comes from ---- */
typedef enum {
    CAP_ARG_LIT = 0,         /* a literal number                            */
    CAP_ARG_PARAM,           /* "$k": this capability's k-th parameter      */
    CAP_ARG_STEP             /* "#k": the result of step k (1-based)        */
} cap_argkind_t;

typedef struct cap_arg {
    uint64_t lit;            /* CAP_ARG_LIT only        */
    uint8_t  kind;           /* cap_argkind_t           */
    uint8_t  idx;            /* CAP_ARG_PARAM/STEP only */
} cap_arg_t;

/* Guard comparison. Unsigned, like the VM's: these are machine values. */
typedef enum {
    CAP_CMP_NONE = 0, CAP_CMP_EQ, CAP_CMP_NE,
    CAP_CMP_LT, CAP_CMP_LE, CAP_CMP_GT, CAP_CMP_GE
} cap_cmp_t;

/* One call in a composition. A step with a guard whose comparison is false is
 * SKIPPED, which is the only way a self-referencing composition ever stops. */
typedef struct cap_step {
    char      call[CAP_NAME_MAX];
    cap_arg_t arg[CAP_MAX_PARAMS];
    uint8_t   narg;
    uint8_t   cmp;                  /* cap_cmp_t; CAP_CMP_NONE = no guard */
    cap_arg_t lhs, rhs;
} cap_step_t;

/* One capability, as held in RAM. The SOURCE of a program capability is NOT
 * here: it lives on disk and is read into a single shared buffer for the run
 * that needs it, so sixteen capabilities do not cost sixteen source buffers. */
typedef struct cap {
    char       name[CAP_NAME_MAX];
    char       doc[CAP_DOC_MAX];
    uint32_t   version;             /* monotonic per name, from 1            */
    uint8_t    kind;                /* cap_kind_t                            */
    uint8_t    nparam;
    char       param[CAP_MAX_PARAMS][CAP_PARAM_NAME_MAX];
    uint8_t    nstep;               /* CAP_KIND_COMPOSE only                 */
    cap_step_t step[CAP_MAX_STEPS];
    uint32_t   src_len;             /* CAP_KIND_PROGRAM only, bytes on disk  */
    uint32_t   ninsn;               /* instructions it assembled to          */
    uint64_t   crc;                 /* of the stored record; identity        */
    /* Since boot, not since it was saved: a capability's history is on disk,
     * its behaviour is not. */
    uint32_t   calls, failures;
    uint8_t    has_backup;          /* a <name>.bak exists to revert to      */
    /* This record was loaded from <name>.bak because <name>.cap was damaged, so
     * that is also where its SOURCE has to be read from. Without this a
     * recovered capability loads its metadata from the good file and its program
     * from the broken one — which is exactly what the first version of this file
     * did, and the failure looked like a corrupt capability that had just been
     * reported as recovered. */
    uint8_t    from_backup;
} cap_t;

/* What one top-level invocation produced. ~1 KiB; one of these per call, on the
 * caller's stack, never one per frame. */
typedef struct cap_result {
    int      rc;                    /* CAP_OK or one of the codes above      */
    uint64_t value;                 /* r0 of the last program that ran       */
    char     text[CAP_TEXT_MAX];    /* its text result, printable, NUL-term  */
    size_t   text_len;
    uint32_t invocations;           /* capabilities entered, tail calls too  */
    uint32_t depth_used;            /* deepest composition frame reached     */
    uint64_t steps;                 /* VM instructions across the whole tree */
    uint64_t delay_us;              /* microseconds delayed across the tree  */
    uint32_t sys_calls;
    char     chain[CAP_CHAIN_MAX];  /* "outer -> inner -> inner"             */
    char     msg[CAP_MSG_MAX];      /* "" on success, else the exact reason  */
} cap_result_t;

/* ---- lifecycle ---- */

/* Choose the store, create it, load every record in it, and render the panel.
 * Idempotent: a second call reloads from disk, which is what a test wants and
 * what a future "rescan the store" tool would call. Prints one kernel line
 * saying how many capabilities were loaded, how many were rejected, and whether
 * the store survives a reboot. Never fails fatally: a machine whose store is
 * unreadable still boots, with no capabilities and a line saying so. */
void capability_init(void);

/* THE ONE CALL kernel/main.c makes. Implemented by tools/capability_tools.c, not
 * by core/capability.c: it binds the live discovery panel into that file's
 * capability_call description and THEN calls capability_init().
 *
 * The order matters and cannot be arranged any other way. A tool descriptor's
 * `description` is a plain `const char *` that core/tool.c reads when it builds
 * the request, so the panel has to be inside a buffer whose address is a
 * compile-time constant — which means the tool layer owns the buffer — and it
 * has to be filled before the first schema is assembled, which is before any
 * tool has ever been invoked. There is no hook between those two facts, so the
 * boot sequence supplies one. Declared here so kernel/main.c needs no
 * tool-private header; nothing in core/ calls it, so a host test that links only
 * core/capability.c does not need it to exist. */
void capability_bringup(void);

/* "/disk/cap" or "/cap". Never NULL, even before capability_init(). */
const char *capability_store_root(void);

/* Where a capability's fs.* syscalls are confined. Never inside the store. */
const char *capability_data_root(void);

/* Non-zero when the store is on a disk and a saved capability outlives a
 * reboot. Zero means the store is in RAM and everything here is for this boot
 * only — which every tool result says out loud. */
int capability_store_persists(void);

/* Records rejected by the last capability_init(): a checksum that did not
 * match, a name that disagreed with its filename, a program that no longer
 * assembles. Non-zero is worth reporting to the operator. */
int capability_rejected(void);

/* ---- the registry ---- */
int          capability_count(void);
const cap_t *capability_at(int index);
const cap_t *capability_find(const char *name);   /* NULL if absent */

/* 1 if `name` is a usable capability name: 1..CAP_NAME_MAX-1 bytes, starting
 * with a lowercase letter, then lowercase letters, digits and underscore. `why`
 * (optional) receives a sentence naming the offending byte. Exposed because the
 * tool layer must refuse a bad name with the same rule the store uses. */
int capability_name_ok(const char *name, char *why, size_t cap);

/* Decode a capability DESCRIPTION out of the raw JSON object a tool received:
 * "name", "doc", "params" (array of names), and exactly one of "program" (source
 * text) or "steps" (an array of {"call","args","when"}). `input` need not be
 * NUL-terminated — it is a tool_call_t input span.
 *
 * This lives here rather than in the tool layer on purpose: a description
 * arrives two ways, from the model and from the store, and the parts they share
 * are decoded once so a description the tool accepted cannot be one the store
 * rejects on the way back in. `msg` is a sentence naming the offending field.
 * Returns CAP_OK or CAP_EINVAL; `out` is zeroed first, and out->version and the
 * counters are left for capability_save() to set. */
int capability_parse_spec(const char *input, size_t len, cap_t *out,
                          char *src, size_t srccap, size_t *srclen,
                          char *msg, size_t cap);

/* ---- mutation ---- */

/* Create or replace `spec`. `src`/`srclen` is the program source for
 * CAP_KIND_PROGRAM and is ignored for CAP_KIND_COMPOSE.
 *
 * Everything is checked BEFORE anything is written: the name, the doc, the
 * parameter names, the arity of every step against the capability it calls, and
 * — for a program — that the source ASSEMBLES. A program that does not assemble
 * is never stored, so "it is in the registry" implies "it ran through the
 * assembler at least once".
 *
 * spec->version is ignored on input and set on output. On failure nothing on
 * disk and nothing in the registry has changed, and `msg` holds the reason.
 * Returns CAP_OK or a negative code. */
int capability_save(cap_t *spec, const char *src, size_t srclen,
                    char *msg, size_t cap);

/* Restore `name`'s backup as a NEW version (see PERSISTENCE above: this is not
 * a decrement, and doing it twice is a redo). CAP_ENOENT when there is no
 * capability, or no backup to restore. */
int capability_revert(const char *name, char *msg, size_t cap);

/* Remove `name`, its backup and its files. CAP_ENOENT if it is not there. */
int capability_delete(const char *name, char *msg, size_t cap);

/* ---- invocation ---- */

/* Call `name` with `nargs` numbers. `nargs` must equal its declared arity —
 * a mismatch is CAP_EINVAL naming both numbers, never a zero-filled register.
 *
 * `out` is always fully populated, including on refusal, and out->msg is a
 * sentence precise enough for the model to repair the call. This is the only
 * entry point that runs anything: the tool layer and (later) apps/cap.c both
 * come through here. */
int capability_invoke(const char *name, const uint64_t *args, int nargs,
                      cap_result_t *out);

/* ---- rendering a composition back to text ---- */

/* "$0", "#2", "17" — the exact inverse of the spec syntax, and the only renderer
 * there is: the record on disk and the listing the model reads come from this
 * one function, so a step cannot read back as something it is not. */
void        capability_arg_text(const cap_arg_t *a, char *dst, size_t cap);
const char *capability_cmp_text(uint8_t cmp);   /* "==", "<=", ... "" if none */

/* ---- discovery ---- */

/* The live list of saved capabilities as ONE line of exactly CAP_PANEL_CHARS
 * printable characters, space-padded, containing no '"', no '\\' and no control
 * byte — so its JSON-escaped length is also constant and the assembled tool
 * schema does not change size when a capability is saved. Never NULL. */
const char *capability_panel(void);

/* Bind a destination for the panel: `dst` must be a writable region of at least
 * CAP_PANEL_CHARS bytes, and every re-render copies EXACTLY that many bytes into
 * it and never a NUL. That is how the tool layer gets a live list into a static
 * tool description whose terminator is already in place — the panel is the tail
 * of a longer string literal. Binding renders immediately. Pass NULL to unbind.
 *
 * A pointer into another module's buffer is not free of taste, and the shape was
 * chosen over the two alternatives deliberately: the tool descriptor's
 * `description` is a plain `const char *` read by core/tool.c at request-build
 * time, so there is no getter to hook, and a description this module owned
 * outright would put the model-facing prose in the wrong file. */
void capability_panel_bind(char *dst, size_t cap);

/* 1 if the panel is exactly CAP_PANEL_CHARS characters and free of the bytes
 * that would change its escaped length. `msg` (optional) gets both numbers.
 * The tool layer calls this and refuses to advertise a panel that fails, since
 * the alternative is a schema whose size depends on the disk. */
int capability_panel_check(char *msg, size_t cap);

/* ---- test injection ---- */

/* The syscall backend capability programs are run against. Defaults to
 * dvm_sys_kernel(), which is NULL on a host build — so a host test supplies a
 * synthetic kernel here and drives the real dispatcher. Passing NULL restores
 * the default. */
void capability_set_sys(const dvm_sys_t *sys);

#endif /* CAPABILITY_H */
