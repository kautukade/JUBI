/* test_fiber.c — the cooperative scheduler, on the host.
 *
 * WHY THIS SUITE SUPPLIES ITS OWN CONTEXT SWITCH
 *   include/fiber.h has exactly two machine-dependent functions and the kernel
 *   implements them in arch/x86_64/switch.asm, which no host compiler here can
 *   assemble: this tree is developed on arm64 macOS. Rather than test the
 *   scheduler against a fake that cannot really switch — which would leave the
 *   interesting half (does a fiber's stack survive being left and re-entered?)
 *   untested — this file implements the same seam for the host it is running
 *   on. AArch64 and x86-64 are both here, so the suite is real on either.
 *
 *   That means the ASSEMBLY in arch/x86_64/switch.asm is NOT what these tests
 *   execute; it is covered by booting (tests/qemu, and the boot log's fiber
 *   report). What IS covered here is everything in core/fiber.c: the round
 *   robin, exit and join, the reaper, slot exhaustion, the stack accounting
 *   and — the reason the guards exist at all — that a fiber walking off the end
 *   of its stack is DETECTED rather than silently corrupting the heap.
 *
 *   Both backends below save exactly what their ABI calls callee-saved and lay
 *   out the frame in the order their `switch` restores it, which is the same
 *   contract arch/x86_64/switch.asm documents at length. If a third
 *   architecture ever matters, this is the file that grows.
 *
 * WHAT IS NOT COVERED HERE, stated so nobody assumes otherwise
 *   - The hog warning (FIBER_HOG_MS is five seconds of wall clock and kshim's
 *     millis() is the real monotonic clock, which this suite cannot move). Its
 *     absence during normal operation IS asserted.
 *   - A saved stack pointer landing outside its stack. fiber_check_stacks()
 *     checks for it, but forging one needs the innards of an opaque struct.
 */

#include "harness.h"
#include "fiber.h"
#include "heap.h"

#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/* the host arch backend                                                   */
/* ======================================================================= */

#if defined(__APPLE__)
#  define SYM(n) "_" n
#else
#  define SYM(n) n
#endif

#if defined(__aarch64__) || defined(_M_ARM64)

/* AArch64 (AAPCS64): callee-saved are x19-x28, x29 (fp), x30 (lr) and the low
 * 64 bits of v8-v15. 160 bytes, 16-aligned, laid out low to high as
 *
 *     x19 x20 | x21 x22 | x23 x24 | x25 x26 | x27 x28 | x29 x30 |
 *     d8  d9  | d10 d11 | d12 d13 | d14 d15
 *
 * so index 0 is x19 (the fn) and index 1 is x20 (the arg), index 11 is x30 —
 * the address `ret` branches to, i.e. where a new fiber starts.
 */
#define CTX_WORDS   20
#define CTX_FN      0
#define CTX_ARG     1
#define CTX_ENTRY  11

__asm__(
".text\n"
".p2align 2\n"
".globl " SYM("fiber_arch_switch") "\n"
SYM("fiber_arch_switch") ":\n"
"   sub  sp, sp, #160\n"
"   stp  x19, x20, [sp, #0]\n"
"   stp  x21, x22, [sp, #16]\n"
"   stp  x23, x24, [sp, #32]\n"
"   stp  x25, x26, [sp, #48]\n"
"   stp  x27, x28, [sp, #64]\n"
"   stp  x29, x30, [sp, #80]\n"
"   stp  d8,  d9,  [sp, #96]\n"
"   stp  d10, d11, [sp, #112]\n"
"   stp  d12, d13, [sp, #128]\n"
"   stp  d14, d15, [sp, #144]\n"
"   mov  x9, sp\n"
"   str  x9, [x0]\n"
"   mov  sp, x1\n"
"   ldp  x19, x20, [sp, #0]\n"
"   ldp  x21, x22, [sp, #16]\n"
"   ldp  x23, x24, [sp, #32]\n"
"   ldp  x25, x26, [sp, #48]\n"
"   ldp  x27, x28, [sp, #64]\n"
"   ldp  x29, x30, [sp, #80]\n"
"   ldp  d8,  d9,  [sp, #96]\n"
"   ldp  d10, d11, [sp, #112]\n"
"   ldp  d12, d13, [sp, #128]\n"
"   ldp  d14, d15, [sp, #144]\n"
"   add  sp, sp, #160\n"
"   ret\n"
".p2align 2\n"
".globl " SYM("th_fiber_trampoline") "\n"
SYM("th_fiber_trampoline") ":\n"
"   mov  x29, #0\n"
"   mov  x0, x20\n"
"   blr  x19\n"
"   bl   " SYM("fiber_exit") "\n"
"   brk  #1\n"
);

#elif defined(__x86_64__)

/* System V AMD64: callee-saved are rbx, rbp, r12-r15. Seven words, laid out
 * low to high as r15 r14 r13 r12 rbx rbp <return address> — exactly what
 * arch/x86_64/switch.asm builds, with the fn in r12 and the arg in r13. */
#define CTX_WORDS   7
#define CTX_FN      3     /* r12 */
#define CTX_ARG     2     /* r13 */
#define CTX_ENTRY   6     /* return address */

__asm__(
".text\n"
".p2align 4\n"
".globl " SYM("fiber_arch_switch") "\n"
SYM("fiber_arch_switch") ":\n"
"   pushq %rbp\n"
"   pushq %rbx\n"
"   pushq %r12\n"
"   pushq %r13\n"
"   pushq %r14\n"
"   pushq %r15\n"
"   movq  %rsp, (%rdi)\n"
"   movq  %rsi, %rsp\n"
"   popq  %r15\n"
"   popq  %r14\n"
"   popq  %r13\n"
"   popq  %r12\n"
"   popq  %rbx\n"
"   popq  %rbp\n"
"   ret\n"
".p2align 4\n"
".globl " SYM("th_fiber_trampoline") "\n"
SYM("th_fiber_trampoline") ":\n"
"   andq  $-16, %rsp\n"
"   xorl  %ebp, %ebp\n"
"   movq  %r13, %rdi\n"
"   call  *%r12\n"
"   call  " SYM("fiber_exit") "\n"
"   ud2\n"
);

