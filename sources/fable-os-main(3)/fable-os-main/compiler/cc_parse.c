/* cc_parse.c — tokens to a typed tree, and every refusal in the subset.
 *
 * THIS FILE IS THE SUBSET. cc.h describes it in prose; the diagnostics below are
 * the enforcement, and if the two ever disagree this file is the truth. A
 * recursive-descent parser with types attached as the tree is built, so an
 * expression's type is known at the moment it is formed and a mistake is
 * reported at the token that made it rather than three productions later.
 *
 * THREE DECISIONS WORTH KNOWING BEFORE READING
 *
 *   1. CONVERSIONS ARE EXPLICIT IN THE TREE. Where C says "usual arithmetic
 *      conversions", this parser inserts real ND_CAST nodes, so both operands of
 *      a binary node have exactly the node's own type. The code generator then
 *      never has to reason about mixed widths or mixed signedness — it reads
 *      node->type->size and node->type->is_unsigned and picks an instruction.
 *      Every signedness bug this design removes is one that would otherwise show
 *      up as a wrong answer rather than as a crash.
 *
 *   2. POINTER MISMATCHES ARE ERRORS, NOT WARNINGS. `char *p = some_int;` and
 *      `int *q = char_ptr;` are refused with both type names in the message.
 *      Real C warns and carries on; there is no build log here for a warning to
 *      sit in, the caller is a language model, and a diagnostic it can act on is
 *      worth more than a program that runs and is wrong. A cast still works —
 *      the point is that it has to be written down.
 *
 *   3. A FUNCTION NAME IS NOT A VALUE. There is no array of function pointers,
 *      no callback, and no way to form a call whose target is not a name this
 *      parser resolved. That is not an omission for want of time: it is the
 *      property that bounds where compiled code can transfer control (cc.h, THE
 *      SYMBOL TABLE), and it is enforced in exactly one place — primary(), where
 *      an identifier naming a function is only legal immediately before '('.
 *
 * DIAGNOSTIC STYLE. The message says what is wrong and, where there is an
 * obvious repair, what to write instead: `.` on a pointer says "use ->", an
 * unknown member lists the members there are, an unknown identifier before '('
 * names the curated table. The reader is a model with one turn to fix it.
 */

#include "cc_int.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================== */
/* base types                                                            */
/* ===================================================================== */

/* The base types, as static singletons. Designated initialisers, so that adding
 * a field to cc_type_t cannot silently change what `int` means here. */
#define BASETY(k, u, sz) { .kind = (k), .is_unsigned = (u), .size = (sz), .align = (sz) }
static cc_type_t ty_void_  = BASETY(TY_VOID,  0, 1);
static cc_type_t ty_char_  = BASETY(TY_CHAR,  0, 1);
static cc_type_t ty_uchar_ = BASETY(TY_CHAR,  1, 1);
static cc_type_t ty_short_ = BASETY(TY_SHORT, 0, 2);
static cc_type_t ty_ushort_= BASETY(TY_SHORT, 1, 2);
static cc_type_t ty_int_   = BASETY(TY_INT,   0, 4);
static cc_type_t ty_uint_  = BASETY(TY_INT,   1, 4);
static cc_type_t ty_long_  = BASETY(TY_LONG,  0, 8);
static cc_type_t ty_ulong_ = BASETY(TY_LONG,  1, 8);

cc_type_t *cc_ty_void = &ty_void_,  *cc_ty_char  = &ty_char_,  *cc_ty_uchar = &ty_uchar_;
cc_type_t *cc_ty_short= &ty_short_, *cc_ty_ushort= &ty_ushort_;
cc_type_t *cc_ty_int  = &ty_int_,   *cc_ty_uint  = &ty_uint_;
cc_type_t *cc_ty_long = &ty_long_,  *cc_ty_ulong = &ty_ulong_;

int cc_is_integer(const cc_type_t *t) {
    return t && (t->kind == TY_CHAR || t->kind == TY_SHORT ||
                 t->kind == TY_INT  || t->kind == TY_LONG);
}
int cc_is_pointerish(const cc_type_t *t) {
    return t && (t->kind == TY_PTR || t->kind == TY_ARRAY);
}
static int is_aggregate(const cc_type_t *t) {
    return t && (t->kind == TY_STRUCT || t->kind == TY_UNION);
}

cc_type_t *cc_pointer_to(cc_type_t *base) {
    cc_type_t *t = cc_alloc(sizeof *t);
    if (!t) return NULL;
    t->kind = TY_PTR;
    t->size = 8;
    t->align = 8;
    t->is_unsigned = 1;          /* pointers compare unsigned */
    t->base = base;
    return t;
}

cc_type_t *cc_array_of(cc_type_t *base, int len) {
    cc_type_t *t = cc_alloc(sizeof *t);
    if (!t) return NULL;
    t->kind = TY_ARRAY;
    t->base = base;
    t->array_len = len;
    t->align = base->align;
    t->size = (len < 0) ? 0 : base->size * len;
    return t;
}

const char *cc_type_name(const cc_type_t *t, char *dst, size_t cap) {
    if (!cap) return dst;
    dst[0] = '\0';
    if (!t) { snprintf(dst, cap, "?"); return dst; }
    switch (t->kind) {
    case TY_VOID:  snprintf(dst, cap, "void"); break;
    case TY_CHAR:  snprintf(dst, cap, t->is_unsigned ? "unsigned char" : "char"); break;
    case TY_SHORT: snprintf(dst, cap, t->is_unsigned ? "unsigned short" : "short"); break;
    case TY_INT:   snprintf(dst, cap, t->is_unsigned ? "unsigned int" : "int"); break;
    case TY_LONG:  snprintf(dst, cap, t->is_unsigned ? "unsigned long" : "long"); break;
    case TY_PTR: {
        char inner[48];
        cc_type_name(t->base, inner, sizeof inner);
        snprintf(dst, cap, "%s *", inner);
        break;
    }
    case TY_ARRAY: {
        char inner[48];
        cc_type_name(t->base, inner, sizeof inner);
        if (t->array_len < 0) snprintf(dst, cap, "%s []", inner);
        else snprintf(dst, cap, "%s [%d]", inner, t->array_len);
        break;
    }
    case TY_STRUCT: snprintf(dst, cap, "struct %s", t->tag ? t->tag : "(anonymous)"); break;
    case TY_UNION:  snprintf(dst, cap, "union %s",  t->tag ? t->tag : "(anonymous)"); break;
    case TY_FUNC: {
        char inner[48];
        cc_type_name(t->base, inner, sizeof inner);
        snprintf(dst, cap, "%s (...)", inner);
        break;
    }
    default: snprintf(dst, cap, "?"); break;
    }
    return dst;
}

/* Two scratch buffers so a message can name both sides of a mismatch. */
static char g_tn1[64], g_tn2[64];
#define TN1(t) cc_type_name((t), g_tn1, sizeof g_tn1)
#define TN2(t) cc_type_name((t), g_tn2, sizeof g_tn2)

/* ===================================================================== */
/* the token cursor                                                      */
/* ===================================================================== */

static cc_token_t *tk(void) { return &cc_ctx.tok[cc_ctx.pos]; }
static cc_token_t *tk_at(int n) {
    int i = cc_ctx.pos + n;
    if (i >= cc_ctx.ntok) i = cc_ctx.ntok - 1;
    return &cc_ctx.tok[i];
}
static void bump(void) { if (cc_ctx.pos < cc_ctx.ntok - 1) cc_ctx.pos++; }

static int eq(const cc_token_t *t, const char *s) {
    size_t n = strlen(s);
    return t->len == n && memcmp(t->text, s, n) == 0 &&
           (t->kind == TK_PUNCT || t->kind == TK_KEYWORD || t->kind == TK_IDENT);
}
static int at(const char *s) { return eq(tk(), s); }
static int consume(const char *s) { if (at(s)) { bump(); return 1; } return 0; }

/* A token's spelling for a message: never the raw bytes of a string literal. */
static const char *spell(const cc_token_t *t) {
    if (t->kind == TK_EOF) return "end of file";
    if (t->kind == TK_STR) return "a string literal";
    if (t->kind == TK_CHAR) return "a character literal";
    return t->text;
}

static int expect(const char *s) {
    if (consume(s)) return 1;
    return cc_errorf(tk(), "expected '%s' here, found '%s'", s, spell(tk()));
}

/* ===================================================================== */
/* scopes and symbols                                                    */
/* ===================================================================== */

typedef struct tag_scope {
    const char *name;
    cc_type_t  *type;
    int         depth;
    struct tag_scope *next;
} tag_scope_t;

static tag_scope_t *g_tags;

static void enter_scope(void) { cc_ctx.scope_depth++; }

static void leave_scope(void) {
    while (cc_ctx.scope && cc_ctx.scope->scope_depth >= cc_ctx.scope_depth)
        cc_ctx.scope = cc_ctx.scope->next;
    while (g_tags && g_tags->depth >= cc_ctx.scope_depth)
        g_tags = g_tags->next;
    cc_ctx.scope_depth--;
}

static cc_sym_t *find_sym(const char *name) {
    for (cc_sym_t *s = cc_ctx.scope; s; s = s->next)
        if (strcmp(s->name, name) == 0) return s;
    return NULL;
}
static cc_sym_t *find_sym_here(const char *name) {
    for (cc_sym_t *s = cc_ctx.scope; s && s->scope_depth >= cc_ctx.scope_depth; s = s->next)
        if (strcmp(s->name, name) == 0) return s;
    return NULL;
}

static cc_sym_t *push_sym(int kind, const char *name, cc_type_t *type) {
    cc_sym_t *s = cc_alloc(sizeof *s);
    if (!s) return NULL;
    s->kind = (uint8_t)kind;
    s->name = name;
    s->type = type;
    s->scope_depth = cc_ctx.scope_depth;
    s->next = cc_ctx.scope;
    cc_ctx.scope = s;
    return s;
}

static cc_type_t *find_tag(const char *name) {
    for (tag_scope_t *t = g_tags; t; t = t->next)
        if (strcmp(t->name, name) == 0) return t->type;
    return NULL;
}
static int push_tag(const char *name, cc_type_t *ty) {
    tag_scope_t *t = cc_alloc(sizeof *t);
    if (!t) return 0;
    t->name = name;
    t->type = ty;
    t->depth = cc_ctx.scope_depth;
    t->next = g_tags;
    g_tags = t;
    return 1;
}

/* ---- the data image ---- */

static int data_alloc(int size, int align, const cc_token_t *tok) {
    int off = (cc_ctx.data_len + align - 1) / align * align;
    if (size < 0 || off > CC_DATA_MAX - size) {
        cc_errorf(tok, "this unit needs more than %d bytes of globals and string "
                       "literals", CC_DATA_MAX);
        return -1;
    }
    cc_ctx.data_len = off + size;
    return off;
}

/* ===================================================================== */
/* node constructors                                                     */
/* ===================================================================== */

static cc_node_t *node(int kind, const cc_token_t *tok) {
    cc_node_t *n = cc_alloc(sizeof *n);
    if (!n) return NULL;
    n->kind = (uint8_t)kind;
    n->tok = tok;
    return n;
}
static cc_node_t *node_num(uint64_t v, cc_type_t *ty, const cc_token_t *tok) {
    cc_node_t *n = node(ND_NUM, tok);
    if (!n) return NULL;
    n->val = v;
    n->type = ty;
    return n;
}
static cc_node_t *node_bin(int kind, cc_node_t *l, cc_node_t *r, const cc_token_t *tok) {
    if (!l || !r) return NULL;
    cc_node_t *n = node(kind, tok);
    if (!n) return NULL;
    n->lhs = l;
    n->rhs = r;
    return n;
}
static cc_node_t *node_un(int kind, cc_node_t *l, const cc_token_t *tok) {
    if (!l) return NULL;
    cc_node_t *n = node(kind, tok);
    if (!n) return NULL;
    n->lhs = l;
    return n;
}
static cc_node_t *node_cast(cc_node_t *e, cc_type_t *ty, const cc_token_t *tok) {
    if (!e) return NULL;
    if (e->type == ty) return e;
    cc_node_t *n = node(ND_CAST, tok);
    if (!n) return NULL;
    n->lhs = e;
    n->type = ty;
    return n;
}

/* Array-to-pointer decay. Everything that wants a VALUE goes through this. */
static cc_node_t *decay(cc_node_t *e) {
    if (!e || !e->type) return e;
    if (e->type->kind != TY_ARRAY) return e;
    cc_node_t *a = node(ND_ADDR, e->tok);
    if (!a) return NULL;
    a->lhs = e;
    a->type = cc_pointer_to(e->type->base);
    return a;
}

static cc_type_t *common_type(cc_type_t *a, cc_type_t *b) {
    /* THE INTEGER PROMOTIONS COME FIRST, AND THEY GO TO int, NOT unsigned int.
     *
     * C11 6.3.1.1p2: a type of rank below int promotes to int whenever int can
     * represent all its values — and a 32-bit int represents every unsigned char
     * and every unsigned short. So both narrow cases promote to cc_ty_int,
     * unconditionally, BEFORE the usual arithmetic conversions below.
     *
     * This used to read `a->is_unsigned ? cc_ty_uint : cc_ty_int`, which made
     * every expression with a uint8_t or uint16_t operand unsigned and turned
     * negative intermediates into ~4.29e9. It was a SILENT wrong answer in the
     * exact types register and driver code is made of, with rc = CC_OK and no
     * diagnostic:
     *     int x = -6; unsigned char u = 2;  x / u   gave 2147483645, not -3
     *     uint8_t a=10, b=20;              (a-b)<0  gave 0, not 1
     * uint8_t and uint16_t are predeclared here (cc_sym.c) and advertised in the
     * built-in <fableos.h>, so a model reached this immediately. */
    if (a->size < 4) a = cc_ty_int;
    if (b->size < 4) b = cc_ty_int;
    if (a->size != b->size) return (a->size > b->size) ? a : b;
    if (b->is_unsigned) return b;
    return a;
}

