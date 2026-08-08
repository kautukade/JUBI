/* test_trace.c — adversarial host tests for lib/trace.c.
 *
 * WHAT THIS SUITE IS FOR
 *   Trace lines are the machine's only proof that the model acted rather than
 *   narrated: trace.h says the operator reads a transcript as two voices, and
 *   that "the model never produces these bytes and cannot forge them". These
 *   tests attack that claim, the line format, the error table, and the counter
 *   semantics that core/tool.c's dispatch guarantee is built on.
 *
 *   Exact bytes matter here, so every assertion compares against the captured
 *   console (kcap_text()) rather than against a substring where it can.
 *
 * THE INVARIANTS GUARDED HERE
 *   Each of these was once violated; the assertions below are what stops the
 *   violation coming back.
 *
 *     1. One call renders exactly one line: one leading '[', one trailing "]\n",
 *        and no raw control character, DEL, '[' or ']' anywhere in between. A
 *        model-chosen path full of newlines and brackets cannot forge a line.
 *     2. The "-> <result>]" tail and the newline are budgeted FIRST. An
 *        over-long ARGUMENT is elided with "..."; the evidence field survives.
 *     3. trace_err() can never print "-> ok". trace_errname(0) is NULL and
 *        trace_err(0) renders "-> error".
 *     4. trace_emissions() counts bytes that reached the console and nothing
 *        else: never advanced while suppressed, never rolled back by
 *        trace_reset(). trace_count() may lie in both directions and is
 *        asserted to be *unsuitable* as evidence, which is why it must not be
 *        what core/tool.c builds its guarantee on.
 */

#include "harness.h"
#include "trace.h"
#include "vfs.h"
#include "model.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void fresh(void) {
    trace_set_enabled(1);
    trace_reset();
    kcap_reset();
}

/* ---- shared shape checks ------------------------------------------------ */

static size_t count_char(const char *s, char c) {
    size_t n = 0;
    for (; *s; s++) if (*s == c) n++;
    return n;
}

/* The structural invariant trace.h promises for EVERY rendered line: it is one
 * line, it opens with '[' and closes with "]\n", and nothing between those can
 * be mistaken for line structure. Returns the line length so callers can chain
 * further assertions. */
static size_t check_well_formed(const char *out) {
    size_t n = strlen(out);
    CHECK(n >= 2);
    CHECK(n <= TRACE_LINE_MAX - 1);
    if (n < 2) return n;
    CHECK_EQ(out[0], '[');
    CHECK_EQ(out[n - 2], ']');
    CHECK_EQ(out[n - 1], '\n');
    /* Exactly one of each bracket, at the two ends: nothing inside can pass for
     * the start or end of a kernel line. */
    CHECK_EQ(count_char(out, '['), 1);
    CHECK_EQ(count_char(out, ']'), 1);
    CHECK_EQ(count_char(out, '\n'), 1);
    /* No raw control character or DEL survived into the rendered line. */
    int raw_control = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c < 0x20 || c == 0x7F) { raw_control = 1; break; }
    }
    CHECK_EQ(raw_control, 0);
    return n;
}

/* ====================================================================== */
/* 1. line format                                                         */
/* ====================================================================== */

static void test_format_ok_ret_err(void) {
    fresh();
    trace_ok("vfs_write", "fd=%d path=%s", 3, "/etc/motd");
    CHECK_STR(kcap_text(), "[vfs_write fd=3 path=/etc/motd -> ok]\n");

    fresh();
    trace_ret(34, "vfs_write", "fd=%d", 3);
    CHECK_STR(kcap_text(), "[vfs_write fd=3 -> 34]\n");

    fresh();
    trace_err(VFS_ENOENT, "vfs_open", "%s", "/no/such");
    CHECK_STR(kcap_text(), "[vfs_open /no/such -> ENOENT]\n");
}

