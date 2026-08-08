/* script.c — a scripted (canned) input source.
 *
 * An input source with nothing behind it but a byte queue. Push text in and it
 * comes back out of input_readline() indistinguishably from someone typing it,
 * which is what makes natural-language turns testable with no hardware, no
 * QEMU, and no keyboard. Host unit tests use it directly; a future in-kernel
 * boot self-test can queue a question and let the normal turn loop answer it.
 *
 * It is *not* registered automatically — a caller has to ask for the singleton
 * and register it, so a production boot has no phantom input device.
 */

#include "input.h"

#define SCRIPT_CAP 8192          /* bytes of queued text */

static char script_q[SCRIPT_CAP];
static int  script_head;         /* next byte to hand out */
static int  script_tail;         /* next free slot        */

static int script_getc(input_source_t *self) {
    (void)self;
    if (script_head >= script_tail) return INPUT_NONE;
    return (unsigned char)script_q[script_head++];
}

/* Echo is on: a scripted turn should produce exactly the same console output a
 * typed one would, so tests can assert on the transcript via kcap_text(). */
static input_source_t script_src = {
    .name    = "script",
    .getc_nb = script_getc,
    .priv    = NULL,
    .flags   = INPUT_ECHO,
};

input_source_t *input_script_source(void) { return &script_src; }

int input_script_pending(void) { return script_tail - script_head; }

void input_script_reset(void) {
    script_head = script_tail = 0;
    input_line_reset(&script_src.ed);
}

/* Slide unconsumed bytes down so a long test run doesn't exhaust the queue. */
static void script_compact(void) {
    int n = script_tail - script_head;
    for (int i = 0; i < n; i++) script_q[i] = script_q[script_head + i];
    script_head = 0;
    script_tail = n;
}

static int script_append(const char *s, int extra_nl) {
    if (!s) return -1;
    int n = 0;
    while (s[n]) n++;

    if (script_tail + n + extra_nl > SCRIPT_CAP) script_compact();
    if (script_tail + n + extra_nl > SCRIPT_CAP) return -1;   /* queue full */

    for (int i = 0; i < n; i++) script_q[script_tail++] = s[i];
    if (extra_nl) script_q[script_tail++] = '\n';
    return n + extra_nl;
}

int input_script_push(const char *text)     { return script_append(text, 0); }
int input_script_pushline(const char *line) { return script_append(line, 1); }
