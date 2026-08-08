/* test_input.c — exercises the real drivers/input/input.c + script.c on the host.
 *
 * The input layer takes untrusted, arbitrary-length bytes from a serial line and
 * assembles them into a fixed-size buffer, so most of what follows is
 * adversarial: over-long lines, lines longer than the caller's buffer, embedded
 * NULs, backspacing past the start, and mixed CR/LF conventions. The rest covers
 * the arbitration policy that lets a test script and a human share the machine.
 *
 * Nothing here touches hardware — that is the whole point of input_source_t.
 */

#include "harness.h"
#include "input.h"

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

/* A test source: a byte queue we can feed at will, standing in for a UART. */
#define FEED_CAP 16384
typedef struct {
    char q[FEED_CAP];
    int  head, tail;
} feed_t;

static int feed_getc(input_source_t *self) {
    feed_t *f = (feed_t *)self->priv;
    if (f->head >= f->tail) return INPUT_NONE;
    return (unsigned char)f->q[f->head++];
}

static void feed_push(feed_t *f, const char *s) {
    while (*s && f->tail < FEED_CAP) f->q[f->tail++] = *s++;
}

static void feed_push_n(feed_t *f, char c, int n) {
    while (n-- > 0 && f->tail < FEED_CAP) f->q[f->tail++] = c;
}

static void feed_clear(feed_t *f) { f->head = f->tail = 0; }

static feed_t feed_a, feed_b;
static input_source_t src_a = { .name = "a", .getc_nb = feed_getc,
                                .priv = &feed_a, .flags = INPUT_ECHO };
static input_source_t src_b = { .name = "b", .getc_nb = feed_getc,
                                .priv = &feed_b, .flags = 0 /* no echo */ };

/* Every test starts from an empty registry: the registry is global state and
 * these tests deliberately register different combinations. */
static void registry_clear(void) {
    input_unregister_source(&src_a);
    input_unregister_source(&src_b);
    input_unregister_source(input_script_source());
    feed_clear(&feed_a);
    feed_clear(&feed_b);
    input_script_reset();
    input_line_reset(&src_a.ed);
    input_line_reset(&src_b.ed);
    kcap_reset();
}

/* Push a whole string through an editor, returning how many lines completed. */
static int push_str(input_line_t *ed, const char *s) {
    int lines = 0;
    for (; *s; s++) lines += input_line_push(ed, (unsigned char)*s);
    return lines;
}

/* ====================================================================== */
/* Line editor: assembly and echo                                         */
/* ====================================================================== */

static void test_basic_line_assembly(void) {
    char buf[64];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);

    CHECK_EQ(push_str(&ed, "what is the capital of France"), 0);
    CHECK_EQ(ed.len, 29);
    CHECK_EQ(input_line_push(&ed, '\n'), 1);
    CHECK_STR(ed.buf, "what is the capital of France");

    char out[64];
    CHECK_EQ(input_line_take(&ed, out, sizeof out), 29);
    CHECK_STR(out, "what is the capital of France");
    CHECK_EQ(ed.len, 0);            /* take() clears the line */
    CHECK_EQ(ed.truncated, 0);
}

static void test_buffer_is_always_nul_terminated(void) {
    char buf[16];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    CHECK_STR(ed.buf, "");
    push_str(&ed, "ab");
    CHECK_STR(ed.buf, "ab");
    input_line_push(&ed, '\b');
    CHECK_STR(ed.buf, "a");         /* backspace must retract the NUL too */
}

static void test_echo_matches_buffer(void) {
    char buf[64];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 1 /* echo */);
    kcap_reset();
    push_str(&ed, "hi there\n");
    CHECK_STR(kcap_text(), "hi there\n");
}

static void test_no_echo_when_disabled(void) {
    char buf[64];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    kcap_reset();
    push_str(&ed, "silent\n");
    CHECK_STR(kcap_text(), "");
    CHECK_STR(ed.buf, "silent");
}

/* ====================================================================== */
/* Backspace / editing                                                    */
/* ====================================================================== */

static void test_backspace_edits(void) {
    char buf[64];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    push_str(&ed, "heXlo\b\b\bllo");   /* type it wrong, rub out three, retype */
    CHECK_EQ(input_line_push(&ed, '\n'), 1);
    CHECK_STR(ed.buf, "hello");
    CHECK_EQ(ed.len, 5);
}

