/* cc.h — the machine compiles C, on itself, at run time.
 *
 * PURPOSE
 *   Everything the model can author on this machine today it authors in a
 *   bespoke assembly language (include/dvm.h) that exists nowhere but this
 *   repository. That is a real ceiling and it is not a ceiling on the machine:
 *   it is a ceiling on the model, which has read essentially every line of C
 *   ever published and almost none of this ISA. Asking it to write a 40-line
 *   driver in dvm assembly is asking it to work from the least of its training;
 *   asking it for the same thing in C is asking for the most.
 *
 *   So: a C compiler, in the kernel, whose input is a string the model produced
 *   this turn and whose output is machine code this machine then executes. A
 *   capability written in C, saved by name, and callable next boot.
 *
 *   The operator's requirement was "we should not be constrained by 'oh no there
 *   is no tool for this'". A tool is a C function. This is the part of the
 *   machine that turns the second sentence into the first.
 *
 * THE SUBSET — decided, bounded, and REFUSED WHEN EXCEEDED
 *   A compiler that miscompiles quietly is worse than no compiler, because the
 *   model cannot see the difference and neither can the operator. Every
 *   construct below is either implemented or rejected with a line, a column and
 *   the offending text; there is no third category and no silent best effort.
 *
 *   IN
 *     types        void, char, signed/unsigned char, short, int, long,
 *                  long long and their unsigned forms; enum; struct; union;
 *                  pointers to any depth; arrays of any rank; typedef.
 *                  uint8_t..uint64_t, int8_t..int64_t, size_t, intptr_t and
 *                  uintptr_t are PREDECLARED — no #include is needed for them.
 *     decls        globals with constant initialisers, locals, static locals,
 *                  extern declarations, forward declarations, function
 *                  definitions with up to CC_MAX_PARAMS parameters.
 *     statements   if/else, while, do/while, for, switch/case/default, break,
 *                  continue, return, compound statements, declarations
 *                  anywhere in a block, the empty statement.
 *     expressions  the whole integer operator set: + - * / % << >> & | ^ ~ !
 *                  && || == != < <= > >= ?: , assignment and every compound
 *                  assignment, prefix and postfix ++/--, unary & and *, . and
 *                  ->, [], sizeof (type and expression), casts, string and
 *                  character literals, decimal/hex/octal integers with u/l
 *                  suffixes, calls.
 *     initialisers scalar, string-literal-into-char-array, and brace lists for
 *                  arrays and structs (nested), with the tail zero-filled.
 *     preprocessor #define (object-like and function-like), #undef, #ifdef,
 *                  #ifndef, #else, #endif, #include of a built-in header or of
 *                  a file in the compiler's include root. Comments, both forms.
 *
 *   OUT — each one produces a diagnostic naming itself, never wrong code
 *     float, double and long double        there is no FPU state in this kernel
 *                                          (the build is -mno-sse) and integer
 *                                          arithmetic is what drivers are made
 *                                          of. `float` is a parse error, not a
 *                                          silently-truncated int.
 *     varargs (`...`)                      see THE SYMBOL TABLE: a variadic
 *                                          callee reads registers its caller
 *                                          never set, and the whole security
 *                                          argument here is that every call has
 *                                          a checked, fixed arity.
 *     function pointers                    see THE SYMBOL TABLE. This is the
 *                                          load-bearing exclusion.
 *     goto and labels                      unstructured flow makes the loop
 *                                          back-edge fuel check (see BOUNDING
 *                                          A NATIVE PROGRAM) unsound, and that
 *                                          check is what makes a compiled
 *                                          program guaranteed to terminate.
 *     struct/union by value in a
 *       parameter, argument or return      the ABI for it is fiddly, wrong in
 *                                          interesting ways, and unnecessary:
 *                                          pass a pointer. Struct ASSIGNMENT
 *                                          between objects IS supported.
 *     bitfields, _Complex, VLAs,
 *       K&R declarations, #if / #elif,
 *       # and ## in macros, _Generic,
 *       compound literals, designated
 *       initialisers, static assertions    all rejected by name.
 *     <stdio.h> and friends                there is no hosted libc; the
 *                                          diagnostic says so and names what
 *                                          IS available instead.
 *
 *   `const`, `volatile` and `register` are ACCEPTED AND IGNORED, which is
 *   stated here because it is the one place this file is knowingly not C: a
 *   write through a `const` pointer compiles. `static` on a file-scope object
 *   or function is accepted and affects nothing, because a compilation unit
 *   here is one string and there is no linker to hide a name from.
 *
 * THE TARGET: NATIVE x86-64, and what that cost
 *   Two targets were possible and the choice is not close.
 *
 *   The driver VM would have kept the sandbox: bounded cycles, every access
 *   traced, no way out. But DVM_MAX_INSNS is 256 instructions per program, and
 *   256 VM instructions is roughly twenty lines of C. Raising it means editing
 *   include/dvm.h and vm/dvm.c, which are another module's, and it would not fix
 *   the deeper mismatch — the VM has sixteen registers, no general memory
 *   addressing (its scratch arena is a separate address space reached only by
 *   offset), and no way to form the address of a local. A C compiler targeting
 *   it would spend most of its complexity emulating pointers.
 *
 *   So the target is native x86-64, which this kernel is unusually well set up
 *   for: boot/boot.asm maps every page present+writable and never sets NX, so a
 *   buffer in .bss is directly callable, and arch/x86_64 has an IDT that captures,
 *   decodes and reports a fault precisely.
 *
 *   WHAT THAT GIVES UP, PLAINLY, AND IT IS THE FIRST THING TO KNOW. There is no
 *   memory sandbox. A compiled program runs in ring 0 with no MMU behind it, and
 *   a wild pointer is a real store to real physical memory. The compiler inserts
 *   no bounds checks and does not pretend to. Measured, in QEMU, with the real
 *   model driving: a program that stores through `(long *)0x900000000` takes a
 *   #PF and THE MACHINE HALTS, with a report naming the vector, CR2, the whole
 *   register file and the sixteen bytes at RIP (`48 89 07`, which is this
 *   emitter's store instruction). It does not resume. An escape hatch was
 *   attempted and is refused for a good reason; cc.c's "A WILD POINTER" comment
 *   has the measurement, the refusal, and the two ways out — one of which is a
 *   change in arch/x86_64/ that is not this module's to make.
 *
 *   What IS bounded is TIME, STACK, and THE SET OF ADDRESSES CONTROL FLOW CAN
 *   REACH — the next two sections. Those three are chosen because they are what
 *   turn a mistake into a machine that will not come back at all; a wild store
 *   is at least a mistake with a precise report attached to it.
 *
 * BOUNDING A NATIVE PROGRAM — termination and stack, by construction
 *   Interrupts are off on this machine (IF is 0, every PIC line masked). A
 *   compiled `while (1) {}` would therefore be the end of the session: no timer,
 *   no preemption, nothing to break in. That is unacceptable for code a language
 *   model wrote, so the code generator emits two checks the program cannot skip:
 *
 *     AT EVERY LOOP BACK-EDGE AND EVERY CALL   `dec qword [r15]; js abort`.
 *         r15 holds the address of this module's runtime block for the whole of
 *         a run and is never loaded from memory the program can write. The
 *         counter is FUEL, set per run and never replenished, so the total
 *         number of loop iterations plus calls a program may execute is a number
 *         chosen by the kernel before it starts.
 *
 *     AT EVERY FUNCTION ENTRY, AFTER THE FRAME IS OPENED   `cmp rsp, [r15+8];
 *         jb abort`. Compiled code runs on ITS OWN static stack, switched to by
 *         the entry trampoline, so runaway recursion cannot reach the kernel's
 *         64 KiB stack at all; and a single frame is capped at CC_MAX_FRAME at
 *         compile time so the check cannot be jumped over by one enormous
 *         local.
 *
 *   `abort` is a stub at the end of the image that restores the kernel's rsp
 *   from the runtime block and returns through the trampoline's epilogue — a
 *   hand-built longjmp, with no C library and no inline assembly anywhere in
 *   this module. A program that runs out of fuel or stack is CC_EFUEL /
 *   CC_ESTACK with the numbers in the message; the machine is at the prompt.
 *
 * THE SYMBOL TABLE — the security boundary, and there is no MMU behind it
 *   Compiled code that cannot call the kernel is a calculator. So a curated
 *   table (compiler/cc_sym.c) names the kernel functions a compiled program may
 *   call, each with a fixed signature. Two properties do the bounding:
 *
 *     1. EVERY CALL TARGET IN GENERATED CODE IS A COMPILE-TIME CONSTANT. There
 *        are no function pointers in the subset, `switch` compiles to a compare
 *        chain and never a jump table, and there is no address-of-function
 *        operator. So the set of addresses a compiled program can transfer
 *        control to is exactly {its own functions} + {the table}, and it is
 *        fixed before the first instruction runs. A program cannot call a kernel
 *        function that is not in the table even by computing its address,
 *        because there is no instruction in the emitter that jumps to a computed
 *        value.
 *     2. EVERY ENTRY IS A FORWARDER THIS MODULE WROTE, not a raw kernel symbol.
 *        `print` is not kputs: it is cc_rt_print, which bounds the length, forces
 *        printable ASCII, and guarantees the kernel's column-zero '[' invariant
 *        (trace.h) survives a program that prints "[vfs_write ... -> ok]". File
 *        access is confined to the compiler's data root by cc_rt_*, which is a
 *        sibling of the store, so a compiled program cannot rewrite the
 *        machine's programs. The table is the interface; the forwarders are the
 *        enforcement.
 *
 *   What is NOT in the table, and why: no port I/O, no MMIO, no PCI, no DMA —
 *   hardware bring-up has a home already (driver_run/driver_install) and it has
 *   a policy engine this does not. No allocator: a compiled program gets its
 *   frame, its globals and nothing else, so it cannot leak. No network. No
 *   capability_invoke and no cc_call, so a compiled program cannot re-enter this
 *   module. See cc_sym.c, where each entry carries its one-line justification.
 *
 * NAMED, SAVED, REUSED
 *   compiler/cc_store.c keeps a program the way core/capability.c keeps a
 *   capability, deliberately in the same shape so the two read alike to a model:
 *   one file per program under the store root, 16 hex digits of FNV-1a-64, a
 *   newline, then a JSON record (net/json.c parses it; no new parser exists
 *   here). The SOURCE is what is kept, not the machine code — source is what a
 *   model can read, diff and repair, it is a fifth of the size, and it means a
 *   change to the code generator improves every saved program at its next call
 *   rather than leaving a museum of images built by an older compiler.
 *
 *   The store is FS_DISK_MOUNT "/cc" when a disk is mounted and "/cc" on ramfs
 *   otherwise; cc_store_persists() says which and every tool result says it out
 *   loud. The live list of saved programs is rendered into the cc_call tool
 *   description as a FIXED CC_PANEL_CHARS characters (see capability.h's
 *   DISCOVERABILITY: the same trick, for the same reason, and the same
 *   _Static_assert plus run-time width check defends it).
 *
 *   WHAT IS NOT UNIFIED, and it is worth saying: a compiled program is not a
 *   capability_t. core/capability.c's kinds are `program` (dvm source) and
 *   `compose`, and adding CC_KIND_NATIVE means editing that file, which is not
 *   this module's. The two stores are siblings and the shapes match on purpose,
 *   so the merge is a third `cap_kind_t` row and a branch in its dispatcher.
 *   Reported, not written.
 *
 * PUBLIC API
 *   cc_bringup / cc_init          boot: choose the store, load it, bind the panel
 *   cc_compile                    source -> the module's one code image
 *   cc_run                        run the last successful compile
 *   cc_compile_run                both, which is what the tool does
 *   cc_call                       run a SAVED program by name (compiles on
 *                                 demand, caches the image)
 *   cc_save / cc_delete           the store's mutations
 *   cc_count / cc_at / cc_find    the store's registry
 *   cc_source                     read a saved program's source back
 *   cc_diag_render                one diagnostic as the three lines a human and
 *                                 a model both read: location, message, the
 *                                 source line with a caret under the column
 *   cc_panel / cc_panel_bind / cc_panel_check    discovery
 *   cc_symbol_count / cc_symbol_at               the table, for documentation
 *
 * DEPENDENCIES
 *   vfs.h (the store), json.h (the record), kernel.h (the console and millis),
 *   trace.h (the ground-truth line), fs.h for FS_DISK_MOUNT only. No heap: the
 *   arena, the token array, the code image and the program stack are static, for
 *   the same reason capability.c's are — a compiler that stops working when
 *   memory is tight is one the machine cannot use to repair itself. No hardware
 *   at all, which is why tests/host/test_cc.c compiles AND EXECUTES real emitted
 *   machine code on the host.
 *
 * SIZE NOTE
 *   The static cost is CC_ARENA_BYTES + CC_CODE_MAX + CC_STACK_BYTES +
 *   CC_SRC_MAX * 2 + the token array + the store, all named below so the
 *   arithmetic is checkable rather than a surprise in a .bss listing.
 *
 * FUTURE EXTENSION POINTS
 *   - A THIRD cap_kind_t in core/capability.c, as above, is the real unification
 *     and the only one that gets compiled code into compositions and into
 *     apps/cap.c's broker.
 *   - PATCHING LIVE KERNEL CODE is what a native target uniquely makes possible
 *     and is not attempted here: it needs a symbol table of writable call sites
 *     and a way to revert one, and getting it wrong is unbootable rather than
 *     merely wrong.
 *   - Registering the code image with arch/x86_64's fault reporter would turn
 *     "fault at 0x...." into "fault in compiled program `foo`, line 12". The
 *     image base and a line table are already here; the consumer is not.
 *   - An optimiser. There is none: the code generator is a syntax-directed
 *     stack machine, every value goes through rax, and that is deliberate for
 *     one release. The IR to hang one off is cc_int.h's node tree.
 *   - Floating point, once the kernel has FPU state to save. -mno-sse is the
 *     blocker, not the parser.
 */
