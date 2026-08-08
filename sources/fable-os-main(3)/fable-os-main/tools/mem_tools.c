/* mem_tools.c — the memory and system-state tool family.
 *
 * PURPOSE
 *   Let the model answer questions about *this machine* from measurements
 *   instead of plausible-sounding guesses. "How much memory are you using?" is
 *   answered from live allocator counters; "is your heap healthy?" is answered
 *   by actually walking the heap; "what's at that address?" is answered by
 *   actually reading the bytes.
 *
 *   Every tool here is read-only with respect to kernel data structures. None of
 *   them allocate: the whole family must stay usable when the thing being
 *   diagnosed is the allocator itself, and (Phase 3) when the thing being
 *   diagnosed is a fault that just happened.
 *
 * THE TOOL SET
 *   heap_stats   live allocator statistics + a non-destructive consistency
 *                verdict. The default question-answering tool.
 *   heap_check   the real structural walk of the heap. Separate from heap_stats
 *                precisely because it is not free of consequences — see below.
 *   mem_map      where the kernel actually lives in physical memory, and which
 *                address range mem_read will accept. Without this, mem_read is a
 *                guessing game; with it, the model can aim.
 *   mem_read     bounded, read-only hex + ASCII dump of kernel memory.
 *   sys_uptime   milliseconds since time_init(), plus a human rendering.
 *
 *   mem_map exists as its own tool rather than as prose in mem_read's schema so
 *   the window is *ground truth read from the linker symbols at runtime* rather
 *   than a documented constant that can silently rot.
 *
 * ============================================================================
 * DECISION: mem_read (raw memory inspection) IS exposed. The safety argument.
 * ============================================================================
 *   The hazard is real and worth stating plainly: this kernel identity-maps the
 *   low 4 GiB with 2 MiB RWX pages (boot/boot.asm), so there is no paging
 *   protection to catch a bad address and a null dereference does not even
 *   fault. There IS an IDT now (arch/x86_64/idt.c, 256 gates), so a fault in the
 *   unmapped range is captured and reported rather than triple-faulting — but
 *   that is a post-mortem, not a guard: it records the crash, it does not let
 *   the read be retried. So a memory read driven by model-supplied numbers is
 *   still the single most dangerous thing in this codebase, and "it is bounded"
 *   has to mean something stronger than a range check on a plausible-looking
 *   value.
 *
 *   It is exposed because the bound available here is not a heuristic, it is a
 *   static property of the link:
 *
 *   1. The accepted window is exactly [0x100000, __bss_end) — the kernel image
 *      as laid out by linker.ld: .boot/.text/.rodata/.driver_table/.tool_table/
 *      .data/.bss. Both ends come from the link, not from a constant a refactor
 *      could invalidate: 0x100000 is the ORIGIN in linker.ld and __bss_end is a
 *      linker-provided symbol that boot.asm itself already uses to zero the BSS.
 *
 *   2. Every byte in that window is RAM the kernel has already touched. The
 *      loader wrote the image; boot.asm zeroed [__bss_start, __bss_end) before
 *      calling into C. So the window is not merely "mapped", it is demonstrably
 *      backed and already both read and written on every boot. A read there
 *      cannot fault, for the same reason the kernel's own execution cannot.
 *
 *   3. The window is a subset of the identity map by construction: __bss_end is
 *      roughly 18 MiB (a ~1.5 MiB image plus the 16 MiB heap arena), far below
 *      the 4 GiB mapped by boot.asm. Because the window's high end is a compile-
 *      time-bounded low address, no accepted address can be non-canonical, above
 *      4 GiB, or otherwise outside the page tables — the three ways an x86-64
 *      read can fault. Those failure modes are excluded by arithmetic, not by
 *      hoping the model behaves.
 *
 *   4. What the window deliberately EXCLUDES is the part that could hurt without
 *      faulting: MMIO. The VGA framebuffer at 0xB8000, the e1000 BARs up near
 *      0xFEBC0000, the LAPIC page. Those are inside the identity map, so reading
 *      them would not fault — it would silently corrupt driver state (many
 *      device registers clear-on-read). That is the actual reason the window is
 *      the kernel image and not "anything below 4 GiB", which would have been
 *      the easy and wrong bound.
 *
 *   5. Length is capped at MEM_READ_MAX (256) bytes per call, and the range test
 *      is written as `len > hi - addr` — a subtraction between two values already
 *      proven ordered — so `addr + len` is never computed and cannot wrap. All
 *      arithmetic is in uint64_t; nothing is cast to a pointer until after the
 *      test passes.
 *
 *   6. The bytes are copied out through a `volatile` byte pointer into a local
 *      buffer before any formatting, so the compiler cannot widen, reorder, or
 *      speculate the access, and the raw pointer's lifetime is a handful of
 *      instructions.
 *
 *   The payoff is Phase 3. When an exception handler captures a fault and asks
 *   the model to diagnose it, everything the model needs to reason about is
 *   inside this exact window: the faulting instruction bytes in .text, the
 *   kernel stack (boot.asm's 64 KiB stack_bottom..stack_top lives in .bss), the
 *   heap arena and its block headers (also .bss), and every global. A diagnosis
 *   tool that could report statistics but never look at a byte would be a
 *   demo of a debugger rather than one. The window was already the right answer
 *   for that phase, so there is no reason to defer it to that phase.
 *
 *   What is NOT exposed: any write (mem_write/poke), any read outside the image
 *   window, and any dereference of a pointer the model supplies to another tool.
 *   mem_read is a read of known-backed RAM; those are not. The IDT does not
 *   change this — a captured fault is a record of the damage, not permission to
 *   risk it, and a stray WRITE is not undone by having been reported.
 *
 * ============================================================================
 * DECISION: heap_check() panics on corruption. How that is handled.
 * ============================================================================
 *   heap_check() does not report corruption — it panics. So a tool the model can
 *   call at will is a tool that can halt the machine, and on a corrupt heap the
 *   "success" path of the handler is simply never reached. Four things follow:
 *
 *   a. A non-destructive pre-flight runs first. stats_verdict() re-derives every
 *      invariant that is checkable from the O(1) counters alone — size
 *      accounting, peak >= current, frees <= allocs, live == allocs - frees,
 *      header overhead plausible. If any of those already disagree, the heap is
 *      *known* suspect and the walk is refused: the model gets a legible error
 *      naming the broken invariant, on a machine that is still alive to receive
 *      it. Statistics-level corruption is therefore reported, never fatal.
 *
 *   b. A breadcrumb is printed by the kernel immediately before the walk. If the
 *      walk panics, the panic is the last thing in the log and would otherwise be
 *      unattributable — with no shell, the operator's only record of *why* the
 *      machine died is what the kernel printed. The trace line still comes after
 *      the call, from the real result, per trace.h.
 *
 *   c. The tool's description tells the model the truth: on a corrupt heap this
 *      call halts the machine, and the panic message is the diagnosis. That makes
 *      it an informed choice rather than a trap, and it is why heap_stats (which
 *      carries the safe verdict) is the tool to reach for casually.
 *
 *   d. It is flagged TOOL_MUTATES. It mutates nothing, but TOOL_MUTATES is the
 *      documented hook for "a future confirmation prompt or read-only mode would
 *      key off this" (tool.h), and a call that can irreversibly halt the machine
 *      is exactly what such a prompt is for. Marking it costs nothing today and
 *      makes the policy hook correct the day it exists.
 *
 * UNTRUSTED INPUT
 *   `input` is model output. The four no-argument tools ignore it entirely — the
 *   most robust possible parse. mem_read validates through json.h only: the span
 *   must parse as a JSON object, "address" must be a string (hex or decimal,
 *   bounded length, overflow-checked) or a non-negative integer, "length" must be
 *   an integer in [1, 256]. Everything else becomes a message that names the
 *   problem and restates the accepted shape, so the model can fix it next turn.
 *
 * OUTPUT BOUNDS
 *   mem_read is the only variable-length output. It stops emitting lines while
 *   there is still room for the notice, then says how many of the requested bytes
 *   it actually showed and at what address to resume — the model is never handed
 *   a prefix it would reason about as complete. mark_truncated() is the backstop
 *   for every other path.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "heap.h"
#include "kernel.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>      /* snprintf, via port/stdio.h when freestanding */

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand. Export a named pointer per tool instead; tests/host/test_mem_tools.c
 * stands in for the linker and builds the table from those. Kernel builds are
 * untouched. */