/* ===================================================================== */
/* forward declarations                                                  */
/* ===================================================================== */

static cc_node_t *expr(void);
static cc_node_t *assign_expr(void);
static cc_node_t *conditional(void);
static cc_node_t *unary(void);
static cc_node_t *cast_expr(void);
static cc_node_t *statement(void);
static cc_node_t *compound(void);
static cc_type_t *declspec(int *is_static, int *is_extern, int *is_typedef);
static cc_type_t *declarator(cc_type_t *base, const cc_token_t **name);
static cc_type_t *abstract_declarator(cc_type_t *base);
static int        is_typename(const cc_token_t *t);
static int        const_expr(int64_t *out);
static cc_node_t *new_add(cc_node_t *l, cc_node_t *r, const cc_token_t *tok);
static cc_node_t *new_sub(cc_node_t *l, cc_node_t *r, const cc_token_t *tok);

/* Recursion guard: model output includes `((((((...` and 500 nested parens must
 * be a diagnostic, not a blown kernel stack. Every recursive production passes
 * through DEEPER/SHALLOWER. */
/* AND A STACK FLOOR, because a depth counter alone was not enough. g_depth
 * bounds how deep the PARSER goes; it says nothing about how much stack each
 * level costs, and the answer changed under this bound twice (a 16 KiB string
 * buffer in string_node(), a 1 KiB walk stack in statement()) without the
 * constant moving. cc_stack_exhausted() measures the frame pointer instead of
 * trusting arithmetic nobody re-does. See include/cc.h. */
static int g_depth;
#define DEEPER(retval)                                                        \
    do {                                                                      \
        if (cc_stack_exhausted(tk())) return retval;                          \
        if (++g_depth > CC_MAX_BLOCK_DEPTH) {                                 \
            cc_errorf(tk(), "this expression or block is nested more than %d " \
                            "deep", CC_MAX_BLOCK_DEPTH);                      \
            g_depth--;                                                        \
            return retval;                                                    \
        }                                                                     \
    } while (0)
#define SHALLOWER() (g_depth--)

/* ===================================================================== */
/* lvalues, assignability                                                */
/* ===================================================================== */

static int is_lvalue(const cc_node_t *n) {
    if (!n) return 0;
    switch (n->kind) {
    case ND_VAR:    return n->sym && n->sym->kind != SY_FUNC && n->sym->kind != SY_ENUMCONST;
    case ND_DEREF:  return 1;
    case ND_MEMBER: return 1;
    default:        return 0;
    }
}

/* Can `src` be stored into `dst` without a cast? The rule is deliberately
 * stricter than C's; see the header comment. */
static int assignable(cc_type_t *dst, cc_node_t *src, const char **why) {
    *why = NULL;
    if (!dst || !src || !src->type) return 0;
    cc_type_t *s = src->type;

    if (dst->kind == TY_VOID) { *why = "a void object cannot hold anything"; return 0; }
    if (is_aggregate(dst) || is_aggregate(s)) {
        if (dst->kind == s->kind && dst->tag && s->tag && strcmp(dst->tag, s->tag) == 0)
            return 1;
        *why = "structs and unions can only be assigned from the same type";
        return 0;
    }
    if (cc_is_integer(dst) && cc_is_integer(s)) return 1;
    if (dst->kind == TY_PTR && s->kind == TY_PTR) {
        /* void * converts both ways; anything else must match exactly. */
        if (dst->base->kind == TY_VOID || s->base->kind == TY_VOID) return 1;
        if (dst->base->kind == s->base->kind &&
            dst->base->size == s->base->size &&
            dst->base->is_unsigned == s->base->is_unsigned) {
            if (is_aggregate(dst->base) &&
                !(dst->base->tag && s->base->tag &&
                  strcmp(dst->base->tag, s->base->tag) == 0)) {
                *why = "these point at different struct types";
                return 0;
            }
            return 1;
        }
        *why = "these point at different types; add a cast if that is deliberate";
        return 0;
    }
    if (dst->kind == TY_PTR && cc_is_integer(s)) {
        if (src->kind == ND_NUM && src->val == 0) return 1;   /* the null pointer */
        *why = "a whole number is not a pointer; only the literal 0 is, or add a cast";
        return 0;
    }
    if (cc_is_integer(dst) && s->kind == TY_PTR) {
        *why = "a pointer is not a whole number; add a cast if you mean the address";
        return 0;
    }
    if (s->kind == TY_FUNC) {
        *why = "a function is not a value here; function pointers are not supported";
        return 0;
    }
    *why = "these types are not compatible";
    return 0;
}

/* ===================================================================== */
/* type assignment for expressions                                       */
/* ===================================================================== */

/* Coerce to a value of type `want`, reporting `role` (e.g. "argument 2 of foo")
 * when it cannot be done. */
static cc_node_t *coerce(cc_node_t *e, cc_type_t *want, const char *role,
                         const cc_token_t *tok) {
    e = decay(e);
    if (!e) return NULL;
    const char *why = NULL;
    if (!assignable(want, e, &why))
        return cc_errorp(tok, "%s: cannot use '%s' where '%s' is needed - %s",
                         role, TN1(e->type), TN2(want), why ? why : "");
    if (is_aggregate(want)) return e;
    return node_cast(e, want, tok);
}

static cc_node_t *want_scalar(cc_node_t *e, const char *role) {
    e = decay(e);
    if (!e) return NULL;
    if (!e->type) return e;
    if (is_aggregate(e->type))
        return cc_errorp(e->tok, "%s cannot be a %s", role, TN1(e->type));
    if (e->type->kind == TY_VOID)
        return cc_errorp(e->tok, "%s cannot be void", role);
    if (e->type->kind == TY_FUNC)
        return cc_errorp(e->tok, "%s cannot be a function name (function pointers "
                                 "are not supported)", role);
    return e;
}

/* Arithmetic on two integers: cast both to the common type. */
static cc_node_t *arith(int kind, cc_node_t *l, cc_node_t *r, const cc_token_t *tok,
                        const char *opname) {
    l = want_scalar(l, "an operand");
    r = want_scalar(r, "an operand");
    if (!l || !r) return NULL;
    if (l->type->kind == TY_PTR || r->type->kind == TY_PTR)
        return cc_errorp(tok, "'%s' cannot be applied to a pointer ('%s' and '%s')",
                         opname, TN1(l->type), TN2(r->type));
    cc_type_t *ct = common_type(l->type, r->type);
    cc_node_t *n = node_bin(kind, node_cast(l, ct, tok), node_cast(r, ct, tok), tok);
    if (!n) return NULL;
    n->type = ct;
    return n;
}

static cc_node_t *new_add(cc_node_t *l, cc_node_t *r, const cc_token_t *tok) {
    l = want_scalar(l, "an operand");
    r = want_scalar(r, "an operand");
    if (!l || !r) return NULL;
    int lp = l->type->kind == TY_PTR, rp = r->type->kind == TY_PTR;
    if (lp && rp)
        return cc_errorp(tok, "two pointers cannot be added ('%s' and '%s')",
                         TN1(l->type), TN2(r->type));
    if (rp) { cc_node_t *t = l; l = r; r = t; lp = 1; }
    if (lp) {
        if (l->type->base->size <= 0)
            return cc_errorp(tok, "cannot do arithmetic on '%s': the pointed-to "
                                  "type has no size", TN1(l->type));
        cc_node_t *scaled = node_bin(ND_MUL, node_cast(r, cc_ty_long, tok),
                                     node_num((uint64_t)l->type->base->size,
                                              cc_ty_long, tok), tok);
        if (!scaled) return NULL;
        scaled->type = cc_ty_long;
        cc_node_t *n = node_bin(ND_ADD, l, scaled, tok);
        if (!n) return NULL;
        n->type = l->type;
        return n;
    }
    return arith(ND_ADD, l, r, tok, "+");
}

static cc_node_t *new_sub(cc_node_t *l, cc_node_t *r, const cc_token_t *tok) {
    l = want_scalar(l, "an operand");
    r = want_scalar(r, "an operand");
    if (!l || !r) return NULL;
    int lp = l->type->kind == TY_PTR, rp = r->type->kind == TY_PTR;
    if (lp && rp) {
        if (l->type->base->size != r->type->base->size)
            return cc_errorp(tok, "cannot subtract '%s' from '%s'",
                             TN2(r->type), TN1(l->type));
        cc_node_t *d = node_bin(ND_SUB, l, r, tok);
        if (!d) return NULL;
        d->type = cc_ty_long;
        cc_node_t *n = node_bin(ND_DIV, d,
                                node_num((uint64_t)l->type->base->size,
                                         cc_ty_long, tok), tok);
        if (!n) return NULL;
        n->type = cc_ty_long;
        return n;
    }
    if (rp)
        return cc_errorp(tok, "a pointer cannot be subtracted from a number "
                              "('%s' - '%s')", TN1(l->type), TN2(r->type));
    if (lp) {
        cc_node_t *scaled = node_bin(ND_MUL, node_cast(r, cc_ty_long, tok),
                                     node_num((uint64_t)l->type->base->size,
                                              cc_ty_long, tok), tok);
        if (!scaled) return NULL;
        scaled->type = cc_ty_long;
        cc_node_t *n = node_bin(ND_SUB, l, scaled, tok);
        if (!n) return NULL;
        n->type = l->type;
        return n;
    }
    return arith(ND_SUB, l, r, tok, "-");
}

/* A comparison. Both sides become the common type; the result is int. */
static cc_node_t *compare(int kind, cc_node_t *l, cc_node_t *r,
                          const cc_token_t *tok, const char *opname) {
    l = want_scalar(l, "an operand");
    r = want_scalar(r, "an operand");
    if (!l || !r) return NULL;
    int lp = l->type->kind == TY_PTR, rp = r->type->kind == TY_PTR;
    cc_node_t *n;
    if (lp || rp) {
        if (lp != rp) {
            cc_node_t *num = lp ? r : l;
            if (!(num->kind == ND_NUM && num->val == 0))
                return cc_errorp(tok,
                                 "'%s' compares '%s' with '%s'; only the literal "
                                 "0 may be compared with a pointer",
                                 opname, TN1(l->type), TN2(r->type));
        }
        n = node_bin(kind, node_cast(l, cc_ty_ulong, tok),
                     node_cast(r, cc_ty_ulong, tok), tok);
    } else {
        cc_type_t *ct = common_type(l->type, r->type);
        n = node_bin(kind, node_cast(l, ct, tok), node_cast(r, ct, tok), tok);
    }
    if (!n) return NULL;
    n->type = cc_ty_int;
    return n;
}

/* ===================================================================== */
/* compound assignment, via a hidden pointer temporary                   */
/* ===================================================================== */

static cc_sym_t *new_temp(cc_type_t *ty, const cc_token_t *tok);

/* `lhs op= rhs` becomes `(tmp = &lhs, *tmp = *tmp op rhs)`, so the lvalue is
 * evaluated exactly once. Same machinery serves ++ and --. */
static cc_node_t *compound_assign(cc_node_t *lhs, cc_node_t *rhs,
                                  int kind, const cc_token_t *tok,
                                  const char *opname) {
    if (!lhs || !rhs) return NULL;
    if (!is_lvalue(lhs))
        return cc_errorp(tok, "the left side of '%s' must be something that can "
                              "be assigned to", opname);
    if (lhs->type->kind == TY_ARRAY)
        return cc_errorp(tok, "an array cannot be assigned to");
    if (is_aggregate(lhs->type))
        return cc_errorp(tok, "'%s' cannot be applied to a %s", opname, TN1(lhs->type));

    cc_sym_t *tmp = new_temp(cc_pointer_to(lhs->type), tok);
    if (!tmp) return NULL;

    cc_node_t *tv = node(ND_VAR, tok);
    if (!tv) return NULL;
    tv->sym = tmp; tv->type = tmp->type;

    cc_node_t *addr = node_un(ND_ADDR, lhs, tok);
    if (!addr) return NULL;
    addr->type = cc_pointer_to(lhs->type);

    cc_node_t *set = node_bin(ND_ASSIGN, tv, addr, tok);
    if (!set) return NULL;
    set->type = tv->type;

    cc_node_t *d1 = node_un(ND_DEREF, tv, tok); if (!d1) return NULL;
    d1->type = lhs->type;
    cc_node_t *d2 = node_un(ND_DEREF, tv, tok); if (!d2) return NULL;
    d2->type = lhs->type;

    cc_node_t *val;
    switch (kind) {
    case ND_ADD: val = new_add(d2, rhs, tok); break;
    case ND_SUB: val = new_sub(d2, rhs, tok); break;
    case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_BITAND: case ND_BITOR: case ND_BITXOR:
        val = arith(kind, d2, rhs, tok, opname); break;
    case ND_SHL: case ND_SHR: {
        cc_node_t *r2 = want_scalar(rhs, "a shift count");
        if (!r2) return NULL;
        val = node_bin(kind, d2, node_cast(r2, cc_ty_int, tok), tok);
        if (val) val->type = d2->type;
        break;
    }
    default: return cc_errorp(tok, "'%s' is not supported", opname);
    }
    if (!val) return NULL;

    cc_node_t *store = coerce(val, lhs->type, "the compound assignment", tok);
    if (!store) return NULL;
    cc_node_t *asn = node_bin(ND_ASSIGN, d1, store, tok);
    if (!asn) return NULL;
    asn->type = lhs->type;

    cc_node_t *comma = node_bin(ND_COMMA, set, asn, tok);
    if (!comma) return NULL;
    comma->type = lhs->type;
    return comma;
}