#ifndef CC_H
#define CC_H

#include <stdint.h>
#include <stddef.h>

/* ---- how much, all static; nothing here is allocated ---- */

/* Source bytes accepted for one compilation unit, before and after the
 * preprocessor. 16 KiB is ~500 lines, which is far more C than a tool call can
 * usefully carry and matches DVM_SRC_MAX so the two families refuse at the same
 * size. */
#define CC_SRC_MAX        16384

/* Tokens, counting the unit AND everything it #includes, before and after macro
 * expansion. There are two static arrays of this many (cc_lex.c explains why
 * they cannot come from the arena), so this number costs
 * 2 * CC_TOKENS_MAX * 40 bytes of .bss: 6144 is 480 KiB. Ordinary C is about
 * four source bytes per token, so 6144 comfortably covers CC_SRC_MAX plus the
 * built-in headers, and pathologically dense input (`a,a,a,`) hits the limit
 * with a diagnostic rather than a truncation. */
#define CC_TOKENS_MAX      6144

/* The parse arena: types, AST nodes, symbols, interned strings. Reset at the
 * start of every compile, so a failed compile leaks exactly nothing. An AST node
 * is ~150 bytes, which is the dominant term: 768 KiB is roughly 5000 nodes,
 * comfortably more than CC_SRC_MAX of C can produce. */
