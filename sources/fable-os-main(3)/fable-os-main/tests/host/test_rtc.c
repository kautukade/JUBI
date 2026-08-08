/* test_rtc.c — the CMOS real-time clock, on the host.
 *
 * Runs the REAL drivers/rtc/rtc.c and the REAL tools/time_tools.c against the
 * real core/tool.c, net/json.c and lib/trace.c. Nothing here is a
 * reimplementation of the code under test.
 *
 * Port I/O cannot run in a host process — and on the arm64 Macs this is
 * developed on, `outb` does not even assemble — so rtc.c routes every CMOS
 * access through one function pointer (rtc_set_backend, see rtc.h). This file
 * installs a synthetic MC146818 in its place: a struct holding a calendar, an
 * encoding mode, an update-in-progress budget, and a hook that advances the
 * clock underneath the reader at a chosen moment. That last part is the whole
 * game. A tear-handling bug produces one wrong answer per rollover — once a
 * second at best, once a year at worst — so it cannot be found by reading a
 * real clock a few times and squinting. It can be found by making the rollover
 * land on register 6, deliberately, and asserting the reader did not believe
 * what it saw.
 *
 * What this suite is built to prove:
 *
 *   1. The calendar arithmetic is exact, not approximately right. Against a
 *      table of timestamps generated independently (Python datetime) covering
 *      the epoch, every leap-year rule including the 100/400 exceptions, both
 *      sides of every awkward boundary, and 2038 — plus an exhaustive walk of
 *      every single day from 1970-01-01 to 2999-12-31 in which consecutive
 *      days must be exactly 86400 seconds apart and must round-trip.
 *
 *   2. Both encodings and both hour formats decode correctly, including the
 *      two cases that are always wrong in a naive driver: 12 AM is hour 0, and
 *      the PM bit must be stripped before BCD conversion, not after.
 *
 *   3. A read is never believed while the chip says it is updating, never
 *      returns a value assembled from two different instants, and never hangs
 *      on a chip that is stuck or absent.
 *
 *   4. The two tools survive hostile input with their result buffers intact.
 *
 * Standing in for the linker: Mach-O has no __start_/__stop_ section symbols,
 * so time_tools.c exports a named pointer per tool under FABLEOS_HOSTTEST and
 * this file assembles the table core/tool.c expects.
 */

#include "harness.h"
#include "rtc.h"
#include "tool.h"
#include "trace.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* stand in for the linker-collected tool table                          */
/* ====================================================================== */

extern const tool_t *const fableos_hosttool_sys_time_tool;
extern const tool_t *const fableos_hosttool_time_convert_tool;

/* Deliberately defined WITHOUT the inner const that core/tool.c's extern
 * declaration carries: in the kernel the linker emits this table read-only,
 * here it has to be filled in at startup. Same symbol, same layout, same size.
 * (Mirrors tests/host/test_mem_tools.c.) */
#if defined(__APPLE__)
#  define SYMPFX "_"
#  define TBL_SECTION __attribute__((section("__DATA,__data")))
#else
#  define SYMPFX ""
#  define TBL_SECTION
#endif

const tool_t *__start_tool_table[2] TBL_SECTION = { 0, 0 };
extern const tool_t *const __stop_tool_table[];

__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 16\n");
_Static_assert(sizeof(__start_tool_table) == 16,
               "tool count changed: update the __stop_tool_table byte offset");

static void install_tool_table(void) {
    __start_tool_table[0] = fableos_hosttool_sys_time_tool;
    __start_tool_table[1] = fableos_hosttool_time_convert_tool;
}

/* ====================================================================== */
/* the synthetic MC146818                                                 */
/* ====================================================================== */

typedef struct {
    rtc_time_t t;             /* what the chip currently believes           */
    int        binary;        /* status B bit 2                             */
    int        h24;           /* status B bit 1                             */
    int        century_mode;  /* 0 = real value, 1 = reads 0x00, 2 = 0xFF   */

    unsigned   uip_left;      /* status A reports "updating" this many more */
    unsigned   uip_forever;   /* status A never clears                      */

    unsigned   reads;         /* register reads served so far               */
    unsigned   tick_every;    /* 0 = off; else +1s every N reads            */
    unsigned   tick_at;       /* 0 = off; else +1s exactly at read N        */

    /* Per-register override: forces one register to a hostile raw byte while
     * every other register stays consistent, which is how a single bad field
     * is isolated. */
    int        ovr_on[256];
    uint8_t    ovr_val[256];
} fake_cmos_t;

static fake_cmos_t fake;

/* Advance the synthetic clock. Uses the module's own converters, which is fine
 * here: they are stimulus, and they are separately pinned against an
 * independently generated table below before any test relies on them. */
static void fake_advance(int seconds) {
    int64_t ts = rtc_to_unix(&fake.t);
    if (ts < 0) return;
    rtc_from_unix(ts + seconds, &fake.t);
}