#ifdef FABLEOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const fableos_hosttool_##var = &(var)
#endif

/* ====================================================================== */
/* the readable window                                                    */
/* ====================================================================== */

#define MEM_READ_MAX       256u   /* bytes per mem_read call            */
#define MEM_READ_DEFAULT    64u
#define MEM_ADDR_TEXT_MAX   34    /* "0x" + 16 hex digits + slack + NUL  */
#define MEM_TAIL_RESERVE   160u   /* room kept free for the truncation notice */

#ifdef FABLEOS_HOSTTEST
/* Host builds have no kernel image to point at. The window starts empty (every
 * read refused) and a test aims it at a canary-guarded buffer, so the real
 * bounds logic is what the tests exercise. */
static uint64_t host_win_lo = 0, host_win_hi = 0;
void mem_tools_set_window(const void *base, size_t len);
void mem_tools_set_window(const void *base, size_t len) {
    host_win_lo = (uint64_t)(uintptr_t)base;
    host_win_hi = host_win_lo + (uint64_t)len;
}
#else
/* linker.ld: the image is linked at 1 MiB; __bss_end closes it. boot.asm uses
 * the same symbol to zero the BSS, so both ends are link-time facts. */
#define KERNEL_IMAGE_BASE 0x0000000000100000ULL
extern char __bss_start[];
extern char __bss_end[];
#endif

