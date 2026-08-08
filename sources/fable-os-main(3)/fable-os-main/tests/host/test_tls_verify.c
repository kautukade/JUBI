/* test_tls_verify.c — the pure half of TLS certificate verification, on the host.
 *
 * Runs the REAL net/tls_ca.c against the REAL drivers/rtc/rtc.c. Nothing here
 * reimplements the code under test.
 *
 * WHAT CAN AND CANNOT BE TESTED HERE, STATED UP FRONT
 *   The actual chain-building, signature checking and hostname matching happen
 *   inside mbedTLS, which is not linked into host tests, so this suite does not
 *   and cannot claim to prove "the verifier works". That claim is made by
 *   booting the kernel: a real handshake to api.anthropic.com succeeding under
 *   the flag, and a deliberately untrusted chain being refused. See the
 *   FABLEOS_VERIFY_CERTS section of README.md.
 *
 *   What is testable here is everything the kernel itself contributes to the
 *   decision, which is exactly where a bug would be silent:
 *
 *     1. THE BUNDLE IS THE ROOTS IT SAYS IT IS. Each embedded PEM block is
 *        base64-decoded and SHA-256'd right here, with a self-contained
 *        reference implementation, and compared against the fingerprint
 *        net/tls_ca.c publishes for it. A one-character edit to the base64, a
 *        swapped certificate, or a "helpfully" added root all fail. This is the
 *        test that makes the header comment's claims enforceable rather than
 *        aspirational.
 *
 *     2. THE BUNDLE IS SHAPED THE WAY mbedTLS NEEDS. mbedtls_x509_crt_parse()
 *        only takes its PEM path when the last byte of the buffer it is given
 *        is NUL; get that wrong and the blob is parsed as DER, one root is
 *        silently dropped, and the kernel comes up trusting less than it thinks.
 *        tls_ca_bundle_len() including the NUL is asserted directly.
 *
 *     3. THE CLOCK ADAPTER PRODUCES THE FIELDS mbedTLS READS. x509.c's
 *        x509_get_current_time() takes tm_year/tm_mon/tm_mday/tm_hour/tm_min/
 *        tm_sec out of our gmtime_r replacement and compares them, in that
 *        order, against the certificate's notBefore/notAfter. So the adapter is
 *        checked field by field against the host libc's own gmtime_r over a
 *        table of awkward instants, and then the *comparison mbedTLS performs*
 *        is transcribed below from mbedtls/library/x509.c and driven from our
 *        clock, against the real validity windows of the real certificates.
 *        A leap-year bug or an off-by-one in tm_mon shows up as a certificate
 *        accepted a month early, which is precisely the kind of thing nobody
 *        notices until it matters.
 *
 *     4. NO CLOCK MEANS NO. With the RTC unreadable, tls_ca_now_unix() must
 *        return the epoch and every real certificate must come out
 *        "not yet valid". A verifier that skips the date check when the clock
 *        is missing is worse than one that has no date check at all, because it
 *        claims to have one.
 *
 * The synthetic MC146818 below is a cut-down cousin of the one in test_rtc.c —
 * that suite owns the hard cases (tearing, UIP, BCD, 12-hour); this one only
 * needs a chip that can be set to a date or made to disappear.
 */

#include "harness.h"
#include "tls_ca.h"
#include "rtc.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ====================================================================== */
/* a synthetic CMOS, just enough to place the kernel at a chosen instant   */
/* ====================================================================== */

static rtc_time_t fake_now;
static int        fake_dead;      /* chip absent: status A never clears */

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static uint8_t fake_read(uint8_t reg)
{
    if (fake_dead) return (reg == RTC_REG_STATUS_A) ? RTC_STATUS_A_UIP : 0xFF;

    switch (reg) {
    case RTC_REG_STATUS_A: return 0x26;                 /* not updating */
    case RTC_REG_STATUS_B: return RTC_STATUS_B_24H;     /* 24-hour, BCD */
    case RTC_REG_SECOND:   return bcd(fake_now.second);
    case RTC_REG_MINUTE:   return bcd(fake_now.minute);
    case RTC_REG_HOUR:     return bcd(fake_now.hour);
    case RTC_REG_DAY:      return bcd(fake_now.day);
    case RTC_REG_MONTH:    return bcd(fake_now.month);
    case RTC_REG_YEAR:     return bcd(fake_now.year % 100);
    case RTC_REG_CENTURY:  return bcd(fake_now.year / 100);
    default:               return 0;
    }
}