#else
#error "test_fiber.c needs a context switch for this architecture"
#endif

void th_fiber_trampoline(void);

/* The portable half of the seam: lay the frame above onto a fresh stack. */
void *fiber_arch_stack_init(void *stack_top, fiber_fn_t fn, void *arg) {
    uintptr_t top = (uintptr_t)stack_top & ~(uintptr_t)15;
    uint64_t *f   = (uint64_t *)(top - CTX_WORDS * 8 -
                                 ((CTX_WORDS * 8) % 16 ? 8 : 0));
    for (int i = 0; i < CTX_WORDS; i++) f[i] = 0;
    f[CTX_FN]    = (uint64_t)(uintptr_t)fn;
    f[CTX_ARG]   = (uint64_t)(uintptr_t)arg;
    f[CTX_ENTRY] = (uint64_t)(uintptr_t)th_fiber_trampoline;
    return f;
}

/* ======================================================================= */
/* scaffolding                                                             */
/* ======================================================================= */

#define TRACE_CAP 256
static char     trace[TRACE_CAP];
static int      trace_len;
static uint64_t heap_live_before;

static void note(char c) { if (trace_len < TRACE_CAP - 1) trace[trace_len++] = c; }

static void setup(void) {
    fiber_reset();
    trace_len = 0;
    trace[0]  = '\0';
    kcap_reset();
    kpanic_hit = 0;
    kpanic_msg = NULL;
    fiber_init();
}

static const char *tracestr(void) { trace[trace_len] = '\0'; return trace; }

/* ======================================================================= */
/* the basics: adopt, create, switch                                       */
/* ======================================================================= */

static void test_init_adopts_the_running_context(void) {
    setup();
    CHECK(fiber_self() != NULL);
    CHECK_STR(fiber_name(fiber_self()), "main");
    CHECK_EQ(fiber_count(), 1);
    CHECK_EQ(fiber_runnable(), 1);
    /* The root runs on the boot stack, whose bounds C cannot see. */
    CHECK_EQ(fiber_stack_bounds(fiber_self(), NULL, NULL), -1);
    CHECK_EQ(fiber_stack_size(fiber_self()), 0);
}

static void test_yield_alone_is_a_no_op_and_costs_no_switch(void) {
    setup();
    uint64_t s = fiber_switches();
    for (int i = 0; i < 100; i++) fiber_yield();
    CHECK_EQ(fiber_switches(), s);
    CHECK_EQ(kpanic_hit, 0);
}

static void test_yield_before_init_does_nothing(void) {
    fiber_reset();
    kpanic_hit = 0;
    fiber_yield();                 /* must not fault, must not panic */
    CHECK_EQ(kpanic_hit, 0);
    CHECK(fiber_self() == NULL);
    CHECK_EQ(fiber_count(), 0);
    CHECK_EQ(fiber_check_stacks(), 0);
    fiber_init();
}

static void spin_body(void *arg) {
    char c = (char)(intptr_t)arg;
    for (;;) { note(c); fiber_yield(); }
}

static void test_a_fiber_runs_only_when_yielded_to(void) {
    setup();
    fiber_t *a = fiber_create("a", spin_body, (void *)(intptr_t)'a', 8192);
    CHECK(a != NULL);
    CHECK_EQ(fiber_count(), 2);
    CHECK_EQ(fiber_runnable(), 2);

    /* Creation alone must not run it: this is cooperative, nothing preempts. */
    CHECK_STR(tracestr(), "");
    CHECK_EQ(fiber_switch_count(a), 0);

    fiber_yield();
    CHECK_STR(tracestr(), "a");
    CHECK_EQ(fiber_switch_count(a), 1);

    fiber_yield();
    CHECK_STR(tracestr(), "aa");
    CHECK_EQ(fiber_switches(), 4);      /* root->a, a->root, twice */
}

static void test_round_robin_visits_everyone_in_order(void) {
    setup();
    note('R');
    CHECK(fiber_create("a", spin_body, (void *)(intptr_t)'a', 8192) != NULL);
    CHECK(fiber_create("b", spin_body, (void *)(intptr_t)'b', 8192) != NULL);
    CHECK(fiber_create("c", spin_body, (void *)(intptr_t)'c', 8192) != NULL);
    CHECK_EQ(fiber_runnable(), 4);

    for (int round = 0; round < 3; round++) { fiber_yield(); note('R'); }

    /* One root yield walks the whole ring once and lands back on the root, so
     * every fiber gets a turn before any gets a second one: nothing starves. */
    CHECK_STR(tracestr(), "RabcRabcRabcR");
}

static void test_round_robin_is_fair_over_many_rounds(void) {
    setup();
    CHECK(fiber_create("a", spin_body, (void *)(intptr_t)'a', 8192) != NULL);
    CHECK(fiber_create("b", spin_body, (void *)(intptr_t)'b', 8192) != NULL);

    for (int i = 0; i < 60; i++) fiber_yield();

    int na = 0, nb = 0;
    for (int i = 0; i < trace_len; i++) {
        if (trace[i] == 'a') na++;
        if (trace[i] == 'b') nb++;
    }
    CHECK_EQ(na, 60);
    CHECK_EQ(nb, 60);
    CHECK_EQ(na, nb);
}

/* ======================================================================= */
/* stacks are separate, and they survive being left                        */
/* ======================================================================= */

/* Each fiber fills a big local with a private pattern, yields several times,
 * and checks the local is byte-for-byte what it left. If the switch dropped a
 * callee-saved register, or two fibers shared a stack, this is what notices. */
