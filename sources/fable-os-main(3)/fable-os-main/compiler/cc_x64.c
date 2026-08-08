/* cc_x64.c — the typed tree to x86-64 machine code.
 *
 * A SYNTAX-DIRECTED STACK MACHINE, on purpose. Every value passes through rax;
 * a binary operator evaluates its right side, pushes it, evaluates its left
 * side, pops the right into rdi and applies one instruction. There is no
 * register allocator, no liveness analysis and no peephole pass. The code is
 * two to four times bigger than gcc's and that is the correct trade for a first
 * release of a compiler running inside a kernel: the emitter is small enough to
 * read in one sitting, and every construct compiles the same way every time,
 * which is what makes a miscompile findable.
 *
 * REGISTER DISCIPLINE, and it is short enough to hold in your head
 *     rax   the value being computed
 *     rdi   the popped right-hand operand, and the address for a store
 *     rcx   a shift count
 *     rdx   clobbered by div/idiv
 *     r10   free
 *     r11   an address or a wide immediate the emitter needed
 *     r15   THE RUNTIME BLOCK, for the whole of a run. Set once by the
 *           trampoline. Never loaded from memory, never written by generated
 *           code, and preserved across every call because it is callee-saved in
 *           the SysV ABI — which is what makes the fuel and stack checks
 *           unforgeable by the program being compiled.
 *     rbp   frame pointer; rsp the program's own stack.
 *
 * THE 64-BIT NORMALISATION INVARIANT, which the whole file depends on:
 * whenever a value is in rax, rax holds it sign-extended (or zero-extended, for
 * an unsigned or pointer type) to all 64 bits. Loads normalise, casts normalise,
 * and every arithmetic result is normalised immediately after the operation.
 * Two things fall out of it. `test rax, rax` is a correct truth test for any
 * type, so && || ! ?: if and while need no per-width variants. And arithmetic on
 * a 32-bit type is done with 32-bit instructions and then re-extended, so `int`
 * overflow wraps at 32 bits exactly as it does everywhere else, instead of
 * quietly becoming 64-bit arithmetic that gives a different answer.
 *
 * WHAT THE EMITTER WILL NOT EMIT, which is the security property (see cc.h):
 *   - No jump or call to a computed value. `call` is either rel32 to a function
 *     in this unit or `call r11` where r11 was loaded with a 64-bit immediate
 *     taken from the curated symbol table one instruction earlier. `switch` is a
 *     chain of compares, never a jump table, so no address is ever read from
 *     memory and jumped to.
 *   - No instruction that reads or writes r15.
 *   - No return path that skips the fuel and stack checks: they are in the
 *     prologue, before the first store, and at every loop back-edge.
 */

#include "cc_int.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The runtime block's field offsets are encoded as displacement bytes below, so
 * they are pinned here rather than trusted. */
_Static_assert(CC_RT_FUEL == 0 && CC_RT_STACKLIM == 8 && CC_RT_KRSP == 16 &&
               CC_RT_PRSP == 24 && CC_RT_ABORTED == 32,
               "cc_rt_t's layout is encoded in the instruction bytes below");
_Static_assert(sizeof(cc_rt_t) >= 40, "cc_rt_t must cover every encoded offset");

/* ===================================================================== */
/* the emitter                                                           */
/* ===================================================================== */

/* Labels and pending rel32 fields. Both are static arrays inside emit_t, so
 * these numbers are .bss: 2048 labels is 8 KiB and 4096 fixups is 32 KiB.
 * CC_CODE_MAX bytes of this emitter's output cannot contain more than about
 * 3000 jumps, so overflow here means the code buffer overflowed first. */
#define MAX_LABELS 2048
#define MAX_FIXUPS 4096

typedef struct {
    uint8_t *buf;
    size_t   cap, len;
    int      overflow;

    int   label_off[MAX_LABELS];
    int   nlabel;
    struct { int at; int label; } fix[MAX_FIXUPS];
    int   nfix;

    int   depth;              /* 8-byte values pushed since function entry */
    int   fn_label[CC_MAX_FUNCS];
    int   epilogue;           /* the current function's epilogue label     */
    int   abort_fuel, abort_stack;

    /* break / continue targets. Two stacks, because `break` binds to the
     * innermost loop OR switch and `continue` only to the innermost loop. */
    int   brk[CC_MAX_BLOCK_DEPTH * 2], nbrk;
    int   cont[CC_MAX_BLOCK_DEPTH * 2], ncont;
} emit_t;

static emit_t E;