#define CC_ARENA_BYTES   786432

/* Machine code emitted for one unit. The generator is naive (~8 bytes per AST
 * node), so 32 KiB is roughly a 1500-line program and overflow is a diagnostic,
 * not a buffer run. */
#define CC_CODE_MAX        32768

/* Bytes of .bss for globals and string literals of the unit being compiled. */
#define CC_DATA_MAX        16384

/* The stack a compiled program runs on. Its OWN, switched to by the entry
 * trampoline, so recursion cannot reach the kernel's.
 *
 * CC_STACK_SLACK is the part a compiled function may NOT enter, and it is 16 KiB
 * rather than a token page for a specific reason: the per-function check runs at
 * function entry, so the deepest legal frame can sit exactly at the limit and
 * then CALL A KERNEL FUNCTION, and that kernel function runs on this stack.
 * fs/fat/fat_dir.c keeps 512-byte sector buffers on the stack and nests them, so
 * a file_write() from the deepest legal frame needs real room below it. The
 * program gets CC_STACK_BYTES - CC_STACK_SLACK; the kernel gets the rest. */
#define CC_STACK_BYTES     65536
#define CC_STACK_SLACK     16384

/* One function's locals, in bytes. This is what makes the stack check sound: a
 * frame bigger than the slack could step over the guard before the check runs,
 * so a frame bigger than this is refused at compile time with its size. */