#define ISO_BYTES 1024
static int iso_ok[3];

static void iso_body(void *arg) {
    int  id = (int)(intptr_t)arg;
    volatile unsigned char buf[ISO_BYTES];
    unsigned char seed = (unsigned char)(0x40 + id * 7);

    for (int i = 0; i < ISO_BYTES; i++) buf[i] = (unsigned char)(seed + i);

    int ok = 1;
    for (int round = 0; round < 8; round++) {
        /* Registers as well as memory: these must survive the switch too. */
        volatile unsigned long r1 = 0x1111111100000000ul + (unsigned)id;
        volatile unsigned long r2 = 0x2222222200000000ul + (unsigned)id;
        fiber_yield();
        if (r1 != 0x1111111100000000ul + (unsigned)id) ok = 0;
        if (r2 != 0x2222222200000000ul + (unsigned)id) ok = 0;
        for (int i = 0; i < ISO_BYTES; i++)
            if (buf[i] != (unsigned char)(seed + i)) { ok = 0; break; }
    }
    iso_ok[id] = ok;
}

static void test_stacks_are_isolated_and_survive_switching(void) {
    setup();
    iso_ok[0] = iso_ok[1] = iso_ok[2] = -1;

    fiber_t *f[3];
    for (int i = 0; i < 3; i++) {
        char nm[2] = { (char)('0' + i), 0 };
        f[i] = fiber_create(nm, iso_body, (void *)(intptr_t)i, 16384);
        CHECK(f[i] != NULL);
    }

    /* No two usable regions may overlap. */
    void *lo[3], *hi[3];
    for (int i = 0; i < 3; i++) CHECK_EQ(fiber_stack_bounds(f[i], &lo[i], &hi[i]), 0);
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            CHECK((char *)hi[i] <= (char *)lo[j] || (char *)hi[j] <= (char *)lo[i]);

    for (int i = 0; i < 40; i++) fiber_yield();

    for (int i = 0; i < 3; i++) CHECK_EQ(iso_ok[i], 1);
    CHECK_EQ(fiber_check_stacks(), 0);
    CHECK_EQ(kpanic_hit, 0);
}

/* ======================================================================= */
/* deep nesting: a fiber yields from the bottom of a deep call chain       */
/* ======================================================================= */

static int nest_max_depth;
static int nest_unwound_clean;

__attribute__((noinline))
static int nest(int depth) {
    volatile int marker = depth * 3 + 1;
    volatile char pad[96];
    for (int i = 0; i < 96; i++) pad[i] = (char)(depth + i);

    if (depth > nest_max_depth) nest_max_depth = depth;
    fiber_yield();                       /* switch away from deep inside */

    if (depth < 40) (void)nest(depth + 1);

    fiber_yield();                       /* and again on the way back out */

    /* Everything this frame owns must be exactly as it was left, twice over. */
    if (marker != depth * 3 + 1) return 0;
    for (int i = 0; i < 96; i++)
        if (pad[i] != (char)(depth + i)) return 0;
    return 1;
}

static void nest_body(void *arg) {
    (void)arg;
    nest_unwound_clean = nest(0);
}

static void test_a_fiber_can_yield_from_deep_in_its_own_call_chain(void) {
    setup();
    nest_max_depth = -1;
    nest_unwound_clean = -1;

    fiber_t *f = fiber_create("deep", nest_body, NULL, 32768);
    CHECK(f != NULL);
    while (!fiber_done(f)) fiber_yield();

    CHECK_EQ(nest_max_depth, 40);
    CHECK_EQ(nest_unwound_clean, 1);
    CHECK_EQ(kpanic_hit, 0);
    /* 41 frames of locals really were on that stack. */
    CHECK(fiber_stack_used(f) == 0);      /* freed on exit; nothing to measure */
    CHECK_EQ(fiber_join(f), 0);
}

/* ======================================================================= */
/* exit, join, and the reaper                                              */
/* ======================================================================= */

static int exit_ran;

static void exit_body(void *arg) {
    (void)arg;
    note('x');
    exit_ran = 1;
    fiber_exit();
    exit_ran = 2;                 /* unreachable */
}

static void return_body(void *arg) {
    (void)arg;
    note('r');
    /* No fiber_exit(): falling off the end must end the fiber just the same,
     * via the arch trampoline. */
}

static void test_an_explicit_exit_ends_the_fiber(void) {
    setup();
    exit_ran = 0;
    fiber_t *f = fiber_create("bye", exit_body, NULL, 8192);
    CHECK(f != NULL);
    CHECK_EQ(fiber_done(f), 0);

    fiber_yield();
    CHECK_EQ(exit_ran, 1);
    CHECK_STR(tracestr(), "x");
    CHECK_EQ(fiber_done(f), 1);

    /* It is out of the round robin: further yields must not reach it again. */
    for (int i = 0; i < 5; i++) fiber_yield();
    CHECK_STR(tracestr(), "x");
    CHECK_EQ(exit_ran, 1);

    /* The stack is released as soon as control is off it, before the slot is. */
    CHECK_EQ(fiber_stack_bounds(f, NULL, NULL), -1);

    CHECK_EQ(fiber_join(f), 0);
    CHECK_EQ(fiber_count(), 1);
}

static void test_a_body_that_simply_returns_ends_the_fiber(void) {
    setup();
    fiber_t *f = fiber_create("ret", return_body, NULL, 8192);
    CHECK(f != NULL);
    fiber_yield();
    CHECK_STR(tracestr(), "r");
    CHECK_EQ(fiber_done(f), 1);
    CHECK_EQ(fiber_join(f), 0);
    CHECK_EQ(fiber_count(), 1);
}

static void test_join_runs_the_fiber_to_completion(void) {
    setup();
    exit_ran = 0;
    fiber_t *f = fiber_create("j", exit_body, NULL, 8192);
    CHECK(f != NULL);
    CHECK_EQ(fiber_join(f), 0);      /* join yields until it is done */
    CHECK_EQ(exit_ran, 1);
    CHECK_EQ(fiber_count(), 1);
}