/* ===================================================================== */
/* struct / union / enum declarations                                    */
/* ===================================================================== */

static int struct_members(cc_type_t *ty) {
    cc_member_t *arr = cc_alloc(sizeof(cc_member_t) * CC_MAX_MEMBERS);
    if (!arr) return 0;
    int n = 0, offset = 0, align = 1;

    while (!at("}")) {
        if (tk()->kind == TK_EOF)
            return cc_errorf(tk(), "this struct or union is never closed with '}'");
        int st = 0, ex = 0, td = 0;
        cc_type_t *base = declspec(&st, &ex, &td);
        if (!base) return 0;
        if (st || ex || td)
            return cc_errorf(tk(), "a member cannot be static, extern or a typedef");
        int first = 1;
        while (!consume(";")) {
            if (!first && !expect(",")) return 0;
            first = 0;
            const cc_token_t *nm = NULL;
            cc_type_t *mt = declarator(base, &nm);
            if (!mt) return 0;
            if (!nm) return cc_errorf(tk(), "this member has no name");
            if (mt->kind == TY_FUNC)
                return cc_errorf(nm, "'%s' is a function; a struct member cannot "
                                     "be a function or a function pointer", nm->text);
            if (mt->kind == TY_VOID)
                return cc_errorf(nm, "'%s' cannot be void", nm->text);
            if (mt->size <= 0 && mt->kind == TY_ARRAY)
                return cc_errorf(nm, "'%s' needs an array size", nm->text);
            if (at(":"))
                return cc_errorf(tk(), "bitfields are not supported");
            if (n >= CC_MAX_MEMBERS)
                return cc_errorf(nm, "a struct may have at most %d members",
                                 CC_MAX_MEMBERS);
            for (int i = 0; i < n; i++)
                if (strcmp(arr[i].name, nm->text) == 0)
                    return cc_errorf(nm, "'%s' is already a member of this %s",
                                     nm->text,
                                     ty->kind == TY_UNION ? "union" : "struct");
            if (mt->align > align) align = mt->align;
            if (ty->kind == TY_UNION) {
                arr[n].offset = 0;
                if (mt->size > offset) offset = mt->size;
            } else {
                offset = (offset + mt->align - 1) / mt->align * mt->align;
                arr[n].offset = offset;
                offset += mt->size;
            }
            arr[n].name = nm->text;
            arr[n].type = mt;
            n++;
        }
    }
    if (!expect("}")) return 0;
    ty->members  = arr;
    ty->nmembers = n;
    ty->align    = align;
    ty->size     = (offset + align - 1) / align * align;
    if (n == 0) return cc_errorf(tk(), "an empty struct or union is not allowed");
    return 1;
}

static cc_type_t *struct_or_union(int kind) {
    const cc_token_t *tag = NULL;
    if (tk()->kind == TK_IDENT) { tag = tk(); bump(); }

    if (!at("{")) {
        if (!tag)
            return cc_errorp(tk(), "expected a tag name or '{' after '%s'",
                             kind == TY_UNION ? "union" : "struct");
        cc_type_t *found = find_tag(tag->text);
        if (!found)
            return cc_errorp(tag, "there is no %s named '%s'",
                             kind == TY_UNION ? "union" : "struct", tag->text);
        return found;
    }
    bump();                                  /* '{' */

    cc_type_t *ty = cc_alloc(sizeof *ty);
    if (!ty) return NULL;
    ty->kind = (uint8_t)kind;
    ty->tag  = tag ? tag->text : NULL;
    ty->size = -1;
    ty->align = 1;
    /* Registered BEFORE the members are read so `struct node { struct node *next; }`
     * works: a pointer to an incomplete type is complete. */
    if (tag && !push_tag(tag->text, ty)) return NULL;
    if (!struct_members(ty)) return NULL;
    return ty;
}

static cc_type_t *enum_decl(void) {
    const cc_token_t *tag = NULL;
    if (tk()->kind == TK_IDENT) { tag = tk(); bump(); }
    if (!at("{")) {
        if (!tag) return cc_errorp(tk(), "expected a tag name or '{' after 'enum'");
        cc_type_t *found = find_tag(tag->text);
        if (!found) return cc_errorp(tag, "there is no enum named '%s'", tag->text);
        return found;
    }
    bump();
    int64_t val = 0;
    int count = 0;
    while (!at("}")) {
        if (tk()->kind != TK_IDENT)
            return cc_errorp(tk(), "an enumerator must be an identifier, not '%s'",
                             spell(tk()));
        const cc_token_t *nm = tk();
        bump();
        if (consume("=")) {
            if (!const_expr(&val)) return NULL;
        }
        if (find_sym_here(nm->text))
            return cc_errorp(nm, "'%s' is already declared in this scope", nm->text);
        cc_sym_t *s = push_sym(SY_ENUMCONST, nm->text, cc_ty_int);
        if (!s) return NULL;
        s->val = val++;
        count++;
        if (!consume(",")) break;
    }
    if (!expect("}")) return NULL;
    if (count == 0) return cc_errorp(tk(), "an empty enum is not allowed");
    /* enum is int, and the tag names that. */
    if (tag && !push_tag(tag->text, cc_ty_int)) return NULL;
    return cc_ty_int;
}

/* ===================================================================== */
/* declaration specifiers                                                */
/* ===================================================================== */

static int is_typename(const cc_token_t *t) {
    static const char *const names[] = {
        "void", "char", "short", "int", "long", "signed", "unsigned",
        "struct", "union", "enum", "const", "volatile", "register",
        "static", "extern", "typedef", "float", "double", "_Bool", NULL };
    for (int i = 0; names[i]; i++) if (eq(t, names[i])) return 1;
    if (t->kind == TK_IDENT) {
        cc_sym_t *s = find_sym(t->text);
        return s && s->kind == SY_TYPEDEF;
    }
    return 0;
}

/* void | char | short | int | long | signed | unsigned in any order, plus one
 * struct/union/enum/typedef-name. Counts are tracked so `long long` works and
 * `int int` is refused. */
static cc_type_t *declspec(int *is_static, int *is_extern, int *is_typedef) {
    int c_void = 0, c_char = 0, c_short = 0, c_int = 0, c_long = 0;
    int c_signed = 0, c_unsigned = 0;
    cc_type_t *named = NULL;
    int any = 0;
    *is_static = *is_extern = *is_typedef = 0;

    for (;;) {
        cc_token_t *t = tk();
        if (eq(t, "static"))   { *is_static = 1;  bump(); any = 1; continue; }
        if (eq(t, "extern"))   { *is_extern = 1;  bump(); any = 1; continue; }
        if (eq(t, "typedef"))  { *is_typedef = 1; bump(); any = 1; continue; }
        /* Accepted and ignored; cc.h says so out loud. */
        if (eq(t, "const") || eq(t, "volatile") || eq(t, "register") ||
            eq(t, "auto") || eq(t, "inline") || eq(t, "restrict")) {
            bump(); any = 1; continue;
        }
        if (eq(t, "float") || eq(t, "double"))
            return cc_errorp(t, "floating point is not supported: this kernel is "
                                "built -mno-sse and saves no FPU state. Use "
                                "integers, or fixed point (scale by 1000)");
        if (eq(t, "_Bool"))
            return cc_errorp(t, "_Bool is not supported; use int, which is what a "
                                "comparison already produces");
        if (eq(t, "_Complex") || eq(t, "_Generic") || eq(t, "_Static_assert"))
            return cc_errorp(t, "'%s' is not supported", t->text);
        if (eq(t, "void"))     { c_void++;     bump(); any = 1; continue; }
        if (eq(t, "char"))     { c_char++;     bump(); any = 1; continue; }
        if (eq(t, "short"))    { c_short++;    bump(); any = 1; continue; }
        if (eq(t, "int"))      { c_int++;      bump(); any = 1; continue; }
        if (eq(t, "long"))     { c_long++;     bump(); any = 1; continue; }
        if (eq(t, "signed"))   { c_signed++;   bump(); any = 1; continue; }
        if (eq(t, "unsigned")) { c_unsigned++; bump(); any = 1; continue; }
        if (eq(t, "struct") || eq(t, "union")) {
            if (named) return cc_errorp(t, "two type names in one declaration");
            int k = eq(t, "struct") ? TY_STRUCT : TY_UNION;
            bump();
            named = struct_or_union(k);
            if (!named) return NULL;
            any = 1; continue;
        }
        if (eq(t, "enum")) {
            if (named) return cc_errorp(t, "two type names in one declaration");
            bump();
            named = enum_decl();
            if (!named) return NULL;
            any = 1; continue;
        }
        if (t->kind == TK_IDENT && !named && !c_void && !c_char && !c_short &&
            !c_int && !c_long && !c_signed && !c_unsigned) {
            cc_sym_t *s = find_sym(t->text);
            if (s && s->kind == SY_TYPEDEF) { named = s->type; bump(); any = 1; continue; }
        }
        break;
    }

    if (!any) return cc_errorp(tk(), "expected a type here, found '%s'", spell(tk()));

    if (named) {
        if (c_void || c_char || c_short || c_int || c_long || c_signed || c_unsigned)
            return cc_errorp(tk(), "a struct, union, enum or typedef name cannot be "
                                   "combined with another type keyword");
        return named;
    }
    if (c_signed && c_unsigned)
        return cc_errorp(tk(), "'signed' and 'unsigned' cannot both appear");
    if (c_void) {
        if (c_char || c_short || c_int || c_long || c_signed || c_unsigned)
            return cc_errorp(tk(), "'void' cannot be combined with another type");
        if (c_void > 1) return cc_errorp(tk(), "'void void' is not a type");
        return cc_ty_void;
    }
    int u = c_unsigned != 0;
    if (c_char) {
        if (c_short || c_int || c_long) return cc_errorp(tk(), "'char' cannot be combined with that");
        if (c_char > 1) return cc_errorp(tk(), "'char char' is not a type");
        return u ? cc_ty_uchar : cc_ty_char;
    }
    if (c_short) {
        if (c_long) return cc_errorp(tk(), "'short' and 'long' cannot both appear");
        if (c_short > 1 || c_int > 1) return cc_errorp(tk(), "that type is spelled twice");
        return u ? cc_ty_ushort : cc_ty_short;
    }
    if (c_long) {
        if (c_long > 2) return cc_errorp(tk(), "'long long long' is not a type");
        if (c_int > 1) return cc_errorp(tk(), "'int' appears twice");
        return u ? cc_ty_ulong : cc_ty_long;
    }
    if (c_int > 1) return cc_errorp(tk(), "'int int' is not a type");
    if (c_int || c_signed || c_unsigned) return u ? cc_ty_uint : cc_ty_int;
    return cc_errorp(tk(), "expected a type here");
}

/* ---- function parameters ---- */

static int func_params(cc_type_t *fn) {
    if (consume(")")) { fn->no_proto = 1; return 1; }
    if (eq(tk(), "void") && eq(tk_at(1), ")")) { bump(); bump(); return 1; }

    for (;;) {
        if (at("..."))
            return cc_errorf(tk(),
                             "variadic functions ('...') are not supported. Every "
                             "call here has a checked, fixed arity - that is what "
                             "bounds what compiled code can do");
        int st = 0, ex = 0, td = 0;
        cc_type_t *base = declspec(&st, &ex, &td);
        if (!base) return 0;
        if (td) return cc_errorf(tk(), "a parameter cannot be a typedef");
        const cc_token_t *nm = NULL;
        cc_type_t *pt = declarator(base, &nm);
        if (!pt) return 0;
        /* An array parameter is a pointer, exactly as in C. */
        if (pt->kind == TY_ARRAY) pt = cc_pointer_to(pt->base);
        if (pt->kind == TY_FUNC)
            return cc_errorf(nm ? nm : tk(),
                             "a parameter cannot be a function or a function "
                             "pointer; function pointers are not supported");
        if (is_aggregate(pt))
            return cc_errorf(nm ? nm : tk(),
                             "a %s cannot be passed by value; pass a pointer to it",
                             pt->kind == TY_UNION ? "union" : "struct");
        if (pt->kind == TY_VOID)
            return cc_errorf(nm ? nm : tk(), "a parameter cannot be void");
        if (fn->nparam >= CC_MAX_PARAMS)
            return cc_errorf(nm ? nm : tk(),
                             "a function may take at most %d parameters (they are "
                             "passed in registers and there are no stack "
                             "arguments)", CC_MAX_PARAMS);
        fn->pname[fn->nparam] = nm;      /* NULL in a prototype; see cc_int.h */
        fn->param[fn->nparam] = pt;
        fn->nparam++;
        if (!consume(",")) break;
    }
    return expect(")");
}