static void mem_window(uint64_t *lo, uint64_t *hi) {
#ifdef FABLEOS_HOSTTEST
    *lo = host_win_lo;
    *hi = host_win_hi;
#else
    *lo = KERNEL_IMAGE_BASE;
    *hi = (uint64_t)(uintptr_t)__bss_end;
    if (*hi < *lo) *hi = *lo;          /* cannot happen; costs one compare */
#endif
}

/* 0 = the read is inside the window. Written so `addr + len` is never formed:
 * both operands of the subtraction are already proven ordered. */
static int window_allows(uint64_t addr, uint64_t len) {
    uint64_t lo, hi;
    mem_window(&lo, &hi);
    if (hi <= lo)      return -1;        /* window empty / not established */
    if (len == 0)      return -1;
    if (len > MEM_READ_MAX) return -1;
    if (addr < lo)     return -1;
    if (addr >= hi)    return -1;
    if (len > hi - addr) return -1;
    return 0;
}

/* ====================================================================== */
/* formatting helpers (no floats: the kernel builds with -mno-sse)        */
/* ====================================================================== */

/* Lowercase hex into `dst`, exactly `digits` wide when digits > 0, otherwise
 * minimal width. Always NUL-terminates. `dst` must hold 17 + NUL. */
static void fmt_hex(char *dst, uint64_t v, int digits) {
    static const char hx[] = "0123456789abcdef";
    char tmp[17];
    int  n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 16) { tmp[n++] = hx[v & 0xF]; v >>= 4; }
    int pad = (digits > n) ? digits - n : 0;
    int o = 0;
    while (pad-- > 0) dst[o++] = '0';
    while (n-- > 0)   dst[o++] = tmp[n];
    dst[o] = '\0';
}

/* "0x" + hex, minimal width — for trace lines and prose. */
static void fmt_addr(char *dst, uint64_t v) {
    dst[0] = '0'; dst[1] = 'x';
    fmt_hex(dst + 2, v, 0);
}

/* "0x" + 16 hex digits — for hexdump gutters, so columns line up. */
static void fmt_addr_wide(char *dst, uint64_t v) {
    dst[0] = '0'; dst[1] = 'x';
    fmt_hex(dst + 2, v, 16);
}

/* Guarantee the model is told when output was clipped, by overwriting the tail
 * of the buffer rather than appending into a buffer that is by definition
 * already full. */
static void mark_truncated(tool_result_t *r) {
    static const char note[] = "\n[output truncated]";
    if (!r || !r->truncated || !r->buf || r->cap == 0) return;
    size_t n = sizeof note - 1;
    if (r->cap < n + 2) {                      /* pathologically small buffer */
        /* Not even room for the notice: leave a single legible marker and a
         * terminator immediately after it, so the result can never be mistaken
         * for a complete (or partially readable) answer. */
        size_t k = 0;
        if (r->cap > 1) r->buf[k++] = '!';
        r->buf[k] = '\0';
        r->len    = k;
        return;
    }
    size_t at = r->cap - 1 - n;
    memcpy(r->buf + at, note, n);
    r->buf[r->cap - 1] = '\0';
    r->len = r->cap - 1;
}

