/* expr.c — the app language: values, fixed-point arithmetic, and the compiler
 *          and evaluator for one expression.
 *
 * PURPOSE
 *   A declarative document can lay out a calculator but it cannot BE one: a
 *   calculator needs digits accumulated into a display, an operator remembered, a
 *   running total, and an evaluation that copes with 1/0. This file is the whole
 *   of that "real logic" half of an app document (include/app.h, DECISION 1), and
 *   it is deliberately the smaller and more boring half.
 *
 *   Nothing here touches a window, a pixel, the console or the heap. The only
 *   external type it reads is gui_widget_t's text, through a const gui_window_t —
 *   which is why every calculator behaviour this machine claims (division by
 *   zero, twelve-digit caps, overflow, formatting) is a host unit test against a
 *   struct rather than a screenshot.
 *
 *   TWO THINGS ARE NOT PURE, and both are held at arm's length. `now`, `clock`
 *   and their siblings read a snapshot that runtime.c took when the event began,
 *   and rand() draws from the machine's entropy source, both through the two
 *   function calls declared at the end of app_impl.h. So this file still contains
 *   no state and touches no hardware directly, and a host test can say what time
 *   it is and what the dice rolled — see app_set_time_source().
 *
 * ARITHMETIC WITHOUT AN FPU
 *   The kernel is built -mno-sse -mno-mmx and has no FPU state to save, so every
 *   number is an int64_t scaled by APP_NUM_SCALE (1e6): six decimal places, and a
 *   magnitude limit of APP_NUM_MAX (9e9) checked after every operation. Multiply
 *   and divide go through __int128 (libgcc supplies the helpers; mbedTLS already
 *   depends on them) so an intermediate cannot wrap before the range check sees
 *   it. There is no path from any input to a wrapped number: the answer is either
 *   in range or it is ERR.
 *
 * THE COMPILER IS WHERE THE BOUNDS ARE PROVED
 *   Recursive descent over a bounded source string, emitting postfix ops. It
 *   resolves every identifier to an index while it goes, tracks the evaluation
 *   stack's high-water mark and refuses an expression that would need more than
 *   APP_STACK slots, and bounds its own recursion with APP_PAREN_DEPTH. So the
 *   evaluator below is a flat loop with no recursion, no allocation and no way to
 *   overflow anything — every check it would otherwise need has already happened.
 *
 * PRECEDENCE, lowest first: || && (== !=) (< <= > >=) (+ -) (* / %) unary(- !)
 *   Left-associative. && and || do NOT short-circuit, and they do not need to:
 *   an expression has no side effects and no operation can fail, so evaluating
 *   both sides is observably identical and the evaluator stays a straight loop.
 */

#include "app_impl.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>       /* vsnprintf; port/stdio.h when freestanding */

/* ====================================================================== */
/* tiny string helpers                                                    */
/* ====================================================================== */

/* Hand-rolled rather than <string.h>: this file compiles freestanding into the
 * kernel and natively for the host tests, and the shapes needed are three. */
static size_t a_len(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void a_cpy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (src) for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int a_eq(const char *a, const char *b) {
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && a[i] == b[i]; i++) { }
    return a[i] == '\0' && b[i] == '\0';
}

/* ====================================================================== */
/* numbers                                                                */
/* ====================================================================== */

/* Accepts optional surrounding spaces, an optional sign, digits, an optional
 * '.', digits. Everything else — an empty string, "1.2.3", "12x", an exponent —
 * is refused, because a calculator that silently reads "12x" as 12 is worse than
 * one that says "error". Digits past the sixth decimal place are truncated
 * toward zero, which is the scale's documented resolution. */
int app_num_parse(const char *s, int64_t *out) {
    int64_t whole = 0, frac = 0, scale = APP_NUM_SCALE;
    int     neg = 0, seen = 0;

    if (!s || !out) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    while (*s >= '0' && *s <= '9') {
        if (whole > APP_NUM_MAX / 10) return -1;
        whole = whole * 10 + (*s - '0');
        if (whole > APP_NUM_MAX / APP_NUM_SCALE) return -1;
        s++;
        seen = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            scale /= 10;
            if (scale > 0) frac += (int64_t)(*s - '0') * scale;
            s++;
            seen = 1;
        }
    }
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '\0' || !seen) return -1;

    int64_t v = whole * APP_NUM_SCALE + frac;
    *out = neg ? -v : v;
    return 0;
}

/* Six decimals with trailing zeros trimmed, and the point dropped when the
 * fraction is empty. Negation happens in unsigned space because -INT64_MIN is
 * undefined and a calculator is eventually handed the extreme. */
