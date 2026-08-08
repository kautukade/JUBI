/* rtc.h — MC146818 CMOS real-time clock: this machine's only wall clock.
 *
 * PURPOSE
 *   Until now the kernel could say how long it had been awake (millis(), a TSC
 *   calibrated against the PIT at boot) but had no idea what *day* it was. That
 *   is not a cosmetic gap. It is the reason TLS certificate validation is
 *   currently switched off: notBefore/notAfter cannot be checked against a
 *   clock that starts at zero every boot. This driver supplies the missing
 *   number — seconds since the Unix epoch — from the one piece of hardware on
 *   the board that keeps counting when the power is off.
 *
 * WHAT THE HARDWARE ACTUALLY DOES
 *   The MC146818 (and every PC chipset's emulation of it, QEMU included) is a
 *   64-byte battery-backed CMOS array behind two ports: write a register index
 *   to 0x70, read/write that register at 0x71. The calendar lives in the first
 *   ten registers. Three things about it bite, and this driver handles all
 *   three because getting any of them wrong yields a plausible-looking wrong
 *   answer rather than an obvious failure:
 *
 *     1. UPDATE IN PROGRESS. Once a second the chip spends up to ~2 ms rolling
 *        the calendar forward, and during that window the registers are
 *        explicitly undefined. Status register A bit 7 (UIP) is set for the
 *        duration. Reading through it returns garbage that looks like a time.
 *
 *     2. TEARING. Even outside the UIP window a read can straddle a rollover:
 *        sample 23:59:59 on 31 December, the chip updates, then sample the
 *        year — and you get 1 January of the year that just ended. So the
 *        registers are sampled twice and the two snapshots must agree byte for
 *        byte before the reading is believed.
 *
 *     3. ENCODING. Status register B says how the values are encoded, and both
 *        settings occur in the wild: bit 2 clear means BCD (0x59 == 59), bit 1
 *        clear means 12-hour with bit 7 of the hour register as the PM flag.
 *        A BCD 12-hour chip read as binary 24-hour is wrong in two directions
 *        at once. The PM bit is stripped BEFORE the BCD conversion, because in
 *        12-hour BCD mode it is set on top of the BCD digits.
 *
 * TIME ZONE
 *   The CMOS clock is assumed to be UTC. That is QEMU's default (`-rtc
 *   base=utc`, the implicit setting) and it is what every sane machine does.
 *   There is no time-zone database here and no DST arithmetic: every timestamp
 *   and every formatted string this module produces is UTC, and says so. If a
 *   host is configured with `-rtc base=localtime` the readings will be off by
 *   that host's UTC offset, and nothing here can detect it — a clock cannot
 *   tell you which zone it is in.
 *
 * THE CALENDAR ARITHMETIC
 *   rtc_to_unix() is proleptic Gregorian and exact over [1970, 2999]: leap
 *   years are the full rule (divisible by 4, except centuries, except
 *   multiples of 400), so 2000 has a 29 February and 2100 does not. This feeds
 *   certificate validity, where an off-by-one-day is a security bug and not a
 *   display glitch, so the arithmetic is table-free where it can be and
 *   exhaustively tested where it cannot (tests/host/test_rtc.c round-trips
 *   every single day from 1970-01-01 to 2999-12-31).
 *
 * PUBLIC API
 *   Hardware:
 *     rtc_read            sample the CMOS now, tear-free, decoded to UTC.
 *     rtc_unix_now        the same reading as seconds since the epoch.
 *     rtc_unix_at_boot    the timestamp captured once during driver init.
 *   Pure calendar arithmetic (no hardware, no allocation, host-tested):
 *     rtc_is_leap_year / rtc_days_in_month / rtc_time_valid
 *     rtc_to_unix / rtc_from_unix / rtc_weekday
 *     rtc_weekday_name / rtc_month_name / rtc_format_iso8601
 *   Test seam:
 *     rtc_set_backend     replace the port-I/O register read.
 *
 * THE TEST SEAM
 *   Port I/O cannot run in a host process (and does not even assemble on the
 *   arm64 Macs this is developed on), so every CMOS access goes through one
 *   function pointer. rtc_set_backend() swaps in a synthetic CMOS: that is how
 *   the BCD/binary decode, the 12/24-hour decode, the UIP wait and the tear
 *   retry are tested without a machine. The kernel never calls it; the default
 *   backend is the real outb/inb pair.
 *
 * DEPENDENCIES
 *   The calendar half depends on nothing but <stdint.h>/<stddef.h> — the same
 *   object file compiles freestanding into the kernel and natively for the host
 *   tests. The hardware half adds io.h; the driver half adds driver.h/device.h
 *   and kernel.h, and is compiled out of host builds entirely.
 *
 * FUTURE EXTENSION POINTS
 *   - port/time.h's time() is a stub returning seconds since boot, and mbedTLS
 *     is built with MBEDTLS_HAVE_TIME off. rtc_unix_now() is exactly the value
 *     that stub should return; wiring it up plus MBEDTLS_HAVE_TIME_DATE is what
 *     turns certificate date validation back on.
 *   - The chip can raise IRQ 8 (periodic, alarm, update-ended). Nothing is
 *     enabled here — every PIC line is masked and the kernel is polled — but
 *     the alarm registers (0x01/0x03/0x05) and status register C (0x0C) are the
 *     hook for a real timekeeping tick once interrupts are live. Neither is
 *     named above: this header only exposes the registers the driver reads.
 *   - Writing the CMOS (setting the clock) is deliberately absent: see the
 *     DECISION note in drivers/rtc/rtc.c.
 */