static void test_format_without_arguments(void) {
    /* Three ways to say "no arguments": NULL, "", and a format that renders
     * empty. All must collapse to the two-field form with no double space. */
    fresh();
    trace_ok("heap_stats", NULL);
    CHECK_STR(kcap_text(), "[heap_stats -> ok]\n");

    fresh();
    trace_ok("heap_stats", "");
    CHECK_STR(kcap_text(), "[heap_stats -> ok]\n");

    fresh();
    trace_ok("heap_stats", "%s", "");
    CHECK_STR(kcap_text(), "[heap_stats -> ok]\n");

    fresh();
    trace_ret(0, "heap_stats", "%s", "");
    CHECK_STR(kcap_text(), "[heap_stats -> 0]\n");

    fresh();
    trace_err(VFS_EINVAL, "heap_stats", "%s", "");
    CHECK_STR(kcap_text(), "[heap_stats -> EINVAL]\n");
}

static void test_format_null_op(void) {
    fresh();
    trace_ok(NULL, "x=%d", 1);
    CHECK_STR(kcap_text(), "[? x=1 -> ok]\n");

    fresh();
    trace_err(VFS_ENOSPC, NULL, NULL);
    CHECK_STR(kcap_text(), "[? -> ENOSPC]\n");
}

static void test_format_percent_and_specifiers(void) {
    fresh();
    trace_ok("op", "%d%% used", 93);
    CHECK_STR(kcap_text(), "[op 93% used -> ok]\n");

    fresh();
    trace_ok("op", "%c%s%u", 'x', "y", 7u);
    CHECK_STR(kcap_text(), "[op xy7 -> ok]\n");

    /* An args string that is only whitespace still counts as "has args". */
    fresh();
    trace_ok("op", " ");
    CHECK_STR(kcap_text(), "[op   -> ok]\n");
}

static void test_ret_extremes(void) {
    fresh();
    trace_ret(0, "op", NULL);
    CHECK_STR(kcap_text(), "[op -> 0]\n");

    fresh();
    trace_ret(-1, "op", NULL);
    CHECK_STR(kcap_text(), "[op -> -1]\n");

    fresh();
    trace_ret(LONG_MAX, "op", NULL);
    CHECK_STR(kcap_text(), "[op -> 9223372036854775807]\n");

    /* LONG_MIN is the classic negation trap: 20 characters plus NUL still fits
     * the 32-byte result scratch, so it renders whole. */
    fresh();
    trace_ret(LONG_MIN, "op", NULL);
    CHECK_STR(kcap_text(), "[op -> -9223372036854775808]\n");
}

/* ====================================================================== */
/* 2. the error table                                                     */
/* ====================================================================== */

static void test_errname_matches_the_real_headers(void) {
    /* trace.c hardcodes -70..-73 with comments claiming they are the MODEL_*
     * codes. Verify against model.h itself rather than trusting the comment. */
    CHECK_EQ(MODEL_ETRANSPORT, -70);
    CHECK_EQ(MODEL_ETIMEOUT,   -71);
    CHECK_EQ(MODEL_EPROTO,     -72);
    CHECK_EQ(MODEL_ESCRIPT,    -73);
    CHECK_STR(trace_errname(MODEL_ETRANSPORT), "ETRANSPORT");
    CHECK_STR(trace_errname(MODEL_ETIMEOUT),   "ETIMEOUT");
    CHECK_STR(trace_errname(MODEL_EPROTO),     "EPROTO");
    CHECK_STR(trace_errname(MODEL_ESCRIPT),    "ESCRIPT");

    /* The VFS half of the shared error space. */
    CHECK_STR(trace_errname(VFS_ENOENT),  "ENOENT");
    CHECK_STR(trace_errname(VFS_EEXIST),  "EEXIST");
    CHECK_STR(trace_errname(VFS_ENOTDIR), "ENOTDIR");
    CHECK_STR(trace_errname(VFS_EINVAL),  "EINVAL");
    CHECK_STR(trace_errname(VFS_ENOSPC),  "ENOSPC");

    /* MODEL_* and VFS_* really do share the low codes, so one table is right. */
    CHECK_EQ(MODEL_EINVAL, VFS_EINVAL);
    CHECK_EQ(MODEL_ENOSPC, VFS_ENOSPC);

    /* Unknown codes must return NULL so callers can print the number. */
    CHECK(trace_errname(0) == NULL);         /* 0 is deliberately absent */
    CHECK(trace_errname(-1) == NULL);
    CHECK(trace_errname(1) == NULL);
    CHECK(trace_errname(-21) == NULL);       /* JSON_ETYPE  */
    CHECK(trace_errname(-40) == NULL);       /* JSON_EDEPTH */
    CHECK(trace_errname(INT_MIN) == NULL);
    CHECK(trace_errname(INT_MAX) == NULL);
}

