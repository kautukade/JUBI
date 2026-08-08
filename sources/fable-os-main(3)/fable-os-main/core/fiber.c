/* fiber.c — the cooperative scheduler. See include/fiber.h for the design and
 * for the argument against doing this with a timer interrupt.
 *
 * Everything in this file is portable C. The two machine-dependent functions
 * it calls (fiber_arch_switch, fiber_arch_stack_init) live in
 * arch/x86_64/switch.asm for the kernel and in tests/host/test_fiber.c for the
 * host suite, which runs on a machine that is not x86-64.
 *
 * THE ONE INVARIANT EVERYTHING ELSE RESTS ON
 *   `current` names the fiber whose stack rsp is on. It is updated by the
 *   switcher BEFORE fiber_arch_switch(), never after — because "after" runs on
 *   the other fiber's stack, at a point in time that belongs to the other
 *   fiber. A fiber resumed later therefore finds `current` already pointing at
 *   itself, set by whoever woke it.
 */

#include "kernel.h"
#include "fiber.h"

/* ---- poison ------------------------------------------------------------ */

/* The band pattern is deliberately not 0 and not 0xFF: both are common in
 * freshly-zeroed or freshly-memset memory, so either would let a real overrun
 * write a value that happens to look intact. */
#define GUARD_WORD  0xF1BE9A11F1BE9A11ull   /* "fiber wall" */
#define FILL_WORD   0xC0DEFEEDC0DEFEEDull   /* untouched stack */

#define GUARD_WORDS (FIBER_GUARD_BYTES / 8)

enum { FIBER_FREE = 0, FIBER_READY = 1, FIBER_DEAD = 2 };

struct fiber {
    void     *sp;             /* saved stack pointer while not running       */
    uint8_t  *block;          /* base of the guarded region (low band first) */
    uint8_t  *lo, *hi;        /* usable region, [lo, hi); NULL if unguarded  */
    size_t    usable;         /* hi - lo                                     */
    int       state;
    int       guard_reported; /* one panic per damaged fiber, not one a switch*/
    /* 1 when `block` came from kmalloc and must be given back. The root's block
     * is boot/boot.asm's .bss and freeing it would hand the identity-map page
     * tables' neighbour to the heap. */
    int       owns_block;
    /* The word an UNTOUCHED stack word holds, which is how stack_used() finds
     * the high-water mark. FILL_WORD for a created fiber, whose middle this
     * module paints; 0 for the root, whose middle is .bss that boot/boot.asm
     * zeroed and which cannot be painted because we are standing on it. */
    uint64_t  fill;
    uint64_t  resumes;        /* times this fiber was switched TO            */
    uint64_t  ran_ms;         /* millis() when it was last given the CPU     */
    char      name[FIBER_NAME_MAX + 1];
};

static struct fiber  fibers[FIBER_MAX];
static struct fiber *current;

/* THE STACK BLOCK OF A FIBER THAT HAS EXITED, waiting for the CPU to be off it.
 *
 * A BLOCK POINTER, AND IT USED TO BE A SLOT POINTER, WHICH WAS TWO BUGS.
 *
 * A `struct fiber *` names a SLOT, and a slot changes identity: fiber_join()
 * returns it to FIBER_FREE and the next fiber_create() reuses it and writes a
 * fresh block into it. reap() runs only on the far side of a switch_to(), so if
 * the fiber woken by fiber_exit() had never run before it starts at the arch
 * trampoline and never reaches reap() — leaving the pointer pending across a
 * join and a create, after which reap() kfree()d the NEW fiber's live stack and
 * zeroed its sp. That left a READY fiber with sp == NULL, which
 * fiber_check_stacks() could not see because it skips slots with no block, and
 * the next switch to it was fiber_arch_switch(&me->sp, NULL). On x86-64 low
 * memory is mapped present+writable, so `mov rsp, 0` does not even fault: six
 * pops read the real-mode IVT and the CPU returns to whatever qword lives at
 * physical 0x30. A silent wild jump, which is worse than a fault.
 *
 * The block pointer has no identity to lose, so recycling a slot cannot reach it.
 *
 * ONE SLOT IS ENOUGH, but only because fiber_exit() drains it before overwriting
 * it — see the argument there. It used to overwrite it blind, and two fibers
 * exiting before any reap orphaned the first block permanently: unreferenced by
 * any slot, so not even fiber_reset() could find it. Measured at 8704 bytes per
 * create/exit/join cycle, growing without bound, attributed to nobody. */
