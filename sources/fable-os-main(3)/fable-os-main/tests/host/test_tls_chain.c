/* test_tls_chain.c — does the certificate verifier actually verify?
 *
 * WHY THIS FILE EXISTS, and why test_tls_verify.c is not enough. That suite is
 * 618k checks against net/tls_ca.c and drivers/rtc/rtc.c — the bundle's shape,
 * its fingerprints, and the calendar arithmetic — and it says so in its own
 * header: it does not link mbedTLS, so it "does not and cannot claim to prove
 * 'the verifier works'". It even transcribes a COPY of mbedTLS's date
 * comparison and drives the copy.
 *
 * The consequence was that the entire opt-in verified configuration had zero
 * automated coverage of any kind — not even a compile check. Nothing in the tree
 * built with -DFABLEOS_VERIFY_CERTS: no host test, no QEMU case, no target. You
 * could turn a path-validation check off and `make test` stayed green.
 *
 * This suite links the REAL vendored mbedTLS, with the kernel's own
 * include/mbedtls_config.h and FABLEOS_VERIFY_CERTS defined, against the REAL
 * net/tls_ca.c bundle and the REAL RTC driver, and asks mbedTLS for verdicts.
 * Fixtures are a small PKI generated once and embedded, so there is no network,
 * no openssl at test time, and no live certificate to expire.
 *
 * THE POINT of a pinned verifier is what it REFUSES, so most of these assert a
 * refusal. A refusal test is worthless without a control that passes through the
 * same code, so every rejection is paired with an acceptance that differs in
 * exactly one property.
 */

#include "harness.h"
#include "tls_ca.h"
#include "rtc.h"

#include "mbedtls/x509_crt.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_internal.h"

#include <string.h>

/* Generated once with openssl; embedded so the suite needs no tools and no
 * network. All four expire 2053-12-14, and the synthetic CMOS clock below is
 * parked inside that window, so these assertions do not rot. */

static const char TEST_ROOT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC/zCCAeegAwIBAgIJAKWB15gC89J+MA0GCSqGSIb3DQEBCwUAMBwxGjAYBgNV\n"
    "BAMMEXRhbGstb3MgdGVzdCByb290MCAXDTI2MDcyOTAyMzYyNFoYDzIwNTMxMjE0\n"
    "MDIzNjI0WjAcMRowGAYDVQQDDBF0YWxrLW9zIHRlc3Qgcm9vdDCCASIwDQYJKoZI\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBAMj1Aium6+qcJbkeG5vQX+7N1UJbO58YEnv0\n"
    "DYRNSGbqh7NEJURaK16KxNXx1VYsCNznpqo65NcyxgWcRTm6bpF539DSqJMpDyp/\n"
    "BWyFB1m3GuCXzUEQ51nNswNh6jMqD8/LUPrQS2RWEMho2xgmplyPx80iMYnAy3Po\n"
    "TLCtBLl+QGRTjwyM2THCWopLLfUrXsi9bdeKDIIYikAlbNHoUvK+Iz/ZANOKPao5\n"
    "VNtOK3NUNCTwTDXB5xvQgfXnGjRzvYA5pD2RRLtu5qOZsz/15KYbnweZ6BuSXv2K\n"
    "ZOS/nS7cA2DSDQvqkLr27MudTZXgSe+m01VS1TXSWn8t+BdTRvECAwEAAaNCMEAw\n"
    "DwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFOsvq61V\n"
    "R8DHnMxQwC0wyMKZjUHfMA0GCSqGSIb3DQEBCwUAA4IBAQA5VaLfGftwunSFwFnx\n"
    "0CtqJNHTFGPxErRiIuKVNkptY72zl8/w/NWTxZblrapvRHttsAUWDzD5N6ErTFFl\n"
    "djEyGI3G+7G/3qGO/MK7ZVKkMJiC33SV8FpdpEshy9oEp8Ze0xhC9KUbQ+U9nqRl\n"
    "eE4Clvp3wK0ByGAWuocMcJ/HeDFQawXl9Flec9Hr2CGFZH9F2oaQio8cLTDSvamu\n"
    "V5EA99SyleEiMp9mwJqR6860DnkDN3V4WhiquAcerIfbcjZr6U/wAakFN5o2sM9j\n"
    "9WWwxo1l0YcuKUZTwFrBD19J8T1cZetpMagbL6uLhzpS+fubxR9RdQZZR/ZJ9UfK\n"
    "v0qt\n"
    "-----END CERTIFICATE-----\n";