/* Put the machine at `ts` seconds past the epoch. */
static void set_clock(int64_t ts)
{
    fake_dead = 0;
    CHECK_EQ(rtc_from_unix(ts, &fake_now), RTC_OK);
    rtc_set_backend(fake_read);
}

static void kill_clock(void)
{
    fake_dead = 1;
    rtc_set_backend(fake_read);
}

/* ====================================================================== */
/* SHA-256, self-contained, so the bundle can be fingerprinted            */
/* ====================================================================== */
/* FIPS 180-4. Deliberately a plain transcription: this is the oracle the
 * embedded certificates are measured against, so it must not share a line of
 * code with anything in the kernel. Verified below against the two published
 * NIST test vectors before it is used to judge anything. */

typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t len; size_t n; } sha256_t;

static uint32_t ror32(uint32_t x, int s) { return (x >> s) | (x << (32 - s)); }

static const uint32_t K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 |
               (uint32_t)p[4*i+2] << 8 | (uint32_t)p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i-15],7) ^ ror32(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror32(w[i-2],17) ^ ror32(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3];
    uint32_t e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e,6) ^ ror32(e,11) ^ ror32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror32(a,2) ^ ror32(a,13) ^ ror32(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha256_init(sha256_t *s)
{
    s->h[0]=0x6a09e667u; s->h[1]=0xbb67ae85u; s->h[2]=0x3c6ef372u; s->h[3]=0xa54ff53au;
    s->h[4]=0x510e527fu; s->h[5]=0x9b05688cu; s->h[6]=0x1f83d9abu; s->h[7]=0x5be0cd19u;
    s->len = 0; s->n = 0;
}

static void sha256_update(sha256_t *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t take = 64 - s->n;
        if (take > n) take = n;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; n -= take;
        if (s->n == 64) { sha256_block(s, s->buf); s->n = 0; }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) sha256_update(s, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8*i));
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4*i]   = (uint8_t)(s->h[i] >> 24);
        out[4*i+1] = (uint8_t)(s->h[i] >> 16);
        out[4*i+2] = (uint8_t)(s->h[i] >> 8);
        out[4*i+3] = (uint8_t)(s->h[i]);
    }
}

static void hex_upper(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) { out[2*i] = H[b[i] >> 4]; out[2*i+1] = H[b[i] & 15]; }
    out[2*n] = '\0';
}

/* ====================================================================== */
/* base64, to get from the embedded PEM back to the DER certificate       */
/* ====================================================================== */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode `n` base64 characters (whitespace skipped, '=' padding honoured) into
 * `out`. Returns the byte count, or -1 on a character that is neither. */
static long b64_decode(const char *in, size_t n, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int      bits = 0;
    size_t   w = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') break;
        int v = b64_val(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (w >= cap) return -1;
            out[w++] = (uint8_t)(acc >> bits);
        }
    }
    return (long)w;
}

/* ====================================================================== */
/* 1. the bundle really is the roots it claims                            */
/* ====================================================================== */

static void test_sha256_reference(void)
{
    /* NIST FIPS 180-4 examples. If these fail, every fingerprint assertion
     * below is meaningless, so they run first. */
    sha256_t s; uint8_t d[32]; char hex[65];

    sha256_init(&s); sha256_update(&s, (const uint8_t *)"abc", 3);
    sha256_final(&s, d); hex_upper(d, 32, hex);
    CHECK_STR(hex, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");

    sha256_init(&s);
    sha256_update(&s, (const uint8_t *)"", 0);
    sha256_final(&s, d); hex_upper(d, 32, hex);
    CHECK_STR(hex, "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");

    /* Multi-block, exercised across the 56/64-byte padding boundaries. */
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&s); sha256_update(&s, (const uint8_t *)msg, strlen(msg));
    sha256_final(&s, d); hex_upper(d, 32, hex);
    CHECK_STR(hex, "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1");

    /* Fed one byte at a time, the streaming path must agree with the bulk one. */
    sha256_init(&s);
    for (size_t i = 0; i < strlen(msg); i++)
        sha256_update(&s, (const uint8_t *)msg + i, 1);
    sha256_final(&s, d); hex_upper(d, 32, hex);
    CHECK_STR(hex, "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1");
}