static uint8_t      *pending_block;
static int           inited;
static uint64_t      switches;
static int           hog_lines;

/* Where the root fiber's stack pointer was when fiber_init() ran, and the
 * deepest it has been seen AT A YIELD POINT since. Kept even when the root's
 * real bounds are known, because the two numbers answer different questions and
 * the difference between them is itself informative: this one is what the
 * scheduler saw, stack_used() is what actually happened. */
static uint8_t *root_mark;
static size_t   root_depth;

/* The real bounds of boot/boot.asm's stack, if anyone told us (kernel/main.c
 * does, from the exported stack_bottom/stack_top). Zero on a host test, which
 * links no boot.asm and stands on a stack it did not allocate. */
static uint8_t *root_lo_hint, *root_hi_hint;

void fiber_root_stack_set(void *lo, void *hi) {
    root_lo_hint = (uint8_t *)lo;
    root_hi_hint = (uint8_t *)hi;
}

/* ---- small helpers ----------------------------------------------------- */

static void name_copy(char *dst, const char *src) {
    int i = 0;
    if (src)
        for (; i < FIBER_NAME_MAX && src[i]; i++) dst[i] = src[i];
    if (i == 0) { dst[0] = '?'; i = 1; }
    dst[i] = '\0';
}

static void fill_words(void *p, uint64_t word, size_t words) {
    uint64_t *q = (uint64_t *)p;
    for (size_t i = 0; i < words; i++) q[i] = word;
}

/* How many words of a band are not the poison value. */
static size_t band_damage(const void *p, size_t words) {
    const uint64_t *q = (const uint64_t *)p;
    size_t bad = 0;
    for (size_t i = 0; i < words; i++) if (q[i] != GUARD_WORD) bad++;
    return bad;
}

static int valid(const struct fiber *f) {
    if (!f) return 0;
    if (f < fibers || f >= fibers + FIBER_MAX) return 0;
    return f->state != FIBER_FREE;
}

/* ---- stack accounting -------------------------------------------------- */

/* High-water mark: the untouched part of a stack still holds FILL_WORD, so the
 * deepest word ever written is the lowest one that does not. Approximate in
 * one direction only — a frame that happened to store FILL_WORD would be read
 * as untouched — which cannot turn a real overrun into a clean report, because
 * the guard bands are checked against a different pattern entirely. */
static size_t stack_used(const struct fiber *f) {
    if (!f || !f->lo) return 0;
    const uint64_t *w = (const uint64_t *)f->lo;
    size_t n = f->usable / 8;
    for (size_t i = 0; i < n; i++)
        if (w[i] != f->fill) return f->usable - i * 8;
    return 0;
}

/* ---- the guards -------------------------------------------------------- */

static void report_violation(const struct fiber *f, const char *band,
                             size_t bad_words) {
    kprintf("fiber: STACK GUARD VIOLATED - fiber \"%s\" wrote into its %s "
            "poison band (%u of %u guard words clobbered)\n",
            f->name, band, (unsigned)bad_words, (unsigned)GUARD_WORDS);
    kprintf("fiber:   usable stack %u bytes at %p..%p, high-water %u bytes\n",
            (unsigned)f->usable, (void *)f->lo, (void *)f->hi,
            (unsigned)stack_used(f));
    kprintf("fiber:   this kernel has no memory protection, so anything that "
            "went past the band landed in whatever kmalloc put next to it. "
            "Give \"%s\" a bigger stack.\n", f->name);
}

