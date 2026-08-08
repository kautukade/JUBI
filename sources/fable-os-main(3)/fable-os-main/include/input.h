/* input.h — input sources: turning bytes from anywhere into a line of text.
 *
 * PURPOSE
 *   fable-os has no shell and no commands. The only way a human drives this
 *   machine is by typing a sentence and pressing Enter, so "a completed line of
 *   natural-language text" is a first-class kernel primitive — not a detail of
 *   some REPL. This header is that primitive, and it is deliberately decoupled
 *   from *where* the characters came from: a PS/2 keyboard on real hardware, a
 *   COM1 UART when the machine is driven over a serial cable (or by a test
 *   script through QEMU), or a canned script with no hardware at all.
 *
 * ARCHITECTURE
 *   Three layers, mirroring the VFS split:
 *     - The public API below (input_readline / input_poll): what the rest of
 *       the kernel calls. It never knows which device produced the line.
 *     - input_source_t: the per-device interface. A source implements exactly
 *       one method — a non-blocking `getc_nb` that yields the next raw byte or
 *       INPUT_NONE — and registers itself. Everything else is shared.
 *     - The line editor (input_line_t): the shared, hardware-free logic that
 *       assembles raw bytes into a line (echo, backspace, CR/LF/CRLF, overflow
 *       truncation). It is pure C with no port I/O, which is what lets it be
 *       unit-tested on the host in tests/host/test_input.c.
 *
 * RESPONSIBILITIES
 *   - Registry of input sources and arbitration between them.
 *   - Line assembly: echo, destructive backspace (both 0x08 and DEL 0x7F),
 *     CR / LF / CRLF termination, and safe truncation of over-long lines.
 *   - Never overflowing the caller's fixed-size buffer, whatever arrives.
 *
 * ARBITRATION POLICY
 *   Round-robin. input_poll() visits every registered source starting one past
 *   the source that produced the previous line, draining at most
 *   INPUT_DRAIN_BUDGET bytes from each before moving on, and returns the first
 *   line that completes. A source that never goes quiet therefore cannot starve
 *   the others, and partial lines are held per-source, so two people typing on
 *   the keyboard and the serial line at once do not interleave into one line.
 *
 * BLOCKING MODEL
 *   Polled and non-blocking, like the rest of this kernel: there are no
 *   interrupts and no IDT. input_poll() never waits; input_readline() is the
 *   convenience blocking wrapper that spins on input_poll() with a `pause`.
 *
 * PUBLIC API
 *   input_register_source / input_unregister_source  — plug a device in.
 *   input_poll / input_readline                      — get a line out.
 *   input_last_source / input_line_was_truncated     — provenance & health.
 *   input_line_init / _push / _take / _reset         — the reusable editor.
 *   input_script_*                                   — the scripted test source.
 *
 * DEPENDENCIES
 *   kernel.h only (kputc/kputs for echo). No hardware, no heap, no port I/O —
 *   sources are registered from static storage, so input works from the moment
 *   the first driver comes up and compiles unchanged on the host.
 *
 * FUTURE EXTENSION POINTS
 *   Sources are just structs with a getc_nb, so a USB HID keyboard, a network
 *   console, or a microphone + speech-to-text front end plug in with no change
 *   here. When an IDT exists, an interrupt-driven source keeps this interface:
 *   the ISR fills a ring, getc_nb drains it, and only the blocking spin in
 *   input_readline() becomes an idle/wait. The per-source echo flag and the
 *   input_last_source() hook are what a future turn loop needs to answer back
 *   on the same channel a question arrived on.
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stddef.h>

/* Longest line we will assemble, including the NUL. Matches the prompt buffer
 * in kernel/main.c; anything longer is truncated, never overflowed. */
#define INPUT_LINE_MAX 4096

/* Returned by a source's getc_nb() when no byte is available right now. */
#define INPUT_NONE (-1)

/* Max bytes input_poll() drains from one source before moving to the next, so
 * a chatty source cannot monopolise the loop. */
#define INPUT_DRAIN_BUDGET 512

/* Source flags. */
#define INPUT_ECHO 0x1   /* echo accepted characters to the console */

/* ---- the shared line editor ------------------------------------------
 * Hardware-free byte-to-line assembly. Feed it bytes; it tells you when a line
 * is done. `buf` is caller-owned and is kept NUL-terminated at all times. */