static const char TEST_LEAF_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDDzCCAfegAwIBAgIIUH0WXotdeUswDQYJKoZIhvcNAQELBQAwHDEaMBgGA1UE\n"
    "AwwRdGFsay1vcyB0ZXN0IHJvb3QwIBcNMjYwNzI5MDIzNjI0WhgPMjA1MzEyMTQw\n"
    "MjM2MjRaMBwxGjAYBgNVBAMMEWFwaS5hbnRocm9waWMuY29tMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAurnIsoxh2Etziy9Z9tOKR1mzT7gRWCYDGgQk\n"
    "gsbyZcL9tnrurCUJVJ0Ozd+VqbAuOLye6tZysAAggrXiyEVcZPUsQytKu2/obltA\n"
    "fxKYCqj+4LciimwGgfcr4+nots/ypqVO0Tci/1SX1gdpKvK50ML33koa+B7+XyXF\n"
    "wOVEavCcOtu9AOJSSby5tjRxytKcS23GKJ4Z3C0HcdvZK0A5vt6FYjhQpHuhs6MB\n"
    "dlRVp36Ix7FpF//60m8lsaYldZNVpPXFI4eqflzf29X7wmwH8lyEO6DtMiefiEBe\n"
    "Pm6nsP7hNZxUapgD29+tjiQLins7skHk+b284Xy4D5N54j9CIwIDAQABo1MwUTAM\n"
    "BgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQEAwIFoDATBgNVHSUEDDAKBggrBgEFBQcD\n"
    "ATAcBgNVHREEFTATghFhcGkuYW50aHJvcGljLmNvbTANBgkqhkiG9w0BAQsFAAOC\n"
    "AQEAKKUsC/MWjN9Fgjp3ZBerXTkFFnNV5sa2p+Ai1hR3T6Hnwd/Qqq/UCy8ofYCk\n"
    "GAag8xKVsxX49c/ST+LDaFg6WrqbOwX1u8cQJcKmLQVzSGfi57ZUsgEqgy5XIPPW\n"
    "l9vh2BanN1oLx891bbrEMmw8DP2ylO95/52PRxBr38BcoZ0cAvnf1M238u6vCEa8\n"
    "QkALQGiyb3ILLH0Rn394/enwzpLSluWYUVAIekOHZljMOfRMdhcovZCnaVx/mMaY\n"
    "UnkNQIfs+pqhBFwrtoq3ZYt0RRfP25Md1rb7Dql0Qr7j02+NxnZlHljpHCo36wHC\n"
    "l8S6MQyLxWr31IRKFEPGKopxIA==\n"
    "-----END CERTIFICATE-----\n";

static const char TEST_LEAF_CODESIGN_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDEzCCAfugAwIBAgIJAMtCyOpG8Lb+MA0GCSqGSIb3DQEBCwUAMBwxGjAYBgNV\n"
    "BAMMEXRhbGstb3MgdGVzdCByb290MCAXDTI2MDcyOTAyMzYyNFoYDzIwNTMxMjE0\n"
    "MDIzNjI0WjAcMRowGAYDVQQDDBFhcGkuYW50aHJvcGljLmNvbTCCASIwDQYJKoZI\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBALq5yLKMYdhLc4svWfbTikdZs0+4EVgmAxoE\n"
    "JILG8mXC/bZ67qwlCVSdDs3flamwLji8nurWcrAAIIK14shFXGT1LEMrSrtv6G5b\n"
    "QH8SmAqo/uC3IopsBoH3K+Pp6LbP8qalTtE3Iv9Ul9YHaSryudDC995KGvge/l8l\n"
    "xcDlRGrwnDrbvQDiUkm8ubY0ccrSnEttxiieGdwtB3Hb2StAOb7ehWI4UKR7obOj\n"
    "AXZUVad+iMexaRf/+tJvJbGmJXWTVaT1xSOHqn5c39vV+8JsB/JchDug7TInn4hA\n"
    "Xj5up7D+4TWcVGqYA9vfrY4kC4p7O7JB5Pm9vOF8uA+TeeI/QiMCAwEAAaNWMFQw\n"
    "DAYDVR0TAQH/BAIwADAOBgNVHQ8BAf8EBAMCBaAwFgYDVR0lAQH/BAwwCgYIKwYB\n"
    "BQUHAwMwHAYDVR0RBBUwE4IRYXBpLmFudGhyb3BpYy5jb20wDQYJKoZIhvcNAQEL\n"
    "BQADggEBAJ1yUYYdc1M4yOXr4Cq4ELF3VYGYsqR5G/Fu9XHbJx+ujPuzO3Wb8npk\n"
    "s4Znqxi2ODRzgKzAoRp7i3cFnKHgSq+jbeZ/1XCY5hvE+tuh1qTS70EfGjVeRKTo\n"
    "JRyu6OwH2WpAhI8Y3P8bTpMrk/yA5+LPsD+uwlYsbB/GskkBrfXPcID9xZFVFuLB\n"
    "f+1/UMJdNKhToLxXlrKiHwBybL7XnEWxdZYg9gb59bCPiheR8SeJGQOUvhYlLRPM\n"
    "awE/E51gVa1uow5JZ/RLpGuYI4361Unabxo8WNtJCm3uWqopgOTxWDDSwDDQAm0g\n"
    "kPUhRzhWd7LdbTKUD0bnoMStv74JScc=\n"
    "-----END CERTIFICATE-----\n";