/* An address on the stack this call is running on.
 *
 * __builtin_frame_address(0) rather than &some_local: taking the address of a
 * local and letting it outlive the function is exactly what -Wdangling-pointer
 * exists to catch, and it would be right to complain — the value is only ever
 * compared, never dereferenced, and saying so in the expression is better than
 * suppressing a warning that is usually correct. */
static uint8_t *here_sp(void) { return (uint8_t *)__builtin_frame_address(0); }

int fiber_check_stacks(void) {
    int      damaged = 0;
    uint8_t *here    = here_sp();

    if (!inited) return 0;

    /* Measure how far the root has descended. Informational, and sampled only
     * at the yield points the root passes through — the root has no band to
     * check, because boot/boot.asm's stack has no bounds C can see. */
    if (current == &fibers[0] && root_mark) {
        size_t d = (size_t)(root_mark > here ? root_mark - here : 0);
        if (d > root_depth) root_depth = d;
    }

    for (int i = 0; i < FIBER_MAX; i++) {
        struct fiber *f = &fibers[i];
        if (f->state == FIBER_FREE || !f->block) continue;

        size_t lo_bad = band_damage(f->block, GUARD_WORDS);
        /* The root has NO high band: stack_top is the end of the region
         * boot/boot.asm reserved, and painting past it would scribble on
         * whatever .bss put next (today, multiboot_info_phys). Underflow is
         * also the direction that cannot happen by recursion. */
        size_t hi_bad = f->owns_block ? band_damage(f->hi, GUARD_WORDS) : 0;

        /* The stack pointer itself. For the fiber that is running, `here` is
         * the truth; for the others, the value the switcher saved. Either way
         * it must be inside the usable region — a saved sp outside it means
         * the frame chain we are about to `ret` through is already gone. */
        const uint8_t *sp = (f == current) ? (const uint8_t *)here
                                           : (const uint8_t *)f->sp;
        int sp_bad = (sp < f->lo || sp > f->hi);

        if (!lo_bad && !hi_bad && !sp_bad) continue;
        damaged++;
        if (f->guard_reported) continue;
        f->guard_reported = 1;

        if (lo_bad) report_violation(f, "low (overflow)", lo_bad);
        if (hi_bad) report_violation(f, "high (underflow)", hi_bad);
        if (sp_bad)
            kprintf("fiber: STACK POINTER OUT OF BOUNDS - fiber \"%s\" has sp "
                    "%p, outside its stack %p..%p\n",
                    f->name, (void *)sp, (void *)f->lo, (void *)f->hi);

        /* A blown stack in a kernel with no memory protection has already
         * corrupted something it does not own. Continuing would run the rest
         * of the boot on top of that. */
        panic("fiber: stack guard violated");
    }
    return damaged;
}

/* ---- scheduling -------------------------------------------------------- */

int fiber_runnable(void) {
    int n = 0;
    for (int i = 0; i < FIBER_MAX; i++)
        if (fibers[i].state == FIBER_READY) n++;
    return n;
}

int fiber_count(void) {
    int n = 0;
    for (int i = 0; i < FIBER_MAX; i++)
        if (fibers[i].state != FIBER_FREE) n++;
    return n;
}

/* Round-robin: the next READY slot after `from`, wrapping. Returns `from`
 * itself when it is the only runnable one, and NULL when nothing at all is
 * (which can only happen if `from` is not READY either — fiber_exit's case). */
static struct fiber *next_runnable(const struct fiber *from) {
    int start = (int)(from - fibers);
    for (int i = 1; i <= FIBER_MAX; i++) {
        struct fiber *f = &fibers[(start + i) % FIBER_MAX];
        if (f->state == FIBER_READY) return f;
    }
    return (struct fiber *)0;
}

/* Free the stack of a fiber that exited. Called only after a switch, i.e. only
 * once the CPU is provably not standing on the memory being released.
 *
 * It touches no fiber slot at all, which is the point: the slot this block came
 * from was disowned by fiber_exit() before the switch, and may since have been
 * joined and handed to a different fiber. There is nothing here to get wrong. */