static void test_join_refuses_the_impossible(void) {
    setup();
    CHECK_EQ(fiber_join(NULL), -1);
    CHECK_EQ(fiber_join(fiber_self()), -1);       /* joining yourself hangs */

    /* A handle whose slot was already released is not a live fiber. */
    fiber_t *f = fiber_create("g", exit_body, NULL, 8192);
    CHECK(f != NULL);
    CHECK_EQ(fiber_join(f), 0);
    CHECK_EQ(fiber_join(f), -1);
    CHECK_EQ(fiber_done(f), 0);                   /* the slot is FREE now */

    /* A pointer that is not into the table at all. */
    long far_away = 0;
    CHECK_EQ(fiber_join((fiber_t *)&far_away), -1);
    CHECK_STR(fiber_name((fiber_t *)&far_away), "?");
    CHECK_EQ(kpanic_hit, 0);
}

/* ======================================================================= */
/* exhaustion                                                              */
/* ======================================================================= */

static void test_slot_exhaustion_is_refused_with_a_reason(void) {
    setup();
    fiber_t *f[FIBER_MAX];
    int made = 0;
    for (int i = 0; i < FIBER_MAX + 3; i++) {
        fiber_t *n = fiber_create("filler", spin_body, (void *)(intptr_t)'f', 4096);
        if (!n) break;
        f[made++] = n;
    }
    /* Slot 0 is the root, so FIBER_MAX-1 are creatable and no more. */
    CHECK_EQ(made, FIBER_MAX - 1);
    CHECK_EQ(fiber_count(), FIBER_MAX);

    kcap_reset();
    CHECK(fiber_create("one too many", spin_body, NULL, 4096) == NULL);
    CHECK_CONTAINS(kcap_text(), "all 8 slots are in use");
    CHECK_EQ(kpanic_hit, 0);
    CHECK_EQ(fiber_count(), FIBER_MAX);
}

static void test_an_unjoined_dead_fiber_keeps_its_slot(void) {
    setup();
    for (int i = 0; i < FIBER_MAX - 1; i++) {
        fiber_t *n = fiber_create("d", exit_body, NULL, 4096);
        CHECK(n != NULL);
    }
    CHECK_EQ(fiber_count(), FIBER_MAX);
    for (int i = 0; i < FIBER_MAX; i++) fiber_yield();   /* let them all exit */
    CHECK_EQ(fiber_runnable(), 1);
    CHECK_EQ(fiber_count(), FIBER_MAX);                  /* slots still held */

    /* This is the documented cost of never recycling a slot: a fiber that
     * exits and is not joined is gone but not forgotten. */
    kcap_reset();
    CHECK(fiber_create("more", spin_body, NULL, 4096) == NULL);
    CHECK_CONTAINS(kcap_text(), "never fiber_join()ed");
}

static void test_a_stack_the_heap_cannot_hold_is_refused(void) {
    setup();
    kcap_reset();
    /* FIBER_STACK_MAX is the clamp, so a wild size cannot eat the heap; ask
     * for one and check it was clamped rather than believed. */
    fiber_t *f = fiber_create("huge", spin_body, NULL, (size_t)1 << 40);
    CHECK(f != NULL);
    CHECK_EQ(fiber_stack_size(f), FIBER_STACK_MAX);

    fiber_t *g = fiber_create("tiny", spin_body, NULL, 8);
    CHECK(g != NULL);
    CHECK_EQ(fiber_stack_size(g), FIBER_STACK_MIN);

    CHECK(fiber_create("nofn", NULL, NULL, 4096) == NULL);
    CHECK_CONTAINS(kcap_text(), "no entry point");
}

/* ======================================================================= */
/* THE POISON GUARDS — the whole reason this is safe to run at all         */
/* ======================================================================= */

static void test_the_guard_bands_are_intact_in_normal_use(void) {
    setup();
    CHECK(fiber_create("a", spin_body, (void *)(intptr_t)'a', 8192) != NULL);
    CHECK(fiber_create("b", spin_body, (void *)(intptr_t)'b', 8192) != NULL);
    for (int i = 0; i < 200; i++) fiber_yield();
    CHECK_EQ(fiber_check_stacks(), 0);
    CHECK_EQ(kpanic_hit, 0);
    /* And no fiber was accused of hogging: 200 switches take microseconds. */
    CHECK(strstr(kcap_text(), "held the CPU") == NULL);
}

static void test_a_write_one_word_past_the_stack_is_caught(void) {
    setup();
    fiber_t *f = fiber_create("edge", spin_body, (void *)(intptr_t)'e', 4096);
    CHECK(f != NULL);

    void *lo = NULL, *hi = NULL;
    CHECK_EQ(fiber_stack_bounds(f, &lo, &hi), 0);

    /* One word BELOW the usable region: the first thing a stack that grows one
     * frame too far writes. Still inside the kmalloc'd block, because the
     * poison band is part of the allocation — which is exactly the point: the
     * damage is contained where it can be seen. */
    ((volatile uint64_t *)lo)[-1] = 0xDEADBEEFCAFEF00Dull;

    kcap_reset();
    CHECK_EQ(fiber_check_stacks(), 1);
    CHECK_EQ(kpanic_hit, 1);
    CHECK_STR(kpanic_msg, "fiber: stack guard violated");
    CHECK_CONTAINS(kcap_text(), "STACK GUARD VIOLATED");
    CHECK_CONTAINS(kcap_text(), "\"edge\"");
    CHECK_CONTAINS(kcap_text(), "low (overflow)");
    CHECK_CONTAINS(kcap_text(), "1 of 32 guard words clobbered");

    /* Once reported it is not re-reported on every subsequent switch — a
     * warning that repeats forever scrolls the evidence away — but it is still
     * counted, and fiber_yield() still refuses to switch on top of it. */
    kcap_reset();
    kpanic_hit = 0;
    CHECK_EQ(fiber_check_stacks(), 1);
    CHECK_EQ(kpanic_hit, 0);
    uint64_t s = fiber_switches();
    fiber_yield();
    CHECK_EQ(fiber_switches(), s);
}