/* type-suffix := "(" params ")" | "[" const "]" type-suffix | ε */
static cc_type_t *type_suffix(cc_type_t *base) {
    if (consume("(")) {
        cc_type_t *fn = cc_alloc(sizeof *fn);
        if (!fn) return NULL;
        fn->kind = TY_FUNC;
        fn->base = base;
        fn->size = 1;
        fn->align = 1;
        if (!func_params(fn)) return NULL;
        return fn;
    }
    if (consume("[")) {
        int len = -1;
        if (!at("]")) {
            int64_t v;
            if (!const_expr(&v)) return NULL;
            if (v <= 0)
                return cc_errorp(tk(), "an array needs a positive size, not %d", (int)v);
            if (v > 1000000)
                return cc_errorp(tk(), "an array of %d elements is larger than this "
                                       "compiler will build", (int)v);
            len = (int)v;
        }
        if (!expect("]")) return NULL;
        cc_type_t *inner = type_suffix(base);
        if (!inner) return NULL;
        if (inner->kind == TY_FUNC)
            return cc_errorp(tk(), "an array of functions is not a type");
        if (len > 0 && inner->size > 0 && len > 1000000 / (inner->size ? inner->size : 1))
            return cc_errorp(tk(), "this array is too large");
        return cc_array_of(inner, len);
    }
    return base;
}

static cc_type_t *declarator(cc_type_t *base, const cc_token_t **name) {
    while (consume("*")) {
        base = cc_pointer_to(base);
        if (!base) return NULL;
        while (eq(tk(), "const") || eq(tk(), "volatile")) bump();
    }
    if (at("(")) {
        /* `(*f)(void)` and `(*a)[3]` are the two shapes that reach here, and both
         * are function pointers or arrays of them in practice. Refuse by name
         * rather than parse something this compiler cannot generate. */
        if (eq(tk_at(1), "*"))
            return cc_errorp(tk(), "function pointers are not supported. A call's "
                                   "target must be a name the compiler resolved - "
                                   "that is what bounds where compiled code can "
                                   "jump");
        return cc_errorp(tk(), "a parenthesised declarator is not supported here");
    }
    if (tk()->kind == TK_KEYWORD)
        return cc_errorp(tk(), "'%s' is a keyword and cannot be a name", tk()->text);
    if (tk()->kind != TK_IDENT) {
        *name = NULL;
        return type_suffix(base);
    }
    *name = tk();
    bump();
    return type_suffix(base);
}

static cc_type_t *abstract_declarator(cc_type_t *base) {
    while (consume("*")) {
        base = cc_pointer_to(base);
        if (!base) return NULL;
        while (eq(tk(), "const") || eq(tk(), "volatile")) bump();
    }
    if (at("("))
        return cc_errorp(tk(), "function pointer types are not supported");
    if (at("[")) return type_suffix(base);
    return base;
}

/* ===================================================================== */
/* constant expressions                                                  */
/* ===================================================================== */

/* Evaluated over the tree so that sizeof, enum constants and the usual operators
 * all work with one implementation. Anything that is not a compile-time constant
 * is reported as such, naming the token. */
static int eval_const(cc_node_t *n, int64_t *out) {
    if (!n) return 0;
    switch (n->kind) {
    case ND_NUM: *out = (int64_t)n->val; return 1;
    case ND_CAST: {
        int64_t v;
        if (!eval_const(n->lhs, &v)) return 0;
        if (!n->type) { *out = v; return 1; }
        switch (n->type->size) {
        case 1: v = n->type->is_unsigned ? (int64_t)(uint8_t)v  : (int64_t)(int8_t)v;  break;
        case 2: v = n->type->is_unsigned ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v; break;
        case 4: v = n->type->is_unsigned ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v; break;
        default: break;
        }
        *out = v;
        return 1;
    }
    case ND_VAR:
        if (n->sym && n->sym->kind == SY_ENUMCONST) { *out = n->sym->val; return 1; }
        return cc_errorf(n->tok, "'%s' is not a compile-time constant",
                         n->sym ? n->sym->name : "this");
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_SHL: case ND_SHR: case ND_BITAND: case ND_BITOR: case ND_BITXOR:
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
    case ND_LOGAND: case ND_LOGOR: case ND_COMMA: {
        int64_t a, b;
        if (!eval_const(n->lhs, &a) || !eval_const(n->rhs, &b)) return 0;
        int uns = n->type && n->type->is_unsigned;
        switch (n->kind) {
        case ND_ADD: *out = (int64_t)((uint64_t)a + (uint64_t)b); break;
        case ND_SUB: *out = (int64_t)((uint64_t)a - (uint64_t)b); break;
        case ND_MUL: *out = (int64_t)((uint64_t)a * (uint64_t)b); break;
        case ND_DIV:
            if (b == 0) return cc_errorf(n->tok, "division by zero in a constant");
            *out = uns ? (int64_t)((uint64_t)a / (uint64_t)b) : a / b;
            break;
        case ND_MOD:
            if (b == 0) return cc_errorf(n->tok, "remainder by zero in a constant");
            *out = uns ? (int64_t)((uint64_t)a % (uint64_t)b) : a % b;
            break;
        case ND_SHL: *out = (int64_t)((uint64_t)a << ((uint64_t)b & 63)); break;
        case ND_SHR: *out = uns ? (int64_t)((uint64_t)a >> ((uint64_t)b & 63))
                                : (a >> (b & 63)); break;
        case ND_BITAND: *out = a & b; break;
        case ND_BITOR:  *out = a | b; break;
        case ND_BITXOR: *out = a ^ b; break;
        case ND_EQ: *out = a == b; break;
        case ND_NE: *out = a != b; break;
        case ND_LT: *out = uns ? ((uint64_t)a < (uint64_t)b) : (a < b); break;
        case ND_LE: *out = uns ? ((uint64_t)a <= (uint64_t)b) : (a <= b); break;
        case ND_LOGAND: *out = (a && b); break;
        case ND_LOGOR:  *out = (a || b); break;
        default: *out = b; break;              /* comma */
        }
        return 1;
    }
    case ND_NOT: { int64_t v; if (!eval_const(n->lhs, &v)) return 0; *out = !v; return 1; }
    case ND_BITNOT: { int64_t v; if (!eval_const(n->lhs, &v)) return 0; *out = ~v; return 1; }
    case ND_COND: {
        int64_t c;
        if (!eval_const(n->cond, &c)) return 0;
        return eval_const(c ? n->then : n->els, out);
    }
    default:
        return cc_errorf(n->tok, "this is not a compile-time constant");
    }
}

static int const_expr(int64_t *out) {
    cc_node_t *e = conditional();
    if (!e) return 0;
    return eval_const(e, out);
}

/* ===================================================================== */
/* locals                                                                */
/* ===================================================================== */

static cc_sym_t *new_local(const char *name, cc_type_t *ty, const cc_token_t *tok) {
    if (!cc_ctx.cur) {
        cc_errorf(tok, "'%s' can only be declared inside a function", name);
        return NULL;
    }
    if (ty->size <= 0) {
        cc_errorf(tok, "'%s' has no size ('%s')", name, TN1(ty));
        return NULL;
    }
    int align = ty->align < 1 ? 1 : ty->align;
    cc_ctx.cur_offset -= ty->size;
    /* Round DOWN to the alignment, since offsets are negative. */
    cc_ctx.cur_offset = -(((-cc_ctx.cur_offset) + align - 1) / align * align);
    if (-cc_ctx.cur_offset > CC_MAX_FRAME) {
        cc_errorf(tok,
                  "the locals of '%s' need %d bytes; one function may use at most "
                  "%d (a bigger frame could step over the stack guard). Use fewer "
                  "or smaller locals, or split the function",
                  cc_ctx.cur->name, -cc_ctx.cur_offset, CC_MAX_FRAME);
        return NULL;
    }
    cc_sym_t *s = push_sym(SY_LOCAL, name, ty);
    if (!s) return NULL;
    s->offset = cc_ctx.cur_offset;
    return s;
}

/* A hidden pointer temporary, from the fixed region at the top of the frame.
 * `cc_ctx.ntemp` is reset at every statement boundary — see CC_TEMP_SLOTS. */
static cc_sym_t *new_temp(cc_type_t *ty, const cc_token_t *tok) {
    if (!cc_ctx.cur) {
        cc_errorf(tok, "'++', '--' and compound assignment only work inside a "
                       "function");
        return NULL;
    }
    if (cc_ctx.ntemp >= CC_TEMP_SLOTS) {
        cc_errorf(tok, "this statement needs more than %d hidden temporaries "
                       "(too many '++' or compound assignments in one "
                       "statement); split it", CC_TEMP_SLOTS);
        return NULL;
    }
    int slot = cc_ctx.ntemp++;
    if (cc_ctx.ntemp > cc_ctx.temp_peak) cc_ctx.temp_peak = cc_ctx.ntemp;
    char buf[24];
    snprintf(buf, sizeof buf, " t%d", slot);   /* a space: unspellable in C */
    cc_sym_t *s = cc_alloc(sizeof *s);
    if (!s) return NULL;
    s->kind = SY_LOCAL;
    s->name = cc_intern(buf, strlen(buf));
    s->type = ty;
    s->offset = -8 * (slot + 1);
    s->scope_depth = cc_ctx.scope_depth;
    return s;                       /* deliberately NOT pushed into any scope */
}

/* ===================================================================== */
/* expressions                                                           */
/* ===================================================================== */

static cc_node_t *funcall(const cc_token_t *nametok, cc_sym_t *sym) {
    cc_type_t *fn = sym->type;
    cc_node_t *n = node(ND_CALL, nametok);
    if (!n) return NULL;
    n->callee = sym;
    n->type = fn->base;

    cc_node_t head; head.next = NULL;
    cc_node_t *cur = &head;
    int nargs = 0;

    if (!consume(")")) {
        for (;;) {
            cc_node_t *a = assign_expr();
            if (!a) return NULL;
            if (nargs >= CC_MAX_PARAMS)
                return cc_errorp(nametok, "'%s' is called with more than %d "
                                          "arguments", sym->name, CC_MAX_PARAMS);
            if (!fn->no_proto && nargs < fn->nparam) {
                char role[64];
                snprintf(role, sizeof role, "argument %d of %s", nargs + 1, sym->name);
                a = coerce(a, fn->param[nargs], role, a->tok);
            } else {
                a = want_scalar(a, "an argument");
                if (a) a = node_cast(a, cc_ty_long, a->tok);
            }
            if (!a) return NULL;
            cur->next = a;
            cur = a;
            nargs++;
            if (!consume(",")) break;
        }
        if (!expect(")")) return NULL;
    }

    if (!fn->no_proto && nargs != fn->nparam)
        return cc_errorp(nametok,
                         "%s takes %d argument(s) but %d %s given",
                         sym->name, fn->nparam, nargs, nargs == 1 ? "was" : "were");
    n->args  = head.next;
    n->nargs = nargs;
    return n;
}

/* A string literal becomes bytes in the data image and an array-of-char. */
static cc_node_t *string_node(const cc_token_t *t) {
    /* Adjacent literals concatenate, which the parser does here because the
     * lexer has no reason to know about it.
     *
     * STATIC, NOT A LOCAL, for the same reason as its twin in cc_lex.c and with
     * a sharper edge: string_node() is reached from primary(), which IS on the
     * recursive path, and DEEPER()/SHALLOWER() never see it — so a 16 KiB
     * automatic array here was 16 KiB of unaccounted kernel stack that the
     * parser's own depth bound could not know about. `28 nested blocks + one
     * string literal` compiled cleanly with 872 bytes of the root stack left. */
    static char buf[CC_SRC_MAX];
    size_t n = 0;
    while (tk()->kind == TK_STR) {
        if (n + tk()->len >= sizeof buf)
            return cc_errorp(t, "this string literal is too long");
        memcpy(buf + n, tk()->text, tk()->len);
        n += tk()->len;
        bump();
    }
    buf[n] = '\0';

    int off = data_alloc((int)n + 1, 1, t);
    if (off < 0) return NULL;
    memcpy(cc_ctx.data + off, buf, n + 1);

    cc_node_t *s = node(ND_STR, t);
    if (!s) return NULL;
    s->data_off = off;
    s->data_len = (int)n + 1;
    s->type = cc_array_of(cc_ty_char, (int)n + 1);
    return s;
}

