/* cc.c — the driver: the arena, the diagnostic sink, and running the result.
 *
 * FOUR THINGS LIVE HERE and they are together because they are the parts every
 * stage touches:
 *
 *   THE ARENA. One static block, a bump pointer, reset at the start of every
 *   compile. Types, AST nodes, symbols, token arrays and interned strings all
 *   come from it and none of them is ever freed individually. A compile that
 *   fails half way therefore leaks exactly nothing, which is the property that
 *   matters when the input is a string a language model produced and the failure
 *   rate is not small. There is no malloc in this compiler.
 *
 *   THE DIAGNOSTIC SINK. cc_errorf() records position, message and the offending
 *   source line, and returns 0 so that `return cc_errorf(...)` unwinds a parser
 *   by returning. cc_ctx.failed is the flag every loop tests. The FIRST
 *   diagnostic is the one that matters — the rest are usually the parser
 *   confused by the first — so the sink keeps them in order and the tool reports
 *   them in order.
 *
 *   THE RUNTIME BLOCK, and ENTERING GENERATED CODE. cc_rt is what r15 points at
 *   for the whole of a run: fuel, the stack limit, and the kernel rsp the abort
 *   stub returns to. Entering compiled code is one indirect call through a
 *   function pointer to the image's first byte, which is the trampoline the code
 *   generator emitted; there is no inline assembly in this module and no
 *   setjmp, because the unwind is the abort stub's `mov rsp, [r15+16]`.
 *
 *   WHERE THE CODE BUFFER COMES FROM, which is the one place this file is aware
 *   of not being the kernel. On the kernel a static array IS executable:
 *   boot/boot.asm maps every page present+writable and never sets NX, and that
 *   is a documented, deliberate property of this machine. On a host the same
 *   array is not, so the host build takes its code buffer from mmap and toggles
 *   PROT_WRITE / PROT_EXEC around each compile. Everything else — the emitter,
 *   the bytes, the trampoline, the fuel checks — is identical, which is what
 *   makes tests/host/test_cc.c able to EXECUTE what the code generator emits
 *   rather than only inspect it.
 */

#include "cc_int.h"

/* kernel.h BEFORE <string.h>: kernel.h declares memcpy/memset/memmove/memcmp
 * itself, and a host <string.h> turns those names into fortified macros, so the
 * other order makes the declarations unparseable. Every module in this tree
 * follows the same order for the same reason. Needed here for kprintf(), which
 * cc_stack_report() uses to say both numbers out loud. */
#include "kernel.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(FABLEOS_HOSTTEST)
#  include <sys/mman.h>
#  ifndef MAP_ANON
#    define MAP_ANON MAP_ANONYMOUS
#  endif
#  ifndef MAP_JIT
#    define MAP_JIT 0
#  endif
#endif

/* ===================================================================== */
/* state                                                                 */
/* ===================================================================== */

cc_ctx_t cc_ctx;
cc_rt_t  cc_rt;

static uint8_t g_arena[CC_ARENA_BYTES];
static size_t  g_arena_used;
static int     g_arena_full;      /* report the overflow once, not per node */

/* The unit's data image: globals then string literals. Static, and its ADDRESS
 * is baked into generated code as a 64-bit immediate, which is legal here
 * because the low 4 GiB is identity-mapped and this array never moves. */
static uint8_t g_data[CC_DATA_MAX] __attribute__((aligned(16)));

/* The compiled program's own stack. Not the kernel's: see cc.h under BOUNDING A
 * NATIVE PROGRAM. 16-aligned at the top, with CC_STACK_SLACK of guard at the
 * bottom that the per-function rsp check enforces. */
static uint8_t g_stack[CC_STACK_BYTES] __attribute__((aligned(16)));

static size_t g_code_len;         /* bytes of the last successful compile   */
static int    g_have_image;
static int    g_entry_params;

/* Bumped on every successful compile. cc_store.c records it alongside a cached
 * image so that ANY intervening compile — from the store, from a tool, from a
 * test — invalidates that cache without this file knowing who did it. A name and
 * a version are not enough: compiling something else and then calling the
 * cached name would run the wrong bytes. */
static uint64_t g_serial;

