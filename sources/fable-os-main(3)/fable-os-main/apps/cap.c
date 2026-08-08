/* cap.c — the capability broker: the one place a document can reach hardware.
 *
 * PURPOSE
 *   An app is a JSON document a model wrote ninety seconds ago. Until this file
 *   existed it was sealed off from every driver: it could compute, lay out
 *   widgets and read the clock, and that was all — a model-built metronome could
 *   not tick audibly, and a calculator could not beep. The operator decided apps
 *   should be able to call drivers, the way apps do on a real operating system.
 *
 *   This is the whole of that road, and it is deliberately narrow:
 *
 *     document  ->  runtime.c compiles {"call":...} and evaluates its arguments
 *               ->  app_cap_request()  validates and QUEUES  (touches no device)
 *               ->  app_cap_service()  performs it, once, from the idle loop
 *               ->  core/audio.c       which owns the driver
 *
 *   runtime.c never mentions audio.h and cap.c never sees an app_inst_t, so
 *   "the kernel brokers every call" is a property of the link graph rather than a
 *   promise: exactly one function in apps/ can cause a hardware effect, and it is
 *   app_cap_service().
 *
 * ============================================================================
 * THIS VALIDATION IS THE SECURITY BOUNDARY. THERE IS NO MMU BEHIND IT.
 * ============================================================================
 *   This kernel is single-threaded, ring-0 only, with no privilege separation,
 *   no userspace and no memory protection — every page is mapped RWX and an app
 *   document is parsed by code running with exactly the same authority as the
 *   driver it is asking for. So there is no second line of defence anywhere
 *   under this file. If a number from a document reached a driver unchecked, the
 *   only thing between it and the hardware would be the driver's own opinion of
 *   it.
 *
 *   Concretely, what that rules out and what it buys:
 *     - a document may name only a capability from the table below. There is no
 *       way to spell "call this function", no pointer, no handle, no address and
 *       no device name in the format at all;
 *     - every argument is a whole number inside a stated range, or one string
 *       out of an enumerated set. Nothing else can be expressed;
 *     - a capability call cannot be issued from inside a handler. It is queued
 *       and performed later, by C, from the idle loop, so a document cannot
 *       reach a driver while the runtime is halfway through executing it;
 *     - and the machine-wide rate limit is enforced HERE rather than by the
 *       driver, because the driver has no idea it is being driven by a document.
 *
 * ============================================================================
 * WHY THE CALL IS QUEUED AND NOT PERFORMED WHERE IT IS WRITTEN
 * ============================================================================
 *   The event loop is single-threaded and polled, and the same loop serves the
 *   natural-language prompt (kernel/main.c's wait_for_sentence). Whatever a
 *   handler does, the operator's sentence waits. So a handler must not wait for a
 *   device:
 *
 *     - the handler runs to completion in arithmetic time, exactly as it did
 *       before this file existed. Clicking a button cannot take longer because
 *       the button makes a noise;
 *     - the sound starts from app_service(), which the IDLE loop calls between
 *       input polls. Nothing is started during a model turn, when the machine is
 *       inside a TLS round trip, and nothing is started from inside gui paint.
 *       That is enforced by which function the ui fiber calls, not by hope: see
 *       the note below;
 *     - one request is in flight at a time, and the next one is refused until the
 *       machine has been free for at least as long as the last one blocked it.
 *
 *   HOW THE "NOT DURING A MODEL TURN" HALF WAS BROKEN, AND WHAT NOW HOLDS IT.
 *   That sentence was true when app_tick() was reached only from
 *   wait_for_sentence(). It became false when app_tick() also became the body of
 *   kernel/main.c's ui fiber, because net/net.c's net_service() yields to that
 *   fiber on every pass of every network wait — a DNS lookup and the model turn
 *   included — and app_tick() ended with an unconditional app_service(). Measured:
 *   ~2000 ui-fiber passes inside one 657 ms API call, with an
 *   [app_call cap=audio.tone ...] line printed between "net: resolving
 *   api.anthropic.com" and "TLS ready". apps/runtime.c now publishes
 *   app_tick_handlers() (compute only) separately from app_tick() (compute plus
 *   pump); the fiber calls the former, the idle loop the latter. A safety property
 *   that rests on a call site has to be spelled out at that call site, which is
 *   why the same note is in include/app.h and kernel/main.c.
 *
 *   HONEST LIMIT, stated because it is the one thing here that is not fully
 *   solved: audio.h's play contract is SYNCHRONOUS ("play must not return until
 *   the device has finished reading the buffer"), because the kernel refills that
 *   buffer for the next sound and has no way to ask a device where it is reading.
 *   So the drain itself blocks. With a native sink it blocks for the length of the
 *   sound; with a sink that is a DRIVER PROGRAM it blocks for longer, because
 *   core/audio.c gives that program a polling budget of the sound plus
 *   AUDIO_PLAY_MARGIN_US and the program spends it in mdelay(), which yields to
 *   nothing. A 200 ms tone was measured holding the machine for 450 ms.
 *
 *   So APP_CAP_AUDIO_MS_MAX does NOT bound the stall — it bounds the duration an
 *   app may request, which is a different number. What bounds the stall is the
 *   duty-cycle rule on `free_at_ms` below: every call is TIMED, and the next one
 *   is refused until the machine has been free for at least that long again plus
 *   APP_CAP_GAP_MS. That keeps app sounds under half the machine whatever a driver
 *   does, instead of asserting a percentage derived from a constant that turned
 *   out not to govern it. A truly non-blocking sound still needs a start/poll
 *   split in the audio service — see FUTURE EXTENSION POINTS in include/app.h.
 *   Long sounds remain the model's own audio_tone tool, which is traced as a tool
 *   call and is not on this path at all.
 *
 * UNTRUSTED INPUT
 *   Argument values arrive as app_val_t from the expression evaluator, so they
 *   can be numbers, arbitrary text, or the ERR value; none of the three can be
 *   trusted to be sane. Each is checked against the table before it becomes an
 *   argument to anything, and a refusal names the bound. A failure sentence
 *   coming back OUT of a sink can contain model bytes (a VM play program's
 *   `abort "reason"`), so it goes through app_cap_clean() before it reaches a
 *   widget or a note — see the comment on that function.
 */