static uint8_t enc(int v) {
    if (fake.binary) return (uint8_t)v;
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t enc_hour(int hour24) {
    if (fake.h24) return enc(hour24);
    int pm = hour24 >= 12;
    int h  = hour24 % 12;
    if (h == 0) h = 12;                       /* 12-hour clocks show 12, not 0 */
    return (uint8_t)(enc(h) | (pm ? RTC_HOUR_PM : 0));
}

static uint8_t fake_read(uint8_t reg) {
    fake.reads++;
    if (fake.tick_every && (fake.reads % fake.tick_every) == 0) fake_advance(1);
    if (fake.tick_at && fake.reads == fake.tick_at)             fake_advance(1);
    if (fake.ovr_on[reg]) return fake.ovr_val[reg];

    switch (reg) {
    case RTC_REG_STATUS_A:
        if (fake.uip_forever) return RTC_STATUS_A_UIP;
        if (fake.uip_left) { fake.uip_left--; return RTC_STATUS_A_UIP; }
        return 0x00;
    case RTC_REG_STATUS_B:
        return (uint8_t)((fake.h24 ? RTC_STATUS_B_24H : 0) |
                         (fake.binary ? RTC_STATUS_B_BINARY : 0));
    case RTC_REG_SECOND:  return enc(fake.t.second);
    case RTC_REG_MINUTE:  return enc(fake.t.minute);
    case RTC_REG_HOUR:    return enc_hour(fake.t.hour);
    case RTC_REG_DAY:     return enc(fake.t.day);
    case RTC_REG_MONTH:   return enc(fake.t.month);
    case RTC_REG_YEAR:    return enc(fake.t.year % 100);
    case RTC_REG_CENTURY:
        if (fake.century_mode == 1) return 0x00;
        if (fake.century_mode == 2) return 0xFF;
        return enc(fake.t.year / 100);
    case RTC_REG_WEEKDAY:
        /* Deliberately wrong. The driver must never read this register: real
         * firmware gets it wrong all the time and it is derivable from the
         * date, so believing it would be a bug with no upside. */
        return 0x00;
    default:              return 0x00;
    }
}

/* A raw backend used to prove the driver reads register 6 never, and to count
 * exactly which registers are touched. */
static uint8_t touched[256];
static uint8_t counting_read(uint8_t reg) {
    if (touched[reg] < 255) touched[reg]++;
    return fake_read(reg);
}

static void fake_install(int y, int mo, int d, int h, int mi, int s,
                         int binary, int h24) {
    memset(&fake, 0, sizeof fake);
    fake.t.year = y; fake.t.month = mo; fake.t.day = d;
    fake.t.hour = h; fake.t.minute = mi; fake.t.second = s;
    fake.binary = binary;
    fake.h24    = h24;
    rtc_set_backend(fake_read);
}

static int same_time(const rtc_time_t *a, int y, int mo, int d,
                     int h, int mi, int s) {
    return a->year == y && a->month == mo && a->day == d &&
           a->hour == h && a->minute == mi && a->second == s;
}

/* ====================================================================== */
/* 1. leap years and month lengths                                        */
/* ====================================================================== */

static void test_leap_years(void) {
    /* The rule, spelled out on the cases that catch a wrong implementation. */
    CHECK(!rtc_is_leap_year(1970));
    CHECK(!rtc_is_leap_year(1971));
    CHECK( rtc_is_leap_year(1972));
    CHECK(!rtc_is_leap_year(1900));   /* century, not divisible by 400 */
    CHECK( rtc_is_leap_year(2000));   /* century, divisible by 400     */
    CHECK(!rtc_is_leap_year(2100));
    CHECK(!rtc_is_leap_year(2200));
    CHECK(!rtc_is_leap_year(2300));
    CHECK( rtc_is_leap_year(2400));
    CHECK( rtc_is_leap_year(2024));
    CHECK(!rtc_is_leap_year(2023));

    /* Cross-check the predicate against the *timestamp* arithmetic, which
     * derives leap days from a completely different expression. If they ever
     * disagree, one of them is wrong and neither can be trusted. */
    for (int y = RTC_YEAR_MIN; y < RTC_YEAR_MAX; y++) {
        rtc_time_t jan1  = { y,     1, 1, 0, 0, 0 };
        rtc_time_t next1 = { y + 1, 1, 1, 0, 0, 0 };
        int64_t len = (rtc_to_unix(&next1) - rtc_to_unix(&jan1)) / 86400;
        CHECK_EQ(len, rtc_is_leap_year(y) ? 366 : 365);
    }
}

static void test_days_in_month(void) {
    static const int common[13] = { 0, 31,28,31,30,31,30,31,31,30,31,30,31 };
    for (int m = 1; m <= 12; m++) {
        CHECK_EQ(rtc_days_in_month(2023, m), common[m]);
        CHECK_EQ(rtc_days_in_month(2024, m), m == 2 ? 29 : common[m]);
        CHECK_EQ(rtc_days_in_month(2000, m), m == 2 ? 29 : common[m]);
        CHECK_EQ(rtc_days_in_month(2100, m), common[m]);
    }
    /* Out of range must be 0, not a wild table read. */
    CHECK_EQ(rtc_days_in_month(2024, 0), 0);
    CHECK_EQ(rtc_days_in_month(2024, 13), 0);
    CHECK_EQ(rtc_days_in_month(2024, -1), 0);
    CHECK_EQ(rtc_days_in_month(2024, 12345), 0);
}

/* ====================================================================== */
/* 2. date -> timestamp, against independently generated values           */
/* ====================================================================== */

typedef struct { rtc_time_t t; int64_t unix_ts; int weekday; } known_t;

/* Generated with Python's datetime, which is not this implementation.
 * 0 = Sunday. */
static const known_t KNOWN[] = {
    { {1970, 1, 1, 0, 0, 0},           0LL, 4 },  /* Thu 01 Jan 1970 — the epoch */
    { {1970, 1, 1, 0, 0, 1},           1LL, 4 },
    { {1970, 1, 2, 0, 0, 0},       86400LL, 5 },
    { {1970,12,31,23,59,59},    31535999LL, 4 },  /* Dec 31 -> Jan 1 rollover */
    { {1971, 1, 1, 0, 0, 0},    31536000LL, 5 },
    { {1972, 2,28,23,59,59},    68169599LL, 1 },  /* first leap year after the epoch */
    { {1972, 2,29, 0, 0, 0},    68169600LL, 2 },
    { {1972, 3, 1, 0, 0, 0},    68256000LL, 3 },
    { {1973, 2,28,23,59,59},    99791999LL, 3 },  /* the same days, non-leap */
    { {1973, 3, 1, 0, 0, 0},    99792000LL, 4 },
    { {1980, 1, 1, 0, 0, 0},   315532800LL, 2 },
    { {1999,12,31,23,59,59},   946684799LL, 5 },  /* century boundary */
    { {2000, 1, 1, 0, 0, 0},   946684800LL, 6 },
    { {2000, 2,28,23,59,59},   951782399LL, 1 },  /* 2000 IS a leap year */
    { {2000, 2,29,12, 0, 0},   951825600LL, 2 },
    { {2000, 3, 1, 0, 0, 0},   951868800LL, 3 },
    { {2001, 9, 9, 1,46,40},  1000000000LL, 0 },
    { {2004, 2,29,23,59,59},  1078099199LL, 0 },
    { {2012, 6,30,23,59,59},  1341100799LL, 6 },
    { {2016,12,31,23,59,59},  1483228799LL, 6 },
    { {2017, 1, 1, 0, 0, 0},  1483228800LL, 0 },
    { {2024, 2,29, 6,30,15},  1709188215LL, 4 },
    { {2026, 7,29, 0,13,37},  1785284017LL, 3 },  /* observed on a real boot */
    { {2038, 1,19, 3,14, 7},  2147483647LL, 2 },  /* INT32_MAX */
    { {2038, 1,19, 3,14, 8},  2147483648LL, 2 },  /* and one second past it */
    { {2069,12,31,23,59,59},  3155759999LL, 2 },  /* both sides of the two-digit */
    { {2070, 1, 1, 0, 0, 0},  3155760000LL, 3 },  /* year pivot                  */
    { {2099,12,31,23,59,59},  4102444799LL, 4 },
    { {2100, 1, 1, 0, 0, 0},  4102444800LL, 5 },
    { {2100, 2,28,23,59,59},  4107542399LL, 0 },  /* 2100 is NOT a leap year */
    { {2100, 3, 1, 0, 0, 0},  4107542400LL, 1 },
    { {2104, 2,29, 0, 0, 0},  4233686400LL, 5 },
    { {2199,12,31,23,59,59},  7258118399LL, 2 },
    { {2200, 3, 1, 0, 0, 0},  7263216000LL, 6 },
    { {2400, 2,29, 0, 0, 0}, 13574563200LL, 2 },  /* 2400 IS a leap year */
    { {2999,12,31,23,59,59}, 32503679999LL, 2 },  /* the top of the range */
};
#define KNOWN_N ((int)(sizeof KNOWN / sizeof KNOWN[0]))

static void test_to_unix_known_values(void) {
    for (int i = 0; i < KNOWN_N; i++) {
        int64_t got = rtc_to_unix(&KNOWN[i].t);
        CHECK_EQ(got, KNOWN[i].unix_ts);
        CHECK_EQ(rtc_weekday(&KNOWN[i].t), KNOWN[i].weekday);
    }
}

static void test_from_unix_known_values(void) {
    for (int i = 0; i < KNOWN_N; i++) {
        rtc_time_t t;
        CHECK_EQ(rtc_from_unix(KNOWN[i].unix_ts, &t), RTC_OK);
        CHECK(same_time(&t, KNOWN[i].t.year, KNOWN[i].t.month, KNOWN[i].t.day,
                        KNOWN[i].t.hour, KNOWN[i].t.minute, KNOWN[i].t.second));
    }
}

static void test_invalid_times_are_rejected(void) {
    /* Each of these is one field away from a legal time. */
    static const rtc_time_t bad[] = {
        { 1969,12,31,23,59,59 },   /* before the epoch          */
        { 3000, 1, 1, 0, 0, 0 },   /* past the range            */
        { 2024, 0, 1, 0, 0, 0 },   /* month 0                   */
        { 2024,13, 1, 0, 0, 0 },   /* month 13                  */
        { 2024, 1, 0, 0, 0, 0 },   /* day 0                     */
        { 2024, 1,32, 0, 0, 0 },   /* day 32                    */
        { 2023, 2,29, 0, 0, 0 },   /* Feb 29 in a common year   */
        { 2100, 2,29, 0, 0, 0 },   /* Feb 29 in a century year  */
        { 2024, 4,31, 0, 0, 0 },   /* April has 30 days         */
        { 2024, 6,31, 0, 0, 0 },
        { 2024, 9,31, 0, 0, 0 },
        { 2024,11,31, 0, 0, 0 },
        { 2024, 1, 1,24, 0, 0 },   /* hour 24                   */
        { 2024, 1, 1,-1, 0, 0 },
        { 2024, 1, 1, 0,60, 0 },   /* minute 60                 */
        { 2024, 1, 1, 0, 0,60 },   /* no leap seconds           */
        { 2024, 1, 1, 0, 0,-1 },
    };
    for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
        CHECK(!rtc_time_valid(&bad[i]));
        CHECK(rtc_to_unix(&bad[i]) < 0);
        CHECK_EQ(rtc_weekday(&bad[i]), RTC_EINVAL);
    }
    CHECK(!rtc_time_valid(NULL));
    CHECK(rtc_to_unix(NULL) < 0);

    /* Feb 29 of every leap year IS legal, so the rejection above is targeted
     * and not a blanket refusal of the 29th. */
    for (int y = 1972; y < RTC_YEAR_MAX; y += 4) {
        rtc_time_t t = { y, 2, 29, 0, 0, 0 };
        CHECK_EQ(rtc_time_valid(&t), rtc_is_leap_year(y));
    }

    /* Out-of-range timestamps in the other direction. */
    rtc_time_t t;
    CHECK_EQ(rtc_from_unix(-1, &t), RTC_EINVAL);
    CHECK_EQ(rtc_from_unix(-1000000000LL, &t), RTC_EINVAL);
    CHECK_EQ(rtc_from_unix(32503680000LL, &t), RTC_EINVAL);   /* 3000-01-01 */
    CHECK_EQ(rtc_from_unix(1LL << 62, &t), RTC_EINVAL);
    CHECK_EQ(rtc_from_unix(0, NULL), RTC_EINVAL);
}