static void e8(uint8_t b) {
    if (E.len >= E.cap) { E.overflow = 1; return; }
    E.buf[E.len++] = b;
}
static void ebytes(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) e8(b[i]); }
static void e32(uint32_t v) { for (int i = 0; i < 4; i++) e8((uint8_t)(v >> (8 * i))); }
static void e64(uint64_t v) { for (int i = 0; i < 8; i++) e8((uint8_t)(v >> (8 * i))); }

#define EMIT(...)                                                             \
    do { static const uint8_t b_[] = { __VA_ARGS__ };                          \
         ebytes(b_, sizeof b_); } while (0)

static int label_new(void) {
    if (E.nlabel >= MAX_LABELS) { E.overflow = 1; return 0; }
    E.label_off[E.nlabel] = -1;
    return E.nlabel++;
}
static void label_here(int l) {
    if (l >= 0 && l < E.nlabel) E.label_off[l] = (int)E.len;
}
/* Emit a rel32 field referring to `l`, to be filled in at the end. */
static void rel32(int l) {
    if (E.nfix >= MAX_FIXUPS) { E.overflow = 1; return; }
    E.fix[E.nfix].at = (int)E.len;
    E.fix[E.nfix].label = l;
    E.nfix++;
    e32(0);
}
static void jmp_to(int l)  { e8(0xE9); rel32(l); }
static void je_to(int l)   { EMIT(0x0F, 0x84); rel32(l); }
static void jne_to(int l)  { EMIT(0x0F, 0x85); rel32(l); }
static void js_to(int l)   { EMIT(0x0F, 0x88); rel32(l); }
static void jb_to(int l)   { EMIT(0x0F, 0x82); rel32(l); }

static void push_rax(void) { e8(0x50); E.depth++; }
static void pop_rdi(void)  { e8(0x5F); E.depth--; }

/* mov rax, imm64 */
static void mov_rax_imm(uint64_t v) { EMIT(0x48, 0xB8); e64(v); }
/* mov r11, imm64 */
static void mov_r11_imm(uint64_t v) { EMIT(0x49, 0xBB); e64(v); }

/* dec qword [r15]; js abort_fuel  — the whole termination guarantee, twice
 * per loop iteration at most and once per call. */
static void fuel_check(void) {
    EMIT(0x49, 0xFF, 0x0F);                 /* dec qword [r15]      */
    js_to(E.abort_fuel);
}
/* cmp rsp, [r15+8]; jb abort_stack */
static void stack_check(void) {
    EMIT(0x49, 0x3B, 0x67, 0x08);           /* cmp rsp, [r15+8]     */
    jb_to(E.abort_stack);
}

/* ===================================================================== */
/* loads, stores, casts                                                  */
/* ===================================================================== */

/* rax = *rax, normalised to 64 bits per `ty`. */
static void load(cc_type_t *ty) {
    if (ty->kind == TY_ARRAY || ty->kind == TY_STRUCT || ty->kind == TY_UNION)
        return;                              /* the address IS the value */
    switch (ty->size) {
    case 1: if (ty->is_unsigned) EMIT(0x48, 0x0F, 0xB6, 0x00);   /* movzx rax,byte[rax] */
            else                 EMIT(0x48, 0x0F, 0xBE, 0x00);   /* movsx rax,byte[rax] */
            break;
    case 2: if (ty->is_unsigned) EMIT(0x48, 0x0F, 0xB7, 0x00);
            else                 EMIT(0x48, 0x0F, 0xBF, 0x00);
            break;
    case 4: if (ty->is_unsigned) EMIT(0x8B, 0x00);               /* mov eax,[rax] */
            else                 EMIT(0x48, 0x63, 0x00);         /* movsxd rax,[rax] */
            break;
    default: EMIT(0x48, 0x8B, 0x00); break;                      /* mov rax,[rax] */
    }
}

/* *rdi = rax, `size` bytes. */
static void store(int size) {
    switch (size) {
    case 1: EMIT(0x88, 0x07); break;                 /* mov [rdi], al  */
    case 2: EMIT(0x66, 0x89, 0x07); break;           /* mov [rdi], ax  */
    case 4: EMIT(0x89, 0x07); break;                 /* mov [rdi], eax */
    default: EMIT(0x48, 0x89, 0x07); break;          /* mov [rdi], rax */
    }
}