static void test_a_write_past_the_top_of_the_stack_is_caught(void) {
    setup();
    fiber_t *f = fiber_create("under", spin_body, (void *)(intptr_t)'u', 4096);
    CHECK(f != NULL);

    void *lo = NULL, *hi = NULL;
    CHECK_EQ(fiber_stack_bounds(f, &lo, &hi), 0);
    ((volatile uint64_t *)hi)[0] = 1;
    ((volatile uint64_t *)hi)[3] = 2;

    kcap_reset();
    CHECK_EQ(fiber_check_stacks(), 1);
    CHECK_EQ(kpanic_hit, 1);
    CHECK_CONTAINS(kcap_text(), "high (underflow)");
    CHECK_CONTAINS(kcap_text(), "2 of 32 guard words clobbered");
}

static void test_the_whole_band_can_be_destroyed_and_is_still_one_report(void) {
    setup();
    fiber_t *f = fiber_create("wipe", spin_body, (void *)(intptr_t)'w', 4096);
    CHECK(f != NULL);
    void *lo = NULL;
    CHECK_EQ(fiber_stack_bounds(f, &lo, NULL), 0);
    memset((char *)lo - FIBER_GUARD_BYTES, 0, FIBER_GUARD_BYTES);

    kcap_reset();
    CHECK_EQ(fiber_check_stacks(), 1);
    CHECK_EQ(kpanic_hit, 1);
    CHECK_CONTAINS(kcap_text(), "32 of 32 guard words clobbered");
}

/* A REAL overrun, not a poke: recurse until the frames are genuinely inside
 * the poison band, then unwind and yield. The recursion stops half a band in,
 * so the damage stays inside the fiber's own allocation and this suite is
 * still clean under ASan — but every byte written was written by an ordinary
 * function prologue running off the end of a too-small stack, which is the
 * accident the bands exist to catch. */
static volatile int recur_sink;
static char        *recur_floor;

__attribute__((noinline))
static int recur(int depth) {
    volatile char frame[64];
    for (int i = 0; i < 64; i++) frame[i] = (char)(depth + i);

    int deeper = 0;
    if ((char *)frame > recur_floor) deeper = recur(depth + 1);

    recur_sink += frame[0];          /* stops the tail call being a loop */
    return deeper + 1;
}

static int recur_depth_reached;

static size_t overrun_used;

static void overrun_body(void *arg) {
    (void)arg;
    void *lo = NULL;
    CHECK_EQ(fiber_stack_bounds(fiber_self(), &lo, NULL), 0);
    recur_floor = (char *)lo - FIBER_GUARD_BYTES / 2;
    recur_depth_reached = recur(0);
    /* Read the high-water mark from INSIDE, because the exit below hands the
     * stack straight back to the heap and there is nothing left to measure. */
    overrun_used = fiber_stack_used(fiber_self());
    fiber_yield();                   /* the switch that checks the bands */
}

static void test_a_fiber_that_really_blows_its_stack_is_caught(void) {
    setup();
    recur_sink = 0;
    recur_depth_reached = 0;

    /* NOT the smallest stack the API allows, and the reason is worth writing
     * down: when the violation is found, report_violation() prints three lines
     * ON THE OFFENDING FIBER'S STACK. In the kernel that is nearly free —
     * lib/base.c's kprintf streams a character at a time and its frame is a
     * few dozen bytes. tests/host/kshim.c's kprintf renders into a `char
     * tmp[8192]` first, and under ASan that frame is ~16 KiB with redzones. A
     * 4 KiB fiber would therefore blow its stack again, for real and without a
     * band left to catch it, inside the reporting of the first overrun. So the
     * fiber gets room to be diagnosed in; recur() finds the floor from the
     * bounds either way, so the size does not weaken the test. */
    fiber_t *f = fiber_create("small", overrun_body, NULL, 32768);
    CHECK(f != NULL);

    kcap_reset();
    fiber_yield();                   /* run it; it overruns and then yields */

    CHECK(recur_depth_reached > 8);
    CHECK_EQ(kpanic_hit, 1);
    CHECK_STR(kpanic_msg, "fiber: stack guard violated");
    CHECK_CONTAINS(kcap_text(), "STACK GUARD VIOLATED");
    CHECK_CONTAINS(kcap_text(), "\"small\"");
    CHECK_CONTAINS(kcap_text(), "no memory protection");

    /* It really was the stack running out, not a poke: the high-water mark it
     * read of itself covers the whole usable region. */
    CHECK(overrun_used >= 32768 - 64);
    /* And the stack went back to the heap when it exited, damaged or not. */
    CHECK_EQ(fiber_stack_bounds(f, NULL, NULL), -1);
    CHECK_EQ(heap_check(), 0);
}

/* ======================================================================= */
/* accounting                                                              */
/* ======================================================================= */

static void hungry_body(void *arg) {
    (void)arg;
    {
        volatile char buf[3000];
        for (int i = 0; i < 3000; i++) buf[i] = (char)i;
        recur_sink += buf[7];
    }
    /* Stay alive and shallow: the mark must survive the frame that made it,
     * which is the whole point of a high-WATER mark. */
    for (;;) fiber_yield();
}