#define CC_MAX_FRAME        2048

_Static_assert(CC_MAX_FRAME < CC_STACK_SLACK,
               "a single frame must fit inside the stack guard, or the "
               "post-prologue rsp check can be jumped over by one big local");

#define CC_MAX_FUNCS          64   /* functions in one unit                  */
#define CC_MAX_PARAMS          6   /* rdi rsi rdx rcx r8 r9; no stack args   */
#define CC_MAX_LOCALS        256   /* named objects live at once in one func  */
#define CC_MAX_MEMBERS        32   /* per struct/union                        */
#define CC_MAX_MACROS         64
#define CC_MAX_MACRO_PARAMS    8
#define CC_INCLUDE_DEPTH       4
#define CC_INCLUDES_MAX        8
#define CC_MAX_SWITCH_CASES   64
#define CC_MAX_BLOCK_DEPTH    32   /* also the parser's recursion bound      */

/* ---- THE COMPILER'S OWN STACK, which is not the compiled program's --------
 *
 * CC_STACK_BYTES above bounds the PROGRAM at run time. This bounds the COMPILER
 * at compile time, and it is a different hazard with a worse failure mode.
 *
 * The compiler recurses over the AST, and it does so on whatever stack its
 * caller was standing on. On this machine that is the root fiber's 64 KiB boot
 * stack, which has the identity-map page tables directly beneath it (see
 * boot/boot.asm). CC_MAX_BLOCK_DEPTH bounds NESTING only; it does not bound the
 * code generator, because a left-leaning chain like `1+1+1+...` is PARSED by an
 * iterative precedence loop (constant parse depth) and then WALKED RECURSIVELY
 * by cc_x64.c's gen_expr(), whose depth is the number of terms. Nothing bounded
 * that but CC_SRC_MAX, so ~2 KB of legal, in-subset, successfully-compiling C
 * — an ordinary model message — overflowed the root stack into the page tables
 * and triple-faulted: no #PF, no fault record, no diagnosis, no self-repair.
 *
 * SO THE BOUND IS MEASURED, NOT GUESSED. cc_compile() records its own frame
 * address and refuses to descend more than CC_STACK_BUDGET below it, and — when
 * something told it where the real stack ends — no closer than
 * CC_STACK_RESERVE to that end, which is what covers being called from an
 * already-deep caller. Every recursive production tests it. Exceeding it is a
 * diagnostic naming the construct, exactly like every other limit here.
 *
 * The two numbers are checked against each other at bring-up rather than
 * asserted at compile time, because only the running kernel knows how big its
 * stack actually is: see cc_stack_report() and the line it prints. */
