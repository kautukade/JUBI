/* app_impl.h — the compiled form of an app document. Internal to apps/.
 *
 * PURPOSE
 *   include/app.h is the contract the rest of the kernel and the model's tools
 *   see. This header is the representation those two files agree on: what a
 *   document turns INTO once it has been validated, and the seam between
 *   apps/expr.c (values, numbers, expressions — pure arithmetic and text, no GUI)
 *   and apps/runtime.c (documents, widgets, statements, windows).
 *
 *   It is a private header on purpose. Nothing outside apps/ should be able to
 *   build an app by hand: app_launch() is the only way in, so the only code that
 *   can produce these structures is the code that validates them.
 *   tests/host/test_app.c includes it directly, which is how the evaluator is
 *   tested as a unit rather than only through a window.
 *
 * WHY POSTFIX
 *   Expressions are compiled at LOAD time into a flat postfix op list with a
 *   pre-computed stack high-water mark. Three properties fall out of that, all of
 *   which matter on a kernel with no memory protection:
 *     1. Evaluation is a loop over an array with no recursion, so a hostile
 *        "((((((..." costs stack in the COMPILER (bounded by APP_PAREN_DEPTH and
 *        rejected with a message) and none at all in the evaluator.
 *     2. Every name is resolved to an index while the document is being checked,
 *        so a typo is a load error with a byte offset instead of a lookup failure
 *        on the tenth click.
 *     3. The stack bound is proved before the app runs, not defended at runtime.
 *
 * EVERY OPERATION IS TOTAL
 *   There is no failure path out of eval(): division by zero, overflow, a string
 *   in an arithmetic slot and num("abc") all produce the ERR value, which
 *   propagates. See DECISION 2 in include/app.h.
 */
#ifndef APP_IMPL_H
#define APP_IMPL_H

#include <stdint.h>
#include <stddef.h>

#include "app.h"
#include "gui.h"
#include "widgets.h"

/* Bytes of one expression source string, including the NUL. Long enough for
 * anything readable and short enough that a runaway string is refused with a
 * number in the message. */
#define APP_EXPR_SRC_MAX   192

/* Nesting of parentheses and calls the compiler will follow. The compiler is the
 * only recursive part of this module. */
#define APP_PAREN_DEPTH    12

/* ====================================================================== */
/* values                                                                 */
/* ====================================================================== */

#define AV_NUM  0     /* fixed point, scaled by APP_NUM_SCALE               */
#define AV_STR  1     /* NUL-terminated, at most APP_TEXT_MAX-1 bytes        */
#define AV_ERR  2     /* the error value; text() renders it as "error"       */

typedef struct app_val {
    uint8_t kind;
    int64_t num;
    char    str[APP_TEXT_MAX];
} app_val_t;

/* ====================================================================== */
/* compiled expressions                                                   */
/* ====================================================================== */

enum {
    AO_CONST = 0,   /* arg = index into konst[]                            */
    AO_STR,         /* arg = offset into strpool                           */
    AO_VAR,         /* arg = var index                                     */
    AO_WIDGET,      /* arg = widget index; yields the widget's text         */
    AO_KEY,         /* the text of the widget that fired the handler        */
    AO_TIME,        /* arg = ATF_*; the event's time snapshot (see below)   */
    /* AO_TIME is the LAST leaf. app_expr_eval()'s leaf fast path is
     * "k <= AO_TIME", so a new leaf goes here and nowhere else. */
    AO_ADD, AO_SUB, AO_MUL, AO_DIV, AO_MOD,
    AO_EQ, AO_NE, AO_LT, AO_LE, AO_GT, AO_GE,
    AO_AND, AO_OR,
    AO_NOT, AO_NEG,
    AO_NUM, AO_TEXT, AO_CAT, AO_LEN, AO_DIGITS, AO_HAS, AO_ISERR,
    AO_ABS, AO_MIN, AO_MAX,
    AO_ROUND, AO_RAND, AO_AT,
    AO__COUNT
};

/* Which field of the time snapshot an AO_TIME leaf reads. `now` is monotonic
 * seconds since boot (millisecond resolution); the rest come from the wall clock
 * and are the ERR value when this machine has no readable one. */
enum {
    ATF_NOW = 0,
    ATF_CLOCK,        /* "HH:MM:SS" */
    ATF_TODAY,        /* "YYYY-MM-DD" */
    ATF_HOUR,
    ATF_MINUTE,
    ATF_SECOND,
    ATF__COUNT
};

typedef struct app_op {
    uint8_t  op;
    uint8_t  pad_;
    uint16_t arg;
} app_op_t;