static const char TEST_INT_NOKU_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC9zCCAd+gAwIBAgIJAO7eRzoR4AWiMA0GCSqGSIb3DQEBCwUAMBwxGjAYBgNV\n"
    "BAMMEXRhbGstb3MgdGVzdCByb290MCAXDTI2MDcyOTAyMzYyNFoYDzIwNTMxMjE0\n"
    "MDIzNjI0WjAzMTEwLwYDVQQDDCh0YWxrLW9zIHRlc3QgaW50ZXJtZWRpYXRlIG5v\n"
    "IGtleUNlcnRTaWduMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAx/El\n"
    "X9MJhWOVubC0LLdINCJIm8Nf4Zp/zXJUuFpIHt4foBbdGcbv4USCAsppCTFOPCjt\n"
    "/qwz1oS/q1uVPmqcryVeHkcHHY6Aq0eCxMb8TIjheXxM6Up7wv+sV2dOhSlYtm4E\n"
    "VdPyYVc+MTYY9Uc22e4bFqCW+AqhH/CJx7Fj0XM/zVUluLHciUjPb03HQUp64bfy\n"
    "4SRX5CXsAsZn8362WWXlVIkVPIgW7wdMv9qFwAzk1RPgGcLEKh6dWnA+36Plii4W\n"
    "5iCTjlLoAwCYUEVDLcXxK1+mQZqvSx5Dogv817ye8V3RBmBirVAX3B84JxSIfI1Z\n"
    "WD+Mx4v95cvvWzAK1QIDAQABoyMwITAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB\n"
    "/wQEAwIFoDANBgkqhkiG9w0BAQsFAAOCAQEAsyl/DlhxJDcEA9kkFShALZh8tK01\n"
    "Dyx3RyTgBW6thB8ODtwpq+21T09rY6o36JROnNWItMU0jmpXQt0RqyyZNkH6Qic1\n"
    "lSL6dpe8a5rycbb9Tt248kwLQpc/6iCIrMIZAokgdPvB3cWI5x27YgB+Fq3T1MzO\n"
    "k0zB8s0zYIcTQ8HJnTAAOPkOERy0asTdj4hKUAHDkVj1gY4YBsgqJwjIl2MCYZwI\n"
    "RUTG8M9YqsHtYpHNXQYmn64u97CHl2xAL26W7NGr3F03cFmURX04p7POYSR3dyb5\n"
    "23hr8uvRXT3xcro6QDiyveMPEi0dTkfaS460VTG+mw8YUS8euRoqz8WuIQ==\n"
    "-----END CERTIFICATE-----\n";