static void test_err_renders_unknown_codes_numerically(void) {
    fresh();
    trace_err(-21, "json_parse", NULL);
    CHECK_STR(kcap_text(), "[json_parse -> -21]\n");

    fresh();
    trace_err(INT_MIN, "op", NULL);
    CHECK_STR(kcap_text(), "[op -> -2147483648]\n");

    fresh();
    trace_err(7, "op", NULL);
    CHECK_STR(kcap_text(), "[op -> 7]\n");
}

/* REGRESSION GUARD: an error reporter must never be able to print a success.
 *
 * trace_err() is fed result codes by callers that do not always check them first
 * — core/tool.c's dispatch fallback forwards `rc` verbatim, and a handler that
 * fails the documented way (out->is_error with rc == TOOL_OK) sends 0 through
 * here. If 0 rendered as "ok", the one voice the operator is supposed to trust
 * would corroborate an action that did not happen. */
static void test_err_zero_renders_as_failure(void) {
    fresh();
    trace_err(0, "unlink", "%s", "/etc/passwd");
    CHECK_STR(kcap_text(), "[unlink /etc/passwd -> error]\n");

    /* 0 is not in the table at all, so no caller can resolve it to a name. */
    CHECK(trace_errname(0) == NULL);

    /* Stronger: sweep the whole plausible code space and prove that trace_err()
     * has no input at all that makes it print a success. "-> ok" is reachable
     * only through trace_ok(), which is the function that means success. */
    for (int code = -300; code <= 300; code++) {
        fresh();
        trace_err(code, "op", NULL);
        const char *out = kcap_text();
        check_well_formed(out);
        CHECK(strstr(out, "-> ok]") == NULL);
        if (code == 0) CHECK_STR(out, "[op -> error]\n");
    }
    kcap_reset();
}

/* ====================================================================== */
/* 3. TRACE_LINE_MAX                                                      */
/* ====================================================================== */

/* REGRESSION GUARD: the result is budgeted before the argument, so an over-long
 * argument can never cost the operator the evidence field or the newline.
 *
 * tool.h warns that a confused model can send "a path of 10,000 characters".
 * When the arguments were rendered first they pushed "-> <result>]\n" clean off
 * the end, producing an unterminated fragment that merged with whatever the
 * kernel printed next — the single field that makes the line evidence was the
 * first one sacrificed. */
static void test_long_argument_is_elided_and_the_result_survives(void) {
    char *huge = (char *)malloc(10001);
    memset(huge, 'p', 10000);
    huge[10000] = '\0';

    fresh();
    trace_ret(34, "vfs_write", "path=%s", huge);
    const char *out = kcap_text();
    size_t n = check_well_formed(out);

    CHECK_EQ(n, TRACE_LINE_MAX - 1);         /* fills the line, never exceeds it */
    CHECK(strstr(out, "... -> 34]\n") != NULL);  /* argument elided, result kept */
    CHECK(strncmp(out, "[vfs_write path=pp", 18) == 0);

    /* The next line starts on its own line — no gluing. */
    trace_ok("vfs_close", "fd=%d", 3);
    CHECK_CONTAINS(kcap_text(), "... -> 34]\n[vfs_close fd=3 -> ok]\n");

    free(huge);
}