/* Re-establish the normalisation invariant for a value of type `ty` in rax. */
static void normalise(cc_type_t *ty) {
    if (!ty) return;
    switch (ty->size) {
    case 1: if (ty->is_unsigned) EMIT(0x48, 0x0F, 0xB6, 0xC0);   /* movzx rax,al */
            else                 EMIT(0x48, 0x0F, 0xBE, 0xC0);   /* movsx rax,al */
            break;
    case 2: if (ty->is_unsigned) EMIT(0x48, 0x0F, 0xB7, 0xC0);
            else                 EMIT(0x48, 0x0F, 0xBF, 0xC0);
            break;
    case 4: if (ty->is_unsigned) EMIT(0x89, 0xC0);               /* mov eax,eax  */
            else                 EMIT(0x48, 0x63, 0xC0);         /* movsxd rax,eax */
            break;
    default: break;
    }
}

/* ===================================================================== */
/* code generation                                                       */
/* ===================================================================== */

static int gen_expr(cc_node_t *n);
static int gen_stmt(cc_node_t *n);

static int fail(const cc_token_t *tok, const char *what) {
    cc_errorf(tok, "%s", what);
    return 0;
}

static int gen_addr(cc_node_t *n) {
    if (!n) return 0;
    switch (n->kind) {
    case ND_VAR:
        if (n->sym->kind == SY_LOCAL) {
            EMIT(0x48, 0x8D, 0x85);                  /* lea rax, [rbp+disp32] */
            e32((uint32_t)(int32_t)n->sym->offset);
            return 1;
        }
        if (n->sym->kind == SY_GLOBAL) {
            mov_rax_imm((uint64_t)(uintptr_t)(cc_ctx.data + n->sym->offset));
            return 1;
        }
        return fail(n->tok, "this name has no address");
    case ND_STR:
        mov_rax_imm((uint64_t)(uintptr_t)(cc_ctx.data + n->data_off));
        return 1;
    case ND_DEREF:
        return gen_expr(n->lhs);
    case ND_MEMBER:
        if (!gen_addr(n->lhs)) return 0;
        if (n->member->offset) {
            EMIT(0x48, 0x05);                        /* add rax, imm32 */
            e32((uint32_t)n->member->offset);
        }
        return 1;
    case ND_COMMA:
        /* `(a, b) = v` is legal C; the address is b's. */
        if (!gen_expr(n->lhs)) return 0;
        return gen_addr(n->rhs);
    default:
        return fail(n->tok, "this value has no address");
    }
}

/* Copy `size` bytes from rax to rdi, unrolled. */
static void copy_bytes(int size) {
    int k = 0;
    while (size - k >= 8) {
        EMIT(0x4C, 0x8B, 0x58); e8((uint8_t)k);      /* mov r11, [rax+k]  */
        EMIT(0x4C, 0x89, 0x5F); e8((uint8_t)k);      /* mov [rdi+k], r11  */
        k += 8;
    }
    if (size - k >= 4) {
        EMIT(0x44, 0x8B, 0x58); e8((uint8_t)k);      /* mov r11d, [rax+k] */
        EMIT(0x44, 0x89, 0x5F); e8((uint8_t)k);
        k += 4;
    }
    if (size - k >= 2) {
        EMIT(0x66, 0x44, 0x8B, 0x58); e8((uint8_t)k);
        EMIT(0x66, 0x44, 0x89, 0x5F); e8((uint8_t)k);
        k += 2;
    }
    while (k < size) {
        EMIT(0x44, 0x0F, 0xB6, 0x58); e8((uint8_t)k); /* movzx r11d, byte[rax+k] */
        EMIT(0x44, 0x88, 0x5F); e8((uint8_t)k);       /* mov [rdi+k], r11b       */
        k++;
    }
}

/* Zero `size` bytes at rax. */
static void zero_bytes(int size) {
    if (size <= 64) {
        int k = 0;
        while (size - k >= 8) {
            EMIT(0x48, 0xC7, 0x40); e8((uint8_t)k); e32(0);   /* mov qword[rax+k],0 */
            k += 8;
        }
        if (size - k >= 4) { EMIT(0xC7, 0x40); e8((uint8_t)k); e32(0); k += 4; }
        if (size - k >= 2) { EMIT(0x66, 0xC7, 0x40); e8((uint8_t)k); e8(0); e8(0); k += 2; }
        while (k < size) { EMIT(0xC6, 0x40); e8((uint8_t)k); e8(0); k++; }
        return;
    }
    /* A byte loop, with a compile-time constant trip count. It carries no fuel
     * check on purpose: the compiler wrote it, its length is known here, and
     * charging the program for the compiler's own bookkeeping would make a
     * fuel budget depend on how a local happened to be declared. */
    int top = label_new();
    EMIT(0x48, 0xB9); e64((uint64_t)size);           /* mov rcx, size */
    label_here(top);
    EMIT(0xC6, 0x00, 0x00);                          /* mov byte [rax], 0 */
    EMIT(0x48, 0xFF, 0xC0);                          /* inc rax           */
    EMIT(0x48, 0xFF, 0xC9);                          /* dec rcx           */
    jne_to(top);
}