static void reap(void) {
    uint8_t *b = pending_block;
    if (!b) return;
    pending_block = (uint8_t *)0;
    kfree(b);
}

/* A fiber that has held the CPU too long while somebody else was waiting.
 * There is nothing to be done about it — this cannot preempt — so all that is
 * on offer is naming it, once, so the hang has a suspect. */
static void check_hog(struct fiber *me, uint64_t now) {
    if (!me->ran_ms) return;
    if (hog_lines >= FIBER_HOG_MAX_LINES) return;
    if (now - me->ran_ms < FIBER_HOG_MS) return;
    if (fiber_runnable() < 2) return;      /* nobody was starved: not a hog */

    hog_lines++;
    kprintf("fiber: \"%s\" held the CPU for %u ms without yielding while "
            "%d other fiber(s) were runnable. Nothing can preempt it; if it "
            "ever stops yielding entirely the machine stops with it.\n",
            me->name, (unsigned)(now - me->ran_ms), fiber_runnable() - 1);
}

static void switch_to(struct fiber *nxt) {
    struct fiber *me = current;

    /* A NULL stack pointer is `mov rsp, 0` followed by six pops, and on this
     * machine that does not fault — low memory is mapped present and writable, so
     * the CPU quietly pops the real-mode IVT and returns into whatever is at
     * physical 0x30. There is no fault frame, no backtrace and nothing to
     * diagnose. Naming the fiber here costs one compare per switch and converts
     * the whole class into a panic that says which fiber and what happened. */
    if (!nxt->sp) {
        kprintf("fiber: fiber \"%s\" is %s but has no saved stack pointer; its "
                "stack was released while it was still scheduled\n",
                nxt->name, nxt->state == FIBER_READY ? "runnable" : "not free");
        panic("fiber: switch to a fiber with no stack");
        return;
    }

    switches++;
    nxt->resumes++;
    nxt->ran_ms = millis();
    current = nxt;                       /* before, never after: see the top */

    fiber_arch_switch(&me->sp, nxt->sp);

    /* Resumed. `current` is already `me` again — whoever woke us set it. The
     * dead fiber whose stack could not be freed while it stood on it can be
     * freed now, from a stack that is definitely not it. */
    reap();
}

void fiber_yield(void) {
    if (!inited || !current) return;     /* no scheduler yet: nothing to do */

    struct fiber *me  = current;
    uint64_t      now = millis();

    check_hog(me, now);

    /* Never switch on top of a stack that is already damaged: the frame chain
     * we would restore later may not exist any more. In the kernel panic() has
     * ended the machine before this is reached; on the host it returns, and
     * the test wants a live process to make assertions in. */
    if (fiber_check_stacks() != 0) return;

    struct fiber *nxt = next_runnable(me);
    if (!nxt || nxt == me) { me->ran_ms = now; return; }

    switch_to(nxt);
}

/* ---- lifecycle --------------------------------------------------------- */