/* ====================================================================== */
/* 3. exhaustive: every day from the epoch to the end of the range        */
/* ====================================================================== */

/* Walk the calendar one day at a time, incrementing year/month/day by hand,
 * and require that rtc_to_unix() advances by exactly 86400 every single step
 * and that rtc_from_unix() lands back on the same date. 376,000-odd days, so
 * every leap year, every century rule, and every month-length transition in
 * the supported range is covered by construction rather than by sampling. */
static void test_every_day_round_trips(void) {
    int64_t expect = 0;                       /* 1970-01-01 is timestamp 0 */
    int     wd     = 4;                       /* ...and a Thursday          */
    int     bad    = 0;

    for (int y = RTC_YEAR_MIN; y <= RTC_YEAR_MAX && !bad; y++) {
        for (int m = 1; m <= 12 && !bad; m++) {
            int dim = rtc_days_in_month(y, m);
            for (int d = 1; d <= dim; d++) {
                rtc_time_t t = { y, m, d, 0, 0, 0 };
                int64_t    ts = rtc_to_unix(&t);
                rtc_time_t back;

                int ok = ts == expect
                      && rtc_weekday(&t) == wd
                      && rtc_from_unix(ts, &back) == RTC_OK
                      && same_time(&back, y, m, d, 0, 0, 0);
                CHECK(ok);            /* one check per day: ~376,000 of them */
                if (!ok) {
                    /* Report the first bad date and stop: a broken calendar
                     * would otherwise print a line for every day left. */
                    printf("    ...first bad day %04d-%02d-%02d: "
                           "ts=%lld want=%lld\n", y, m, d,
                           (long long)ts, (long long)expect);
                    bad = 1;
                    break;
                }
                expect += 86400;
                wd = (wd + 1) % 7;
            }
        }
    }
    CHECK(!bad);
    /* And the walk ended exactly where the table says it should. */
    CHECK_EQ(expect - 86400 + 86399, 32503679999LL);
}

/* The same idea inside a day: every second of a day must be representable and
 * must round-trip, which is what exercises the hour/minute/second split. */
static void test_every_second_of_a_day(void) {
    rtc_time_t base = { 2024, 2, 29, 0, 0, 0 };   /* a leap day, why not */
    int64_t    day  = rtc_to_unix(&base);
    int        bad  = 0;
    for (int s = 0; s < 86400 && !bad; s++) {
        rtc_time_t t = { 2024, 2, 29, s / 3600, (s / 60) % 60, s % 60 };
        rtc_time_t back;
        int ok = rtc_to_unix(&t) == day + s
              && rtc_from_unix(day + s, &back) == RTC_OK
              && same_time(&back, t.year, t.month, t.day,
                           t.hour, t.minute, t.second);
        CHECK(ok);
        if (!ok) { printf("    ...first bad second of day: %d\n", s); bad = 1; }
    }
}

/* ====================================================================== */
/* 4. BCD / binary and 12 / 24 hour decoding                              */
/* ====================================================================== */

