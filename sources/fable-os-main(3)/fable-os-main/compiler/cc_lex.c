/* cc_lex.c — bytes to tokens, and the preprocessor.
 *
 * TWO STAGES, ONE FILE, because they share the token representation and nothing
 * else needs either of them.
 *
 * THE LEXER is ordinary except in one respect: it refuses input rather than
 * coping with it. A byte that is not printable ASCII, tab, newline or carriage
 * return is an error naming its value and position, because model output arrives
 * with embedded NULs and stray UTF-8 more often than it arrives with a stray
 * semicolon, and "unexpected token" thirty lines later is not a fixable message.
 * A number that looks like a float is refused AT THE LEXER, with the whole
 * literal quoted, for the same reason: `1.5` must not become `1`.
 *
 * THE PREPROCESSOR is a token-stream rewriter with an INPUT STACK. The main
 * stream is frame 0; expanding a macro pushes its replacement list as a new
 * frame, and tokens are read from the innermost frame until it is exhausted.
 * That gives two things for free:
 *
 *   - Nested expansion works without recursion in C: the substituted tokens are
 *     rescanned because they are simply the next thing the reader sees.
 *   - RECURSION IS BOUNDED EXACTLY. A frame stays open until it is consumed, so
 *     "is this macro currently expanding?" is "is its name on the frame stack?".
 *     #define A A, and #define A B with #define B A, both stop at the first
 *     re-entry and are reported by name rather than looping. The global
 *     expansion budget below is a second net, not the first one.
 *
 * Only frame 0 is scanned for directives: a '#' produced by a macro is not a
 * directive in this subset, and pretending otherwise is how a preprocessor grows
 * to ten thousand lines.
 *
 * WHAT IS DELIBERATELY ABSENT: #if / #elif (they need a constant-expression
 * evaluator over macro-expanded tokens, which is a second parser), # and ##,
 * variadic macros, #pragma, #line. Every one of them is refused by name, and the
 * name is in the message, because a model that is told "#if is not supported;
 * use #ifdef" fixes it in one turn.
 */

#include "cc_int.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================== */
/* keywords                                                              */
/* ===================================================================== */

/* Rejected constructs are keywords too. `float` must lex as a keyword so the
 * parser can say "floating point is not supported" instead of "unknown type
 * name 'float'", which reads like a typo. */
static const char *const kw[] = {
    "void", "char", "short", "int", "long", "signed", "unsigned",
    "struct", "union", "enum", "typedef", "sizeof",
    "if", "else", "while", "do", "for", "switch", "case", "default",
    "break", "continue", "return",
    "static", "extern", "const", "volatile", "register",
    "float", "double", "goto", "inline", "restrict",
    "_Bool", "_Complex", "_Static_assert", "_Generic", "auto",
    NULL
};

static int is_keyword(const char *s, size_t n) {
    for (int i = 0; kw[i]; i++)
        if (strlen(kw[i]) == n && memcmp(kw[i], s, n) == 0) return 1;
    return 0;
}

/* Longest first: '<<=' must not lex as '<' '<='. */
static const char *const punct[] = {
    "<<=", ">>=", "...",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##",
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "!", "<", ">", "=",
    "(", ")", "[", "]", "{", "}", ".", ",", ";", ":", "?", "#",
    NULL
};