/* One binary integer operation on rax (left) and rdi (right), at the node's
 * width, followed by normalisation. */
static int gen_binop(cc_node_t *n) {
    cc_type_t *ty = n->type;
    int w64 = (ty->size == 8) || cc_is_pointerish(ty);
    int uns = ty->is_unsigned || cc_is_pointerish(ty);

    switch (n->kind) {
    case ND_ADD: if (w64) EMIT(0x48, 0x01, 0xF8); else EMIT(0x01, 0xF8); break;
    case ND_SUB: if (w64) EMIT(0x48, 0x29, 0xF8); else EMIT(0x29, 0xF8); break;
    /* IMUL r64, r/m64 is the one RM-form instruction here: its ModRM `reg` field
     * is the DESTINATION, the opposite of add/sub/and/or/xor/cmp above. 0xC7 is
     * therefore `imul rax, rdi`; 0xF8 would be `imul rdi, rax`, which computes
     * the same product and leaves it in the wrong register. */
    case ND_MUL: if (w64) EMIT(0x48, 0x0F, 0xAF, 0xC7); else EMIT(0x0F, 0xAF, 0xC7); break;
    case ND_DIV:
    case ND_MOD:
        if (uns) {
            EMIT(0x31, 0xD2);                             /* xor edx, edx */
            if (w64) EMIT(0x48, 0xF7, 0xF7); else EMIT(0xF7, 0xF7);   /* div rdi */
        } else {
            if (w64) EMIT(0x48, 0x99); else EMIT(0x99);    /* cqo / cdq   */
            if (w64) EMIT(0x48, 0xF7, 0xFF); else EMIT(0xF7, 0xFF);   /* idiv rdi */
        }
        if (n->kind == ND_MOD) {
            if (w64) EMIT(0x48, 0x89, 0xD0);              /* mov rax, rdx */
            else     EMIT(0x89, 0xD0);                    /* mov eax, edx */
        }
        break;
    case ND_BITAND: if (w64) EMIT(0x48, 0x21, 0xF8); else EMIT(0x21, 0xF8); break;
    case ND_BITOR:  if (w64) EMIT(0x48, 0x09, 0xF8); else EMIT(0x09, 0xF8); break;
    case ND_BITXOR: if (w64) EMIT(0x48, 0x31, 0xF8); else EMIT(0x31, 0xF8); break;
    case ND_SHL:
    case ND_SHR:
        EMIT(0x48, 0x89, 0xF9);                           /* mov rcx, rdi */
        if (n->kind == ND_SHL) { if (w64) EMIT(0x48, 0xD3, 0xE0); else EMIT(0xD3, 0xE0); }
        else if (uns)          { if (w64) EMIT(0x48, 0xD3, 0xE8); else EMIT(0xD3, 0xE8); }
        else                   { if (w64) EMIT(0x48, 0xD3, 0xF8); else EMIT(0xD3, 0xF8); }
        break;
    default:
        return fail(n->tok, "this operator is not implemented");
    }
    normalise(ty);
    return 1;
}

static int gen_compare(cc_node_t *n) {
    cc_type_t *ot = n->lhs->type;
    int w64 = (ot->size == 8) || cc_is_pointerish(ot);
    int uns = ot->is_unsigned || cc_is_pointerish(ot);

    if (w64) EMIT(0x48, 0x39, 0xF8); else EMIT(0x39, 0xF8);   /* cmp rax, rdi */
    switch (n->kind) {
    case ND_EQ: EMIT(0x0F, 0x94, 0xC0); break;                /* sete  al */
    case ND_NE: EMIT(0x0F, 0x95, 0xC0); break;                /* setne al */
    case ND_LT: if (uns) EMIT(0x0F, 0x92, 0xC0); else EMIT(0x0F, 0x9C, 0xC0); break;
    case ND_LE: if (uns) EMIT(0x0F, 0x96, 0xC0); else EMIT(0x0F, 0x9E, 0xC0); break;
    default: return fail(n->tok, "this comparison is not implemented");
    }
    EMIT(0x0F, 0xB6, 0xC0);                                   /* movzx eax, al */
    return 1;
}