static void test_bcd_and_binary_modes(void) {
    rtc_time_t t;

    /* BCD, 24-hour: the QEMU/PC default. 0x59 must read as 59, not 89. */
    fake_install(2026, 7, 29, 23, 59, 58, 0 /* BCD */, 1 /* 24h */);
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK(same_time(&t, 2026, 7, 29, 23, 59, 58));

    /* Binary, 24-hour. */
    fake_install(2026, 7, 29, 23, 59, 58, 1, 1);
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK(same_time(&t, 2026, 7, 29, 23, 59, 58));

    /* Every hour of a day, in all four combinations of the two mode bits. */
    for (int binary = 0; binary <= 1; binary++)
        for (int h24 = 0; h24 <= 1; h24++)
            for (int h = 0; h < 24; h++) {
                fake_install(2024, 2, 29, h, 34, 56, binary, h24);
                CHECK_EQ(rtc_read(&t), RTC_OK);
                CHECK(same_time(&t, 2024, 2, 29, h, 34, 56));
            }

    /* Every minute/second value, so no BCD digit pair is left untried. */
    for (int v = 0; v < 60; v++) {
        fake_install(2024, 6, 15, 12, v, 59 - v, 0, 1);
        CHECK_EQ(rtc_read(&t), RTC_OK);
        CHECK(same_time(&t, 2024, 6, 15, 12, v, 59 - v));
    }
}

static void test_twelve_hour_edge_cases(void) {
    rtc_time_t t;

    /* The two that a naive driver gets wrong: 12 AM is midnight (hour 0) and
     * 12 PM is noon (hour 12). Asserted through the raw register value so the
     * test does not depend on the encoder above being right. */
    fake_install(2026, 1, 1, 0, 0, 0, 0 /* BCD */, 0 /* 12h */);
    CHECK_EQ(fake_read(RTC_REG_HOUR), 0x12);          /* 12 AM, PM bit clear */
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.hour, 0);

    fake_install(2026, 1, 1, 12, 0, 0, 0, 0);
    CHECK_EQ(fake_read(RTC_REG_HOUR), 0x92);          /* 12 PM = 0x80 | 0x12 */
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.hour, 12);

    fake_install(2026, 1, 1, 13, 0, 0, 0, 0);
    CHECK_EQ(fake_read(RTC_REG_HOUR), 0x81);          /* 1 PM */
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.hour, 13);

    fake_install(2026, 1, 1, 23, 0, 0, 0, 0);
    CHECK_EQ(fake_read(RTC_REG_HOUR), 0x91);          /* 11 PM = 0x80 | 0x11 */
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.hour, 23);

    fake_install(2026, 1, 1, 11, 0, 0, 0, 0);
    CHECK_EQ(fake_read(RTC_REG_HOUR), 0x11);          /* 11 AM */
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.hour, 11);

    /* Binary 12-hour: the PM bit rides on a binary value instead. */
    fake_install(2026, 1, 1, 12, 0, 0, 1 /* binary */, 0);
    CHECK_EQ(fake_read(RTC_REG_HOUR), 0x8C);          /* 0x80 | 12 */
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.hour, 12);

    fake_install(2026, 1, 1, 0, 0, 0, 1, 0);
    CHECK_EQ(fake_read(RTC_REG_HOUR), 0x0C);          /* 12, PM bit clear */
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.hour, 0);
}

/* Hour 0 and hours past 12 are not legal 12-hour readings; a chip claiming
 * 12-hour mode while reporting them is broken, and a broken reading must be
 * refused rather than folded into something plausible. */
static void test_illegal_twelve_hour_values(void) {
    static const struct { uint8_t raw; int rc; } cases[] = {
        { 0x00, RTC_EINVAL },   /* hour 0 does not exist on a 12-hour clock  */
        { 0x80, RTC_EINVAL },   /* "0 PM"                                    */
        { 0x13, RTC_EINVAL },   /* BCD 13: past 12                           */
        { 0x19, RTC_EINVAL },
        { 0x99, RTC_EINVAL },
        { 0x0A, RTC_EINVAL },   /* not valid BCD at all                      */
        { 0x1A, RTC_EINVAL },
        { 0xFF, RTC_EINVAL },   /* the value an absent register reads back   */
        { 0x01, RTC_OK      },  /* 1 AM  */
        { 0x12, RTC_OK      },  /* 12 AM */
        { 0x91, RTC_OK      },  /* 11 PM */
        { 0x92, RTC_OK      },  /* 12 PM */
    };
    for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
        rtc_time_t t;
        fake_install(2026, 1, 1, 9, 0, 0, 0 /* BCD */, 0 /* 12h */);
        fake.ovr_on[RTC_REG_HOUR]  = 1;
        fake.ovr_val[RTC_REG_HOUR] = cases[i].raw;
        CHECK_EQ(rtc_read(&t), cases[i].rc);
    }
}

/* Any register can come back as something that is not a possible value —
 * a nibble outside 0-9 in BCD mode, or a legal-looking number that is not a
 * legal calendar field. Both must be refused, one field at a time. */
static void test_impossible_field_values_are_refused(void) {
    static const struct { uint8_t reg; uint8_t raw; } bad[] = {
        { RTC_REG_SECOND, 0x5A },   /* 'A' is not a BCD digit */
        { RTC_REG_SECOND, 0xA5 },
        { RTC_REG_SECOND, 0x60 },   /* 60 seconds: no leap seconds here */
        { RTC_REG_SECOND, 0x99 },
        { RTC_REG_MINUTE, 0x60 },
        { RTC_REG_MINUTE, 0xFF },
        { RTC_REG_HOUR,   0x24 },   /* hour 24 in 24-hour mode */
        { RTC_REG_HOUR,   0x99 },
        { RTC_REG_DAY,    0x00 },   /* day 0 */
        { RTC_REG_DAY,    0x32 },   /* day 32 */
        { RTC_REG_DAY,    0xFF },
        { RTC_REG_MONTH,  0x00 },
        { RTC_REG_MONTH,  0x13 },
        { RTC_REG_MONTH,  0xFF },
        { RTC_REG_YEAR,   0xAB },
    };
    for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
        rtc_time_t t;
        fake_install(2026, 7, 29, 12, 0, 0, 0 /* BCD */, 1 /* 24h */);
        fake.ovr_on[bad[i].reg]  = 1;
        fake.ovr_val[bad[i].reg] = bad[i].raw;
        CHECK_EQ(rtc_read(&t), RTC_EINVAL);
    }

    /* 29 February of a common year is the one that needs the calendar, not
     * just a range check: every field is individually legal. */
    rtc_time_t t;
    fake_install(2023, 3, 1, 12, 0, 0, 0, 1);
    fake.ovr_on[RTC_REG_MONTH] = 1; fake.ovr_val[RTC_REG_MONTH] = 0x02;
    fake.ovr_on[RTC_REG_DAY]   = 1; fake.ovr_val[RTC_REG_DAY]   = 0x29;
    CHECK_EQ(rtc_read(&t), RTC_EINVAL);

    /* ...and the same registers in a leap year are accepted. */
    fake_install(2024, 3, 1, 12, 0, 0, 0, 1);
    fake.ovr_on[RTC_REG_MONTH] = 1; fake.ovr_val[RTC_REG_MONTH] = 0x02;
    fake.ovr_on[RTC_REG_DAY]   = 1; fake.ovr_val[RTC_REG_DAY]   = 0x29;
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK(same_time(&t, 2024, 2, 29, 12, 0, 0));
}