#include "app_impl.h"

#include "audio.h"
#include "kernel.h"   /* kputs + console_cursor: see the trace guard below */
#include "trace.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>       /* snprintf / vsnprintf; port/stdio.h when freestanding */

/* ====================================================================== */
/* small helpers (apps/ has no libc)                                       */
/* ====================================================================== */

static size_t c_len(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void c_cpy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (src) for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int c_eq(const char *a, const char *b) {
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && a[i] == b[i]; i++) { }
    return a[i] == '\0' && b[i] == '\0';
}

static void whyf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (!dst || !cap) return;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

void app_cap_clean(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || !cap) return;
    if (src)
        for (; src[i] && i + 1 < cap; i++) {
            unsigned char ch = (unsigned char)src[i];
            dst[i] = (ch < 0x20 || ch == 0x7F || ch == '[' || ch == ']')
                         ? ' ' : (char)ch;
        }
    dst[i] = '\0';
}

/* ====================================================================== */
/* THE CAPABILITY TABLE — the enumeration app.h documents                  */
/* ====================================================================== */

/* Capability ids. The name a document writes is "<family>.<verb>", so the
 * families app.h lists as extension points (clock, fs, net) each become rows
 * here and nothing about the document format changes: a new capability is a
 * table entry plus a case in perform(), and the tool's action=caps text, the
 * rejection messages and the bounds documentation are all generated from the
 * table, so they cannot fall out of step with it. */
enum { CAP_AUDIO_TONE = 0, CAP__COUNT };

/* audio.tone's arguments.
 *
 * EVERY BOUND HERE IS A DECISION, and app.h states each one with its reason.
 * The two that matter most:
 *   hz  0 is a rest (exactly what audio.h means by it) and 20..20000 is the
 *       audible band. A 20 GHz tone is refused rather than clamped, because
 *       clamping would report a sound that was not the one asked for — and
 *       because a refusal with the number in it is what lets a model fix itself.
 *   ms  1..APP_CAP_AUDIO_MS_MAX, which is far below audio.h's own AUDIO_MAX_MS.
 *       An app's sound is a UI beep; see the header. A 10-hour tone is refused
 *       by this bound, and named.
 */
