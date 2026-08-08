/* cc_int.h — internals shared by the four halves of the compiler.
 *
 * Not a public interface: include/cc.h is. This holds the token, the type, the
 * AST node and the emitter state, plus the arena and the diagnostic sink, which
 * every stage needs and nothing outside compiler/ should see.
 *
 * THE PIPELINE, and where each piece lives
 *     cc_lex.c     bytes  -> tokens          (comments, literals, positions)
 *                  tokens -> tokens          (#define / #include / #ifdef)
 *     cc_parse.c   tokens -> types + AST     (declarations, statements,
 *                                             expressions, and every semantic
 *                                             refusal: this is where the subset
 *                                             is actually defined)
 *     cc_x64.c     AST    -> machine code    (syntax-directed, everything
 *                                             through rax, no optimiser)
 *     cc.c         the driver, the arena, the diagnostic sink, cc_run's
 *                  trampoline entry and the fuel/stack runtime block
 *
 * ONE ERROR MODEL, used everywhere: cc_errorf() records a diagnostic against a
 * token and returns 0/NULL so the caller can unwind by returning. There is no
 * setjmp and no panic path: a stage that fails records why and stops, and
 * cc_ctx.failed is what every loop tests. That is why a malformed unit produces
 * a diagnostic rather than a fault however deep the confusion goes.
 */
#ifndef CC_INT_H
#define CC_INT_H

#include <stdint.h>
#include <stddef.h>

#include "cc.h"

/* ===================================================================== */
/* tokens                                                                */
/* ===================================================================== */

typedef enum {
    TK_EOF = 0,
    TK_IDENT,
    TK_KEYWORD,
    TK_PUNCT,
    TK_NUM,
    TK_STR,          /* string literal; `str`/`strlen` hold the decoded bytes */
    TK_CHAR,         /* character literal, value in `val`                     */
    TK_HASH          /* a '#' in column one; only the preprocessor sees these */
} cc_tokkind_t;

/* 40 bytes, and that is a budget rather than an accident: two arrays of
 * CC_TOKENS_MAX of these are static (see cc_lex.c), so every field costs 480 KiB
 * of .bss per 8 bytes. There is deliberately no separate decoded-string field —
 * for TK_STR, `text`/`len` ARE the decoded bytes, NUL-terminated like every other
 * spelling — and no macro-hygiene field, because the preprocessor's frame stack
 * answers "is this macro expanding?" exactly. */
typedef struct cc_token {
    uint8_t   kind;
    uint8_t   bol;          /* first token on its line (for '#' detection)   */
    uint8_t   is_unsigned;  /* TK_NUM: had a u suffix or does not fit signed  */
    uint8_t   is_long;      /* TK_NUM: had an l suffix                       */
    int32_t   line, col;
    uint32_t  len;          /* bytes of `text`, excluding the NUL            */
    uint32_t  pad_;
    const char *file;       /* interned; never freed                         */
    const char *text;       /* interned spelling, always NUL-terminated. For  */
                            /* TK_STR this is the DECODED text.               */
    uint64_t  val;          /* TK_NUM / TK_CHAR                              */
} cc_token_t;

/* ===================================================================== */
/* types                                                                 */
/* ===================================================================== */

typedef enum {
    TY_VOID = 0, TY_CHAR, TY_SHORT, TY_INT, TY_LONG,
    TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_FUNC
} cc_tykind_t;

typedef struct cc_member cc_member_t;

typedef struct cc_type {
    uint8_t  kind;
    uint8_t  is_unsigned;
    int      size;             /* sizeof; -1 while a struct is incomplete    */
    int      align;
    struct cc_type *base;      /* PTR/ARRAY element, FUNC return type        */
    int      array_len;
    /* struct / union */
    const char  *tag;
    cc_member_t *members;
    int          nmembers;
    /* function */
    struct cc_type *param[CC_MAX_PARAMS];
    /* The parameter NAME tokens, when this type came from a declarator that had
     * them. A prototype need not name anything, so these may be NULL; a
     * definition's own declarator always fills them, which is how the body's
     * scope learns the names without re-walking the token stream. */
    const struct cc_token *pname[CC_MAX_PARAMS];
    int      nparam;
    uint8_t  no_proto;         /* declared as f() — arity unchecked, warned   */
} cc_type_t;

struct cc_member {
    const char     *name;
    cc_type_t      *type;
    int             offset;
};

/* ===================================================================== */
/* symbols                                                               */
/* ===================================================================== */

typedef enum {
    SY_LOCAL = 1,     /* rbp-relative                                        */
    SY_GLOBAL,        /* an offset into the unit's data image                */
    SY_FUNC,          /* a function defined in this unit                     */
    SY_EXTERN,        /* a curated kernel symbol; `addr` is its address      */
    SY_ENUMCONST,     /* a constant; `val`                                   */
    SY_TYPEDEF        /* a name for a type                                   */
} cc_symkind_t;