int fiber_init(void) {
    if (inited) return 0;

    for (int i = 0; i < FIBER_MAX; i++) fibers[i].state = FIBER_FREE;

    struct fiber *root = &fibers[0];
    root->state  = FIBER_READY;
    root->sp     = (void *)0;            /* filled in by the first switch */
    root->block  = (uint8_t *)0;
    root->lo     = root->hi = (uint8_t *)0;
    root->usable = 0;
    root->owns_block = 0;
    root->fill   = 0;
    root->resumes = 1;                   /* it is running right now */
    root->ran_ms = millis();
    root->guard_reported = 0;
    name_copy(root->name, "main");

    /* GUARD THE ROOT, if its bounds were handed to us.
     *
     * This is the line include/fiber.h used to defer, and the argument for
     * deferring it ("the root runs mbedTLS and the deepest user should keep the
     * biggest stack") was overtaken by compiler/: a single cc_compile() descends
     * several times deeper than the whole TLS path, on untrusted input, with the
     * identity-map page tables 0 bytes below. Unguarded, that is a triple fault
     * with no #PF, no fault record and nothing on the serial line.
     *
     * Two differences from a created fiber, both forced by the root already
     * running on this memory:
     *   - the middle is NOT painted, because we are standing in it. The
     *     high-water scan therefore looks for the lowest non-ZERO word, which is
     *     sound only because boot/boot.asm rep-stosb-zeroes the whole of .bss
     *     before anything else runs. fill = 0 records that.
     *   - the low band IS painted, because nothing has ever been that deep. It
     *     costs FIBER_GUARD_BYTES of usable stack and buys the one thing worth
     *     having: an overflow becomes a named panic instead of a dead machine. */
    if (root_lo_hint && root_hi_hint &&
        (size_t)(root_hi_hint - root_lo_hint) > FIBER_GUARD_BYTES * 4) {
        root->block  = root_lo_hint;
        root->lo     = root_lo_hint + FIBER_GUARD_BYTES;
        root->hi     = root_hi_hint;
        root->usable = (size_t)(root->hi - root->lo);
        fill_words(root->block, GUARD_WORD, GUARD_WORDS);
    }

    current      = root;
    pending_block = (uint8_t *)0;
    switches     = 0;
    hog_lines    = 0;
    root_mark    = here_sp();
    root_depth   = 0;
    inited       = 1;
    return 0;
}

fiber_t *fiber_create(const char *name, fiber_fn_t fn, void *arg,
                      size_t stack_bytes) {
    if (!inited) {
        kprintf("fiber: create(\"%s\") before fiber_init()\n",
                name ? name : "?");
        return (fiber_t *)0;
    }
    if (!fn) {
        kprintf("fiber: create(\"%s\") with no entry point\n",
                name ? name : "?");
        return (fiber_t *)0;
    }

    if (stack_bytes == 0)               stack_bytes = FIBER_STACK_DEFAULT;
    if (stack_bytes < FIBER_STACK_MIN)  stack_bytes = FIBER_STACK_MIN;
    if (stack_bytes > FIBER_STACK_MAX)  stack_bytes = FIBER_STACK_MAX;
    stack_bytes = (stack_bytes + 15u) & ~(size_t)15u;

    struct fiber *f = (struct fiber *)0;
    for (int i = 1; i < FIBER_MAX; i++)          /* slot 0 is always the root */
        if (fibers[i].state == FIBER_FREE) { f = &fibers[i]; break; }
    if (!f) {
        kprintf("fiber: cannot create \"%s\": all %d slots are in use. A fiber "
                "that exited but was never fiber_join()ed still holds one.\n",
                name ? name : "?", FIBER_MAX);
        return (fiber_t *)0;
    }

    size_t   total = stack_bytes + 2u * FIBER_GUARD_BYTES;
    uint8_t *block = (uint8_t *)kmemalign(16, total);
    if (!block) {
        kprintf("fiber: cannot create \"%s\": the heap has no %u byte stack\n",
                name ? name : "?", (unsigned)total);
        return (fiber_t *)0;
    }

    f->block      = block;
    f->lo         = block + FIBER_GUARD_BYTES;
    f->hi         = f->lo + stack_bytes;
    f->usable     = stack_bytes;
    f->owns_block = 1;
    f->fill       = FILL_WORD;

    fill_words(block,  GUARD_WORD, GUARD_WORDS);          /* low band  */
    fill_words(f->hi,  GUARD_WORD, GUARD_WORDS);          /* high band */
    fill_words(f->lo,  FILL_WORD,  stack_bytes / 8);      /* the middle */

    f->sp = fiber_arch_stack_init(f->hi, fn, arg);

    /* CHECK THE ARCH SEAM, HERE, WHERE IT CAN STILL BE SAID OUT LOUD.
     *
     * fiber_arch_stack_init() and fiber_arch_switch() are one frame layout
     * written twice, in assembly, in the same file — and if they ever
     * disagree (a register added to the push list and not to the frame) the
     * first switch `ret`s to a word that is not a return address, on a machine
     * with no memory protection, and the report is a triple fault at a random
     * address with nothing to connect it back to here. So make the one thing
     * that is checkable from C a hard error at creation time: the initial
     * stack pointer must be an 8-aligned address inside the stack the frame
     * was supposed to be written to. (8, not 16: on x86-64 the frame is an odd
     * number of words below a 16-aligned top, exactly as a `call` leaves it.) */
    if ((uint8_t *)f->sp < f->lo || (uint8_t *)f->sp >= f->hi ||
        ((uintptr_t)f->sp & 7u) != 0) {
        kprintf("fiber: refusing \"%s\": the arch backend built a context at %p, "
                "which is not an 8-aligned address inside the %u byte stack at "
                "%p..%p it was given. fiber_arch_stack_init() and "
                "fiber_arch_switch() have diverged.\n",
                name ? name : "?", f->sp, (unsigned)stack_bytes,
                (void *)f->lo, (void *)f->hi);
        kfree(block);
        f->block = (uint8_t *)0;
        f->lo = f->hi = (uint8_t *)0;
        f->usable = 0;
        f->sp = (void *)0;
        return (fiber_t *)0;
    }

    f->state          = FIBER_READY;
    f->resumes        = 0;
    f->ran_ms         = 0;
    f->guard_reported = 0;
    name_copy(f->name, name);

    kprintf("fiber: \"%s\" created, %u byte stack at %p..%p "
            "(+%u byte poison band each end)\n",
            f->name, (unsigned)stack_bytes, (void *)f->lo, (void *)f->hi,
            (unsigned)FIBER_GUARD_BYTES);
    return (fiber_t *)f;
}