static const app_cap_arg_t audio_tone_args[] = {
    { "hz",   ACA_NUM, 1, 0, (int64_t)AUDIO_HZ_MAX,        440,
      "0 = a rest" },
    { "ms",   ACA_NUM, 0, 1, (int64_t)APP_CAP_AUDIO_MS_MAX, 120, (const char *)0 },
    { "amp",  ACA_NUM, 0, 0, (int64_t)AUDIO_AMP_MAX,         0,
      "kernel's level" },
    { "wave", ACA_STR, 0, 0, 0,                              0,
      "default 'sine': 'square' 'triangle'" },
};

typedef struct app_cap {
    const char          *name;
    const char          *what;      /* one line, for action=caps            */
    const app_cap_arg_t *arg;
    uint8_t              nargs;
} app_cap_t;

static const app_cap_t table[CAP__COUNT] = {
    { "audio.tone",
      "a tone, through the audio driver",
      audio_tone_args,
      (uint8_t)(sizeof audio_tone_args / sizeof audio_tone_args[0]) },
};

_Static_assert(sizeof audio_tone_args / sizeof audio_tone_args[0]
                   <= APP_CAP_MAX_ARGS,
               "a capability may not take more arguments than a call can hold");

int app_cap_count(void) { return CAP__COUNT; }

int app_cap_lookup(const char *name) {
    for (int i = 0; i < CAP__COUNT; i++)
        if (c_eq(name, table[i].name)) return i;
    return -1;
}

const char *app_cap_name(int cap) {
    if (cap < 0 || cap >= CAP__COUNT) return "";
    return table[cap].name;
}

int app_cap_nargs(int cap) {
    if (cap < 0 || cap >= CAP__COUNT) return 0;
    return table[cap].nargs;
}

const app_cap_arg_t *app_cap_arg(int cap, int index) {
    if (cap < 0 || cap >= CAP__COUNT) return (const app_cap_arg_t *)0;
    if (index < 0 || index >= table[cap].nargs) return (const app_cap_arg_t *)0;
    return &table[cap].arg[index];
}

void app_cap_names(char *dst, size_t cap) {
    size_t o = 0;
    if (!dst || !cap) return;
    dst[0] = '\0';
    for (int i = 0; i < CAP__COUNT; i++) {
        size_t need = c_len(table[i].name) + (o ? 2 : 0);
        if (o + need + 1 >= cap) break;
        if (o) { dst[o++] = ','; dst[o++] = ' '; }
        c_cpy(dst + o, cap - o, table[i].name);
        o += c_len(table[i].name);
    }
}

/* ====================================================================== */
/* the specification a model reads                                        */
/* ====================================================================== */

/* Generated from the table above, so the bounds a model is told and the bounds
 * that are enforced are the same numbers — the reason this is code and not a
 * paragraph in tools/app_tools.c. That file supplies the prose and the launchable
 * example around it, and prints the whole thing for action=caps; it is a tool
 * RESULT, so it costs nothing from the tool schema budget chat.h bounds (see the
 * header of app_tools.c). */