/* ====================================================================== */
/* heap statistics: the safe verdict                                      */
/* ====================================================================== */

/* Re-derive every invariant that is checkable from the O(1) counters alone,
 * without touching a single block header. Returns NULL when the statistics are
 * self-consistent, else a short name for the invariant that failed.
 *
 * This is the non-panicking half of a heap check (see the header comment) and is
 * non-static so tests/host/test_mem_tools.c can drive it with hand-built,
 * deliberately-inconsistent stats — corruption that would be unsafe to
 * manufacture in a live arena. */
const char *mem_tools_stats_verdict(const heap_stats_t *s);
const char *mem_tools_stats_verdict(const heap_stats_t *s) {
    if (!s)                                   return "null-stats";
    if (s->arena_size == 0)                   return "arena-size-zero";
    if (s->bytes_in_use > s->arena_size)      return "in-use-exceeds-arena";
    if (s->bytes_free   > s->arena_size)      return "free-exceeds-arena";
    if (s->overhead     > s->arena_size)      return "overhead-exceeds-arena";
    if (s->bytes_in_use + s->bytes_free + s->overhead != s->arena_size)
        return "size-accounting";
    if (s->peak_in_use < s->bytes_in_use)     return "peak-below-current";
    if (s->total_frees > s->total_allocs)     return "more-frees-than-allocs";
    /* kmalloc/kmemalign bump both allocs and live blocks; kfree bumps frees and
     * drops one. krealloc in place touches neither. So this holds exactly. */
    if ((uint64_t)s->used_blocks != s->total_allocs - s->total_frees)
        return "live-blocks-vs-alloc-delta";
    if (s->bytes_free > 0 && s->free_blocks == 0)
        return "free-bytes-without-free-block";
    /* Every block carries a header; 16 is the allocator's alignment floor, so
     * this stays true across any header layout change. */
    if (s->overhead < (s->used_blocks + s->free_blocks) * 16)
        return "overhead-too-small-for-block-count";
    return NULL;
}

/* ====================================================================== */
/* heap_stats                                                             */
/* ====================================================================== */

static int t_heap_stats(const tool_call_t *call, tool_result_t *r) {
    (void)call;                     /* no arguments: nothing to misparse */

    heap_stats_t s;
    heap_stats(&s);

    const char   *bad         = mem_tools_stats_verdict(&s);
    unsigned long outstanding = (unsigned long)(s.total_allocs - s.total_frees);
    /* Per-mille, integer only. */
    unsigned long permille = s.arena_size
        ? (unsigned long)(((uint64_t)s.bytes_in_use * 1000u) / s.arena_size) : 0;

    tool_result_printf(r,
        "heap arena:  %lu bytes (%lu KiB)\n"
        "in use:      %lu bytes (%lu KiB) = %lu.%lu%% of arena\n"
        "free:        %lu bytes (%lu KiB)\n"
        "overhead:    %lu bytes in block headers\n"
        "peak in use: %lu bytes (%lu KiB)\n"
        "blocks:      %lu live, %lu free\n"
        "lifetime:    %lu allocs, %lu frees, %lu outstanding\n"
        "consistency: %s\n",
        (unsigned long)s.arena_size,   (unsigned long)(s.arena_size / 1024),
        (unsigned long)s.bytes_in_use, (unsigned long)(s.bytes_in_use / 1024),
        permille / 10, permille % 10,
        (unsigned long)s.bytes_free,   (unsigned long)(s.bytes_free / 1024),
        (unsigned long)s.overhead,
        (unsigned long)s.peak_in_use,  (unsigned long)(s.peak_in_use / 1024),
        (unsigned long)s.used_blocks,  (unsigned long)s.free_blocks,
        (unsigned long)s.total_allocs, (unsigned long)s.total_frees, outstanding,
        bad ? bad : "counters self-consistent (run heap_check to walk the heap)");

    mark_truncated(r);

    if (bad) {
        r->is_error = 1;
        trace_err(TOOL_EINVAL, "heap_stats", "inconsistent=%s", bad);
        return TOOL_OK;
    }
    trace_ret((long)s.used_blocks, "heap_stats",
              "in_use=%luB peak=%luB free_blocks=%lu",
              (unsigned long)s.bytes_in_use, (unsigned long)s.peak_in_use,
              (unsigned long)s.free_blocks);
    return TOOL_OK;
}