static const char TEST_LEAF_UNDER_NOKU_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDJzCCAg+gAwIBAgIJANaM66jHmYwbMA0GCSqGSIb3DQEBCwUAMDMxMTAvBgNV\n"
    "BAMMKHRhbGstb3MgdGVzdCBpbnRlcm1lZGlhdGUgbm8ga2V5Q2VydFNpZ24wIBcN\n"
    "MjYwNzI5MDIzNjI0WhgPMjA1MzEyMTQwMjM2MjRaMBwxGjAYBgNVBAMMEWFwaS5h\n"
    "bnRocm9waWMuY29tMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAurnI\n"
    "soxh2Etziy9Z9tOKR1mzT7gRWCYDGgQkgsbyZcL9tnrurCUJVJ0Ozd+VqbAuOLye\n"
    "6tZysAAggrXiyEVcZPUsQytKu2/obltAfxKYCqj+4LciimwGgfcr4+nots/ypqVO\n"
    "0Tci/1SX1gdpKvK50ML33koa+B7+XyXFwOVEavCcOtu9AOJSSby5tjRxytKcS23G\n"
    "KJ4Z3C0HcdvZK0A5vt6FYjhQpHuhs6MBdlRVp36Ix7FpF//60m8lsaYldZNVpPXF\n"
    "I4eqflzf29X7wmwH8lyEO6DtMiefiEBePm6nsP7hNZxUapgD29+tjiQLins7skHk\n"
    "+b284Xy4D5N54j9CIwIDAQABo1MwUTAMBgNVHRMBAf8EAjAAMA4GA1UdDwEB/wQE\n"
    "AwIFoDATBgNVHSUEDDAKBggrBgEFBQcDATAcBgNVHREEFTATghFhcGkuYW50aHJv\n"
    "cGljLmNvbTANBgkqhkiG9w0BAQsFAAOCAQEAeIpaj3sx7C0lzccla1GSOJk4hn9r\n"
    "ubCKfxmH4rq+fFFoXsCm++ABz523c0mf7RJvDCkYWIhYZgq2OUdRPOxOSRQ0j5W7\n"
    "Y4bNZ+CaExw6YO/RnifsVU0Nrx7LB1RQw9Ryme8rWTBWVeTka7pieiU0My+lIwkC\n"
    "RjpPx0/h/yc+Woo05Ctun2N0mmDzOoXwy/nRdT65In4LKiz+93RsXA05jqFYRxSj\n"
    "nqOg2UrtSfZwNUn8lLB8yBDmAepPTORXsN9mLJWjpdm8dITaaInryDqx04lFAN8X\n"
    "4CnprBc5+bUr8I31EEM+/5CJ3EiOY9t+w9Yuht13OYGj4CyYr4YpxW0a3w==\n"
    "-----END CERTIFICATE-----\n";

/* ---------------------------------------------------------------------- */
/* a synthetic MC146818, so "now" is a decision this file makes            */
/* ---------------------------------------------------------------------- */

static rtc_time_t g_now;
static int        g_clock_dead;

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static uint8_t fake_cmos(uint8_t reg) {
    if (g_clock_dead) return (reg == RTC_REG_STATUS_A) ? RTC_STATUS_A_UIP : 0xFF;
    switch (reg) {
        case RTC_REG_STATUS_A: return 0x00;              /* never updating */
        case RTC_REG_STATUS_B: return RTC_STATUS_B_24H;  /* BCD, 24-hour   */
        case RTC_REG_SECOND:   return bcd(g_now.second);
        case RTC_REG_MINUTE:   return bcd(g_now.minute);
        case RTC_REG_HOUR:     return bcd(g_now.hour);
        case RTC_REG_DAY:      return bcd(g_now.day);
        case RTC_REG_MONTH:    return bcd(g_now.month);
        case RTC_REG_YEAR:     return bcd(g_now.year % 100);
        case RTC_REG_CENTURY:  return bcd(g_now.year / 100);
        default:               return 0;
    }
}

static void set_now(int y, int mo, int d) {
    g_clock_dead = 0;
    g_now.year = y; g_now.month = mo; g_now.day = d;
    g_now.hour = 12; g_now.minute = 0; g_now.second = 0;
    rtc_set_backend(fake_cmos);
}