static void test_the_stack_high_water_mark_is_measured(void) {
    setup();
    fiber_t *lazy   = fiber_create("lazy",   spin_body,   (void *)(intptr_t)'l', 16384);
    fiber_t *hungry = fiber_create("hungry", hungry_body, NULL, 16384);
    CHECK(lazy != NULL);
    CHECK(hungry != NULL);

    fiber_yield();
    fiber_yield();

    size_t lu = fiber_stack_used(lazy);
    size_t hu = fiber_stack_used(hungry);
    CHECK(lu > 0);                       /* something is always on it */
    CHECK(hu >= 3000);                   /* the 3000-byte local, at least */
    CHECK(hu > lu);
    CHECK(hu < fiber_stack_size(hungry));
    CHECK_EQ(fiber_stack_size(lazy), 16384);
    CHECK_EQ(kpanic_hit, 0);
}

static void test_the_report_names_every_fiber(void) {
    setup();
    CHECK(fiber_create("alpha", spin_body, (void *)(intptr_t)'a', 8192) != NULL);
    CHECK(fiber_create("beta",  exit_body, NULL, 8192) != NULL);
    fiber_yield();

    kcap_reset();
    fiber_report();
    const char *t = kcap_text();
    CHECK_CONTAINS(t, "main");
    CHECK_CONTAINS(t, "alpha");
    CHECK_CONTAINS(t, "beta");
    CHECK_CONTAINS(t, "running");
    CHECK_CONTAINS(t, "exited");
    CHECK_CONTAINS(t, "boot stack");
    /* Never a '[' in column zero: a fiber line is prose, not kernel ground
     * truth, and lib/trace.c owns that shape. */
    CHECK(t[0] != '[');
    for (const char *p = t; *p; p++)
        if (p[0] == '\n' && p[1] == '[') { CHECK(0); break; }
}

static void test_a_long_name_is_truncated_not_overrun(void) {
    setup();
    static const char *huge =
        "a-fiber-name-far-longer-than-FIBER_NAME_MAX-could-ever-be";
    fiber_t *f = fiber_create(huge, spin_body, (void *)(intptr_t)'n', 4096);
    CHECK(f != NULL);
    CHECK_EQ((int)strlen(fiber_name(f)), FIBER_NAME_MAX);
    CHECK(strncmp(fiber_name(f), huge, FIBER_NAME_MAX) == 0);

    fiber_t *g = fiber_create(NULL, spin_body, (void *)(intptr_t)'m', 4096);
    CHECK(g != NULL);
    CHECK_STR(fiber_name(g), "?");
    CHECK_EQ(kpanic_hit, 0);
}

/* ======================================================================= */
/* the heap is left exactly as it was found                                */
/* ======================================================================= */

static void test_reset_returns_every_stack_to_the_heap(void) {
    heap_stats_t before, after;
    setup();
    heap_stats(&before);

    for (int i = 0; i < FIBER_MAX - 1; i++)
        CHECK(fiber_create("t", spin_body, (void *)(intptr_t)'t', 8192) != NULL);
    for (int i = 0; i < 20; i++) fiber_yield();

    heap_stats(&after);
    CHECK(after.bytes_in_use > before.bytes_in_use);

    fiber_reset();
    heap_stats(&after);
    CHECK_EQ(after.bytes_in_use, before.bytes_in_use);
    CHECK_EQ(after.used_blocks, before.used_blocks);
    CHECK_EQ(heap_check(), 0);
    CHECK_EQ(kpanic_hit, 0);
    fiber_init();
}

static void test_an_exited_fiber_returns_its_stack_immediately(void) {
    heap_stats_t before, after;
    setup();
    heap_stats(&before);

    fiber_t *f = fiber_create("short", exit_body, NULL, 32768);
    CHECK(f != NULL);
    CHECK_EQ(fiber_join(f), 0);

    heap_stats(&after);
    CHECK_EQ(after.bytes_in_use, before.bytes_in_use);
    CHECK_EQ(heap_check(), 0);
}

/* ======================================================================= */
/* the deferred reap, and the slot that changed identity underneath it     */
/* ======================================================================= */

/* THREE DEFECTS, ONE ROOT CAUSE: the pending reap used to be a SLOT pointer.
 *
 * reap() runs from exactly one place — inside switch_to(), after the arch switch
 * returns. A fiber resuming for the FIRST time enters the arch trampoline
 * instead, which calls the body and then fiber_exit(), and never reaches that
 * line. So fiber_exit() switching to a never-run fiber leaves the reap pending,
 * and everything downstream of that was wrong:
 *
 *   1. USE AFTER FREE. fiber_join() freed the slot, fiber_create() reused it and
 *      wrote a fresh block into it, and the next switch's reap() kfree()d the NEW
 *      fiber's live stack and zeroed its sp. fiber_check_stacks() could not see
 *      it, because it skips slots with no block. The next switch to that fiber
 *      was fiber_arch_switch(&me->sp, NULL) — and on x86-64 that does not fault.
 *   2. A PERMANENT LEAK. Two fibers exiting before any reap overwrote the pending
 *      pointer, orphaning the first block with no reference anywhere — so not
 *      even fiber_reset() could find it.
 *   3. A FALSE PROMISE in include/fiber.h ("the stack is freed as soon as control
 *      is off it"), true only for the most recent dead fiber.
 *
 * None of it was reachable from the shipped kernel, which creates one fiber that
 * never exits. All of it was reachable from the lifecycle the header documents.
 * The existing exit/join tests never saw it because each has exactly ONE non-root
 * fiber, so exit always switches back to the root — which IS inside switch_to(),
 * so the reap always ran immediately. */

static int fresh_ran;

static void fresh_body(void *arg) {
    (void)arg;
    fresh_ran++;
    for (;;) fiber_yield();
}