static cc_node_t *primary(void) {
    cc_token_t *t = tk();

    if (consume("(")) {
        DEEPER(NULL);
        cc_node_t *e = expr();
        SHALLOWER();
        if (!e) return NULL;
        if (!expect(")")) return NULL;
        return e;
    }
    if (eq(t, "sizeof")) {
        bump();
        if (at("(") && is_typename(tk_at(1))) {
            bump();
            int st, ex, td;
            cc_type_t *base = declspec(&st, &ex, &td);
            if (!base) return NULL;
            cc_type_t *ty = abstract_declarator(base);
            if (!ty) return NULL;
            if (!expect(")")) return NULL;
            if (ty->size <= 0 && ty->kind != TY_VOID)
                return cc_errorp(t, "sizeof('%s') is not known", TN1(ty));
            if (ty->kind == TY_VOID)
                return cc_errorp(t, "sizeof(void) is not a size");
            return node_num((uint64_t)ty->size, cc_ty_ulong, t);
        }
        DEEPER(NULL);
        cc_node_t *e = unary();
        SHALLOWER();
        if (!e) return NULL;
        if (!e->type || e->type->size <= 0)
            return cc_errorp(t, "sizeof this expression is not known");
        return node_num((uint64_t)e->type->size, cc_ty_ulong, t);
    }
    if (t->kind == TK_NUM) {
        bump();
        cc_type_t *ty;
        if (t->is_long) ty = t->is_unsigned ? cc_ty_ulong : cc_ty_long;
        else            ty = t->is_unsigned ? cc_ty_uint  : cc_ty_int;
        return node_num(t->val, ty, t);
    }
    if (t->kind == TK_CHAR) { bump(); return node_num(t->val, cc_ty_int, t); }
    if (t->kind == TK_STR)  { return string_node(t); }

    if (t->kind == TK_IDENT) {
        bump();
        cc_sym_t *s = find_sym(t->text);
        if (!s) {
            if (at("("))
                return cc_errorp(t,
                                 "there is no function called '%s'. You may call "
                                 "functions you define in this unit and the "
                                 "kernel functions the compiler predeclares "
                                 "(print, print_num, print_hex, time_ms, strlen, "
                                 "memcpy, memset, memcmp, file_read, file_write, "
                                 "file_size)", t->text);
            return cc_errorp(t, "'%s' is not declared", t->text);
        }
        if (s->kind == SY_TYPEDEF)
            return cc_errorp(t, "'%s' is a type name, not a value", t->text);
        if (s->kind == SY_ENUMCONST)
            return node_num((uint64_t)s->val, cc_ty_int, t);
        if (s->kind == SY_FUNC || s->kind == SY_EXTERN) {
            if (!consume("("))
                return cc_errorp(t,
                                 "'%s' is a function and can only be called, not "
                                 "used as a value: function pointers are not "
                                 "supported", t->text);
            return funcall(t, s);
        }
        cc_node_t *v = node(ND_VAR, t);
        if (!v) return NULL;
        v->sym = s;
        v->type = s->type;
        return v;
    }
    if (t->kind == TK_KEYWORD) {
        if (eq(t, "float") || eq(t, "double"))
            return cc_errorp(t, "floating point is not supported: this kernel is "
                                "built -mno-sse and saves no FPU state");
        if (eq(t, "_Static_assert") || eq(t, "_Generic") || eq(t, "_Complex") ||
            eq(t, "_Bool"))
            return cc_errorp(t, "'%s' is not supported", t->text);
        if (eq(t, "goto"))
            return cc_errorp(t, "goto is not supported; use while, for, break or "
                                "continue");
        return cc_errorp(t, "'%s' is a keyword and cannot be used as a value here",
                         t->text);
    }
    return cc_errorp(t, "expected a value here, found '%s'", spell(t));
}

static cc_node_t *postfix(void) {
    cc_node_t *n = primary();
    if (!n) return NULL;

    for (;;) {
        cc_token_t *t = tk();
        if (consume("[")) {
            DEEPER(NULL);
            cc_node_t *idx = expr();
            SHALLOWER();
            if (!idx) return NULL;
            if (!expect("]")) return NULL;
            cc_node_t *base = decay(n);
            if (!base) return NULL;
            if (base->type->kind != TY_PTR)
                return cc_errorp(t, "'%s' cannot be indexed with []", TN1(n->type));
            cc_node_t *sum = new_add(base, idx, t);
            if (!sum) return NULL;
            cc_node_t *d = node_un(ND_DEREF, sum, t);
            if (!d) return NULL;
            d->type = sum->type->base;
            if (d->type->kind == TY_VOID)
                return cc_errorp(t, "'void *' cannot be indexed; cast it first");
            n = d;
            continue;
        }
        if (at(".") || at("->")) {
            int arrow = at("->");
            bump();
            if (tk()->kind != TK_IDENT)
                return cc_errorp(tk(), "expected a member name after '%s'",
                                 arrow ? "->" : ".");
            const cc_token_t *mt = tk();
            bump();
            cc_node_t *obj = n;
            if (arrow) {
                cc_node_t *p = decay(obj);
                if (!p) return NULL;
                if (p->type->kind != TY_PTR)
                    return cc_errorp(mt, "'->' needs a pointer on the left, but "
                                         "this is '%s'. Use '.' instead", TN1(obj->type));
                if (!is_aggregate(p->type->base))
                    return cc_errorp(mt, "'->' needs a pointer to a struct or "
                                         "union, but this is '%s'", TN1(p->type));
                cc_node_t *d = node_un(ND_DEREF, p, mt);
                if (!d) return NULL;
                d->type = p->type->base;
                obj = d;
            } else {
                if (obj->type && obj->type->kind == TY_PTR &&
                    is_aggregate(obj->type->base))
                    return cc_errorp(mt, "'.' needs a struct or union on the left, "
                                         "but this is '%s'. Use '->' instead",
                                     TN1(obj->type));
                if (!is_aggregate(obj->type))
                    return cc_errorp(mt, "'.' needs a struct or union on the left, "
                                         "but this is '%s'", TN1(obj->type));
            }
            cc_member_t *found = NULL;
            for (int i = 0; i < obj->type->nmembers; i++)
                if (strcmp(obj->type->members[i].name, mt->text) == 0) {
                    found = &obj->type->members[i];
                    break;
                }
            if (!found) {
                char list[128];
                size_t o = 0;
                list[0] = '\0';
                for (int i = 0; i < obj->type->nmembers && o + 2 < sizeof list; i++) {
                    int w = snprintf(list + o, sizeof list - o, "%s%s",
                                     i ? ", " : "", obj->type->members[i].name);
                    if (w < 0 || (size_t)w >= sizeof list - o) break;
                    o += (size_t)w;
                }
                return cc_errorp(mt, "'%s' has no member '%s'. It has: %s",
                                 TN1(obj->type), mt->text, list);
            }
            if (!is_lvalue(obj) && obj->kind != ND_DEREF)
                return cc_errorp(mt, "this struct value has no address, so its "
                                     "members cannot be reached");
            cc_node_t *m = node(ND_MEMBER, mt);
            if (!m) return NULL;
            m->lhs = obj;
            m->member = found;
            m->type = found->type;
            n = m;
            continue;
        }
        if (at("++") || at("--")) {
            int delta = at("++") ? 1 : -1;
            bump();
            /* (v += 1) - 1, with the lvalue evaluated once by compound_assign. */
            cc_node_t *one = node_num(1, cc_ty_int, t);
            cc_node_t *inc = compound_assign(n, one, delta > 0 ? ND_ADD : ND_SUB, t,
                                             delta > 0 ? "++" : "--");
            if (!inc) return NULL;
            cc_node_t *back = node_num(1, cc_ty_int, t);
            cc_node_t *res = (delta > 0) ? new_sub(inc, back, t) : new_add(inc, back, t);
            if (!res) return NULL;
            n = res;
            continue;
        }
        return n;
    }
}

static cc_node_t *unary(void) {
    cc_token_t *t = tk();

    if (consume("+")) { DEEPER(NULL); cc_node_t *e = cast_expr(); SHALLOWER();
                        return want_scalar(e, "an operand"); }
    if (consume("-")) {
        DEEPER(NULL); cc_node_t *e = cast_expr(); SHALLOWER();
        e = want_scalar(e, "an operand");
        if (!e) return NULL;
        if (e->type->kind == TY_PTR)
            return cc_errorp(t, "a pointer cannot be negated");
        cc_type_t *ct = common_type(e->type, cc_ty_int);
        return new_sub(node_num(0, ct, t), node_cast(e, ct, t), t);
    }
    if (consume("!")) {
        DEEPER(NULL); cc_node_t *e = cast_expr(); SHALLOWER();
        e = want_scalar(e, "an operand");
        if (!e) return NULL;
        cc_node_t *n = node_un(ND_NOT, e, t);
        if (!n) return NULL;
        n->type = cc_ty_int;
        return n;
    }
    if (consume("~")) {
        DEEPER(NULL); cc_node_t *e = cast_expr(); SHALLOWER();
        e = want_scalar(e, "an operand");
        if (!e) return NULL;
        if (e->type->kind == TY_PTR)
            return cc_errorp(t, "'~' cannot be applied to a pointer");
        cc_type_t *ct = common_type(e->type, cc_ty_int);
        cc_node_t *n = node_un(ND_BITNOT, node_cast(e, ct, t), t);
        if (!n) return NULL;
        n->type = ct;
        return n;
    }
    if (at("++") || at("--")) {
        int plus = at("++");
        bump();
        DEEPER(NULL); cc_node_t *e = unary(); SHALLOWER();
        if (!e) return NULL;
        return compound_assign(e, node_num(1, cc_ty_int, t),
                               plus ? ND_ADD : ND_SUB, t, plus ? "++" : "--");
    }
    if (consume("&")) {
        /* cast-expression, not unary-expression: C's grammar says so, and
         * without it `*(int *)p` parses the '(' as a parenthesised expression
         * and reports "expected a value here, found 'int'" - a message about a
         * cast that is perfectly well formed. */
        DEEPER(NULL); cc_node_t *e = cast_expr(); SHALLOWER();
        if (!e) return NULL;
        if (e->type && e->type->kind == TY_FUNC)
            return cc_errorp(t, "the address of a function cannot be taken: "
                                "function pointers are not supported");
        if (!is_lvalue(e))
            return cc_errorp(t, "'&' needs something with an address; this value "
                                "has none");
        cc_node_t *n = node_un(ND_ADDR, e, t);
        if (!n) return NULL;
        n->type = cc_pointer_to(e->type);
        return n;
    }
    if (consume("*")) {
        DEEPER(NULL); cc_node_t *e = cast_expr(); SHALLOWER();   /* see '&' above */
        e = decay(e);
        if (!e) return NULL;
        if (e->type->kind != TY_PTR)
            return cc_errorp(t, "'*' needs a pointer, but this is '%s'", TN1(e->type));
        if (e->type->base->kind == TY_VOID)
            return cc_errorp(t, "'void *' cannot be dereferenced; cast it to a "
                                "pointer to something first");
        if (e->type->base->kind == TY_FUNC)
            return cc_errorp(t, "function pointers are not supported");
        cc_node_t *n = node_un(ND_DEREF, e, t);
        if (!n) return NULL;
        n->type = e->type->base;
        return n;
    }
    return postfix();
}

static cc_node_t *cast_expr(void) {
    if (at("(") && is_typename(tk_at(1))) {
        cc_token_t *t = tk();
        bump();
        int st, ex, td;
        cc_type_t *base = declspec(&st, &ex, &td);
        if (!base) return NULL;
        cc_type_t *ty = abstract_declarator(base);
        if (!ty) return NULL;
        if (!expect(")")) return NULL;
        if (at("{"))
            return cc_errorp(t, "compound literals are not supported");
        DEEPER(NULL);
        cc_node_t *e = cast_expr();
        SHALLOWER();
        e = decay(e);
        if (!e) return NULL;
        if (ty->kind == TY_VOID) {
            cc_node_t *n = node_cast(e, cc_ty_void, t);
            return n;
        }
        if (is_aggregate(ty) || is_aggregate(e->type))
            return cc_errorp(t, "a struct or union cannot be cast ('%s' to '%s')",
                             TN1(e->type), TN2(ty));
        if (ty->kind == TY_ARRAY)
            return cc_errorp(t, "a value cannot be cast to an array type");
        if (e->type->kind == TY_VOID)
            return cc_errorp(t, "a void value cannot be cast to '%s'", TN1(ty));
        cc_node_t *n = node(ND_CAST, t);
        if (!n) return NULL;
        n->lhs = e;
        n->type = ty;
        return n;
    }
    return unary();
}

static cc_node_t *mul_expr(void) {
    cc_node_t *n = cast_expr();
    if (!n) return NULL;
    for (;;) {
        cc_token_t *t = tk();
        if (consume("*"))      { DEEPER(NULL); cc_node_t *r = cast_expr(); SHALLOWER();
                                 n = arith(ND_MUL, n, r, t, "*"); }
        else if (consume("/")) { DEEPER(NULL); cc_node_t *r = cast_expr(); SHALLOWER();
                                 n = arith(ND_DIV, n, r, t, "/"); }
        else if (consume("%")) { DEEPER(NULL); cc_node_t *r = cast_expr(); SHALLOWER();
                                 n = arith(ND_MOD, n, r, t, "%%"); }
        else return n;
        if (!n) return NULL;
    }
}

static cc_node_t *add_expr(void) {
    cc_node_t *n = mul_expr();
    if (!n) return NULL;
    for (;;) {
        cc_token_t *t = tk();
        if (consume("+"))      { DEEPER(NULL); cc_node_t *r = mul_expr(); SHALLOWER();
                                 n = new_add(n, r, t); }
        else if (consume("-")) { DEEPER(NULL); cc_node_t *r = mul_expr(); SHALLOWER();
                                 n = new_sub(n, r, t); }
        else return n;
        if (!n) return NULL;
    }
}

static cc_node_t *shift_expr(void) {
    cc_node_t *n = add_expr();
    if (!n) return NULL;
    for (;;) {
        cc_token_t *t = tk();
        int k;
        if (at("<<")) k = ND_SHL;
        else if (at(">>")) k = ND_SHR;
        else return n;
        bump();
        DEEPER(NULL); cc_node_t *r = add_expr(); SHALLOWER();
        n = want_scalar(n, "an operand");
        r = want_scalar(r, "a shift count");
        if (!n || !r) return NULL;
        if (n->type->kind == TY_PTR || r->type->kind == TY_PTR)
            return cc_errorp(t, "a pointer cannot be shifted");
        /* The shift's result type is the PROMOTED left operand — the right one
         * does not join a common type at all (C11 6.5.7p3). Promotion is to int
         * for every narrow type, unsigned or not, for the reason spelled out in
         * common_type(); this is the second, open-coded copy of that rule and it
         * had the same bug. `(uint8_t)0x80 >> 1` must be 0x40 either way, but
         * `(short)-2 >> 1` is -1 and was 0x7FFFFFFF. */
        cc_type_t *lt = n->type->size < 4 ? cc_ty_int : n->type;
        cc_node_t *s = node_bin(k, node_cast(n, lt, t), node_cast(r, cc_ty_int, t), t);
        if (!s) return NULL;
        s->type = lt;
        n = s;
    }
}

