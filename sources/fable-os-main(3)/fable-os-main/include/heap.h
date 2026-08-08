/* heap.h — kernel heap allocator interface.
 *
 * PURPOSE
 *   Provides dynamic memory (kmalloc/kfree/krealloc) for the kernel and for
 *   the vendored libraries (mbedTLS, lwIP) that reach it through the libc shim.
 *
 * RESPONSIBILITIES
 *   - Hand out and reclaim variable-sized blocks from a managed arena.
 *   - Coalesce adjacent free blocks to fight fragmentation.
 *   - Detect heap corruption (header magic) and use-after-free (poisoning).
 *   - Expose allocation statistics for debugging and future tuning.
 *
 * PUBLIC API
 *   kmalloc / kfree / krealloc / kcalloc  — the standard allocation surface.
 *   kmemalign                             — power-of-two aligned allocation.
 *   heap_stats / heap_dump / heap_check   — introspection & debugging hooks.
 *
 * DEPENDENCIES
 *   kernel.h (panic, kprintf, memset/memcpy). No hardware dependencies — the
 *   backing store is a static arena today, so the allocator is available from
 *   the very first instruction of kernel_main without any bring-up.
 *
 * FUTURE EXTENSION POINTS
 *   The whole implementation sits behind this header, so a different allocator
 *   (slab, buddy, per-CPU caches) can replace mm/heap.c without touching any
 *   caller. When a physical memory manager exists, heap_init() can be handed a
 *   region carved from it instead of the compiled-in arena.
 */
#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

/* Optional: install a backing region for the heap. Called automatically on the
 * first allocation if not called explicitly, so ordering can never brick boot.
 * A future physical memory manager can call this with a region it owns. */
void  heap_init(void *base, size_t size);

/* Core allocation surface. kmalloc returns 16-byte aligned memory (enough for
 * any scalar / SSE type); kfree accepts NULL; krealloc(NULL, n) == kmalloc(n)
 * and krealloc(p, 0) frees p and returns NULL. */
void *kmalloc(size_t n);
void  kfree(void *p);
void *krealloc(void *p, size_t n);
void *kcalloc(size_t count, size_t size);

/* Aligned allocation. `align` must be a power of two >= 16. Freed with kfree(),
 * like any other block. Useful for DMA rings and page-aligned buffers later. */
void *kmemalign(size_t align, size_t n);

/* Allocation statistics — a snapshot, cheap to take. */
typedef struct heap_stats {
    size_t   arena_size;    /* total bytes managed */
    size_t   bytes_in_use;  /* payload currently allocated */
    size_t   bytes_free;    /* payload currently free */
    size_t   overhead;      /* bytes consumed by block headers */
    size_t   peak_in_use;   /* high-water mark of bytes_in_use */
    size_t   free_blocks;   /* number of free blocks (fragmentation proxy) */
    size_t   used_blocks;   /* number of live allocations */
    uint64_t total_allocs;  /* lifetime kmalloc count. A krealloc served in
                             * place (grown or shrunk) touches neither counter;
                             * only the copy path, which calls kmalloc, does. */
    uint64_t total_frees;   /* lifetime kfree count */
} heap_stats_t;

void heap_stats(heap_stats_t *out);
void heap_dump(void);        /* pretty-print stats via kprintf */
int  heap_check(void);       /* walk the heap; 0 = intact, panics on corruption */

#endif /* HEAP_H */