void fiber_exit(void) {
    struct fiber *me = current;

    if (!inited || !me) {
        panic("fiber_exit: no scheduler");
        return;
    }
    if (me == &fibers[0]) {
        /* The root owns the context every other fiber was branched from; if it
         * ends there is nothing left to switch back to. */
        panic("fiber_exit: the root fiber cannot exit");
        return;
    }

    me->state = FIBER_DEAD;

    struct fiber *nxt = next_runnable(me);
    if (!nxt) {
        panic("fiber_exit: nothing runnable is left");
        return;
    }

    /* DRAIN BEFORE CLAIMING. If a block is already waiting it belongs to some
     * OTHER fiber that exited without a reap running in between (it woke a fiber
     * that had never run, so control went to the arch trampoline rather than back
     * into switch_to()). We are demonstrably not standing on that block — we are
     * standing on ours — so it can be freed right here, and must be, because the
     * next line is about to be the only reference to it. */
    if (pending_block && pending_block != me->block) {
        kfree(pending_block);
        pending_block = (uint8_t *)0;
    }

    /* We are standing on the stack that has to be freed, so hand it to the
     * fiber we are about to wake; switch_to()'s reap() runs on the far side.
     *
     * The slot DISOWNS the block now, before the switch, so that from this moment
     * no fiber_t claims memory that is about to be released. That is what makes a
     * later fiber_join() and fiber_create() on this slot harmless: there is
     * nothing left in it to be freed twice or to be mistaken for live. */
    pending_block = me->block;
    me->block      = (uint8_t *)0;
    me->owns_block = 0;
    me->lo = me->hi = (uint8_t *)0;
    me->usable      = 0;

    /* THE SAME CHECK switch_to() MAKES, for the same reason, because this is the
     * same five lines written twice. `mov rsp, 0` does not fault on this machine;
     * it is a silent wild jump through the real-mode IVT. Unreachable today —
     * only the root can hold sp == NULL and the root cannot exit — but a guarded
     * copy and an unguarded copy of one switch sitting in one file is exactly how
     * the bug the other comment exists to prevent comes back. */
    if (!nxt->sp) {
        kprintf("fiber: fiber \"%s\" is %s but has no saved stack pointer; its "
                "stack was released while it was still scheduled\n",
                nxt->name, nxt->state == FIBER_READY ? "runnable" : "not free");
        panic("fiber_exit: switch to a fiber with no stack");
        return;
    }

    switches++;
    nxt->resumes++;
    nxt->ran_ms = millis();
    current = nxt;
    fiber_arch_switch(&me->sp, nxt->sp);

    /* Unreachable: nothing ever makes a FIBER_DEAD fiber runnable again, and
     * its stack no longer exists. */
    panic("fiber_exit: a dead fiber was resumed");
}