static cc_node_t *rel_expr(void) {
    cc_node_t *n = shift_expr();
    if (!n) return NULL;
    for (;;) {
        cc_token_t *t = tk();
        if (consume("<"))       { DEEPER(NULL); cc_node_t *r = shift_expr(); SHALLOWER();
                                  n = compare(ND_LT, n, r, t, "<"); }
        else if (consume("<=")) { DEEPER(NULL); cc_node_t *r = shift_expr(); SHALLOWER();
                                  n = compare(ND_LE, n, r, t, "<="); }
        else if (consume(">"))  { DEEPER(NULL); cc_node_t *r = shift_expr(); SHALLOWER();
                                  n = compare(ND_LT, r, n, t, ">"); }
        else if (consume(">=")) { DEEPER(NULL); cc_node_t *r = shift_expr(); SHALLOWER();
                                  n = compare(ND_LE, r, n, t, ">="); }
        else return n;
        if (!n) return NULL;
    }
}

static cc_node_t *eq_expr(void) {
    cc_node_t *n = rel_expr();
    if (!n) return NULL;
    for (;;) {
        cc_token_t *t = tk();
        if (consume("=="))      { DEEPER(NULL); cc_node_t *r = rel_expr(); SHALLOWER();
                                  n = compare(ND_EQ, n, r, t, "=="); }
        else if (consume("!=")) { DEEPER(NULL); cc_node_t *r = rel_expr(); SHALLOWER();
                                  n = compare(ND_NE, n, r, t, "!="); }
        else return n;
        if (!n) return NULL;
    }
}

static cc_node_t *bitand_expr(void) {
    cc_node_t *n = eq_expr();
    while (n && at("&")) {
        cc_token_t *t = tk(); bump();
        DEEPER(NULL); cc_node_t *r = eq_expr(); SHALLOWER();
        n = arith(ND_BITAND, n, r, t, "&");
    }
    return n;
}
static cc_node_t *bitxor_expr(void) {
    cc_node_t *n = bitand_expr();
    while (n && at("^")) {
        cc_token_t *t = tk(); bump();
        DEEPER(NULL); cc_node_t *r = bitand_expr(); SHALLOWER();
        n = arith(ND_BITXOR, n, r, t, "^");
    }
    return n;
}
static cc_node_t *bitor_expr(void) {
    cc_node_t *n = bitxor_expr();
    while (n && at("|")) {
        cc_token_t *t = tk(); bump();
        DEEPER(NULL); cc_node_t *r = bitxor_expr(); SHALLOWER();
        n = arith(ND_BITOR, n, r, t, "|");
    }
    return n;
}
static cc_node_t *logand_expr(void) {
    cc_node_t *n = bitor_expr();
    while (n && at("&&")) {
        cc_token_t *t = tk(); bump();
        DEEPER(NULL); cc_node_t *r = bitor_expr(); SHALLOWER();
        n = want_scalar(n, "an operand");
        r = want_scalar(r, "an operand");
        if (!n || !r) return NULL;
        cc_node_t *a = node_bin(ND_LOGAND, n, r, t);
        if (!a) return NULL;
        a->type = cc_ty_int;
        n = a;
    }
    return n;
}
static cc_node_t *logor_expr(void) {
    cc_node_t *n = logand_expr();
    while (n && at("||")) {
        cc_token_t *t = tk(); bump();
        DEEPER(NULL); cc_node_t *r = logand_expr(); SHALLOWER();
        n = want_scalar(n, "an operand");
        r = want_scalar(r, "an operand");
        if (!n || !r) return NULL;
        cc_node_t *a = node_bin(ND_LOGOR, n, r, t);
        if (!a) return NULL;
        a->type = cc_ty_int;
        n = a;
    }
    return n;
}

static cc_node_t *conditional(void) {
    cc_node_t *c = logor_expr();
    if (!c) return NULL;
    if (!at("?")) return c;
    cc_token_t *t = tk();
    bump();
    c = want_scalar(c, "the condition of '?:'");
    if (!c) return NULL;
    DEEPER(NULL);
    cc_node_t *a = expr();
    SHALLOWER();
    if (!a || !expect(":")) return NULL;
    DEEPER(NULL);
    cc_node_t *b = conditional();
    SHALLOWER();
    if (!b) return NULL;
    a = decay(a); b = decay(b);
    if (!a || !b) return NULL;

    cc_node_t *n = node(ND_COND, t);
    if (!n) return NULL;
    n->cond = c;
    if (a->type->kind == TY_VOID || b->type->kind == TY_VOID) {
        n->then = a; n->els = b; n->type = cc_ty_void;
        return n;
    }
    if (is_aggregate(a->type) || is_aggregate(b->type))
        return cc_errorp(t, "'?:' cannot produce a struct or union");
    if (a->type->kind == TY_PTR || b->type->kind == TY_PTR) {
        const char *why;
        if (a->type->kind == TY_PTR && assignable(a->type, b, &why)) {
            n->then = a; n->els = node_cast(b, a->type, t); n->type = a->type;
        } else if (b->type->kind == TY_PTR && assignable(b->type, a, &why)) {
            n->then = node_cast(a, b->type, t); n->els = b; n->type = b->type;
        } else {
            return cc_errorp(t, "the two sides of '?:' are '%s' and '%s', which "
                                "have no common type", TN1(a->type), TN2(b->type));
        }
        return n;
    }
    cc_type_t *ct = common_type(a->type, b->type);
    n->then = node_cast(a, ct, t);
    n->els  = node_cast(b, ct, t);
    n->type = ct;
    return n;
}

static cc_node_t *assign_expr(void) {
    cc_node_t *n = conditional();
    if (!n) return NULL;
    cc_token_t *t = tk();

    if (consume("=")) {
        DEEPER(NULL);
        cc_node_t *r = assign_expr();
        SHALLOWER();
        if (!r) return NULL;
        if (!is_lvalue(n))
            return cc_errorp(t, "the left side of '=' must be something that can "
                                "be assigned to");
        if (n->type->kind == TY_ARRAY)
            return cc_errorp(t, "an array cannot be assigned to; copy its elements "
                                "or use memcpy");
        cc_node_t *v = coerce(r, n->type, "this assignment", t);
        if (!v) return NULL;
        cc_node_t *a = node_bin(is_aggregate(n->type) ? ND_COPY : ND_ASSIGN, n, v, t);
        if (!a) return NULL;
        a->type = n->type;
        return a;
    }
    struct { const char *op; int kind; } ops[] = {
        { "+=", ND_ADD }, { "-=", ND_SUB }, { "*=", ND_MUL }, { "/=", ND_DIV },
        { "%=", ND_MOD }, { "&=", ND_BITAND }, { "|=", ND_BITOR },
        { "^=", ND_BITXOR }, { "<<=", ND_SHL }, { ">>=", ND_SHR },
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        if (at(ops[i].op)) {
            bump();
            DEEPER(NULL);
            cc_node_t *r = assign_expr();
            SHALLOWER();
            if (!r) return NULL;
            return compound_assign(n, r, ops[i].kind, t, ops[i].op);
        }
    }
    return n;
}

static cc_node_t *expr(void) {
    cc_node_t *n = assign_expr();
    while (n && at(",")) {
        cc_token_t *t = tk();
        bump();
        DEEPER(NULL);
        cc_node_t *r = assign_expr();
        SHALLOWER();
        if (!r) return NULL;
        cc_node_t *c = node_bin(ND_COMMA, n, r, t);
        if (!c) return NULL;
        c->type = r->type;
        n = c;
    }
    return n;
}

/* ===================================================================== */
/* initialisers                                                          */
/* ===================================================================== */

/* A local initialiser becomes statements: zero the whole object, then assign the
 * pieces that were written. Zeroing first is what makes `int a[10] = {1};`
 * behave, and it is one instruction sequence rather than a special case per
 * shape. */
static int local_init(cc_sym_t *sym, cc_node_t **tail, const cc_token_t *tok);

static cc_node_t *lvalue_of(cc_sym_t *sym, const cc_token_t *tok) {
    cc_node_t *v = node(ND_VAR, tok);
    if (!v) return NULL;
    v->sym = sym;
    v->type = sym->type;
    return v;
}

/* Build an lvalue for `base[index]` or `base.member`. */
static cc_node_t *index_of(cc_node_t *base, int idx, const cc_token_t *tok) {
    cc_node_t *p = decay(base);
    if (!p) return NULL;
    cc_node_t *sum = new_add(p, node_num((uint64_t)idx, cc_ty_long, tok), tok);
    if (!sum) return NULL;
    cc_node_t *d = node_un(ND_DEREF, sum, tok);
    if (!d) return NULL;
    d->type = sum->type->base;
    return d;
}
static cc_node_t *member_of(cc_node_t *base, cc_member_t *m, const cc_token_t *tok) {
    cc_node_t *n = node(ND_MEMBER, tok);
    if (!n) return NULL;
    n->lhs = base;
    n->member = m;
    n->type = m->type;
    return n;
}

static int append(cc_node_t **tail, cc_node_t *stmt) {
    if (!stmt) return 0;
    (*tail)->next = stmt;
    *tail = stmt;
    return 1;
}

static cc_node_t *expr_stmt_of(cc_node_t *e, const cc_token_t *tok) {
    cc_node_t *s = node(ND_EXPR_STMT, tok);
    if (!s) return NULL;
    s->lhs = e;
    return s;
}

/* Assign one initialiser to `dst`, which is an lvalue of any supported type. */
static int init_into(cc_node_t *dst, cc_node_t **tail, const cc_token_t *tok) {
    DEEPER(0);
    int ok = 0;

    if (at("{")) {
        bump();
        cc_type_t *ty = dst->type;
        if (ty->kind == TY_ARRAY) {
            int i = 0;
            if (!at("}")) for (;;) {
                if (ty->array_len >= 0 && i >= ty->array_len) {
                    cc_errorf(tk(), "this initialiser has more than %d elements",
                              ty->array_len);
                    goto done;
                }
                cc_node_t *el = index_of(dst, i, tok);
                if (!el) goto done;
                if (!init_into(el, tail, tok)) goto done;
                i++;
                if (!consume(",")) break;
                if (at("}")) break;
            }
            if (!expect("}")) goto done;
            ok = 1;
            goto done;
        }
        if (is_aggregate(ty)) {
            int i = 0;
            if (!at("}")) for (;;) {
                if (i >= ty->nmembers) {
                    cc_errorf(tk(), "'%s' has only %d member(s)", TN1(ty), ty->nmembers);
                    goto done;
                }
                cc_node_t *m = member_of(dst, &ty->members[i], tok);
                if (!m) goto done;
                if (!init_into(m, tail, tok)) goto done;
                i++;
                if (ty->kind == TY_UNION) break;   /* only the first member */
                if (!consume(",")) break;
                if (at("}")) break;
            }
            if (!expect("}")) goto done;
            ok = 1;
            goto done;
        }
        /* `int x = { 5 };` is legal C and harmless. */
        if (!init_into(dst, tail, tok)) goto done;
        if (!consume(",")) { /* nothing */ }
        if (!expect("}")) goto done;
        ok = 1;
        goto done;
    }

    if (at("[") || at("."))
        { cc_errorf(tk(), "designated initialisers are not supported"); goto done; }

    /* A string literal filling a char array. */
    if (tk()->kind == TK_STR && dst->type->kind == TY_ARRAY &&
        dst->type->base->kind == TY_CHAR) {
        const cc_token_t *st = tk();
        cc_node_t *s = string_node(st);
        if (!s) goto done;
        int n = s->data_len;
        if (dst->type->array_len >= 0 && n > dst->type->array_len) {
            /* C allows dropping the NUL when the array is exactly the text's
             * length; anything shorter is an error naming both numbers. */
            if (n - 1 > dst->type->array_len) {
                cc_errorf(st, "this string needs %d bytes but the array holds %d",
                          n, dst->type->array_len);
                goto done;
            }
            n = dst->type->array_len;
        }
        for (int i = 0; i < n; i++) {
            cc_node_t *el = index_of(dst, i, st);
            if (!el) goto done;
            cc_node_t *ch = node_num((uint64_t)(int64_t)(int8_t)
                                     cc_ctx.data[s->data_off + i], cc_ty_int, st);
            cc_node_t *a = node_bin(ND_ASSIGN, el, node_cast(ch, el->type, st), st);
            if (!a) goto done;
            a->type = el->type;
            if (!append(tail, expr_stmt_of(a, st))) goto done;
        }
        ok = 1;
        goto done;
    }

    {
        const cc_token_t *et = tk();
        cc_node_t *e = assign_expr();
        if (!e) goto done;
        if (dst->type->kind == TY_ARRAY) {
            cc_errorf(et, "an array can only be initialised with { ... } or, for "
                          "char arrays, a string literal");
            goto done;
        }
        cc_node_t *v = coerce(e, dst->type, "this initialiser", et);
        if (!v) goto done;
        cc_node_t *a = node_bin(is_aggregate(dst->type) ? ND_COPY : ND_ASSIGN,
                                dst, v, et);
        if (!a) goto done;
        a->type = dst->type;
        if (!append(tail, expr_stmt_of(a, et))) goto done;
        ok = 1;
    }

done:
    SHALLOWER();
    return ok;
}

static int local_init(cc_sym_t *sym, cc_node_t **tail, const cc_token_t *tok) {
    cc_node_t *z = node(ND_MEMZERO, tok);
    if (!z) return 0;
    z->sym = sym;
    z->type = sym->type;
    if (!append(tail, z)) return 0;
    cc_node_t *dst = lvalue_of(sym, tok);
    if (!dst) return 0;
    return init_into(dst, tail, tok);
}