#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stddef.h>

/* ---- error codes (errno-style negatives, shared space with vfs.h/tool.h) ---- */
#define RTC_OK          0
#define RTC_EIO        -5    /* the chip never produced a stable reading      */
#define RTC_ENODEV    -19    /* no RTC backend (host build without injection) */
#define RTC_EINVAL    -22    /* bad argument, or an impossible calendar value */

/* Range rtc_to_unix()/rtc_from_unix() accept. The floor is the epoch, so a
 * valid timestamp is never negative and a negative return is unambiguously an
 * error. The ceiling keeps every intermediate product far inside int64. */
#define RTC_YEAR_MIN 1970
#define RTC_YEAR_MAX 2999

/* "YYYY-MM-DDTHH:MM:SSZ" plus the NUL. */
#define RTC_ISO8601_MAX 21

/* ---- CMOS register indices (written to port 0x70) ----
 * Exposed so tests can build synthetic CMOS contents against the same names
 * the driver reads. 0x32 is the century register the ACPI FADT points at; it
 * is what QEMU implements, and the driver treats it as advisory (see rtc.c). */
#define RTC_REG_SECOND    0x00
#define RTC_REG_MINUTE    0x02
#define RTC_REG_HOUR      0x04
#define RTC_REG_WEEKDAY   0x06
#define RTC_REG_DAY       0x07
#define RTC_REG_MONTH     0x08
#define RTC_REG_YEAR      0x09
#define RTC_REG_STATUS_A  0x0A
#define RTC_REG_STATUS_B  0x0B
#define RTC_REG_CENTURY   0x32

#define RTC_STATUS_A_UIP     0x80   /* update in progress: registers undefined */
#define RTC_STATUS_B_24H     0x02   /* set = 24-hour, clear = 12-hour + PM bit */
#define RTC_STATUS_B_BINARY  0x04   /* set = binary,  clear = BCD              */
#define RTC_HOUR_PM          0x80   /* in 12-hour mode, bit 7 of the hour reg  */

/* A decoded UTC calendar reading. Always 24-hour, always four-digit year: a
 * 12-hour chip is normalised on the way out so no caller has to care. */
typedef struct rtc_time {
    int year;     /* full year, e.g. 2026 */
    int month;    /* 1..12  */
    int day;      /* 1..31  */
    int hour;     /* 0..23  */
    int minute;   /* 0..59  */
    int second;   /* 0..59  */
} rtc_time_t;