void app_num_format(int64_t v, char *dst, size_t cap) {
    char     tmp[24];
    char     fbuf[8];
    int      n = 0, fn = 0;
    int      neg = v < 0;
    uint64_t mag = neg ? (~(uint64_t)v + 1u) : (uint64_t)v;
    uint64_t whole = mag / (uint64_t)APP_NUM_SCALE;
    uint64_t frac  = mag % (uint64_t)APP_NUM_SCALE;
    size_t   o = 0;

    if (!dst || cap == 0) return;

    for (int i = 5; i >= 0; i--) {
        uint64_t d = frac;
        for (int k = 0; k < i; k++) d /= 10;
        fbuf[fn++] = (char)('0' + (int)(d % 10));
    }
    while (fn > 0 && fbuf[fn - 1] == '0') fn--;

    if (whole == 0) tmp[n++] = '0';
    while (whole > 0 && n < (int)sizeof tmp) {
        tmp[n++] = (char)('0' + (int)(whole % 10));
        whole /= 10;
    }

    if (neg && o + 1 < cap) dst[o++] = '-';
    for (int i = n - 1; i >= 0 && o + 1 < cap; i--) dst[o++] = tmp[i];
    if (fn > 0 && o + 1 < cap) {
        dst[o++] = '.';
        for (int i = 0; i < fn && o + 1 < cap; i++) dst[o++] = fbuf[i];
    }
    dst[o] = '\0';
}

/* ====================================================================== */
/* values                                                                 */
/* ====================================================================== */

app_val_t app_val_num(int64_t scaled) {
    app_val_t v;
    v.kind = AV_NUM;
    v.num  = scaled;
    v.str[0] = '\0';
    return v;
}

app_val_t app_val_str(const char *s) {
    app_val_t v;
    v.kind = AV_STR;
    v.num  = 0;
    a_cpy(v.str, sizeof v.str, s);
    return v;
}

app_val_t app_val_err(void) {
    app_val_t v;
    v.kind = AV_ERR;
    v.num  = 0;
    v.str[0] = '\0';
    app_note_err_value();
    return v;
}

/* The one place a value becomes text. "error" for ERR is not a placeholder: it
 * is what a calculator display must say, and it is why the shipped calculator
 * needs no error handling of its own beyond latching. */
const char *app_val_text(const app_val_t *v, char *dst, size_t cap) {
    if (!dst || cap == 0) return "";
    dst[0] = '\0';
    if (!v) return dst;
    if (v->kind == AV_STR)      a_cpy(dst, cap, v->str);
    else if (v->kind == AV_NUM) app_num_format(v->num, dst, cap);
    else                        a_cpy(dst, cap, "error");
    return dst;
}

/* 1 true, 0 false, -1 for ERR — the caller decides what an error condition
 * means (statement execution treats it as false and records a note). */
int app_val_truthy(const app_val_t *v) {
    if (!v) return 0;
    if (v->kind == AV_ERR) return -1;
    if (v->kind == AV_NUM) return v->num != 0;
    return v->str[0] != '\0';
}

/* ====================================================================== */
/* the function table                                                     */
/* ====================================================================== */

static const struct { const char *name; uint8_t op; uint8_t arity; } fns[] = {
    { "num",    AO_NUM,    1 },
    { "text",   AO_TEXT,   1 },
    { "cat",    AO_CAT,    2 },
    { "len",    AO_LEN,    1 },
    { "digits", AO_DIGITS, 1 },
    { "has",    AO_HAS,    2 },
    { "iserr",  AO_ISERR,  1 },
    { "abs",    AO_ABS,    1 },
    { "min",    AO_MIN,    2 },
    { "max",    AO_MAX,    2 },
    { "round",  AO_ROUND,  1 },
    { "rand",   AO_RAND,   1 },
    { "at",     AO_AT,     2 },
};
#define NFNS ((int)(sizeof fns / sizeof fns[0]))

/* The time snapshot, as leaves rather than calls: `clock` reads like a value
 * because it IS one for the whole of one event (app_env_now() in app_impl.h). */
static const struct { const char *name; uint8_t field; } times[] = {
    { "now",    ATF_NOW    },
    { "clock",  ATF_CLOCK  },
    { "today",  ATF_TODAY  },
    { "hour",   ATF_HOUR   },
    { "minute", ATF_MINUTE },
    { "second", ATF_SECOND },
};
#define NTIMES ((int)(sizeof times / sizeof times[0]))

/* ====================================================================== */
/* the compiler                                                           */
/* ====================================================================== */

typedef struct {
    const char   *s;
    size_t        n;
    size_t        pos;
    app_inst_t   *in;
    app_error_t  *err;
    int           cur, max;      /* evaluation stack simulation            */
    int           depth;         /* parenthesis / call recursion           */
} cx_t;

