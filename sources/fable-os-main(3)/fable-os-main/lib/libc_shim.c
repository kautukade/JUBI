/* libc_shim.c — the handful of libc functions lwIP and mbedTLS need that aren't
 * already in base.c, plus the lwIP port hooks and an entropy source for TLS. */

#include "kernel.h"
#include "io.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ---- string ---- */
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

/* NOT dead code, however it looks from a default build. mbedTLS's PEM decoder
 * (mbedtls_pem_read_buffer) and mbedtls_x509_crt_parse call strstr to find the
 * "-----BEGIN ..." delimiters, and include/mbedtls_config.h only defines
 * MBEDTLS_PEM_PARSE_C under FABLEOS_VERIFY_CERTS. So `nm -u` over a default
 * build's objects reports no caller, deleting this looks safe, every
 * translation unit still compiles because port/string.h declares it — and
 * `make EXTRA_CFLAGS=-DFABLEOS_VERIFY_CERTS` fails at LINK time with three
 * undefined references. That happened. The documented security-hardening build
 * is the one thing an unbounded-by-construction primitive earns its place for;
 * if it is ever removed again, remove MBEDTLS_PEM_PARSE_C's users first. */
char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

/* ---- stdlib ----
 * mbedTLS reaches the kernel heap through calloc/free only; malloc and realloc
 * are here because port/stdlib.h declares the whole quartet for vendored code,
 * and a heap shim that offers three quarters of it is the kind of asymmetry
 * that costs someone an afternoon. They have no caller in the current build. */
void *malloc(size_t n)            { return kmalloc(n); }
void  free(void *p)               { kfree(p); }
void *calloc(size_t n, size_t sz) { return kcalloc(n, sz); }
void *realloc(void *p, size_t n)  { return krealloc(p, n); }
int   abs(int x)                  { return x < 0 ? -x : x; }

long strtol(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if ((base == 16 || base == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (base == 0) base = 10;
    long v = 0;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

/* ---- time (seconds since boot; mbedTLS session timestamps only) ---- */
long time(long *t) {
    long s = (long)(millis() / 1000);
    if (t) *t = s;
    return s;
}

/* ---- lwIP port hooks ---- */
unsigned int sys_now(void) { return (unsigned int)millis(); }

/* Cheap LCG seeded once from the TSC. Good enough for ISN/txid entropy. */
int rand(void) {
    static uint32_t seed;
    if (seed == 0) seed = (uint32_t)rdtsc() | 1;
    seed = seed * 1103515245u + 12345u;
    return (int)((seed >> 16) & 0x7FFF);
}

unsigned int lwip_rand(void) {
    return ((unsigned int)rand() << 17) ^ ((unsigned int)rand() << 9) ^ (unsigned int)rand();
}

/* snprintf/vsnprintf USED TO BE HERE. They now live in lib/kfmt.c, which
 * includes nothing but <stdint.h>/<stddef.h>/<stdarg.h> so that a host suite
 * can link the real kernel formatter and test it. This file cannot be compiled
 * for a host test — it needs kernel.h, io.h, kmalloc, millis and RDRAND — which
 * is why the formatter had no test at all while it was in here, and why a
 * defect in it survived in the text models read to fix their own programs. Do
 * not move them back. */

/* ====================================================================== */
/* Entropy for TLS — mbedTLS calls this to seed CTR_DRBG.                   */
/* Uses RDRAND when the CPU advertises it, else mixes the TSC. NOTE: this   */
/* is functional, not a vetted CSPRNG — fine for a hobby OS, not for real   */
/* secrets. mbedtls_hardware_poll is enabled via MBEDTLS_ENTROPY_HARDWARE_ALT. */
/* ====================================================================== */

static int have_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    eax = 1;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    return (ecx >> 30) & 1;
}

/* The weak fallback: an LCG over the TSC. Not a CSPRNG, but never
 * uninitialised and never a repeated constant, which is the property that
 * matters when it is the last resort for seeding CTR_DRBG. */
static uint64_t tsc_mix(void) {
    static uint64_t st;
    st = (st ^ rdtsc()) * 6364136223846793005ull + 1442695040888963407ull;
    return st ^ (st >> 31) ^ ((uint64_t)rand() << 32);
}

static uint64_t rdrand64(void) {
    uint64_t v; unsigned char ok;
    for (int tries = 0; tries < 32; tries++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) return v;
    }
    /* CPUID advertised RDRAND but the instruction kept declining — a documented
     * hardware/hypervisor state. Falling back is not optional: returning `v`
     * here would return an UNINITIALISED local and mbedtls_hardware_poll below
     * would still report success, seeding TLS from stack garbage. */
    return tsc_mix();
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    static int rdrand = -1;
    if (rdrand < 0) rdrand = have_rdrand();
    size_t i = 0;
    while (i < len) {
        uint64_t r = rdrand ? rdrand64() : tsc_mix();
        size_t n = (len - i < sizeof r) ? (len - i) : sizeof r;
        memcpy(output + i, &r, n);
        i += n;
    }
    *olen = len;
    return 0;
}