size_t app_cap_spec(char *dst, size_t cap) {
    size_t need = 0;

    /* One appender, so a short buffer truncates cleanly and the return value is
     * still the number of bytes the text WANTED (snprintf's convention).
     *
     * line_ MUST BE BIGGER THAN THE LONGEST SENTENCE BELOW. It was 320 while one
     * paragraph was 500, and the paragraph came out of snprintf clipped mid-word
     * while `need` still reported the full length — a truncation that no caller
     * could detect. A test now asserts the closing sentence survives, and every
     * PUT here is kept to one short paragraph. */
    #define PUT(...)                                                          \
        do {                                                                  \
            char line_[384];                                                  \
            int  n_ = snprintf(line_, sizeof line_, __VA_ARGS__);              \
            if (n_ > 0) {                                                      \
                if (dst && cap && need + (size_t)n_ < cap)                     \
                    c_cpy(dst + need, cap - need, line_);                      \
                need += (size_t)n_;                                            \
            }                                                                  \
        } while (0)

    if (dst && cap) dst[0] = '\0';

    /* COMPACT ON PURPOSE. This whole page has to fit CHAT_TOOL_RESULT_CAP
     * (1024 bytes) with the worked example and the prose around it, or the model
     * reads a specification that stops mid-argument — so an argument is one short
     * line: name, range, whether it is required, and the default. Nothing is
     * spelled out twice, and every number here is the one that is enforced. */
    for (int i = 0; i < CAP__COUNT; i++) {
        PUT("%s - %s\n", table[i].name, table[i].what);
        for (int k = 0; k < table[i].nargs; k++) {
            const app_cap_arg_t *a = &table[i].arg[k];
            if (a->kind == ACA_STR)
                PUT("  %s  text, %s\n", a->name, a->choices ? a->choices : "");
            else if (a->required)
                PUT("  %s  number %ld-%ld, REQUIRED%s%s%s\n", a->name,
                    (long)a->lo, (long)a->hi, a->choices ? " (" : "",
                    a->choices ? a->choices : "", a->choices ? ")" : "");
            else
                PUT("  %s  number %ld-%ld, default %ld%s%s%s\n", a->name,
                    (long)a->lo, (long)a->hi, (long)a->def,
                    a->choices ? " (" : "", a->choices ? a->choices : "",
                    a->choices ? ")" : "");
        }
    }

    PUT("The KERNEL performs it: an app never reaches a driver, and a value out "
        "of range is REFUSED, not clamped, when the handler runs.\n");
    PUT("Nothing plays inside a handler: it is queued, and the idle loop starts "
        "one per %u ms at most, so the prompt keeps answering. Each prints a "
        "kernel trace line.\n", (unsigned)APP_CAP_GAP_MS);

    #undef PUT
    return need;
}

/* ====================================================================== */
/* validating one argument                                                */
/* ====================================================================== */

/* EVERY REFUSAL IS SAID TWICE, LONG AND SHORT, and that is not duplication —
 * they go to two different readers:
 *
 *   `why`   the whole sentence, with the numbers and the bound in it. It becomes
 *           the app's note (app_last_error()), which app action=state hands to
 *           the MODEL, and it is what makes attempt two land.
 *   `brief` at most APP_TEXT_MAX-1 bytes, because it goes into the "into" widget
 *           and a widget shows 39 characters (GUI_TEXT_MAX). Written as a phrase
 *           rather than a clipped sentence, so a status line reads "hz out of
 *           range" instead of "audio.tone: \"hz\" is 20000000000, which i".
 */
typedef struct { char *why; size_t why_cap; char *brief; size_t brief_cap; } msg_t;

static void brief_of(const msg_t *m, const char *s) {
    if (m && m->brief) c_cpy(m->brief, m->brief_cap, s);
}

/* A number argument: round to the nearest whole number (halves away from zero,
 * exactly like the language's round()) and bounds-check. Rounding rather than
 * refusing a fraction is deliberate — "rand(3)*220" and "ms/2" are ordinary
 * things to write, and 439.999996 is a request for 440. */
static int arg_num(const app_val_t *v, const app_cap_arg_t *a, const char *cname,
                   int64_t *out, const msg_t *m) {
    char phrase[APP_TEXT_MAX];

    if (v->kind == AV_ERR) {
        whyf(m->why, m->why_cap,
             "%s: \"%s\" is an error value (a division by zero, an overflow, or "
             "arithmetic on text), so there is nothing to play", cname, a->name);
        snprintf(phrase, sizeof phrase, "%s is an error", a->name);
        brief_of(m, phrase);
        return APP_EINVAL;
    }
    if (v->kind != AV_NUM) {
        char seen[24];
        app_cap_clean(seen, sizeof seen, v->str);
        whyf(m->why, m->why_cap,
             "%s: \"%s\" must be a number, but the expression produced the text "
             "\"%s\"", cname, a->name, seen);
        snprintf(phrase, sizeof phrase, "%s is not a number", a->name);
        brief_of(m, phrase);
        return APP_EINVAL;
    }

    int64_t n = v->num;
    int64_t half = APP_NUM_SCALE / 2;
    n = (n >= 0) ? (n + half) / APP_NUM_SCALE : -((-n + half) / APP_NUM_SCALE);

    if (n < a->lo || n > a->hi) {
        whyf(m->why, m->why_cap,
             "%s: \"%s\" is %ld, which is outside %ld to %ld%s%s. The kernel "
             "refuses it rather than playing something else",
             cname, a->name, (long)n, (long)a->lo, (long)a->hi,
             a->choices ? " - " : "", a->choices ? a->choices : "");
        snprintf(phrase, sizeof phrase, "%s out of range", a->name);
        brief_of(m, phrase);
        return APP_EINVAL;
    }
    *out = n;
    return APP_OK;
}