typedef struct cc_sym {
    uint8_t     kind;
    const char *name;
    cc_type_t  *type;
    int         offset;        /* SY_LOCAL: rbp-relative (negative)          */
                               /* SY_GLOBAL: data image offset               */
    int64_t     val;           /* SY_ENUMCONST                              */
    void       *addr;          /* SY_EXTERN                                  */
    int         func_index;    /* SY_FUNC: index into cc_ctx.func[]          */
    uint8_t     defined;       /* SY_FUNC: a body was seen                   */
    int         scope_depth;
    struct cc_sym *next;       /* scope chain, innermost first               */
} cc_sym_t;

/* ===================================================================== */
/* AST                                                                   */
/* ===================================================================== */

typedef enum {
    ND_NUM = 1, ND_VAR, ND_MEMBER, ND_DEREF, ND_ADDR, ND_ASSIGN, ND_CAST,
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_SHL, ND_SHR,
    ND_BITAND, ND_BITOR, ND_BITXOR, ND_BITNOT,
    ND_EQ, ND_NE, ND_LT, ND_LE, ND_NOT, ND_LOGAND, ND_LOGOR, ND_COND,
    ND_COMMA, ND_CALL, ND_STR,
    ND_RETURN, ND_IF, ND_LOOP, ND_BLOCK, ND_EXPR_STMT, ND_BREAK, ND_CONTINUE,
    ND_SWITCH, ND_CASE, ND_NOP,
    /* There is deliberately no ND_POSTINC: `a++` is built as (a += 1) - 1 with a
     * hidden pointer temporary, so ++ and -- and every compound assignment share
     * one code path and one set of type rules. See compound_assign(). */
    ND_MEMZERO,        /* zero `type->size` bytes at `sym` (initialiser tails) */
    ND_COPY            /* struct assignment: copy type->size bytes           */
} cc_nodekind_t;

typedef struct cc_node cc_node_t;
struct cc_node {
    uint8_t    kind;
    cc_type_t *type;
    const cc_token_t *tok;      /* for diagnostics; never NULL after parsing  */

    cc_node_t *lhs, *rhs;
    cc_node_t *next;            /* statement / argument list                 */

    /* ND_NUM */
    uint64_t   val;

    /* ND_VAR / ND_MEMBER */
    cc_sym_t     *sym;
    cc_member_t  *member;

    /* ND_IF / ND_LOOP / ND_COND */
    cc_node_t *cond, *then, *els, *init, *post, *body;

    /* ND_LOOP: a `for`/`while` tests before the body, a `do` after it        */
    uint8_t    test_first;

    /* ND_CALL */
    cc_sym_t  *callee;
    cc_node_t *args;
    int        nargs;

    /* ND_STR: an offset into the data image, and its length                  */
    int        data_off;
    int        data_len;

    /* ND_SWITCH / ND_CASE */
    cc_node_t *cases;           /* ND_CASE list, in source order             */
    cc_node_t *default_case;
    uint64_t   case_val;
    int        label;           /* filled by the code generator              */
};

/* One function to emit. */
typedef struct cc_func {
    const char *name;
    cc_type_t  *type;
    cc_sym_t   *param[CC_MAX_PARAMS];
    int         nparam;
    cc_node_t  *body;
    int         frame_bytes;    /* locals, 16-aligned                        */
    int         code_off;       /* filled by the code generator              */
    const cc_token_t *tok;
} cc_func_t;

/* ===================================================================== */
/* the compilation context — one, static, reset per compile              */
/* ===================================================================== */

typedef struct cc_ctx {
    /* input */
    const char *unit;

    /* tokens */
    cc_token_t *tok;            /* the post-preprocessor array               */
    int         ntok;
    int         pos;            /* the parser's cursor                       */

    /* declarations */
    cc_func_t   func[CC_MAX_FUNCS];
    int         nfunc;
    cc_sym_t   *scope;          /* innermost-first chain                     */
    int         scope_depth;

    /* the unit's data image: globals then string literals                   */
    uint8_t    *data;
    int         data_len;

    /* the function being parsed */
    cc_func_t  *cur;
    int         cur_offset;     /* next local's rbp offset (negative)        */
    int         ntemp;          /* compiler temporaries used by THIS statement */
    int         temp_peak;      /* the most any one statement needed          */
    int         loop_depth;
    int         switch_depth;

    /* diagnostics */
    int         failed;
    uint32_t    ndiag;
    cc_diag_t   diag[CC_DIAG_MAX];

    /* the original source, kept for the snippet a diagnostic shows           */
    const char *src;
    size_t      src_len;
} cc_ctx_t;

extern cc_ctx_t cc_ctx;

/* ===================================================================== */
/* arena, interning, diagnostics — cc.c                                  */
/* ===================================================================== */

void  cc_arena_reset(void);
void *cc_alloc(size_t n);             /* zeroed; NULL and a diagnostic when full */
const char *cc_intern(const char *s, size_t n);   /* NUL-terminated copy      */

/* Record a diagnostic against `tok` (NULL means "no position") and set failed.
 * Always returns 0, so `return cc_errorf(...)` is the unwind idiom. */
int cc_errorf(const cc_token_t *tok, const char *fmt, ...);

/* The same, returning NULL, for the many functions that return a pointer. */
void *cc_errorp(const cc_token_t *tok, const char *fmt, ...);