#define CC_STACK_BUDGET   (24u * 1024u)
#define CC_STACK_RESERVE  (6u * 1024u)

_Static_assert(CC_STACK_BUDGET + CC_STACK_RESERVE < 65536u,
               "the compiler's stack budget plus its reserve must fit inside "
               "boot/boot.asm's 64 KiB root stack with room for the caller");

/* Tell the compiler where the stack it is running on ends, so it can keep
 * CC_STACK_RESERVE clear of that end whatever depth its caller was at. Pass
 * (0, 0) to forget. Idempotent, and safe to never call: without it the budget
 * is still enforced relative to cc_compile()'s own frame. */
void cc_stack_limit_set(const void *stack_lo);

/* Print the compiler's stack budget against the stack actually available, and
 * return 0 when they are consistent. Called from cc_bringup(); prints BOTH
 * numbers either way, because a constant that mirrors a measured value is the
 * defect class that has broken this tree more than once. */
int cc_stack_report(void);

/* Compiler temporaries per STATEMENT. A compound assignment (`a[i] += b`) and
 * `++`/`--` each need one hidden pointer so the lvalue is evaluated exactly
 * once. They are reused between statements — a temporary cannot outlive the
 * expression that made it, because no expression in this subset contains a
 * statement — so this is the most any single statement may need, not the most a
 * function may. Exceeding it is a diagnostic naming the statement. */
