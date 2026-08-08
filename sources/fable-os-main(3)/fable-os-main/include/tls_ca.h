/* tls_ca.h — the kernel's TLS trust anchors, and the clock they are checked
 * against.
 *
 * PURPOSE
 *   Until this file existed the kernel completed a real TLS handshake with
 *   api.anthropic.com and then believed whatever certificate came back. The
 *   traffic was encrypted; it was not authenticated. Anything that could answer
 *   on 10.0.2.2's route to 443 — QEMU's user-mode stack, a proxy, a hostile
 *   router — could present a self-signed certificate for any name and the
 *   kernel would hand it the conversation, and (with a key compiled in) the
 *   API key. This module supplies the two things a verifier needs and the
 *   kernel did not have: a set of trusted roots, and a wall clock.
 *
 *   It is deliberately inert unless the kernel is built with
 *   -DFABLEOS_VERIFY_CERTS. See "THE BUILD FLAG" below.
 *
 * WHAT IS TRUSTED, AND HOW THAT WAS DETERMINED
 *   The bundle holds exactly two roots, both operated by Google Trust
 *   Services. They were chosen by looking at the chain api.anthropic.com
 *   actually serves, not by copying a generic 150-root bundle:
 *
 *     $ openssl s_client -connect api.anthropic.com:443 \
 *           -servername api.anthropic.com -showcerts </dev/null
 *       0 s:/CN=api.anthropic.com
 *       1 s:/C=US/O=Google Trust Services/CN=WE1
 *       2 s:/C=US/O=Google Trust Services LLC/CN=GTS Root R4
 *
 *   The served chain terminates in the *cross-signed* GTS Root R4 (issued by
 *   GlobalSign Root CA), which is what browsers with older trust stores need.
 *   We do not need it: we trust the self-signed GTS Root R4 directly, so the
 *   chain is anchored one link earlier and the cross-signature — and GlobalSign
 *   — never enter the trust decision at all. Verified with the real chain:
 *
 *     $ openssl verify -CAfile <these two roots> -untrusted WE1.pem leaf.pem
 *       leaf.pem: OK
 *
 *     TLS_CA_ROOT_GTS_R4  "GTS Root R4"  ECDSA P-384, 2016-06-22..2036-06-22
 *       SHA-256 349DFA4058C5E263123B398AE795573C4E1313C83FE68F93556CD5E8031B3C7D
 *       The root that anchors the chain in use today (leaf -> WE1 -> R4).
 *
 *     TLS_CA_ROOT_GTS_R1  "GTS Root R1"  RSA-4096,    2016-06-22..2036-06-22
 *       SHA-256 D947432ABDE7B7FA90FC2E6B59101B1280E0E1C7E4E40FA3C6887FFF57A7F4CF
 *       Not on the path today. Included because Google Trust Services issues
 *       from two parallel hierarchies — the EC one under R4 (intermediates
 *       WE1/WE2) and the RSA one under R1 (intermediates WR1..WR4) — and which
 *       one a given leaf comes from is the CA's choice, changeable without
 *       notice. Trusting the sibling root of the same operator costs one extra
 *       anchor and removes a whole class of "it broke on Tuesday".
 *
 *   PROVENANCE. Both PEM blocks were taken from the Mozilla/CCADB root store as
 *   shipped by Homebrew's ca-certificates
 *   (/opt/homebrew/etc/ca-certificates/cert.pem) and cross-checked, byte for
 *   byte, against macOS's own SystemRootCertificates.keychain
 *   (`security find-certificate -a -c "GTS Root R4" -p ...`). Two independent
 *   stores, identical SHA-256 fingerprints — recorded above and asserted by
 *   tests/host/test_tls_verify.c against the embedded bytes, so a future edit
 *   to the bundle that changes a certificate fails the test suite.
 *
 *   A NOTE ON THE OTHER GTS ROOTS YOU WILL FIND. There are two self-signed
 *   certificates in circulation for each of R1 and R4: the original 2016 issue
 *   and a 2020 re-issue with a different serial number. They carry the same
 *   subject and, crucially, the same public key, so either works as an anchor.
 *   macOS ships the 2016 serials, Mozilla ships the 2020 serials, and this
 *   bundle carries Mozilla's. The fingerprints differ; the SubjectPublicKeyInfo
 *   hashes are identical (2e84b5af... for R1, eca50d5a... for R4), which is why
 *   the cross-check above was done on the key and not only on the file.
 *
 * THE OPERATIONAL RISK, STATED HONESTLY
 *   A pinned trust set is strictly safer against interception and strictly more
 *   fragile against time. Three ways this kernel loses the ability to talk to
 *   the API, none of which is a bug:
 *
 *     1. Anthropic moves api.anthropic.com to a different CA. The handshake
 *        starts failing the same hour, with BADCERT_NOT_TRUSTED, and stays
 *        failing until somebody rebuilds with a new root. A browser would not
 *        notice; this kernel is bricked for that endpoint.
 *     2. Google rotates the roots. R1 and R4 expire 2036-06-22. Real roots are
 *        usually replaced years before expiry, and the replacement will not be
 *        in this file.
 *     3. A root is distrusted after an incident. Then this file is trusting
 *        something the rest of the internet no longer does, and nothing here
 *        will tell you — there is no CRL, no OCSP, and no revocation checking
 *        of any kind in this kernel (MBEDTLS_X509_CRL_PARSE_C is off).
 *
 *   The mitigation is not technical. It is that this file names its roots, says
 *   where they came from, and prints them at boot, so the failure is legible in
 *   ten seconds instead of being a mystery. The alternative — trusting whatever
 *   shows up — has no failure mode at all, which is exactly the problem.
 *
 * THE BUILD FLAG
 *   Everything here compiles into every build; nothing here is *used* unless
 *   FABLEOS_VERIFY_CERTS is defined. Without the flag the kernel behaves exactly
 *   as it always has (ALTCP_MBEDTLS_AUTHMODE 0, no CA chain installed, no date
 *   checking), because other work in this tree boots against that behaviour and
 *   a pinned root that stops working is a very effective way to block three
 *   people at once. With the flag:
 *
 *     lwipopts.h            ALTCP_MBEDTLS_AUTHMODE -> 2 (VERIFY_REQUIRED)
 *     net/net.c             passes tls_ca_bundle() to altcp_tls_create_config_client
 *     net/net.c             mbedtls_ssl_set_hostname() becomes the verified name
 *     mbedtls_config.h      MBEDTLS_HAVE_TIME_DATE, so notBefore/notAfter run
 *     mbedtls_config.h      mbedtls_time -> fableos_tls_time (below)
 *     mbedtls_config.h      MBEDTLS_PLATFORM_GMTIME_R_ALT, so the epoch->calendar
 *                           conversion mbedTLS needs is ours (below)
 *     mbedtls_config.h      MBEDTLS_PEM_PARSE_C/BASE64_C, so the bundle parses
 *
 * THE CLOCK
 *   mbedTLS asks the platform two questions when it checks a validity period:
 *   "what time is it" (mbedtls_time) and "what calendar date is that"
 *   (mbedtls_platform_gmtime_r). The stock answer to the second is gmtime_r,
 *   which does not exist in a freestanding kernel; the stock answer to the
 *   first, in this tree, was seconds since boot, which would place every
 *   handshake in January 1970. Both are answered here, from the CMOS RTC
 *   (drivers/rtc/rtc.c).
 *
 *   FAIL CLOSED. If the RTC cannot be read, tls_ca_now_unix() returns 0 — the
 *   epoch — rather than guessing. Every real certificate has a notBefore after
 *   1970, so mbedTLS marks it MBEDTLS_X509_BADCERT_FUTURE and refuses the
 *   handshake. A verifier with no clock must reject, not shrug; the one thing
 *   it must never do is skip the date check because it is inconvenient.
 *
 *   AND THE CLOCK IS ONLY AS GOOD AS THE HOST'S. There is no NTP here. The RTC
 *   is whatever the machine (or QEMU) says, assumed to be UTC, and
 *   drivers/rtc/rtc.c deliberately offers no way to set it — see the DECISION
 *   note at the top of that file. A wrong host clock produces wrong validity
 *   answers in both directions and nothing in this kernel can detect it.
 *
 * PUBLIC API
 *   Trust anchors:
 *     tls_ca_bundle / tls_ca_bundle_len   the PEM bytes handed to mbedTLS
 *     tls_ca_root_count / tls_ca_root     what is in there, for printing
 *     tls_ca_bundle_selftest              structural check of the embedded blob
 *     tls_ca_pem_count                    count PEM blocks in an arbitrary blob
 *   Clock (pure given the RTC; this is what the host tests drive):
 *     tls_ca_now_unix                     seconds since the epoch, 0 = no clock
 *     tls_ca_gmtime_r                     epoch seconds -> struct tm (UTC)
 *   mbedTLS adapters (only compiled under FABLEOS_VERIFY_CERTS, and only ever
 *   called by mbedTLS itself):
 *     fableos_tls_time                     MBEDTLS_PLATFORM_TIME_MACRO
 *     mbedtls_platform_gmtime_r           MBEDTLS_PLATFORM_GMTIME_R_ALT
 *
 * DEPENDENCIES
 *   drivers/rtc/rtc.c for the clock, <time.h> for struct tm (port/time.h in the
 *   kernel, the real one on the host). No mbedTLS headers outside the flagged
 *   adapter block, which is what lets tests/host/test_tls_verify.c link this
 *   file natively.
 *
 * FUTURE EXTENSION POINTS
 *   - Revocation. There is none. MBEDTLS_X509_CRL_PARSE_C plus a fetched CRL,
 *     or OCSP stapling, is the honest next step; neither is here.
 *   - Entropy. Verification says nothing about the quality of the CTR_DRBG
 *     seed, which is still mbedtls_hardware_poll()'s RDRAND/TSC mix in
 *     lib/libc_shim.c. That is a separate, unfixed weakness.
 *   - A second bundle for a user-configured endpoint, so the model could be
 *     pointed somewhere else without a rebuild. Today the trust set is as
 *     hardcoded as the IP address.
 */