typedef struct app_expr {
    uint16_t first;      /* index into inst->op                             */
    uint16_t count;
    uint8_t  depth;      /* stack slots this expression needs (<= APP_STACK) */
} app_expr_t;

/* ====================================================================== */
/* compiled statements                                                    */
/* ====================================================================== */

#define AS_SET   0
#define AS_IF    1
#define AS_STOP  2
#define AS_CALL  3

#define AT_VAR     0
#define AT_WIDGET  1

typedef struct app_stmt {
    uint8_t  kind;
    uint8_t  tgt_kind;      /* AS_SET: AT_VAR or AT_WIDGET                  */
    uint16_t tgt;           /* AS_SET: var or widget index;                 */
                            /* AS_CALL: index into inst->call[]             */
    uint16_t expr;          /* AS_SET / AS_IF: expression index             */
    uint16_t then_first, then_count;
    uint16_t else_first, else_count;
} app_stmt_t;

/* ====================================================================== */
/* compiled capability calls — {"call":...,"with":{...},"into":...}         */
/* ====================================================================== */

/* Arguments a single capability may take. Four covers audio.tone (hz, ms, amp,
 * wave) and every capability sketched in app.h's CAPABILITIES section; it is a
 * static bound so that a call statement is a fixed-size record and compiling one
 * allocates nothing. */
#define APP_CAP_MAX_ARGS   4

/* Call statements per document. Eight is far more than any UI needs (a keypad
 * with one sound is ONE call statement, because a tag lets one handler serve ten
 * keys) and bounds both this array and the expression slots the arguments
 * occupy: 8 * 4 = 32 of APP_MAX_EXPRS. */
#define APP_MAX_CALLS      8

/* One compiled call. `argmask` bit k means "argument k of this capability was
 * supplied", and then expr[k] is the expression that produces it; an absent
 * optional argument uses the table's default and expr[k] is meaningless. The
 * arguments are NOT evaluated at compile time — they are expressions, exactly
 * like a `set`'s "to" — so their BOUNDS are enforced when the handler runs. See
 * apps/cap.c. */
typedef struct app_call {
    uint8_t  cap;                        /* index into apps/cap.c's table    */
    uint8_t  argmask;
    uint16_t expr[APP_CAP_MAX_ARGS];
    uint8_t  has_into;                   /* "into" was given                 */
    uint8_t  into_kind;                  /* AT_VAR or AT_WIDGET              */
    uint16_t into;
} app_call_t;

/* ====================================================================== */
/* handlers                                                               */
/* ====================================================================== */

#define AE_CLICK   0
#define AE_SUBMIT  1
#define AE_TICK    2

/* Selectors (names and tags) are resolved at load time into a bitmask of widget
 * indices, so dispatch is a shift and a test and no string comparison happens on
 * a click. APP_MAX_WIDGETS is 48, which fits a uint64_t with room to spare.
 *
 * AE_TICK has no selector (mask is 0) and carries a period instead. `due` is the
 * monotonic millisecond reading at which it may next run; app_tick() sets it to
 * now+period AFTER running, so a slow or long-delayed tick can never accumulate a
 * backlog to catch up on — see app_tick() in runtime.c. */
typedef struct app_handler {
    uint8_t  ev;
    uint64_t mask;
    uint16_t first, count;      /* statement block                          */
    uint32_t period;            /* AE_TICK: milliseconds between runs        */
    uint64_t due;               /* AE_TICK: monotonic ms of the next run     */
} app_handler_t;

/* ====================================================================== */
/* one running app                                                        */
/* ====================================================================== */

typedef struct app_inst {
    uint8_t   used;
    uint32_t  win;                                  /* window id == app id  */
    char      title[GUI_TITLE_MAX];

    char      wname[APP_MAX_WIDGETS][APP_NAME_MAX];
    uint16_t  nwidget;

    char      vname[APP_MAX_VARS][APP_NAME_MAX];
    app_val_t var[APP_MAX_VARS];
    uint16_t  nvar;

    app_stmt_t stmt[APP_MAX_STMTS];
    uint16_t   nstmt;
    app_expr_t expr[APP_MAX_EXPRS];
    uint16_t   nexpr;
    app_op_t   op[APP_MAX_OPS];
    uint16_t   nop;
    int64_t    konst[APP_MAX_CONSTS];
    uint16_t   nkonst;
    char       strpool[APP_STRPOOL];
    uint16_t   strused;

    app_handler_t handler[APP_MAX_HANDLERS];
    uint16_t      nhandler;

    app_call_t call[APP_MAX_CALLS];
    uint16_t   ncall;

    /* The last runtime note, app_last_error()'s buffer.
     *
     * 224 AND NOT 128, because a capability refusal is the longest sentence this
     * runtime produces and it is the one that has to survive whole: at 128 the
     * audio service's own "no audio output driver is registered ... register
     * itself as the audio sink first" lost its last clause, which is the clause
     * that says what to DO about it. The bound matches app_error_t.msg, so a
     * refusal reads the same whether it happened at load time or at run time. */
    char      lasterr[224];
} app_inst_t;