static int fail(cx_t *c, size_t at, const char *fmt, ...) {
    if (c->err) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err->msg, sizeof c->err->msg, fmt, ap);
        va_end(ap);
        c->err->offset = (int)at;
    }
    return -1;
}

static void skip_ws(cx_t *c) {
    while (c->pos < c->n && (c->s[c->pos] == ' ' || c->s[c->pos] == '\t')) c->pos++;
}

static int at_end(cx_t *c) { skip_ws(c); return c->pos >= c->n; }

/* Emit one op, maintaining the stack simulation. `push` is the net effect on the
 * stack: +1 for a leaf, -1 for a binary operator, 0 for a unary one. */
static int emit(cx_t *c, uint8_t op, uint16_t arg, int push) {
    app_inst_t *in = c->in;
    if (in->nop >= APP_MAX_OPS)
        return fail(c, c->pos, "the app's expressions are too large in total "
                              "(limit %d operations across every handler)",
                    APP_MAX_OPS);
    in->op[in->nop].op   = op;
    in->op[in->nop].pad_ = 0;
    in->op[in->nop].arg  = arg;
    in->nop++;
    c->cur += push;
    if (c->cur > c->max) c->max = c->cur;
    if (c->max > APP_STACK)
        return fail(c, c->pos, "expression is too deeply nested to evaluate "
                              "(needs more than %d stack slots)", APP_STACK);
    return 0;
}

static int intern_num(cx_t *c, int64_t v, uint16_t *out) {
    app_inst_t *in = c->in;
    for (uint16_t i = 0; i < in->nkonst; i++)
        if (in->konst[i] == v) { *out = i; return 0; }
    if (in->nkonst >= APP_MAX_CONSTS)
        return fail(c, c->pos, "too many distinct numbers in this app (limit %d)",
                    APP_MAX_CONSTS);
    in->konst[in->nkonst] = v;
    *out = in->nkonst++;
    return 0;
}

static int intern_str(cx_t *c, const char *s, size_t len, uint16_t *out) {
    app_inst_t *in = c->in;
    if (len >= APP_TEXT_MAX)
        return fail(c, c->pos, "string literal is %d bytes; the limit is %d",
                    (int)len, APP_TEXT_MAX - 1);
    if ((size_t)in->strused + len + 1 > APP_STRPOOL)
        return fail(c, c->pos, "too much literal text in this app (limit %d bytes)",
                    APP_STRPOOL);
    uint16_t off = in->strused;
    for (size_t i = 0; i < len; i++) in->strpool[off + i] = s[i];
    in->strpool[off + len] = '\0';
    in->strused = (uint16_t)(off + len + 1);
    *out = off;
    return 0;
}

static int is_name_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
static int is_name_char(char ch) {
    return is_name_start(ch) || (ch >= '0' && ch <= '9');
}

/* Append " sep name" without ever writing past cap. Hand-rolled rather than
 * snprintf-accumulating, because snprintf returns the length it WANTED and
 * adding that to an offset is how a truncating list walks off the end. */
static void list_add(char *dst, size_t cap, size_t *o, const char *sep,
                     const char *name) {
    size_t need = a_len(name) + (*o ? a_len(sep) : 0);
    if (*o + need + 1 >= cap) return;
    if (*o) { a_cpy(dst + *o, cap - *o, sep); *o += a_len(sep); }
    a_cpy(dst + *o, cap - *o, name);
    *o += a_len(name);
}

/* What the known names are, for an error message that lets the model fix a typo
 * in one turn instead of guessing. Bounded by the message buffer. */
static void put_known_names(cx_t *c, char *dst, size_t cap) {
    app_inst_t *in = c->in;
    size_t o = 0;
    if (!cap) return;
    dst[0] = '\0';
    for (uint16_t i = 0; i < in->nvar; i++)
        list_add(dst, cap, &o, ", ", in->vname[i]);
    for (uint16_t i = 0; i < in->nwidget; i++)
        if (in->wname[i][0]) list_add(dst, cap, &o, ", ", in->wname[i]);
    list_add(dst, cap, &o, ", ", "key");
    for (int i = 0; i < NTIMES; i++) list_add(dst, cap, &o, ", ", times[i].name);
}

static int parse_expr(cx_t *c, int prec);

