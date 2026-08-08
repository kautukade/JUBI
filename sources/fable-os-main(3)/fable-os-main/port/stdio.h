/* stdio.h — freestanding shim. mbedTLS uses snprintf/vsnprintf (string
 * formatting only; no real files, since MBEDTLS_FS_IO is off). */
#ifndef PORT_STDIO_H
#define PORT_STDIO_H
#include <stddef.h>
#include <stdarg.h>

int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

#endif /* PORT_STDIO_H */