static int is_ident1(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident2(int c) { return is_ident1(c) || (c >= '0' && c <= '9'); }
static int is_digit(int c)  { return c >= '0' && c <= '9'; }
static int is_hex(int c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hexval(int c) {
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/* ===================================================================== */
/* the two token arrays                                                  */
/* ===================================================================== */

/* Tokens do NOT come from the arena, for a reason worth stating: two streams are
 * live at once (the lexer's raw tokens and the preprocessor's output), a bump
 * allocator cannot give the first one back, and an #include adds a THIRD that
 * must coexist with the outer file's unread tokens. So there are exactly two
 * static arrays and the .bss arithmetic is explicit:
 * 2 x CC_TOKENS_MAX x sizeof(cc_token_t).
 *
 * THE RAW POOL is bump-allocated. Every file in a compilation — the unit and each
 * #include — takes a slice of it, and they all stay valid for the whole
 * preprocess, which is what makes splicing an include trivial: the outer frame
 * keeps pointing at its own slice. One consequence, and it is the right one:
 * CC_TOKENS_MAX bounds ALL the source in a unit AND its includes together, so
 * there is one number and one diagnostic rather than a per-file limit that
 * interacts with a global one. */
static cc_token_t g_raw[CC_TOKENS_MAX];
static int        g_raw_used;
static cc_token_t g_ppout[CC_TOKENS_MAX];

void cc_token_pool_reset(void) { g_raw_used = 0; }

/* ===================================================================== */
/* the lexer                                                             */
/* ===================================================================== */

typedef struct {
    const char *p, *end;
    const char *file;
    int         line, col;
    cc_token_t *out;
    int         n, cap;
} lexer_t;

/* A token used only to carry a position into cc_errorf before any token
 * exists. Static so nothing is allocated on an error path. */
static cc_token_t g_here;

static const cc_token_t *here(lexer_t *L) {
    memset(&g_here, 0, sizeof g_here);
    g_here.line = L->line;
    g_here.col  = L->col;
    g_here.file = L->file;
    g_here.text = "";
    return &g_here;
}

static void advance(lexer_t *L, int n) {
    for (int i = 0; i < n && L->p < L->end; i++, L->p++) {
        if (*L->p == '\n') { L->line++; L->col = 1; }
        else if (*L->p == '\t') L->col += 8 - ((L->col - 1) % 8);
        else L->col++;
    }
}

static cc_token_t *push(lexer_t *L, int kind, int line, int col) {
    if (L->n >= L->cap) {
        cc_errorf(here(L),
                  "this unit needs more than %d tokens; split it into saved "
                  "programs and call them", CC_TOKENS_MAX);
        return NULL;
    }
    cc_token_t *t = &L->out[L->n++];
    memset(t, 0, sizeof *t);
    t->kind = (uint8_t)kind;
    t->line = line;
    t->col  = col;
    t->file = L->file;
    t->text = "";
    return t;
}

/* Decode one escape sequence at L->p (which points just past the backslash).
 * Returns the value; consumes what it used. */
static int read_escape(lexer_t *L) {
    if (L->p >= L->end) return 0;
    int c = (unsigned char)*L->p;
    advance(L, 1);
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return '\0';
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case 'x': {
        if (L->p >= L->end || !is_hex((unsigned char)*L->p)) {
            cc_errorf(here(L), "\\x needs at least one hexadecimal digit");
            return 0;
        }
        int v = 0, n = 0;
        while (L->p < L->end && is_hex((unsigned char)*L->p) && n < 2) {
            v = v * 16 + hexval((unsigned char)*L->p);
            advance(L, 1);
            n++;
        }
        return v & 0xff;
    }
    default:
        cc_errorf(here(L), "unknown escape sequence \\%c", c);
        return c;
    }
}

/* A NUL-terminated copy of `n` bytes at `s`, in the arena. */
static const char *dup_n(const char *s, size_t n) { return cc_intern(s, n); }

int cc_tokenize(const char *src, size_t len, const char *file,
                cc_token_t **out) {
    lexer_t L;
    L.p    = src;
    L.end  = src + len;
    L.file = cc_intern(file, strlen(file));
    L.line = 1;
    L.col  = 1;
    L.out  = g_raw + g_raw_used;
    L.cap  = CC_TOKENS_MAX - g_raw_used;
    L.n    = 0;
    if (L.cap <= 1) {
        cc_errorf(NULL, "this unit and its #includes need more than %d tokens "
                        "together", CC_TOKENS_MAX);
        return -1;
    }

    int bol = 1;

    while (L.p < L.end) {
        int c = (unsigned char)*L.p;

        /* backslash-newline: a line continuation, invisible to everything above */
        if (c == '\\' && L.p + 1 < L.end &&
            (L.p[1] == '\n' || (L.p[1] == '\r' && L.p + 2 < L.end && L.p[2] == '\n'))) {
            advance(&L, L.p[1] == '\r' ? 3 : 2);
            continue;
        }
        if (c == '\n') { advance(&L, 1); bol = 1; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { advance(&L, 1); continue; }

        /* comments */
        if (c == '/' && L.p + 1 < L.end && L.p[1] == '/') {
            while (L.p < L.end && *L.p != '\n') advance(&L, 1);
            continue;
        }
        if (c == '/' && L.p + 1 < L.end && L.p[1] == '*') {
            int sl = L.line, sc = L.col;
            advance(&L, 2);
            int closed = 0;
            while (L.p + 1 < L.end) {
                if (L.p[0] == '*' && L.p[1] == '/') { advance(&L, 2); closed = 1; break; }
                advance(&L, 1);
            }
            if (!closed) {
                g_here.line = sl; g_here.col = sc; g_here.file = L.file;
                g_here.text = ""; g_here.kind = TK_PUNCT; g_here.len = 0;
                cc_errorf(&g_here, "this /* comment is never closed");
                return -1;
            }
            continue;
        }

        /* every other byte must be one this language can spell */
        if (c < 0x20 || c > 0x7e) {
            cc_errorf(here(&L),
                      "byte 0x%02x is not valid C source (only printable ASCII, "
                      "tab and newline are)", c);
            return -1;
        }

        int line = L.line, col = L.col;

        /* numbers */
        if (is_digit(c)) {
            const char *start = L.p;
            uint64_t v = 0;
            int base = 10;
            if (c == '0' && L.p + 1 < L.end && (L.p[1] == 'x' || L.p[1] == 'X')) {
                advance(&L, 2);
                base = 16;
                if (L.p >= L.end || !is_hex((unsigned char)*L.p)) {
                    cc_errorf(here(&L), "0x needs at least one hexadecimal digit");
                    return -1;
                }
                while (L.p < L.end && is_hex((unsigned char)*L.p)) {
                    v = v * 16 + (uint64_t)hexval((unsigned char)*L.p);
                    advance(&L, 1);
                }
            } else {
                if (c == '0') base = 8;
                while (L.p < L.end && is_digit((unsigned char)*L.p)) {
                    if (base == 8 && *L.p > '7') {
                        cc_errorf(here(&L),
                                  "'%c' is not an octal digit (a leading 0 makes "
                                  "this an octal literal)", *L.p);
                        return -1;
                    }
                    v = v * (uint64_t)base + (uint64_t)(*L.p - '0');
                    advance(&L, 1);
                }
            }
            /* Anything that would make this a floating literal: refuse it whole,
             * because silently truncating 1.5 to 1 is the exact failure this
             * compiler exists to not have. */
            if (L.p < L.end && (*L.p == '.' ||
                                ((*L.p == 'e' || *L.p == 'E') && base == 10) ||
                                ((*L.p == 'p' || *L.p == 'P') && base == 16))) {
                const char *fs = start;
                while (L.p < L.end &&
                       (is_ident2((unsigned char)*L.p) || *L.p == '.' ||
                        ((*L.p == '+' || *L.p == '-') &&
                         (L.p[-1] == 'e' || L.p[-1] == 'E' ||
                          L.p[-1] == 'p' || L.p[-1] == 'P'))))
                    advance(&L, 1);
                g_here.line = line; g_here.col = col; g_here.file = L.file;
                g_here.text = dup_n(fs, (size_t)(L.p - fs));
                g_here.len  = (uint32_t)(L.p - fs);
                g_here.kind = TK_NUM;
                cc_errorf(&g_here,
                          "floating point is not supported: '%s'. This kernel is "
                          "built -mno-sse and has no FPU state; use integers, or "
                          "scale by a power of ten", g_here.text);
                return -1;
            }
            int is_u = 0, is_l = 0;
            for (;;) {
                if (L.p < L.end && (*L.p == 'u' || *L.p == 'U')) { is_u = 1; advance(&L, 1); }
                else if (L.p < L.end && (*L.p == 'l' || *L.p == 'L')) { is_l = 1; advance(&L, 1); }
                else break;
            }
            if (L.p < L.end && is_ident1((unsigned char)*L.p)) {
                cc_errorf(here(&L),
                          "'%c' cannot follow a number; the only suffixes here "
                          "are u, l and combinations of them", *L.p);
                return -1;
            }
            cc_token_t *t = push(&L, TK_NUM, line, col);
            if (!t) return -1;
            t->bol = (uint8_t)bol; bol = 0;
            t->val = v;
            /* THE TYPE OF AN INTEGER CONSTANT, C11 6.4.4.1p5 AND ITS TABLE.
             *
             * The candidate list depends on the SUFFIX and on the BASE, and this
             * used to ignore both: is_long was `v > 0x7fffffff` regardless, so
             * every constant from 2^31 up became 64-bit. sizeof(0xFFFFFFFF) was
             * 8 instead of 4, `0xFFFFFFFFu + 1u` was 4294967296 instead of 0
             * (unsigned int arithmetic that never wrapped), `~0x80000000 ==
             * 0x7fffffff` was false, and `0xFFFFFFFF == -1` was false. The same
             * expression written through `unsigned int` variables was correct,
             * which is the worst shape this can take: a model that tests one
             * form and ships the other gets a different answer, silently. And
             * 0xFFFFFFFF and 0x80000000 are the two most common constants in the
             * register code this compiler exists for.
             *
             * The rule, with only int/unsigned/long/unsigned long available here
             * (long and long long are the same 64-bit type in this subset):
             *   suffixed u  : unsigned int, then unsigned long
             *   suffixed l  : long, then unsigned long (unsigned long only if
             *                 the value does not fit a signed long)
             *   suffixed ul : unsigned long
             *   decimal, no suffix : int, then long   — never unsigned int, so
             *                 4294967295 is a long, which is what C says and
             *                 what this already did correctly
             *   hex/octal, no suffix : int, unsigned int, long, unsigned long */
            {
                int wide, uns;
                if (is_u && is_l)      { wide = 1; uns = 1; }
                else if (is_u)         { wide = v > 0xffffffffull; uns = 1; }
                else if (is_l)         { wide = 1; uns = v > 0x7fffffffffffffffull; }
                else if (base == 10)   { wide = v > 0x7fffffffull;
                                         uns  = v > 0x7fffffffffffffffull; }
                else                   { /* hex or octal: unsigned is a candidate */
                                         wide = v > 0xffffffffull;
                                         uns  = wide ? (v > 0x7fffffffffffffffull)
                                                     : (v > 0x7fffffffull); }
                t->is_unsigned = (uint8_t)uns;
                t->is_long     = (uint8_t)wide;
            }
            t->text = dup_n(start, (size_t)(L.p - start));
            t->len  = (uint32_t)(L.p - start);
            continue;
        }

        /* identifiers and keywords */
        if (is_ident1(c)) {
            const char *start = L.p;
            while (L.p < L.end && is_ident2((unsigned char)*L.p)) advance(&L, 1);
            size_t n = (size_t)(L.p - start);
            if (n > 200) {
                cc_errorf(here(&L),
                          "this identifier is %d characters long; the limit is 200",
                          (int)n);
                return -1;
            }
            cc_token_t *t = push(&L, is_keyword(start, n) ? TK_KEYWORD : TK_IDENT,
                                 line, col);
            if (!t) return -1;
            t->bol = (uint8_t)bol; bol = 0;
            t->text = dup_n(start, n);
            t->len  = (uint32_t)n;
            continue;
        }

        /* string literals */
        if (c == '"') {
            advance(&L, 1);
            /* STATIC, NOT A LOCAL. CC_SRC_MAX is 16384, and a 16 KiB automatic
             * array here put a 16,648-byte frame on cc_tokenize() — a quarter of
             * the whole 64 KiB root stack, paid by EVERY compile whether or not
             * the source contains a string. The kernel is single-threaded and
             * this scan does not recurse or re-enter, so one buffer is enough. */
            static char buf[CC_SRC_MAX];
            size_t n = 0;
            for (;;) {
                if (L.p >= L.end || *L.p == '\n') {
                    g_here.line = line; g_here.col = col; g_here.file = L.file;
                    g_here.text = "\""; g_here.len = 1; g_here.kind = TK_STR;
                    cc_errorf(&g_here,
                              "this string literal is never closed (a \" must be "
                              "closed on the same line)");
                    return -1;
                }
                if (*L.p == '"') { advance(&L, 1); break; }
                int ch;
                if (*L.p == '\\') { advance(&L, 1); ch = read_escape(&L); }
                else { ch = (unsigned char)*L.p; advance(&L, 1); }
                if (n + 1 >= sizeof buf) {
                    cc_errorf(here(&L), "this string literal is too long");
                    return -1;
                }
                buf[n++] = (char)ch;
            }
            buf[n] = '\0';
            cc_token_t *t = push(&L, TK_STR, line, col);
            if (!t) return -1;
            t->bol  = (uint8_t)bol; bol = 0;
            t->text = dup_n(buf, n);      /* the DECODED bytes; see cc_int.h */
            t->len  = (uint32_t)n;
            continue;
        }

        /* character literals */
        if (c == '\'') {
            advance(&L, 1);
            if (L.p < L.end && *L.p == '\'') {
                cc_errorf(here(&L), "'' is empty; a character literal needs one character");
                return -1;
            }
            int ch;
            if (L.p < L.end && *L.p == '\\') { advance(&L, 1); ch = read_escape(&L); }
            else if (L.p < L.end) { ch = (unsigned char)*L.p; advance(&L, 1); }
            else { cc_errorf(here(&L), "this character literal is never closed"); return -1; }
            if (L.p >= L.end || *L.p != '\'') {
                cc_errorf(here(&L),
                          "a character literal holds exactly one character; for "
                          "text use \"double quotes\"");
                return -1;
            }
            advance(&L, 1);
            cc_token_t *t = push(&L, TK_CHAR, line, col);
            if (!t) return -1;
            t->bol = (uint8_t)bol; bol = 0;
            /* char is signed here, matching the -m64 SysV ABI. */
            t->val  = (uint64_t)(int64_t)(int8_t)ch;
            t->text = "character literal";
            t->len  = 17;
            continue;
        }

        /* punctuators */
        {
            int matched = 0;
            for (int i = 0; punct[i]; i++) {
                size_t n = strlen(punct[i]);
                if ((size_t)(L.end - L.p) >= n && memcmp(L.p, punct[i], n) == 0) {
                    cc_token_t *t = push(&L,
                                         (punct[i][0] == '#' && n == 1 && bol)
                                             ? TK_HASH : TK_PUNCT,
                                         line, col);
                    if (!t) return -1;
                    t->bol = (uint8_t)bol; bol = 0;
                    t->text = punct[i];
                    t->len  = (uint32_t)n;
                    advance(&L, (int)n);
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
        }

        cc_errorf(here(&L), "'%c' has no meaning in C", c);
        return -1;
    }

    cc_token_t *t = push(&L, TK_EOF, L.line, L.col);
    if (!t) return -1;
    t->bol  = 1;
    t->text = "end of file";
    t->len  = 11;

    *out = L.out;
    g_raw_used += L.n;
    return L.n;
}

/* ===================================================================== */
/* the preprocessor                                                      */
/* ===================================================================== */

typedef struct {
    const char *name;
    uint8_t     func_like;
    uint8_t     nparam;
    const char *param[CC_MAX_MACRO_PARAMS];
    cc_token_t *body;
    int         nbody;
} macro_t;

typedef struct {
    cc_token_t *t;
    int         n, i;
    const char *macro;     /* the macro this frame is expanding, or NULL */
} frame_t;

#define PP_FRAMES 16

typedef struct {
    macro_t   macro[CC_MAX_MACROS];
    int        nmacro;
    frame_t    fr[PP_FRAMES];
    int        sp;
    cc_token_t *out;
    int         nout, cap;
    /* #ifdef nesting: 1 = taking this branch, 0 = skipping it */
    uint8_t     cond[CC_MAX_BLOCK_DEPTH];
    uint8_t     cond_taken[CC_MAX_BLOCK_DEPTH];
    int         ncond;
    int         includes;
    int         inc_depth;
    uint32_t    expansions;
} pp_t;

/* Expansions in one unit. A budget, not the primary defence: the frame-stack
 * check below catches a self-referential macro immediately and by name. This
 * catches the pathological-but-finite case, e.g. 30 macros each doubling. */
#define PP_MAX_EXPANSIONS 200000u

static cc_read_fn g_reader = NULL;
void cc_set_include_reader(cc_read_fn fn) { g_reader = fn; }

static macro_t *find_macro(pp_t *P, const char *name, size_t n) {
    for (int i = 0; i < P->nmacro; i++)
        if (strlen(P->macro[i].name) == n && memcmp(P->macro[i].name, name, n) == 0)
            return &P->macro[i];
    return NULL;
}

static int expanding(pp_t *P, const char *name) {
    for (int i = 0; i <= P->sp; i++)
        if (P->fr[i].macro && strcmp(P->fr[i].macro, name) == 0) return 1;
    return 0;
}

static int emit(pp_t *P, const cc_token_t *t) {
    if (P->nout >= P->cap)
        return cc_errorf(t, "the preprocessor produced more than %d tokens; a "
                            "macro is probably expanding without bound",
                         CC_TOKENS_MAX);
    P->out[P->nout++] = *t;
    return 1;
}

/* Peek/get on the frame stack. Frame 0 is the main stream and never pops. */
static cc_token_t *pp_peek(pp_t *P) {
    while (P->sp > 0 && P->fr[P->sp].i >= P->fr[P->sp].n) P->sp--;
    frame_t *f = &P->fr[P->sp];
    if (f->i >= f->n) return &f->t[f->n - 1];    /* the EOF token */
    return &f->t[f->i];
}
static cc_token_t *pp_next(pp_t *P) {
    cc_token_t *t = pp_peek(P);
    frame_t *f = &P->fr[P->sp];
    if (f->i < f->n) f->i++;
    return t;
}

/* The token AFTER the current one, without consuming anything.
 *
 * This exists because the alternative — consume, look, and rewind on
 * disappointment — is not sound on a stack of frames: if the consumed token was
 * the last of its frame, pp_peek() has already popped that frame, and restoring
 * the saved (sp, i) makes the same token current again. The macro is then
 * re-examined, re-consumed, re-restored, for ever. A function-like macro
 * mentioned without parentheses at the end of a macro body is enough to reach
 * it, so the lookahead is done without mutation instead. */
static cc_token_t *pp_peek_ahead(pp_t *P) {
    pp_peek(P);                        /* normalise: pop exhausted frames */
    int sp = P->sp;
    int i  = P->fr[sp].i + 1;
    for (;;) {
        if (i < P->fr[sp].n) return &P->fr[sp].t[i];
        if (sp == 0) return &P->fr[0].t[P->fr[0].n - 1];
        sp--;
        i = P->fr[sp].i;
    }
}
static int at_eof(pp_t *P) {
    return P->sp == 0 && P->fr[0].i >= P->fr[0].n - 1;
}

static int tok_is(const cc_token_t *t, const char *s) {
    return t->len == strlen(s) && memcmp(t->text, s, t->len) == 0;
}

/* Read a directive's tokens up to the end of its line, from frame 0 only. */
static int read_line(pp_t *P, cc_token_t **out, int *n) {
    frame_t *f = &P->fr[0];
    int start = f->i;
    while (f->i < f->n - 1 && !f->t[f->i].bol) f->i++;
    *out = &f->t[start];
    *n   = f->i - start;
    return 1;
}

static int def_macro(pp_t *P, cc_token_t *line, int n) {
    if (n < 1) return cc_errorf(line, "#define needs a name");
    if (line[0].kind != TK_IDENT && line[0].kind != TK_KEYWORD)
        return cc_errorf(&line[0], "#define needs an identifier, not '%s'", line[0].text);
    if (line[0].kind == TK_KEYWORD)
        return cc_errorf(&line[0], "'%s' is a keyword and cannot be #defined", line[0].text);

    macro_t *m = find_macro(P, line[0].text, line[0].len);
    if (!m) {
        if (P->nmacro >= CC_MAX_MACROS)
            return cc_errorf(&line[0], "more than %d macros are defined", CC_MAX_MACROS);
        m = &P->macro[P->nmacro++];
    }
    memset(m, 0, sizeof *m);
    m->name = line[0].text;

    int i = 1;
    /* Function-like only when '(' touches the name: #define A (x) is an
     * object-like macro whose body is "(x)", and getting that wrong changes the
     * meaning of ordinary code. */
    if (i < n && line[i].kind == TK_PUNCT && tok_is(&line[i], "(") &&
        line[i].line == line[0].line &&
        line[i].col == line[0].col + (int)line[0].len) {
        m->func_like = 1;
        i++;
        if (i < n && tok_is(&line[i], ")")) i++;
        else for (;;) {
            if (i >= n) return cc_errorf(&line[0], "#define %s( has no closing ')'", m->name);
            if (tok_is(&line[i], "..."))
                return cc_errorf(&line[i],
                                 "variadic macros are not supported; give %s a "
                                 "fixed number of parameters", m->name);
            if (line[i].kind != TK_IDENT)
                return cc_errorf(&line[i], "'%s' is not a macro parameter name", line[i].text);
            if (m->nparam >= CC_MAX_MACRO_PARAMS)
                return cc_errorf(&line[i], "a macro may take at most %d parameters",
                                 CC_MAX_MACRO_PARAMS);
            m->param[m->nparam++] = line[i].text;
            i++;
            if (i < n && tok_is(&line[i], ",")) { i++; continue; }
            if (i < n && tok_is(&line[i], ")")) { i++; break; }
            return cc_errorf(&line[i < n ? i : n - 1],
                             "expected ',' or ')' in the parameter list of %s", m->name);
        }
    }

    for (int k = i; k < n; k++)
        if (line[k].kind == TK_PUNCT && (tok_is(&line[k], "#") || tok_is(&line[k], "##")))
            return cc_errorf(&line[k],
                             "'%s' in a macro body is not supported (no "
                             "stringizing and no token pasting)", line[k].text);

    m->nbody = n - i;
    if (m->nbody > 0) {
        m->body = cc_alloc((size_t)m->nbody * sizeof(cc_token_t));
        if (!m->body) return 0;
        memcpy(m->body, &line[i], (size_t)m->nbody * sizeof(cc_token_t));
    }
    return 1;
}

/* Substitute `args` into `m`'s body, producing a fresh token array. */
static int substitute(macro_t *m, cc_token_t **argv, int *argn,
                      const cc_token_t *site, cc_token_t **out, int *nout) {
    int total = 0;
    for (int i = 0; i < m->nbody; i++) {
        int pi = -1;
        if (m->body[i].kind == TK_IDENT)
            for (int k = 0; k < m->nparam; k++)
                if (strcmp(m->param[k], m->body[i].text) == 0) { pi = k; break; }
        total += (pi >= 0) ? argn[pi] : 1;
    }
    if (total > CC_TOKENS_MAX)
        return cc_errorf(site, "expanding %s produces too many tokens", m->name);

    cc_token_t *buf = total ? cc_alloc((size_t)total * sizeof(cc_token_t)) : NULL;
    if (total && !buf) return 0;

    int o = 0;
    for (int i = 0; i < m->nbody; i++) {
        int pi = -1;
        if (m->body[i].kind == TK_IDENT)
            for (int k = 0; k < m->nparam; k++)
                if (strcmp(m->param[k], m->body[i].text) == 0) { pi = k; break; }
        if (pi >= 0) {
            for (int j = 0; j < argn[pi]; j++) {
                buf[o] = argv[pi][j];
                /* Report an error inside an expansion at the CALL SITE: the
                 * model wrote that line, not the macro body's line. */
                buf[o].line = site->line;
                buf[o].col  = site->col;
                buf[o].file = site->file;
                buf[o].bol  = 0;
                o++;
            }
        } else {
            buf[o] = m->body[i];
            buf[o].line = site->line;
            buf[o].col  = site->col;
            buf[o].file = site->file;
            buf[o].bol  = 0;
            o++;
        }
    }
    *out  = buf;
    *nout = o;
    return 1;
}

/* Collect the arguments of a function-like invocation. The '(' has not been
 * consumed yet. */
static int read_args(pp_t *P, macro_t *m, const cc_token_t *site,
                     cc_token_t **argv, int *argn) {
    pp_next(P);                                    /* '(' */
    static cc_token_t argbuf[CC_MAX_MACRO_PARAMS][32];
    int count = 0;
    int depth = 0;
    int n = 0;

    if (tok_is(pp_peek(P), ")") ) { pp_next(P); count = 0; goto done; }

    for (;;) {
        cc_token_t *t = pp_peek(P);
        if (t->kind == TK_EOF)
            return cc_errorf(site, "the call to macro %s has no closing ')'", m->name);
        if (depth == 0 && (tok_is(t, ",") || tok_is(t, ")"))) {
            if (count >= CC_MAX_MACRO_PARAMS)
                return cc_errorf(site, "too many arguments to macro %s", m->name);
            argv[count] = argbuf[count];
            argn[count] = n;
            count++;
            n = 0;
            int closing = tok_is(t, ")");
            pp_next(P);
            if (closing) break;
            continue;
        }
        if (tok_is(t, "(")) depth++;
        else if (tok_is(t, ")")) depth--;
        if (count >= CC_MAX_MACRO_PARAMS)
            return cc_errorf(site, "too many arguments to macro %s", m->name);
        if (n >= 32)
            return cc_errorf(site, "argument %d of macro %s is longer than 32 "
                             "tokens", count + 1, m->name);
        argbuf[count][n++] = *t;
        pp_next(P);
    }
done:
    if (count != m->nparam)
        return cc_errorf(site,
                         "macro %s takes %d argument(s) but %d were given",
                         m->name, m->nparam, count);
    return 1;
}

static int do_include(pp_t *P, cc_token_t *line, int n);

static int directive(pp_t *P) {
    cc_token_t *hash = pp_next(P);                 /* the '#' */
    cc_token_t *line; int n;
    read_line(P, &line, &n);
    if (n == 0) return 1;                          /* a bare '#' is a null directive */

    const cc_token_t *d = &line[0];
    int skipping = 0;
    for (int i = 0; i < P->ncond; i++) if (!P->cond[i]) skipping = 1;

    if (tok_is(d, "ifdef") || tok_is(d, "ifndef")) {
        int want = tok_is(d, "ifdef");
        if (P->ncond >= CC_MAX_BLOCK_DEPTH)
            return cc_errorf(d, "#ifdef nested more than %d deep", CC_MAX_BLOCK_DEPTH);
        int on = 0;
        if (n < 2 || (line[1].kind != TK_IDENT && line[1].kind != TK_KEYWORD))
            return cc_errorf(d, "#%s needs a macro name", d->text);
        if (!skipping)
            on = (find_macro(P, line[1].text, line[1].len) != NULL) == want;
        P->cond[P->ncond] = (uint8_t)on;
        P->cond_taken[P->ncond] = (uint8_t)on;
        P->ncond++;
        return 1;
    }
    if (tok_is(d, "else")) {
        if (P->ncond == 0) return cc_errorf(d, "#else without a matching #ifdef");
        P->cond[P->ncond - 1] = (uint8_t)(!P->cond_taken[P->ncond - 1]);
        P->cond_taken[P->ncond - 1] = 1;
        return 1;
    }
    if (tok_is(d, "endif")) {
        if (P->ncond == 0) return cc_errorf(d, "#endif without a matching #ifdef");
        P->ncond--;
        return 1;
    }
    if (skipping) return 1;

    if (tok_is(d, "define")) return def_macro(P, line + 1, n - 1);
    if (tok_is(d, "undef")) {
        if (n < 2) return cc_errorf(d, "#undef needs a name");
        macro_t *m = find_macro(P, line[1].text, line[1].len);
        if (m) {
            int idx = (int)(m - P->macro);
            P->macro[idx] = P->macro[--P->nmacro];
        }
        return 1;
    }
    if (tok_is(d, "include")) return do_include(P, line + 1, n - 1);

    if (tok_is(d, "if") || tok_is(d, "elif"))
        return cc_errorf(d,
                         "#%s is not supported: there is no constant-expression "
                         "evaluator in this preprocessor. Use #ifdef / #ifndef",
                         d->text);
    if (tok_is(d, "pragma") || tok_is(d, "line") || tok_is(d, "error") ||
        tok_is(d, "warning"))
        return cc_errorf(d, "#%s is not supported", d->text);

    (void)hash;
    return cc_errorf(d, "'#%s' is not a directive this compiler knows", d->text);
}

static int pp_run(pp_t *P);

static int do_include(pp_t *P, cc_token_t *line, int n) {
    char name[CC_FILE_MAX];
    name[0] = '\0';

    if (n >= 1 && line[0].kind == TK_STR) {
        if (line[0].len == 0 || line[0].len >= sizeof name)
            return cc_errorf(&line[0], "#include \"...\" needs a name of 1..%d characters",
                             (int)sizeof name - 1);
        memcpy(name, line[0].text, line[0].len);
        name[line[0].len] = '\0';
    } else if (n >= 2 && tok_is(&line[0], "<")) {
        size_t o = 0;
        int closed = 0;
        for (int i = 1; i < n; i++) {
            if (tok_is(&line[i], ">")) { closed = 1; break; }
            if (o + line[i].len >= sizeof name)
                return cc_errorf(&line[0], "#include <...> name is too long");
            memcpy(name + o, line[i].text, line[i].len);
            o += line[i].len;
        }
        name[o] = '\0';
        if (!closed) return cc_errorf(&line[0], "#include <...> has no closing '>'");
    } else {
        return cc_errorf(n ? &line[0] : NULL,
                         "#include needs \"a-name.h\" or <a-name.h>");
    }

    if (P->includes >= CC_INCLUDES_MAX)
        return cc_errorf(&line[0], "more than %d #includes in one unit", CC_INCLUDES_MAX);
    if (P->inc_depth >= CC_INCLUDE_DEPTH)
        return cc_errorf(&line[0],
                         "#include nested more than %d deep (is \"%s\" including "
                         "itself?)", CC_INCLUDE_DEPTH, name);

    const char *text = cc_builtin_header(name);
    char *buf = NULL;
    if (!text) {
        /* A hosted libc header is the single most likely wrong guess, so it gets
         * its own sentence naming what this machine has instead. */
        static const char *const hosted[] = {
            "stdio.h", "stdlib.h", "math.h", "assert.h", "errno.h", "time.h",
            "ctype.h", "limits.h", "unistd.h", NULL };
        for (int i = 0; hosted[i]; i++)
            if (strcmp(hosted[i], name) == 0)
                return cc_errorf(&line[0],
                                 "<%s> does not exist: this is a kernel with no "
                                 "hosted libc. Nothing needs including - print, "
                                 "print_num, print_hex, print_char, time_ms, "
                                 "strlen, memcpy, memset, memcmp, file_read, "
                                 "file_write, file_size and the uint*_t types are "
                                 "all declared already", name);
        if (!g_reader)
            return cc_errorf(&line[0],
                             "\"%s\" cannot be read: this build has no include "
                             "root", name);
        buf = cc_alloc(CC_SRC_MAX + 1);
        if (!buf) return 0;
        int got = g_reader(name, buf, CC_SRC_MAX + 1);
        if (got < 0)
            return cc_errorf(&line[0],
                             "\"%s\" is not in the include root; #include here "
                             "reaches built-in headers and files saved there, "
                             "nothing else", name);
        buf[got] = '\0';
        text = buf;
    }

    cc_token_t *sub = NULL;
    int nsub = cc_tokenize(text, strlen(text), name, &sub);
    if (nsub < 0) return 0;

    /* Splice by running the same preprocessor over the included stream: an
     * included header's #defines must be visible after it, which is the entire
     * point of a header, so the macro table and the output are SHARED and only
     * the reader's position is saved.
     *
     * Deliberately not `pp_t inner = *P`: pp_t carries the macro table and is
     * ~20 KiB, and four nested includes of a copy is most of the kernel's 64 KiB
     * stack. Save the four fields that are per-file and nothing else. */
    frame_t  save_fr0   = P->fr[0];
    int      save_sp    = P->sp;
    int      save_ncond = P->ncond;
    uint8_t  save_cond[CC_MAX_BLOCK_DEPTH], save_taken[CC_MAX_BLOCK_DEPTH];
    memcpy(save_cond,  P->cond,       sizeof save_cond);
    memcpy(save_taken, P->cond_taken, sizeof save_taken);

    P->fr[0].t = sub;
    P->fr[0].n = nsub;
    P->fr[0].i = 0;
    P->fr[0].macro = NULL;
    P->sp = 0;
    P->ncond = 0;
    P->inc_depth++;
    P->includes++;

    int ok = pp_run(P);

    if (ok && P->ncond != 0)
        ok = cc_errorf(&sub[nsub - 1],
                       "\"%s\" leaves %d #ifdef(s) unclosed", name, P->ncond);

    P->inc_depth--;
    P->fr[0] = save_fr0;
    P->sp    = save_sp;
    P->ncond = save_ncond;
    memcpy(P->cond,       save_cond,  sizeof save_cond);
    memcpy(P->cond_taken, save_taken, sizeof save_taken);
    return ok;
}

static int pp_run(pp_t *P) {
    for (;;) {
        if (cc_ctx.failed) return 0;

        if (P->sp == 0 && P->fr[0].i < P->fr[0].n) {
            cc_token_t *t = &P->fr[0].t[P->fr[0].i];
            if (t->kind == TK_HASH) { if (!directive(P)) return 0; continue; }
        }

        int skipping = 0;
        for (int i = 0; i < P->ncond; i++) if (!P->cond[i]) skipping = 1;
        if (skipping && P->sp == 0) {
            if (at_eof(P)) break;
            P->fr[0].i++;
            continue;
        }

        if (at_eof(P) && P->sp == 0) break;

        cc_token_t *t = pp_peek(P);
        if (t->kind == TK_EOF && P->sp == 0) break;

        if (t->kind == TK_IDENT) {
            macro_t *m = find_macro(P, t->text, t->len);
            if (m && !expanding(P, m->name)) {
                cc_token_t site = *t;
                if (m->func_like) {
                    /* Not an invocation unless a '(' follows; a bare mention of a
                     * function-like macro's name is just an identifier. */
                    if (tok_is(pp_peek_ahead(P), "(")) {
                        pp_next(P);                       /* the name */
                        cc_token_t *argv[CC_MAX_MACRO_PARAMS];
                        int argn[CC_MAX_MACRO_PARAMS];
                        for (int i = 0; i < CC_MAX_MACRO_PARAMS; i++) { argv[i] = NULL; argn[i] = 0; }
                        if (!read_args(P, m, &site, argv, argn)) return 0;
                        cc_token_t *body; int nbody;
                        if (!substitute(m, argv, argn, &site, &body, &nbody)) return 0;
                        if (++P->expansions > PP_MAX_EXPANSIONS)
                            return cc_errorf(&site,
                                             "macro expansion has not terminated "
                                             "after %u substitutions",
                                             PP_MAX_EXPANSIONS);
                        if (nbody == 0) continue;
                        if (P->sp + 1 >= PP_FRAMES)
                            return cc_errorf(&site,
                                             "macros nested more than %d deep at %s",
                                             PP_FRAMES, m->name);
                        P->sp++;
                        P->fr[P->sp].t = body;
                        P->fr[P->sp].n = nbody;
                        P->fr[P->sp].i = 0;
                        P->fr[P->sp].macro = m->name;
                        continue;
                    }
                } else {
                    pp_next(P);
                    if (++P->expansions > PP_MAX_EXPANSIONS)
                        return cc_errorf(&site,
                                         "macro expansion has not terminated after "
                                         "%u substitutions", PP_MAX_EXPANSIONS);
                    if (m->nbody == 0) continue;
                    if (P->sp + 1 >= PP_FRAMES)
                        return cc_errorf(&site, "macros nested more than %d deep at %s",
                                         PP_FRAMES, m->name);
                    cc_token_t *body = cc_alloc((size_t)m->nbody * sizeof(cc_token_t));
                    if (!body) return 0;
                    for (int i = 0; i < m->nbody; i++) {
                        body[i] = m->body[i];
                        body[i].line = site.line;
                        body[i].col  = site.col;
                        body[i].file = site.file;
                        body[i].bol  = 0;
                    }
                    P->sp++;
                    P->fr[P->sp].t = body;
                    P->fr[P->sp].n = m->nbody;
                    P->fr[P->sp].i = 0;
                    P->fr[P->sp].macro = m->name;
                    continue;
                }
            }
        }

        t = pp_next(P);
        if (t->kind == TK_EOF) break;
        if (t->kind == TK_HASH) {
            /* A '#' that reached here came from a macro body or mid-line. */
            return cc_errorf(t, "'#' is only a directive at the start of a line");
        }
        if (!emit(P, t)) return 0;
    }
    return 1;
}

int cc_preprocess(cc_token_t *in, int nin, cc_token_t **out, int *nout) {
    static pp_t P;                 /* the macro table; one compile at a time */
    memset(&P, 0, sizeof P);
    P.cap = CC_TOKENS_MAX;
    P.out = g_ppout;
    P.fr[0].t = in;
    P.fr[0].n = nin;
    P.fr[0].i = 0;

    if (!pp_run(&P)) return 0;
    if (P.ncond != 0)
        return cc_errorf(&in[nin - 1], "%d #ifdef(s) are never closed with #endif",
                         P.ncond);

    cc_token_t eof;
    memset(&eof, 0, sizeof eof);
    eof.kind = TK_EOF;
    eof.line = in[nin - 1].line;
    eof.col  = in[nin - 1].col;
    eof.file = in[nin - 1].file;
    eof.text = "end of file";
    eof.len  = 11;
    if (!emit(&P, &eof)) return 0;

    *out  = P.out;
    *nout = P.nout;
    return 1;
}

/* ===================================================================== */
/* built-in headers                                                      */
/* ===================================================================== */

/* <fableos.h> exists so that a model which habitually writes an #include finds
 * one, and so that the curated kernel declarations are READABLE from inside a
 * program. It declares nothing that is not already in scope: cc_declare_symbols()
 * predeclares every one of these before the unit is parsed, and a redeclaration
 * that matches is accepted. That is deliberate — the failure mode of "you must
 * include the right header" is a wasted turn. */
static const char *const HDR_FABLEOS =
    "/* fableos.h - what a compiled program may call. All of this is already\n"
    "   declared before your first line; this header exists to be read. */\n"
    "#ifndef FABLEOS_H\n"
    "#define FABLEOS_H\n"
    "typedef unsigned char  uint8_t;\n"
    "typedef unsigned short uint16_t;\n"
    "typedef unsigned int   uint32_t;\n"
    "typedef unsigned long  uint64_t;\n"
    "typedef signed char    int8_t;\n"
    "typedef short          int16_t;\n"
    "typedef int            int32_t;\n"
    "typedef long           int64_t;\n"
    "typedef unsigned long  size_t;\n"
    "#endif\n";

static const char *const HDR_EMPTY =
    "/* provided by the compiler itself; nothing to declare */\n";

const char *cc_builtin_header(const char *name) {
    if (strcmp(name, "fableos.h") == 0) return HDR_FABLEOS;
    /* stdint/stddef are habits, not dependencies: the types are predeclared. */
    if (strcmp(name, "stdint.h") == 0 || strcmp(name, "stddef.h") == 0 ||
        strcmp(name, "stdbool.h") == 0 || strcmp(name, "string.h") == 0)
        return HDR_EMPTY;
    return NULL;
}