typedef struct input_line {
    char         *buf;         /* caller-owned storage                       */
    int           cap;         /* capacity of buf, including the NUL         */
    int           len;         /* characters currently held                  */
    unsigned char echo;        /* nonzero: echo to the console as we go      */
    unsigned char truncated;   /* set once the line overflowed cap-1         */
    unsigned char eat_lf;      /* a CR just ended a line: swallow one LF      */
} input_line_t;

/* Bind an editor to storage. cap must be >= 2. Clears any previous state. */
void input_line_init(input_line_t *ed, char *buf, int cap, int echo);

/* Discard the partial line and all edit state. */
void input_line_reset(input_line_t *ed);

/* Feed one raw byte. Returns 1 when the byte completed a line (the line is then
 * in ed->buf, NUL-terminated, and ed->len is its length), 0 otherwise.
 *
 * Handling, in order:
 *   '\n' or '\r'  end the line; a '\n' directly after a '\r' is swallowed so
 *                 CR, LF and CRLF all mean exactly one line ending.
 *   '\b' / 0x7F   destructive backspace; a no-op on an empty line.
 *   '\0'          dropped — a NUL can never be part of a NUL-terminated line,
 *                 and silently truncating the string later would be worse.
 *   anything else appended if there is room, otherwise dropped (and the line is
 *                 flagged truncated). Dropped characters are not echoed, so the
 *                 display never disagrees with the buffer. */
int input_line_push(input_line_t *ed, int c);

/* Copy the completed line into `out` (at most cap-1 chars plus a NUL), clear the
 * editor's buffer, and return the number of characters copied. Copying into a
 * smaller buffer truncates rather than overflows. Safe with out == NULL.
 *
 * The `truncated` and `eat_lf` flags deliberately survive the take, so the
 * caller can read `truncated` afterwards and so a CR/LF pair split across two
 * calls still counts as one line ending. input_line_reset() clears both. */
int input_line_take(input_line_t *ed, char *out, int cap);

/* ---- input sources ---------------------------------------------------- */
struct input_source;

/* A source of raw bytes. The one method must never block: return the next byte
 * (0..255) if one is available, or INPUT_NONE if not. */
typedef int (*input_getc_fn)(struct input_source *self);

typedef struct input_source {
    const char         *name;      /* "kbd", "serial", "script"              */
    input_getc_fn       getc_nb;   /* non-blocking byte source               */
    void               *priv;      /* implementation-private state           */
    uint32_t            flags;     /* INPUT_ECHO                             */

    /* --- private to drivers/input/input.c; do not touch from a source --- */
    struct input_source *next;
    input_line_t         ed;
    char                 buf[INPUT_LINE_MAX];
} input_source_t;

/* Register/unregister a source. The struct must have static lifetime; the
 * registry stores the pointer, not a copy. Registering the same source twice is
 * a no-op. Returns 0 on success, -1 on a bad argument. */
int input_register_source(input_source_t *src);
int input_unregister_source(input_source_t *src);

/* Non-blocking. Returns the length of a completed line copied into buf (0 for
 * an empty line — the user just pressed Enter), or INPUT_NONE if no source has
 * a full line yet. buf is always NUL-terminated when >= 0 is returned. */
int input_poll(char *buf, int cap);

/* Blocking. Spins on input_poll() until a line arrives and returns its length.
 * Returns INPUT_NONE immediately if no sources are registered, so a caller can
 * never deadlock on a machine with no input device. */
int input_readline(char *buf, int cap);

/* Which source produced the line most recently returned, or NULL. A future turn
 * loop uses this to reply on the channel the question came in on. */
input_source_t *input_last_source(void);

/* Whether the line most recently returned hit the buffer limit and lost
 * characters. Cleared at the start of each new line. */
int input_line_was_truncated(void);

/* Number of registered sources — mostly for boot logging and tests. */
int input_source_count(void);

/* ---- scripted source (tests) ------------------------------------------
 * A source with no hardware behind it: push text in, and it comes back out of
 * input_readline() exactly as if someone had typed it. This is what lets a host
 * test — or a future scripted boot test — drive natural-language turns.
 *
 * The caller decides whether it is live: get the singleton and register it. */
input_source_t *input_script_source(void);

/* Queue raw text, which may contain any number of line endings. Returns the
 * number of bytes queued, or -1 if the queue is full (nothing is queued). */
int input_script_push(const char *text);

/* Queue text followed by '\n' — one complete typed line. Same return. */
int input_script_pushline(const char *line);

/* Bytes still waiting to be consumed. */
int input_script_pending(void);

/* Drop everything queued and clear the source's partial line. */
void input_script_reset(void);

#endif /* INPUT_H */