/* Walk the embedded bundle, decode each PEM block to DER, fingerprint it, and
 * demand it matches what tls_ca_root(i) advertises — in the same order. */
static void test_bundle_fingerprints(void)
{
    const char *pem = tls_ca_bundle();
    const char *B   = "-----BEGIN CERTIFICATE-----";
    const char *E   = "-----END CERTIFICATE-----";

    int         found = 0;
    const char *p     = pem;

    for (;;) {
        const char *b = strstr(p, B);
        if (!b) break;
        const char *body = b + strlen(B);
        const char *e    = strstr(body, E);
        CHECK(e != NULL);
        if (!e) break;

        static uint8_t der[8192];
        long dlen = b64_decode(body, (size_t)(e - body), der, sizeof der);
        CHECK(dlen > 0);

        /* A certificate is a DER SEQUENCE; if the base64 decoded to something
         * that is not one, the bundle is not what anybody thinks it is. */
        CHECK(dlen > 4 && der[0] == 0x30);

        sha256_t s; uint8_t d[32]; char hex[65];
        sha256_init(&s);
        sha256_update(&s, der, (size_t)(dlen > 0 ? dlen : 0));
        sha256_final(&s, d);
        hex_upper(d, 32, hex);

        const tls_ca_root_t *r = tls_ca_root(found);
        CHECK(r != NULL);
        if (r) {
            /* The assertion the whole file exists for: the bytes compiled into
             * the kernel are the certificate the source comment names. */
            CHECK_STR(hex, r->sha256);
        }

        found++;
        p = e + strlen(E);
    }

    CHECK_EQ(found, tls_ca_root_count());
    CHECK_EQ(found, 2);          /* two roots, on purpose; see tls_ca.h */
}

static void test_bundle_shape(void)
{
    const char *pem = tls_ca_bundle();
    size_t      len = tls_ca_bundle_len();

    CHECK(pem != NULL);
    CHECK(len > 1000);

    /* mbedtls_x509_crt_parse() tests buf[buflen - 1] == '\0' to decide PEM vs
     * DER. If tls_ca_bundle_len() ever stops including the NUL, the bundle is
     * reparsed as DER, mbedtls_x509_crt_parse_der() takes the first
     * certificate, ignores the rest, and the kernel trusts one root while its
     * boot banner claims two. */
    CHECK_EQ(pem[len - 1], '\0');
    CHECK_EQ(len, strlen(pem) + 1);

    CHECK_EQ(tls_ca_bundle_selftest(), TLS_CA_OK);
    CHECK_EQ(tls_ca_pem_count(pem, len - 1), tls_ca_root_count());

    /* The PEM must be exactly PEM: no stray CR (mbedTLS's reader is picky about
     * the armour lines) and nothing before the first BEGIN. */
    CHECK(strchr(pem, '\r') == NULL);
    CHECK(strncmp(pem, "-----BEGIN CERTIFICATE-----", 27) == 0);
}

