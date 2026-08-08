/* tool.c — the model's syscall table: registry, schema assembly, dispatch.
 *
 * See tool.h for the contract. Tools self-register into the linker-collected
 * "tool_table" section (linker.ld), exactly like drivers do, so there is no
 * central list here to keep in sync — this file never learns any tool's name.
 *
 * DEFENSIVE POSTURE
 *   Two distinct untrusted inputs meet in this file and they need different
 *   treatment:
 *
 *   - The tool_use block (name, id, input span) is MODEL output. A confused or
 *     adversarial model sends unknown names, absent ids, and nonsense arguments.
 *     Every such case must become a legible error the model can recover from on
 *     its next turn, never a fault.
 *
 *   - A tool DESCRIPTOR is kernel source, but it is written by many hands and a
 *     malformed one would otherwise corrupt the request body sent to the API —
 *     turning one typo in one tool into a total outage, or worse, letting a
 *     stray '}' inject top-level keys into the request. Descriptors are
 *     therefore validated before they are advertised, and a bad one is skipped
 *     rather than trusted.
 */

#include "tool.h"
#include "kernel.h"
#include "trace.h"
#include "json.h"

#include <stdarg.h>
#include <stdio.h>      /* vsnprintf, via port/stdio.h when freestanding */

/* Provided by the linker (see the .tool_table section in linker.ld). */
extern const tool_t *const __start_tool_table[];
extern const tool_t *const __stop_tool_table[];

static size_t zlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* ====================================================================== */
/* registry                                                              */
/* ====================================================================== */

int tool_count(void) {
    return (int)(__stop_tool_table - __start_tool_table);
}

const tool_t *tool_at(int index) {
    if (index < 0 || index >= tool_count()) return NULL;
    return __start_tool_table[index];
}

/* A legal tool name is NUL-terminated within TOOL_NAME_MAX bytes, i.e. at most
 * TOOL_NAME_MAX-1 characters. Anything longer is rejected consistently by BOTH
 * the lookup below and the schema builder above, so a name can never be
 * advertised to the model yet be undispatchable. */
static int name_ok(const char *name) {
    if (!name || !name[0]) return 0;
    for (size_t i = 0; i < TOOL_NAME_MAX; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '\0') return 1;
        /* A name reaches the console as the operation field of a trace line, and
         * a descriptor is written by many hands. Refuse anything that could
         * forge line structure outright, rather than relying on the renderer to
         * escape it: a tool whose name contains a newline is a bug, not a tool.
         * Names are identifiers, so printable-and-not-a-bracket is no real
         * constraint. */
        if (c < 0x20 || c == 0x7F || c == '[' || c == ']') return 0;
    }
    return 0;                 /* longer than any legal tool name */
}

/* Bounded compare. `a` is a validated descriptor name; `b` is model-supplied and
 * may be neither short nor NUL-terminated within the window. */
static int name_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (size_t i = 0; i < TOOL_NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 0;
}

/* A descriptor is only usable if its name is legal and its input_schema really
 * is a JSON object. Validating the schema here is what stops one malformed
 * registration from corrupting the whole request body. */
static int tool_usable(const tool_t *t) {
    if (!t || !t->invoke || !name_ok(t->name)) return 0;
    if (!t->input_schema) return 0;

    json_value_t v;
    if (json_parse(t->input_schema, zlen(t->input_schema), &v) != 0) return 0;
    return v.type == JSON_OBJECT;
}

const tool_t *tool_find(const char *name) {
    if (!name) return NULL;
    int n = tool_count();
    for (int i = 0; i < n; i++) {
        const tool_t *t = __start_tool_table[i];
        if (tool_usable(t) && name_eq(t->name, name)) return t;
    }
    return NULL;
}

/* ====================================================================== */
/* schema assembly                                                       */
/* ====================================================================== */

/* A bounded writer that keeps counting after it stops writing, so the caller
 * gets snprintf semantics: the return value is the length that WOULD be needed.
 * json_writer_t deliberately freezes its length on overflow, which is the right
 * behaviour there but cannot report a required size. */
typedef struct {
    char  *dst;
    size_t cap;
    size_t need;
} sbuf_t;

static void sput_n(sbuf_t *s, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s->dst && s->cap && s->need + 1 < s->cap) s->dst[s->need] = p[i];
        s->need++;
    }
}

static void sput(sbuf_t *s, const char *p) { sput_n(s, p, zlen(p)); }

static void sput_char(sbuf_t *s, char c) { sput_n(s, &c, 1); }

/* Emit `"..."` with JSON string escaping, inline so no temporary is needed and
 * the required length stays exact even when the output is truncated. */
static void sput_json_string(sbuf_t *s, const char *p) {
    static const char hex[] = "0123456789abcdef";
    sput_char(s, '"');
    for (size_t i = 0; p && p[i]; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
            case '"':  sput(s, "\\\"");  break;
            case '\\': sput(s, "\\\\");  break;
            case '\b': sput(s, "\\b");   break;
            case '\f': sput(s, "\\f");   break;
            case '\n': sput(s, "\\n");   break;
            case '\r': sput(s, "\\r");   break;
            case '\t': sput(s, "\\t");   break;
            default:
                if (c < 0x20) {
                    sput(s, "\\u00");
                    sput_char(s, hex[(c >> 4) & 0xF]);
                    sput_char(s, hex[c & 0xF]);
                } else {
                    sput_char(s, (char)c);
                }
        }
    }
    sput_char(s, '"');
}