/* The SysV argument registers, in order. */
static void pop_arg(int i) {
    switch (i) {
    case 0: e8(0x5F); break;                  /* pop rdi */
    case 1: e8(0x5E); break;                  /* pop rsi */
    case 2: e8(0x5A); break;                  /* pop rdx */
    case 3: e8(0x59); break;                  /* pop rcx */
    case 4: EMIT(0x41, 0x58); break;          /* pop r8  */
    default: EMIT(0x41, 0x59); break;         /* pop r9  */
    }
    E.depth--;
}
static void mov_rax_from_arg(int i) {
    switch (i) {
    case 0: EMIT(0x48, 0x89, 0xF8); break;    /* mov rax, rdi */
    case 1: EMIT(0x48, 0x89, 0xF0); break;    /* mov rax, rsi */
    case 2: EMIT(0x48, 0x89, 0xD0); break;    /* mov rax, rdx */
    case 3: EMIT(0x48, 0x89, 0xC8); break;    /* mov rax, rcx */
    case 4: EMIT(0x4C, 0x89, 0xC0); break;    /* mov rax, r8  */
    default: EMIT(0x4C, 0x89, 0xC8); break;   /* mov rax, r9  */
    }
}

static int gen_call(cc_node_t *n) {
    int i = 0;
    for (cc_node_t *a = n->args; a; a = a->next) {
        if (!gen_expr(a)) return 0;
        push_rax();
        i++;
    }
    for (int k = i - 1; k >= 0; k--) pop_arg(k);

    /* SysV wants rsp 16-aligned at the call. E.depth counts the 8-byte values
     * this frame has pushed, and the prologue leaves rsp 16-aligned, so an odd
     * depth is exactly the case that needs a pad. */
    int pad = (E.depth & 1);
    if (pad) EMIT(0x48, 0x83, 0xEC, 0x08);               /* sub rsp, 8 */

    fuel_check();

    if (n->callee->kind == SY_FUNC) {
        if (n->callee->func_index < 0 || n->callee->func_index >= cc_ctx.nfunc)
            return fail(n->tok, "this function has no body in this unit");
        e8(0xE8);                                        /* call rel32 */
        rel32(E.fn_label[n->callee->func_index]);
    } else {
        /* A curated kernel entry point. The target is a 64-bit IMMEDIATE from the
         * symbol table, loaded one instruction before the call: no value the
         * program computed ever reaches a control transfer. */
        mov_r11_imm((uint64_t)(uintptr_t)n->callee->addr);
        EMIT(0x41, 0xFF, 0xD3);                          /* call r11 */
    }
    if (pad) EMIT(0x48, 0x83, 0xC4, 0x08);               /* add rsp, 8 */

    /* The callee returned in rax with whatever width its C type has; re-establish
     * the invariant for the call's own type. */
    if (n->type && n->type->kind != TY_VOID) normalise(n->type);
    return 1;
}