/* ---- global initialisers, written straight into the data image ---- */

static int write_scalar(uint8_t *at_, cc_type_t *ty, int64_t v) {
    switch (ty->size) {
    case 1: at_[0] = (uint8_t)v; return 1;
    case 2: { uint16_t x = (uint16_t)v; memcpy(at_, &x, 2); return 1; }
    case 4: { uint32_t x = (uint32_t)v; memcpy(at_, &x, 4); return 1; }
    case 8: { uint64_t x = (uint64_t)v; memcpy(at_, &x, 8); return 1; }
    default: return 0;
    }
}

static int global_init(uint8_t *dst, cc_type_t *ty, const cc_token_t *nametok) {
    DEEPER(0);
    int ok = 0;

    if (at("{")) {
        bump();
        if (ty->kind == TY_ARRAY) {
            int i = 0;
            if (!at("}")) for (;;) {
                if (ty->array_len >= 0 && i >= ty->array_len) {
                    cc_errorf(tk(), "this initialiser has more than %d elements",
                              ty->array_len);
                    goto done;
                }
                if (!global_init(dst + i * ty->base->size, ty->base, nametok)) goto done;
                i++;
                if (!consume(",")) break;
                if (at("}")) break;
            }
            if (!expect("}")) goto done;
            ok = 1; goto done;
        }
        if (is_aggregate(ty)) {
            int i = 0;
            if (!at("}")) for (;;) {
                if (i >= ty->nmembers) {
                    cc_errorf(tk(), "'%s' has only %d member(s)", TN1(ty), ty->nmembers);
                    goto done;
                }
                if (!global_init(dst + ty->members[i].offset, ty->members[i].type,
                                 nametok)) goto done;
                i++;
                if (ty->kind == TY_UNION) break;
                if (!consume(",")) break;
                if (at("}")) break;
            }
            if (!expect("}")) goto done;
            ok = 1; goto done;
        }
        if (!global_init(dst, ty, nametok)) goto done;
        if (!expect("}")) goto done;
        ok = 1; goto done;
    }

    if (tk()->kind == TK_STR &&
        ((ty->kind == TY_ARRAY && ty->base->kind == TY_CHAR) ||
         (ty->kind == TY_PTR && ty->base->kind == TY_CHAR))) {
        const cc_token_t *st = tk();
        cc_node_t *s = string_node(st);
        if (!s) goto done;
        if (ty->kind == TY_PTR) {
            /* The literal's absolute address. Legal because the data image is a
             * static array whose address is a link-time constant and the low
             * 4 GiB is identity-mapped. */
            uint64_t a = (uint64_t)(uintptr_t)(cc_ctx.data + s->data_off);
            memcpy(dst, &a, 8);
            ok = 1; goto done;
        }
        int n = s->data_len;
        if (ty->array_len >= 0 && n > ty->array_len) {
            if (n - 1 > ty->array_len) {
                cc_errorf(st, "this string needs %d bytes but the array holds %d",
                          n, ty->array_len);
                goto done;
            }
            n = ty->array_len;
        }
        memcpy(dst, cc_ctx.data + s->data_off, (size_t)n);
        ok = 1; goto done;
    }

    if (ty->kind == TY_ARRAY || is_aggregate(ty)) {
        cc_errorf(tk(), "'%s' needs a { ... } initialiser", nametok->text);
        goto done;
    }
    {
        int64_t v;
        if (!const_expr(&v)) goto done;
        if (!write_scalar(dst, ty, v)) {
            cc_errorf(nametok, "'%s' cannot be initialised with a constant",
                      nametok->text);
            goto done;
        }
        ok = 1;
    }
done:
    SHALLOWER();
    return ok;
}

/* ===================================================================== */
/* statements                                                            */
/* ===================================================================== */

/* Every declaration in a block, appended to `tail` as initialiser statements. */
static int block_decl(cc_node_t **tail) {
    cc_ctx.ntemp = 0;
    int st = 0, ex = 0, td = 0;
    cc_type_t *base = declspec(&st, &ex, &td);
    if (!base) return 0;

    if (consume(";")) return 1;                 /* `struct s { ... };` */

    for (;;) {
        const cc_token_t *nm = NULL;
        cc_type_t *ty = declarator(base, &nm);
        if (!ty) return 0;
        if (!nm) return cc_errorf(tk(), "this declaration has no name");
        if (ty->kind == TY_FUNC)
            return cc_errorf(nm, "a function cannot be declared inside another "
                                 "function");
        if (td) {
            if (!push_sym(SY_TYPEDEF, nm->text, ty)) return 0;
            if (!consume(",")) break;
            continue;
        }
        if (ex)
            return cc_errorf(nm, "'extern' inside a function is not supported; the "
                                 "kernel functions you may call are already "
                                 "declared");
        if (find_sym_here(nm->text))
            return cc_errorf(nm, "'%s' is already declared in this block", nm->text);
        if (ty->kind == TY_VOID)
            return cc_errorf(nm, "'%s' cannot be void: void is a return type, not "
                                 "something an object can hold", nm->text);

        /* `char s[] = "text";` — the size comes from the initialiser. */
        if (ty->kind == TY_ARRAY && ty->array_len < 0) {
            if (!at("=") )
                return cc_errorf(nm, "'%s' needs an array size, or an initialiser "
                                     "to take it from", nm->text);
            if (tk_at(1)->kind == TK_STR)
                ty = cc_array_of(ty->base, (int)tk_at(1)->len + 1);
            else
                return cc_errorf(nm, "'%s[]' can only take its size from a string "
                                     "literal here; give it a size", nm->text);
            if (!ty) return 0;
        }

        if (st) {
            /* A static local is a global with a local name. */
            int off = data_alloc(ty->size, ty->align, nm);
            if (off < 0) return 0;
            cc_sym_t *s = push_sym(SY_GLOBAL, nm->text, ty);
            if (!s) return 0;
            s->offset = off;
            if (consume("=")) {
                if (!global_init(cc_ctx.data + off, ty, nm)) return 0;
            }
        } else {
            cc_sym_t *s = new_local(nm->text, ty, nm);
            if (!s) return 0;
            if (consume("=")) {
                if (!local_init(s, tail, nm)) return 0;
            }
        }
        if (!consume(",")) break;
    }
    return expect(";");
}

static cc_node_t *compound(void) {
    const cc_token_t *tok = tk();
    if (!expect("{")) return NULL;
    DEEPER(NULL);
    enter_scope();

    cc_node_t *blk = node(ND_BLOCK, tok);
    cc_node_t head;
    head.next = NULL;
    cc_node_t *tail = &head;
    int ok = (blk != NULL);

    while (ok && !at("}")) {
        if (tk()->kind == TK_EOF) {
            cc_errorf(tok, "this '{' is never closed");
            ok = 0;
            break;
        }
        if (is_typename(tk()) &&
            !(tk()->kind == TK_IDENT && eq(tk_at(1), ":"))) {
            if (!block_decl(&tail)) { ok = 0; break; }
            continue;
        }
        cc_node_t *s = statement();
        if (!s) { ok = 0; break; }
        tail->next = s;
        tail = s;
    }
    if (ok) ok = expect("}");
    leave_scope();
    SHALLOWER();
    if (!ok) return NULL;
    blk->body = head.next;
    return blk;
}

/* Walk a switch body and thread every `case` onto n->cases, with n->default_case
 * for the default. Its own function so that its 1 KiB walk stack is paid only by
 * a compile that actually contains a switch — see the call site. */
/* Represent `v` the way the code generator will hold a value of type `t` in
 * rax: sign- or zero-extended to all 64 bits. See the 64-BIT NORMALISATION
 * INVARIANT at the top of cc_x64.c — this is the compile-time half of it. */
static uint64_t as_type(uint64_t v, const cc_type_t *t) {
    if (!t) return v;
    switch (t->size) {
    case 1:  return t->is_unsigned ? (uint64_t)(uint8_t)v
                                   : (uint64_t)(int64_t)(int8_t)v;
    case 2:  return t->is_unsigned ? (uint64_t)(uint16_t)v
                                   : (uint64_t)(int64_t)(int16_t)v;
    case 4:  return t->is_unsigned ? (uint64_t)(uint32_t)v
                                   : (uint64_t)(int64_t)(int32_t)v;
    default: return v;
    }
}

static int collect_cases(cc_node_t *n) {
    cc_node_t *tailc = NULL;
    int ncase = 0;
    int ok = 1;
    /* An explicit stack, because this walk must not recurse as deeply as a
     * model's braces go. */
    cc_node_t *stack[CC_MAX_BLOCK_DEPTH * 4];
    int sp = 0;
    stack[sp++] = n->body;
    while (sp > 0 && ok) {
        cc_node_t *s = stack[--sp];
        for (; s; s = s->next) {
            if (s->kind == ND_SWITCH) continue;   /* a nested switch owns its cases */
            if (s->kind == ND_CASE) {
                if (s->default_case) {
                    if (n->default_case) {
                        cc_errorf(s->tok, "this switch already has a 'default'");
                        ok = 0; break;
                    }
                    n->default_case = s;
                } else {
                    /* CONVERT THE CASE CONSTANT TO THE CONTROLLING EXPRESSION'S
                     * TYPE, HERE, BEFORE THE DUPLICATE SCAN AND BEFORE CODEGEN.
                     *
                     * C11 6.8.4.2p5 requires it, and cc_x64.c compares against
                     * rax, which holds the condition normalised to 64 bits. Left
                     * unconverted, one side of that cmp obeyed the invariant and
                     * the other did not, so a case value outside the condition's
                     * range MATCHED NOTHING instead of being truncated — a third
                     * category of behaviour that include/cc.h says does not
                     * exist. Two silent wrong answers came out of it:
                     *   int x=0;      switch(x){case 4294967296L: ...}  never hit
                     *   unsigned x=~0u; switch(x){case -1: ...}         never hit
                     * The second is ordinary, fully-defined C that clang does not
                     * even warn about. Converting first also makes the duplicate
                     * scan below correct: `case 0` and `case 4294967296L` on an
                     * int switch are the same label and must be rejected. */
                    s->case_val = as_type(s->case_val, n->cond ? n->cond->type : NULL);
                    for (cc_node_t *c = n->cases; c; c = c->cases)
                        if (c->case_val == s->case_val) {
                            cc_errorf(s->tok, "this switch already has a "
                                              "case for %d", (int)s->case_val);
                            ok = 0; break;
                        }
                    if (!ok) break;
                    if (++ncase > CC_MAX_SWITCH_CASES) {
                        cc_errorf(s->tok, "a switch may have at most %d cases",
                                  CC_MAX_SWITCH_CASES);
                        ok = 0; break;
                    }
                    if (tailc) tailc->cases = s; else n->cases = s;
                    tailc = s;
                }
            }
            cc_node_t *kids[4] = { s->body, s->then, s->els, s->lhs };
            for (int k = 0; k < 4; k++) {
                if (!kids[k]) continue;
                if (kids[k]->kind != ND_BLOCK && kids[k]->kind != ND_CASE &&
                    kids[k]->kind != ND_IF && kids[k]->kind != ND_LOOP)
                    continue;
                if (sp >= (int)(sizeof stack / sizeof stack[0])) {
                    cc_errorf(s->tok, "this switch body is nested too deeply");
                    ok = 0; break;
                }
                stack[sp++] = kids[k];
            }
            if (!ok) break;
        }
    }
    return ok;
}