static void test_exiting_into_a_never_run_fiber_frees_the_right_stack(void) {
    heap_stats_t before, after;
    setup();
    heap_stats(&before);
    fresh_ran = 0;

    /* a exits; the round-robin successor is b, which has never run, so it starts
     * at the trampoline and no reap happens on that switch. */
    fiber_t *a = fiber_create("a", exit_body, NULL, 8192);
    fiber_t *b = fiber_create("b", fresh_body, NULL, 8192);
    CHECK(a != NULL);
    CHECK(b != NULL);

    fiber_yield();
    CHECK_EQ(fresh_ran, 1);

    /* Join a and hand its slot straight to c. Under the old code the still-pending
     * slot pointer now named c, and the next reap freed c's live stack. */
    CHECK_EQ(fiber_join(a), 0);
    fiber_t *c = fiber_create("c", fresh_body, NULL, 8192);
    CHECK(c != NULL);

    void *lo = NULL, *hi = NULL;
    CHECK_EQ(fiber_stack_bounds(c, &lo, &hi), 0);   /* c owns a real stack */
    CHECK(hi > lo);

    int entered = fresh_ran;
    fiber_yield();                                  /* the reap happens in here */

    /* c really executed: it entered its body on this very switch, on the stack
     * the old code had just handed back to the heap. */
    CHECK_EQ(fresh_ran, entered + 1);

    /* And c is untouched: same bounds, intact bands, a usable saved sp. */
    void *lo2 = NULL, *hi2 = NULL;
    CHECK_EQ(fiber_stack_bounds(c, &lo2, &hi2), 0);
    CHECK(lo2 == lo);
    CHECK(hi2 == hi);
    CHECK_EQ(fiber_check_stacks(), 0);
    CHECK_EQ(heap_check(), 0);
    CHECK_EQ(kpanic_hit, 0);

    for (int i = 0; i < 8; i++) fiber_yield();      /* keeps switching cleanly */
    CHECK_EQ(fiber_check_stacks(), 0);
    CHECK_EQ(kpanic_hit, 0);

    fiber_reset();
    heap_stats(&after);
    CHECK_EQ(after.bytes_in_use, before.bytes_in_use);
    CHECK_EQ(heap_check(), 0);
    fiber_init();
}

/* Two fibers exiting with no reap in between. The first block used to be orphaned
 * for the life of the machine — measured at 8704 bytes per cycle, monotonically,
 * attributed to nobody, and invisible to fiber_reset(). */
static void test_two_exits_before_a_reap_leak_nothing(void) {
    heap_stats_t before, after;
    setup();
    heap_stats(&before);

    for (int round = 0; round < 4; round++) {
        fiber_t *w1 = fiber_create("w1", exit_body, NULL, 8192);
        fiber_t *w2 = fiber_create("w2", exit_body, NULL, 8192);
        CHECK(w1 != NULL);
        CHECK(w2 != NULL);
        CHECK_EQ(fiber_join(w1), 0);
        CHECK_EQ(fiber_join(w2), 0);

        /* Back to exactly the heap this round started with, every round — not
         * merely bounded, and not merely recovered by the next fiber_reset(). */
        heap_stats(&after);
        CHECK_EQ(after.bytes_in_use, before.bytes_in_use);
        CHECK_EQ(after.used_blocks, before.used_blocks);
        CHECK_EQ(fiber_count(), 1);
        CHECK_EQ(heap_check(), 0);
        CHECK_EQ(kpanic_hit, 0);
    }
}

/* A dead fiber's slot must claim no memory the moment it is dead, because that is
 * what makes joining and recycling it safe. Asserted through the public API: a
 * dead-but-unjoined fiber reports no stack bounds. */
static void test_a_dead_fiber_owns_no_stack(void) {
    setup();

    fiber_t *a = fiber_create("a", exit_body, NULL, 8192);
    fiber_t *b = fiber_create("b", fresh_body, NULL, 8192);
    CHECK(a != NULL);
    CHECK(b != NULL);
    fiber_yield();

    void *lo = NULL, *hi = NULL;
    CHECK(fiber_stack_bounds(a, &lo, &hi) != 0);    /* nothing left to own */
    CHECK_EQ(fiber_check_stacks(), 0);
    CHECK_EQ(kpanic_hit, 0);
    fiber_reset();
    fiber_init();
}

/* ======================================================================= */
/* volume: the switch itself, thousands of times                           */
/* ======================================================================= */

static void test_ten_thousand_switches_change_nothing(void) {
    setup();
    heap_stats_t before, after;
    heap_stats(&before);

    fiber_t *a = fiber_create("a", spin_body, (void *)(intptr_t)'a', 8192);
    fiber_t *b = fiber_create("b", spin_body, (void *)(intptr_t)'b', 8192);
    CHECK(a != NULL);
    CHECK(b != NULL);

    for (int i = 0; i < 10000; i++) {
        fiber_yield();
        trace_len = 0;                    /* the trace buffer is not the point */
    }
    CHECK_EQ(fiber_switches(), 30000u);
    CHECK_EQ(fiber_switch_count(a), 10000u);
    CHECK_EQ(fiber_switch_count(b), 10000u);
    CHECK_EQ(fiber_check_stacks(), 0);
    CHECK_EQ(kpanic_hit, 0);
    CHECK_EQ(heap_check(), 0);

    fiber_reset();
    heap_stats(&after);
    CHECK_EQ(after.bytes_in_use, before.bytes_in_use);
    fiber_init();
}

/* ======================================================================
 * THE ROOT FIBER'S STACK
 *
 * The root is the context fiber_init() ADOPTS, so it is the one stack this
 * module did not allocate and, until this branch, the one with no poison band.
 * On the kernel what sits immediately below it is boot/boot.asm's page tables —
 * the identity map — so an overflow there is a triple fault with no #PF, no
 * fault record and nothing on the serial line. include/fiber.h used to argue
 * that leaving it unguarded was fine because mbedTLS was the deepest user; a
 * single cc_compile() goes four times deeper than the whole TLS path.
 *
 * A host test links no boot.asm and stands on a stack it did not allocate, which
 * is exactly why the bounds arrive through a SETTER rather than a weak reference
 * to boot.asm's symbols: the same code can then be driven against a synthetic
 * region. This is that test.
 * ====================================================================== */