static int gen_expr(cc_node_t *n) {
    if (!n) return 0;
    if (E.overflow) return 0;
    /* THE ONE BOUND THE PARSER CANNOT PROVIDE. cc_parse.c's DEEPER() bounds
     * NESTING; this walk's depth is the number of TERMS. `1+1+1+...` parses at
     * constant depth through an iterative precedence loop (cc_parse.c
     * add_expr()) and builds an N-deep left-leaning tree, so ~2 KB of legal C
     * used to recurse ~1200 frames deep on the root stack and take the identity
     * map with it. E.overflow above is the CODE BUFFER's flag and has nothing to
     * do with this. See "THE COMPILER'S OWN STACK" in include/cc.h. */
    if (cc_stack_exhausted(n->tok)) return 0;

    switch (n->kind) {
    case ND_NUM:
        mov_rax_imm(n->val);
        return 1;

    case ND_VAR:
        if (n->sym->kind == SY_ENUMCONST) { mov_rax_imm((uint64_t)n->sym->val); return 1; }
        if (!gen_addr(n)) return 0;
        load(n->type);
        return 1;

    case ND_STR:
        return gen_addr(n);

    case ND_ADDR:
        return gen_addr(n->lhs);

    case ND_DEREF:
    case ND_MEMBER:
        if (!gen_addr(n)) return 0;
        load(n->type);
        return 1;

    case ND_ASSIGN:
        if (!gen_addr(n->lhs)) return 0;
        push_rax();
        if (!gen_expr(n->rhs)) return 0;
        pop_rdi();
        store(n->type->size);
        return 1;

    case ND_COPY: {
        if (n->type->size > 128)
            return fail(n->tok, "this struct is larger than 128 bytes, which is "
                                "more than assignment will copy; use memcpy");
        if (!gen_addr(n->lhs)) return 0;
        push_rax();
        if (!gen_addr(n->rhs)) return 0;
        pop_rdi();
        copy_bytes(n->type->size);
        EMIT(0x48, 0x89, 0xF8);                       /* mov rax, rdi (the dest) */
        return 1;
    }

    case ND_CAST:
        if (!gen_expr(n->lhs)) return 0;
        if (n->type->kind == TY_VOID) return 1;
        normalise(n->type);
        return 1;

    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR: case ND_SHL: case ND_SHR:
        if (!gen_expr(n->rhs)) return 0;
        push_rax();
        if (!gen_expr(n->lhs)) return 0;
        pop_rdi();
        return gen_binop(n);

    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        if (!gen_expr(n->rhs)) return 0;
        push_rax();
        if (!gen_expr(n->lhs)) return 0;
        pop_rdi();
        return gen_compare(n);

    case ND_NOT:
        if (!gen_expr(n->lhs)) return 0;
        EMIT(0x48, 0x85, 0xC0);                       /* test rax, rax */
        EMIT(0x0F, 0x94, 0xC0);                       /* sete al       */
        EMIT(0x0F, 0xB6, 0xC0);                       /* movzx eax, al */
        return 1;

    case ND_BITNOT:
        if (!gen_expr(n->lhs)) return 0;
        if (n->type->size == 8) EMIT(0x48, 0xF7, 0xD0); else EMIT(0xF7, 0xD0);
        normalise(n->type);
        return 1;

    case ND_LOGAND: {
        int lfalse = label_new(), lend = label_new();
        if (!gen_expr(n->lhs)) return 0;
        EMIT(0x48, 0x85, 0xC0);
        je_to(lfalse);
        if (!gen_expr(n->rhs)) return 0;
        EMIT(0x48, 0x85, 0xC0);
        je_to(lfalse);
        EMIT(0xB8); e32(1);                           /* mov eax, 1 */
        jmp_to(lend);
        label_here(lfalse);
        EMIT(0x31, 0xC0);                             /* xor eax, eax */
        label_here(lend);
        return 1;
    }
    case ND_LOGOR: {
        int ltrue = label_new(), lend = label_new();
        if (!gen_expr(n->lhs)) return 0;
        EMIT(0x48, 0x85, 0xC0);
        jne_to(ltrue);
        if (!gen_expr(n->rhs)) return 0;
        EMIT(0x48, 0x85, 0xC0);
        jne_to(ltrue);
        EMIT(0x31, 0xC0);
        jmp_to(lend);
        label_here(ltrue);
        EMIT(0xB8); e32(1);
        label_here(lend);
        return 1;
    }
    case ND_COND: {
        int lelse = label_new(), lend = label_new();
        if (!gen_expr(n->cond)) return 0;
        EMIT(0x48, 0x85, 0xC0);
        je_to(lelse);
        if (!gen_expr(n->then)) return 0;
        jmp_to(lend);
        label_here(lelse);
        if (!gen_expr(n->els)) return 0;
        label_here(lend);
        return 1;
    }
    case ND_COMMA:
        if (!gen_expr(n->lhs)) return 0;
        return gen_expr(n->rhs);

    case ND_CALL:
        return gen_call(n);

    default:
        return fail(n->tok, "this expression is not implemented");
    }
}