static void test_backspace_del_variant(void) {
    /* Terminals send DEL (0x7F) for Backspace far more often than 0x08. */
    char buf[64];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    push_str(&ed, "abc");
    input_line_push(&ed, 0x7F);
    input_line_push(&ed, '\n');
    CHECK_STR(ed.buf, "ab");
}

static void test_backspace_past_start_is_a_noop(void) {
    char buf[64];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 1);
    kcap_reset();
    for (int i = 0; i < 100; i++) input_line_push(&ed, '\b');
    CHECK_EQ(ed.len, 0);
    CHECK_STR(kcap_text(), "");     /* nothing to erase => no erase sequence */
    push_str(&ed, "ok\n");
    CHECK_STR(ed.buf, "ok");
}

static void test_backspace_echo_sequence(void) {
    char buf[64];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 1);
    kcap_reset();
    push_str(&ed, "ab\b");
    CHECK_STR(kcap_text(), "ab\b \b");   /* the classic destructive erase */
}

static void test_backspace_frees_room_after_truncation(void) {
    /* Truncation is sticky for the line, but the freed slot is reusable. */
    char buf[5];                          /* 4 usable characters */
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    push_str(&ed, "abcdefg");
    CHECK_STR(ed.buf, "abcd");
    CHECK_EQ(ed.truncated, 1);
    input_line_push(&ed, '\b');
    input_line_push(&ed, 'Z');
    CHECK_STR(ed.buf, "abcZ");
}

/* ====================================================================== */
/* Overflow — the adversarial cases                                       */
/* ====================================================================== */

/* Canaries either side of the line buffer catch a one-byte overrun, which is
 * exactly the bug class a fixed buffer fed by a serial line invites. */
static void test_overflow_never_writes_out_of_bounds(void) {
    struct { char pre[16]; char buf[16]; char post[16]; } g;
    memset(&g, 0x5A, sizeof g);

    input_line_t ed;
    input_line_init(&ed, g.buf, (int)sizeof g.buf, 0);

    for (int i = 0; i < 100000; i++) input_line_push(&ed, 'A' + (i % 26));
    CHECK_EQ(input_line_push(&ed, '\n'), 1);

    CHECK_EQ(ed.len, 15);                    /* cap-1 */
    CHECK_EQ((int)strlen(g.buf), 15);
    CHECK_EQ(ed.truncated, 1);
    for (int i = 0; i < 16; i++) {
        CHECK_EQ((unsigned char)g.pre[i], 0x5A);
        CHECK_EQ((unsigned char)g.post[i], 0x5A);
    }
}

static void test_overflow_does_not_echo_dropped_chars(void) {
    /* If the screen showed characters the buffer dropped, the user would be
     * editing something that isn't there. */
    char buf[6];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 1);
    kcap_reset();
    push_str(&ed, "0123456789\n");
    CHECK_STR(kcap_text(), "01234\n");
    CHECK_STR(ed.buf, "01234");
}

static void test_truncated_flag_clears_on_reset(void) {
    char buf[4];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    push_str(&ed, "abcdef\n");
    CHECK_EQ(ed.truncated, 1);
    input_line_reset(&ed);
    CHECK_EQ(ed.truncated, 0);
    push_str(&ed, "xy\n");
    CHECK_EQ(ed.truncated, 0);
    CHECK_STR(ed.buf, "xy");
}

static void test_take_into_smaller_buffer_truncates(void) {
    char buf[64], small[5];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    push_str(&ed, "abcdefghij\n");
    CHECK_EQ(ed.truncated, 0);               /* fits the editor fine... */
    CHECK_EQ(input_line_take(&ed, small, (int)sizeof small), 4);
    CHECK_STR(small, "abcd");                /* ...but not the caller's buffer */
    CHECK_EQ(ed.truncated, 1);
}

static void test_take_is_safe_with_null(void) {
    char buf[16];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    push_str(&ed, "hi\n");
    CHECK_EQ(input_line_take(&ed, NULL, 0), 0);
    CHECK_EQ(ed.len, 0);
}