/* number | 'string' | name | name(args) | (expr) | -x | !x */
static int parse_primary(cx_t *c) {
    skip_ws(c);
    if (c->pos >= c->n) return fail(c, c->pos, "expression ends early; a value "
                                               "was expected here");
    size_t start = c->pos;
    char   ch    = c->s[c->pos];

    if (ch == '(') {
        if (++c->depth > APP_PAREN_DEPTH)
            return fail(c, start, "too many nested parentheses (limit %d)",
                        APP_PAREN_DEPTH);
        c->pos++;
        if (parse_expr(c, 0) != 0) return -1;
        skip_ws(c);
        if (c->pos >= c->n || c->s[c->pos] != ')')
            return fail(c, c->pos, "expected ')' to close the '(' at offset %d",
                        (int)start);
        c->pos++;
        c->depth--;
        return 0;
    }
    if (ch == '-' || ch == '!') {
        /* Bounded like every other recursion here: "!!!!!!..." must be refused
         * with a message, not by running out of stack in the compiler. */
        if (++c->depth > APP_PAREN_DEPTH)
            return fail(c, start, "too many stacked unary operators (limit %d)",
                        APP_PAREN_DEPTH);
        c->pos++;
        if (parse_primary(c) != 0) return -1;
        c->depth--;
        return emit(c, ch == '-' ? AO_NEG : AO_NOT, 0, 0);
    }
    if (ch == '\'' || ch == '"') {
        char  q = ch;
        size_t b = ++c->pos;
        while (c->pos < c->n && c->s[c->pos] != q) c->pos++;
        if (c->pos >= c->n)
            return fail(c, start, "unterminated string literal (no closing %c)", q);
        size_t   len = c->pos - b;
        uint16_t off = 0;
        c->pos++;                                   /* past the close quote */
        if (intern_str(c, c->s + b, len, &off) != 0) return -1;
        return emit(c, AO_STR, off, +1);
    }
    if (ch >= '0' && ch <= '9') {
        char buf[32];
        size_t o = 0;
        while (c->pos < c->n && ((c->s[c->pos] >= '0' && c->s[c->pos] <= '9') ||
                                  c->s[c->pos] == '.')) {
            if (o + 1 < sizeof buf) buf[o++] = c->s[c->pos];
            c->pos++;
        }
        buf[o] = '\0';
        int64_t v = 0;
        if (app_num_parse(buf, &v) != 0)
            return fail(c, start, "\"%s\" is not a number this machine can hold "
                                  "(6 decimal places, magnitude up to 9000000000)",
                        buf);
        uint16_t k = 0;
        if (intern_num(c, v, &k) != 0) return -1;
        return emit(c, AO_CONST, k, +1);
    }
    if (is_name_start(ch)) {
        char name[APP_NAME_MAX + 8];
        size_t o = 0;
        while (c->pos < c->n && is_name_char(c->s[c->pos])) {
            if (o + 1 < sizeof name) name[o++] = c->s[c->pos];
            c->pos++;
        }
        name[o] = '\0';

        skip_ws(c);
        int is_call = (c->pos < c->n && c->s[c->pos] == '(');

        if (is_call) {
            for (int i = 0; i < NFNS; i++) {
                if (!a_eq(name, fns[i].name)) continue;
                if (++c->depth > APP_PAREN_DEPTH)
                    return fail(c, start, "too many nested calls (limit %d)",
                                APP_PAREN_DEPTH);
                c->pos++;                                    /* past '('    */
                for (int k = 0; k < fns[i].arity; k++) {
                    if (k > 0) {
                        skip_ws(c);
                        if (c->pos >= c->n || c->s[c->pos] != ',')
                            return fail(c, c->pos,
                                        "%s() takes %d arguments; expected a "
                                        "',' here", name, (int)fns[i].arity);
                        c->pos++;
                    }
                    if (parse_expr(c, 0) != 0) return -1;
                }
                skip_ws(c);
                if (c->pos >= c->n || c->s[c->pos] != ')')
                    return fail(c, c->pos, "%s() takes exactly %d argument(s); "
                                           "expected ')' here",
                                name, (int)fns[i].arity);
                c->pos++;
                c->depth--;
                return emit(c, fns[i].op, 0, -(fns[i].arity - 1));
            }
            char   known[96];
            size_t o2 = 0;
            known[0] = '\0';
            for (int i = 0; i < NFNS; i++) list_add(known, sizeof known, &o2, " ",
                                                    fns[i].name);
            return fail(c, start, "unknown function \"%s\". Available: %s",
                        name, known);
        }

        if (a_eq(name, "key")) return emit(c, AO_KEY, 0, +1);
        for (int i = 0; i < NTIMES; i++)
            if (a_eq(name, times[i].name))
                return emit(c, AO_TIME, times[i].field, +1);

        for (uint16_t i = 0; i < c->in->nvar; i++)
            if (a_eq(name, c->in->vname[i])) return emit(c, AO_VAR, i, +1);
        for (uint16_t i = 0; i < c->in->nwidget; i++)
            if (c->in->wname[i][0] && a_eq(name, c->in->wname[i]))
                return emit(c, AO_WIDGET, i, +1);

        char known[128];
        put_known_names(c, known, sizeof known);
        return fail(c, start, "unknown name \"%s\". Known names here: %s "
                              "(declare a var, or give a widget a \"name\")",
                    name, known);
    }
    return fail(c, start, "unexpected character '%c' in an expression", ch);
}

