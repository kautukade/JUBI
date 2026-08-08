/* trace.c — see trace.h. Kernel-emitted ground truth for model actions.
 *
 * Deliberately allocation-free and hardware-free: formatting happens in fixed
 * stack buffers via the libc shim's vsnprintf, and output goes through kputs.
 * That keeps this callable from anywhere, including a future exception handler
 * reporting a fault while the heap is suspect.
 *
 * Two properties here are load-bearing for the whole project and are easy to
 * lose in a refactor, so they are spelled out:
 *
 *   1. ARGUMENTS ARE UNTRUSTED. They are model-controlled in nearly every real
 *      tool. They are escaped (control characters and both brackets) so a
 *      crafted path can never inject a second line that looks genuine.
 *   2. trace_emissions() COUNTS ONLY REAL OUTPUT. It is monotonic, unaffected by
 *      trace_reset(), and does not advance while suppressed. tool_dispatch()
 *      uses it to prove an action produced ground truth; the request counter
 *      (trace_count) cannot serve that purpose because a tool can move it.
 */

#include "trace.h"
#include "kernel.h"
#include "vfs.h"

#include <stdarg.h>
#include <stdio.h>     /* vsnprintf, via port/stdio.h when freestanding */

static uint32_t trace_seq       = 0;   /* lines REQUESTED (movable, for tests) */
static uint64_t trace_written   = 0;   /* lines EMITTED  (monotonic, trusted)  */
static int      trace_is_on     = 1;

/* Symbolic names for the kernel's errno-style codes. The VFS_* values are the
 * canonical negative errnos used across the kernel (see vfs.h); model.h reuses
 * the same space (MODEL_EINVAL/ENOSPC alias the VFS codes deliberately) and adds
 * transport codes. One table means a trace line reads the same wherever the
 * error came from.
 *
 * NOTE: 0 is deliberately absent. trace_err(0) means "the action failed but
 * carried no code", and rendering that as "ok" would make a failure look like a
 * success in the one place the operator is supposed to be able to trust. */
static const struct { int code; const char *name; } err_names[] = {
    { VFS_ENOENT,   "ENOENT"      },
    { VFS_EEXIST,   "EEXIST"      },
    { VFS_ENOTDIR,  "ENOTDIR"     },
    { VFS_EINVAL,   "EINVAL"      },
    { VFS_ENOSPC,   "ENOSPC"      },
    { -70,          "ETRANSPORT"  },   /* MODEL_ETRANSPORT */
    { -71,          "ETIMEOUT"    },   /* MODEL_ETIMEOUT   */
    { -72,          "EPROTO"      },   /* MODEL_EPROTO     */
    { -73,          "ESCRIPT"     },   /* MODEL_ESCRIPT    */
};

const char *trace_errname(int err) {
    for (size_t i = 0; i < sizeof err_names / sizeof err_names[0]; i++)
        if (err_names[i].code == err) return err_names[i].name;
    return NULL;
}

/* Copy `src` into `dst`, escaping anything that could forge line structure:
 * every control character, DEL, and both brackets become \xNN. Returns the
 * number of bytes written (never more than cap-1, always NUL-terminated). */
