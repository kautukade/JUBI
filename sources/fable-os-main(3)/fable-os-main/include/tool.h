/* tool.h — the model's syscall interface.
 *
 * PURPOSE
 *   Gives the model the ability to *act on this machine* rather than only print
 *   text about it. A tool is a kernel function the model may invoke by name: the
 *   VFS, the device registry, the heap allocator, PCI config space. When the
 *   model asks to write a file, real bytes land in a real filesystem, and the
 *   kernel prints a trace line proving it (see trace.h).
 *
 *   This is the difference between "a kernel with a chatbot in it" and an
 *   operating system whose interface is natural language. The model has syscalls.
 *
 * ARCHITECTURE
 *   Tools self-register through the linker, exactly like drivers do (driver.h).
 *   There is no central list to edit, which means a new tool family is a new .c
 *   file and nothing else — no shared file to serialise on:
 *
 *       static int my_tool(const tool_call_t *c, tool_result_t *r) { ... }
 *       static const tool_t my_tool_def = {
 *           .name         = "read_file",
 *           .description  = "Read a file from the filesystem.",
 *           .input_schema = "{\"type\":\"object\","
 *                           "\"properties\":{\"path\":{\"type\":\"string\"}},"
 *                           "\"required\":[\"path\"]}",
 *           .invoke       = my_tool,
 *       };
 *       REGISTER_TOOL(my_tool_def);
 *
 *   Add the source path to KERNEL_SRCS in the Makefile and it is live: exposed
 *   to the model in the request's "tools" array, and dispatchable by name.
 *
 * RESPONSIBILITIES
 *   - Registry: enumerate registered tools, look one up by name.
 *   - Schema assembly: build the "tools":[...] JSON array sent to the API, from
 *     the registered descriptors, so the advertised contract and the dispatch
 *     table can never drift apart.
 *   - Dispatch: route a tool_use block to its handler, enforce the result buffer
 *     bound, emit the trace line, and turn an unknown name into a legible error
 *     the model can recover from rather than a kernel fault.
 *
 * THE INPUT CONTRACT
 *   A handler receives the tool_use block's `input` object as a RAW JSON span
 *   (not a parsed structure), and reads fields with json.h. Raw is deliberate:
 *   every tool's arguments differ, the span costs no allocation, and json.h is
 *   already hardened and host-tested against malformed network data. Treat the
 *   span as untrusted — it is model output, and a confused model can send a path
 *   of 10,000 characters or a negative length.
 *
 * THE RESULT CONTRACT
 *   Handlers write plain text (or JSON text) into the caller-provided buffer via
 *   tool_result_printf. The text is what the model sees as the tool_result
 *   content, so it should be terse and factual — the model reasons over it. Set
 *   is_error on failure; the loop marks the block so the model knows the action
 *   did not happen and can try something else.
 *
 * DEPENDENCIES
 *   json.h (reading the input span, escaping output), trace.h (the ground-truth
 *   line), and whatever subsystem the individual tool drives. Nothing here
 *   depends on the transport, so the whole surface is testable against the mock
 *   in net/model_mock.c with no network.
 *
 * FUTURE EXTENSION POINTS
 *   `flags` carries per-tool policy. TOOL_MUTATES marks tools that change system
 *   state, which is the hook a future confirmation prompt or read-only mode would
 *   key off. Once processes and users exist, a permission field alongside it lets
 *   the same registry serve unprivileged callers. The screen tools (Phase 2), the
 *   fault-recovery tools (Phase 3), and the driver VM (Phase 4) all register here
 *   like any other tool — the loop never learns their names.
 */
#ifndef TOOL_H
#define TOOL_H

#include <stdint.h>
#include <stddef.h>

#define TOOL_NAME_MAX     48
#define TOOL_ID_MAX       64      /* Anthropic tool_use ids, e.g. "toolu_..." */
#define TOOL_RESULT_MAX   4096    /* per-call result text budget */

/* ---- per-tool policy flags ---- */
#define TOOL_MUTATES      0x1     /* changes system state (write, unlink, poke) */

/* ---- error codes (errno-style negatives, shared space with vfs.h) ---- */
#define TOOL_OK           0
#define TOOL_ENOENT      -2       /* no such tool registered  */
#define TOOL_EINVAL     -22       /* bad/missing arguments    */
#define TOOL_ENOSPC     -28       /* result did not fit       */

/* One invocation, decoded from a tool_use content block. */
typedef struct tool_call {
    const char *id;          /* tool_use id; echoed back in the tool_result */
    const char *name;        /* tool name the model asked for               */
    const char *input;       /* RAW JSON object span — NOT NUL-terminated   */
    size_t      input_len;   /* length of that span                         */
} tool_call_t;

/* Where a handler writes its answer. The buffer belongs to the caller. */
typedef struct tool_result {
    char   *buf;
    size_t  cap;
    size_t  len;             /* bytes written so far */
    int     is_error;        /* nonzero => the action did not happen */
    int     truncated;       /* set when output was clipped to cap   */
} tool_result_t;

/* A registered tool. `input_schema` is a JSON Schema object, verbatim, as the
 * API's tool definition expects. */
typedef struct tool {
    const char *name;
    const char *description;
    const char *input_schema;
    uint32_t    flags;
    int       (*invoke)(const tool_call_t *call, tool_result_t *out);
} tool_t;

/* Place a pointer to the tool in the "tool_table" section. KEEP() in the linker
 * script stops it being garbage-collected. Mirrors REGISTER_DRIVER. */
#define REGISTER_TOOL(var)                                                     \
    static const tool_t *const __toolptr_##var                                 \
        __attribute__((used, section("tool_table"))) = &(var)

/* ---- registry ---- */
int           tool_count(void);
const tool_t *tool_at(int index);
const tool_t *tool_find(const char *name);

/* Build the request's "tools" JSON array (the bare `[...]`, no key) from every
 * registered tool. Returns the length that WOULD be written, like snprintf, so
 * the caller can detect truncation. */
size_t tools_build_schema(char *dst, size_t cap);

/* ---- dispatch ---- */

/* Initialise a result over a caller-owned buffer. */
void tool_result_init(tool_result_t *r, char *buf, size_t cap);

/* Append formatted text to a result, respecting cap and setting `truncated`.
 * Returns the number of bytes appended. */
size_t tool_result_printf(tool_result_t *r, const char *fmt, ...);

/* Look up `call->name`, invoke it, and emit the trace line. Returns TOOL_OK when
 * the handler ran (even if the handler itself reported a logical failure via
 * out->is_error), or TOOL_ENOENT when no such tool exists — in which case a
 * legible message is written into `out` for the model to read. */
int tool_dispatch(const tool_call_t *call, tool_result_t *out);

#endif /* TOOL_H */