/* Binary operator table: token, op, precedence. Longest token first so that
 * "==" is never read as "=" and "<=" never as "<". */
static const struct { const char *tok; uint8_t op; int prec; } bins[] = {
    { "||", AO_OR,  1 },
    { "&&", AO_AND, 2 },
    { "==", AO_EQ,  3 },
    { "!=", AO_NE,  3 },
    { "<=", AO_LE,  4 },
    { ">=", AO_GE,  4 },
    { "<",  AO_LT,  4 },
    { ">",  AO_GT,  4 },
    { "+",  AO_ADD, 5 },
    { "-",  AO_SUB, 5 },
    { "*",  AO_MUL, 6 },
    { "/",  AO_DIV, 6 },
    { "%",  AO_MOD, 6 },
};
#define NBINS ((int)(sizeof bins / sizeof bins[0]))

static int parse_expr(cx_t *c, int prec) {
    if (parse_primary(c) != 0) return -1;
    for (;;) {
        skip_ws(c);
        if (c->pos >= c->n) return 0;
        int found = -1;
        for (int i = 0; i < NBINS; i++) {
            size_t len = a_len(bins[i].tok);
            if (c->pos + len > c->n) continue;
            size_t k = 0;
            while (k < len && c->s[c->pos + k] == bins[i].tok[k]) k++;
            if (k == len) { found = i; break; }
        }
        if (found < 0) return 0;
        if (bins[found].prec <= prec) return 0;
        c->pos += a_len(bins[found].tok);
        if (parse_expr(c, bins[found].prec) != 0) return -1;
        if (emit(c, bins[found].op, 0, -1) != 0) return -1;
    }
}

int app_expr_compile(app_inst_t *in, const char *src, uint16_t *out_index,
                     app_error_t *err) {
    cx_t c;
    if (!in || !src || !out_index) return APP_EINVAL;
    if (in->nexpr >= APP_MAX_EXPRS) {
        if (err) {
            snprintf(err->msg, sizeof err->msg,
                     "too many expressions in this app (limit %d)", APP_MAX_EXPRS);
            err->offset = -1;
        }
        return APP_ENOSPC;
    }

    c.s = src; c.n = a_len(src); c.pos = 0; c.in = in; c.err = err;
    c.cur = 0; c.max = 0; c.depth = 0;

    uint16_t first = in->nop;
    if (c.n == 0) {
        if (err) {
            a_cpy(err->msg, sizeof err->msg, "expression is empty");
            err->offset = 0;
        }
        return APP_EINVAL;
    }
    if (parse_expr(&c, 0) != 0) return APP_EINVAL;
    if (!at_end(&c)) {
        (void)fail(&c, c.pos, "unexpected trailing text in the expression "
                              "(starting at offset %d)", (int)c.pos);
        return APP_EINVAL;
    }
    if (c.cur != 1) {
        (void)fail(&c, 0, "the expression does not produce exactly one value");
        return APP_EINVAL;
    }

    in->expr[in->nexpr].first = first;
    in->expr[in->nexpr].count = (uint16_t)(in->nop - first);
    in->expr[in->nexpr].depth = (uint8_t)c.max;
    *out_index = in->nexpr++;
    return APP_OK;
}

/* ====================================================================== */
/* the evaluator                                                          */
/* ====================================================================== */

static int64_t clamp_ok(__int128 r, int *ok) {
    if (r > (__int128)APP_NUM_MAX || r < -(__int128)APP_NUM_MAX) { *ok = 0; return 0; }
    *ok = 1;
    return (int64_t)r;
}

static app_val_t arith(uint8_t op, const app_val_t *a, const app_val_t *b) {
    int ok = 1;
    if (a->kind != AV_NUM || b->kind != AV_NUM) return app_val_err();
    __int128 r;
    switch (op) {
    case AO_ADD: r = (__int128)a->num + b->num; break;
    case AO_SUB: r = (__int128)a->num - b->num; break;
    case AO_MUL: r = ((__int128)a->num * b->num) / APP_NUM_SCALE; break;
    case AO_DIV:
        if (b->num == 0) return app_val_err();
        r = ((__int128)a->num * APP_NUM_SCALE) / b->num;
        break;
    case AO_MOD:
        if (b->num == 0) return app_val_err();
        r = (__int128)a->num % b->num;
        break;
    default:     return app_val_err();
    }
    int64_t v = clamp_ok(r, &ok);
    return ok ? app_val_num(v) : app_val_err();
}

