/* tls_ca.c — trusted roots for api.anthropic.com, and the wall clock mbedTLS
 * checks certificate validity against.
 *
 * Read include/tls_ca.h first: it documents which roots are here, how they were
 * chosen, where the bytes came from, and the operational risk of pinning them.
 * This file is the data plus about eighty lines of glue.
 *
 * WHY THE BUNDLE IS PEM AND NOT DER
 *   DER would be smaller and would need neither mbedtls/pem.c nor base64.c. It
 *   was rejected for two reasons. First, mbedtls_x509_crt_parse() only walks a
 *   *multi*-certificate blob on its PEM path; handed concatenated DER it parses
 *   the first certificate, ignores everything after it, and returns success —
 *   a two-root bundle would silently become a one-root bundle. Second, a wall
 *   of hex is unauditable. The text below can be pasted straight into
 *   `openssl x509 -noout -text` by anyone who wants to check what this kernel
 *   trusts, and diffed against a published root. For a trust store that is
 *   worth two kilobytes of base64 and one extra mbedTLS module.
 *
 * WHY THE CLOCK LIVES HERE AND NOT IN port/time.h
 *   port/time.h's time() returns seconds since boot and says so in its own
 *   comment; changing it would change the meaning of every mbedTLS timestamp in
 *   the default build, which is not this change's to do. Instead mbedTLS is
 *   pointed at fableos_tls_time() through MBEDTLS_PLATFORM_TIME_MACRO, under the
 *   flag only. The default build's time() is untouched and still returns uptime.
 */

#include "tls_ca.h"
#include "rtc.h"

#ifdef FABLEOS_VERIFY_CERTS
/* Only the two adapter functions at the bottom need mbedTLS's declarations.
 * Keeping the include inside the flag is what lets tests/host/test_tls_verify.c
 * compile this exact file natively, with no -Imbedtls/include and no config. */
#include "mbedtls/platform_util.h"
#endif

/* ====================================================================== */
/* the bundle                                                             */
/* ====================================================================== */

/* GTS Root R4 first: it is the root actually on api.anthropic.com's path
 * today, and mbedTLS searches the trusted list in order.
 *
 *   GTS Root R4  ECDSA P-384  serial 0203E5C068EF631A9C72905052
 *   SHA-256 349DFA4058C5E263123B398AE795573C4E1313C83FE68F93556CD5E8031B3C7D
 *   notBefore 2016-06-22T00:00:00Z  notAfter 2036-06-22T00:00:00Z
 *
 *   GTS Root R1  RSA-4096     serial 0203E5936F31B01349886BA217
 *   SHA-256 D947432ABDE7B7FA90FC2E6B59101B1280E0E1C7E4E40FA3C6887FFF57A7F4CF
 *   notBefore 2016-06-22T00:00:00Z  notAfter 2036-06-22T00:00:00Z
 *
 * Both from the Mozilla/CCADB store, cross-checked against macOS's system
 * keychain. Do not reformat: mbedTLS's PEM reader wants the armour lines exact,
 * and the fingerprints asserted in tests/host/test_tls_verify.c are of these
 * bytes. */
static const char ca_bundle_pem[] =
    /* ---- GTS Root R4 ---- */
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n"
    /* ---- GTS Root R1 ---- */
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n";

/* Order matches the PEM above. `why` is printed at boot so an operator sees the
 * trust decision, not just that one was made. */
static const tls_ca_root_t ca_roots[] = {
    { "GTS Root R4", "ECDSA P-384",
      "349DFA4058C5E263123B398AE795573C4E1313C83FE68F93556CD5E8031B3C7D",
      "2036-06-22",
      "anchors api.anthropic.com today (leaf -> WE1 -> GTS Root R4)" },
    { "GTS Root R1", "RSA-4096",
      "D947432ABDE7B7FA90FC2E6B59101B1280E0E1C7E4E40FA3C6887FFF57A7F4CF",
      "2036-06-22",
      "the RSA sibling hierarchy; the CA may move a leaf there without notice" },
};

#define CA_ROOT_COUNT ((int)(sizeof ca_roots / sizeof ca_roots[0]))

const char *tls_ca_bundle(void)    { return ca_bundle_pem; }

/* sizeof, not strlen: the terminating NUL must be inside the length mbedTLS is
 * given or mbedtls_x509_crt_parse() will not recognise the blob as PEM. */
size_t tls_ca_bundle_len(void)     { return sizeof ca_bundle_pem; }

int tls_ca_root_count(void)        { return CA_ROOT_COUNT; }

const tls_ca_root_t *tls_ca_root(int index)
{
    if (index < 0 || index >= CA_ROOT_COUNT) return (const tls_ca_root_t *)0;
    return &ca_roots[index];
}

/* ====================================================================== */
/* structural PEM check                                                   */
/* ====================================================================== */

static const char PEM_BEGIN[] = "-----BEGIN CERTIFICATE-----";
static const char PEM_END[]   = "-----END CERTIFICATE-----";

/* Offset of `needle` in [p, p+n) at or after `from`, or -1. Length-bounded
 * rather than strstr-based: the input may be a caller's buffer with embedded
 * NULs, and this function is reachable from test inputs. */