/* ===================================================================== */
/* the code buffer                                                       */
/* ===================================================================== */

#if defined(FABLEOS_HOSTTEST)

static uint8_t *g_code;

static uint8_t *code_buffer(void) {
    if (!g_code) {
        void *p = mmap(NULL, CC_CODE_MAX, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) return NULL;
        g_code = (uint8_t *)p;
    }
    return g_code;
}
static int code_make_writable(void) {
    return g_code && mprotect(g_code, CC_CODE_MAX, PROT_READ | PROT_WRITE) == 0;
}
static int code_make_executable(void) {
    return g_code && mprotect(g_code, CC_CODE_MAX, PROT_READ | PROT_EXEC) == 0;
}

#else

/* Every page on this machine is present+writable and NX is never set, so a
 * .bss array is directly callable. That is why the RWX linker warning is
 * expected and intentional, and it is what makes this feature possible at all. */
static uint8_t g_code_static[CC_CODE_MAX] __attribute__((aligned(16)));

static uint8_t *code_buffer(void)     { return g_code_static; }
static int      code_make_writable(void)   { return 1; }
static int      code_make_executable(void) { return 1; }

#endif

/* ===================================================================== */
/* the arena                                                             */
/* ===================================================================== */

void cc_arena_reset(void) {
    g_arena_used = 0;
    g_arena_full = 0;
}

void *cc_alloc(size_t n) {
    size_t want = (n + 15u) & ~(size_t)15u;
    if (want < n || want > sizeof g_arena || g_arena_used + want > sizeof g_arena) {
        if (!g_arena_full) {
            g_arena_full = 1;
            cc_errorf(NULL,
                      "this unit needs more than %d bytes of compiler working "
                      "memory; split it into smaller saved programs",
                      CC_ARENA_BYTES);
        }
        return NULL;
    }
    void *p = g_arena + g_arena_used;
    g_arena_used += want;
    memset(p, 0, want);
    return p;
}

/* ---- interning ----
 * Every identifier, spelling and string literal is copied into the arena. Doing
 * it through a hash table rather than a fresh copy per token is not premature:
 * 8192 tokens of ordinary C mention perhaps 200 distinct spellings, and the
 * difference is tens of kilobytes of a 256 KiB arena. */
#define INTERN_BUCKETS 256
typedef struct intern_node { const char *s; struct intern_node *next; } intern_node_t;
static intern_node_t *g_bucket[INTERN_BUCKETS];

const char *cc_intern(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    unsigned b = h % INTERN_BUCKETS;
    for (intern_node_t *it = g_bucket[b]; it; it = it->next)
        if (strlen(it->s) == n && memcmp(it->s, s, n) == 0) return it->s;

    char *copy = cc_alloc(n + 1);
    if (!copy) return "";
    if (n) memcpy(copy, s, n);
    copy[n] = '\0';
    intern_node_t *node = cc_alloc(sizeof *node);
    if (!node) return copy;
    node->s = copy;
    node->next = g_bucket[b];
    g_bucket[b] = node;
    return copy;
}

static void intern_reset(void) {
    for (int i = 0; i < INTERN_BUCKETS; i++) g_bucket[i] = NULL;
}

/* ===================================================================== */
/* diagnostics                                                           */
/* ===================================================================== */

/* Copy the source line `line` (1-based) of the unit into `dst`, expanding tabs,
 * windowing on `col` when the line is longer than the buffer, and reporting the
 * caret column within what was copied. */