int fiber_join(fiber_t *h) {
    struct fiber *f = (struct fiber *)h;

    if (!inited || !valid(f) || f == current) return -1;

    while (f->state != FIBER_DEAD) {
        if (fiber_runnable() < 2) return -1;   /* it can never finish: no spin */
        fiber_yield();
    }

    /* Its stack is gone, or is the one block waiting for a reap; either way this
     * slot no longer owns any memory, because fiber_exit() disowned it before
     * switching away. That is what makes releasing the slot safe — the previous
     * version of this comment asserted the reap had already HAPPENED, which is
     * not true when the fiber it woke had never run, and a recycled slot then let
     * reap() free the next occupant's live stack. Belt and braces: if a block is
     * somehow still here, it is unreferenced the moment the slot is freed, so
     * release it rather than leak it. */
    if (f->block && f->owns_block) {
        kfree(f->block);
        f->block = (uint8_t *)0;
    }
    f->block          = (uint8_t *)0;
    f->owns_block     = 0;
    f->lo = f->hi     = (uint8_t *)0;
    f->usable         = 0;
    f->sp             = (void *)0;
    f->state          = FIBER_FREE;
    f->guard_reported = 0;
    return 0;
}

void fiber_reset(void) {
    if (inited && current && current != &fibers[0]) {
        panic("fiber_reset: only the root fiber may tear the system down");
        return;
    }
    /* The block waiting for a reap belongs to no slot (fiber_exit() disowned it),
     * so the loop below cannot reach it and it would leak past a teardown whose
     * entire job is to release every fiber stack. */
    if (pending_block) kfree(pending_block);
    pending_block = (uint8_t *)0;

    for (int i = 0; i < FIBER_MAX; i++) {
        /* owns_block, not block: the root's block is boot/boot.asm's .bss and
         * handing it to the heap would give away the page tables' neighbour. */
        if (fibers[i].block && fibers[i].owns_block) kfree(fibers[i].block);
        fibers[i].block  = (uint8_t *)0;
        fibers[i].owns_block = 0;
        fibers[i].fill   = 0;
        fibers[i].lo     = fibers[i].hi = (uint8_t *)0;
        fibers[i].usable = 0;
        fibers[i].sp     = (void *)0;
        fibers[i].state  = FIBER_FREE;
        fibers[i].guard_reported = 0;
        fibers[i].resumes = 0;
        fibers[i].ran_ms  = 0;
        fibers[i].name[0] = '\0';
    }
    current      = (struct fiber *)0;
    pending_block = (uint8_t *)0;
    switches     = 0;
    hog_lines    = 0;
    root_mark    = (uint8_t *)0;
    root_depth   = 0;
    inited       = 0;
}

/* ---- introspection ----------------------------------------------------- */

fiber_t *fiber_self(void) { return (fiber_t *)current; }

const char *fiber_name(const fiber_t *h) {
    const struct fiber *f = (const struct fiber *)h;
    return valid(f) ? f->name : "?";
}

int fiber_done(const fiber_t *h) {
    const struct fiber *f = (const struct fiber *)h;
    return valid(f) && f->state == FIBER_DEAD;
}

int fiber_stack_bounds(const fiber_t *h, void **lo, void **hi) {
    const struct fiber *f = (const struct fiber *)h;
    if (!valid(f) || !f->lo) return -1;
    if (lo) *lo = (void *)f->lo;
    if (hi) *hi = (void *)f->hi;
    return 0;
}