/* mbedTLS wants an entropy source because the kernel config selects
 * MBEDTLS_ENTROPY_HARDWARE_ALT. Nothing here needs randomness. */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen) {
    (void)data;
    for (size_t i = 0; i < len; i++) out[i] = (unsigned char)(i * 7 + 11);
    *olen = len;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* one verification, spelled out                                          */
/* ---------------------------------------------------------------------- */

/* Verify `chain` (leaf first, NUL-terminated PEM) for `host` against `trust`.
 * Returns the verify flags; *rc receives mbedtls_x509_crt_verify's return. */
static uint32_t verify(const char *chain, const char *trust,
                       const char *host, int *rc) {
    mbedtls_x509_crt c, t;
    uint32_t flags = 0;
    mbedtls_x509_crt_init(&c);
    mbedtls_x509_crt_init(&t);

    CHECK_EQ(mbedtls_x509_crt_parse(&c, (const unsigned char *)chain,
                                    strlen(chain) + 1), 0);
    CHECK_EQ(mbedtls_x509_crt_parse(&t, (const unsigned char *)trust,
                                    strlen(trust) + 1), 0);

    int r = mbedtls_x509_crt_verify(&c, &t, NULL, host, &flags, NULL, NULL);
    if (rc) *rc = r;

    mbedtls_x509_crt_free(&c);
    mbedtls_x509_crt_free(&t);
    return flags;
}

/* Two PEMs concatenated, for a leaf-then-intermediate chain. */
static const char *chain2(const char *a, const char *b) {
    static char buf[8192];
    size_t n = strlen(a);
    memcpy(buf, a, n);
    memcpy(buf + n, b, strlen(b) + 1);
    return buf;
}

/* ---------------------------------------------------------------------- */

/* Control. Without this, every rejection below could be a rejection of
 * something unrelated — a broken fixture, a dead clock, a config that refuses
 * everything. This is the same code path saying yes. */
static void test_a_correct_chain_is_accepted(void) {
    set_now(2030, 6, 1);
    int rc = -1;
    uint32_t f = verify(TEST_LEAF_PEM, TEST_ROOT_PEM, "api.anthropic.com", &rc);
    CHECK_EQ(rc, 0);
    CHECK_EQ((long long)f, 0);
}

/* THE pinning property: a technically perfect certificate for exactly the right
 * host, signed by a CA this kernel does not trust, must be refused. Note the
 * fixture leaf is named api.anthropic.com, so the ONLY thing wrong is the
 * anchor — which is what pinning is for. */
static void test_an_unpinned_root_is_refused(void) {
    set_now(2030, 6, 1);
    int rc = 0;
    uint32_t f = verify(TEST_LEAF_PEM, (const char *)tls_ca_bundle(),
                        "api.anthropic.com", &rc);
    CHECK(rc != 0);
    CHECK(f & MBEDTLS_X509_BADCERT_NOT_TRUSTED);
}

/* The real bundle must be exactly what net/net.c hands to mbedTLS, parsed by
 * mbedTLS itself rather than by a transcription of it — and it must yield every
 * root, not just the first. (Concatenated DER would silently parse one.) */
static void test_the_real_bundle_parses_to_every_root(void) {
    mbedtls_x509_crt ca;
    mbedtls_x509_crt_init(&ca);
    CHECK_EQ(mbedtls_x509_crt_parse(&ca, tls_ca_bundle(), tls_ca_bundle_len()), 0);
    int n = 0;
    for (mbedtls_x509_crt *p = &ca; p; p = p->next) n++;
    CHECK_EQ(n, tls_ca_root_count());
    CHECK(n >= 2);
    mbedtls_x509_crt_free(&ca);
}

/* Hostname matching is the half a CA bundle does not give you: a valid
 * certificate for some other name is still the wrong certificate. */
static void test_the_wrong_hostname_is_refused(void) {
    set_now(2030, 6, 1);
    int rc = 0;
    uint32_t f = verify(TEST_LEAF_PEM, TEST_ROOT_PEM, "not-the-api.invalid", &rc);
    CHECK(rc != 0);
    CHECK(f & MBEDTLS_X509_BADCERT_CN_MISMATCH);
}

/* Dates are checked against the CMOS clock, so moving the clock must move the
 * verdict — with the certificate, the anchor and the hostname all unchanged.
 * This is also the assertion that fails if MBEDTLS_HAVE_TIME_DATE ever goes
 * missing, which is the define that turns expiry checking from enforced into
 * merely parsed. */
static void test_the_clock_decides_validity(void) {
    /* inside the fixture window */
    set_now(2030, 6, 1);
    int rc = -1;
    CHECK_EQ((long long)verify(TEST_LEAF_PEM, TEST_ROOT_PEM,
                               "api.anthropic.com", &rc), 0);
    CHECK_EQ(rc, 0);

    /* after it */
    set_now(2060, 1, 1);
    rc = 0;
    uint32_t f = verify(TEST_LEAF_PEM, TEST_ROOT_PEM, "api.anthropic.com", &rc);
    CHECK(rc != 0);
    CHECK(f & MBEDTLS_X509_BADCERT_EXPIRED);

    /* before it */
    set_now(2000, 1, 1);
    rc = 0;
    f = verify(TEST_LEAF_PEM, TEST_ROOT_PEM, "api.anthropic.com", &rc);
    CHECK(rc != 0);
    CHECK(f & MBEDTLS_X509_BADCERT_FUTURE);
}

/* An unreadable chip must fail CLOSED. tls_ca_now_unix() returns the epoch, so
 * every certificate looks not-yet-valid and nothing is accepted. */
static void test_a_dead_clock_fails_closed(void) {
    set_now(2030, 6, 1);
    g_clock_dead = 1;
    CHECK_EQ((long long)tls_ca_now_unix(), (long long)TLS_CA_NO_CLOCK);

    int rc = 0;
    uint32_t f = verify(TEST_LEAF_PEM, TEST_ROOT_PEM, "api.anthropic.com", &rc);
    CHECK(rc != 0);
    CHECK(f & MBEDTLS_X509_BADCERT_FUTURE);
    g_clock_dead = 0;
}

/* MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE. Both purpose checks are ON by default
 * in mbedTLS's own config.h, each with a "disabling this can be a security
 * risk" warning, and include/mbedtls_config.h is a complete REPLACEMENT config
 * — so they were absent. This certificate is for the right host, from the right
 * root, in date; its EKU says codeSigning. It is not a TLS server certificate.
 *
 * mbedtls_x509_crt_verify() does NOT check EKU in any configuration; the check
 * lives in mbedtls_ssl_check_cert_usage(), which ssl_parse_certificate_verify()
 * calls on every handshake. So call what the handshake calls. */
static void test_extended_key_usage_is_enforced(void) {
    set_now(2030, 6, 1);
    const mbedtls_ssl_ciphersuite_t *cs =
        mbedtls_ssl_ciphersuite_from_id(MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256);
    CHECK(cs != NULL);

    struct { const char *pem; int ok; } cases[] = {
        { TEST_LEAF_PEM,          1 },   /* EKU serverAuth   */
        { TEST_LEAF_CODESIGN_PEM, 0 },   /* EKU codeSigning  */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mbedtls_x509_crt leaf;
        mbedtls_x509_crt_init(&leaf);
        CHECK_EQ(mbedtls_x509_crt_parse(&leaf, (const unsigned char *)cases[i].pem,
                                        strlen(cases[i].pem) + 1), 0);
        uint32_t uflags = 0;
        int r = mbedtls_ssl_check_cert_usage(&leaf, cs, MBEDTLS_SSL_IS_SERVER,
                                             &uflags);
        if (cases[i].ok) {
            CHECK_EQ(r, 0);
            CHECK_EQ((long long)uflags, 0);
        } else {
            CHECK(r != 0);
            CHECK(uflags & MBEDTLS_X509_BADCERT_EXT_KEY_USAGE);
        }
        mbedtls_x509_crt_free(&leaf);
    }
}

/* MBEDTLS_X509_CHECK_KEY_USAGE. An intermediate with CA:TRUE whose keyUsage
 * omits keyCertSign must not be accepted as an issuer: its own CA put that
 * restriction there to say "this key must not sign certificates". Without the
 * define, path building fell back to basicConstraints CA:TRUE alone. */
static void test_key_usage_is_enforced_on_issuers(void) {
    set_now(2030, 6, 1);
    int rc = 0;
    uint32_t f = verify(chain2(TEST_LEAF_UNDER_NOKU_PEM, TEST_INT_NOKU_PEM),
                        TEST_ROOT_PEM, "api.anthropic.com", &rc);
    CHECK(rc != 0);
    CHECK(f & MBEDTLS_X509_BADCERT_NOT_TRUSTED);
}

/* The default profile must still be doing its own job. If this ever passes, the
 * config has been loosened somewhere. */
static void test_weak_crypto_is_still_refused(void) {
    set_now(2030, 6, 1);
    /* An SHA-1-signed or short-key certificate would raise BAD_MD/BAD_KEY; we
     * assert the profile is the default one rather than shipping more fixtures. */
    CHECK(mbedtls_x509_crt_profile_default.allowed_mds != 0);
    CHECK_EQ((long long)(mbedtls_x509_crt_profile_default.allowed_mds &
                         MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA1)), 0);
    CHECK(mbedtls_x509_crt_profile_default.rsa_min_bitlen >= 2048);
}

int main(void) {
    RUN(test_a_correct_chain_is_accepted);
    RUN(test_an_unpinned_root_is_refused);
    RUN(test_the_real_bundle_parses_to_every_root);
    RUN(test_the_wrong_hostname_is_refused);
    RUN(test_the_clock_decides_validity);
    RUN(test_a_dead_clock_fails_closed);
    RUN(test_extended_key_usage_is_enforced);
    RUN(test_key_usage_is_enforced_on_issuers);
    RUN(test_weak_crypto_is_still_refused);
    return th_report("tls_chain");
}
