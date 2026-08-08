/* arch/cc.h — lwIP compiler/architecture abstraction for fable-os. */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* x86 is little-endian. */
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* We have no <inttypes.h>; define lwIP's printf format strings directly. */
#define LWIP_NO_INTTYPES_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

/* Struct packing (GCC). */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x

/* Diagnostics + assertions routed to the kernel console.
 *
 * NOT kprintf. The kernel's kprintf (lib/base.c) implements a deliberately
 * reduced grammar — %s %c %d %u %x %p with a 0/width — and on any conversion
 * outside it prints the two characters verbatim and consumes NO vararg, so
 * every remaining argument on that line is read from the wrong slot and a later
 * %s dereferences a non-pointer. lwIP's own format strings use SZT_F, which is
 * "lu" above, so the first lwIP diagnostic to print a size_t would do exactly
 * that — in the middle of debugging a network problem, which is the one moment
 * the console has to be trustworthy. libc_shim.c's vsnprintf does implement the
 * l/ll/z modifiers, width and precision, so format there and print the bytes.
 *
 * 256 bytes: longer than any format string in the vendored tree, and this runs
 * on the kernel's 64 KiB stack. */
extern void kprintf(const char *fmt, ...);
extern void kputs(const char *s);
extern void panic(const char *msg);
extern int  vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

static inline void lwip_diag(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    kputs(b);
}

#define LWIP_PLATFORM_DIAG(x)   do { lwip_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { kprintf("\nlwIP ASSERT: %s\n", x); panic("lwip assert"); } while (0)

/* Randomness for TCP ISN / DNS txids. */
extern unsigned int lwip_rand(void);
#define LWIP_RAND() ((u32_t)lwip_rand())

#endif /* LWIP_ARCH_CC_H */