size_t tools_build_schema(char *dst, size_t cap) {
    sbuf_t s = { dst, cap, 0 };

    sput_char(&s, '[');
    int n = tool_count();
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        const tool_t *t = __start_tool_table[i];
        /* Skip anything that would produce malformed JSON or that could not be
         * dispatched if the model asked for it. */
        if (!tool_usable(t)) continue;

        if (emitted++) sput_char(&s, ',');
        sput(&s, "{\"name\":");
        sput_json_string(&s, t->name);
        sput(&s, ",\"description\":");
        sput_json_string(&s, t->description ? t->description : "");
        /* Validated as a JSON object by tool_usable() — insert verbatim. */
        sput(&s, ",\"input_schema\":");
        sput(&s, t->input_schema);
        sput_char(&s, '}');
    }
    sput_char(&s, ']');

    /* Always NUL-terminate whatever did fit. */
    if (dst && cap) dst[s.need < cap ? s.need : cap - 1] = '\0';
    return s.need;
}

/* ====================================================================== */
/* results                                                               */
/* ====================================================================== */

void tool_result_init(tool_result_t *r, char *buf, size_t cap) {
    if (!r) return;
    r->buf       = buf;
    r->cap       = cap;
    r->len       = 0;
    r->is_error  = 0;
    r->truncated = 0;
    if (buf && cap) buf[0] = '\0';
}

size_t tool_result_printf(tool_result_t *r, const char *fmt, ...) {
    if (!r || !r->buf || r->cap == 0) return 0;
    /* A corrupted len must not underflow the room calculation into a huge
     * value and hand vsnprintf a bogus bound. */
    if (r->len >= r->cap) { r->len = r->cap - 1; r->truncated = 1; return 0; }

    size_t  room = r->cap - r->len;          /* >= 1 */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->buf + r->len, room, fmt, ap);
    va_end(ap);

    if (n < 0) return 0;
    if ((size_t)n >= room) {                 /* genuinely clipped */
        r->truncated = 1;
        r->len = r->cap - 1;
        return room - 1;
    }
    r->len += (size_t)n;
    return (size_t)n;
}

/* ====================================================================== */
/* dispatch                                                              */
/* ====================================================================== */

int tool_dispatch(const tool_call_t *call, tool_result_t *out) {
    if (!call || !out || !out->buf || out->cap == 0) return TOOL_EINVAL;

    const tool_t *t = tool_find(call->name);
    if (!t) {
        /* An unknown name is a recoverable mistake, not a fault: tell the model
         * what actually exists so it can correct itself next turn. Names are
         * listed only while a whole one still fits, so the model is never taught
         * a tool that does not exist by reading a clipped suffix. */
        out->is_error = 1;
        tool_result_printf(out, "no such tool: %s. available:",
                           call->name ? call->name : "(null)");
        int n = tool_count();
        for (int i = 0; i < n; i++) {
            const tool_t *k = __start_tool_table[i];
            if (!tool_usable(k)) continue;
            /* +2 for the leading space and the NUL. */
            if (out->len + zlen(k->name) + 2 >= out->cap) {
                tool_result_printf(out, " ...");
                break;
            }
            tool_result_printf(out, " %s", k->name);
        }
        trace_err(TOOL_ENOENT, "tool_dispatch", "%s",
                  call->name ? call->name : "(null)");
        return TOOL_ENOENT;
    }

    /* Tools emit their own trace lines because only they know the real argument
     * values and return codes — that is the whole point of the mechanism. But
     * the guarantee that *every* model action produces a kernel-emitted line
     * cannot depend on each tool remembering to, nor can it be defeated by a
     * tool that fiddles with the trace module. So:
     *
     *   - measure with trace_emissions(), which counts only bytes that actually
     *     reached the console, is monotonic, and is immune to trace_reset();
     *   - force output on across the call and restore the operator's setting
     *     afterwards, so one tool cannot silence its own action or mute every
     *     dispatch that follows it.
     */
    int      saved_on = trace_enabled();
    uint64_t before   = trace_emissions();

    trace_set_enabled(1);
    int rc = t->invoke(call, out);
    trace_set_enabled(saved_on);

    /* A handler that reports a failure code without flagging the result would
     * otherwise have the kernel trace a failure while the model is told the
     * action succeeded. Reconcile the two voices before either is believed. */
    if (rc != TOOL_OK) out->is_error = 1;

    /* A clipped result is not a success: the model would reason over a prefix as
     * if it were whole. The notice must OVERWRITE the tail rather than be
     * appended — by the time truncation is detected the buffer is full by
     * definition, so appending is a no-op and the model would receive a silently
     * clipped payload. Losing a few more bytes of a result that was already
     * incomplete is the cheap side of this trade. */
    if (out->truncated) {
        char note[48];
        int  nn = snprintf(note, sizeof note, "\n[result truncated at %u bytes]",
                           (unsigned)(out->cap - 1));
        if (nn > 0 && out->cap > (size_t)nn + 1) {
            size_t nl = (size_t)nn;
            if (out->len + nl < out->cap) {
                /* Room to append: keep the whole result and name the bound. */
                memcpy(out->buf + out->len, note, nl);
                out->len += nl;
                out->buf[out->len] = '\0';
            } else {
                /* No room — and this is the case that actually matters, because
                 * detecting truncation implies the buffer is already full. An
                 * append would be a silent no-op and the model would receive a
                 * clipped payload believing it complete, so overwrite the tail
                 * instead. Losing a few more bytes of an already-incomplete
                 * result is the cheap side of the trade. */
                memcpy(out->buf + (out->cap - 1 - nl), note, nl);
                out->buf[out->cap - 1] = '\0';
                out->len = out->cap - 1;
            }
        }
    }

    if (trace_emissions() == before) {
        if (out->is_error) trace_err(rc, t->name, "%s", "");
        else               trace_ok(t->name, "%s", "");
    }

    return TOOL_OK;   /* the handler ran; logical failure is in out->is_error */
}