static void snippet(int line, int col, char *dst, size_t cap, int *caret_out) {
    dst[0] = '\0';
    *caret_out = 0;
    if (!cc_ctx.src || line < 1) return;

    const char *p = cc_ctx.src, *end = cc_ctx.src + cc_ctx.src_len;
    int l = 1;
    while (l < line && p < end) { if (*p++ == '\n') l++; }
    if (l != line) return;
    const char *ls = p;
    while (p < end && *p != '\n') p++;
    const char *le = p;

    /* Expand tabs to the same 8-column stops the lexer counted, so the caret
     * lands where the lexer said it would. */
    char exp[CC_SNIP_MAX * 4];
    size_t n = 0;
    for (const char *q = ls; q < le && n + 8 < sizeof exp; q++) {
        if (*q == '\t') { do { exp[n++] = ' '; } while (n % 8 != 0); }
        else if ((unsigned char)*q >= 0x20 && (unsigned char)*q < 0x7f) exp[n++] = *q;
        else exp[n++] = ' ';
    }
    exp[n] = '\0';

    size_t room = cap - 1;
    if (n <= room) {
        memcpy(dst, exp, n + 1);
        *caret_out = (col >= 1 && (size_t)col <= n + 1) ? col : 0;
        return;
    }
    /* Window so the caret is visible, with a leading "..." when clipped. */
    size_t start = 0;
    if ((size_t)col > room / 2) start = (size_t)col - room / 2;
    if (start + room > n) start = n - room;
    size_t lead = start ? 3 : 0;
    if (lead) { memcpy(dst, "...", 3); start += 3; }
    size_t take = room - lead;
    if (start + take > n) take = n - start;
    memcpy(dst + lead, exp + start, take);
    dst[lead + take] = '\0';
    long c = (long)col - (long)start + (long)lead;
    *caret_out = (c >= 1 && c <= (long)(lead + take) + 1) ? (int)c : 0;
}

static void record(const cc_token_t *tok, const char *fmt, va_list ap) {
    cc_ctx.failed = 1;
    if (cc_ctx.ndiag >= CC_DIAG_MAX) return;
    cc_diag_t *d = &cc_ctx.diag[cc_ctx.ndiag++];
    memset(d, 0, sizeof *d);

    if (tok) {
        d->line = tok->line;
        d->col  = tok->col;
        snprintf(d->file, sizeof d->file, "%s",
                 tok->file ? tok->file : (cc_ctx.unit ? cc_ctx.unit : "main.c"));
    } else {
        snprintf(d->file, sizeof d->file, "%s", cc_ctx.unit ? cc_ctx.unit : "main.c");
    }
    vsnprintf(d->msg, sizeof d->msg, fmt, ap);

    /* The snippet can only come from the unit's own text; an included header's
     * bytes are not kept after tokenizing. */
    if (tok && cc_ctx.unit && d->file[0] &&
        strcmp(d->file, cc_ctx.unit) == 0)
        snippet(d->line, d->col, d->text, sizeof d->text, &d->caret);
}

int cc_errorf(const cc_token_t *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    record(tok, fmt, ap);
    va_end(ap);
    return 0;
}

void *cc_errorp(const cc_token_t *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    record(tok, fmt, ap);
    va_end(ap);
    return NULL;
}

size_t cc_diag_render(const cc_diag_t *d, char *dst, size_t cap) {
    if (!cap) return 0;
    size_t n = 0;
    int w;
    w = snprintf(dst, cap, "%s:%d:%d: %s", d->file, d->line, d->col, d->msg);
    n = (w < 0) ? 0 : ((size_t)w >= cap ? cap - 1 : (size_t)w);
    if (d->text[0] && n + 2 < cap) {
        w = snprintf(dst + n, cap - n, "\n    %s", d->text);
        n += (w < 0) ? 0 : ((size_t)w >= cap - n ? cap - n - 1 : (size_t)w);
        if (d->caret > 0 && n + 6 < cap) {
            w = snprintf(dst + n, cap - n, "\n    ");
            n += (size_t)w;
            int spaces = d->caret - 1;
            while (spaces-- > 0 && n + 2 < cap) dst[n++] = ' ';
            if (n + 1 < cap) dst[n++] = '^';
            dst[n] = '\0';
        }
    }
    return n;
}

size_t cc_diags_render(const cc_result_t *res, char *dst, size_t cap) {
    if (!cap) return 0;
    dst[0] = '\0';
    size_t n = 0;
    for (uint32_t i = 0; i < res->ndiag && n + 2 < cap; i++) {
        if (i) { dst[n++] = '\n'; dst[n] = '\0'; }
        n += cc_diag_render(&res->diag[i], dst + n, cap - n);
    }
    return n;
}

/* ===================================================================== */
/* compile                                                               */
/* ===================================================================== */

static void result_reset(cc_result_t *res) {
    memset(res, 0, sizeof *res);
}