/* ====================================================================== */
/* the pending call                                                       */
/* ====================================================================== */

/* ONE SLOT, MACHINE-WIDE, not one per app. There is one speaker: "who is making
 * the sound" has to have one answer (audio.h makes the same choice about sinks),
 * and the rate limit is a property of the machine rather than of a document. A
 * second request while this one waits is refused with a sentence saying so,
 * which is information the app can display; a queue would instead hide the
 * overload and then play a burst of stale beeps.
 *
 * ONE SLOT IS NOT THE SAME AS FIRST COME FOR EVER, and for a while it was. This
 * file grants the slot to whoever asks first and has no idea who else wanted it,
 * so FAIRNESS IS run_ticks()'s JOB, in apps/runtime.c: it rotates which document
 * gets to ask first, and it rotates on the GRANT rather than on a timer, so no
 * period in the machine can alias with it. Before that existed, two documents
 * with the same tick period were phase-locked and the lower pool slot won every
 * single time — measured at 132 sounds to 0 over 66 s. The refusals this file
 * hands out ("still busy", "too soon") are worded as transient because that is
 * what they now are; if this slot ever stops rotating they become permanent
 * refusals that read as temporary, which is the worst kind of error message this
 * project can produce. */
static struct {
    int      used;
    int      cap;
    uint32_t app;
    uint8_t  has_into, into_kind;
    uint16_t into;
    /* validated audio.tone arguments */
    uint32_t hz, ms, amp;
    uint8_t  wave;
} pend;

/* THE RATE LIMIT IS A MEASURED DUTY CYCLE, NOT A FIXED INTERVAL.
 *
 * It used to be "one sound started every APP_CAP_GAP_MS", timed from the moment
 * the previous one STARTED. Two things were wrong with that, and both of them
 * made a documented safety bound false rather than merely loose:
 *
 *   1. APP_CAP_AUDIO_MS_MAX bounds the duration an app may REQUEST. It does not
 *      bound how long performing that request BLOCKS this machine, and the two
 *      are not the same number. When the audio sink is a driver program — the
 *      normal outcome of driver_install, and the state this whole project exists
 *      to reach — core/audio.c's run_vm() raises the program's delay budget to
 *      the sound's length PLUS AUDIO_PLAY_MARGIN_US (250 ms), and the program
 *      spends it polling a device with mdelay(), which yields to nothing. A 200 ms
 *      app tone was measured holding the machine for 450 ms.
 *   2. Timing the gap from the START meant the blocking time ate the gap
 *      one-for-one. 450 ms of blocking inside a 500 ms interval is 90% of the
 *      machine, against a header that claimed 40%.
 *
 * So the interval is now timed from the moment the sound FINISHED, and it is at
 * least as long as the sound actually blocked for. The worst-case duty cycle is
 * therefore block/(block + APP_CAP_GAP_MS + block), which is strictly under half
 * whatever a driver does, and under 4% for the 20-150 ms beeps an app actually
 * asks for. A driver that blocks for longer buys itself a proportionally longer
 * silence rather than a larger share of the machine — which is the property the
 * headers claim, now enforced by measurement instead of by arithmetic on a
 * constant that turned out not to govern the thing it was named after.
 *
 * Measuring is the only way to know: the kernel cannot ask a model-authored
 * driver how long it will take, and nothing in this file gets to see the sink. */