static void test_root_metadata(void)
{
    CHECK_EQ(tls_ca_root_count(), 2);
    CHECK(tls_ca_root(-1) == NULL);
    CHECK(tls_ca_root(tls_ca_root_count()) == NULL);
    CHECK(tls_ca_root(1000000) == NULL);

    for (int i = 0; i < tls_ca_root_count(); i++) {
        const tls_ca_root_t *r = tls_ca_root(i);
        CHECK(r != NULL);
        if (!r) continue;
        CHECK(r->name && r->name[0]);
        CHECK(r->key && r->key[0]);
        CHECK(r->not_after && r->not_after[0]);
        CHECK(r->why && r->why[0]);
        /* The fingerprint is printed and compared as text; it has to be exactly
         * 64 uppercase hex digits or the comparison above is not what it looks
         * like. */
        CHECK_EQ((int)strlen(r->sha256), 64);
        for (int k = 0; k < 64; k++) {
            char c = r->sha256[k];
            CHECK((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
        }
    }

    /* Order is load-bearing: mbedTLS searches the trusted list in order and the
     * root actually on api.anthropic.com's path should be first. */
    CHECK_STR(tls_ca_root(0)->name, "GTS Root R4");
    CHECK_STR(tls_ca_root(1)->name, "GTS Root R1");
}

/* ====================================================================== */
/* 2. tls_ca_pem_count against input it was not designed for              */
/* ====================================================================== */

static void test_pem_count_edges(void)
{
    const char *B = "-----BEGIN CERTIFICATE-----";
    const char *E = "-----END CERTIFICATE-----";
    char buf[512];

    CHECK_EQ(tls_ca_pem_count(NULL, 10), TLS_CA_EMALFORMED);
    CHECK_EQ(tls_ca_pem_count("", 0), 0);
    CHECK_EQ(tls_ca_pem_count("nothing to see here", 19), 0);

    /* A BEGIN with no END: malformed, not "one certificate". */
    snprintf(buf, sizeof buf, "%s\nAAAA\n", B);
    CHECK_EQ(tls_ca_pem_count(buf, strlen(buf)), TLS_CA_EMALFORMED);

    /* A dangling END. */
    snprintf(buf, sizeof buf, "AAAA\n%s\n", E);
    CHECK_EQ(tls_ca_pem_count(buf, strlen(buf)), TLS_CA_EMALFORMED);

    /* END before BEGIN. */
    snprintf(buf, sizeof buf, "%s\n%s\n", E, B);
    CHECK_EQ(tls_ca_pem_count(buf, strlen(buf)), TLS_CA_EMALFORMED);

    /* Well formed, one and two blocks. */
    snprintf(buf, sizeof buf, "%s\nAAAA\n%s\n", B, E);
    CHECK_EQ(tls_ca_pem_count(buf, strlen(buf)), 1);
    snprintf(buf, sizeof buf, "%s\nAA\n%s\n%s\nBB\n%s\n", B, E, B, E);
    CHECK_EQ(tls_ca_pem_count(buf, strlen(buf)), 2);

    /* Junk around and between the blocks is ignored, the way a real bundle with
     * human-readable headers between certificates would be. */
    snprintf(buf, sizeof buf, "hello\n%s\nAA\n%s\nsubject=x\n%s\nBB\n%s\ntail", B, E, B, E);
    CHECK_EQ(tls_ca_pem_count(buf, strlen(buf)), 2);

    /* A truncated length must not read past it: the same buffer, cut before the
     * first END, is malformed. */
    snprintf(buf, sizeof buf, "%s\nAAAA\n%s\n", B, E);
    CHECK_EQ(tls_ca_pem_count(buf, strlen(B) + 3), TLS_CA_EMALFORMED);

    /* Embedded NULs must not stop the scan or run off the end. */
    memset(buf, 0, sizeof buf);
    memcpy(buf, B, strlen(B));
    buf[strlen(B)] = '\0';
    memcpy(buf + strlen(B) + 1, E, strlen(E));
    CHECK_EQ(tls_ca_pem_count(buf, strlen(B) + 1 + strlen(E)), 1);

    /* Markers that are one character short of the real thing are not markers. */
    CHECK_EQ(tls_ca_pem_count("-----BEGIN CERTIFICATE----", 26), 0);

    /* Fuzz: random bytes must never do anything but return a count or the one
     * error code, and must never read out of bounds (ASan/UBSan builds of the
     * host suite are the point of the bounded length). */
    srand(20260728);
    for (int it = 0; it < 4000; it++) {
        char f[128];
        size_t n = (size_t)(rand() % (int)sizeof f);
        for (size_t i = 0; i < n; i++) {
            int r = rand() % 8;
            f[i] = (r == 0) ? '-' : (r == 1) ? 'B' : (r == 2) ? 'E'
                 : (char)(rand() % 256);
        }
        int rc = tls_ca_pem_count(f, n);
        CHECK(rc >= 0 || rc == TLS_CA_EMALFORMED);
    }
}

/* ====================================================================== */
/* 3. the clock adapter                                                   */
/* ====================================================================== */

/* Instants chosen to break naive calendar code, plus everything a certificate
 * date is likely to be. */
static const int64_t interesting[] = {
    0,                      /* 1970-01-01T00:00:00Z                        */
    1,
    86399, 86400, 86401,    /* first midnight                              */
    68169600,               /* 1972-02-29, first leap day of the epoch     */
    951782400,              /* 2000-02-29, the century that IS a leap year */
    951868799, 951868800,   /* and the second either side of its midnight  */
    1466553600,             /* 2016-06-22, the pinned roots' notBefore     */
    2098569600,             /* 2036-06-22, the pinned roots' notAfter      */
    1735689600,             /* 2025-01-01                                  */
    1753293600,             /* 2025-07-23                                  */
    2147483647,             /* INT32_MAX, the 2038 problem                 */
    2147483648LL,
    4102444800LL,           /* 2100-01-01, the century that is NOT a leap  */
    4107542400LL,           /* 2100-03-01, one day after the missing 29th  */
    32503679999LL,          /* 2999-12-31T23:59:59Z, the driver's ceiling  */
};

static void test_gmtime_matches_libc(void)
{
    for (size_t i = 0; i < sizeof interesting / sizeof interesting[0]; i++) {
        int64_t ts = interesting[i];

        struct tm ours;
        CHECK(tls_ca_gmtime_r(ts, &ours) == &ours);

        time_t    t = (time_t)ts;
        struct tm want;
        CHECK(gmtime_r(&t, &want) == &want);

        /* The six fields x509.c actually reads, plus the two it does not, so a
         * caller that ever does read them is not lied to. */
        CHECK_EQ(ours.tm_year, want.tm_year);
        CHECK_EQ(ours.tm_mon,  want.tm_mon);
        CHECK_EQ(ours.tm_mday, want.tm_mday);
        CHECK_EQ(ours.tm_hour, want.tm_hour);
        CHECK_EQ(ours.tm_min,  want.tm_min);
        CHECK_EQ(ours.tm_sec,  want.tm_sec);
        CHECK_EQ(ours.tm_wday, want.tm_wday);
        CHECK_EQ(ours.tm_yday, want.tm_yday);
        CHECK_EQ(ours.tm_isdst, 0);
    }
}

/* A dense sweep: every hour of a decade spanning the certificates that matter,
 * compared against libc. Cheap, and it is where an off-by-one in the day-of-
 * year accumulation would hide. */
static void test_gmtime_sweep(void)
{
    for (int64_t ts = 1735689600LL;                 /* 2025-01-01 */
                 ts < 1735689600LL + 10LL*366*86400; ts += 3607) {
        struct tm ours, want;
        time_t t = (time_t)ts;
        if (!tls_ca_gmtime_r(ts, &ours)) { CHECK(0); continue; }
        gmtime_r(&t, &want);
        CHECK_EQ(ours.tm_year, want.tm_year);
        CHECK_EQ(ours.tm_mon,  want.tm_mon);
        CHECK_EQ(ours.tm_mday, want.tm_mday);
        CHECK_EQ(ours.tm_hour, want.tm_hour);
        CHECK_EQ(ours.tm_min,  want.tm_min);
        CHECK_EQ(ours.tm_sec,  want.tm_sec);
        CHECK_EQ(ours.tm_yday, want.tm_yday);
    }
}

static void test_gmtime_refuses_the_impossible(void)
{
    struct tm tm;

    /* NULL out. Returning a pointer here would be a write through NULL, and on
     * this kernel a null write does not even fault (page 0 is mapped). */
    CHECK(tls_ca_gmtime_r(0, NULL) == NULL);

    /* Before the epoch and past the driver's ceiling. NULL is what makes
     * x509_get_current_time() return -1, which makes both is_past() and
     * is_future() answer "yes" and the certificate be rejected twice over. */
    CHECK(tls_ca_gmtime_r(-1, &tm) == NULL);
    CHECK(tls_ca_gmtime_r(-1000000000LL, &tm) == NULL);
    CHECK(tls_ca_gmtime_r(INT64_MIN, &tm) == NULL);
    CHECK(tls_ca_gmtime_r(32503680000LL, &tm) == NULL);   /* 3000-01-01 */
    CHECK(tls_ca_gmtime_r(INT64_MAX, &tm) == NULL);

    /* The last second it does convert. */
    CHECK(tls_ca_gmtime_r(32503679999LL, &tm) == &tm);
    CHECK_EQ(tm.tm_year + 1900, 2999);
    CHECK_EQ(tm.tm_mon + 1, 12);
    CHECK_EQ(tm.tm_mday, 31);
}

static void test_now_reads_the_chip(void)
{
    /* A known instant in, the same instant out. */
    for (size_t i = 0; i < sizeof interesting / sizeof interesting[0]; i++) {
        set_clock(interesting[i]);
        CHECK_EQ(tls_ca_now_unix(), interesting[i]);
    }

    /* Every call re-reads: the clock must not be cached across a change. */
    set_clock(1735689600LL);
    CHECK_EQ(tls_ca_now_unix(), 1735689600LL);
    set_clock(1735689660LL);
    CHECK_EQ(tls_ca_now_unix(), 1735689660LL);
}

static void test_no_clock_is_the_epoch_not_a_negative(void)
{
    /* Two ways to have no clock: no backend at all (a host build), and a chip
     * that never stops saying "updating" (a dead or absent MC146818). */
    rtc_set_backend(NULL);
    CHECK_EQ(tls_ca_now_unix(), TLS_CA_NO_CLOCK);
    CHECK_EQ(tls_ca_now_unix(), 0);
    CHECK(tls_ca_now_unix() >= 0);

    kill_clock();
    CHECK_EQ(tls_ca_now_unix(), TLS_CA_NO_CLOCK);
    CHECK(tls_ca_now_unix() >= 0);

    /* And the epoch converts, so mbedTLS gets a date rather than a NULL — the
     * distinction matters: a date makes the certificate "not yet valid" with a
     * legible flag, NULL makes it unverifiable. Both reject; the first says why. */
    struct tm tm;
    CHECK(tls_ca_gmtime_r(tls_ca_now_unix(), &tm) == &tm);
    CHECK_EQ(tm.tm_year + 1900, 1970);

    rtc_set_backend(NULL);
}

/* ====================================================================== */
/* 4. the comparison mbedTLS will actually make                           */
/* ====================================================================== */
/* Transcribed from mbedtls/library/x509.c (2.28.9): x509_check_time() and the
 * two wrappers that call it. Reproduced here, not called from the kernel,
 * because the point is to drive mbedTLS's *real* decision procedure with OUR
 * clock and assert the verdict — mbedTLS is not linked into host tests, so the
 * only alternative is to hope.
 *
 *     static int x509_check_time( const mbedtls_x509_time *before,
 *                                 const mbedtls_x509_time *after )
 *     {
 *         if( before->year  > after->year )  return( 1 );
 *         if( before->year == after->year &&
 *             before->mon   > after->mon )   return( 1 );
 *         ... and so on through day, hour, min, sec ...
 *         return( 0 );
 *     }
 */

typedef struct { int year, mon, day, hour, min, sec; } x509_time_t;

static int x509_check_time(const x509_time_t *before, const x509_time_t *after)
{
    if (before->year  > after->year)  return 1;
    if (before->year == after->year &&
        before->mon   > after->mon)   return 1;
    if (before->year == after->year &&
        before->mon  == after->mon  &&
        before->day   > after->day)   return 1;
    if (before->year == after->year &&
        before->mon  == after->mon  &&
        before->day  == after->day  &&
        before->hour  > after->hour)  return 1;
    if (before->year == after->year &&
        before->mon  == after->mon  &&
        before->day  == after->day  &&
        before->hour == after->hour &&
        before->min   > after->min)   return 1;
    if (before->year == after->year &&
        before->mon  == after->mon  &&
        before->day  == after->day  &&
        before->hour == after->hour &&
        before->min  == after->min  &&
        before->sec   > after->sec)   return 1;
    return 0;
}

/* x509_get_current_time(), rebuilt on top of the adapter mbedTLS is pointed at
 * by MBEDTLS_PLATFORM_TIME_MACRO / MBEDTLS_PLATFORM_GMTIME_R_ALT. Returns 0 on
 * success, -1 when the clock could not be turned into a date. */
static int x509_now(x509_time_t *now)
{
    struct tm tm;
    if (!tls_ca_gmtime_r(tls_ca_now_unix(), &tm)) return -1;
    now->year = tm.tm_year + 1900;
    now->mon  = tm.tm_mon + 1;
    now->day  = tm.tm_mday;
    now->hour = tm.tm_hour;
    now->min  = tm.tm_min;
    now->sec  = tm.tm_sec;
    return 0;
}

static int time_is_past(const x509_time_t *to)
{
    x509_time_t now;
    if (x509_now(&now) != 0) return 1;          /* mbedTLS's fail-closed default */
    return x509_check_time(&now, to);
}

static int time_is_future(const x509_time_t *from)
{
    x509_time_t now;
    if (x509_now(&now) != 0) return 1;
    return x509_check_time(from, &now);
}

/* GTS Root R4 / R1: notBefore 2016-06-22T00:00:00Z, notAfter 2036-06-22. */
static const x509_time_t root_from = { 2016, 6, 22, 0, 0, 0 };
static const x509_time_t root_to   = { 2036, 6, 22, 0, 0, 0 };

/* The leaf api.anthropic.com served on 2026-07-28, as an example of the ~90-day
 * window a real endpoint certificate has. Values from
 * `openssl x509 -noout -dates`:
 *   notBefore=Jul 24 18:00:59 2026 GMT   notAfter=Oct 22 19:00:52 2026 GMT */
static const x509_time_t leaf_from = { 2026, 7, 24, 18, 0, 59 };
static const x509_time_t leaf_to   = { 2026, 10, 22, 19, 0, 52 };

static void test_validity_verdicts(void)
{
    /* Inside both windows: accepted. */
    set_clock(1785283200LL);                    /* 2026-07-29T00:00:00Z */
    CHECK_EQ(time_is_future(&root_from), 0);
    CHECK_EQ(time_is_past(&root_to),     0);
    CHECK_EQ(time_is_future(&leaf_from), 0);
    CHECK_EQ(time_is_past(&leaf_to),     0);

    /* One second before the leaf's notBefore: not yet valid. This is the check
     * that does not exist at all without MBEDTLS_HAVE_TIME_DATE. */
    set_clock(1784916058LL);                    /* 2026-07-24T18:00:58Z */
    CHECK_EQ(time_is_future(&leaf_from), 1);
    /* Exactly at notBefore: valid. mbedTLS's comparison is inclusive. */
    set_clock(1784916059LL);                    /* 2026-07-24T18:00:59Z */
    CHECK_EQ(time_is_future(&leaf_from), 0);

    /* Exactly at notAfter: still valid. One second later: expired. */
    set_clock(1792695652LL);                    /* 2026-10-22T19:00:52Z */
    CHECK_EQ(time_is_past(&leaf_to), 0);
    set_clock(1792695653LL);
    CHECK_EQ(time_is_past(&leaf_to), 1);

    /* Far future: the leaf has expired but the roots have not, which is the
     * pair of answers that makes an expired-leaf handshake fail for the right
     * reason rather than because the anchor also died. */
    set_clock(1893456000LL);                    /* 2030-01-01 */
    CHECK_EQ(time_is_past(&leaf_to), 1);
    CHECK_EQ(time_is_past(&root_to), 0);
    CHECK_EQ(time_is_future(&root_from), 0);

    /* Past the pin's own expiry: the roots go too, exactly as tls_ca.h warns. */
    set_clock(2114380800LL);                    /* 2037-01-01 */
    CHECK_EQ(time_is_past(&root_to), 1);

    /* Before the roots existed. */
    set_clock(1420070400LL);                    /* 2015-01-01 */
    CHECK_EQ(time_is_future(&root_from), 1);
    CHECK_EQ(time_is_future(&leaf_from), 1);

    /* Boundary of the root window, to the second, both sides. */
    set_clock(1466553599LL);
    CHECK_EQ(time_is_future(&root_from), 1);
    set_clock(1466553600LL);
    CHECK_EQ(time_is_future(&root_from), 0);

    rtc_set_backend(NULL);
}

static void test_no_clock_rejects_everything(void)
{
    /* THE test of the fail-closed design. With no readable RTC the kernel is at
     * the epoch, so every certificate ever issued is "not yet valid" and no
     * handshake can succeed. The failure is loud and dated, not silent. */
    kill_clock();

    CHECK_EQ(time_is_future(&leaf_from), 1);
    CHECK_EQ(time_is_future(&root_from), 1);

    /* And nothing sneaks through the other side: a certificate that expired
     * long ago is still not accepted just because we think it is 1970. */
    const x509_time_t ancient_to = { 1969, 12, 31, 23, 59, 59 };
    CHECK_EQ(time_is_past(&ancient_to), 1);

    rtc_set_backend(NULL);
    CHECK_EQ(time_is_future(&leaf_from), 1);
    CHECK_EQ(time_is_future(&root_from), 1);
}

/* ====================================================================== */

int main(void)
{
    RUN(test_sha256_reference);
    RUN(test_bundle_shape);
    RUN(test_bundle_fingerprints);
    RUN(test_root_metadata);
    RUN(test_pem_count_edges);
    RUN(test_gmtime_matches_libc);
    RUN(test_gmtime_sweep);
    RUN(test_gmtime_refuses_the_impossible);
    RUN(test_now_reads_the_chip);
    RUN(test_no_clock_is_the_epoch_not_a_negative);
    RUN(test_validity_verdicts);
    RUN(test_no_clock_rejects_everything);
    return th_report("tls_verify");
}
