/* time_tools.c — the wall-clock tool family.
 *
 * PURPOSE
 *   A machine that cannot tell you the date is a machine you cannot trust with
 *   anything time-shaped: certificate expiry, "is this log from today", "how
 *   long ago did that happen". Until drivers/rtc/rtc.c existed, this kernel's
 *   only notion of time was milliseconds since its own boot, so every answer
 *   the model could give about the date was necessarily invented. These two
 *   tools replace that invention with a reading taken from the battery-backed
 *   CMOS clock microseconds before the answer was printed.
 *
 * THE TOOLS
 *   sys_time      what the wall clock says right now: ISO 8601 UTC, the Unix
 *                 timestamp, the weekday, and when this machine booted in
 *                 wall-clock terms. No arguments.
 *   time_convert  turn a Unix timestamp into a UTC calendar date and say how
 *                 far it is from now. This is the direction that matters for
 *                 the question this whole subsystem exists to answer — "has
 *                 that certificate expired?" — because the comparison is
 *                 against a number the kernel measured, not one the model
 *                 remembered.
 *
 *   Both are read-only and neither allocates. There is deliberately no
 *   set_time tool: see the DECISION note at the top of drivers/rtc/rtc.c for
 *   why a clock the model can move is not a clock you can validate a
 *   certificate against.
 *
 * HONESTY ABOUT THE CLOCK
 *   Every result says where the number came from and that the chip is assumed
 *   to be UTC, because that assumption is unverifiable from inside the machine
 *   (rtc.h). When the RTC cannot be read, both tools say so and set is_error
 *   rather than falling back to uptime dressed up as a date — a wrong date is
 *   worse than no date, since the model cannot tell the difference and will
 *   reason confidently from it.
 *
 * UNTRUSTED INPUT
 *   `call->input` is model output. sys_time ignores it entirely, which is the
 *   most robust possible parse. time_convert goes through json.h only: the span
 *   must parse as a JSON object, "unix" must be present and a JSON number that
 *   fits int64 (json_int reports overflow rather than wrapping), and the value
 *   must land inside the range rtc_from_unix() can represent. Everything else
 *   becomes a message naming the problem and restating the accepted shape.
 *
 * DEPENDENCIES
 *   rtc.h (the clock), json.h (reading the span), trace.h (the ground-truth
 *   line), kernel.h for millis(). Nothing here touches a port directly.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "rtc.h"
#include "kernel.h"

#include <stdint.h>
#include <stddef.h>

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand. Export a named pointer per tool instead; tests/host/test_rtc.c stands
 * in for the linker and builds the table from those. Kernel builds are
 * untouched. */
#ifdef FABLEOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const fableos_hosttool_##var = &(var)
#endif

/* ====================================================================== */
/* helpers                                                                */
/* ====================================================================== */

/* Guarantee the model is told when output was clipped, by overwriting the tail
 * of the buffer rather than appending into one that is already full. Mirrors
 * the same backstop in tools/mem_tools.c. */
static void mark_truncated(tool_result_t *r) {
    static const char note[] = "\n[output truncated]";
    if (!r || !r->truncated || !r->buf || r->cap == 0) return;
    size_t n = sizeof note - 1;
    if (r->cap < n + 2) {
        size_t k = 0;
        if (r->cap > 1) r->buf[k++] = '!';
        r->buf[k] = '\0';
        r->len    = k;
        return;
    }
    size_t at = r->cap - 1 - n;
    for (size_t i = 0; i < n; i++) r->buf[at + i] = note[i];
    r->buf[r->cap - 1] = '\0';
    r->len = r->cap - 1;
}

/* The largest timestamp rtc_from_unix() can represent, derived from the range
 * the converter actually accepts rather than written down as a constant that
 * could drift away from RTC_YEAR_MAX. */
static int64_t unix_max(void) {
    rtc_time_t last = { RTC_YEAR_MAX, 12, 31, 23, 59, 59 };
    return rtc_to_unix(&last);
}

/* One line explaining an unreadable clock, in the same words for both tools. */
static void report_no_clock(tool_result_t *r, const char *op, int rc) {
    r->is_error = 1;
    tool_result_printf(r,
        "error: this machine has no readable wall clock right now (CMOS RTC "
        "error %d). It knows how long it has been running (%lu ms, see "
        "sys_uptime) but not what date it is, so no date can be reported and "
        "none should be guessed.\n",
        rc, (unsigned long)millis());
    mark_truncated(r);
    trace_err(rc, op, "rtc=unreadable");
}

/* ====================================================================== */
/* sys_time                                                               */
/* ====================================================================== */

static int t_sys_time(const tool_call_t *call, tool_result_t *r) {
    (void)call;                     /* no arguments: nothing to misparse */

    rtc_time_t now;
    int rc = rtc_read(&now);
    if (rc != RTC_OK) { report_no_clock(r, "sys_time", rc); return TOOL_OK; }

    int64_t ts = rtc_to_unix(&now);
    if (ts < 0) { report_no_clock(r, "sys_time", (int)ts); return TOOL_OK; }

    char iso[RTC_ISO8601_MAX];
    rtc_format_iso8601(&now, iso, sizeof iso);

    uint64_t up = millis();
    tool_result_printf(r,
        "utc:     %s\n"
        "day:     %s, %d %s %d\n"
        "unix:    %ld\n",
        iso, rtc_weekday_name(rtc_weekday(&now)),
        now.day, rtc_month_name(now.month), now.year,
        (long)ts);

    /* Boot time in wall-clock terms. Taken once at driver init, so it is a
     * measurement rather than "now minus uptime" arithmetic. */
    int64_t boot = rtc_unix_at_boot();
    if (boot >= 0) {
        rtc_time_t bt;
        char boot_iso[RTC_ISO8601_MAX];
        if (rtc_from_unix(boot, &bt) == RTC_OK &&
            rtc_format_iso8601(&bt, boot_iso, sizeof boot_iso))
            tool_result_printf(r, "booted:  %s (%lu ms ago by the timer)\n",
                               boot_iso, (unsigned long)up);
    }

    tool_result_printf(r,
        "source:  MC146818 CMOS real-time clock, read just now. The chip is "
        "assumed to hold UTC (the host default); this machine has no time-zone "
        "data and cannot verify that.\n");
    mark_truncated(r);

    trace_ret((long)ts, "sys_time", "utc=%s", iso);
    return TOOL_OK;
}