/* ====================================================================== */
/* 5. the century register                                                */
/* ====================================================================== */

static void test_century_register(void) {
    rtc_time_t t;

    /* Present and correct: believed. */
    fake_install(2026, 7, 29, 12, 0, 0, 0, 1);
    fake.century_mode = 0;
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.year, 2026);

    fake_install(2104, 3, 1, 12, 0, 0, 0, 1);
    fake.century_mode = 0;
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.year, 2104);

    /* Unimplemented (reads 0x00): fall back to the two-digit pivot, so 26
     * means 2026 and 85 means 1985. */
    fake_install(2026, 7, 29, 12, 0, 0, 0, 1);
    fake.century_mode = 1;
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.year, 2026);

    fake_install(1985, 7, 29, 12, 0, 0, 0, 1);
    fake.century_mode = 1;
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.year, 1985);

    /* Absent (reads 0xFF, and 0xFF is not valid BCD either): same fallback. */
    fake_install(2026, 7, 29, 12, 0, 0, 0, 1);
    fake.century_mode = 2;
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.year, 2026);

    fake_install(2026, 7, 29, 12, 0, 0, 1 /* binary */, 1);
    fake.century_mode = 2;                          /* 0xFF = 255 in binary */
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.year, 2026);

    /* The pivot itself: 69 -> 2069, 70 -> 1970. */
    fake_install(2069, 1, 1, 0, 0, 0, 0, 1); fake.century_mode = 1;
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.year, 2069);
    fake_install(1970, 1, 1, 0, 0, 0, 0, 1); fake.century_mode = 1;
    CHECK_EQ(rtc_read(&t), RTC_OK);
    CHECK_EQ(t.year, 1970);
}

/* ====================================================================== */
/* 6. update-in-progress                                                  */
/* ====================================================================== */

static void test_uip_is_waited_out(void) {
    rtc_time_t t;

    /* The chip says "updating" for a while, then settles. The reader must
     * wait, not sample through it. */
    for (unsigned n = 0; n <= 64; n += 8) {
        fake_install(2026, 7, 29, 12, 0, 0, 0, 1);
        fake.uip_left = n;
        CHECK_EQ(rtc_read(&t), RTC_OK);
        CHECK(same_time(&t, 2026, 7, 29, 12, 0, 0));
    }
}

static void test_stuck_uip_never_hangs(void) {
    rtc_time_t t;
    fake_install(2026, 7, 29, 12, 0, 0, 0, 1);
    fake.uip_forever = 1;
    /* Bounded: if this ever loops forever the test binary hangs, which is a
     * louder failure than an assertion. */
    CHECK_EQ(rtc_read(&t), RTC_ENODEV);
    CHECK(fake.reads > 0);
}

static void test_absent_chip_reports_no_device(void) {
    rtc_time_t t;
    rtc_set_backend(NULL);                   /* restore the "no hardware" default */
    /* Every register reads 0xFF, so status A has UIP set forever. */
    CHECK_EQ(rtc_read(&t), RTC_ENODEV);
    CHECK(rtc_unix_now() < 0);
}

static void test_null_argument(void) {
    fake_install(2026, 7, 29, 12, 0, 0, 0, 1);
    CHECK_EQ(rtc_read(NULL), RTC_EINVAL);
}

/* ====================================================================== */
/* 7. tearing                                                             */
/* ====================================================================== */

/* Roll the clock over from one instant to the next at every possible point
 * inside the first two register sweeps. Whatever moment the update lands on,
 * the answer must be ONE OF THE TWO WHOLE INSTANTS — never a mixture of them.
 *
 * Both outcomes are correct. Depending on where the update lands, the reader
 * either sees it and re-reads (giving the new instant) or completes two
 * agreeing sweeps before it happens (giving the old one, which was genuinely
 * true when it was sampled). What must never happen is the third outcome: a
 * value the chip never held, such as the seconds of the old instant beside the
 * year of the new one. That is the reading that would put a certificate check
 * a year out.
 *
 * (Which of the two comes back depends on the boundary: at a New Year every
 * register moves, so every tear is visible and the retry always lands on the
 * new instant; at a leap-day rollover the month/year/century bytes are
 * unchanged, so a tear confined to them is indistinguishable from a clean read
 * of the old instant — and returning that old instant is the right answer.) */
static void tear_across(int y0, int mo0, int d0, int h0, int mi0, int s0,
                        int y1, int mo1, int d1, int h1, int mi1, int s1) {
    int saw_old = 0, saw_new = 0;

    for (unsigned at = 1; at <= 18; at++) {
        rtc_time_t t;
        fake_install(y0, mo0, d0, h0, mi0, s0, 0, 1);
        fake.tick_at = at;

        int rc = rtc_read(&t);
        CHECK_EQ(rc, RTC_OK);
        if (rc != RTC_OK) continue;

        int old_one = same_time(&t, y0, mo0, d0, h0, mi0, s0);
        int new_one = same_time(&t, y1, mo1, d1, h1, mi1, s1);
        if (!(old_one || new_one))
            printf("    FAIL torn reading at read %u: %04d-%02d-%02d "
                   "%02d:%02d:%02d\n", at, t.year, t.month, t.day,
                   t.hour, t.minute, t.second);
        CHECK(old_one || new_one);
        saw_old += old_one;
        saw_new += new_one;
    }
    /* The retry path fired at least once, and every single iteration produced
     * one of the two real instants. */
    CHECK(saw_new > 0);
    CHECK_EQ(saw_old + saw_new, 18);
}

static void test_tear_at_new_year(void) {
    /* 1999-12-31 23:59:59 -> 2000-01-01 00:00:00: every field changes at once,
     * including the century register. */
    tear_across(1999,12,31,23,59,59,  2000, 1, 1, 0, 0, 0);
}

static void test_tear_at_leap_day(void) {
    tear_across(2024, 2,28,23,59,59,  2024, 2,29, 0, 0, 0);
    tear_across(2024, 2,29,23,59,59,  2024, 3, 1, 0, 0, 0);
    tear_across(2100, 2,28,23,59,59,  2100, 3, 1, 0, 0, 0);   /* no Feb 29 */
}