#ifndef TLS_CA_H
#define TLS_CA_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>       /* struct tm — port/time.h in the kernel, libc on host */

/* ---- errors from tls_ca_bundle_selftest() (errno-style negatives) ---- */
#define TLS_CA_OK            0
#define TLS_CA_EMALFORMED   -1   /* the embedded PEM is not well-formed      */
#define TLS_CA_ECOUNT       -2   /* block count != tls_ca_root_count()       */
#define TLS_CA_ETERM        -3   /* the blob is not NUL-terminated as mbedTLS
                                    requires for PEM input                   */

/* What tls_ca_now_unix() returns when this machine has no readable clock.
 * Zero, not -1: it flows into mbedTLS as a real timestamp (the epoch), which
 * makes every certificate "not yet valid" and fails the handshake closed. */
#define TLS_CA_NO_CLOCK   ((int64_t)0)

/* ====================================================================== */
/* trust anchors                                                          */
/* ====================================================================== */

/* The embedded PEM bundle. NUL-terminated, and tls_ca_bundle_len() *includes*
 * that NUL — mbedTLS only takes the PEM path in mbedtls_x509_crt_parse() when
 * buf[buflen - 1] == '\0', so passing the string length alone silently reparses
 * the text as DER and fails. Never NULL. */
const char *tls_ca_bundle(void);
size_t      tls_ca_bundle_len(void);