static void test_degenerate_capacities(void) {
    /* cap < 2 leaves no room for a character plus a NUL; the editor must accept
     * nothing rather than write anywhere. */
    char one[1] = { 0x7F };
    input_line_t ed;
    input_line_init(&ed, one, 1, 0);
    CHECK_EQ(input_line_push(&ed, 'x'), 0);
    CHECK_EQ(input_line_push(&ed, '\n'), 0);
    CHECK_EQ((unsigned char)one[0], 0x7F);   /* untouched */

    input_line_init(&ed, NULL, 64, 0);       /* NULL storage */
    CHECK_EQ(input_line_push(&ed, 'x'), 0);
    CHECK_EQ(input_line_push(&ed, '\n'), 0);

    input_line_push(NULL, 'x');              /* must not crash */
    input_line_reset(NULL);
    CHECK_EQ(input_line_take(NULL, one, 1), 0);
}

/* ====================================================================== */
/* Line endings: CR vs LF vs CRLF                                         */
/* ====================================================================== */

static void test_lf_line_ending(void) {
    char buf[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    CHECK_EQ(push_str(&ed, "one\ntwo\n"), 2);
}

static void test_cr_line_ending(void) {
    char buf[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    CHECK_EQ(push_str(&ed, "one\r"), 1);
    CHECK_STR(ed.buf, "one");
}

static void test_crlf_is_one_line_ending(void) {
    char buf[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    /* A DOS/telnet client sends CRLF; that must be one line, not one plus an
     * empty one. */
    CHECK_EQ(push_str(&ed, "hello\r\n"), 1);
    CHECK_STR(ed.buf, "hello");
    CHECK_EQ(push_str(&ed, "world\r\n"), 1);
}

static void test_crlf_split_across_takes(void) {
    /* The CR completes a line, the caller takes it, and only then does the LF
     * arrive. It must still be swallowed. */
    char buf[32], out[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    CHECK_EQ(push_str(&ed, "ask\r"), 1);
    CHECK_EQ(input_line_take(&ed, out, sizeof out), 3);
    CHECK_STR(out, "ask");
    CHECK_EQ(input_line_push(&ed, '\n'), 0);      /* swallowed, not a new line */
    CHECK_EQ(push_str(&ed, "next\n"), 1);
    CHECK_STR(ed.buf, "next");
}

static void test_lone_lf_after_text_is_not_swallowed(void) {
    /* eat_lf must only apply immediately after a CR. */
    char buf[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    CHECK_EQ(push_str(&ed, "a\rb\n"), 2);
}

static void test_lfcr_yields_two_lines(void) {
    /* LF then CR is not a convention anyone uses; treating it as two endings is
     * the honest reading and is documented behaviour. */
    char buf[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    CHECK_EQ(push_str(&ed, "x\n\r"), 2);
}

static void test_empty_lines(void) {
    char buf[32], out[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    CHECK_EQ(input_line_push(&ed, '\n'), 1);
    CHECK_EQ(input_line_take(&ed, out, sizeof out), 0);
    CHECK_STR(out, "");
    CHECK_EQ(push_str(&ed, "\n\n\n"), 3);
}

static void test_nul_is_dropped(void) {
    /* A NUL inside the line would silently truncate every consumer of the
     * string; it must never make it into the buffer. */
    char buf[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    input_line_push(&ed, 'a');
    input_line_push(&ed, '\0');
    input_line_push(&ed, 'b');
    input_line_push(&ed, '\n');
    CHECK_STR(ed.buf, "ab");
    CHECK_EQ(ed.len, 2);
}

static void test_high_bytes_survive(void) {
    /* UTF-8 questions must pass through untouched. */
    char buf[32];
    input_line_t ed;
    input_line_init(&ed, buf, sizeof buf, 0);
    push_str(&ed, "caf\xC3\xA9\n");
    CHECK_STR(ed.buf, "caf\xC3\xA9");
    CHECK_EQ(ed.len, 5);
}

/* ====================================================================== */
/* Source registry                                                        */
/* ====================================================================== */

static void test_register_and_count(void) {
    registry_clear();
    CHECK_EQ(input_source_count(), 0);
    CHECK_EQ(input_register_source(&src_a), 0);
    CHECK_EQ(input_source_count(), 1);
    CHECK_EQ(input_register_source(&src_a), 0);   /* duplicate is a no-op */
    CHECK_EQ(input_source_count(), 1);
    CHECK_EQ(input_register_source(&src_b), 0);
    CHECK_EQ(input_source_count(), 2);
    CHECK_EQ(input_unregister_source(&src_a), 0);
    CHECK_EQ(input_source_count(), 1);
    CHECK_EQ(input_unregister_source(&src_a), -1); /* not registered */
    registry_clear();
    CHECK_EQ(input_source_count(), 0);
}

static void test_register_rejects_bad_source(void) {
    static input_source_t broken = { .name = "broken", .getc_nb = NULL };
    CHECK_EQ(input_register_source(NULL), -1);
    CHECK_EQ(input_register_source(&broken), -1);  /* no getc_nb */
    CHECK_EQ(input_unregister_source(NULL), -1);
}

static void test_readline_without_sources_never_blocks(void) {
    registry_clear();
    char buf[32];
    memset(buf, 'x', sizeof buf);
    CHECK_EQ(input_readline(buf, sizeof buf), INPUT_NONE);
    CHECK_STR(buf, "");
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);
}

static void test_poll_is_non_blocking(void) {
    registry_clear();
    input_register_source(&src_a);
    char buf[32];
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);   /* nothing queued */

    feed_push(&feed_a, "part");                          /* partial line */
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);

    feed_push(&feed_a, "ial\n");
    CHECK_EQ(input_poll(buf, sizeof buf), 4 + 3);
    CHECK_STR(buf, "partial");
    registry_clear();
}

/* ====================================================================== */
/* End-to-end through a source                                            */
/* ====================================================================== */

static void test_source_roundtrip_and_provenance(void) {
    registry_clear();
    input_register_source(&src_a);
    feed_push(&feed_a, "how do i boot this thing\n");

    char buf[128];
    CHECK_EQ(input_readline(buf, sizeof buf), 24);
    CHECK_STR(buf, "how do i boot this thing");
    CHECK(input_last_source() == &src_a);
    CHECK_STR(input_last_source()->name, "a");
    CHECK_EQ(input_line_was_truncated(), 0);
    registry_clear();
}

static void test_source_echo_flag_is_honoured(void) {
    registry_clear();
    input_register_source(&src_b);            /* src_b has no INPUT_ECHO */
    feed_push(&feed_b, "quiet\n");
    char buf[64];
    kcap_reset();
    CHECK_EQ(input_readline(buf, sizeof buf), 5);
    CHECK_STR(kcap_text(), "");

    registry_clear();
    input_register_source(&src_a);            /* src_a echoes */
    feed_push(&feed_a, "loud\n");
    kcap_reset();
    CHECK_EQ(input_readline(buf, sizeof buf), 4);
    CHECK_STR(kcap_text(), "loud\n");
    registry_clear();
}

static void test_very_long_line_through_a_source(void) {
    /* 20000 bytes with no line ending in sight, then a newline: the classic
     * "someone pasted a file into the terminal" case. It must come back capped
     * at INPUT_LINE_MAX-1, flagged, and with the kernel still alive. */
    registry_clear();
    input_register_source(&src_a);
    feed_push_n(&feed_a, 'q', FEED_CAP - 1);
    feed_push(&feed_a, "\n");

    static char buf[INPUT_LINE_MAX];
    int n = input_readline(buf, sizeof buf);
    CHECK_EQ(n, INPUT_LINE_MAX - 1);
    CHECK_EQ((int)strlen(buf), INPUT_LINE_MAX - 1);
    CHECK_EQ(input_line_was_truncated(), 1);
    for (int i = 0; i < n; i++) CHECK_EQ(buf[i] == 'q', 1);

    /* And the next line is clean — truncation is per-line, not sticky. */
    feed_clear(&feed_a);
    feed_push(&feed_a, "short\n");
    CHECK_EQ(input_readline(buf, sizeof buf), 5);
    CHECK_STR(buf, "short");
    CHECK_EQ(input_line_was_truncated(), 0);
    registry_clear();
}

static void test_long_line_into_small_caller_buffer(void) {
    registry_clear();
    input_register_source(&src_b);
    feed_push(&feed_b, "the quick brown fox jumps over the lazy dog\n");
    char small[10];
    CHECK_EQ(input_readline(small, sizeof small), 9);
    CHECK_STR(small, "the quick");
    CHECK_EQ(input_line_was_truncated(), 1);
    registry_clear();
}

static void test_line_endings_through_a_source(void) {
    registry_clear();
    input_register_source(&src_b);
    feed_push(&feed_b, "unix\ndos\r\nmac\rmixed\r\n\n");
    char buf[64];
    CHECK_EQ(input_readline(buf, sizeof buf), 4); CHECK_STR(buf, "unix");
    CHECK_EQ(input_readline(buf, sizeof buf), 3); CHECK_STR(buf, "dos");
    CHECK_EQ(input_readline(buf, sizeof buf), 3); CHECK_STR(buf, "mac");
    CHECK_EQ(input_readline(buf, sizeof buf), 5); CHECK_STR(buf, "mixed");
    CHECK_EQ(input_readline(buf, sizeof buf), 0); CHECK_STR(buf, "");
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);
    registry_clear();
}

static void test_drain_budget_does_not_lose_bytes(void) {
    /* input_poll drains at most INPUT_DRAIN_BUDGET bytes per source per call, so
     * a line longer than the budget takes several polls. None of it may be lost. */
    registry_clear();
    input_register_source(&src_b);
    int len = INPUT_DRAIN_BUDGET * 3 + 7;
    feed_push_n(&feed_b, 'z', len);
    feed_push(&feed_b, "\n");

    char buf[64];
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);   /* budget exhausted */

    static char big[INPUT_LINE_MAX];
    CHECK_EQ(input_readline(big, sizeof big), len);
    registry_clear();
}

/* ====================================================================== */
/* Multi-source arbitration                                               */
/* ====================================================================== */

static void test_two_sources_do_not_interleave(void) {
    /* Someone typing on the keyboard while a script feeds the serial line must
     * not produce a single blended line. */
    registry_clear();
    input_register_source(&src_a);
    input_register_source(&src_b);

    feed_push(&feed_a, "hel");
    feed_push(&feed_b, "wor");
    char buf[64];
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);

    feed_push(&feed_a, "lo\n");
    CHECK_EQ(input_readline(buf, sizeof buf), 5);
    CHECK_STR(buf, "hello");
    CHECK(input_last_source() == &src_a);

    feed_push(&feed_b, "ld\n");
    CHECK_EQ(input_readline(buf, sizeof buf), 5);
    CHECK_STR(buf, "world");
    CHECK(input_last_source() == &src_b);
    registry_clear();
}

static void test_round_robin_prevents_starvation(void) {
    /* src_a floods; src_b has one line. Round-robin must serve b after a's
     * first line rather than draining a to the end. */
    registry_clear();
    input_register_source(&src_a);
    input_register_source(&src_b);
    feed_push(&feed_a, "a1\na2\na3\n");
    feed_push(&feed_b, "b1\n");

    char buf[64];
    const char *want[] = { "a1", "b1", "a2", "a3" };
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(input_readline(buf, sizeof buf), 2);
        CHECK_STR(buf, want[i]);
    }
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);
    registry_clear();
}

