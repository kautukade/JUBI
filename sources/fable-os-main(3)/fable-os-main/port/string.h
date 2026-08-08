/* string.h — freestanding shim for the vendored trees AND for kernel code that
 * includes <string.h> (fs/vfs/vfs.c and fs/native/ramfs.c reach strncpy through
 * here). Every one of these is defined in lib/libc_shim.c; this file and that
 * one have to stay in step, which is why declarations are not trimmed here
 * alone.
 *
 * strcpy is declared and NOT defined: nothing in any build configuration calls
 * it, and it stays declared only because vendored code expects the name to
 * exist. strstr looks equally dead from a default build and is not — mbedTLS's
 * PEM parser needs it whenever FABLEOS_VERIFY_CERTS is set, and because these
 * declarations make every TU compile regardless, removing its definition breaks
 * that build at LINK time only. See the comment on strstr in lib/libc_shim.c
 * before touching either. */
#ifndef PORT_STRING_H
#define PORT_STRING_H
#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);

#endif /* PORT_STRING_H */