#define CC_TEMP_SLOTS         16
#define CC_TEMP_BYTES         (CC_TEMP_SLOTS * 8)

/* Fuel: loop back-edges plus calls a program may execute. The default is what
 * cc_run() uses when the caller asks for none, and the maximum is what a tool
 * may raise it to. 2 million iterations is ~0.2 s of real hardware and a few
 * seconds under QEMU's interpreter, which is the right order for "the operator
 * is waiting at a prompt". */
#define CC_FUEL_DEFAULT   2000000ull
#define CC_FUEL_MAX      50000000ull

/* ---- the store ---- */
#define CC_MAX_PROGRAMS       16
#define CC_NAME_MAX           32   /* incl. NUL */
#define CC_DOC_MAX           200   /* incl. NUL */
#define CC_PARAM_NAME_MAX     20   /* incl. NUL */
#define CC_FILE_MAX           40   /* a unit or include name, incl. NUL */

/* One record on disk: CC_SRC_MAX doubles in the worst case of JSON escaping,
 * the metadata is under 1 KiB. */
#define CC_RECORD_MAX      36864

/* ---- reported text ---- */
/* A diagnostic sentence. These messages ARE the product — they are what a model
 * reads to repair its own source in one turn — so the budget is generous enough
 * to say what is wrong AND what to write instead. cc_result_t carries
 * CC_DIAG_MAX of them and lives on a caller's stack, which is what bounds it. */
#define CC_MSG_MAX           288
#define CC_SNIP_MAX           98   /* the offending source line, elided       */
/* Four, not more: the first diagnostic is nearly always the real one and the
 * rest are the parser confused by it. */
#define CC_DIAG_MAX            4
#define CC_OUT_MAX          1024   /* text a running program printed          */

/* The live discovery panel, in characters, excluding the NUL. Every rendering
 * is EXACTLY this long — see cc_panel(). Schema is the scarcest resource in this
 * tree: include/chat.h's CHAT_TOOLS_BYTES had under 4.4 KiB unspent when this
 * family was written, and the C subset manual in cc_compile's description had to
 * come out of the same budget. So this is half of what capability.h spends, and
 * the renderer ends a full panel with "+N more (cc_source)" rather than half a
 * name. If the schema budget is ever grown, this is the first number that should
 * get some of it. */
#define CC_PANEL_CHARS       384

/* ---- result codes (errno-style negatives; 0 = success) ---- */
#define CC_OK           0
#define CC_ENOENT      -2    /* no saved program of that name                */
#define CC_EIO         -5    /* the store could not be read or written       */
#define CC_EINVAL     -22    /* bad arguments to a public call               */
#define CC_ENOSPC     -28    /* a fixed buffer or the store is full          */
#define CC_ESYNTAX    -90    /* the source is not in the subset; see diag[]  */
#define CC_ETYPE      -91    /* it parses but does not type-check            */
#define CC_ELIMIT     -92    /* it is legal C but too big for a limit above  */
#define CC_EFUEL      -93    /* the program ran out of fuel                  */
#define CC_ESTACK     -94    /* the program ran out of its own stack         */
#define CC_ENOEXEC    -95    /* this build cannot execute x86-64 code        */
#define CC_ECORRUPT   -96    /* a stored record failed its own checksum      */

/* One diagnostic. Position is 1-based and points at the offending TOKEN, which
 * is what makes the caret land somewhere a model can act on. */
typedef struct cc_diag {
    int  line, col;
    char file[CC_FILE_MAX];
    char msg[CC_MSG_MAX];
    char text[CC_SNIP_MAX];      /* the source line, tabs expanded, elided */
    int  caret;                  /* column within `text` for the caret, 1-based */
} cc_diag_t;