/* -2 = incomparable (an ERR operand, or ordering two different kinds).
 * Otherwise the sign of a - b. Equality across kinds is defined as "unequal"
 * rather than as an error, so `op == ''` on a numeric var is false, not ERR. */
static int compare(const app_val_t *a, const app_val_t *b, int ordering) {
    if (a->kind == AV_ERR || b->kind == AV_ERR) return -2;
    if (a->kind != b->kind) return ordering ? -2 : 2;
    if (a->kind == AV_NUM)
        return a->num < b->num ? -1 : (a->num > b->num ? 1 : 0);
    /* Byte-wise, unsigned: a total order that needs no locale and no table. */
    for (size_t i = 0; ; i++) {
        unsigned char x = (unsigned char)a->str[i], y = (unsigned char)b->str[i];
        if (x != y) return x < y ? -1 : 1;
        if (x == '\0') return 0;
    }
}

/* ---- the time snapshot, rendered ---------------------------------------
 * The wall clock arrives from a function pointer whose implementation may be a
 * CMOS chip, so it is treated as untrusted input like everything else here: a
 * reading outside the calendar is the ERR value, not a string with garbage in it.
 * The formats are fixed-width on purpose — "9:5:3" is not a clock, and the app
 * language has no padding operator, which is exactly why `clock` and `today` are
 * pre-rendered rather than left to the document to assemble. */
static void put2(char *d, int v) {
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + v % 10);
}

static int wall_sane(const app_wall_t *w) {
    return w->year >= 1970 && w->year <= 9999 &&
           w->month >= 1 && w->month <= 12 && w->day >= 1 && w->day <= 31 &&
           w->hour <= 23 && w->minute <= 59 && w->second <= 59;
}

static app_val_t time_value(uint16_t field) {
    uint64_t   ms = 0;
    app_wall_t w;
    int        have;

    w.year = 0; w.month = w.day = 1; w.hour = w.minute = w.second = 0;
    /* `now` is monotonic and needs nothing from the wall clock, so do not ask
     * for one: passing NULL is what keeps app_env_now()'s documented "a document
     * that never says clock never touches the CMOS" true for the stopwatch idiom
     * as well as for documents with no time leaf at all. */
    have = app_env_now(&ms, field == ATF_NOW ? (app_wall_t *)0 : &w);

    if (field == ATF_NOW) {
        /* Monotonic ms to fixed-point SECONDS: one scale unit is 1e-6 s, so a
         * millisecond is 1000 of them. Saturates rather than wrapping, which
         * takes 285 years of uptime to reach. */
        if (ms > (uint64_t)(APP_NUM_MAX / 1000)) return app_val_num(APP_NUM_MAX);
        return app_val_num((int64_t)ms * 1000);
    }
    if (!have || !wall_sane(&w)) return app_val_err();

    switch (field) {
    case ATF_CLOCK: {
        char b[9];
        put2(b, w.hour); b[2] = ':';
        put2(b + 3, w.minute); b[5] = ':';
        put2(b + 6, w.second); b[8] = '\0';
        return app_val_str(b);
    }
    case ATF_TODAY: {
        char b[11];
        int  y = w.year;
        b[0] = (char)('0' + (y / 1000) % 10);
        b[1] = (char)('0' + (y / 100) % 10);
        b[2] = (char)('0' + (y / 10) % 10);
        b[3] = (char)('0' + y % 10);
        b[4] = '-';
        put2(b + 5, w.month); b[7] = '-';
        put2(b + 8, w.day);   b[10] = '\0';
        return app_val_str(b);
    }
    case ATF_HOUR:   return app_val_num((int64_t)w.hour   * APP_NUM_SCALE);
    case ATF_MINUTE: return app_val_num((int64_t)w.minute * APP_NUM_SCALE);
    case ATF_SECOND: return app_val_num((int64_t)w.second * APP_NUM_SCALE);
    default:         return app_val_err();
    }
}