/* One embedded root, for printing and for tests. All fields are static strings
 * and never NULL. `sha256` is 64 uppercase hex characters, no separators. */
typedef struct tls_ca_root {
    const char *name;      /* "GTS Root R4"                      */
    const char *key;       /* "ECDSA P-384"                      */
    const char *sha256;    /* fingerprint of the DER certificate */
    const char *not_after; /* "2036-06-22", the pin's expiry date */
    const char *why;       /* one line: why this root is trusted */
} tls_ca_root_t;

int                  tls_ca_root_count(void);
const tls_ca_root_t *tls_ca_root(int index);   /* NULL if out of range */

/* Count "-----BEGIN CERTIFICATE-----"/"-----END CERTIFICATE-----" pairs in a
 * blob of `len` bytes. Returns the number of well-formed blocks, or
 * TLS_CA_EMALFORMED if a BEGIN is unterminated or an END appears first. Does
 * not decode base64 and does not validate the certificates — mbedTLS does that,
 * and this is the cheap structural check that runs before it. */
int tls_ca_pem_count(const char *pem, size_t len);

/* Structural check of the *embedded* bundle: NUL-terminated, parses as PEM,
 * and contains exactly tls_ca_root_count() blocks. TLS_CA_OK or a negative
 * code. Cheap enough to call at boot, which net_init() does under the flag. */
int tls_ca_bundle_selftest(void);

/* ====================================================================== */
/* the clock mbedTLS checks certificates against                          */
/* ====================================================================== */

/* Seconds since 1970-01-01T00:00:00Z from the CMOS RTC, or TLS_CA_NO_CLOCK (0)
 * if the chip cannot be read. Reads the hardware every call: a certificate
 * check must not be answered from a value cached before the machine slept. */
int64_t tls_ca_now_unix(void);

/* Break `ts` (seconds since the epoch, UTC) into `out`. Fills every field
 * struct tm has, including tm_wday and tm_yday, with tm_isdst forced to 0 —
 * there is no local time here and never will be. Returns `out`, or NULL if
 * `out` is NULL or `ts` is outside the range drivers/rtc/rtc.c converts
 * ([0, 2999-12-31T23:59:59Z]). NULL is what makes mbedTLS treat the certificate
 * as unverifiable rather than valid. */
struct tm *tls_ca_gmtime_r(int64_t ts, struct tm *out);

#endif /* TLS_CA_H */
