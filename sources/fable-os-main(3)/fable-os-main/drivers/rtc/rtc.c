/* rtc.c — MC146818 CMOS real-time clock driver. See include/rtc.h for the
 * hardware notes, the UTC assumption, and the public contract.
 *
 * LAYOUT OF THIS FILE
 *   1. the register-read backend (the only thing that touches a port)
 *   2. calendar arithmetic — pure, freestanding, exhaustively host-tested
 *   3. decode: BCD/binary, 12/24-hour, the century register
 *   4. rtc_read(): the UIP wait and the tear-free double sample
 *   5. formatting
 *   6. the driver: REGISTER_DRIVER, the device node, and a boot self-check
 *
 * Sections 2-5 compile on the host as-is. Section 1's default backend and the
 * whole of section 6 are kernel-only.
 *
 * ============================================================================
 * DECISION: this driver READS the clock and never writes it.
 * ============================================================================
 *   Setting the CMOS clock is one outb away and is deliberately not here, and
 *   not exposed as a tool. Three reasons, in order of how much they matter:
 *
 *   a. The clock is the thing certificate validity will be checked against.
 *     A wall clock that the model can move is not a security control; it is a
 *     way to make an expired certificate look current. The moment TLS date
 *     checking is switched on (rtc.h, FUTURE EXTENSION POINTS), a settable
 *     clock silently un-does it. Read-only keeps that door shut by
 *     construction rather than by policy.
 *
 *   b. Writing the calendar registers correctly means halting updates first
 *     (status B bit 7, SET), writing all seven registers, then clearing it.
 *     Get interrupted or get the encoding wrong and the chip is left with
 *     updates disabled — the clock stops, and nothing here would notice.
 *
 *   c. Nothing needs it. The host's clock is already correct, QEMU hands it
 *     straight through, and there is no NTP client and no user asking to
 *     change the date. A capability with no use and a real downside is a
 *     capability not to build.
 */

#include "rtc.h"

#ifndef FABLEOS_HOSTTEST
#  include "io.h"
#  include "kernel.h"
#  include "driver.h"
#  include "device.h"
#endif

/* ====================================================================== */
/* 1. the register-read backend                                           */
/* ====================================================================== */

#define CMOS_INDEX_PORT 0x70
#define CMOS_DATA_PORT  0x71

#ifndef FABLEOS_HOSTTEST
/* Bit 7 of the index port is the NMI disable line. It is left CLEAR (NMIs
 * enabled) rather than saved and restored: nothing else in this kernel touches
 * 0x70, so there is no state to preserve, and masking NMIs around a read we
 * expect to complete in microseconds would only widen the window in which a
 * genuine machine-check went unreported. */
static uint8_t cmos_port_read(uint8_t reg) {
    outb(CMOS_INDEX_PORT, (uint8_t)(reg & 0x7F));
    return inb(CMOS_DATA_PORT);
}
#  define CMOS_DEFAULT_BACKEND cmos_port_read
#else
/* Host builds have no ports. Every register reads back 0xFF, which sets UIP
 * forever, so rtc_read() reports RTC_ENODEV promptly instead of inventing a
 * date. Tests install a real backend with rtc_set_backend(). */
static uint8_t cmos_absent_read(uint8_t reg) {
    (void)reg;
    return 0xFF;
}
#  define CMOS_DEFAULT_BACKEND cmos_absent_read
#endif

static rtc_cmos_read_fn g_cmos = CMOS_DEFAULT_BACKEND;

void rtc_set_backend(rtc_cmos_read_fn fn) {
    g_cmos = fn ? fn : CMOS_DEFAULT_BACKEND;
}

static uint8_t cmos(uint8_t reg) { return g_cmos(reg); }

/* ====================================================================== */
/* 2. calendar arithmetic                                                 */
/* ====================================================================== */