static app_val_t call_fn(uint8_t op, const app_val_t *a, const app_val_t *b) {
    char ta[APP_TEXT_MAX], tb[APP_TEXT_MAX];

    switch (op) {
    case AO_ISERR:
        return app_val_num(a->kind == AV_ERR ? APP_NUM_SCALE : 0);
    case AO_NUM: {
        if (a->kind == AV_NUM) return *a;
        if (a->kind == AV_ERR) return app_val_err();
        int64_t v = 0;
        if (app_num_parse(a->str, &v) != 0) return app_val_err();
        return app_val_num(v);
    }
    case AO_TEXT:
        if (a->kind == AV_ERR) return app_val_str("error");
        return app_val_str(app_val_text(a, ta, sizeof ta));
    case AO_CAT: {
        if (a->kind == AV_ERR || b->kind == AV_ERR) return app_val_err();
        app_val_text(a, ta, sizeof ta);
        app_val_text(b, tb, sizeof tb);
        size_t la = a_len(ta), lb = a_len(tb);
        if (la + lb >= APP_TEXT_MAX) return app_val_err();
        app_val_t r = app_val_str(ta);
        for (size_t i = 0; i < lb; i++) r.str[la + i] = tb[i];
        r.str[la + lb] = '\0';
        return r;
    }
    case AO_LEN:
        if (a->kind == AV_ERR) return app_val_err();
        app_val_text(a, ta, sizeof ta);
        return app_val_num((int64_t)a_len(ta) * APP_NUM_SCALE);
    case AO_DIGITS: {
        if (a->kind == AV_ERR) return app_val_err();
        app_val_text(a, ta, sizeof ta);
        int64_t n = 0;
        for (size_t i = 0; ta[i]; i++) if (ta[i] >= '0' && ta[i] <= '9') n++;
        return app_val_num(n * APP_NUM_SCALE);
    }
    case AO_HAS: {
        if (a->kind == AV_ERR || b->kind == AV_ERR) return app_val_err();
        app_val_text(a, ta, sizeof ta);
        app_val_text(b, tb, sizeof tb);
        if (!tb[0]) return app_val_num(0);
        for (size_t i = 0; ta[i]; i++) {
            size_t k = 0;
            while (tb[k] && ta[i + k] == tb[k]) k++;
            if (!tb[k]) return app_val_num(APP_NUM_SCALE);
        }
        return app_val_num(0);
    }
    case AO_ABS:
        if (a->kind != AV_NUM) return app_val_err();
        return app_val_num(a->num < 0 ? -a->num : a->num);
    case AO_MIN:
    case AO_MAX: {
        if (a->kind != AV_NUM || b->kind != AV_NUM) return app_val_err();
        int lo = a->num < b->num;
        if (op == AO_MIN) return app_val_num(lo ? a->num : b->num);
        return app_val_num(lo ? b->num : a->num);
    }
    /* round(x): to the nearest whole number, halves away from zero — the rule a
     * person means by "to the nearest penny" (round(x*100)/100). Negation is done
     * on the magnitude so -INT64_MIN is never formed, and the result goes through
     * the same range check as arithmetic, so 9e9 rounds to ERR rather than to a
     * wrapped number. */
    case AO_ROUND: {
        if (a->kind != AV_NUM) return app_val_err();
        int      neg = a->num < 0;
        __int128 mag = neg ? -(__int128)a->num : (__int128)a->num;
        int      ok  = 0;
        mag = ((mag + APP_NUM_SCALE / 2) / APP_NUM_SCALE) * APP_NUM_SCALE;
        int64_t r = clamp_ok(neg ? -mag : mag, &ok);
        return ok ? app_val_num(r) : app_val_err();
    }
    /* rand(n): a whole number in 0..n-1. n is truncated toward zero, so
     * rand(6.9) is rand(6); n outside 1..APP_RAND_MAX is ERR, which is how a
     * document learns it asked for something impossible instead of silently
     * getting zero every time.
     *
     * WHY THERE IS NO REJECTION LOOP. The usual uniform draw retries when the
     * raw value lands in the biased tail, and "retries" on a single-threaded
     * kernel means a document could in principle choose n to make it spin. The
     * multiply-high form below is branch-free and its bias is at most n/2^64 —
     * for a six-sided die, one part in 3e18. Bounded beats perfect here. */
    case AO_RAND: {
        if (a->kind != AV_NUM) return app_val_err();
        int64_t n = a->num / APP_NUM_SCALE;
        if (n < 1 || n > APP_RAND_MAX) return app_val_err();
        uint64_t r = app_env_random();
        uint64_t v = (uint64_t)(((unsigned __int128)r *
                                 (unsigned __int128)(uint64_t)n) >> 64);
        if (v >= (uint64_t)n) v = (uint64_t)n - 1;   /* unreachable; cheap */
        return app_val_num((int64_t)v * APP_NUM_SCALE);
    }
    /* at(s,i): the i'th BYTE of s, counting from 0, as a one-character string.
     * Bytes, not characters: text here is UTF-8 and a multi-byte character would
     * be cut in half, so this is for picking out of an ASCII set (a password
     * alphabet, a list of answers). Out of range is ERR rather than "" so that a
     * document walking off the end of its own alphabet says so on the screen. */
    case AO_AT: {
        if (a->kind == AV_ERR || b->kind != AV_NUM) return app_val_err();
        if (b->num < 0) return app_val_err();
        app_val_text(a, ta, sizeof ta);
        int64_t i = b->num / APP_NUM_SCALE;
        if ((uint64_t)i >= (uint64_t)a_len(ta)) return app_val_err();
        char one[2];
        one[0] = ta[i];
        one[1] = '\0';
        return app_val_str(one);
    }
    default:
        return app_val_err();
    }
}