static int gen_stmt(cc_node_t *n) {
    /* Statements recurse too (a block inside an if inside a loop), and this
     * frame is the biggest in the emitter. Same floor, same reason. */
    if (n && cc_stack_exhausted(n->tok)) return 0;

    if (!n) return 1;
    if (E.overflow) return 0;

    switch (n->kind) {
    case ND_NOP:
        return 1;

    case ND_BLOCK:
        for (cc_node_t *s = n->body; s; s = s->next)
            if (!gen_stmt(s)) return 0;
        return 1;

    case ND_EXPR_STMT:
        return gen_expr(n->lhs);

    case ND_MEMZERO: {
        cc_node_t v;
        memset(&v, 0, sizeof v);
        v.kind = ND_VAR;
        v.sym  = n->sym;
        v.type = n->type;
        v.tok  = n->tok;
        if (!gen_addr(&v)) return 0;
        zero_bytes(n->type->size);
        return 1;
    }

    case ND_RETURN:
        if (n->lhs && !gen_expr(n->lhs)) return 0;
        jmp_to(E.epilogue);
        return 1;

    case ND_IF: {
        int lelse = label_new(), lend = label_new();
        if (!gen_expr(n->cond)) return 0;
        EMIT(0x48, 0x85, 0xC0);
        je_to(n->els ? lelse : lend);
        if (!gen_stmt(n->then)) return 0;
        if (n->els) {
            jmp_to(lend);
            label_here(lelse);
            if (!gen_stmt(n->els)) return 0;
        }
        label_here(lend);
        return 1;
    }

    case ND_LOOP: {
        int ltop = label_new(), lcont = label_new(), lend = label_new();
        if (E.nbrk >= (int)(sizeof E.brk / sizeof E.brk[0]) ||
            E.ncont >= (int)(sizeof E.cont / sizeof E.cont[0]))
            return fail(n->tok, "loops are nested too deeply");
        E.brk[E.nbrk++] = lend;
        E.cont[E.ncont++] = lcont;

        if (n->init && !gen_stmt(n->init)) return 0;
        label_here(ltop);
        if (n->test_first && n->cond) {
            if (!gen_expr(n->cond)) return 0;
            EMIT(0x48, 0x85, 0xC0);
            je_to(lend);
        }
        if (!gen_stmt(n->body)) return 0;
        label_here(lcont);
        if (n->post && !gen_stmt(n->post)) return 0;
        if (!n->test_first && n->cond) {
            if (!gen_expr(n->cond)) return 0;
            EMIT(0x48, 0x85, 0xC0);
            je_to(lend);
        }
        /* THE BACK-EDGE. Every path that repeats the body passes through here,
         * which is what makes "a compiled program terminates" true rather than
         * hoped for. */
        fuel_check();
        jmp_to(ltop);
        label_here(lend);

        E.nbrk--;
        E.ncont--;
        return 1;
    }

    case ND_BREAK:
        if (E.nbrk == 0) return fail(n->tok, "'break' has nothing to leave");
        jmp_to(E.brk[E.nbrk - 1]);
        return 1;

    case ND_CONTINUE:
        if (E.ncont == 0) return fail(n->tok, "'continue' has no loop");
        jmp_to(E.cont[E.ncont - 1]);
        return 1;

    case ND_CASE:
        /* -1 means the switch's case walk never found this label. Placing label 0
         * instead would silently move a FUNCTION's entry point, so it is refused
         * rather than emitted. */
        if (n->label < 0)
            return fail(n->tok, "this 'case' is buried somewhere its switch could "
                                "not find it; put it directly in the switch body "
                                "or in a plain block");
        label_here(n->label);
        return gen_stmt(n->body);

    case ND_SWITCH: {
        int lend = label_new();
        if (E.nbrk >= (int)(sizeof E.brk / sizeof E.brk[0]))
            return fail(n->tok, "switches are nested too deeply");

        for (cc_node_t *c = n->cases; c; c = c->cases) c->label = label_new();
        if (n->default_case) n->default_case->label = label_new();

        if (!gen_expr(n->cond)) return 0;
        /* A compare chain, deliberately: a jump table would mean reading an
         * address out of memory and jumping to it, which is the one thing the
         * emitter must never do (see the header comment). */
        for (cc_node_t *c = n->cases; c; c = c->cases) {
            mov_r11_imm(c->case_val);
            EMIT(0x49, 0x3B, 0xC3);                   /* cmp rax, r11 */
            je_to(c->label);
        }
        jmp_to(n->default_case ? n->default_case->label : lend);

        E.brk[E.nbrk++] = lend;
        if (!gen_stmt(n->body)) return 0;
        E.nbrk--;
        label_here(lend);
        return 1;
    }

    default:
        /* An expression used as a statement that the parser wrapped differently. */
        return gen_expr(n);
    }
}

/* ===================================================================== */
/* function bodies, the trampoline, and the abort stubs                  */
/* ===================================================================== */