/* Walk the argument length across the elision cliff. The line must be
 * well-formed at EVERY length, must never exceed TRACE_LINE_MAX-1 bytes, and
 * must switch from verbatim to elided at exactly the documented point. No write
 * may land outside trace.c's stack buffers (ASan/UBSan are watching). */
static void test_line_length_boundary_sweep(void) {
    /* emit() budgets: "[" op " -> " result "]\n" NUL, then gives the argument
     * whatever is left minus the space that separates it from the op. */
    const size_t fixed = 1 + strlen("op") + 4 + strlen("ok") + 2 + 1;
    const size_t room  = TRACE_LINE_MAX - fixed - 1;   /* 499 */

    for (size_t len = 470; len <= 520; len++) {
        char *arg = (char *)malloc(len + 1);
        memset(arg, 'a', len);
        arg[len] = '\0';

        fresh();
        trace_ok("op", "%s", arg);
        const char *out = kcap_text();
        size_t n = check_well_formed(out);

        char want[TRACE_LINE_MAX];
        if (len <= room) {
            /* Fits: the argument is rendered whole, no ellipsis. */
            snprintf(want, sizeof want, "[op %s -> ok]\n", arg);
            CHECK(strstr(out, "...") == NULL);
        } else {
            /* Does not fit: keep room-3 bytes of it and mark the elision. */
            arg[room - 3] = '\0';
            snprintf(want, sizeof want, "[op %s... -> ok]\n", arg);
            CHECK_EQ(n, TRACE_LINE_MAX - 1);
        }
        CHECK_STR(out, want);

        free(arg);
    }

    /* The switch point itself, pinned exactly. */
    {
        char arg[TRACE_LINE_MAX];
        memset(arg, 'a', room); arg[room] = '\0';
        fresh(); trace_ok("op", "%s", arg);
        CHECK_EQ(strlen(kcap_text()), TRACE_LINE_MAX - 1);
        CHECK(strstr(kcap_text(), "...") == NULL);

        arg[room + 1] = '\0'; memset(arg, 'a', room + 1);
        fresh(); trace_ok("op", "%s", arg);
        CHECK_EQ(strlen(kcap_text()), TRACE_LINE_MAX - 1);
        CHECK(strstr(kcap_text(), "a... -> ok]\n") != NULL);
    }
    kcap_reset();
}

/* The op name shares the line budget with the argument. Unlike the argument it
 * is never model-controlled — it is a kernel string literal, or a tool name that
 * tool.c has already bounded to TOOL_NAME_MAX-1 (47) bytes — so the reachable
 * range is tiny. Sweep far past it anyway, with and without arguments: the line
 * must stay well-formed and the result must always survive.
 *
 * (Only the ARGUMENT is elided when the budget is tight; the op is not. See the
 * accompanying report for the unreachable-but-real overflow above ~495 bytes of
 * op, which is why this sweep stops where it does rather than at TRACE_LINE_MAX.) */
static void test_op_length_sweep(void) {
    char op[512];
    for (size_t oplen = 1; oplen <= 400; oplen += 3) {
        memset(op, 'o', oplen);
        op[oplen] = '\0';

        fresh();
        trace_ok(op, NULL);
        CHECK(strstr(kcap_text(), " -> ok]\n") != NULL);
        check_well_formed(kcap_text());

        /* Same op, plus an argument long enough to force the elision path. */
        fresh();
        trace_err(VFS_ENOENT, op, "%s", "/a/model/supplied/path/that/is/long");
        CHECK(strstr(kcap_text(), "-> ENOENT]\n") != NULL);
        check_well_formed(kcap_text());
    }
    kcap_reset();
}

/* REGRESSION GUARD: a crafted path must not forge trace lines.
 *
 * trace.h: "The model never produces these bytes and cannot forge them." The
 * arguments ARE model-controlled in every realistic tool (a path, a device name,
 * a key), so this is the claim most worth attacking. A newline used to end the
 * line early and start one byte-for-byte indistinguishable from a genuine
 * kernel trace; three lines came out of one call and trace_count() reported 1. */