/* Non-zero if `s` is entirely printable ASCII plus \n and \t. Model output
 * arrives with embedded NULs; the lexer would catch them, but rejecting them
 * here means the diagnostic names the whole input rather than one byte deep
 * inside a string literal. */
static int text_ok(const char *s, size_t n, size_t *bad_at, int *bad_byte) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\t' || c == '\r') continue;
        if (c < 0x20 || c > 0x7e) { *bad_at = i; *bad_byte = c; return 0; }
    }
    return 1;
}

/* ===================================================================== */
/* the compiler's own stack floor                                        */
/* ===================================================================== */

/* See "THE COMPILER'S OWN STACK" in include/cc.h. */
const unsigned char *cc_stack_floor_addr;

/* The low end of the stack this code runs on, if anybody said. Zero means
 * "unknown", in which case the budget is enforced relative to cc_compile()'s
 * own frame and nothing else. */
static const unsigned char *g_stack_lo;

void cc_stack_limit_set(const void *stack_lo) {
    g_stack_lo = (const unsigned char *)stack_lo;
}

void cc_stack_floor_arm(const void *frame) {
    const unsigned char *fp = (const unsigned char *)frame;

    /* The budget, measured down from here. This is the bound that matters when
     * the caller was shallow, which is the normal case. */
    const unsigned char *floor = fp - CC_STACK_BUDGET;

    /* The reserve, measured up from the real end of the stack. This is the bound
     * that matters when the caller was already deep — an agenda item three
     * frames inside a model turn, say — and it is the one that keeps the
     * compiler off the poison band no matter who called it. Whichever floor is
     * HIGHER wins, because a higher floor is the stricter one. */
    if (g_stack_lo) {
        const unsigned char *hard = g_stack_lo + CC_STACK_RESERVE;
        if (hard > floor) floor = hard;
        /* A caller already below the reserve cannot compile at all, and saying
         * so is better than pretending there is room. floor > fp makes the very
         * first check fail, which is exactly right. */
    }
    cc_stack_floor_addr = floor;
}

int cc_stack_low(void) {
    if (!cc_stack_floor_addr) return 0;
    return (const unsigned char *)__builtin_frame_address(0) <= cc_stack_floor_addr;
}

int cc_stack_exhausted(const cc_token_t *tok) {
    if (!cc_stack_low()) return 0;
    /* Once, not once per frame on the way out: the unwind passes back through
     * every level and cc_errorf() would record CC_DIAG_MAX identical lines,
     * pushing out the one diagnostic that says where. */
    if (cc_ctx.failed) return 1;
    cc_errorf(tok,
              "this construct is too deeply nested for the compiler to walk: it "
              "would need more than %u bytes of kernel stack. Break the "
              "expression into named intermediate variables, or the function "
              "into smaller functions",
              (unsigned)CC_STACK_BUDGET);
    return 1;
}

int cc_stack_report(void) {
    /* How much stack is really below us, if anybody said. */
    const unsigned char *fp = (const unsigned char *)__builtin_frame_address(0);
    unsigned need = (unsigned)(CC_STACK_BUDGET + CC_STACK_RESERVE);

    if (!g_stack_lo) {
        kprintf("cc: compile-time stack budget %u bytes (+%u reserve); the size "
                "of the stack under it was never declared, so only the budget "
                "is enforced\n",
                (unsigned)CC_STACK_BUDGET, (unsigned)CC_STACK_RESERVE);
        return 0;
    }

    unsigned have = (fp > g_stack_lo) ? (unsigned)(fp - g_stack_lo) : 0u;
    if (have >= need) {
        kprintf("cc: compile-time stack budget %u bytes (+%u reserve) against "
                "%u bytes below this frame - ok\n",
                (unsigned)CC_STACK_BUDGET, (unsigned)CC_STACK_RESERVE, have);
        return 0;
    }

    /* BOTH NUMBERS, ALWAYS. A constant that mirrors a measured value going
     * stale is the defect class that has turned this tree red more than once,
     * and a report that says "mismatch" without the two figures makes the next
     * person measure it again. */
    kprintf("cc: STACK BUDGET DOES NOT FIT - the compiler is allowed %u bytes "
            "plus a %u byte reserve (%u total) but only %u bytes lie below this "
            "frame. Lower CC_STACK_BUDGET in include/cc.h or give this stack "
            "more room; until then a deep compile can reach the poison band.\n",
            (unsigned)CC_STACK_BUDGET, (unsigned)CC_STACK_RESERVE, need, have);
    return -1;
}