static void test_unregister_mid_stream_keeps_the_rest_working(void) {
    registry_clear();
    input_register_source(&src_a);
    input_register_source(&src_b);
    feed_push(&feed_a, "gone\n");
    feed_push(&feed_b, "still here\n");

    input_unregister_source(&src_a);
    char buf[64];
    CHECK_EQ(input_readline(buf, sizeof buf), 10);
    CHECK_STR(buf, "still here");
    CHECK(input_last_source() == &src_b);
    registry_clear();
}

/* ====================================================================== */
/* Scripted source                                                        */
/* ====================================================================== */

static void test_script_drives_turns(void) {
    registry_clear();
    input_register_source(input_script_source());

    CHECK_EQ(input_script_pending(), 0);
    CHECK(input_script_pushline("what time is it") > 0);
    CHECK_EQ(input_script_pending(), 16);
    CHECK(input_script_push("and where am i\nand who are you\n") > 0);

    char buf[128];
    kcap_reset();
    CHECK_EQ(input_readline(buf, sizeof buf), 15);
    CHECK_STR(buf, "what time is it");
    CHECK_STR(input_last_source()->name, "script");
    CHECK_STR(kcap_text(), "what time is it\n");   /* echoes like real typing */

    CHECK_EQ(input_readline(buf, sizeof buf), 14);
    CHECK_STR(buf, "and where am i");
    CHECK_EQ(input_readline(buf, sizeof buf), 15);
    CHECK_STR(buf, "and who are you");
    CHECK_EQ(input_script_pending(), 0);
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);
    registry_clear();
}