static void test_crafted_argument_cannot_forge_a_trace_line(void) {
    fresh();
    uint64_t e0 = trace_emissions();
    const char *evil_path = "/tmp/a\n[vfs_write fd=3 bytes=9999 -> ok]\n"
                            "[net_connect 10.0.0.1:22 -> ok]";
    trace_err(VFS_ENOENT, "vfs_open", "%s", evil_path);

    const char *out = kcap_text();

    /* Exactly one line, byte for byte, with every structural character of the
     * attack neutralised: \n -> \x0a, [ -> \x5b, ] -> \x5d. */
    CHECK_STR(out,
        "[vfs_open /tmp/a\\x0a\\x5bvfs_write fd=3 bytes=9999 -> ok\\x5d"
        "\\x0a\\x5bnet_connect 10.0.0.1:22 -> ok\\x5d -> ENOENT]\n");
    check_well_formed(out);

    /* Not one forged line survives anywhere in the output. */
    CHECK(strstr(out, "[vfs_write fd=3 bytes=9999 -> ok]") == NULL);
    CHECK(strstr(out, "[net_connect 10.0.0.1:22 -> ok]") == NULL);

    /* One call, one requested line, one line on the console. */
    CHECK_EQ(trace_count(), 1);
    CHECK_EQ(trace_emissions() - e0, 1);

    /* A carriage return would overwrite the line on a serial console. */
    fresh();
    trace_ok("vfs_open", "%s", "/tmp/x\r[fmt_disk /dev/sda -> ok]");
    CHECK_STR(kcap_text(),
              "[vfs_open /tmp/x\\x0d\\x5bfmt_disk /dev/sda -> ok\\x5d -> ok]\n");
    check_well_formed(kcap_text());

    /* The same attack from a *long* path, so it goes down the elision branch:
     * truncating the escaped text can never re-expose a raw bracket or newline,
     * because there is none left in it to expose. */
    char *evil_long = (char *)malloc(4001);
    for (int i = 0; i < 4000; i++)
        evil_long[i] = (i % 3 == 0) ? '\n' : (i % 3 == 1) ? '[' : ']';
    evil_long[4000] = '\0';
    fresh();
    trace_ok("vfs_open", "%s", evil_long);
    check_well_formed(kcap_text());
    CHECK_EQ(strlen(kcap_text()), TRACE_LINE_MAX - 1);
    CHECK(strstr(kcap_text(), "... -> ok]\n") != NULL);
    free(evil_long);

    kcap_reset();
}

/* Every byte value, in an argument, one at a time: the escape decision must be
 * exactly "control characters, DEL, and both brackets", and nothing else may be
 * altered. This is the sanitizer's contract stated exhaustively, so a future
 * "optimisation" that skips a range is caught immediately. */
static void test_sanitize_covers_every_byte(void) {
    static const char hex[] = "0123456789abcdef";
    for (int c = 1; c <= 255; c++) {
        char arg[4] = { 'a', (char)c, 'b', '\0' };
        char want[TRACE_LINE_MAX];

        if (c < 0x20 || c == 0x7F || c == '[' || c == ']')
            snprintf(want, sizeof want, "[op a\\x%c%cb -> ok]\n",
                     hex[(c >> 4) & 0xF], hex[c & 0xF]);
        else
            snprintf(want, sizeof want, "[op a%cb -> ok]\n", (char)c);

        fresh();
        trace_ok("op", "%s", arg);
        CHECK_STR(kcap_text(), want);
        check_well_formed(kcap_text());
    }

    /* An argument made only of the characters that need escaping still renders
     * as one line, and the escapes are lower-case hex as documented. */
    fresh();
    trace_ok("op", "%s", "\x01\x1f\x7f[]");
    CHECK_STR(kcap_text(), "[op \\x01\\x1f\\x7f\\x5b\\x5d -> ok]\n");

    /* A NUL simply ends the argument — it cannot smuggle bytes past the copy. */
    fresh();
    trace_ok("op", "%s", "safe");
    CHECK_STR(kcap_text(), "[op safe -> ok]\n");
    kcap_reset();
}