int cc_compile(const char *src, size_t len, const char *unit, cc_result_t *res) {
    if (!res) return CC_EINVAL;
    result_reset(res);
    g_have_image = 0;
    g_code_len   = 0;

    if (!src) {
        cc_ctx.unit = unit ? unit : "main.c";
        cc_ctx.src = NULL; cc_ctx.src_len = 0;
        cc_ctx.ndiag = 0; cc_ctx.failed = 0;
        cc_errorf(NULL, "there is no source to compile");
        res->ndiag = cc_ctx.ndiag;
        memcpy(res->diag, cc_ctx.diag, sizeof res->diag);
        res->rc = CC_EINVAL;
        return CC_EINVAL;
    }

    /* Arm the stack floor from THIS frame, before anything recurses. Every
     * recursive production below tests it; see include/cc.h. */
    cc_stack_floor_arm(__builtin_frame_address(0));

    memset(&cc_ctx, 0, sizeof cc_ctx);
    cc_arena_reset();
    cc_token_pool_reset();
    intern_reset();
    memset(g_data, 0, sizeof g_data);

    cc_ctx.unit    = cc_intern(unit ? unit : "main.c",
                               strlen(unit ? unit : "main.c"));
    cc_ctx.src     = src;
    cc_ctx.src_len = len;
    cc_ctx.data    = g_data;
    cc_ctx.data_len = 0;

    int rc = CC_ESYNTAX;

    if (len > CC_SRC_MAX) {
        cc_errorf(NULL, "the source is %d bytes; the limit is %d",
                  (int)len, CC_SRC_MAX);
        rc = CC_ELIMIT;
        goto out;
    }
    {
        size_t bad_at = 0; int bad_byte = 0;
        if (!text_ok(src, len, &bad_at, &bad_byte)) {
            cc_errorf(NULL,
                      "byte %d of the source is 0x%02x, which is not valid C "
                      "text (printable ASCII, tab and newline only)",
                      (int)bad_at, bad_byte);
            rc = CC_ESYNTAX;
            goto out;
        }
    }

    {
        cc_token_t *raw = NULL;
        int nraw = cc_tokenize(src, len, cc_ctx.unit, &raw);
        if (nraw < 0) goto out;

        cc_token_t *pp = NULL;
        int npp = 0;
        if (!cc_preprocess(raw, nraw, &pp, &npp)) goto out;

        cc_ctx.tok  = pp;
        cc_ctx.ntok = npp;
        cc_ctx.pos  = 0;
        res->tokens = (uint32_t)npp;
    }

    if (!cc_declare_symbols()) goto out;
    if (!cc_parse_unit()) { rc = CC_ETYPE; goto out; }

    {
        if (!code_buffer()) {
            cc_errorf(NULL, "no executable code buffer is available in this build");
            rc = CC_ENOEXEC;
            goto out;
        }
        if (!code_make_writable()) {
            cc_errorf(NULL, "the code buffer could not be made writable");
            rc = CC_ENOEXEC;
            goto out;
        }
        size_t clen = 0;
        if (!cc_codegen(code_buffer(), CC_CODE_MAX, &clen)) { rc = CC_ELIMIT; goto out; }
        if (!code_make_executable()) {
            cc_errorf(NULL, "the code buffer could not be made executable");
            rc = CC_ENOEXEC;
            goto out;
        }
        g_code_len   = clen;
        g_have_image = 1;
        res->code_bytes = (uint32_t)clen;
    }

    /* main() is the entry point. Asked for by name because every model on earth
     * knows what main is, and because a per-unit entry declaration is one more
     * thing to get wrong. */
    {
        int found = -1;
        for (int i = 0; i < cc_ctx.nfunc; i++)
            if (strcmp(cc_ctx.func[i].name, "main") == 0) { found = i; break; }
        if (found < 0) {
            cc_errorf(NULL,
                      "there is no main() in this unit. The entry point must be "
                      "called main and may take up to %d whole-number "
                      "parameters", CC_MAX_PARAMS);
            rc = CC_ETYPE;
            g_have_image = 0;
            goto out;
        }
        g_entry_params  = cc_ctx.func[found].nparam;
        res->entry_params = (uint32_t)g_entry_params;
    }

    res->nfuncs     = (uint32_t)cc_ctx.nfunc;
    res->data_bytes = (uint32_t)cc_ctx.data_len;
    g_serial++;
    res->rc         = CC_OK;
    res->ndiag      = cc_ctx.ndiag;
    memcpy(res->diag, cc_ctx.diag, sizeof res->diag);
    return CC_OK;

out:
    res->ndiag = cc_ctx.ndiag;
    memcpy(res->diag, cc_ctx.diag, sizeof res->diag);
    if (res->ndiag == 0) {
        /* Never report a failure with nothing to read: a diagnostic-free failure
         * is a bug in this file, and saying so is better than saying nothing. */
        cc_ctx.ndiag = 0;
        cc_errorf(NULL, "the compiler failed without recording a reason "
                        "(this is a compiler bug; please report the source)");
        res->ndiag = cc_ctx.ndiag;
        memcpy(res->diag, cc_ctx.diag, sizeof res->diag);
    }
    res->rc      = rc;
    g_have_image = 0;
    return rc;
}