static uint64_t free_at_ms;      /* no sound may START before this reading */
static uint64_t last_block_ms;   /* how long the previous one held the machine */
static int      started_any;

void app_cap_reset(void) {
    pend.used     = 0;
    free_at_ms    = 0;
    last_block_ms = 0;
    started_any   = 0;
}

int app_cap_pending(void) { return pend.used != 0; }

/* ====================================================================== */
/* app_cap_request — validate, then queue                                 */
/* ====================================================================== */

int app_cap_request(const app_cap_req_t *rq, char *why, size_t why_cap,
                    char *brief, size_t brief_cap) {
    msg_t m;
    m.why = why; m.why_cap = why_cap; m.brief = brief; m.brief_cap = brief_cap;
    if (why && why_cap)     why[0]   = '\0';
    if (brief && brief_cap) brief[0] = '\0';

    if (!rq || rq->cap < 0 || rq->cap >= CAP__COUNT || !rq->arg) {
        whyf(why, why_cap, "that is not a capability this kernel offers");
        brief_of(&m, "unknown capability");
        return APP_EINVAL;
    }

    const app_cap_t *c = &table[rq->cap];

    /* Refuse before validating arguments when the machine plainly cannot do it,
     * so a silent machine is never told its frequency is wrong (audio.h exposes
     * audio_no_sink_reason() for exactly this ordering). */
    if (rq->cap == CAP_AUDIO_TONE && !audio_available()) {
        whyf(why, why_cap, "%s", audio_no_sink_reason());
        brief_of(&m, "no audio driver");
        return APP_EINVAL;
    }

    /* ---- arguments, BEFORE the queue and the rate limit ----
     *
     * ORDER MATTERS, AND THIS IS THE USEFUL ONE. A bad argument is a permanent
     * fault in the document; "too soon" and "still busy" are transient facts about
     * this millisecond. A document whose hz is 5 MHz would otherwise be told "too
     * soon" whenever it happened to click twice, and the model would fix the
     * timing rather than the number. Validating first means the permanent fault
     * always wins — and it is why everything below lands in LOCALS: writing
     * pend.hz before the pending check is what would corrupt a request that is
     * already waiting. Only the last few lines of this function touch `pend`. */
    uint32_t stage_hz = 0, stage_ms = 0, stage_amp = 0;
    uint8_t  stage_wave = 0;

    int64_t num[APP_CAP_MAX_ARGS];
    char    str[APP_CAP_MAX_ARGS][APP_TEXT_MAX];
    char    phrase[APP_TEXT_MAX];

    for (int k = 0; k < c->nargs; k++) {
        const app_cap_arg_t *a = &c->arg[k];
        int present = (rq->argmask >> k) & 1;

        num[k]    = a->def;
        str[k][0] = '\0';

        if (!present) {
            if (a->required) {
                whyf(why, why_cap, "%s: \"%s\" is required (%s)", c->name,
                     a->name, a->choices
                                  ? a->choices
                                  : "a number inside the documented range");
                snprintf(phrase, sizeof phrase, "%s is required", a->name);
                brief_of(&m, phrase);
                return APP_EINVAL;
            }
            continue;
        }
        if (a->kind == ACA_NUM) {
            int rc = arg_num(&rq->arg[k], a, c->name, &num[k], &m);
            if (rc != APP_OK) return rc;
        } else {
            if (rq->arg[k].kind != AV_STR) {
                whyf(why, why_cap,
                     "%s: \"%s\" must be text in single quotes, e.g. "
                     "\"'square'\" - accepted: %s", c->name, a->name,
                     a->choices ? a->choices : "");
                snprintf(phrase, sizeof phrase, "%s is not text", a->name);
                brief_of(&m, phrase);
                return APP_EINVAL;
            }
            c_cpy(str[k], sizeof str[k], rq->arg[k].str);
        }
    }

    /* ---- capability-specific checks ---- */
    if (rq->cap == CAP_AUDIO_TONE) {
        /* hz: 0 is a rest, and the audible band starts at AUDIO_HZ_MIN. The
         * table cannot express "0 or 20..20000" as one range, so the hole is
         * checked here and the message names both halves. */
        if (num[0] != 0 && num[0] < (int64_t)AUDIO_HZ_MIN) {
            whyf(why, why_cap,
                 "audio.tone: \"hz\" is %ld, which is below %u Hz and would not "
                 "be audible. Use 0 for a deliberate rest",
                 (long)num[0], (unsigned)AUDIO_HZ_MIN);
            brief_of(&m, "hz too low to hear");
            return APP_EINVAL;
        }
        audio_wave_t w = AUDIO_WAVE_SINE;
        if (str[3][0] && audio_wave_from_name(str[3], &w) != 0) {
            char seen[24];
            app_cap_clean(seen, sizeof seen, str[3]);
            whyf(why, why_cap, "audio.tone: \"wave\" is \"%s\"; accepted: %s",
                 seen, c->arg[3].choices ? c->arg[3].choices : "");
            brief_of(&m, "no such wave");
            return APP_EINVAL;
        }
        stage_hz   = (uint32_t)num[0];
        stage_ms   = (uint32_t)num[1];
        stage_amp  = (uint32_t)num[2];
        stage_wave = (uint8_t)w;
    }

    /* ---- and only now the two transient refusals ----
     * One at a time, and not faster than the machine will start them. Both are
     * refusals rather than queue slots: see the comment on `pend`. */
    if (pend.used) {
        whyf(why, why_cap,
             "a sound is already waiting to be played; this one was dropped "
             "rather than queued behind it");
        brief_of(&m, "still busy");
        return APP_EBUSY;
    }
    /* A clock that went BACKWARDS (a suspend, or a test installing another time
     * source) must not silence every app until it catches up, so a reading before
     * the window opened is treated as "the window is open" — the same rule
     * app_tick() applies to a tick handler's due time. */
    if (started_any && rq->now_ms < free_at_ms &&
        free_at_ms - rq->now_ms <= (uint64_t)APP_CAP_QUIET_MAX_MS) {
        uint64_t wait = free_at_ms - rq->now_ms;
        whyf(why, why_cap,
             "too soon: the last app sound held this machine for %lu ms, so it "
             "stays quiet for that long again plus %u ms - %lu ms to go. Ask less "
             "often or for a shorter sound",
             (unsigned long)last_block_ms, (unsigned)APP_CAP_GAP_MS,
             (unsigned long)wait);
        brief_of(&m, "too soon");
        return APP_EBUSY;
    }

    pend.hz        = stage_hz;
    pend.ms        = stage_ms;
    pend.amp       = stage_amp;
    pend.wave      = stage_wave;
    pend.cap       = rq->cap;
    pend.app       = rq->app;
    pend.has_into  = rq->has_into;
    pend.into_kind = rq->into_kind;
    pend.into      = rq->into;
    pend.used      = 1;
    return APP_OK;
}

