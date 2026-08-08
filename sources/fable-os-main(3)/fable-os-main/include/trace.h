/* trace.h — kernel-emitted ground truth for model actions.
 *
 * PURPOSE
 *   This machine has no shell. There is no `ls`, no `cat`, no command language —
 *   the only human interface is a sentence in natural language. That creates one
 *   hard problem: when the model says "I wrote that file", how does the operator
 *   know it actually happened rather than being narrated convincingly?
 *
 *   Trace lines are the answer. Every tool the model invokes emits a line
 *   printed *by the kernel*, in C, from the real return value, after the call
 *   completed. The model never produces these bytes and cannot forge them.
 *
 *   Read a session transcript as two interleaved voices:
 *       ai> I'll write that now.                 <- the model, prose, persuasive
 *       [vfs_write fd=3 bytes=34 -> ok]          <- the kernel, fact, verifiable
 *       ai> Done - 34 bytes at /etc/motd.        <- the model again
 *
 *   The bracketed lines are also what replaces `ls` in a world with no commands:
 *   the operator never types anything to inspect the system, but still sees
 *   exactly which kernel state was touched.
 *
 * RESPONSIBILITIES
 *   - One consistent, machine-parseable line format for every tool invocation.
 *   - Render error codes symbolically (ENOENT, not -2) so both the operator and
 *     the model get a legible failure.
 *   - Count emissions so tests can assert "exactly N kernel actions happened",
 *     which is how a claimed action is distinguished from a real one.
 *
 * PUBLIC API
 *   trace_ok / trace_ret / trace_err  — emit one line; the result comes first as
 *   a fixed argument and the call's arguments are the varargs tail, so the
 *   "-> result" arrow is produced by this module and is always consistent.
 *
 *   trace_count / trace_reset        — assertion hooks for tests.
 *   trace_set_enabled                — silence tracing (e.g. while replaying a
 *                                      golden transcript) without touching call
 *                                      sites.
 *
 * DEPENDENCIES
 *   kernel.h (kputs) and vsnprintf from the libc shim. No hardware, no heap, no
 *   allocation — safe to call from any context, including (later) from inside an
 *   exception handler while reporting a fault.
 *
 * FUTURE EXTENSION POINTS
 *   Output currently goes to the shared console. Once the turn loop routes
 *   replies per input source (see input.h's input_last_source), trace lines can
 *   follow the same channel so a serial-driven test gets a clean transcript
 *   while the VGA console keeps the human view. A monotonic sequence number is
 *   already carried per line, which is what a future structured/JSON trace sink
 *   or an on-disk audit log would key off.
 */
#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>
#include <stddef.h>

/* Longest single rendered trace line, including the brackets and newline. */
#define TRACE_LINE_MAX 512

/* FORGERY RESISTANCE
 *   Trace arguments are model-controlled in almost every real tool (a path, a
 *   device name). If they were rendered verbatim, a path containing a newline
 *   followed by "[vfs_write ... -> ok]" would emit bytes indistinguishable from
 *   a genuine kernel line, which would defeat the entire point of this module.
 *   So the renderer escapes every control character AND both bracket characters
 *   inside the argument field as \xNN. The invariant callers may rely on:
 *
 *       exactly one line, one leading '[', one trailing ']\n', per invocation,
 *       and no raw '[' or ']' anywhere inside it.
 *
 *   Result and operation names come from kernel source, not from the model. */

/* Emit `[<op> <args> -> ok]`. Use when success carries no interesting value. */
void trace_ok(const char *op, const char *args_fmt, ...);

/* Emit `[<op> <args> -> <ret>]`. Use when the return value is the point
 * (bytes written, a file handle, a count). */
void trace_ret(long ret, const char *op, const char *args_fmt, ...);

/* Emit `[<op> <args> -> <SYMBOL>]`, e.g. `-> ENOENT`. Unknown codes render as
 * the signed integer so a new error can never be silently swallowed. */
void trace_err(int err, const char *op, const char *args_fmt, ...);

/* Map a kernel error code to its symbolic name ("ENOENT"). Returns NULL when
 * the code is unknown, so callers can fall back to printing the number. */
const char *trace_errname(int err);

/* ---- test / control hooks ---- */
/* WARNING: trace_count() is a convenience for tests and is NOT proof that
 * anything was printed — it advances even while output is suppressed, and
 * trace_reset() moves it backwards. Anything that needs to *prove* a line
 * reached the console must use trace_emissions() below. */

uint32_t trace_count(void);        /* lines requested since the last reset */
void     trace_reset(void);        /* zero the request counter (not emissions) */
void     trace_set_enabled(int on);/* 0 suppresses output; request count still runs */
int      trace_enabled(void);

/* Monotonic count of trace lines actually WRITTEN to the console. Never reset,
 * never advanced while suppressed, and not affected by trace_reset(). This is
 * the only counter suitable for proving an action produced ground truth — see
 * tool_dispatch(), which relies on exactly that property. */
uint64_t trace_emissions(void);

#endif /* TRACE_H */
