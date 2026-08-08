/* mbedtls_config.h — mbedTLS 2.28 config for fable-os.
 *
 * Tuned for one job: a TLS 1.2 *client* that can complete a handshake with a
 * modern HTTPS server (ECDHE + AES-GCM / ChaCha20, P-256/P-384/X25519) from a
 * freestanding kernel. Selected with -DMBEDTLS_CONFIG_FILE="mbedtls_config.h".
 *
 * Deliberately minimal for a bare-metal target:
 *   - No filesystem, no sockets.
 *   - Entropy comes from mbedtls_hardware_poll() (RDRAND/TSC) — see libc_shim.c.
 *     That is NOT a vetted CSPRNG seed and this file does not fix it.
 *   - Certificate verification is a build-time choice. See the FABLEOS_VERIFY_CERTS
 *     block at the bottom: without the flag the kernel runs
 *     MBEDTLS_SSL_VERIFY_NONE and the certificate is parsed but not
 *     trust-checked (encrypted, not authenticated); with it, the chain is
 *     verified against include/tls_ca.h's pinned roots, the hostname is checked,
 *     and notBefore/notAfter are enforced against the CMOS clock.
 */
#ifndef FABLEOS_MBEDTLS_CONFIG_H
#define FABLEOS_MBEDTLS_CONFIG_H

/* ---- platform ---- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME                  /* session timestamps. On its own this
                                              is NOT date validation — that needs
                                              MBEDTLS_HAVE_TIME_DATE, which only
                                              the flag block below turns on. */
#define MBEDTLS_NO_PLATFORM_ENTROPY        /* no /dev/urandom; use hardware poll */
#define MBEDTLS_ENTROPY_HARDWARE_ALT       /* we provide mbedtls_hardware_poll() */

/* ---- big numbers / elliptic curves / RSA ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_GENPRIME

/* ---- hashes ---- */
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C                      /* legacy cert signatures */
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* ---- symmetric ciphers / AEAD ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

/* ---- RNG ---- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* ---- ASN.1 / OID / public keys / X.509 ---- */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---- TLS (client, 1.2) ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* ---- key exchanges (ECDHE preferred; plain RSA as a fallback) ---- */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* ====================================================================== */
/* FABLEOS_VERIFY_CERTS — real certificate verification, opt-in            */
/* ====================================================================== */
/*
 * Everything above this line is the historical configuration and is what a
 * plain `make` still produces, byte for byte. Building with
 *
 *     make EXTRA_CFLAGS=-DFABLEOS_VERIFY_CERTS
 *
 * adds the four things mbedTLS needs before it will actually refuse a bad
 * certificate. (The fifth, MBEDTLS_SSL_VERIFY_REQUIRED, is set through lwIP's
 * ALTCP_MBEDTLS_AUTHMODE in port/lwipopts.h; the sixth, the CA chain itself,
 * is installed by net/net.c from net/tls_ca.c.)
 *
 * The flag defaults OFF on purpose and not out of timidity: other work in this
 * tree boots this kernel continuously, and a pinned trust set is exactly the
 * kind of thing that starts failing on someone else's afternoon. Turning
 * verification on must be a decision, and reversing it must be a rebuild.
 *
 * WHY EACH ONE IS NEEDED
 *
 *   MBEDTLS_HAVE_TIME_DATE
 *     Without it, x509.c compiles mbedtls_x509_time_is_past()/is_future() to
 *     `return 0` — the notBefore/notAfter fields are parsed and then ignored.
 *     This is the single define that decides whether validity periods are
 *     enforced or merely stored, so "we check expiry" is false until it is set.
 *
 *   MBEDTLS_PLATFORM_C + MBEDTLS_PLATFORM_TIME_MACRO/_TYPE_MACRO
 *     mbedTLS's default clock is the C library's time(). In this kernel that is
 *     port/time.h's stub, which counts seconds since boot, so every handshake
 *     would be evaluated as if it were January 1970 and every real certificate
 *     would look not-yet-valid. Redirect it to fableos_tls_time(), which reads
 *     the CMOS RTC. MBEDTLS_PLATFORM_C is a prerequisite of the two TIME macros
 *     (mbedtls/check_config.h enforces that); it pulls in library/platform.c,
 *     which with no *_ALT options selected compiles to almost nothing.
 *     _TYPE_MACRO pins mbedtls_time_t to `long` so the adapter's signature is
 *     fixed and does not depend on which <time.h> got included first.
 *
 *   MBEDTLS_PLATFORM_GMTIME_R_ALT
 *     Having a timestamp is not enough: x509.c turns it into a calendar date
 *     with mbedtls_platform_gmtime_r(), whose stock implementation calls
 *     gmtime_r(). There is no gmtime_r in a freestanding build — without this
 *     define the kernel does not link. net/tls_ca.c provides the replacement on
 *     top of the RTC driver's proleptic-Gregorian arithmetic.
 *
 *   MBEDTLS_PEM_PARSE_C + MBEDTLS_BASE64_C
 *     The trust bundle is PEM. mbedtls_x509_crt_parse() only iterates over
 *     *multiple* certificates on its PEM path; without PEM support it would
 *     parse the first root of the bundle, silently ignore the second, and
 *     report success. See the "WHY THE BUNDLE IS PEM" note in net/tls_ca.c.
 *
 *   MBEDTLS_X509_CHECK_KEY_USAGE + MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE
 *     Both are ON by default in mbedTLS's own config.h, each with an explicit
 *     "disabling this can be a security risk" warning, and this file is a
 *     complete REPLACEMENT config — so leaving them out silently dropped two
 *     path-validation checks that a pinning verifier has no business missing:
 *       - keyUsage: an intermediate with CA:TRUE but keyUsage restricted to
 *         digitalSignature could still act as an issuer, even though its own
 *         CA put that restriction there to say "this key must not sign
 *         certificates". Same define also requires a leaf's keyUsage to permit
 *         the signature the ciphersuite needs.
 *       - extendedKeyUsage: a certificate for this host issued for some other
 *         purpose (codeSigning, say) was accepted as a TLS server certificate.
 *         "Any certificate this CA ever issued for this name" is a materially
 *         larger set than "a TLS server certificate for this name".
 *     Neither is a live break — it takes a mis-issuance by a pinned root — but
 *     both are cheap, and the live api.anthropic.com chain satisfies them
 *     (leaf: KU=digitalSignature, EKU=serverAuth; WE1: keyCertSign).
 */
#ifdef FABLEOS_VERIFY_CERTS

#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO long
#define MBEDTLS_PLATFORM_TIME_MACRO      fableos_tls_time
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE

/* Declared here rather than by including tls_ca.h, because this file is pulled
 * in at the top of every mbedTLS translation unit — before <time.h>, before
 * anything — and MBEDTLS_PLATFORM_TIME_MACRO expands at each call site, so the
 * prototype has to already be visible there. Defined in net/tls_ca.c; the
 * matching declaration in include/tls_ca.h is what keeps the two in step.
 * (mbedtls_platform_gmtime_r needs no declaration here: mbedTLS declares it
 * itself in mbedtls/platform_util.h.) */
#ifndef __ASSEMBLER__
long fableos_tls_time(long *tloc);
#endif

#endif /* FABLEOS_VERIFY_CERTS */

#include "mbedtls/check_config.h"

#endif /* FABLEOS_MBEDTLS_CONFIG_H */