size_t fiber_stack_size(const fiber_t *h) {
    const struct fiber *f = (const struct fiber *)h;
    return valid(f) ? f->usable : 0;
}

size_t fiber_stack_used(const fiber_t *h) {
    const struct fiber *f = (const struct fiber *)h;
    if (!valid(f)) return 0;
    /* The root has a real high-water mark once its bounds are known (the .bss
     * zero-scan); root_depth is only the yield-point sample, which is blind to
     * the deepest frames and is the wrong answer whenever a better one exists. */
    if (f == &fibers[0] && !f->lo) return root_depth;
    return stack_used(f);
}

uint64_t fiber_switches(void) { return switches; }

uint64_t fiber_switch_count(const fiber_t *h) {
    const struct fiber *f = (const struct fiber *)h;
    return valid(f) ? f->resumes : 0;
}

void fiber_report(void) {
    if (!inited) { kputs("fiber: not initialised\n"); return; }

    kprintf("fiber: %d live, %d runnable, %u switches\n",
            fiber_count(), fiber_runnable(), (unsigned)switches);
    for (int i = 0; i < FIBER_MAX; i++) {
        struct fiber *f = &fibers[i];
        if (f->state == FIBER_FREE) continue;

        const char *st = f == current      ? "running"
                       : f->state == FIBER_DEAD ? "exited"
                                                : "ready";
        /* No '-' flag and no '*' width: lib/base.c's kprintf understands only
         * %s %c %d %u %x %p %% with an optional 0 and a literal width, and
         * anything else prints two characters while consuming no argument —
         * which shifts every later one. tests/host/kshim.c enforces exactly
         * that grammar, so this is checked and not merely believed. */
        if (!f->lo) {
            /* The root. Its size is boot/boot.asm's and not visible here, so
             * report the depth measured rather than invent a denominator.
             *
             * AND SAY WHAT THE NUMBER IS, because it is not a high-water mark and
             * reads exactly like one. root_depth is only updated at the yield
             * points the root passes through, and those are shallow polling loops:
             * net_service() yields as its LAST statement, after net_poll_rx() has
             * returned, and the mbedTLS handshake happens inside net_poll_rx().
             * The root's deepest frames therefore cannot be sampled, and this
             * figure came out at the same 656 bytes on boots that did a full TLS
             * handshake and on boots that did nothing, while the true depth read
             * out of RAM was 5128. "at a yield point" is the honest caption. */
            kprintf("fiber:   %s [%s] boot stack, %u bytes deep at the deepest "
                    "YIELD POINT (not a high-water mark: the deepest frames on "
                    "this stack never yield), resumed %u times\n",
                    f->name, st, (unsigned)root_depth, (unsigned)f->resumes);
        } else {
            size_t used = stack_used(f);
            kprintf("fiber:   %s [%s] stack %u/%u bytes used (%u%%), "
                    "resumed %u times\n",
                    f->name, st, (unsigned)used, (unsigned)f->usable,
                    (unsigned)(f->usable ? used * 100u / f->usable : 0u),
                    (unsigned)f->resumes);
            /* THE ROOT PRINTS BOTH NUMBERS, and the gap between them is the
             * point. The first is the real high-water mark; the second is all
             * the scheduler could ever see, because the root's deepest frames
             * (mbedTLS inside net_poll_rx(), the compiler inside a tool call)
             * never reach a yield point. They differed by 8x on a plain boot and
             * by 100x during a compile; printing only the second is what let
             * include/fiber.h call a live hazard a diagnostic gap. */
            if (i == 0)
                kprintf("fiber:     of which %u bytes is all the scheduler ever "
                        "sampled: the deepest frames on this stack never yield. "
                        "The %u byte figure above is the .bss high-water mark and "
                        "is the one to trust. Low poison band: %u bytes.\n",
                        (unsigned)root_depth, (unsigned)used,
                        (unsigned)FIBER_GUARD_BYTES);
        }
    }
}