static cc_node_t *statement(void) {
    const cc_token_t *tok = tk();

    /* A new statement starts with an empty temporary pool: see CC_TEMP_SLOTS.
     * Sound because no expression in this subset contains a statement, so a
     * temporary can never be live across this point. */
    cc_ctx.ntemp = 0;

    if (at("{")) return compound();
    if (consume(";")) return node(ND_NOP, tok);

    if (eq(tok, "goto"))
        return cc_errorp(tok,
                         "goto is not supported: every loop back-edge carries the "
                         "fuel check that guarantees a compiled program "
                         "terminates, and a jump to a label could avoid it. Use "
                         "while, for, break or continue");

    if (eq(tok, "return")) {
        bump();
        cc_node_t *n = node(ND_RETURN, tok);
        if (!n) return NULL;
        cc_type_t *want = cc_ctx.cur ? cc_ctx.cur->type->base : cc_ty_int;
        if (at(";")) {
            if (want->kind != TY_VOID)
                return cc_errorp(tok, "'%s' returns '%s', so 'return' needs a value",
                                 cc_ctx.cur ? cc_ctx.cur->name : "this function",
                                 TN1(want));
            bump();
            return n;
        }
        cc_node_t *e = expr();
        if (!e) return NULL;
        if (want->kind == TY_VOID)
            return cc_errorp(tok, "'%s' returns void, so 'return' cannot have a "
                                  "value", cc_ctx.cur ? cc_ctx.cur->name : "this");
        cc_node_t *v = coerce(e, want, "the returned value", tok);
        if (!v) return NULL;
        n->lhs = v;
        if (!expect(";")) return NULL;
        return n;
    }

    if (eq(tok, "if")) {
        bump();
        DEEPER(NULL);
        cc_node_t *n = node(ND_IF, tok);
        int ok = n && expect("(");
        if (ok) {
            n->cond = want_scalar(expr(), "the condition of 'if'");
            ok = n->cond && expect(")");
        }
        if (ok) { n->then = statement(); ok = n->then != NULL; }
        if (ok && consume("else")) { n->els = statement(); ok = n->els != NULL; }
        SHALLOWER();
        return ok ? n : NULL;
    }

    if (eq(tok, "while")) {
        bump();
        DEEPER(NULL);
        cc_node_t *n = node(ND_LOOP, tok);
        int ok = n && expect("(");
        if (n) n->test_first = 1;
        if (ok) {
            n->cond = want_scalar(expr(), "the condition of 'while'");
            ok = n->cond && expect(")");
        }
        if (ok) {
            cc_ctx.loop_depth++;
            n->body = statement();
            cc_ctx.loop_depth--;
            ok = n->body != NULL;
        }
        SHALLOWER();
        return ok ? n : NULL;
    }

    if (eq(tok, "do")) {
        bump();
        DEEPER(NULL);
        cc_node_t *n = node(ND_LOOP, tok);
        int ok = (n != NULL);
        if (ok) {
            cc_ctx.loop_depth++;
            n->body = statement();
            cc_ctx.loop_depth--;
            ok = n->body != NULL;
        }
        if (ok) ok = expect("while") && expect("(");
        if (ok) {
            n->cond = want_scalar(expr(), "the condition of 'do while'");
            ok = n->cond && expect(")") && expect(";");
        }
        SHALLOWER();
        return ok ? n : NULL;
    }

    if (eq(tok, "for")) {
        bump();
        DEEPER(NULL);
        enter_scope();
        cc_node_t *n = node(ND_LOOP, tok);
        int ok = n && expect("(");
        if (n) n->test_first = 1;
        if (ok) {
            if (!at(";")) {
                if (is_typename(tk())) {
                    cc_node_t head; head.next = NULL;
                    cc_node_t *tail = &head;
                    ok = block_decl(&tail);
                    if (ok) {
                        cc_node_t *blk = node(ND_BLOCK, tok);
                        if (!blk) ok = 0;
                        else { blk->body = head.next; n->init = blk; }
                    }
                } else {
                    cc_node_t *e = expr();
                    ok = e && expect(";");
                    if (ok) n->init = expr_stmt_of(e, tok);
                    if (ok && !n->init) ok = 0;
                }
            } else bump();
        }
        if (ok && !at(";")) {
            n->cond = want_scalar(expr(), "the condition of 'for'");
            ok = n->cond != NULL;
        }
        if (ok) ok = expect(";");
        if (ok && !at(")")) {
            cc_node_t *e = expr();
            ok = e != NULL;
            if (ok) { n->post = expr_stmt_of(e, tok); ok = n->post != NULL; }
        }
        if (ok) ok = expect(")");
        if (ok) {
            cc_ctx.loop_depth++;
            n->body = statement();
            cc_ctx.loop_depth--;
            ok = n->body != NULL;
        }
        leave_scope();
        SHALLOWER();
        return ok ? n : NULL;
    }

    if (eq(tok, "switch")) {
        bump();
        DEEPER(NULL);
        cc_node_t *n = node(ND_SWITCH, tok);
        int ok = n && expect("(");
        if (ok) {
            cc_node_t *c = want_scalar(expr(), "the value of 'switch'");
            if (c && c->type->kind == TY_PTR) {
                cc_errorf(tok, "'switch' needs a whole number, not a pointer");
                c = NULL;
            }
            if (c) c = node_cast(c, common_type(c->type, cc_ty_int), tok);
            n->cond = c;
            ok = c && expect(")");
        }
        if (ok) {
            /* switch_depth, not loop_depth: `break` belongs to the switch but
             * `continue` still belongs to the enclosing loop, and conflating the
             * two makes `continue` inside a switch inside a loop jump to the
             * wrong place — silently. */
            cc_ctx.switch_depth++;
            n->body = compound();
            cc_ctx.switch_depth--;
            ok = n->body != NULL;
        }
        /* Collect the cases by walking the body: a case may sit at any depth of
         * block, and rejecting that would refuse ordinary code. Out of line
         * because its explicit walk stack is 1 KiB, and at -O0 a local lives in
         * its function's frame whether or not the branch holding it is taken —
         * so leaving it here cost 1 KiB on EVERY statement(), at every one of
         * CC_MAX_BLOCK_DEPTH levels, for a construct most functions never use. */
        if (ok) ok = collect_cases(n);
        SHALLOWER();
        return ok ? n : NULL;
    }

    if (eq(tok, "case") || eq(tok, "default")) {
        int is_default = eq(tok, "default");
        if (cc_ctx.switch_depth == 0)
            return cc_errorp(tok, "'%s' can only appear inside a switch", tok->text);
        bump();
        cc_node_t *n = node(ND_CASE, tok);
        if (!n) return NULL;
        n->label = -1;               /* the switch fills this; see cc_x64.c */
        if (is_default) n->default_case = n;
        else {
            int64_t v;
            if (!const_expr(&v)) return NULL;
            n->case_val = (uint64_t)v;
        }
        if (!expect(":")) return NULL;
        if (at("}")) { n->body = node(ND_NOP, tok); return n->body ? n : NULL; }
        n->body = statement();
        return n->body ? n : NULL;
    }

    if (eq(tok, "break") || eq(tok, "continue")) {
        int brk = eq(tok, "break");
        if (brk ? (cc_ctx.loop_depth == 0 && cc_ctx.switch_depth == 0)
                : (cc_ctx.loop_depth == 0))
            return cc_errorp(tok, "'%s' is only meaningful inside a loop%s",
                             tok->text, brk ? " or switch" : "");
        bump();
        cc_node_t *n = node(brk ? ND_BREAK : ND_CONTINUE, tok);
        if (!n || !expect(";")) return NULL;
        return n;
    }

    if (tk()->kind == TK_IDENT && eq(tk_at(1), ":"))
        return cc_errorp(tok, "labels are not supported (nor is goto): a compiled "
                              "program's loops must all carry a fuel check");

    {
        cc_node_t *e = expr();
        if (!e) return NULL;
        if (!expect(";")) return NULL;
        return expr_stmt_of(e, tok);
    }
}

/* ===================================================================== */
/* top level                                                             */
/* ===================================================================== */

static int same_func_type(cc_type_t *a, cc_type_t *b) {
    if (a->base->kind != b->base->kind || a->base->size != b->base->size) return 0;
    if (a->no_proto || b->no_proto) return 1;
    if (a->nparam != b->nparam) return 0;
    for (int i = 0; i < a->nparam; i++)
        if (a->param[i]->kind != b->param[i]->kind ||
            a->param[i]->size != b->param[i]->size) return 0;
    return 1;
}

static int function(cc_type_t *ty, const cc_token_t *nm, int is_definition_pos) {
    if (is_aggregate(ty->base))
        return cc_errorf(nm, "'%s' cannot return a %s by value; return a number "
                             "and write the result through a pointer parameter",
                         nm->text, ty->base->kind == TY_UNION ? "union" : "struct");
    if (ty->base->kind == TY_ARRAY)
        return cc_errorf(nm, "'%s' cannot return an array", nm->text);

    cc_sym_t *s = find_sym(nm->text);
    if (s && s->kind != SY_FUNC && s->kind != SY_EXTERN)
        return cc_errorf(nm, "'%s' is already declared as something that is not a "
                             "function", nm->text);
    if (s && !same_func_type(s->type, ty))
        return cc_errorf(nm, "'%s' is already declared with a different signature",
                         nm->text);
    if (!s) {
        s = push_sym(SY_FUNC, nm->text, ty);
        if (!s) return 0;
        s->func_index = -1;
    }
    if (!is_definition_pos) return 1;

    /* Order matters: every curated symbol is marked defined, so testing that
     * first reports "defined twice" for `void print(char *s) {}`, which sends the
     * reader looking for a second definition that does not exist. */
    if (s->kind == SY_EXTERN)
        return cc_errorf(nm, "'%s' is a kernel function this compiler provides; "
                             "choose another name", nm->text);
    if (s->defined)
        return cc_errorf(nm, "'%s' is defined twice", nm->text);
    if (cc_ctx.nfunc >= CC_MAX_FUNCS)
        return cc_errorf(nm, "a unit may define at most %d functions", CC_MAX_FUNCS);

    cc_func_t *f = &cc_ctx.func[cc_ctx.nfunc];
    memset(f, 0, sizeof *f);
    f->name = nm->text;
    f->type = ty;
    f->tok  = nm;
    s->func_index = cc_ctx.nfunc;
    s->defined = 1;
    s->type = ty;         /* the definition's prototype wins */
    cc_ctx.nfunc++;

    cc_ctx.cur = f;
    /* Locals start BELOW the fixed temporary region, so a temporary's rbp offset
     * is known the moment it is handed out (see new_temp) rather than after the
     * body has been parsed. */
    cc_ctx.cur_offset = -CC_TEMP_BYTES;
    cc_ctx.ntemp = 0;
    cc_ctx.temp_peak = 0;
    enter_scope();

    if (ty->no_proto) ty->nparam = 0;

    for (int i = 0; i < ty->nparam; i++) {
        const cc_token_t *pn = ty->pname[i];
        if (!pn) {
            cc_errorf(nm, "parameter %d of '%s' has no name; a definition must "
                          "name every parameter", i + 1, nm->text);
            leave_scope(); cc_ctx.cur = NULL; return 0;
        }
        if (find_sym_here(pn->text)) {
            cc_errorf(pn, "'%s' is used twice as a parameter name", pn->text);
            leave_scope(); cc_ctx.cur = NULL; return 0;
        }
        cc_sym_t *ps = new_local(pn->text, ty->param[i], pn);
        if (!ps) { leave_scope(); cc_ctx.cur = NULL; return 0; }
        f->param[i] = ps;
    }
    f->nparam = ty->nparam;

    f->body = compound();
    leave_scope();
    if (!f->body) { cc_ctx.cur = NULL; return 0; }

    int frame = -cc_ctx.cur_offset;
    f->frame_bytes = (frame + 15) / 16 * 16;
    cc_ctx.cur = NULL;
    return 1;
}

int cc_parse_unit(void) {
    g_tags = NULL;
    g_depth = 0;

    while (tk()->kind != TK_EOF) {
        if (cc_ctx.failed) return 0;

        int st = 0, ex = 0, td = 0;
        cc_type_t *base = declspec(&st, &ex, &td);
        if (!base) return 0;

        if (consume(";")) continue;               /* `struct s { ... };` */

        int was_definition = 0;
        for (int first = 1;; first = 0) {
            const cc_token_t *nm = NULL;
            cc_type_t *ty = declarator(base, &nm);
            if (!ty) return 0;
            if (!nm) return cc_errorf(tk(), "this declaration has no name");

            if (td) {
                cc_sym_t *e = find_sym_here(nm->text);
                if (e && !(e->kind == SY_TYPEDEF && e->type->kind == ty->kind &&
                           e->type->size == ty->size))
                    return cc_errorf(nm, "'%s' is already declared as something "
                                         "else", nm->text);
                if (!e && !push_sym(SY_TYPEDEF, nm->text, ty)) return 0;
                if (!consume(",")) break;
                continue;
            }

            if (ty->kind == TY_FUNC) {
                if (at("{")) {
                    if (!first)
                        return cc_errorf(tk(), "a function definition cannot follow "
                                               "a comma");
                    if (!function(ty, nm, 1)) return 0;
                    was_definition = 1;
                    break;
                }
                if (!function(ty, nm, 0)) return 0;
                if (!consume(",")) break;
                continue;
            }

            /* A file-scope object. */
            if (ex) {
                cc_sym_t *e = find_sym(nm->text);
                if (!e)
                    return cc_errorf(nm, "'%s' is declared extern, but this machine "
                                         "provides no such object. Compiled code "
                                         "may call the predeclared kernel "
                                         "functions; it cannot reach kernel "
                                         "variables", nm->text);
                if (!consume(",")) break;
                continue;
            }
            if (find_sym_here(nm->text))
                return cc_errorf(nm, "'%s' is already declared", nm->text);
            if (ty->kind == TY_ARRAY && ty->array_len < 0) {
                if (at("=") && tk_at(1)->kind == TK_STR)
                    ty = cc_array_of(ty->base, (int)tk_at(1)->len + 1);
                else
                    return cc_errorf(nm, "'%s' needs an array size", nm->text);
                if (!ty) return 0;
            }
            if (ty->kind == TY_VOID)
                return cc_errorf(nm, "'%s' cannot be void", nm->text);
            if (ty->size <= 0)
                return cc_errorf(nm, "'%s' has no size", nm->text);
            int off = data_alloc(ty->size, ty->align, nm);
            if (off < 0) return 0;
            cc_sym_t *s = push_sym(SY_GLOBAL, nm->text, ty);
            if (!s) return 0;
            s->offset = off;
            if (consume("=")) {
                if (!global_init(cc_ctx.data + off, ty, nm)) return 0;
            }
            if (!consume(",")) break;
        }
        /* A definition ends with its own '}' and takes no ';'. Everything else
         * does, and saying so here is the difference between one clear
         * diagnostic and a cascade. */
        if (was_definition) continue;
        if (!expect(";")) return 0;
    }

    if (cc_ctx.nfunc == 0)
        return cc_errorf(NULL, "this unit defines no functions");

    /* A call to a function that was declared and never defined cannot be
     * emitted, and finding that out at run time is not an option. */
    for (cc_sym_t *s = cc_ctx.scope; s; s = s->next)
        if (s->kind == SY_FUNC && !s->defined)
            return cc_errorf(NULL, "'%s' is declared but never defined in this "
                                   "unit; there is no linker here", s->name);

    return !cc_ctx.failed;
}