static const tool_t heap_stats_tool = {
    .name        = "heap_stats",
    .description =
        "Report live kernel heap statistics: arena size, bytes in use, bytes "
        "free, header overhead, peak usage, live and free block counts, and "
        "lifetime allocation/free counts. Also returns a consistency verdict "
        "derived from the counters alone, which is safe to call at any time. "
        "Use this to answer questions about memory usage. Takes no arguments.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_heap_stats,
};
REGISTER_TOOL(heap_stats_tool);

/* ====================================================================== */
/* heap_check                                                             */
/* ====================================================================== */

static int t_heap_check(const tool_call_t *call, tool_result_t *r) {
    (void)call;

    heap_stats_t s;
    heap_stats(&s);

    /* Pre-flight: if the counters already disagree, the arena is known suspect
     * and walking it would very likely panic. Report instead of dying. */
    const char *bad = mem_tools_stats_verdict(&s);
    if (bad) {
        r->is_error = 1;
        tool_result_printf(r,
            "REFUSED: heap statistics are already inconsistent (%s), so the heap "
            "is corrupt or the counters are. Not walking it: heap_check() panics "
            "on corruption, which would halt this machine and end the session. "
            "Report the inconsistency instead; heap_stats has the full numbers.\n",
            bad);
        mark_truncated(r);
        trace_err(TOOL_EINVAL, "heap_check", "presuspect=%s", bad);
        return TOOL_OK;
    }

    unsigned long blocks = (unsigned long)(s.used_blocks + s.free_blocks);

    /* Breadcrumb before the walk: if it panics, this is the only record of what
     * the machine was doing when it died.
     *
     * NOTE THE CONVERSIONS. This is kprintf() from lib/base.c, whose grammar is
     * "%s %c %d %u %x %p %%" and NOTHING ELSE (see include/kernel.h). It has no
     * 'l' length modifier: on an unrecognised conversion it prints '%' plus the
     * offending character and DOES NOT CONSUME THE VARARG, so "%lu" emits the
     * literal text "%lu" and shifts every later argument by one. This line used
     * to say "%lu" and printed its own format string on real hardware for
     * exactly that reason - a post-mortem breadcrumb that carried no data.
     *
     * The trap is that the OTHER formatter in this kernel, lib/libc_shim.c's
     * vsnprintf (reached via snprintf and tool_result_printf), DOES handle 'l'.
     * The same format string is therefore correct three lines away and silently
     * wrong here. Do not use %l in a kprintf; cast to unsigned instead. Both
     * values are small enough that the cast cannot lose anything: the arena is
     * 16 MiB and a block count cannot exceed it. */
    kprintf("heap_check: walking %u blocks of a %u KiB arena "
            "(panics and halts if corrupt)\n",
            (unsigned)blocks, (unsigned)(s.arena_size / 1024));

    int rc = heap_check();          /* panics — does not return — if corrupt */

    if (rc != 0) {                  /* today unreachable; correct if that changes */
        r->is_error = 1;
        tool_result_printf(r, "heap_check() reported failure code %d\n", rc);
        mark_truncated(r);
        trace_err(rc, "heap_check", "blocks=%lu", blocks);
        return TOOL_OK;
    }

    tool_result_printf(r,
        "heap intact. Walked %lu blocks (%lu live, %lu free) across a %lu KiB "
        "arena. Every block header magic is valid, the doubly-linked physical "
        "list is consistent in both directions, no two adjacent free blocks were "
        "left uncoalesced, and the block sizes sum exactly to the arena size. "
        "Counter invariants also hold.\n",
        blocks, (unsigned long)s.used_blocks, (unsigned long)s.free_blocks,
        (unsigned long)(s.arena_size / 1024));
    mark_truncated(r);

    trace_ok("heap_check", "blocks=%lu arena=%luKiB",
             blocks, (unsigned long)(s.arena_size / 1024));
    return TOOL_OK;
}