/* ====================================================================== */
/* app_cap_service — the only hardware action in apps/                     */
/* ====================================================================== */

int app_cap_service(uint64_t now_ms) {
    if (!pend.used) return 0;
    pend.used = 0;                     /* taken: a failure must not retry itself */

    /* THE APP MAY HAVE CLOSED between asking and now (a click that also closed
     * the window, or the operator's close box). A window that is gone must not
     * make a noise, so the request is dropped — and it is dropped without
     * spending the rate limit, because nothing was played. */
    if (!app_is_app(pend.app)) return 1;

    if (pend.cap != CAP_AUDIO_TONE) return 1;    /* unreachable: one capability */

    uint32_t amp = pend.amp ? pend.amp : AUDIO_AMP_DEFAULT;
    int rc = audio_tone_ex(pend.hz, pend.ms, amp, (audio_wave_t)pend.wave);

    /* HOW LONG DID THAT ACTUALLY COST THE MACHINE? Read the clock again rather
     * than assuming pend.ms: the sink may be a driver program polling a device,
     * in which case it blocks for the sound plus its polling margin, and the
     * kernel has no way to know that number in advance. A failed call is timed
     * too — a driver that hangs its budget and then reports failure has spent
     * the machine's time just the same.
     *
     * The clock can go backwards (a suspend, a test installing another source);
     * treat that as "no measurable block" rather than as an enormous one, which
     * would silence every app for the length of a bogus interval. */
    uint64_t done_ms = app_now_ms();
    uint64_t block   = (done_ms >= now_ms) ? done_ms - now_ms : 0;
    if (block > APP_CAP_QUIET_MAX_MS) block = APP_CAP_QUIET_MAX_MS;

    last_block_ms = block;
    /* Quiet for as long as it blocked, plus the ordinary gap, measured from the
     * moment it FINISHED. Clamped, so a pathological measurement cannot mute
     * apps for the rest of the boot. */
    uint64_t quiet = block + (uint64_t)APP_CAP_GAP_MS;
    if (quiet > APP_CAP_QUIET_MAX_MS) quiet = APP_CAP_QUIET_MAX_MS;
    free_at_ms  = done_ms + quiet;
    started_any = 1;
    app_note_sound();

    /* GROUND TRUTH, WHATEVER THE OUTCOME. An app is model-authored, so the
     * operator's only evidence that a document made the machine emit a sound is
     * a line the kernel printed from the real return value — the same bracketed
     * voice a tool call produces (trace.h). Every field below comes from kernel
     * source or from a number this file validated: `wave` is one of
     * audio_wave_name()'s three literals and NOT a string from the document, and
     * the sink's failure sentence is deliberately absent, because it can contain
     * model bytes. It reaches the app and the model through `into` and
     * app_last_error() instead, cleaned. */
    /* A KERNEL TRACE LINE STARTS IN COLUMN ZERO (trace.h), and this is the first
     * thing in the machine that prints while the PROMPT is on screen: the idle
     * loop performs the sound between two input polls, so the cursor is sitting
     * after "> " with no newline behind it, and the line would come out as
     * "> [app_call ...]". That is not a forgery — nothing model-controlled is
     * involved — but it stops the operator's and the harness's "^\[" from seeing
     * a genuine action. One newline first, only when it is needed, so an idle
     * machine does not accumulate blank lines. */
    {
        int row = 0, col = 0;
        console_cursor(&row, &col);
        if (col != 0) kputs("\n");
    }

    if (rc == AUDIO_OK)
        trace_ok("app_call", "cap=audio.tone app=%u hz=%u ms=%u amp=%u wave=%s",
                 (unsigned)pend.app, (unsigned)pend.hz, (unsigned)pend.ms,
                 (unsigned)amp, audio_wave_name((audio_wave_t)pend.wave));
    else
        trace_err(rc, "app_call",
                  "cap=audio.tone app=%u hz=%u ms=%u amp=%u wave=%s",
                  (unsigned)pend.app, (unsigned)pend.hz, (unsigned)pend.ms,
                  (unsigned)amp, audio_wave_name((audio_wave_t)pend.wave));

    /* The outcome, in the two voices app_cap_request() also uses: the sink's whole
     * sentence becomes this app's note (which app action=state hands the model),
     * and a short phrase goes in the "into" widget, which shows 39 characters. The
     * sentence is CLEANED first — it can carry a VM play program's `abort` text,
     * i.e. model bytes. */
    const char *phrase = "the driver failed";
    if (rc == AUDIO_OK)             phrase = "ok";
    else if (rc == AUDIO_ENOSINK)   phrase = "no audio driver";
    else if (rc == AUDIO_ENOSPC)    phrase = "too long for the buffer";
    else if (rc == AUDIO_EINVAL)    phrase = "the kernel refused it";

    if (rc != AUDIO_OK) {
        const char *e = audio_last_error();
        char        note[224];
        app_cap_clean(note, sizeof note, (e && e[0]) ? e : "the sound failed");
        app_cap_note(pend.app, note);
    } else {
        /* CLEAR IT. A sound that worked must not leave the last refusal standing
         * in app_last_error(), or `app action=state` shows "ok" in the widget and
         * "no audio driver is registered" underneath it — two contradictory
         * answers, one of them stale, in the place the model reads to decide what
         * to do next. */
        app_cap_note(pend.app, "");
    }
    if (pend.has_into)
        app_cap_deliver(pend.app, pend.into_kind, pend.into, phrase);
    return 1;
}