/* ====================================================================== */
/* apps/expr.c — numbers, values, compilation and evaluation               */
/* ====================================================================== */

/* Fixed-point text <-> number. Both hand-rolled: there is no strtod in this
 * kernel, no %f in its printf, and a calculator's formatting rules (six
 * decimals, trailing zeros trimmed, the point kept only when it earns its place)
 * are not printf's rules anyway. */
int  app_num_parse(const char *s, int64_t *out);          /* 0 ok, -1 not a number */
void app_num_format(int64_t v, char *dst, size_t cap);

/* Value constructors and the one renderer everything text-shaped goes through. */
app_val_t   app_val_num(int64_t scaled);
app_val_t   app_val_str(const char *s);
app_val_t   app_val_err(void);
const char *app_val_text(const app_val_t *v, char *dst, size_t cap);
int         app_val_truthy(const app_val_t *v);           /* -1 when ERR */

/* Compile one expression source string into `in`. Returns APP_OK, or APP_EINVAL /
 * APP_ENOSPC with err->msg and err->offset (a byte offset into `src`) filled in.
 * On failure `in` may hold partly-emitted ops; the caller abandons the whole
 * instance, which is why that is safe. */
int app_expr_compile(app_inst_t *in, const char *src, uint16_t *out_index,
                     app_error_t *err);

/* Evaluate a compiled expression. `key` is the value of `key` in that context
 * (may be NULL, in which case `key` evaluates to the empty string) and `win` is
 * where widget text is read from (may be NULL: every widget then reads ERR).
 * Total: always returns a value. */
app_val_t app_expr_eval(const app_inst_t *in, uint16_t index,
                        const app_val_t *key, const gui_window_t *win);

/* Bump the shared ERR counter in app_stats(). Defined in runtime.c so that
 * expr.c stays free of state. */
void app_note_err_value(void);

/* Bump the counter of capability calls the kernel actually PERFORMED. Same
 * arrangement, for the same reason: cap.c holds no statistics of its own, so
 * app_stats() stays the one place a tool reads them from. */
void app_note_sound(void);

/* ====================================================================== */
/* the environment: time and randomness                                    */
/* ====================================================================== */

/* THE TWO IMPURE THINGS AN EXPRESSION CAN READ, and both are reached through
 * runtime.c so that expr.c keeps holding no state and touching no hardware.
 *
 * app_env_now() hands back the SNAPSHOT runtime.c took when the event began, not
 * a fresh reading. Two reasons, both load-bearing:
 *   - `hour` and `minute` in one handler must not straddle a minute boundary and
 *     render 10:59 as "11:59". One snapshot per event makes every expression in
 *     that event agree.
 *   - the wall clock is a device (CMOS port I/O). Reading it per operand would
 *     put unbounded-ish I/O inside the evaluator; reading it at most once per
 *     event, and only if the document actually mentions it, keeps evaluation the
 *     flat arithmetic loop it is documented to be.
 * Returns 1 when *wall is a usable reading, 0 when this machine has no readable
 * wall clock (then every wall-clock leaf is ERR). *ms is always set.
 *
 * app_env_random() returns 64 fresh bits from the machine's entropy source
 * (RDRAND, or the TSC mixer behind it — lib/libc_shim.c's mbedtls_hardware_poll,
 * reused rather than reinvented). It cannot fail and cannot block: the source
 * itself falls back rather than spinning. */
int      app_env_now(uint64_t *ms, app_wall_t *wall);
uint64_t app_env_random(void);

/* ====================================================================== */
/* apps/cap.c — the capability broker                                      */
/* ====================================================================== */

/* THE SEAM. runtime.c compiles and evaluates; cap.c decides what a capability
 * IS, validates it and performs it. Neither reaches into the other's structures:
 * cap.c never sees an app_inst_t (it holds an app id and calls back through
 * app_cap_deliver), and runtime.c never mentions audio.h. That is what keeps
 * "the kernel brokers every call" a property of the code rather than a claim —
 * there is exactly one function in this kernel that can turn a document into a
 * hardware action, and it is app_cap_service(). */