static const tool_t sys_time_tool = {
    .name        = "sys_time",
    .description =
        "Report the current date and time, read from this machine's "
        "battery-backed CMOS real-time clock: ISO 8601 UTC, the Unix "
        "timestamp, the weekday, and when the machine booted in wall-clock "
        "terms. Use this whenever the date or time is needed; do not guess it. "
        "Takes no arguments. For elapsed time since boot use sys_uptime.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_sys_time,
};
REGISTER_TOOL(sys_time_tool);

/* ====================================================================== */
/* time_convert                                                           */
/* ====================================================================== */

static int t_time_convert(const tool_call_t *call, tool_result_t *r) {
    json_value_t root, v;

    if (!call || !call->input || call->input_len == 0 ||
        json_parse(call->input, call->input_len, &root) != JSON_OK ||
        root.type != JSON_OBJECT) {
        r->is_error = 1;
        tool_result_printf(r,
            "error: expected a JSON object, e.g. {\"unix\": 1700000000}.\n");
        mark_truncated(r);
        trace_err(TOOL_EINVAL, "time_convert", "bad_input");
        return TOOL_OK;
    }

    if (json_get(&root, "unix", &v) != JSON_OK) {
        r->is_error = 1;
        tool_result_printf(r,
            "error: missing required field \"unix\" (a Unix timestamp in "
            "seconds, as an integer).\n");
        mark_truncated(r);
        trace_err(TOOL_EINVAL, "time_convert", "missing=unix");
        return TOOL_OK;
    }

    int64_t ts;
    if (v.type != JSON_NUMBER || json_int(&v, &ts) != JSON_OK) {
        r->is_error = 1;
        tool_result_printf(r,
            "error: \"unix\" must be an integer number of seconds since "
            "1970-01-01T00:00:00Z, not a string and not out of range.\n");
        mark_truncated(r);
        trace_err(TOOL_EINVAL, "time_convert", "type=unix");
        return TOOL_OK;
    }

    rtc_time_t t;
    if (rtc_from_unix(ts, &t) != RTC_OK) {
        r->is_error = 1;
        tool_result_printf(r,
            "error: \"unix\" value %ld is outside the range this kernel "
            "converts, which is 0 (%d-01-01T00:00:00Z) to %ld "
            "(%d-12-31T23:59:59Z).\n",
            (long)ts, RTC_YEAR_MIN, (long)unix_max(), RTC_YEAR_MAX);
        mark_truncated(r);
        trace_err(TOOL_EINVAL, "time_convert", "range=%ld", (long)ts);
        return TOOL_OK;
    }

    char iso[RTC_ISO8601_MAX];
    rtc_format_iso8601(&t, iso, sizeof iso);
    tool_result_printf(r, "%ld = %s (%s, %d %s %d)\n",
                       (long)ts, iso, rtc_weekday_name(rtc_weekday(&t)),
                       t.day, rtc_month_name(t.month), t.year);

    /* The comparison against a measured "now" is the point of the tool. */
    int64_t now = rtc_unix_now();
    if (now < 0) {
        tool_result_printf(r,
            "relative: unknown - the CMOS clock could not be read (error %d), "
            "so this timestamp cannot be compared against now.\n", (int)now);
    } else {
        int64_t d    = now - ts;
        int64_t mag  = d < 0 ? -d : d;
        char now_iso[RTC_ISO8601_MAX];
        rtc_time_t nt;
        now_iso[0] = '\0';
        if (rtc_from_unix(now, &nt) == RTC_OK)
            rtc_format_iso8601(&nt, now_iso, sizeof now_iso);
        tool_result_printf(r,
            "relative: %ld s (%ld days) in the %s; now is %s\n",
            (long)mag, (long)(mag / 86400),
            d == 0 ? "present" : (d > 0 ? "past" : "future"),
            now_iso[0] ? now_iso : "unknown");
    }
    mark_truncated(r);

    trace_ret((long)ts, "time_convert", "utc=%s", iso);
    return TOOL_OK;
}

static const tool_t time_convert_tool = {
    .name        = "time_convert",
    .description =
        "Convert a Unix timestamp (seconds since 1970-01-01T00:00:00Z) into a "
        "UTC calendar date, and report how far it is from the current reading "
        "of this machine's real-time clock. Use this to decide whether a "
        "deadline or a certificate validity date has passed, rather than doing "
        "the arithmetic yourself.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{\"unix\":{\"type\":\"integer\","
        "\"description\":\"seconds since 1970-01-01T00:00:00Z\"}},"
        "\"required\":[\"unix\"]}",
    .flags        = 0,
    .invoke       = t_time_convert,
};
REGISTER_TOOL(time_convert_tool);