/* What one compile (and optional run) produced. ~2 KiB; one per call, on the
 * caller's stack. */
typedef struct cc_result {
    int      rc;

    /* compile */
    uint32_t ndiag;
    cc_diag_t diag[CC_DIAG_MAX];
    uint32_t code_bytes;         /* machine code emitted                    */
    uint32_t data_bytes;         /* globals plus string literals            */
    uint32_t nfuncs;
    uint32_t tokens;
    uint32_t entry_params;       /* arity of main()                         */

    /* run */
    int      ran;                /* 1 if the image was actually entered     */
    uint64_t value;              /* what main() returned                    */
    uint64_t fuel_used;
    uint64_t stack_used;         /* deepest byte of the program stack used  */
    uint32_t ncalls;             /* kernel-symbol calls made                */
    size_t   out_len;
    char     out[CC_OUT_MAX];    /* what the program printed, printable ASCII */
} cc_result_t;

/* One saved program, as held in RAM. The SOURCE is on disk and is read into a
 * single shared buffer for the compile that needs it. */
typedef struct cc_prog {
    char     name[CC_NAME_MAX];
    char     doc[CC_DOC_MAX];
    uint32_t version;                             /* monotonic, from 1      */
    uint8_t  nparam;
    char     param[CC_MAX_PARAMS][CC_PARAM_NAME_MAX];
    uint32_t src_len;
    uint64_t crc;                                 /* of the stored record   */
    uint32_t calls, failures;                     /* since boot             */
} cc_prog_t;

/* One curated kernel entry point, for documentation and for the emitter. */
typedef struct cc_symbol {
    const char *decl;   /* exactly as a C declaration, e.g. "long time_ms(void)" */
    const char *why;    /* one line: why a compiled program may call it     */
} cc_symbol_t;

/* ---- lifecycle ---- */

/* THE ONE CALL kernel/main.c makes: bind the live panel into tools/cc_tools.c's
 * cc_call description, then cc_init(). Same ordering argument as
 * capability_bringup() — a tool descriptor's description is a plain const char *
 * read by core/tool.c at request-build time, so the panel must live in the tool
 * layer's buffer and must be filled before the first schema is assembled.
 * Implemented in tools/cc_tools.c; nothing in compiler/ calls it. */
void cc_bringup(void);

/* Choose the store, create it and its data root, load every record, render the
 * panel. Idempotent. Never fails fatally: a machine whose store is unreadable
 * still boots, with no saved programs and one line saying so. */
void cc_init(void);

const char *cc_store_root(void);   /* "/disk/cc" or "/cc"; never NULL       */
const char *cc_data_root(void);    /* where a program's file calls land     */
int         cc_store_persists(void);
int         cc_rejected(void);      /* records refused by the last cc_init() */

/* ---- compile and run ---- */

/* Compile `src` (need not be NUL-terminated) into the module's one code image.
 * `unit` names it in diagnostics ("main.c" if NULL). `res` is always fully
 * populated; on failure res->ndiag is at least 1. Returns CC_OK or a negative
 * code. Compiling again discards the previous image. */
int cc_compile(const char *src, size_t len, const char *unit, cc_result_t *res);

/* Run the last successful cc_compile's image. A WILD POINTER IS NOT CAUGHT: it
 * takes a CPU fault, arch/x86_64 prints the vector, CR2, the register file and
 * the bytes at RIP, and the machine halts. cc.c documents the attempt that was
 * made to escape that and exactly why it is refused.
 *
 * `nargs` must equal the arity of its main(); a mismatch is CC_EINVAL naming both
 * numbers. `fuel` of 0 means CC_FUEL_DEFAULT; anything above CC_FUEL_MAX is
 * clamped. `res` may be the same struct cc_compile() filled, and its run fields
 * are overwritten. */
int cc_run(const uint64_t *args, int nargs, uint64_t fuel, cc_result_t *res);

/* cc_compile then cc_run, which is what the tool does. */
int cc_compile_run(const char *src, size_t len, const char *unit,
                   const uint64_t *args, int nargs, uint64_t fuel,
                   cc_result_t *res);