/* What kind of value an argument is, and how it is bounded. Numbers are whole
 * numbers after rounding (a fractional expression result is rounded to nearest,
 * halves away from zero) and are checked against [lo,hi] INCLUSIVE. Strings are
 * validated by the capability itself, and `choices` is the list a refusal
 * quotes. */
enum { ACA_NUM = 0, ACA_STR };

typedef struct app_cap_arg {
    const char *name;
    uint8_t     kind;        /* ACA_NUM | ACA_STR                            */
    uint8_t     required;
    int64_t     lo, hi;      /* ACA_NUM: inclusive bounds                    */
    int64_t     def;         /* ACA_NUM: value used when absent              */
    const char *choices;     /* ACA_STR: what is accepted, and the default   */
} app_cap_arg_t;

int                  app_cap_lookup(const char *name);   /* -1 = unknown     */
const char          *app_cap_name(int cap);              /* "" out of range  */
int                  app_cap_count(void);
int                  app_cap_nargs(int cap);
const app_cap_arg_t *app_cap_arg(int cap, int index);    /* NULL out of range */

/* Every capability name, comma-separated, for the refusal an unknown one earns.
 * Truncates rather than overflowing; always NUL-terminated. */
void app_cap_names(char *dst, size_t cap);

/* One call, with its arguments ALREADY EVALUATED by runtime.c. Returns APP_OK
 * when the call was accepted (it will be performed by the next
 * app_cap_service()), or APP_EINVAL / APP_EBUSY. Nothing here touches hardware:
 * that is app_cap_service()'s job and nowhere else's.
 *
 * A refusal is written TWICE, because it has two readers: `why` is the whole
 * sentence with the bound in it (it becomes the app's note, which app
 * action=state hands the model) and `brief` is a phrase short enough for the
 * "into" widget, which shows GUI_TEXT_MAX-1 characters. Either may be NULL. */
typedef struct app_cap_req {
    uint32_t         app;                     /* app (== window) id          */
    int              cap;
    uint64_t         now_ms;                  /* the event's time snapshot   */
    const app_val_t *arg;                     /* APP_CAP_MAX_ARGS values     */
    uint8_t          argmask;
    uint8_t          has_into, into_kind;
    uint16_t         into;
} app_cap_req_t;

int app_cap_request(const app_cap_req_t *rq, char *why, size_t why_cap,
                    char *brief, size_t brief_cap);

/* Is a call waiting to be performed? Cheap enough for the idle loop to ask on
 * every pass: it reads one flag. */
int app_cap_pending(void);

/* Perform the pending call, if any: this is the ONLY place in apps/ that reaches
 * a driver. Returns 1 if a call was performed (or deliberately dropped), 0 if
 * there was nothing to do. `now_ms` is a fresh monotonic reading. */
int app_cap_service(uint64_t now_ms);

/* One monotonic millisecond reading through the installed time source, provided
 * by apps/runtime.c. apps/cap.c needs it to read the clock AFTER a capability
 * call as well as before, because how long the call actually blocked the machine
 * is not knowable in advance: a sound's requested duration is not the same number
 * as the time a driver spends polling a device to play it. */
uint64_t app_now_ms(void);

/* Write `text` into a var or widget of a running app. Implemented in runtime.c
 * (it owns the pool) and called by cap.c to report an outcome that is only known
 * after the sound has played. A no-op if that app has closed in the meantime. */
void app_cap_deliver(uint32_t app, uint8_t tgt_kind, uint16_t tgt,
                     const char *text);

/* Record a one-line note against a running app (app_last_error(), and the
 * "note:" line app action=state prints). Also runtime.c's, for the same reason. */
void app_cap_note(uint32_t app, const char *text);

/* Copy `src` into `dst` with every byte that could corrupt the console turned
 * into a space: control characters, and '[' and ']'.
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT DECORATIVE. A failure sentence from a sink
 * can contain MODEL BYTES — a VM play program's `abort "reason"` reaches
 * audio_last_error() verbatim — and this module puts that sentence into a widget
 * and into app_last_error(), which a tool result prints. trace.h's one invariant
 * is that a '[' in column zero is the kernel speaking; a reason of
 * "\n[vfs_write fd=3 -> ok]" would forge one the moment anything echoed it. The
 * trace line this module emits carries no sink text at all, and this makes the
 * other two paths safe as well. */
void app_cap_clean(char *dst, size_t cap, const char *src);

/* Test hook: drop the pending call and forget the rate limiter, so a suite can
 * start each case from a known state. Nothing in the kernel calls it. */
void app_cap_reset(void);

#endif /* APP_IMPL_H */