static void test_tear_forever_is_reported_not_guessed(void) {
    /* A chip whose registers change every few reads can never produce two
     * matching sweeps. The reader must give up and say so rather than return
     * whichever mixture it happened to hold. */
    rtc_time_t t;
    fake_install(2026, 7, 29, 12, 0, 0, 0, 1);
    fake.tick_every = 3;
    CHECK_EQ(rtc_read(&t), RTC_EIO);
}

/* The tight loop the task asks for, run against a synthetic chip that ticks at
 * a realistic rate: the reading must never go backwards, never skip, and never
 * be an illegal date. */
static void test_tight_loop_is_never_garbage(void) {
    rtc_time_t t;
    int64_t    prev = -1;
    int        bad = 0, backwards = 0, wild = 0, rollovers = 0;
    rtc_time_t y2k = { 2000, 1, 1, 0, 0, 0 };

    fake_install(1999, 12, 31, 23, 59, 55, 0, 1);
    fake.tick_every = 37;            /* about one second per four sweeps, so
                                        updates land inside sweeps constantly */
    for (int i = 0; i < 20000; i++) {
        if (rtc_read(&t) != RTC_OK) { bad++; continue; }
        if (!rtc_time_valid(&t))    { bad++; continue; }
        int64_t ts = rtc_to_unix(&t);
        if (ts < 0)                 { bad++; continue; }
        if (prev >= 0) {
            if (ts < prev)           backwards++;
            else if (ts == prev + 1) rollovers++;
            /* A torn reading across this particular boundary is out by a year
             * or a month, never by a couple of seconds — so anything past a
             * small slack is garbage rather than time passing. */
            else if (ts > prev + 4)  wild++;
        }
        prev = ts;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(backwards, 0);
    CHECK_EQ(wild, 0);
    CHECK(rollovers > 100);          /* the loop really did cross rollovers */
    CHECK(prev >= rtc_to_unix(&y2k)); /* ...including the year boundary */
}

/* ====================================================================== */
/* 8. the weekday register is never trusted                               */
/* ====================================================================== */

static void test_weekday_register_is_not_read(void) {
    rtc_time_t t;
    memset(touched, 0, sizeof touched);
    fake_install(2026, 7, 29, 12, 0, 0, 0, 1);
    rtc_set_backend(counting_read);
    CHECK_EQ(rtc_read(&t), RTC_OK);
    /* The synthetic chip returns 0 (= Sunday, if anyone believed it) for
     * register 6. 2026-07-29 is a Wednesday. */
    CHECK_EQ(touched[RTC_REG_WEEKDAY], 0);
    CHECK_EQ(rtc_weekday(&t), 3);
    CHECK_STR(rtc_weekday_name(rtc_weekday(&t)), "Wednesday");
    /* And the registers it does read are the ones it needs. */
    CHECK(touched[RTC_REG_STATUS_A] > 0);
    CHECK(touched[RTC_REG_STATUS_B] > 0);
    CHECK(touched[RTC_REG_SECOND]   > 0);
    CHECK(touched[RTC_REG_YEAR]     > 0);
    CHECK(touched[RTC_REG_CENTURY]  > 0);
    rtc_set_backend(fake_read);
}

static void test_name_helpers(void) {
    CHECK_STR(rtc_weekday_name(0), "Sunday");
    CHECK_STR(rtc_weekday_name(6), "Saturday");
    CHECK_STR(rtc_weekday_name(-1), "?");
    CHECK_STR(rtc_weekday_name(7), "?");
    CHECK_STR(rtc_weekday_name(999999), "?");
    CHECK_STR(rtc_month_name(1), "January");
    CHECK_STR(rtc_month_name(12), "December");
    CHECK_STR(rtc_month_name(0), "?");
    CHECK_STR(rtc_month_name(13), "?");
    CHECK_STR(rtc_month_name(-5), "?");
}

/* ====================================================================== */
/* 9. formatting                                                          */
/* ====================================================================== */

static void test_iso8601(void) {
    char buf[64];
    rtc_time_t t;

    memset(buf, 0x5A, sizeof buf);
    t = (rtc_time_t){ 2026, 7, 29, 0, 13, 37 };
    CHECK_EQ(rtc_format_iso8601(&t, buf, sizeof buf), 20);
    CHECK_STR(buf, "2026-07-29T00:13:37Z");

    t = (rtc_time_t){ 1970, 1, 1, 0, 0, 0 };
    CHECK_EQ(rtc_format_iso8601(&t, buf, sizeof buf), 20);
    CHECK_STR(buf, "1970-01-01T00:00:00Z");

    t = (rtc_time_t){ 2999, 12, 31, 23, 59, 59 };
    CHECK_EQ(rtc_format_iso8601(&t, buf, sizeof buf), 20);
    CHECK_STR(buf, "2999-12-31T23:59:59Z");

    /* Exactly enough room. */
    char tight[RTC_ISO8601_MAX];
    t = (rtc_time_t){ 2000, 2, 29, 9, 8, 7 };
    CHECK_EQ(rtc_format_iso8601(&t, tight, sizeof tight), 20);
    CHECK_STR(tight, "2000-02-29T09:08:07Z");

    /* One byte short: refuse and terminate, never a partial date. */
    char small[RTC_ISO8601_MAX - 1];
    memset(small, 0x5A, sizeof small);
    CHECK_EQ(rtc_format_iso8601(&t, small, sizeof small), 0);
    CHECK_STR(small, "");

    /* Invalid time, NULL destination, zero capacity. */
    rtc_time_t bad = { 2023, 2, 29, 0, 0, 0 };
    CHECK_EQ(rtc_format_iso8601(&bad, buf, sizeof buf), 0);
    CHECK_STR(buf, "");
    CHECK_EQ(rtc_format_iso8601(&t, NULL, 64), 0);
    CHECK_EQ(rtc_format_iso8601(&t, buf, 0), 0);
    CHECK_EQ(rtc_format_iso8601(NULL, buf, sizeof buf), 0);

    /* The formatter must not write past what it claims. */
    char guarded[40];
    memset(guarded, 0x5A, sizeof guarded);
    t = (rtc_time_t){ 2026, 7, 29, 0, 13, 37 };
    CHECK_EQ(rtc_format_iso8601(&t, guarded, RTC_ISO8601_MAX), 20);
    for (size_t i = RTC_ISO8601_MAX; i < sizeof guarded; i++)
        CHECK_EQ((unsigned char)guarded[i], 0x5A);
}

/* ====================================================================== */
/* 10. the tools                                                          */
/* ====================================================================== */

#define GUARD      64
#define GUARD_BYTE 0xA5

static char          g_store[GUARD + TOOL_RESULT_MAX + GUARD];
static char         *g_out = g_store + GUARD;
static tool_result_t g_res;

static void guards_reset(void) { memset(g_store, GUARD_BYTE, sizeof g_store); }

static int guards_intact(size_t cap) {
    for (size_t i = 0; i < GUARD; i++)
        if ((unsigned char)g_store[i] != GUARD_BYTE) return 0;
    for (size_t i = GUARD + cap; i < sizeof g_store; i++)
        if ((unsigned char)g_store[i] != GUARD_BYTE) return 0;
    return 1;
}

/* Dispatch with a raw (deliberately NOT NUL-terminated) input span. */
static const char *call_n(const char *name, const char *input, size_t len,
                          size_t cap) {
    guards_reset();
    tool_result_init(&g_res, g_out, cap);

    static char span[8192];
    memset(span, 0x5A, sizeof span);
    if (input && len) {
        if (len > sizeof span) len = sizeof span;
        memcpy(span, input, len);
    }

    tool_call_t c = { .id = "toolu_test", .name = name,
                      .input = input ? span : NULL, .input_len = input ? len : 0 };
    tool_dispatch(&c, &g_res);
    CHECK(guards_intact(cap));
    return g_out;
}

static const char *call(const char *name, const char *input) {
    return call_n(name, input, input ? strlen(input) : 0, TOOL_RESULT_MAX);
}

static void test_tools_are_registered(void) {
    CHECK_EQ(tool_count(), 2);
    CHECK(tool_find("sys_time") != NULL);
    CHECK(tool_find("time_convert") != NULL);
    /* Neither changes machine state, so neither is flagged. */
    CHECK_TOOL_FLAGS("sys_time", ~0u, 0u);
    CHECK_TOOL_FLAGS("time_convert", ~0u, 0u);

    /* The advertised schema must be real JSON containing both names. */
    static char schema[8192];
    size_t n = tools_build_schema(schema, sizeof schema);
    CHECK(n > 0 && n < sizeof schema);
    CHECK_CONTAINS(schema, "sys_time");
    CHECK_CONTAINS(schema, "time_convert");
    json_value_t root;
    CHECK_EQ(json_parse(schema, n, &root), JSON_OK);
    CHECK_EQ(root.type, JSON_ARRAY);
    CHECK_EQ((int)json_count(&root), 2);
}

static void test_sys_time_reports_the_clock(void) {
    fake_install(2026, 7, 29, 0, 13, 37, 0, 1);
    uint64_t before = trace_emissions();
    const char *out = call("sys_time", "{}");
    CHECK_EQ((int)(trace_emissions() - before), 1);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "2026-07-29T00:13:37Z");
    CHECK_CONTAINS(out, "1785284017");
    CHECK_CONTAINS(out, "Wednesday");
    CHECK_CONTAINS(out, "29 July 2026");
    CHECK_CONTAINS(out, "UTC");
    /* The trace line is the kernel's, and it carries the same reading. */
    CHECK_CONTAINS(kcap_text(), "[sys_time utc=2026-07-29T00:13:37Z -> 1785284017]");

    /* A different instant gives a different answer: the tool is reading, not
     * remembering. */
    fake_install(2000, 2, 29, 12, 0, 0, 0, 0 /* 12-hour BCD this time */);
    out = call("sys_time", "{}");
    CHECK_CONTAINS(out, "2000-02-29T12:00:00Z");
    CHECK_CONTAINS(out, "951825600");
}

