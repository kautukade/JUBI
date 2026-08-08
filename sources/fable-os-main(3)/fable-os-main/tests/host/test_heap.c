/* test_heap.c — exercises the real mm/heap.c on the host.
 *
 * This is the reference example for the host harness: it compiles an unmodified
 * kernel source file against kshim.c and asserts real behaviour. Copy this shape
 * for new pure-logic modules (JSON parsing, trace formatting, the driver VM).
 */

#include "harness.h"
#include "heap.h"

static void test_alloc_roundtrip(void) {
    void *p = kmalloc(64);
    CHECK(p != NULL);
    memset(p, 0xAB, 64);
    CHECK_EQ(((unsigned char *)p)[63], 0xAB);
    kfree(p);
}

static void test_alignment(void) {
    /* kmalloc promises 16-byte alignment (enough for any scalar / SSE type). */
    for (size_t n = 1; n <= 256; n <<= 1) {
        void *p = kmalloc(n);
        CHECK(p != NULL);
        CHECK_EQ((uintptr_t)p % 16, 0);
        kfree(p);
    }
}

static void test_free_null_is_safe(void) {
    kfree(NULL);           /* documented as accepted */
    CHECK(1);
}

static void test_realloc_semantics(void) {
    /* krealloc(NULL, n) == kmalloc(n) */
    void *p = krealloc(NULL, 32);
    CHECK(p != NULL);

    memset(p, 0x5A, 32);
    void *q = krealloc(p, 128);
    CHECK(q != NULL);
    /* contents must survive the grow */
    for (int i = 0; i < 32; i++) CHECK_EQ(((unsigned char *)q)[i], 0x5A);

    /* krealloc(p, 0) frees and returns NULL */
    CHECK(krealloc(q, 0) == NULL);
}

static void test_kcalloc_zeroes(void) {
    unsigned char *p = kcalloc(16, 8);
    CHECK(p != NULL);
    for (int i = 0; i < 16 * 8; i++) CHECK_EQ(p[i], 0);
    kfree(p);
}

static void test_kmemalign(void) {
    for (size_t a = 16; a <= 4096; a <<= 1) {
        void *p = kmemalign(a, 100);
        CHECK(p != NULL);
        CHECK_EQ((uintptr_t)p % a, 0);
        kfree(p);
    }
}

/* test_kmemalign above passed for years while kmemalign() was returning
 * misaligned memory, because whether the bug fired depended on the arena's
 * address modulo the requested alignment — one static added anywhere could flip
 * it. `front_gap == 0` was being used as a proxy for "the payload is already
 * aligned", which is only true in one of the two cases that produce a zero gap.
 *
 * So do not ask once. Walk the residue: a spacer allocation shifts the first
 * free payload by 16 bytes each round, which sweeps every possible pl % align. */
static void test_kmemalign_at_every_residue(void) {
    for (size_t a = 32; a <= 1024; a <<= 1) {
        for (int shift = 0; shift < 24; shift++) {
            void *spacer = kmalloc(16 + (size_t)shift * 16);
            void *p      = kmemalign(a, 64);
            CHECK(p != NULL);
            CHECK_EQ((uintptr_t)p % a, 0);
            /* usable for its whole length, and the heap survives it */
            memset(p, 0xAB, 64);
            CHECK_EQ(heap_check(), 0);
            kfree(p);
            kfree(spacer);
        }
    }
    /* Same sweep with varying sizes, since the split path differs. */
    for (size_t a = 64; a <= 256; a <<= 1) {
        for (int i = 0; i < 120; i++) {
            size_t sz     = (size_t)(i * 13 % 900) + 1;
            void  *spacer = kmalloc((size_t)(i * 7 % 512) + 1);
            void  *p      = kmemalign(a, sz);
            CHECK(p != NULL);
            CHECK_EQ((uintptr_t)p % a, 0);
            memset(p, 0x5A, sz);
            kfree(p);
            kfree(spacer);
        }
    }
    CHECK_EQ(heap_check(), 0);
}

/* The property that matters most for the TLS path: thousands of alloc/free
 * cycles must coalesce back down and leave the heap structurally intact. */
static void test_coalescing_reclaims(void) {
    heap_stats_t before, after;
    heap_stats(&before);

    for (int round = 0; round < 200; round++) {
        void *ps[32];
        for (int i = 0; i < 32; i++) {
            ps[i] = kmalloc(64 + (i * 37) % 512);
            CHECK(ps[i] != NULL);
        }
        for (int i = 0; i < 32; i++) kfree(ps[i]);
    }

    CHECK_EQ(heap_check(), 0);          /* 0 = intact */
    heap_stats(&after);

    /* Everything allocated in this test was freed, so in-use must return to the
     * starting level — that is the whole point of a coalescing free list. */
    CHECK_EQ(after.bytes_in_use, before.bytes_in_use);
    CHECK(after.total_frees > 6000);
    CHECK(after.peak_in_use >= before.bytes_in_use);
}

static void test_stats_are_consistent(void) {
    heap_stats_t st;
    void *p = kmalloc(1024);
    heap_stats(&st);
    CHECK(st.arena_size > 0);
    CHECK(st.bytes_in_use >= 1024);
    CHECK(st.used_blocks >= 1);
    CHECK(st.bytes_in_use + st.bytes_free + st.overhead <= st.arena_size);
    kfree(p);
}

static void test_heap_check_on_clean_heap(void) {
    CHECK_EQ(heap_check(), 0);
    CHECK_EQ(kpanic_hit, 0);      /* a clean heap must never panic */
}

int main(void) {
    console_init();
    RUN(test_alloc_roundtrip);
    RUN(test_alignment);
    RUN(test_free_null_is_safe);
    RUN(test_realloc_semantics);
    RUN(test_kcalloc_zeroes);
    RUN(test_kmemalign);
    RUN(test_kmemalign_at_every_residue);
    RUN(test_coalescing_reclaims);
    RUN(test_stats_are_consistent);
    RUN(test_heap_check_on_clean_heap);
    return th_report("heap");
}
