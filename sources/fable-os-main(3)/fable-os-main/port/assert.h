/* assert.h — freestanding shim, so that a vendored `#include <assert.h>`
 * resolves. Nothing in the compiled set actually calls assert(): mbedTLS's
 * common.h and platform_util.h include this header but use MBEDTLS_* macros and
 * static_assert, and lwIP has its own LWIP_ASSERT (routed to panic() by
 * port/arch/cc.h, see the note in port/lwipopts.h). So the macro below is a
 * contract for a caller that does not exist yet rather than a description of
 * anything that happens today — but it is what SHOULD happen if one appears:
 * halting beats continuing past a violated invariant in ring 0. */
#ifndef PORT_ASSERT_H
#define PORT_ASSERT_H

void panic(const char *msg);

#define assert(expr) do { if (!(expr)) panic("assert failed: " #expr); } while (0)

#endif /* PORT_ASSERT_H */