/* Run a SAVED program by name: reads its source, compiles it (reusing the
 * cached image when the store has not changed), and runs it. CC_ENOENT names
 * what does exist. */
int cc_call(const char *name, const uint64_t *args, int nargs, uint64_t fuel,
            cc_result_t *res);

/* ---- the store ---- */

/* 1 if `name` is usable: 1..CC_NAME_MAX-1 bytes, a lowercase letter then
 * lowercase letters, digits and underscore. `why` (optional) names the
 * offending byte. Exposed so the tool layer refuses with the store's own rule. */
int cc_name_ok(const char *name, char *why, size_t cap);

int            cc_count(void);
const cc_prog_t *cc_at(int index);
const cc_prog_t *cc_find(const char *name);

/* Read a saved program's source into `dst`. Returns bytes written, or a
 * negative code. Always NUL-terminates when cap > 0. */
int cc_source(const char *name, char *dst, size_t cap);

/* Compile `src` and, only if it compiles AND its main() matches `nparam`, save
 * it as `name`. Everything is checked before anything is written, so a saved
 * program has been through the code generator at least once. Replacing an
 * existing name is a new version with the previous one kept as <name>.bak.
 * `res` carries the compile's diagnostics. */
int cc_save(const char *name, const char *doc,
            const char *const *params, int nparam,
            const char *src, size_t len, cc_result_t *res,
            char *msg, size_t msgcap);

int cc_delete(const char *name, char *msg, size_t msgcap);

/* ---- diagnostics ---- */

/* Render one diagnostic as up to three lines:
 *     main.c:7:9: cannot take the address of a value that has no address
 *         q = &(a + b);
 *             ^
 * Returns the length written. Never emits a '[' in column zero (trace.h). */
size_t cc_diag_render(const cc_diag_t *d, char *dst, size_t cap);

/* Every diagnostic in `res`, rendered, newline-separated. */
size_t cc_diags_render(const cc_result_t *res, char *dst, size_t cap);

/* ---- discovery ---- */

/* The live list of saved programs as ONE line of exactly CC_PANEL_CHARS
 * printable characters, space-padded, containing no '"', no '\\' and no control
 * byte — so its JSON-escaped length is constant and the assembled tool schema
 * does not change size when a program is saved. Never NULL. */
const char *cc_panel(void);

/* Bind a destination of at least CC_PANEL_CHARS bytes; every re-render copies
 * EXACTLY that many bytes and never a NUL. Binding renders immediately. NULL
 * unbinds. Same shape and same reasoning as capability_panel_bind(). */
void cc_panel_bind(char *dst, size_t cap);

/* 1 if the panel is exactly CC_PANEL_CHARS characters and free of the bytes
 * that would change its escaped length. `msg` gets both numbers. */
int cc_panel_check(char *msg, size_t cap);

/* ---- the curated symbol table (compiler/cc_sym.c) ---- */
int                 cc_symbol_count(void);
const cc_symbol_t  *cc_symbol_at(int index);

/* ---- #include, and where it reads from ---- */

/* What `#include "name.h"` resolves against. Returns bytes read, or negative if
 * there is no such file. The tool layer points this at the compiler's private
 * file directory - the same one a compiled program's file_write() lands in - so
 * a program can write a header that a later compile uses. A host test replaces
 * it. NULL (the default) means #include of a file is refused, saying so; the
 * built-in headers still work. */
typedef int (*cc_read_fn)(const char *name, char *dst, size_t cap);
void cc_set_include_reader(cc_read_fn fn);

/* ---- run counters ---- */

/* Zero the counters and the output capture a run reports. cc_run() calls this
 * itself; it is public so a test can assert that a count is a count and not a
 * running total. There is deliberately NO hook to move the data root: the real
 * one is a directory in the real VFS, which a host test can create and inspect,
 * so a redirect would only make the tests weaker than the thing they test. */
void cc_rt_reset(void);

/* Runtime calls REFUSED for an out-of-range length or an unusable file name.
 * Reported alongside the run, because a silently-refused memcpy is a wrong
 * answer that looks like a right one. */
uint32_t cc_rt_refusals(void);

#endif /* CC_H */