/* ====================================================================== */
/* 4. counter, reset, enable                                              */
/* ====================================================================== */

static void test_counter_semantics(void) {
    fresh();
    CHECK_EQ(trace_count(), 0);
    trace_ok("a", NULL);
    CHECK_EQ(trace_count(), 1);
    trace_ret(1, "b", NULL);
    CHECK_EQ(trace_count(), 2);
    trace_err(VFS_EINVAL, "c", NULL);
    CHECK_EQ(trace_count(), 3);
    trace_reset();
    CHECK_EQ(trace_count(), 0);

    /* Reset does not touch the enabled flag or the console. */
    CHECK_EQ(trace_enabled(), 1);
    CHECK_CONTAINS(kcap_text(), "[a -> ok]\n[b -> 1]\n[c -> EINVAL]\n");
}

static void test_enabled_semantics(void) {
    fresh();
    CHECK_EQ(trace_enabled(), 1);            /* on by default */

    uint64_t e0 = trace_emissions();
    trace_set_enabled(0);
    CHECK_EQ(trace_enabled(), 0);
    trace_ok("silenced", NULL);
    CHECK_STR(kcap_text(), "");
    /* Documented in trace.h: "0 suppresses output; request count still runs". */
    CHECK_EQ(trace_count(), 1);
    CHECK_EQ(trace_emissions(), e0);         /* but emissions do not */

    /* Any non-zero normalises to 1. */
    trace_set_enabled(2);
    CHECK_EQ(trace_enabled(), 1);
    trace_set_enabled(-1);
    CHECK_EQ(trace_enabled(), 1);

    kcap_reset();
    trace_ok("audible", NULL);
    CHECK_STR(kcap_text(), "[audible -> ok]\n");
    CHECK_EQ(trace_count(), 2);
    CHECK_EQ(trace_emissions(), e0 + 1);     /* exactly the one that was heard */
}

/* REGRESSION GUARD: trace_emissions() is evidence; trace_count() is not.
 *
 * core/tool.c's "every model action produces a kernel line" guarantee is a
 * before/after comparison of a counter. Built on trace_count() it was unsound in
 * both directions:
 *     counter up, nothing printed   -> tracing disabled
 *     counter down, lines printed   -> trace_reset()
 * Both directions are re-asserted here for trace_count() (that weakness is now
 * documented in trace.h, so it must stay true and stay visible), and the
 * corresponding trace_emissions() behaviour is asserted to be immune. */
static void test_emissions_is_evidence_and_the_counter_is_not(void) {
    /* Direction 1: suppressed output. The request counter moves; emissions
     * must not, because nothing reached the console. */
    fresh();
    uint32_t c0 = trace_count();
    uint64_t e0 = trace_emissions();
    trace_set_enabled(0);
    trace_ok("fmt_disk", "%s", "/dev/sda");
    trace_ret(4096, "vfs_write", "fd=%d", 3);
    trace_set_enabled(1);
    CHECK_EQ(trace_count(), c0 + 2);         /* counter moved... */
    CHECK_STR(kcap_text(), "");              /* ...console empty */
    CHECK_EQ(trace_emissions(), e0);         /* ...and emissions did not move */

    /* Direction 2: a rollback. Lines really were printed; the request counter
     * goes backwards anyway. Emissions are monotonic and untouched. */
    fresh();
    e0 = trace_emissions();
    trace_ok("real_action", NULL);
    CHECK_EQ(trace_emissions(), e0 + 1);
    uint32_t before = trace_count();
    trace_reset();
    CHECK_EQ(trace_count(), 0);
    CHECK(before != 0);                      /* a "change" that is a rollback */
    CHECK_CONTAINS(kcap_text(), "[real_action -> ok]\n");
    CHECK_EQ(trace_emissions(), e0 + 1);     /* reset cannot erase the evidence */

    /* Repeated resets cannot drive emissions down or make them wrap. */
    for (int i = 0; i < 100; i++) trace_reset();
    CHECK_EQ(trace_emissions(), e0 + 1);
    CHECK_EQ(trace_count(), 0);
}