static const tool_t heap_check_tool = {
    .name        = "heap_check",
    .description =
        "Walk the entire kernel heap and verify its structural integrity: block "
        "header magic, forward/backward link consistency, absence of uncoalesced "
        "adjacent free blocks, and exact size accounting. Returns a description "
        "of what was verified when the heap is intact. WARNING: if the heap IS "
        "corrupt this call panics and halts the machine - the panic message is "
        "the diagnosis, but the session ends. A cheap, always-safe consistency "
        "verdict is included in heap_stats; prefer that unless a real structural "
        "walk is what is being asked for. Takes no arguments.",
    /* Mutates nothing, but can irreversibly halt the machine: TOOL_MUTATES is
     * the existing hook a confirmation prompt would key off. See the file
     * header for the full rationale. */
    .flags        = TOOL_MUTATES,
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .invoke       = t_heap_check,
};
REGISTER_TOOL(heap_check_tool);

/* ====================================================================== */
/* sys_uptime                                                             */
/* ====================================================================== */

static int t_sys_uptime(const tool_call_t *call, tool_result_t *r) {
    (void)call;

    uint64_t      ms   = millis();
    unsigned long secs = (unsigned long)(ms / 1000u);
    unsigned long h    = secs / 3600u;
    unsigned long m    = (secs / 60u) % 60u;
    unsigned long sec  = secs % 60u;
    unsigned long frac = (unsigned long)(ms % 1000u);

    tool_result_printf(r,
        "uptime: %luh %02lum %02lus.%03lu (%lu ms since time_init)\n",
        h, m, sec, frac, (unsigned long)ms);
    mark_truncated(r);

    trace_ret((long)ms, "sys_uptime", "up=%luh%02lum%02lus", h, m, sec);
    return TOOL_OK;
}

static const tool_t sys_uptime_tool = {
    .name        = "sys_uptime",
    .description =
        "Report how long this machine has been running, as milliseconds since "
        "the timer was calibrated at boot plus a human-readable rendering. "
        "Takes no arguments.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_sys_uptime,
};
REGISTER_TOOL(sys_uptime_tool);

/* ====================================================================== */
/* mem_map                                                                */
/* ====================================================================== */

static int t_mem_map(const tool_call_t *call, tool_result_t *r) {
    (void)call;

    uint64_t lo, hi;
    mem_window(&lo, &hi);

    heap_stats_t s;
    heap_stats(&s);

    char a_lo[MEM_ADDR_TEXT_MAX], a_hi[MEM_ADDR_TEXT_MAX];
    fmt_addr(a_lo, lo);
    fmt_addr(a_hi, hi);

#ifndef FABLEOS_HOSTTEST
    char a_bss[MEM_ADDR_TEXT_MAX];
    fmt_addr(a_bss, (uint64_t)(uintptr_t)__bss_start);
    tool_result_printf(r,
        "kernel image: %s .. %s (%lu KiB), linked at 1 MiB by linker.ld\n"
        "  .bss spans %s .. %s - it holds the page tables, the 64 KiB kernel "
        "stack, and the heap arena\n",
        a_lo, a_hi, (unsigned long)((hi - lo) / 1024), a_bss, a_hi);
#else
    tool_result_printf(r, "kernel image: %s .. %s (%lu KiB)\n",
                       a_lo, a_hi, (unsigned long)((hi - lo) / 1024));
#endif

    tool_result_printf(r,
        "heap arena:   %lu KiB, allocated inside .bss (see heap_stats)\n"
        "identity map: 0x0 .. 0x100000000 (low 4 GiB, 2 MiB RWX pages, boot.asm) "
        "with no memory protection\n"
        "mem_read window: %s .. %s, max %lu bytes per call. Reads outside this "
        "window are refused: the window is the only region proven to be RAM the "
        "kernel already wrote, and everything outside it may be device MMIO whose "
        "registers change when read.\n",
        (unsigned long)(s.arena_size / 1024),
        a_lo, a_hi, (unsigned long)MEM_READ_MAX);
    mark_truncated(r);

    trace_ok("mem_map", "window=%s-%s size=%luKiB",
             a_lo, a_hi, (unsigned long)((hi - lo) / 1024));
    return TOOL_OK;
}

static const tool_t mem_map_tool = {
    .name        = "mem_map",
    .description =
        "Report the kernel's physical memory layout: where the kernel image "
        "begins and ends, where .bss (page tables, kernel stack, heap arena) "
        "lives, how much of the address space is identity-mapped, and exactly "
        "which address range mem_read will accept. Call this before mem_read to "
        "find valid addresses. Takes no arguments.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_mem_map,
};
REGISTER_TOOL(mem_map_tool);

/* ====================================================================== */
/* mem_read                                                               */
/* ====================================================================== */

