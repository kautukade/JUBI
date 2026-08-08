/* stdlib.h — freestanding shim for the vendored trees. Of these, the compiled
 * set actually reaches calloc and free (mbedTLS's allocator) and atoi
 * (lwip/src/core/netif.c); malloc, realloc, strtol, abs and rand have no
 * referencing object in the kernel link. They are declared because their
 * definitions still exist in lib/libc_shim.c — the two files are one shim and
 * should be trimmed together, not here alone. */
#ifndef PORT_STDLIB_H
#define PORT_STDLIB_H
#include <stddef.h>

void *malloc(size_t n);
void  free(void *p);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
int   atoi(const char *s);
long  strtol(const char *s, char **end, int base);
int   abs(int x);
int   rand(void);

#endif /* PORT_STDLIB_H */