static void test_sys_time_ignores_its_input(void) {
    fake_install(2026, 7, 29, 0, 13, 37, 0, 1);
    static const char *junk[] = {
        "{}", "", "not json", "[[[[[[[[", "{\"unix\":", "null", "1e999",
        "{\"a\":\"\xff\xfe\"}", "\x00\x01\x02",
    };
    for (int i = 0; i < (int)(sizeof junk / sizeof junk[0]); i++) {
        const char *out = call("sys_time", junk[i]);
        CHECK_EQ(g_res.is_error, 0);
        CHECK_CONTAINS(out, "2026-07-29T00:13:37Z");
    }
    /* Including a NULL span. */
    const char *out = call_n("sys_time", NULL, 0, TOOL_RESULT_MAX);
    CHECK_CONTAINS(out, "2026-07-29T00:13:37Z");
}

static void test_sys_time_without_a_clock(void) {
    rtc_set_backend(NULL);                   /* no hardware */
    const char *out = call("sys_time", "{}");
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(out, "no readable wall clock");
    /* It must not invent a date. */
    CHECK(strstr(out, "T00:00:00Z") == NULL);
    /* trace.h renders a code its table does not know as the signed integer,
     * which is what RTC_ENODEV (-19) does today — the table in lib/trace.c
     * carries the VFS/model codes only. Still legible, still unforgeable. */
    CHECK_CONTAINS(kcap_text(), "[sys_time rtc=unreadable -> -19]");
}

static void test_time_convert_valid(void) {
    fake_install(2026, 7, 29, 0, 13, 37, 0, 1);

    const char *out = call("time_convert", "{\"unix\":0}");
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "1970-01-01T00:00:00Z");
    CHECK_CONTAINS(out, "Thursday");
    CHECK_CONTAINS(out, "past");

    out = call("time_convert", "{\"unix\":951825600}");
    CHECK_CONTAINS(out, "2000-02-29T12:00:00Z");

    out = call("time_convert", "{\"unix\":32503679999}");
    CHECK_CONTAINS(out, "2999-12-31T23:59:59Z");
    CHECK_CONTAINS(out, "future");

    out = call("time_convert", "{\"unix\":1785284017}");
    CHECK_CONTAINS(out, "2026-07-29T00:13:37Z");
    CHECK_CONTAINS(out, "present");

    /* Extra members are ignored, not fatal. */
    out = call("time_convert", "{\"noise\":[1,2,3],\"unix\":0,\"more\":{}}");
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "1970-01-01T00:00:00Z");
}

static void test_time_convert_rejects_bad_input(void) {
    fake_install(2026, 7, 29, 0, 13, 37, 0, 1);
    static const char *bad[] = {
        "",                          /* empty span            */
        "{}",                        /* missing field         */
        "[]",                        /* not an object         */
        "\"a string\"",
        "42",
        "{\"unix\":\"1700000000\"}", /* string, not integer   */
        "{\"unix\":null}",
        "{\"unix\":true}",
        "{\"unix\":[0]}",
        "{\"unix\":{}}",
        "{\"unix\":-1}",             /* before the epoch      */
        "{\"unix\":-9223372036854775807}",
        "{\"unix\":32503680000}",    /* one past the range    */
        "{\"unix\":99999999999999}",
        "{\"unix\":1e30}",           /* exponent: rejected    */
        "{\"unix\":",                /* truncated             */
        "{\"unix\":0",
        "not json at all",
    };
    for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
        const char *out = call("time_convert", bad[i]);
        CHECK_EQ(g_res.is_error, 1);
        CHECK_CONTAINS(out, "error:");
        /* Every rejection still names the shape the tool wants. */
        CHECK(strstr(out, "unix") != NULL);
    }
}

