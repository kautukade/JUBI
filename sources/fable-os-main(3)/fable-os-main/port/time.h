/* time.h — freestanding shim: what mbedTLS gets when it asks for <time.h>.
 *
 * This is NOT a stub for stray includes. MBEDTLS_HAVE_TIME is defined
 * unconditionally (include/mbedtls_config.h), so in the DEFAULT build
 * mbedtls_time() IS the time() below, counting seconds since boot — fine for
 * session timestamps, and the reason certificate dates cannot be judged in that
 * build. With -DFABLEOS_VERIFY_CERTS, MBEDTLS_PLATFORM_TIME_MACRO redirects
 * mbedTLS to fableos_tls_time() (net/tls_ca.c, reading the CMOS RTC) and
 * MBEDTLS_HAVE_TIME_DATE makes notBefore/notAfter enforced against it — so in
 * that build nothing here is on the validation path. See the long argument in
 * include/mbedtls_config.h; net/tls_ca.c is the authority on which clock is
 * used when. */
#ifndef PORT_TIME_H
#define PORT_TIME_H
#include <stddef.h>

typedef long time_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
        tm_wday, tm_yday, tm_isdst;
};

/* Seconds since boot — enough for mbedTLS session timestamps, and not a
 * calendar clock: 0 here means January 1970, which is why a build that wants to
 * enforce certificate validity periods redirects mbedTLS at the RTC instead of
 * at this. */
time_t time(time_t *t);

#endif /* PORT_TIME_H */