static long find_from(const char *p, size_t n, size_t from,
                      const char *needle, size_t nlen)
{
    if (nlen == 0 || nlen > n) return -1;
    for (size_t i = from; i + nlen <= n; i++) {
        size_t k = 0;
        while (k < nlen && p[i + k] == needle[k]) k++;
        if (k == nlen) return (long)i;
    }
    return -1;
}

int tls_ca_pem_count(const char *pem, size_t len)
{
    if (!pem) return TLS_CA_EMALFORMED;

    const size_t blen = sizeof PEM_BEGIN - 1;
    const size_t elen = sizeof PEM_END - 1;

    int    count = 0;
    size_t pos   = 0;

    for (;;) {
        long b = find_from(pem, len, pos, PEM_BEGIN, blen);
        long e = find_from(pem, len, pos, PEM_END,   elen);

        if (b < 0) {
            /* No more openings. A dangling END is malformed, not "done". */
            return e < 0 ? count : TLS_CA_EMALFORMED;
        }
        /* An END before the next BEGIN means the blocks are interleaved or the
         * previous block was never opened. Either way, refuse to guess. */
        if (e < 0 || e < b) return TLS_CA_EMALFORMED;

        /* And a second BEGIN before this block's END means this block was never
         * terminated — the other half of the header's contract. Looking for the
         * opener again from just past this one and finding it before `e` is
         * exactly that case; without this, "BEGIN a BEGIN b END" counted as one
         * well-formed block because both searches started from the same cursor. */
        long b2 = find_from(pem, len, (size_t)b + blen, PEM_BEGIN, blen);
        if (b2 >= 0 && b2 < e) return TLS_CA_EMALFORMED;

        count++;
        pos = (size_t)e + elen;
    }
}

int tls_ca_bundle_selftest(void)
{
    size_t len = tls_ca_bundle_len();

    /* mbedTLS's PEM detection is literally `buf[buflen - 1] == '\0'`. If that
     * ever stops holding, the bundle is parsed as DER and the kernel comes up
     * trusting nothing, so check it explicitly instead of trusting sizeof. */
    if (len == 0 || ca_bundle_pem[len - 1] != '\0') return TLS_CA_ETERM;

    int n = tls_ca_pem_count(ca_bundle_pem, len - 1);
    if (n < 0)                  return TLS_CA_EMALFORMED;
    if (n != CA_ROOT_COUNT)     return TLS_CA_ECOUNT;
    return TLS_CA_OK;
}

/* ====================================================================== */
/* the clock                                                              */
/* ====================================================================== */

int64_t tls_ca_now_unix(void)
{
    int64_t now = rtc_unix_now();
    /* rtc_unix_now() returns a negative errno-style code when the chip cannot
     * be read. Collapse every failure to the epoch: see the FAIL CLOSED note in
     * tls_ca.h. A negative value handed to mbedTLS would be worse than useless,
     * because gmtime of a negative time is a pre-1970 date and some
     * certificates would then look valid. */
    return now < 0 ? TLS_CA_NO_CLOCK : now;
}

struct tm *tls_ca_gmtime_r(int64_t ts, struct tm *out)
{
    if (!out) return (struct tm *)0;

    rtc_time_t t;
    if (rtc_from_unix(ts, &t) != RTC_OK) return (struct tm *)0;

    int wday = rtc_weekday(&t);
    if (wday < 0) return (struct tm *)0;

    /* Day of year, 0-based, the way struct tm wants it. */
    int yday = t.day - 1;
    for (int m = 1; m < t.month; m++) yday += rtc_days_in_month(t.year, m);

    out->tm_sec   = t.second;
    out->tm_min   = t.minute;
    out->tm_hour  = t.hour;
    out->tm_mday  = t.day;
    out->tm_mon   = t.month - 1;      /* struct tm counts months from 0 ... */
    out->tm_year  = t.year - 1900;    /* ... and years from 1900.           */
    out->tm_wday  = wday;
    out->tm_yday  = yday;
    out->tm_isdst = 0;                /* UTC has no daylight saving, ever. */
    return out;
}

/* ====================================================================== */
/* mbedTLS adapters — flag-gated, called only by mbedTLS                  */
/* ====================================================================== */

#ifdef FABLEOS_VERIFY_CERTS

/* MBEDTLS_PLATFORM_TIME_MACRO. Signature must match mbedtls_time_t (*)(
 * mbedtls_time_t *), and mbedtls_config.h pins mbedtls_time_t to `long`. */
long fableos_tls_time(long *tloc)
{
    long now = (long)tls_ca_now_unix();
    if (tloc) *tloc = now;
    return now;
}

/* MBEDTLS_PLATFORM_GMTIME_R_ALT. mbedTLS's own implementation calls gmtime_r,
 * which does not exist here; this replaces it wholesale. x509.c's
 * x509_get_current_time() reads tm_year/tm_mon/tm_mday/tm_hour/tm_min/tm_sec
 * out of the result and returns -1 if we hand back NULL, which makes
 * mbedtls_x509_time_is_past()/is_future() both answer "yes" and the
 * certificate is rejected as expired *and* not-yet-valid. Failing closed. */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    if (!tt) return (struct tm *)0;
    return tls_ca_gmtime_r((int64_t)*tt, tm_buf);
}

#endif /* FABLEOS_VERIFY_CERTS */