static void test_time_convert_without_a_clock(void) {
    rtc_set_backend(NULL);
    const char *out = call("time_convert", "{\"unix\":0}");
    /* The conversion itself does not need the clock, so it still succeeds; only
     * the "how far from now" half is reported as unknown. */
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "1970-01-01T00:00:00Z");
    CHECK_CONTAINS(out, "relative: unknown");
}

/* BINARY-mode registers were passed straight through with no range check. BCD
 * cannot produce a value above 99 (a non-decimal nibble already returns -1), but
 * binary can, and the YEAR register is the one field where an out-of-range byte
 * still lands inside a plausible date: with yr2 > 99 the century branch is
 * skipped, so a VALID century register is discarded and the two-digit pivot is
 * applied to a three-digit number. year_reg=99 gave 2099 (century honoured);
 * year_reg=100 gave 2000 (century ignored) and rtc_read() returned RTC_OK.
 *
 * A wrong-but-valid date is the exact failure mode this driver exists to prevent
 * — and with certificate validity checked against this clock, it is a
 * security-relevant wrong answer. Note the existing hostile-byte table runs in
 * BCD only, which is why this survived: BCD is the encoding that happened to
 * have a guard. */
static void test_binary_mode_rejects_out_of_range_registers(void) {
    struct { uint8_t reg; int lo; int hi; } f[] = {
        { RTC_REG_SECOND, 0, 59 }, { RTC_REG_MINUTE, 0, 59 },
        { RTC_REG_HOUR,   0, 23 }, { RTC_REG_DAY,    1, 31 },
        { RTC_REG_MONTH,  1, 12 }, { RTC_REG_YEAR,   0, 99 },
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        for (int v = 0; v < 256; v++) {
            fake_install(2026, 6, 15, 12, 30, 30, 1 /* BINARY */, 1 /* 24h */);
            fake.ovr_on[f[i].reg]  = 1;
            fake.ovr_val[f[i].reg] = (uint8_t)v;
            rtc_time_t t;
            int rc = rtc_read(&t);
            if (v < f[i].lo || v > f[i].hi) CHECK(rc != RTC_OK);
        }
    }

    /* The century register must be honoured consistently either side of 99,
     * rather than being silently dropped one byte later. */
    for (int y = 96; y <= 99; y++) {
        fake_install(2026, 6, 15, 12, 30, 30, 1, 1);
        fake.ovr_on[RTC_REG_YEAR]     = 1;
        fake.ovr_val[RTC_REG_YEAR]    = (uint8_t)y;
        fake.ovr_on[RTC_REG_CENTURY]  = 1;
        fake.ovr_val[RTC_REG_CENTURY] = 20;
        rtc_time_t t;
        CHECK_EQ(rtc_read(&t), RTC_OK);
        CHECK_EQ(t.year, 2000 + y);
    }
    for (int y = 100; y <= 255; y++) {
        fake_install(2026, 6, 15, 12, 30, 30, 1, 1);
        fake.ovr_on[RTC_REG_YEAR]     = 1;
        fake.ovr_val[RTC_REG_YEAR]    = (uint8_t)y;
        fake.ovr_on[RTC_REG_CENTURY]  = 1;
        fake.ovr_val[RTC_REG_CENTURY] = 20;
        rtc_time_t t;
        CHECK(rtc_read(&t) != RTC_OK);
    }
}

static void test_tiny_result_buffers(void) {
    fake_install(2026, 7, 29, 0, 13, 37, 0, 1);
    for (size_t cap = 1; cap <= 96; cap++) {
        call_n("sys_time", "{}", 2, cap);
        CHECK(guards_intact(cap));
        CHECK(g_res.len < cap);
        CHECK(memchr(g_out, '\0', cap) != NULL);   /* always terminated */

        call_n("time_convert", "{\"unix\":0}", 10, cap);
        CHECK(guards_intact(cap));
        CHECK(g_res.len < cap);
        CHECK(memchr(g_out, '\0', cap) != NULL);
    }
}

static void test_fuzz(void) {
    fake_install(2026, 7, 29, 0, 13, 37, 0, 1);
    unsigned seed = 0x71C0FFEEu;
    char     buf[512];

    for (int iter = 0; iter < 4000; iter++) {
        size_t n = 0;
        /* Half the time, start from something that looks like the real shape
         * so the fuzzer spends its budget past the first parse failure. */
        if (iter & 1) {
            const char *pfx = "{\"unix\":";
            for (const char *p = pfx; *p; p++) buf[n++] = *p;
        }
        size_t extra = (seed >> 8) % 64;
        for (size_t i = 0; i < extra && n < sizeof buf; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[n++] = (char)((seed >> 16) & 0xFF);
        }
        seed = seed * 1103515245u + 12345u;

        const char *name = (seed & 1) ? "sys_time" : "time_convert";
        call_n(name, buf, n, TOOL_RESULT_MAX);
        CHECK(guards_intact(TOOL_RESULT_MAX));
        CHECK(g_res.len < TOOL_RESULT_MAX);
        CHECK_EQ(kpanic_hit, 0);
    }
}

/* ====================================================================== */

int main(void) {
    install_tool_table();
    kcap_reset();

    RUN(test_leap_years);
    RUN(test_days_in_month);
    RUN(test_to_unix_known_values);
    RUN(test_from_unix_known_values);
    RUN(test_invalid_times_are_rejected);
    RUN(test_every_day_round_trips);
    RUN(test_every_second_of_a_day);

    RUN(test_bcd_and_binary_modes);
    RUN(test_twelve_hour_edge_cases);
    RUN(test_illegal_twelve_hour_values);
    RUN(test_impossible_field_values_are_refused);
    RUN(test_century_register);

    RUN(test_uip_is_waited_out);
    RUN(test_stuck_uip_never_hangs);
    RUN(test_absent_chip_reports_no_device);
    RUN(test_null_argument);

    RUN(test_tear_at_new_year);
    RUN(test_tear_at_leap_day);
    RUN(test_tear_forever_is_reported_not_guessed);
    RUN(test_tight_loop_is_never_garbage);

    RUN(test_weekday_register_is_not_read);
    RUN(test_name_helpers);
    RUN(test_iso8601);

    RUN(test_tools_are_registered);
    RUN(test_sys_time_reports_the_clock);
    RUN(test_sys_time_ignores_its_input);
    RUN(test_sys_time_without_a_clock);
    RUN(test_time_convert_valid);
    RUN(test_time_convert_rejects_bad_input);
    RUN(test_time_convert_without_a_clock);
    RUN(test_binary_mode_rejects_out_of_range_registers);
    RUN(test_tiny_result_buffers);
    RUN(test_fuzz);

    return th_report("rtc");
}