/* Stronger: emissions must equal what the console actually received, over a
 * long random mix of the operations a hostile tool has available. Any path that
 * advances the counter without printing — or prints without advancing it —
 * shows up as a mismatch here. */
static void test_emissions_tracks_console_output_exactly(void) {
    fresh();
    uint64_t e0 = trace_emissions();
    uint64_t prev = e0;
    uint32_t seed = 0xc0ffee11u;

    for (int i = 0; i < 2000; i++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned pick = (seed >> 8) % 8;
        int      on   = trace_enabled();
        uint64_t bef  = trace_emissions();

        switch (pick) {
            case 0: trace_set_enabled(0);                     break;
            case 1: trace_set_enabled(1);                     break;
            case 2: trace_reset();                            break;
            case 3: trace_ok("op", "%s", "/p");               break;
            case 4: trace_ret((long)i, "op", "i=%d", i);      break;
            case 5: trace_err(VFS_ENOENT, "op", NULL);        break;
            case 6: trace_err(0, "op", "%s", "x\ny");         break;
            default: trace_ok("op", NULL);                    break;
        }

        /* An emitting call advances emissions by exactly one iff output was
         * enabled; a control call never advances it at all. */
        uint64_t want = bef + ((pick >= 3 && on) ? 1u : 0u);
        CHECK_EQ(trace_emissions(), want);
        CHECK(trace_emissions() >= prev);     /* monotonic, always */
        prev = trace_emissions();
    }

    trace_set_enabled(1);
    /* Every emitted line ends in '\n' and nothing else prints here, so the
     * newline count on the console is the ground truth for the emission count. */
    CHECK_EQ(count_char(kcap_text(), '\n'), trace_emissions() - e0);
    CHECK(trace_emissions() - e0 > 100);      /* the run was not vacuous */
    kcap_reset();
}

static void test_no_output_between_resets(void) {
    /* trace_reset() is documented as zeroing "the counter and sequence number".
     * There is only one variable, so a future structured sink keying off a
     * monotonic sequence number (trace.h's stated extension point) would see it
     * restart — worth pinning now. */
    fresh();
    trace_ok("a", NULL);
    trace_ok("b", NULL);
    CHECK_EQ(trace_count(), 2);
    trace_reset();
    trace_ok("c", NULL);
    CHECK_EQ(trace_count(), 1);
    CHECK_CONTAINS(kcap_text(), "[a -> ok]\n[b -> ok]\n[c -> ok]\n");
}

static void test_high_volume_is_stable(void) {
    /* 5,000 lines through the same stack buffers: proves no leak of state
     * between calls and gives ASan a wide window on the formatting path. */
    fresh();
    uint64_t e0 = trace_emissions();
    for (int i = 0; i < 5000; i++) trace_ret(i, "vfs_write", "fd=%d", i % 16);
    CHECK_EQ(trace_count(), 5000);
    CHECK_EQ(trace_emissions() - e0, 5000);
    CHECK_EQ(count_char(kcap_text(), '\n'), 5000);
    CHECK_CONTAINS(kcap_text(), "[vfs_write fd=0 -> 0]\n");
    CHECK_CONTAINS(kcap_text(), "[vfs_write fd=7 -> 4999]\n");   /* 4999 % 16 */
    kcap_reset();
}