static int gen_func(cc_func_t *f, int index) {
    label_here(E.fn_label[index]);
    f->code_off = (int)E.len;
    E.depth = 0;
    E.epilogue = label_new();
    E.nbrk = E.ncont = 0;

    e8(0x55);                                        /* push rbp        */
    EMIT(0x48, 0x89, 0xE5);                          /* mov rbp, rsp    */
    int frame = f->frame_bytes;
    if (frame > 0) { EMIT(0x48, 0x81, 0xEC); e32((uint32_t)frame); }

    /* The two checks, before the first store this frame makes. Order matters:
     * fuel first (it is the cheaper one and the common exhaustion), then the
     * stack limit, which is only meaningful once rsp has moved. */
    fuel_check();
    stack_check();

    /* Incoming parameters into their frame slots. Ascending order is required,
     * not stylistic: rdi is both argument 0 and the address register the store
     * uses, so argument 0 must be consumed before rdi is reused. */
    for (int i = 0; i < f->nparam; i++) {
        cc_sym_t *p = f->param[i];
        mov_rax_from_arg(i);
        EMIT(0x48, 0x8D, 0xBD);                      /* lea rdi, [rbp+disp32] */
        e32((uint32_t)(int32_t)p->offset);
        store(p->type->size);
    }

    if (!gen_stmt(f->body)) return 0;

    /* Falling off the end of a non-void function is undefined in C. Here it
     * returns 0, deterministically, because "undefined" in a kernel with no
     * memory protection is a worse answer than "zero". */
    EMIT(0x31, 0xC0);                                /* xor eax, eax */
    label_here(E.epilogue);
    EMIT(0x48, 0x89, 0xEC);                          /* mov rsp, rbp */
    e8(0x5D);                                        /* pop rbp      */
    e8(0xC3);                                        /* ret          */
    return 1;
}

int cc_codegen(uint8_t *code, size_t cap, size_t *out_len) {
    memset(&E, 0, sizeof E);
    E.buf = code;
    E.cap = cap;
    E.len = 0;

    int entry = -1;
    for (int i = 0; i < cc_ctx.nfunc; i++) {
        E.fn_label[i] = label_new();
        if (strcmp(cc_ctx.func[i].name, "main") == 0) entry = i;
    }
    if (entry < 0)
        return cc_errorf(NULL, "there is no main() to enter");

    E.abort_fuel  = label_new();
    E.abort_stack = label_new();
    int ldone = label_new();

    /* ---- the entry trampoline, at offset 0 ----
     * It establishes r15, saves the kernel's rsp for the abort path, and switches
     * to the program's own stack. Written in emitted bytes rather than inline
     * assembly so that exactly one language in this module knows about x86. */
    e8(0x55);                                        /* push rbp     */
    EMIT(0x41, 0x57);                                /* push r15     */
    e8(0x53);                                        /* push rbx     */
    EMIT(0x49, 0xBF); e64((uint64_t)(uintptr_t)&cc_rt);   /* mov r15, &cc_rt */
    EMIT(0x49, 0x89, 0x67, 0x10);                    /* mov [r15+16], rsp */
    EMIT(0x49, 0x8B, 0x67, 0x18);                    /* mov rsp, [r15+24] */
    e8(0xE8); rel32(E.fn_label[entry]);              /* call main    */
    label_here(ldone);
    EMIT(0x49, 0x8B, 0x67, 0x10);                    /* mov rsp, [r15+16] */
    e8(0x5B);                                        /* pop rbx      */
    EMIT(0x41, 0x5F);                                /* pop r15      */
    e8(0x5D);                                        /* pop rbp      */
    e8(0xC3);                                        /* ret          */

    for (int i = 0; i < cc_ctx.nfunc; i++)
        if (!gen_func(&cc_ctx.func[i], i)) goto over;

    /* ---- the abort stubs: a hand-built longjmp ---- */
    label_here(E.abort_fuel);
    EMIT(0x41, 0xC7, 0x47, 0x20); e32(CC_ABORT_FUEL);    /* mov [r15+32], 1 */
    EMIT(0x31, 0xC0);                                    /* xor eax, eax    */
    jmp_to(ldone);
    label_here(E.abort_stack);
    EMIT(0x41, 0xC7, 0x47, 0x20); e32(CC_ABORT_STACK);
    EMIT(0x31, 0xC0);
    jmp_to(ldone);

    if (E.overflow) goto over;

    /* ---- resolve every rel32 ---- */
    for (int i = 0; i < E.nfix; i++) {
        int l = E.fix[i].label;
        if (l < 0 || l >= E.nlabel || E.label_off[l] < 0)
            return cc_errorf(NULL, "internal: a jump target was never placed "
                                   "(this is a compiler bug)");
        int32_t rel = (int32_t)(E.label_off[l] - (E.fix[i].at + 4));
        uint8_t *p = code + E.fix[i].at;
        p[0] = (uint8_t)(uint32_t)rel;
        p[1] = (uint8_t)((uint32_t)rel >> 8);
        p[2] = (uint8_t)((uint32_t)rel >> 16);
        p[3] = (uint8_t)((uint32_t)rel >> 24);
    }

    *out_len = E.len;
    return 1;

over:
    if (E.overflow)
        return cc_errorf(NULL,
                         "this unit needs more than %d bytes of machine code; "
                         "split it into saved programs that call each other",
                         CC_CODE_MAX);
    return 0;
}