/* ====================================================================== */
/* hardware                                                               */
/* ====================================================================== */

/* Sample the CMOS now and decode it. Waits out any update in progress, then
 * reads the register file twice and requires the two snapshots to be identical,
 * so a rollover cannot be observed half-applied. `out` is untouched on failure.
 * Returns:
 *   RTC_OK      a decoded, plausible UTC reading.
 *   RTC_ENODEV  the chip never left its update window — it is not ticking, or
 *               there is no chip (every register reads back 0xFF, so UIP looks
 *               permanently set).
 *   RTC_EIO     it went quiet but never produced two agreeing sweeps.
 *   RTC_EINVAL  it produced something that is not a possible date, or `out` is
 *               NULL. */
int rtc_read(rtc_time_t *out);

/* rtc_read() rendered as seconds since 1970-01-01T00:00:00Z, or the negative
 * error code from rtc_read()/rtc_to_unix(). Never returns a stale value. */
int64_t rtc_unix_now(void);

/* The timestamp taken once during driver init, or a negative error if that read
 * failed. Cheap, constant, and the anchor for "how long has this been up in
 * wall-clock terms" without touching the hardware again. */
int64_t rtc_unix_at_boot(void);

/* ====================================================================== */
/* pure calendar arithmetic — no hardware, no allocation, no I/O          */
/* ====================================================================== */

int rtc_is_leap_year(int year);

/* 28/29/30/31, or 0 if `month` is outside 1..12. */
int rtc_days_in_month(int year, int month);

/* 1 if every field is in range AND `day` exists in that month of that year
 * (so 2100-02-29 is rejected while 2000-02-29 is accepted). */
int rtc_time_valid(const rtc_time_t *t);

/* Seconds since the epoch, or RTC_EINVAL (negative) for a time that fails
 * rtc_time_valid(). Exact over [RTC_YEAR_MIN, RTC_YEAR_MAX]; leap seconds do
 * not exist in Unix time and are not modelled. */
int64_t rtc_to_unix(const rtc_time_t *t);

/* The exact inverse over the same range. RTC_OK, or RTC_EINVAL if `ts` is
 * negative or lands past RTC_YEAR_MAX. */
int rtc_from_unix(int64_t ts, rtc_time_t *out);

/* 0 = Sunday .. 6 = Saturday, or RTC_EINVAL for an invalid time. Computed from
 * the date, never read from CMOS register 6 — firmware routinely leaves that
 * register wrong, and it is derivable, so trusting it would be a bug source
 * with no upside. */
int rtc_weekday(const rtc_time_t *t);

/* "Sunday".."Saturday" / "January".."December". Never NULL: an out-of-range
 * argument yields "?" so a caller can print the result unconditionally. */
const char *rtc_weekday_name(int weekday);
const char *rtc_month_name(int month);

/* Write "YYYY-MM-DDTHH:MM:SSZ" (RFC 3339, UTC) into `dst`. Returns the number
 * of bytes written excluding the NUL, or 0 if `t` is invalid or `cap` is under
 * RTC_ISO8601_MAX — in which case `dst` is set to the empty string when it can
 * hold one. Years are clamped to four digits by the valid-range check. */
size_t rtc_format_iso8601(const rtc_time_t *t, char *dst, size_t cap);

/* ====================================================================== */
/* test seam                                                              */
/* ====================================================================== */

/* Read one CMOS register. The kernel's default implementation is the real
 * outb(0x70)/inb(0x71) pair; on the host there is no default and every read
 * reports "no hardware" until a backend is installed. */
typedef uint8_t (*rtc_cmos_read_fn)(uint8_t reg);

/* Install a register-read backend, or restore the default with NULL. Exists so
 * tests/host/test_rtc.c can feed synthetic CMOS contents — including contents
 * that tear under the reader's feet — to code that is otherwise unreachable
 * without a machine. */
void rtc_set_backend(rtc_cmos_read_fn fn);

#endif /* RTC_H */