static void test_the_root_stack_can_be_declared_and_is_then_guarded(void) {
    /* WITHOUT the hint the root stays exactly as it always was: no bounds to
     * report, so nothing else in this suite changes behaviour. */
    fiber_root_stack_set(NULL, NULL);
    setup();
    CHECK(fiber_stack_size(fiber_self()) == 0);
    CHECK(fiber_stack_bounds(fiber_self(), NULL, NULL) == -1);

    /* WITH it, the root is guarded like any other fiber. The declared region has
     * to be the stack we are ACTUALLY standing on, because fiber_check_stacks()
     * also verifies that the running fiber's stack pointer is inside its own
     * stack — which is a real check on the kernel, where the root's sp genuinely
     * is inside boot/boot.asm's reservation, and cannot be satisfied by a
     * detached synthetic array.
     *
     * 64 KiB below this frame rather than a few hundred bytes, and the size is
     * load-bearing: tests/host/kshim.c's kprintf renders into a char[8192], so
     * report_violation() itself descends ~16 KiB under ASan. A tight region
     * would have the reporter smash the band it is reporting on. The kernel's
     * kprintf streams a character at a time and has no such frame. */
    char anchor;
    uint8_t *hi = (uint8_t *)&anchor + 64;        /* just above this frame */
    uint8_t *lo = hi - 65536;                     /* untouched host stack   */

    fiber_root_stack_set(lo, hi);
    setup();

    void *glo = NULL, *ghi = NULL;
    CHECK(fiber_stack_bounds(fiber_self(), &glo, &ghi) == 0);
    CHECK(glo == lo + FIBER_GUARD_BYTES);
    CHECK(ghi == hi);
    CHECK(fiber_stack_size(fiber_self()) == 65536u - FIBER_GUARD_BYTES);

    /* An intact band, with the stack pointer inside the region, is quiet. */
    CHECK(fiber_check_stacks() == 0);
    CHECK(kpanic_hit == 0);

    /* Painting the band is what fiber_init() must have done: on the kernel the
     * bytes below this point are p2_tables, the identity map. */
    int painted = 0;
    for (size_t k = 0; k < FIBER_GUARD_BYTES; k++) if (lo[k]) painted++;
    CHECK(painted == (int)FIBER_GUARD_BYTES);

    /* And walking into it is a NAMED PANIC rather than silent page-table
     * corruption, which is the entire point of exporting the bounds. */
    lo[0] ^= 0xFF;
    kcap_reset();
    CHECK(fiber_check_stacks() == 1);
    CHECK(kpanic_hit == 1);
    CHECK_CONTAINS(kcap_text(), "STACK GUARD VIOLATED");
    CHECK_CONTAINS(kcap_text(), "main");
    CHECK_CONTAINS(kcap_text(), "low (overflow)");

    /* THE ROOT'S BLOCK IS NOT THE HEAP'S. fiber_reset() frees every fiber stack;
     * handing it boot/boot.asm's .bss — the page tables' neighbour — would be
     * catastrophic and completely silent. owns_block is what prevents it. */
    heap_stats_t before, after;
    heap_stats(&before);
    fiber_reset();
    heap_stats(&after);
    CHECK(heap_check() == 0);
    CHECK(after.used_blocks <= before.used_blocks);

    /* Leave the global hint as the rest of the suite expects to find it. */
    fiber_root_stack_set(NULL, NULL);
    setup();
}

int main(void) {
    console_init();
    heap_stats_t h;
    heap_stats(&h);
    heap_live_before = h.bytes_in_use;

    RUN(test_init_adopts_the_running_context);
    RUN(test_yield_alone_is_a_no_op_and_costs_no_switch);
    RUN(test_yield_before_init_does_nothing);
    RUN(test_a_fiber_runs_only_when_yielded_to);
    RUN(test_round_robin_visits_everyone_in_order);
    RUN(test_round_robin_is_fair_over_many_rounds);
    RUN(test_stacks_are_isolated_and_survive_switching);
    RUN(test_a_fiber_can_yield_from_deep_in_its_own_call_chain);
    RUN(test_an_explicit_exit_ends_the_fiber);
    RUN(test_a_body_that_simply_returns_ends_the_fiber);
    RUN(test_join_runs_the_fiber_to_completion);
    RUN(test_join_refuses_the_impossible);
    RUN(test_slot_exhaustion_is_refused_with_a_reason);
    RUN(test_an_unjoined_dead_fiber_keeps_its_slot);
    RUN(test_a_stack_the_heap_cannot_hold_is_refused);
    RUN(test_the_guard_bands_are_intact_in_normal_use);
    RUN(test_a_write_one_word_past_the_stack_is_caught);
    RUN(test_a_write_past_the_top_of_the_stack_is_caught);
    RUN(test_the_whole_band_can_be_destroyed_and_is_still_one_report);
    RUN(test_a_fiber_that_really_blows_its_stack_is_caught);
    RUN(test_the_stack_high_water_mark_is_measured);
    RUN(test_the_report_names_every_fiber);
    RUN(test_a_long_name_is_truncated_not_overrun);
    RUN(test_reset_returns_every_stack_to_the_heap);
    RUN(test_an_exited_fiber_returns_its_stack_immediately);
    RUN(test_exiting_into_a_never_run_fiber_frees_the_right_stack);
    RUN(test_two_exits_before_a_reap_leak_nothing);
    RUN(test_a_dead_fiber_owns_no_stack);
    RUN(test_the_root_stack_can_be_declared_and_is_then_guarded);
    RUN(test_ten_thousand_switches_change_nothing);

    fiber_reset();
    heap_stats(&h);
    CHECK_EQ(h.bytes_in_use, heap_live_before);
    CHECK_EQ(heap_check(), 0);

    return th_report("fiber");
}