int rtc_is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static const int MONTH_LEN[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* Days elapsed in a common year before the 1st of `month`. February's 28 is
 * baked in; the leap day is added separately so the table stays a constant. */
static const int MONTH_START[13] = {
    0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

int rtc_days_in_month(int year, int month) {
    if (month < 1 || month > 12) return 0;
    if (month == 2 && rtc_is_leap_year(year)) return 29;
    return MONTH_LEN[month - 1];
}

int rtc_time_valid(const rtc_time_t *t) {
    if (!t) return 0;
    if (t->year   < RTC_YEAR_MIN || t->year > RTC_YEAR_MAX) return 0;
    if (t->month  < 1 || t->month  > 12) return 0;
    if (t->hour   < 0 || t->hour   > 23) return 0;
    if (t->minute < 0 || t->minute > 59) return 0;
    /* No leap seconds: the MC146818 cannot produce :60 and Unix time has no
     * representation for it, so 60 is a broken reading, not a rare one. */
    if (t->second < 0 || t->second > 59) return 0;
    if (t->day    < 1 || t->day    > rtc_days_in_month(t->year, t->month)) return 0;
    return 1;
}

/* Days from 1970-01-01 to `year`-01-01, proleptic Gregorian.
 *
 * The inner expression is the standard "days before January 1st of year Y,
 * counting 0001-01-01 as day 0": whole years times 365, plus one day for every
 * completed leap year, where a leap year is every 4th, minus every 100th, plus
 * every 400th. Evaluating it at 1970 gives 719162, so subtracting that constant
 * re-bases the count on the Unix epoch. Only called with year >= RTC_YEAR_MIN,
 * so every division is on a positive value and truncation is floor. */
static int64_t days_before_year(int year) {
    int64_t y = (int64_t)year - 1;
    return y * 365 + y / 4 - y / 100 + y / 400 - 719162;
}

static int64_t days_before_month(int year, int month) {
    int64_t d = MONTH_START[month];
    if (month > 2 && rtc_is_leap_year(year)) d += 1;
    return d;
}

int64_t rtc_to_unix(const rtc_time_t *t) {
    if (!rtc_time_valid(t)) return RTC_EINVAL;
    int64_t days = days_before_year(t->year)
                 + days_before_month(t->year, t->month)
                 + (int64_t)(t->day - 1);
    return days * 86400
         + (int64_t)t->hour * 3600
         + (int64_t)t->minute * 60
         + (int64_t)t->second;
}

int rtc_from_unix(int64_t ts, rtc_time_t *out) {
    if (!out) return RTC_EINVAL;
    if (ts < 0) return RTC_EINVAL;           /* nothing before the epoch */

    int64_t days = ts / 86400;
    int64_t rem  = ts % 86400;

    int year = RTC_YEAR_MIN;
    for (;;) {
        int64_t len = rtc_is_leap_year(year) ? 366 : 365;
        if (days < len) break;
        days -= len;
        if (++year > RTC_YEAR_MAX) return RTC_EINVAL;
    }

    int month = 1;
    for (;;) {
        int64_t len = rtc_days_in_month(year, month);
        if (days < len) break;
        days -= len;
        month++;                              /* bounded: the year-loop above
                                                 guarantees days < 366 here */
    }

    out->year   = year;
    out->month  = month;
    out->day    = (int)days + 1;
    out->hour   = (int)(rem / 3600);
    out->minute = (int)((rem / 60) % 60);
    out->second = (int)(rem % 60);
    return RTC_OK;
}

int rtc_weekday(const rtc_time_t *t) {
    int64_t ts = rtc_to_unix(t);
    if (ts < 0) return RTC_EINVAL;
    /* 1970-01-01 was a Thursday, which is index 4 when Sunday is 0. */
    return (int)(((ts / 86400) + 4) % 7);
}

const char *rtc_weekday_name(int weekday) {
    static const char *const n[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday" };
    return (weekday >= 0 && weekday < 7) ? n[weekday] : "?";
}

const char *rtc_month_name(int month) {
    static const char *const n[12] = { "January", "February", "March", "April",
                                       "May", "June", "July", "August",
                                       "September", "October", "November",
                                       "December" };
    return (month >= 1 && month <= 12) ? n[month - 1] : "?";
}

/* ====================================================================== */
/* 3. decode                                                              */
/* ====================================================================== */

/* One sample of every register the calendar needs, taken back to back. Status B
 * is carried along so the encoding is the one that was in force for THIS
 * sample, and so a mode change between samples counts as a tear. */
typedef struct {
    uint8_t second, minute, hour, day, month, year, century, status_b;
} cmos_snapshot_t;

/* BCD -> binary, or a pass-through in binary mode. Returns -1 for a BCD byte
 * containing a non-decimal nibble, which is how a garbage read (0xFF from an
 * absent chip, or a value sampled mid-update) is caught before it can be
 * mistaken for a plausible field value. */
static int decode_byte(uint8_t v, int binary) {
    if (binary) return (int)v;
    if ((v & 0x0F) > 9 || (v >> 4) > 9) return -1;
    return (int)((v >> 4) * 10 + (v & 0x0F));
}

/* Turn a raw snapshot into a UTC calendar reading. RTC_OK or RTC_EINVAL. */
static int decode_snapshot(const cmos_snapshot_t *s, rtc_time_t *out) {
    const int binary = (s->status_b & RTC_STATUS_B_BINARY) != 0;
    const int h24    = (s->status_b & RTC_STATUS_B_24H) != 0;

    /* In 12-hour mode the PM flag rides in bit 7 of the hour register, ON TOP
     * of the BCD digits. Strip it first or 0x92 (12-hour BCD for 12 PM) decodes
     * as a nibble pair with a 9 in the tens place and a wrong answer. */
    uint8_t hour_raw = s->hour;
    int     pm       = 0;
    if (!h24) {
        pm       = (hour_raw & RTC_HOUR_PM) != 0;
        hour_raw = (uint8_t)(hour_raw & (uint8_t)~RTC_HOUR_PM);
    }

    int sec  = decode_byte(s->second, binary);
    int min  = decode_byte(s->minute, binary);
    int hour = decode_byte(hour_raw,  binary);
    int day  = decode_byte(s->day,    binary);
    int mon  = decode_byte(s->month,  binary);
    int yr2  = decode_byte(s->year,   binary);
    int cent = decode_byte(s->century, binary);

    if (sec < 0 || min < 0 || hour < 0 || day < 0 || mon < 0 || yr2 < 0)
        return RTC_EINVAL;

    /* BCD mode cannot produce a value above 99 — a non-decimal nibble already
     * returned -1 above — but binary mode passes the raw byte straight through,
     * and an MC146818 counter is 0..99 in either encoding. The year register is
     * the one field where an out-of-range byte still lands inside a plausible
     * date: with yr2 > 99 the century branch below is skipped, so a VALID
     * century register is silently discarded and the two-digit pivot is applied
     * to a three-digit number (year_reg=100, century=20 -> 2000, while
     * year_reg=99 -> 2099). A wrong-but-valid date is exactly the failure mode
     * this driver exists to prevent, and with certificate validity checked
     * against this clock it is a security-relevant wrong answer, so refuse it. */
    if (yr2 > 99) return RTC_EINVAL;

    if (!h24) {
        /* 12-hour clocks count 12,1,2,...,11 — so 12 AM is midnight (hour 0)
         * and 12 PM is noon (hour 12). Anything outside 1..12 is not a legal
         * 12-hour reading. */
        if (hour < 1 || hour > 12) return RTC_EINVAL;
        hour = hour % 12;
        if (pm) hour += 12;
    }

    /* The century register is advisory. Real boards leave it at 0, 0xFF, or
     * simply do not implement 0x32 at all; QEMU does implement it. Believe it
     * only when it names a century this driver could plausibly be running in,
     * otherwise fall back to the conventional two-digit pivot. Either way the
     * result is range-checked by rtc_time_valid() below. */
    int year;
    if (cent >= 19 && cent <= 29) year = cent * 100 + yr2;   /* yr2 <= 99 above */
    else if (yr2 < 70)            year = 2000 + yr2;
    else                          year = 1900 + yr2;

    rtc_time_t t;
    t.year = year; t.month = mon; t.day = day;
    t.hour = hour; t.minute = min; t.second = sec;
    if (!rtc_time_valid(&t)) return RTC_EINVAL;

    *out = t;
    return RTC_OK;
}

/* ====================================================================== */
/* 4. rtc_read: the UIP wait and the tear-free double sample              */
/* ====================================================================== */

/* How long to wait for the update window to close.
 *
 * The MC146818's update cycle is specified at under 2 ms, so the honest bound
 * is a wall-clock one and RTC_UIP_WAIT_MS is it: 20 ms is ten times the
 * hardware's worst case and still fails a dead chip fast enough that nobody
 * notices at boot.
 *
 * RTC_UIP_SPINS is the backstop for builds with no clock (the host tests), and
 * it is deliberately enormous. An earlier version bounded the wait by iteration
 * count alone at 20,000, on the reasoning that a port access costs microseconds
 * — which is true on an ISA bus and wrong under emulation. Measured on QEMU/TCG
 * a CMOS access is about 5 ns, so 20,000 iterations covered 108 us of a 244 us
 * update window and the driver reported "no clock" once per second boundary.
 * Counting iterations is not a way to measure time; the count here only exists
 * so a host test cannot loop forever. */
#define RTC_UIP_SPINS    1000000u
#define RTC_UIP_WAIT_MS  20u

/* Monotonic milliseconds, when the build has such a thing. Host test builds do
 * not, and return a constant — which disables the wall-clock bound and leaves
 * the iteration cap in charge, exactly as intended there. */
static uint64_t now_ms(void) {
#ifndef FABLEOS_HOSTTEST
    return millis();
#else
    return 0;
#endif
}

/* Snapshots needed before two agree. Each pass costs one full register sweep;
 * a real disagreement can only be caused by an update landing between two
 * sweeps, which is once a second against a sweep that takes microseconds. */
#define RTC_READ_PASSES 5

static int64_t g_boot_unix = RTC_ENODEV;

static int uip_clear(void) {
    uint64_t start = now_ms();
    for (uint32_t i = 0; i < RTC_UIP_SPINS; i++) {
        if (!(cmos(RTC_REG_STATUS_A) & RTC_STATUS_A_UIP)) return 1;
        /* Sample the clock every 256 spins: often enough to hold the 20 ms
         * bound tightly, rarely enough to stay off the hot path. */
        if ((i & 0xFF) == 0xFF && now_ms() - start > RTC_UIP_WAIT_MS) return 0;
    }
    return 0;
}

static void sample(cmos_snapshot_t *s) {
    s->status_b = cmos(RTC_REG_STATUS_B);
    s->second   = cmos(RTC_REG_SECOND);
    s->minute   = cmos(RTC_REG_MINUTE);
    s->hour     = cmos(RTC_REG_HOUR);
    s->day      = cmos(RTC_REG_DAY);
    s->month    = cmos(RTC_REG_MONTH);
    s->year     = cmos(RTC_REG_YEAR);
    s->century  = cmos(RTC_REG_CENTURY);
}

static int snapshot_eq(const cmos_snapshot_t *a, const cmos_snapshot_t *b) {
    return a->second  == b->second  && a->minute == b->minute &&
           a->hour    == b->hour    && a->day    == b->day    &&
           a->month   == b->month   && a->year   == b->year   &&
           a->century == b->century && a->status_b == b->status_b;
}

int rtc_read(rtc_time_t *out) {
    if (!out) return RTC_EINVAL;

    cmos_snapshot_t prev, cur;
    int have_prev = 0;

    for (int pass = 0; pass < RTC_READ_PASSES; pass++) {
        /* Never sample through an update: the registers are undefined while
         * UIP is set, and "undefined" here means values that look like a time.
         * A chip that never clears UIP is not ticking; say so rather than
         * sampling it anyway. */
        if (!uip_clear()) return RTC_ENODEV;

        sample(&cur);

        /* Two identical sweeps taken across a guaranteed-quiet window cannot
         * have straddled a rollover: an update between them would have moved at
         * least the seconds register. */
        if (have_prev && snapshot_eq(&prev, &cur)) {
            return decode_snapshot(&cur, out);
        }
        prev      = cur;
        have_prev = 1;
    }
    return RTC_EIO;
}

int64_t rtc_unix_now(void) {
    rtc_time_t t;
    int rc = rtc_read(&t);
    if (rc != RTC_OK) return rc;
    return rtc_to_unix(&t);
}

int64_t rtc_unix_at_boot(void) { return g_boot_unix; }

/* ====================================================================== */
/* 5. formatting                                                          */
/* ====================================================================== */

static void put2(char *dst, int v) {
    dst[0] = (char)('0' + (v / 10) % 10);
    dst[1] = (char)('0' + v % 10);
}

size_t rtc_format_iso8601(const rtc_time_t *t, char *dst, size_t cap) {
    if (!dst || cap == 0) return 0;
    if (!rtc_time_valid(t) || cap < RTC_ISO8601_MAX) { dst[0] = '\0'; return 0; }

    /* year is 1970..2999, so exactly four digits, no padding question. */
    dst[0] = (char)('0' + (t->year / 1000) % 10);
    dst[1] = (char)('0' + (t->year / 100)  % 10);
    dst[2] = (char)('0' + (t->year / 10)   % 10);
    dst[3] = (char)('0' + t->year % 10);
    dst[4] = '-';  put2(dst + 5,  t->month);
    dst[7] = '-';  put2(dst + 8,  t->day);
    dst[10] = 'T'; put2(dst + 11, t->hour);
    dst[13] = ':'; put2(dst + 14, t->minute);
    dst[16] = ':'; put2(dst + 17, t->second);
    dst[19] = 'Z';
    dst[20] = '\0';
    return 20;
}

/* ====================================================================== */
/* 6. the driver                                                          */
/* ====================================================================== */
#ifndef FABLEOS_HOSTTEST

/* kprintf() has no 64-bit conversion (lib/base.c: %u is int-sized), and a Unix
 * timestamp outgrows 32 bits in 2106. Render it here and pass a string. */
#define RTC_I64_MAX 24
static void fmt_i64(char *dst, int64_t v) {
    char tmp[RTC_I64_MAX];
    int  n = 0, o = 0;
    uint64_t u;
    if (v < 0) { dst[o++] = '-'; u = (uint64_t)(-(v + 1)) + 1u; }
    else       { u = (uint64_t)v; }
    if (u == 0) tmp[n++] = '0';
    while (u && n < (int)sizeof tmp) { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    while (n--) dst[o++] = tmp[n];
    dst[o] = '\0';
}

/* Read the clock repeatedly and check the readings against each other. This is
 * the part of the driver that cannot be proven by a single successful read: a
 * tear-handling bug shows up as one bad sample in thousands, exactly at the
 * rollover, and a boot that reads the clock once will never see it.
 *
 * The default build does a short sweep (a few milliseconds) that proves every
 * reading is a legal calendar date and that time never runs backwards. A
 * diagnostic build
 *
 *     make EXTRA_CFLAGS=-DFABLEOS_RTC_STRESS
 *
 * runs for over two seconds instead, which guarantees crossing at least two
 * second-boundaries — i.e. the update window this code exists to survive. */
#ifdef FABLEOS_RTC_STRESS
#  define RTC_SELFCHECK_READS  2000000u
#  define RTC_SELFCHECK_MS     2500u
#else
#  define RTC_SELFCHECK_READS  64u
#  define RTC_SELFCHECK_MS     0u
#endif

static void rtc_selfcheck(void) {
    uint32_t reads = 0, bad = 0, backwards = 0, rollovers = 0, jumps = 0;
    int64_t  first = -1, prev = -1;
    uint64_t deadline = RTC_SELFCHECK_MS ? millis() + RTC_SELFCHECK_MS : 0;

    for (uint32_t i = 0; i < RTC_SELFCHECK_READS; i++) {
        rtc_time_t t;
        reads++;
        if (rtc_read(&t) != RTC_OK) { bad++; continue; }

        int64_t ts = rtc_to_unix(&t);
        if (ts < 0) { bad++; continue; }
        if (first < 0) first = ts;
        if (prev >= 0) {
            if (ts < prev)          backwards++;
            else if (ts == prev + 1) rollovers++;
            else if (ts > prev + 1)  jumps++;
        }
        prev = ts;

        if (RTC_SELFCHECK_MS && millis() >= deadline) break;
    }

    char span[RTC_I64_MAX];
    fmt_i64(span, (first >= 0 && prev >= first) ? prev - first : 0);
    kprintf("rtc: self-check %u reads: %u unreadable, %u out of order, "
            "%u rollover(s) crossed, %u gap(s), %ss span\n",
            (unsigned)reads, (unsigned)bad, (unsigned)backwards,
            (unsigned)rollovers, (unsigned)jumps, span);
}

static const driver_t rtc_driver;
static int64_t g_suspend_unix = RTC_ENODEV;

static int rtc_init(void) {
    rtc_time_t t;
    int rc = rtc_read(&t);
    if (rc != RTC_OK) {
        /* No wall clock. The machine still boots and still talks; it just
         * cannot date anything, which is the state it was in before this
         * driver existed. Say so once, plainly, and carry on. */
        kprintf("rtc: no usable CMOS clock (error %d) - this machine does not "
                "know the date\n", rc);
        device_t *d = device_create("rtc0", DEV_CLASS_TIMER);
        device_add_resource(d, RES_IOPORT, CMOS_INDEX_PORT, 2);
        device_register(d);
        device_bind_driver(d, (driver_t *)&rtc_driver);
        device_set_state(d, DEV_STATE_FAILED);
        return -1;
    }

    g_boot_unix = rtc_to_unix(&t);

    char iso[RTC_ISO8601_MAX], unix_s[RTC_I64_MAX];
    rtc_format_iso8601(&t, iso, sizeof iso);
    fmt_i64(unix_s, g_boot_unix);
    kprintf("rtc: MC146818 at 0x%x: %s (%s, unix %s, assumed UTC)\n",
            CMOS_INDEX_PORT, iso, rtc_weekday_name(rtc_weekday(&t)), unix_s);

    rtc_selfcheck();

    device_t *d = device_create("rtc0", DEV_CLASS_TIMER);
    device_add_resource(d, RES_IOPORT, CMOS_INDEX_PORT, 2);
    device_add_resource(d, RES_IRQ, 8, 1);   /* wired, never unmasked; see rtc.h */
    device_register(d);
    device_bind_driver(d, (driver_t *)&rtc_driver);
    device_set_state(d, DEV_STATE_ACTIVE);
    return 0;
}

/* The RTC is the one device that keeps working while it is "off" — that is the
 * whole point of a battery-backed clock, and it is what makes it the only
 * honest way to measure how long a suspend lasted. Nothing is powered down:
 * suspend records the time, resume reads it again and reports the gap. */
static int rtc_suspend(device_t *d) {
    (void)d;
    g_suspend_unix = rtc_unix_now();
    return 0;
}

static int rtc_resume(device_t *d) {
    (void)d;
    int64_t now = rtc_unix_now();
    if (now >= 0 && g_suspend_unix >= 0) {
        char gap[RTC_I64_MAX];
        fmt_i64(gap, now - g_suspend_unix);
        kprintf("rtc: resumed, wall clock advanced %ss while suspended\n", gap);
    }
    g_suspend_unix = RTC_ENODEV;
    return 0;
}

/* DRV_LEVEL_LATE: the clock has no dependants during bring-up, and running
 * last keeps the boot self-check's few milliseconds out of the path of the
 * drivers that other subsystems actually wait on. */
static const driver_t rtc_driver = {
    .name    = "rtc",
    .level   = DRV_LEVEL_LATE,
    .cls     = DEV_CLASS_TIMER,
    .init    = rtc_init,
    .suspend = rtc_suspend,
    .resume  = rtc_resume,
};
REGISTER_DRIVER(rtc_driver);

#endif /* !FABLEOS_HOSTTEST */