/* ===================================================================== */
/* run                                                                   */
/* ===================================================================== */

/* The stack the program runs on, top-down, 16-aligned, with the guard at the
 * bottom. rsp is set to `top` and the per-function check refuses anything below
 * `limit`, so a program's whole stack footprint is
 * CC_STACK_BYTES - CC_STACK_SLACK and a frame that would step over the guard was
 * refused at compile time (CC_MAX_FRAME). */
static void stack_window(uint64_t *top, uint64_t *limit) {
    uintptr_t base = (uintptr_t)g_stack;
    uintptr_t hi   = (base + CC_STACK_BYTES) & ~(uintptr_t)15u;
    *top   = (uint64_t)hi;
    *limit = (uint64_t)(base + CC_STACK_SLACK);
}

typedef uint64_t (*cc_entry_t)(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);

/* ---- A WILD POINTER, AND WHY THIS DOES NOT CATCH ONE ----
 *
 * Fuel bounds time and the rsp check bounds stack, but nothing bounds a wild
 * pointer: `*(long *)0x900000000 = 1` in a compiled program is a real store to an
 * unmapped page. Measured in QEMU: #PF, error 2, CR2 = 0x900000000, RIP inside
 * the code buffer, `code @RIP : 48 89 07` — which is exactly the `mov [rdi], rax`
 * this emitter produces for a store — and then the machine HALTS with that
 * report, because no recovery plan was armed.
 *
 * arch/x86_64 has the right mechanism for this and it was TRIED HERE: wrapping
 * the entry in fault_guard_run() so a fatal fault abandons the call chain and
 * returns to the prompt. IT IS REFUSED, for a reason that is correct and is
 * written down in include/fault.h: an escape must return to a frame that
 * OUTLIVES the frames being discarded, so fault_escape() requires the guard's
 * saved rsp to be ABOVE the faulting rsp. A compiled program runs on its own
 * stack (g_stack, in .bss, at a higher address than the boot stack), so the
 * guard's saved rsp is BELOW the faulting one and the check fails —
 * "guard-corrupt ... the fault guard's saved stack pointer is not above the
 * faulting one", verified in QEMU. The machine then halts exactly as it would
 * with no guard, so the attempt cost nothing and bought nothing, and the code is
 * gone rather than left in as decoration.
 *
 * THE TWO WAYS OUT, neither of which is this module's to take:
 *   - Give up the private stack and run compiled code below the caller's rsp.
 *     Then the guard works, but runaway recursion is back on the kernel's 64 KiB
 *     stack, which is shared with lwIP and mbedTLS. That trades a verified
 *     property for an unverified one.
 *   - Teach fault_guard_t that its body ran on a DIFFERENT stack whose bounds the
 *     arming code declares. The struct already records a stack identity for the
 *     fiber case; this is one more field and one more branch in fault_escape().
 *     That is a change in arch/x86_64/, which this module does not own.
 * Reported, not written. Until then: a wild pointer in a compiled program halts
 * the machine, loudly and precisely, and cc.h says so at the top.
 */