static size_t sanitize(char *dst, size_t cap, const char *src) {
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    if (!dst || cap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }

    for (size_t i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        int escape = (c < 0x20) || (c == 0x7F) || (c == '[') || (c == ']');
        if (escape) {
            if (o + 4 >= cap) break;
            dst[o++] = '\\';
            dst[o++] = 'x';
            dst[o++] = hex[(c >> 4) & 0xF];
            dst[o++] = hex[c & 0xF];
        } else {
            if (o + 1 >= cap) break;
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

static size_t zlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Render `[<op> <args> -> <result>]`.
 *
 * The result field is budgeted FIRST. It is the part that makes the line
 * evidence, so when a long argument would overflow the line, the argument is
 * truncated with an ellipsis rather than silently pushing "-> result]" and the
 * newline off the end. tool.h warns that a confused model can send a
 * 10,000-character path, so this path is exercised in practice. */
static void emit(const char *op, const char *args_fmt, va_list ap,
                 const char *result) {
    trace_seq++;
    if (!trace_is_on) return;

    if (!op)     op     = "?";
    if (!result) result = "?";

    /* The operation name is sanitized too, not just the arguments. It looks like
     * kernel-authored text, but it is a tool DESCRIPTOR's name field, and a
     * descriptor is written by many hands: a name containing "]\n[vfs_write
     * /etc/shadow -> ok]" would otherwise print bytes indistinguishable from a
     * genuine kernel line. tool.c also refuses such names outright; this is the
     * second line of defence, and the one that holds for non-tool callers. */
    char sop[128];
    sanitize(sop, sizeof sop, op);

    /* Format the caller's arguments, then make them safe to print. */
    char raw[TRACE_LINE_MAX];
    raw[0] = '\0';
    if (args_fmt && args_fmt[0])
        vsnprintf(raw, sizeof raw, args_fmt, ap);

    char args[TRACE_LINE_MAX];
    sanitize(args, sizeof args, raw);

    /* Budget the result tail FIRST and unconditionally. Everything else may be
     * elided; " -> <result>]\n" may not, because it is the part that makes the
     * line evidence. Both the op and the args are truncated against what is
     * left, so the invariant in trace.h ("one line, one leading '[', one
     * trailing ']\n'") holds for every possible input length. */
    char   line[TRACE_LINE_MAX];
    size_t reserve = 4 + zlen(result) + 2 + 1;      /* " -> " result "]\n" NUL */
    size_t avail   = (reserve + 2 < sizeof line) ? sizeof line - reserve - 1 : 0;

    /* '[' plus the op must fit inside avail; elide the op if it cannot. */
    size_t oplen = zlen(sop);
    if (oplen + 1 > avail) {
        size_t keep = (avail > 4) ? avail - 4 : 0;  /* room for "..." */
        sop[keep] = '\0';
        if (keep) { sop[keep - 1] = '.'; }          /* mark it as clipped */
        oplen = zlen(sop);
    }

    if (args[0] == '\0') {
        snprintf(line, sizeof line, "[%s -> %s]\n", sop, result);
    } else {
        size_t room = (avail > oplen + 1) ? avail - oplen - 1 : 0;
        size_t alen = zlen(args);
        if (alen > room) {
            if (room > 3) args[room - 3] = '\0';
            else          args[0] = '\0';
            if (args[0] == '\0')
                snprintf(line, sizeof line, "[%s -> %s]\n", sop, result);
            else
                snprintf(line, sizeof line, "[%s %s... -> %s]\n",
                         sop, args, result);
        } else {
            snprintf(line, sizeof line, "[%s %s -> %s]\n", sop, args, result);
        }
    }

    kputs(line);
    trace_written++;
}

void trace_ok(const char *op, const char *args_fmt, ...) {
    va_list ap;
    va_start(ap, args_fmt);
    emit(op, args_fmt, ap, "ok");
    va_end(ap);
}

void trace_ret(long ret, const char *op, const char *args_fmt, ...) {
    char res[32];
    snprintf(res, sizeof res, "%ld", ret);

    va_list ap;
    va_start(ap, args_fmt);
    emit(op, args_fmt, ap, res);
    va_end(ap);
}

void trace_err(int err, const char *op, const char *args_fmt, ...) {
    const char *name = trace_errname(err);
    char        res[32];
    if (name)          snprintf(res, sizeof res, "%s", name);
    else if (err == 0) snprintf(res, sizeof res, "%s", "error");
    else               snprintf(res, sizeof res, "%d", err);

    va_list ap;
    va_start(ap, args_fmt);
    emit(op, args_fmt, ap, res);
    va_end(ap);
}

uint32_t trace_count(void)          { return trace_seq; }
void     trace_reset(void)          { trace_seq = 0; }   /* NOT trace_written */
void     trace_set_enabled(int on)  { trace_is_on = on ? 1 : 0; }
int      trace_enabled(void)        { return trace_is_on; }
uint64_t trace_emissions(void)      { return trace_written; }