app_val_t app_expr_eval(const app_inst_t *in, uint16_t index,
                        const app_val_t *key, const gui_window_t *win) {
    app_val_t st[APP_STACK];
    int       sp = 0;

    if (!in || index >= in->nexpr) return app_val_err();
    const app_expr_t *e = &in->expr[index];
    if (e->depth > APP_STACK) return app_val_err();

    for (uint16_t i = 0; i < e->count; i++) {
        const app_op_t *o = &in->op[e->first + i];
        uint8_t         k = o->op;

        /* Leaves. The compiler proved there is room, but the check stays: a
         * corrupted instance must refuse, not write past the array. */
        if (k <= AO_TIME) {
            if (sp >= APP_STACK) return app_val_err();
            switch (k) {
            case AO_CONST:
                st[sp++] = app_val_num(o->arg < in->nkonst ? in->konst[o->arg] : 0);
                break;
            case AO_STR:
                st[sp++] = app_val_str(o->arg < APP_STRPOOL ? in->strpool + o->arg
                                                            : "");
                break;
            case AO_VAR:
                st[sp++] = (o->arg < in->nvar) ? in->var[o->arg] : app_val_err();
                break;
            case AO_WIDGET: {
                const gui_widget_t *w = (win && o->arg < win->nwidgets)
                                      ? &win->widgets[o->arg] : (const gui_widget_t *)0;
                st[sp++] = w ? app_val_str(w->text) : app_val_err();
                break;
            }
            case AO_KEY:
                st[sp++] = key ? *key : app_val_str("");
                break;
            default:                       /* AO_TIME */
                st[sp++] = time_value(o->arg);
                break;
            }
            continue;
        }

        if (k == AO_NOT || k == AO_NEG) {
            if (sp < 1) return app_val_err();
            app_val_t a = st[sp - 1];
            if (k == AO_NEG) {
                st[sp - 1] = (a.kind == AV_NUM) ? app_val_num(-a.num)
                                                : app_val_err();
            } else {
                int t = app_val_truthy(&a);
                st[sp - 1] = (t < 0) ? app_val_err()
                                     : app_val_num(t ? 0 : APP_NUM_SCALE);
            }
            continue;
        }

        /* Unary functions consume one and push one. */
        if (k == AO_NUM || k == AO_TEXT || k == AO_LEN || k == AO_DIGITS ||
            k == AO_ISERR || k == AO_ABS || k == AO_ROUND || k == AO_RAND) {
            if (sp < 1) return app_val_err();
            st[sp - 1] = call_fn(k, &st[sp - 1], &st[sp - 1]);
            continue;
        }

        /* Everything else is binary. */
        if (sp < 2) return app_val_err();
        app_val_t b = st[--sp];
        app_val_t a = st[sp - 1];
        switch (k) {
        case AO_ADD: case AO_SUB: case AO_MUL: case AO_DIV: case AO_MOD:
            st[sp - 1] = arith(k, &a, &b);
            break;
        case AO_EQ: case AO_NE: {
            int cmp = compare(&a, &b, 0);
            if (cmp == -2) st[sp - 1] = app_val_err();
            else {
                int eq = (cmp == 0);
                st[sp - 1] = app_val_num(((k == AO_EQ) == (eq != 0))
                                         ? APP_NUM_SCALE : 0);
            }
            break;
        }
        case AO_LT: case AO_LE: case AO_GT: case AO_GE: {
            int cmp = compare(&a, &b, 1);
            if (cmp == -2) { st[sp - 1] = app_val_err(); break; }
            int t = (k == AO_LT) ? (cmp < 0) : (k == AO_LE) ? (cmp <= 0)
                  : (k == AO_GT) ? (cmp > 0) : (cmp >= 0);
            st[sp - 1] = app_val_num(t ? APP_NUM_SCALE : 0);
            break;
        }
        case AO_AND: case AO_OR: {
            int ta = app_val_truthy(&a), tb = app_val_truthy(&b);
            if (ta < 0 || tb < 0) { st[sp - 1] = app_val_err(); break; }
            int t = (k == AO_AND) ? (ta && tb) : (ta || tb);
            st[sp - 1] = app_val_num(t ? APP_NUM_SCALE : 0);
            break;
        }
        default:
            st[sp - 1] = call_fn(k, &a, &b);
            break;
        }
    }

    if (sp != 1) return app_val_err();
    return st[0];
}