/* A deterministic fuzz over op/arg lengths, argument BYTES and result codes.
 * Nothing here knows the "right" output; it asserts the invariants that must
 * hold for every line no matter what a tool passes in, with ASan/UBSan watching
 * the two stack buffers in emit(). The arguments are filled with arbitrary bytes
 * — including newlines and brackets — so every iteration is also a forgery
 * attempt against the sanitizer and the elision path at once. */
static void test_format_invariants_fuzz(void) {
    uint32_t seed = 0x5eed1234u;
    char op[80], arg[700];

    for (int iter = 0; iter < 400; iter++) {
        seed = seed * 1664525u + 1013904223u;
        size_t oplen  = 1 + (seed >> 8) % (sizeof op - 1);
        seed = seed * 1664525u + 1013904223u;
        size_t arglen = (seed >> 8) % (sizeof arg - 1);
        seed = seed * 1664525u + 1013904223u;
        int    code   = (int)((seed >> 8) % 200) - 100;

        memset(op, 'o', oplen);   op[oplen] = '\0';
        for (size_t i = 0; i < arglen; i++) {
            seed = seed * 1664525u + 1013904223u;
            unsigned b = (seed >> 8) % 255u + 1u;      /* 1..255, never NUL */
            arg[i] = (char)b;
        }
        arg[arglen] = '\0';

        fresh();
        uint64_t e0 = trace_emissions();
        switch (iter % 3) {
            case 0: trace_ok(op, "%s", arg);              break;
            case 1: trace_ret((long)code, op, "%s", arg); break;
            default: trace_err(code, op, "%s", arg);      break;
        }

        /* One request, one line printed, and that line is well-formed: one
         * '[' at the front, one "]\n" at the back, nothing in between that can
         * be mistaken for either. */
        CHECK_EQ(trace_count(), 1);
        CHECK_EQ(trace_emissions() - e0, 1);
        check_well_formed(kcap_text());
    }
    kcap_reset();
}

static void test_non_ascii_arguments_pass_through(void) {
    /* UTF-8 in a filename must survive verbatim — trace lines are read by a
     * human, and mangling bytes here would misreport what was touched. Bytes
     * >= 0x80 cannot forge line structure, so escaping them would be pure loss. */
    fresh();
    trace_ok("vfs_open", "%s", "/home/café/naïve.txt");
    CHECK_STR(kcap_text(), "[vfs_open /home/café/naïve.txt -> ok]\n");

    /* ...but the 0x01 in the same string is a control character and IS escaped. */
    fresh();
    trace_ok("vfs_open", "%s", "\xff\xfe\x01");
    CHECK_STR(kcap_text(), "[vfs_open \xff\xfe\\x01 -> ok]\n");

    /* DEL is not below 0x20 but is just as invisible on a console. */
    fresh();
    trace_ok("vfs_open", "%s", "a\x7f" "b");
    CHECK_STR(kcap_text(), "[vfs_open a\\x7fb -> ok]\n");
}

/* ====================================================================== */

int main(void) {
    console_init();

    RUN(test_format_ok_ret_err);
    RUN(test_format_without_arguments);
    RUN(test_format_null_op);
    RUN(test_format_percent_and_specifiers);
    RUN(test_ret_extremes);

    RUN(test_errname_matches_the_real_headers);
    RUN(test_err_renders_unknown_codes_numerically);
    RUN(test_err_zero_renders_as_failure);

    RUN(test_long_argument_is_elided_and_the_result_survives);
    RUN(test_line_length_boundary_sweep);
    RUN(test_op_length_sweep);
    RUN(test_crafted_argument_cannot_forge_a_trace_line);
    RUN(test_sanitize_covers_every_byte);

    RUN(test_counter_semantics);
    RUN(test_enabled_semantics);
    RUN(test_emissions_is_evidence_and_the_counter_is_not);
    RUN(test_emissions_tracks_console_output_exactly);
    RUN(test_no_output_between_resets);
    RUN(test_high_volume_is_stable);
    RUN(test_format_invariants_fuzz);
    RUN(test_non_ascii_arguments_pass_through);

    return th_report("trace");
}
