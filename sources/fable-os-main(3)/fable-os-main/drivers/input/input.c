/* input.c — the input-source registry, arbitration, and the shared line editor.
 *
 * See include/input.h for the architecture. Nothing in this file touches
 * hardware: sources hand it bytes, it hands the kernel lines. That is what lets
 * the same object be compiled by the host compiler for tests/host/test_input.c.
 */

#include "input.h"
#include "kernel.h"

/* Spin hint for the blocking wrapper. Guarded because the host test build runs
 * on whatever the developer's machine is (Apple silicon included). */
static inline void input_idle(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause");
#endif
}

/* ====================================================================== */
/* Line editor                                                            */
/* ====================================================================== */

void input_line_init(input_line_t *ed, char *buf, int cap, int echo) {
    if (!ed) return;
    ed->buf  = buf;
    ed->cap  = (buf && cap >= 2) ? cap : 0;   /* cap 0 == accept nothing */
    ed->echo = echo ? 1 : 0;
    input_line_reset(ed);
}

void input_line_reset(input_line_t *ed) {
    if (!ed) return;
    ed->len       = 0;
    ed->truncated = 0;
    ed->eat_lf    = 0;
    if (ed->buf && ed->cap > 0) ed->buf[0] = '\0';
}

int input_line_push(input_line_t *ed, int c) {
    if (!ed || ed->cap <= 0) return 0;
    c &= 0xFF;

    /* CRLF: a LF immediately behind a CR belongs to the line ending we already
     * consumed, so it must not produce a second (empty) line. */
    if (ed->eat_lf) {
        ed->eat_lf = 0;
        if (c == '\n') return 0;
    }

    if (c == '\n' || c == '\r') {
        if (c == '\r') ed->eat_lf = 1;
        if (ed->echo) kputc('\n');
        ed->buf[ed->len] = '\0';
        return 1;
    }

    if (c == '\b' || c == 0x7F) {              /* destructive backspace */
        if (ed->len > 0) {
            ed->len--;
            ed->buf[ed->len] = '\0';
            if (ed->echo) kputs("\b \b");
        }
        return 0;
    }

    /* A NUL can never live inside a NUL-terminated line; dropping it here beats
     * silently truncating the string in whatever consumes it later. */
    if (c == '\0') return 0;

    if (ed->len < ed->cap - 1) {
        ed->buf[ed->len++] = (char)c;
        ed->buf[ed->len]   = '\0';
        if (ed->echo) kputc((char)c);
    } else {
        /* Full. Drop the byte rather than overflow, and do not echo it — the
         * screen must never claim to hold a character the buffer does not. */
        ed->truncated = 1;
    }
    return 0;
}

int input_line_take(input_line_t *ed, char *out, int cap) {
    if (!ed) return 0;
    int n = ed->len;
    if (out && cap > 0) {
        if (n > cap - 1) { n = cap - 1; ed->truncated = 1; }
        for (int i = 0; i < n; i++) out[i] = ed->buf[i];
        out[n] = '\0';
    } else {
        n = 0;
    }
    /* Clear the line but keep the two flags the caller may want to read back:
     * `truncated` (did this line lose characters?) and `eat_lf` (the CR that
     * ended it may be followed by its LF on the very next byte). Both are
     * cleared by input_line_reset() or when the next line starts. */
    ed->len = 0;
    if (ed->buf && ed->cap > 0) ed->buf[0] = '\0';
    return n;
}

/* ====================================================================== */
/* Source registry                                                        */
/* ====================================================================== */

static input_source_t *srcs;        /* singly-linked, static-lifetime nodes */
static input_source_t *rr;          /* round-robin cursor: try this one first */
static input_source_t *last_src;    /* produced the most recent line          */
static int             last_trunc;  /* ...and whether it lost characters      */
static int             nsrcs;

int input_register_source(input_source_t *src) {
    if (!src || !src->getc_nb) return -1;
    for (input_source_t *s = srcs; s; s = s->next)
        if (s == src) return 0;                    /* already in: no-op */

    input_line_init(&src->ed, src->buf, INPUT_LINE_MAX,
                    (src->flags & INPUT_ECHO) != 0);
    src->next = NULL;

    input_source_t **tail = &srcs;
    while (*tail) tail = &(*tail)->next;
    *tail = src;
    if (!rr) rr = src;
    nsrcs++;
    return 0;
}

int input_unregister_source(input_source_t *src) {
    if (!src) return -1;
    for (input_source_t **p = &srcs; *p; p = &(*p)->next) {
        if (*p == src) {
            *p = src->next;
            src->next = NULL;
            nsrcs--;
            if (rr == src)       rr = srcs;
            if (last_src == src) last_src = NULL;
            return 0;
        }
    }
    return -1;
}

int input_source_count(void) { return nsrcs; }

input_source_t *input_last_source(void) { return last_src; }

int input_line_was_truncated(void) { return last_trunc; }

/* ====================================================================== */
/* Arbitration                                                            */
/* ====================================================================== */

static input_source_t *next_after(input_source_t *s) {
    return (s && s->next) ? s->next : srcs;
}

int input_poll(char *buf, int cap) {
    if (!srcs || !buf || cap <= 0) return INPUT_NONE;
    if (!rr) rr = srcs;

    input_source_t *s = rr;
    for (int visited = 0; visited < nsrcs; visited++, s = next_after(s)) {
        for (int budget = 0; budget < INPUT_DRAIN_BUDGET; budget++) {
            int c = s->getc_nb(s);
            if (c == INPUT_NONE) break;
            if (!input_line_push(&s->ed, c)) continue;

            int n = input_line_take(&s->ed, buf, cap);
            last_trunc = s->ed.truncated;
            s->ed.truncated = 0;
            last_src = s;
            rr = next_after(s);      /* fairness: someone else goes first next */
            return n;
        }
    }
    /* Nobody completed a line; resume from where we stopped next time. */
    rr = s;
    return INPUT_NONE;
}

int input_readline(char *buf, int cap) {
    if (buf && cap > 0) buf[0] = '\0';
    if (!srcs) return INPUT_NONE;      /* no input device: never deadlock */
    for (;;) {
        int n = input_poll(buf, cap);
        if (n != INPUT_NONE) return n;
        input_idle();
    }
}