/* Parse a bounded, NUL-terminated address literal: optional whitespace, optional
 * "0x"/"0X" prefix (hex) else decimal, at least one digit, optional trailing
 * whitespace, nothing else. Overflow is rejected, never wrapped.
 * Returns 0 on success. */
static int parse_u64(const char *s, uint64_t *out) {
    if (!s) return -1;

    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;

    unsigned base = 10;
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }

    uint64_t v      = 0;
    int      digits = 0;
    for (; s[i]; i++) {
        unsigned d;
        char     c = s[i];
        if      (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10u;
        else break;
        if (d >= base) break;
        if (v > (UINT64_MAX - d) / base) return -1;      /* overflow */
        v = v * base + d;
        digits++;
    }
    if (digits == 0) return -1;

    /* Tolerate trailing whitespace, reject trailing junk ("0x1000 bytes"). */
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] != '\0') return -1;

    *out = v;
    return 0;
}

/* Render one 16-byte hexdump row. Returns the length written (NUL-terminated).
 * `out` must hold at least 96 bytes; `n` must be 1..16. */
static size_t hexdump_row(char *out, uint64_t addr,
                          const unsigned char *p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    size_t o = 0;

    fmt_addr_wide(out, addr);
    o = 18;                                   /* "0x" + 16 digits */
    out[o++] = ' ';
    out[o++] = ' ';

    for (size_t i = 0; i < 16; i++) {
        if (i == 8) out[o++] = ' ';           /* mid-row gutter */
        if (i < n) {
            out[o++] = hx[(p[i] >> 4) & 0xF];
            out[o++] = hx[p[i] & 0xF];
        } else {
            out[o++] = ' ';
            out[o++] = ' ';
        }
        out[o++] = ' ';
    }

    out[o++] = '|';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[o++] = '|';
    out[o++] = '\n';
    out[o]   = '\0';
    return o;
}

static int mem_read_fail(tool_result_t *r, const char *why, const char *detail) {
    uint64_t lo, hi;
    mem_window(&lo, &hi);
    char a_lo[MEM_ADDR_TEXT_MAX], a_hi[MEM_ADDR_TEXT_MAX];
    fmt_addr(a_lo, lo);
    fmt_addr(a_hi, hi);

    r->is_error = 1;
    tool_result_printf(r,
        "mem_read refused: %s%s%s. Expected an object like "
        "{\"address\":\"0x100000\",\"length\":64}: address is a hex (0x-prefixed) "
        "or decimal string, or a non-negative integer, and must lie in the "
        "readable window %s .. %s; length is an integer from 1 to %lu. "
        "Call mem_map for the current layout.\n",
        why, detail ? " " : "", detail ? detail : "",
        a_lo, a_hi, (unsigned long)MEM_READ_MAX);
    mark_truncated(r);
    trace_err(TOOL_EINVAL, "mem_read", "%s", why);
    return TOOL_OK;
}