static void test_script_reset_and_bad_input(void) {
    registry_clear();
    input_register_source(input_script_source());
    input_script_pushline("discard me");
    CHECK(input_script_pending() > 0);
    input_script_reset();
    CHECK_EQ(input_script_pending(), 0);

    char buf[64];
    CHECK_EQ(input_poll(buf, sizeof buf), INPUT_NONE);
    CHECK_EQ(input_script_push(NULL), -1);
    CHECK_EQ(input_script_pushline(NULL), -1);
    registry_clear();
}

static void test_script_queue_recycles(void) {
    /* Pushing far more total text than the queue holds must work as long as it
     * is consumed as it goes — the queue compacts rather than filling up. */
    registry_clear();
    input_register_source(input_script_source());
    char buf[128];
    for (int i = 0; i < 500; i++) {
        CHECK(input_script_pushline("tell me something interesting please") > 0);
        CHECK_EQ(input_readline(buf, sizeof buf), 36);
    }
    CHECK_STR(buf, "tell me something interesting please");
    registry_clear();
}

static void test_script_rejects_oversized_push(void) {
    registry_clear();
    static char huge[65536];
    memset(huge, 'x', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';
    CHECK_EQ(input_script_push(huge), -1);      /* refused, not overflowed */
    CHECK_EQ(input_script_pending(), 0);
    registry_clear();
}

int main(void) {
    /* Unbuffered: the input layer is a spin loop by design, so if a change ever
     * makes input_readline() hang, the log must show which test got stuck. */
    setvbuf(stdout, NULL, _IONBF, 0);
    console_init();

    RUN(test_basic_line_assembly);
    RUN(test_buffer_is_always_nul_terminated);
    RUN(test_echo_matches_buffer);
    RUN(test_no_echo_when_disabled);

    RUN(test_backspace_edits);
    RUN(test_backspace_del_variant);
    RUN(test_backspace_past_start_is_a_noop);
    RUN(test_backspace_echo_sequence);
    RUN(test_backspace_frees_room_after_truncation);

    RUN(test_overflow_never_writes_out_of_bounds);
    RUN(test_overflow_does_not_echo_dropped_chars);
    RUN(test_truncated_flag_clears_on_reset);
    RUN(test_take_into_smaller_buffer_truncates);
    RUN(test_take_is_safe_with_null);
    RUN(test_degenerate_capacities);

    RUN(test_lf_line_ending);
    RUN(test_cr_line_ending);
    RUN(test_crlf_is_one_line_ending);
    RUN(test_crlf_split_across_takes);
    RUN(test_lone_lf_after_text_is_not_swallowed);
    RUN(test_lfcr_yields_two_lines);
    RUN(test_empty_lines);
    RUN(test_nul_is_dropped);
    RUN(test_high_bytes_survive);

    RUN(test_register_and_count);
    RUN(test_register_rejects_bad_source);
    RUN(test_readline_without_sources_never_blocks);
    RUN(test_poll_is_non_blocking);

    RUN(test_source_roundtrip_and_provenance);
    RUN(test_source_echo_flag_is_honoured);
    RUN(test_very_long_line_through_a_source);
    RUN(test_long_line_into_small_caller_buffer);
    RUN(test_line_endings_through_a_source);
    RUN(test_drain_budget_does_not_lose_bytes);

    RUN(test_two_sources_do_not_interleave);
    RUN(test_round_robin_prevents_starvation);
    RUN(test_unregister_mid_stream_keeps_the_rest_working);

    RUN(test_script_drives_turns);
    RUN(test_script_reset_and_bad_input);
    RUN(test_script_queue_recycles);
    RUN(test_script_rejects_oversized_push);

    return th_report("input");
}