int cc_run(const uint64_t *args, int nargs, uint64_t fuel, cc_result_t *res) {
    if (!res) return CC_EINVAL;
    res->ran = 0;
    res->value = 0;
    res->fuel_used = 0;
    res->stack_used = 0;
    res->ncalls = 0;
    res->out_len = 0;
    res->out[0] = '\0';

    if (!g_have_image) {
        res->rc = CC_EINVAL;
        return CC_EINVAL;
    }
    if (nargs < 0 || nargs > CC_MAX_PARAMS) {
        res->rc = CC_EINVAL;
        return CC_EINVAL;
    }
    if (nargs != g_entry_params) {
        res->rc = CC_EINVAL;
        return CC_EINVAL;
    }

    if (fuel == 0) fuel = CC_FUEL_DEFAULT;
    if (fuel > CC_FUEL_MAX) fuel = CC_FUEL_MAX;

    uint64_t a[CC_MAX_PARAMS];
    for (int i = 0; i < CC_MAX_PARAMS; i++) a[i] = (args && i < nargs) ? args[i] : 0;

    uint64_t top, limit;
    stack_window(&top, &limit);

    /* Poison the program stack so "how deep did it go" is measurable rather than
     * asserted, and so one run cannot read the previous run's locals. */
    memset(g_stack, 0xCD, sizeof g_stack);

    cc_rt.fuel        = (int64_t)fuel;
    cc_rt.stack_limit = limit;
    cc_rt.kernel_rsp  = 0;
    cc_rt.prog_rsp    = top;
    cc_rt.aborted     = 0;

    cc_rt_reset();

#if !defined(__x86_64__)
    /* Compiling for a non-x86-64 host: everything up to and including the emitted
     * bytes is exercised, but they cannot be RUN in this process. Say so rather
     * than pretend. Nothing above this point differs, which is why the
     * Makefile builds tests/host/test_cc.c as an x86-64 process wherever it can. */
    (void)a;
    res->rc = CC_ENOEXEC;
    return CC_ENOEXEC;
#else
    cc_entry_t entry = (cc_entry_t)(void *)code_buffer();
    uint64_t v = entry(a[0], a[1], a[2], a[3], a[4], a[5]);

    res->value     = v;
    res->fuel_used = fuel - (uint64_t)(cc_rt.fuel < 0 ? 0 : cc_rt.fuel);
    res->ncalls    = cc_rt_calls();
    res->out_len   = cc_rt_output(res->out, sizeof res->out);
    res->ran       = 1;

    /* Deepest byte touched: the first non-poison byte from the bottom. Only ever
     * a lower bound (a frame may legitimately write 0xCD), which is why it is
     * reported as a cost and never used as a bound. */
    {
        size_t i = 0;
        while (i < sizeof g_stack && g_stack[i] == 0xCD) i++;
        res->stack_used = (uint64_t)(sizeof g_stack - i);
    }

    if (cc_rt.aborted == CC_ABORT_FUEL) { res->rc = CC_EFUEL;  return CC_EFUEL; }
    if (cc_rt.aborted == CC_ABORT_STACK) { res->rc = CC_ESTACK; return CC_ESTACK; }
    res->rc = CC_OK;
    return CC_OK;
#endif
}

int cc_compile_run(const char *src, size_t len, const char *unit,
                   const uint64_t *args, int nargs, uint64_t fuel,
                   cc_result_t *res) {
    int rc = cc_compile(src, len, unit, res);
    if (rc != CC_OK) return rc;
    return cc_run(args, nargs, fuel, res);
}

/* ---- accessors the store and the tool layer need ---- */

int cc_entry_arity(void) { return g_have_image ? g_entry_params : -1; }
int cc_have_image(void)  { return g_have_image; }
size_t cc_image_len(void){ return g_code_len; }
const uint8_t *cc_image(void) { return code_buffer(); }
uint64_t cc_compile_serial(void) { return g_serial; }