static int t_mem_read(const tool_call_t *call, tool_result_t *r) {
    if (!call || !call->input || call->input_len == 0)
        return mem_read_fail(r, "no arguments given", NULL);

    json_value_t root;
    if (json_parse(call->input, call->input_len, &root) != JSON_OK)
        return mem_read_fail(r, "arguments are not valid JSON", NULL);
    if (root.type != JSON_OBJECT)
        return mem_read_fail(r, "arguments are not a JSON object", NULL);

    /* ---- address: string (hex/decimal) or non-negative integer ---- */
    json_value_t av;
    if (json_get(&root, "address", &av) != JSON_OK)
        return mem_read_fail(r, "missing required field \"address\"", NULL);

    uint64_t addr = 0;
    if (av.type == JSON_STRING) {
        char text[MEM_ADDR_TEXT_MAX];
        if (json_str(&av, text, sizeof text, NULL) != JSON_OK)
            return mem_read_fail(r, "\"address\" string is too long", NULL);
        if (parse_u64(text, &addr) != 0)
            return mem_read_fail(r, "\"address\" is not a valid number", NULL);
    } else if (av.type == JSON_NUMBER) {
        int64_t iv;
        if (json_int(&av, &iv) != JSON_OK)
            return mem_read_fail(r, "\"address\" is not an integer", NULL);
        if (iv < 0)
            return mem_read_fail(r, "\"address\" is negative", NULL);
        addr = (uint64_t)iv;
    } else {
        return mem_read_fail(r, "\"address\" must be a string or an integer", NULL);
    }

    /* ---- length: optional integer in [1, MEM_READ_MAX] ---- */
    uint64_t     len = MEM_READ_DEFAULT;
    json_value_t lv;
    int          lrc = json_get(&root, "length", &lv);
    if (lrc == JSON_OK) {
        if (lv.type != JSON_NUMBER)
            return mem_read_fail(r, "\"length\" must be an integer", NULL);
        int64_t iv;
        if (json_int(&lv, &iv) != JSON_OK)
            return mem_read_fail(r, "\"length\" is not a representable integer", NULL);
        if (iv < 1)
            return mem_read_fail(r, "\"length\" must be at least 1", NULL);
        if (iv > (int64_t)MEM_READ_MAX)
            return mem_read_fail(r, "\"length\" exceeds the 256-byte per-call cap",
                                 NULL);
        len = (uint64_t)iv;
    } else if (lrc != JSON_ENOENT) {
        return mem_read_fail(r, "\"length\" could not be read", NULL);
    }

    /* ---- the bound. Nothing below this line runs unless it passes. ---- */
    if (window_allows(addr, len) != 0) {
        char a[MEM_ADDR_TEXT_MAX];
        fmt_addr(a, addr);
        char detail[80];
        snprintf(detail, sizeof detail, "(asked for %lu bytes at %s)",
                 (unsigned long)len, a);
        return mem_read_fail(r, "address range is outside the readable window",
                             detail);
    }

    /* Copy out through a volatile byte pointer, then let go of the raw pointer:
     * the compiler cannot widen or reorder the access, and formatting works on a
     * local snapshot. */
    unsigned char snap[MEM_READ_MAX];
    {
        const volatile unsigned char *p =
            (const volatile unsigned char *)(uintptr_t)addr;
        for (uint64_t i = 0; i < len; i++) snap[i] = p[i];
    }

    char a_start[MEM_ADDR_TEXT_MAX];
    fmt_addr(a_start, addr);
    tool_result_printf(r, "%lu bytes at %s:\n", (unsigned long)len, a_start);

    uint64_t shown = 0;
    int      clipped = 0;
    for (uint64_t off = 0; off < len; off += 16) {
        size_t n = (size_t)((len - off < 16) ? (len - off) : 16);
        char   row[96];
        size_t rl = hexdump_row(row, addr + off, snap + off, n);

        /* Stop while there is still room to say that we stopped. */
        if (r->len + rl + MEM_TAIL_RESERVE >= r->cap) { clipped = 1; break; }
        tool_result_printf(r, "%s", row);
        shown += n;
    }

    if (clipped) {
        char a_next[MEM_ADDR_TEXT_MAX];
        fmt_addr(a_next, addr + shown);
        tool_result_printf(r,
            "[TRUNCATED: showed %lu of the %lu bytes requested. "
            "Call mem_read again with address %s to continue.]\n",
            (unsigned long)shown, (unsigned long)len, a_next);
    }
    mark_truncated(r);

    char a_trace[MEM_ADDR_TEXT_MAX];
    fmt_addr(a_trace, addr);
    trace_ret((long)shown, "mem_read", "addr=%s len=%lu",
              a_trace, (unsigned long)len);
    return TOOL_OK;
}

static const tool_t mem_read_tool = {
    .name        = "mem_read",
    .description =
        "Read raw kernel memory and return it as a hex + ASCII dump, 16 bytes "
        "per line. Read-only and bounded: at most 256 bytes per call, and only "
        "from inside the kernel image window reported by mem_map (the loaded "
        "image plus .bss, which holds the kernel stack and the heap arena). "
        "Addresses outside that window are refused rather than read, because "
        "this kernel has no memory protection and no fault handling. Use it to "
        "inspect heap block headers, stack contents, globals, or code bytes when "
        "diagnosing a problem.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"address\":{\"type\":\"string\","
        "\"description\":\"Address to read. Hex with a 0x prefix (e.g. 0x100000) "
        "or plain decimal. Must be inside the window reported by mem_map.\"},"
        "\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256,"
        "\"description\":\"Number of bytes to read. Defaults to 64.\"}},"
        "\"required\":[\"address\"]}",
    .flags        = 0,
    .invoke       = t_mem_read,
};
REGISTER_TOOL(mem_read_tool);