/* ---- the compiler's own stack floor ------------------------------------
 *
 * See "THE COMPILER'S OWN STACK" in include/cc.h for why this exists. Set by
 * cc_compile() from its own frame; every recursive production must test it.
 *
 * cc_stack_low() is the raw predicate (no diagnostic, no side effect), for the
 * one caller that wants to test without recording. cc_stack_exhausted() records
 * the diagnostic and returns 1, so the idiom at a recursion site is
 *
 *     if (cc_stack_exhausted(tok)) return 0;
 *
 * The measurement is __builtin_frame_address(0) compared against a floor. It is
 * only ever COMPARED, never dereferenced. Stacks grow down on every target this
 * compiler runs on and the host suite runs the same code, so a host test
 * exercises the same arithmetic. */
extern const unsigned char *cc_stack_floor_addr;

int cc_stack_low(void);
int cc_stack_exhausted(const cc_token_t *tok);

/* Recompute the floor from the caller's frame. Called once, by cc_compile(). */
void cc_stack_floor_arm(const void *frame);

/* ===================================================================== */
/* the lexer and the preprocessor — cc_lex.c                             */
/* ===================================================================== */

/* Tokenize `src` into a fresh array. Returns the count, or -1 with a
 * diagnostic. Tokens are arena-allocated. */
int cc_tokenize(const char *src, size_t len, const char *file,
                cc_token_t **out);

/* Run the preprocessor over `in`/`nin`, producing `out`/`nout`. */
int cc_preprocess(cc_token_t *in, int nin, cc_token_t **out, int *nout);

/* Reset the raw token pool. Called once per compile, before the unit is
 * tokenized; see the pool's comment in cc_lex.c. */
void cc_token_pool_reset(void);

/* The built-in headers, so #include <fableos.h> resolves with no filesystem. */
const char *cc_builtin_header(const char *name);

/* ===================================================================== */
/* the parser — cc_parse.c                                               */
/* ===================================================================== */

/* Parse cc_ctx.tok into cc_ctx.func[] and the data image. 0 on failure. */
int cc_parse_unit(void);

/* The base types, shared and never modified. */
extern cc_type_t *cc_ty_void, *cc_ty_char, *cc_ty_uchar, *cc_ty_short,
                 *cc_ty_ushort, *cc_ty_int, *cc_ty_uint, *cc_ty_long,
                 *cc_ty_ulong;

cc_type_t *cc_pointer_to(cc_type_t *base);
cc_type_t *cc_array_of(cc_type_t *base, int len);
int        cc_is_integer(const cc_type_t *t);
int        cc_is_pointerish(const cc_type_t *t);
/* "int *", "struct point", "unsigned long [4]" — for diagnostics. */
const char *cc_type_name(const cc_type_t *t, char *dst, size_t cap);

/* ===================================================================== */
/* the code generator — cc_x64.c                                         */
/* ===================================================================== */

/* Emit every function in cc_ctx into `code`. On success *out_len is the byte
 * count and *entry_off is the offset of the entry trampoline (always 0).
 * Returns 0 and records a diagnostic on overflow. */
int cc_codegen(uint8_t *code, size_t cap, size_t *out_len);

/* ===================================================================== */
/* the runtime block generated code reads through r15 — cc.c             */
/* ===================================================================== */

/* Layout is FIXED and the code generator encodes these offsets as displacements,
 * so the _Static_asserts in cc_x64.c pin them. Do not reorder. */
typedef struct cc_rt {
    int64_t  fuel;          /* +0   decremented at every back-edge and call  */
    uint64_t stack_limit;   /* +8   compiled rsp may not go below this       */
    uint64_t kernel_rsp;    /* +16  saved by the trampoline; the abort target */
    uint64_t prog_rsp;      /* +24  the top of the compiled program's stack  */
    uint32_t aborted;       /* +32  1 = fuel, 2 = stack                      */
    uint32_t pad;
} cc_rt_t;

#define CC_RT_FUEL        0
#define CC_RT_STACKLIM    8
#define CC_RT_KRSP       16
#define CC_RT_PRSP       24
#define CC_RT_ABORTED    32

extern cc_rt_t cc_rt;

/* The abort codes the stub writes into cc_rt.aborted. */
#define CC_ABORT_FUEL   1
#define CC_ABORT_STACK  2

/* ===================================================================== */
/* the curated symbol table — cc_sym.c                                   */
/* ===================================================================== */

/* Declare every curated symbol into the current (global) scope. Called once per
 * compile, before the unit is parsed, so a program needs no #include to call
 * one and cannot shadow one with a different signature without being told. */
int cc_declare_symbols(void);

/* Reset the runtime counters and the output capture. */
void cc_rt_reset(void);
size_t cc_rt_output(char *dst, size_t cap);
uint32_t cc_rt_calls(void);

/* ===================================================================== */
/* what cc.c exposes to cc_store.c                                       */
/* ===================================================================== */

int            cc_entry_arity(void);   /* -1 when there is no image        */
int            cc_have_image(void);
size_t         cc_image_len(void);
const uint8_t *cc_image(void);
uint64_t       cc_compile_serial(void);

#endif /* CC_INT_H */
