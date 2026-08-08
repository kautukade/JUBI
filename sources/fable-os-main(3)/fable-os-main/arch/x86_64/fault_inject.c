/* fault_inject.c — deliberately break the machine, for testing the IDT.
 *
 * ============================================================================
 * THIS FILE EXISTS ONLY TO CAUSE FAULTS. EVERY FUNCTION IN IT IS A BUG ON
 * PURPOSE. NOTHING IN THE NORMAL BOOT PATH MAY EVER CALL INTO IT.
 * ============================================================================
 *
 * WHY IT HAS TO EXIST
 *   An exception handler that has never handled an exception is a hypothesis.
 *   Before this file, the only way to find out whether the IDT worked was to
 *   wait for a real bug, at which point the thing being debugged is also the
 *   thing doing the debugging. So the faults are manufactured, one per class,
 *   and the report for each is checked by eye and by the QEMU suite.
 *
 * HOW IT IS KEPT OUT OF REACH
 *   Three independent locks, because a machine whose only interface is natural
 *   language must not have a "kill yourself" verb sitting one hallucination
 *   away from the model:
 *
 *     1. No tool registers it. tools/fault_tools.c is read-only by design and
 *        says so; the model cannot reach any of this.
 *     2. Nothing calls fault_inject() in a normal build. The only call site is
 *        fault_inject_selftest(), which is compiled out entirely unless
 *        FABLEOS_FAULT_TEST is defined on the command line
 *        (`make EXTRA_CFLAGS=-DFABLEOS_FAULT_TEST=FAULT_INJECT_PF_WRITE`).
 *     3. Even with the flag, the self-test announces itself first, so a log can
 *        never be mistaken for a real crash.
 *
 * WHY A NULL DEREFERENCE IS NOT ONE OF THE OPTIONS
 *   The obvious way to raise #PF is `*(int *)0 = 1`. On this machine that does
 *   nothing at all: boot/boot.asm identity-maps the low 4 GiB with
 *   present+writable 2 MiB huge pages, so virtual address 0 is ordinary
 *   writable RAM. Worse, it cannot simply be unmapped — address 0 shares its
 *   2 MiB huge page with the VGA text framebuffer at 0xB8000, so unmapping it
 *   would take the console with it, and a later phase depends on those pages
 *   staying RWX.
 *
 *   So the page-fault injectors use FAULT_PF_ADDR: an address above the 4 GiB
 *   identity map but still canonical (bits 63:47 all zero), which is the
 *   smallest possible deviation from "a null pointer" that actually faults.
 *   Read, write and execute variants exist because the page-fault error code's
 *   W/R and I/D bits are the two most useful bits in it and are worth proving
 *   independently.
 *
 * WHY EACH FAULT IS RAISED THE WAY IT IS
 *   #DE  a real IDIV by a volatile zero, not a compiler-folded constant.
 *   #BP  int3 — the one continuable trap, so the report is followed by the
 *        machine carrying on, which proves IRETQ restores state correctly.
 *   #UD  ud2, the architecturally guaranteed invalid opcode.
 *   #GP  a selector one slot past the end of the GDT. A non-canonical access
 *        would also do it, but this way the error code is non-zero and the
 *        report's selector decoding gets exercised.
 *   #DF  set RSP to a non-canonical value and push. The push raises #SS; the
 *        CPU then cannot push the #SS frame either, which is the definition of
 *        a double fault. Before the IST existed this was an instant triple
 *        fault and a silent VM reset; it is the single best proof that the IST
 *        works.
 *
 * THE CALL DEPTH IS DELIBERATE
 *   Every injection goes through three noinline frames (depth3 -> depth2 ->
 *   depth1 -> the fault) so the backtrace in the report has real, checkable
 *   structure instead of a single frame.
 */

#include "fault.h"
#include "kernel.h"

#include <stdint.h>

/* Canonical (bits 63:47 are all zero) but far above the 4 GiB that boot.asm
 * identity-maps, so the page really is not present. 64 GiB. */
#define FAULT_PF_ADDR 0x0000001000000000ULL

const char *fault_inject_name(fault_inject_kind_t kind) {
    switch (kind) {
        case FAULT_INJECT_NONE:     return "none";
        case FAULT_INJECT_DE:       return "#DE divide by zero";
        case FAULT_INJECT_BP:       return "#BP int3 (continuable)";
        case FAULT_INJECT_UD:       return "#UD invalid opcode";
        case FAULT_INJECT_DF:       return "#DF fault on a broken stack";
        case FAULT_INJECT_GP:       return "#GP selector past the GDT limit";
        case FAULT_INJECT_PF_READ:  return "#PF read of an unmapped page";
        case FAULT_INJECT_PF_WRITE: return "#PF write to an unmapped page";
        case FAULT_INJECT_PF_EXEC:  return "#PF instruction fetch from an unmapped page";
        default:                    return "unknown";
    }
}

/* `volatile` and `noinline` throughout: without them the optimiser (or even
 * -O0's constant folding) is entitled to notice that these are all undefined
 * behaviour and emit nothing, or emit a ud2 of its own choosing at a different
 * address, which would make the reports lies. */

static volatile int   zero_divisor = 0;
static volatile int   one          = 1;
static volatile void *sink;

__attribute__((noinline)) static void do_de(void) {
    volatile int q = one / zero_divisor;      /* IDIV with a zero divisor */
    sink = (void *)(uintptr_t)q;
}

__attribute__((noinline)) static void do_bp(void) {
    __asm__ volatile("int3");
}

__attribute__((noinline)) static void do_ud(void) {
    __asm__ volatile("ud2");
}

__attribute__((noinline)) static void do_gp(void) {
    /* The GDT idt_init() builds has five slots, so its limit is 0x27. Selector
     * 0x30 is past the end: loading it raises #GP with the selector as the
     * error code. Loading DS rather than CS keeps it a clean, single-fault
     * event. */
    __asm__ volatile("movl $0x30, %%eax\n\t"
                     "movl %%eax, %%ds"
                     : : : "eax");
}

__attribute__((noinline)) static void do_df(void) {
    /* 0xDEAD000000000000 is non-canonical, so the PUSH cannot complete and the
     * CPU cannot push the resulting #SS frame either. RSP is destroyed by
     * design; the handler halts, so nothing has to put it back. */
    __asm__ volatile("movabsq $0xDEAD000000000000, %%rsp\n\t"
                     "pushq   $0"
                     : : : "memory");
}

__attribute__((noinline)) static void do_pf_read(void) {
    sink = (void *)(uintptr_t)*(const volatile uint64_t *)FAULT_PF_ADDR;
}

__attribute__((noinline)) static void do_pf_write(void) {
    *(volatile uint64_t *)FAULT_PF_ADDR = 0x1234;
}

__attribute__((noinline)) static void do_pf_exec(void) {
    void (*fn)(void) = (void (*)(void))(uintptr_t)FAULT_PF_ADDR;
    fn();
}

__attribute__((noinline)) static void depth1(fault_inject_kind_t k) {
    switch (k) {
        case FAULT_INJECT_DE:       do_de();       break;
        case FAULT_INJECT_BP:       do_bp();       break;
        case FAULT_INJECT_UD:       do_ud();       break;
        case FAULT_INJECT_DF:       do_df();       break;
        case FAULT_INJECT_GP:       do_gp();       break;
        case FAULT_INJECT_PF_READ:  do_pf_read();  break;
        case FAULT_INJECT_PF_WRITE: do_pf_write(); break;
        case FAULT_INJECT_PF_EXEC:  do_pf_exec();  break;
        default: break;
    }
}

__attribute__((noinline)) static void depth2(fault_inject_kind_t k) { depth1(k); }
__attribute__((noinline)) static void depth3(fault_inject_kind_t k) { depth2(k); }

void fault_inject(fault_inject_kind_t kind) {
    if (kind == FAULT_INJECT_NONE) return;
    kprintf("\n[fault-inject] about to raise: %s\n"
            "[fault-inject] this is a TEST. Nothing is wrong with the machine.\n",
            fault_inject_name(kind));
    depth3(kind);
    /* Reached in exactly two cases, and the distinction is the whole point of
     * The recovery path: either the exception was a continuable trap (#BP), or it was a
     * real fault that a recovery plan resumed from. fault_last() knows which,
     * so say which — a log that called a recovered #PF "continuable" would
     * misrepresent the one thing this build exists to demonstrate. */
    const fault_record_t *rec = fault_last();
    if (rec && rec->fix_set && rec->fix == FAULT_FIX_OK)
        kprintf("[fault-inject] returned from %s - the fault was RECOVERED "
                "(%s) and IRETQ resumed at %p\n",
                fault_inject_name(kind), rec->fix_detail,
                (void *)(uintptr_t)rec->resume_rip);
    else
        kprintf("[fault-inject] returned from %s - the trap was continuable "
                "and IRETQ restored execution\n", fault_inject_name(kind));
}

/* ====================================================================== */
/* the build-flag-gated boot self-test                                    */
/* ====================================================================== */

#ifdef FABLEOS_FAULT_TEST
/* Compiled in ONLY for a deliberate diagnostic build:
 *     make EXTRA_CFLAGS=-DFABLEOS_FAULT_TEST=FAULT_INJECT_PF_WRITE
 * A normal build contains none of the code below and registers nothing.
 *
 * The hook is a DRV_LEVEL_LATE driver rather than a call bolted into
 * kernel_main() for three reasons:
 *   - It needs no edit to any shared file. The driver table is self-
 *     registering (driver.h), exactly like the tool table, so the whole
 *     mechanism is confined to this one file.
 *   - It runs late in drivers_init(), which is AFTER the serial console has
 *     bound to COM1. A fault raised any earlier would only reach the VGA text
 *     buffer and would be missing from every serial log and every QEMU test —
 *     the report would exist but nothing could capture it.
 *   - It gives the fault a real call stack (drivers_init -> init -> ... ) so
 *     the backtrace in the report has genuine depth to reconstruct rather than
 *     being raised from the top of kernel_main.
 *
 * RECOVERY SCENARIOS
 *   Adding -DFABLEOS_FAULT_RECOVER=N arms a plan before the injection, so the
 *   whole Phase 3b path — arm, fault, validate, patch the live frame, IRETQ,
 *   keep booting — can be watched end to end on real hardware. Each N exists to
 *   demonstrate exactly one claim, and the claim it demonstrates is printed
 *   before the fault so a log can never be misread:
 *
 *     1  skip            a #PF is stepped over and the machine reaches the
 *                        prompt. Before Phase 3b this halted.
 *     2  set + skip      a register is written and then the instruction is
 *                        stepped over. Proves the register really is patched
 *                        in the live frame.
 *     3  rip out of      a set_rip target outside [code_lo, code_hi) is refused
 *        window          at arm time, and the fault then halts as it should.
 *     4  refault loop    "set a register and retry" with a value that does not
 *                        fix anything. Re-faults at the same RIP; the kernel
 *                        must give up after FAULT_REFAULT_MAX attempts instead
 *                        of spinning forever.
 *     5  ring            two faults with a fault_report tool call held open
 *                        across the second one. Proves the first record is not
 *                        rewritten under its reader.
 *     6  diagnosis       the Phase 3b-2 loop, end to end: a #PF is survived, the
 *                        KERNEL asks the model what it was, the model answers
 *                        with a diagnosis and a proposed fix, the fix is armed
 *                        through the real fault_recover tool, and a SECOND #PF
 *                        is then handled by the model's own plan. The machine
 *                        boots on to the prompt.
 *     7  no recursion    a fault raised from INSIDE the transport, while a
 *                        diagnosis request is in flight. Proves the reply is
 *                        discarded, no fix is armed, diagnosis latches off for
 *                        the rest of the boot, and the network stack is never
 *                        re-entered.
 *
 *   Scenarios 6 and 7 need a model, and there is no API key. They use a scripted
 *   transport defined below — the same model_transport_t seam net/net.c
 *   implements, with a canned reply instead of TLS. It is compiled ONLY into
 *   these builds; a normal kernel contains none of it, and net/model_mock.c is
 *   deliberately not linked into the kernel (its 512 KiB of request buffers are
 *   a host-test luxury). What this proves is every line of net/faultchat.c
 *   running on real hardware, with a real fault, through the real tool
 *   registry. What it cannot prove is what a real model would actually say.
 */
#include "driver.h"

#ifndef FABLEOS_FAULT_RECOVER
#define FABLEOS_FAULT_RECOVER 0
#endif

#if FABLEOS_FAULT_RECOVER != 0
static void announce(const char *claim) {
    kprintf("\n[fault-recover] SCENARIO %d: %s\n", (int)FABLEOS_FAULT_RECOVER,
            claim);
}

static void arm(const fault_plan_t *p, const char *what) {
    fault_fix_t rc = fault_plan_arm(p);
    if (rc == FAULT_FIX_OK)
        kprintf("[fault-recover] arm(%s) -> ACCEPTED\n", what);
    else
        kprintf("[fault-recover] arm(%s) -> REFUSED: %s - %s\n", what,
                fault_fix_name(rc), fault_fix_desc(rc));
}

#if FABLEOS_FAULT_RECOVER == 5
#include "tool.h"

/* Invoke a real tool through the real registry, into a caller-owned buffer, so
 * scenario 5 holds a genuinely rendered fault_report answer across a second
 * fault rather than a hand-made imitation of one. */
static size_t call_report(uint32_t seq, char *buf, size_t cap) {
    char args[40];
    size_t n = 0;
    /* Hand-built rather than snprintf'd: this runs in a diagnostic build and
     * the fewer moving parts between the fault and the evidence, the better. */
    static const char pre[] = "{\"seq\":";
    for (const char *p = pre; *p; p++) args[n++] = *p;
    char dig[12];
    int  d = 0;
    if (seq == 0) dig[d++] = '0';
    while (seq) { dig[d++] = (char)('0' + (seq % 10)); seq /= 10; }
    while (d) args[n++] = dig[--d];
    args[n++] = '}';
    args[n]   = '\0';

    tool_result_t r;
    tool_result_init(&r, buf, cap);
    tool_call_t c = { .id = "toolu_selftest", .name = "fault_report",
                      .input = args, .input_len = n };
    if (tool_dispatch(&c, &r) != TOOL_OK) { buf[0] = '\0'; return 0; }
    return r.len;
}

static char ring_buf_a[2048];
static char ring_buf_b[2048];
#endif  /* FABLEOS_FAULT_RECOVER == 5 */

#if FABLEOS_FAULT_RECOVER == 6 || FABLEOS_FAULT_RECOVER == 7
#include "faultchat.h"
#include "model.h"

/* ---------------------------------------------------------------------
 * A scripted model, on real hardware.
 *
 * There is no API key, so the far end of the transport seam is a canned
 * Messages API response instead of TLS to api.anthropic.com. EVERYTHING on this
 * side of the seam is the shipping code: net/faultchat.c builds the request,
 * model.c serialises it, json.c parses the reply, and the proposed fix is armed
 * by dispatching the real fault_recover tool out of the real registry.
 * ------------------------------------------------------------------- */

/* What the "model" says. The outer object is a Messages API response; the text
 * block contains the JSON object faultchat.h's contract asks for. */
static const char SCRIPTED_REPLY[] =
    "{\"id\":\"msg_scripted\",\"type\":\"message\",\"role\":\"assistant\","
    "\"content\":[{\"type\":\"text\",\"text\":"
    "\"{\\\"diagnosis\\\":\\\"The instruction at RIP is a 7-byte store through "
    "rax, and rax holds 0x1000000000 - 64 GiB, far above the 4 GiB that "
    "boot.asm identity-maps, so that page is simply not present. Nothing "
    "consumes the stored value, so stepping over the store is "
    "safe.\\\",\\\"fix\\\":{\\\"action\\\":\\\"skip\\\",\\\"vector\\\":14,"
    "\\\"uses\\\":2}}\""
    "}],\"stop_reason\":\"end_turn\"}";

static int    script_entries;      /* how many times the transport was used */
static int    script_fault;        /* raise a fault from inside send()      */

static int mem_has(const char *hay, size_t n, const char *needle) {
    size_t m = 0;
    while (needle[m]) m++;
    if (m == 0 || n < m) return 0;
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && hay[i + k] == needle[k]) k++;
        if (k == m) return 1;
    }
    return 0;
}

/* Assert, out loud and in the log, that the request the kernel built actually
 * carries what a model needs. A log that only said "request sent" would prove
 * nothing about its contents. */
static void report_carries(const char *req, size_t n, const char *what,
                           const char *needle) {
    kprintf("[fault-diagnose]   carries %s : %s\n", what,
            mem_has(req, n, needle) ? "yes" : "NO <-- MISSING");
}

static int script_send(model_transport_t *t,
                       const char *req_body, size_t req_len,
                       char *resp_buf, size_t resp_cap,
                       model_response_t *out) {
    (void)t;
    /* model_transport_t promises nothing about the buffer, and `resp_cap - 1`
     * below is a size_t: a zero cap would wrap it to SIZE_MAX and run the copy
     * off the end of memory. Refuse instead, exactly as the real transport
     * would. */
    if (!resp_buf || resp_cap == 0 || !out) return MODEL_EINVAL;

    script_entries++;

    kprintf("[fault-diagnose] scripted transport entered (call %d), request is "
            "%u bytes\n", script_entries, (unsigned)req_len);
    report_carries(req_body, req_len, "the exception    ",
                   "KERNEL FAULT: #PF (vector 14) - Page Fault");
    report_carries(req_body, req_len, "the decoded error",
                   "page not present, write, kernel-mode access");
    report_carries(req_body, req_len, "the faulting RIP ", "RIP        : 0x");
    report_carries(req_body, req_len, "the CR2 address  ",
                   "CR2        : 0x0000001000000000");
    report_carries(req_body, req_len, "the code bytes   ",
                   "48 c7 00 34 12 00 00");
    report_carries(req_body, req_len, "the registers    ", "R15 0x");
    report_carries(req_body, req_len, "the backtrace    ", "backtrace  : RBP");
    report_carries(req_body, req_len, "memory near RIP  ", "MEMORY AROUND RIP");
    report_carries(req_body, req_len, "the answer shape ",
                   "ANSWER WITH ONE JSON OBJECT");

    if (script_fault) {
        kputs("[fault-diagnose] the transport is now going to FAULT, exactly as "
              "lwIP or mbedTLS would if the first fault had left the heap "
              "mid-mutation.\n");
        do_pf_read();
        kputs("[fault-diagnose] the transport survived its own fault (an armed "
              "skip plan stepped over it).\n");
        /* And now the naive reaction to that new fault: diagnose it right here,
         * from inside the transport that is still running. This is the call
         * that would recurse through a non-reentrant network stack, and the
         * re-entrancy latch is the only thing stopping it. */
        kprintf("[fault-diagnose] a nested faultchat_pump() from INSIDE the "
                "transport returns %d (must be %d, EBUSY)\n",
                faultchat_pump(), FAULTCHAT_EBUSY);
        kputs("[fault-diagnose] the transport is returning a perfectly good "
              "reply anyway. It must still be thrown away.\n");
    }

    size_t n = sizeof SCRIPTED_REPLY - 1;
    if (n >= resp_cap) n = resp_cap - 1;
    for (size_t i = 0; i < n; i++) resp_buf[i] = SCRIPTED_REPLY[i];
    resp_buf[n] = '\0';

    out->http_status = 200;
    {
        static const char line[] = "HTTP/1.1 200 OK";
        size_t i = 0;
        for (; i < sizeof line - 1 && i + 1 < sizeof out->status_line; i++)
            out->status_line[i] = line[i];
        out->status_line[i] = '\0';
    }
    out->body      = resp_buf;
    out->body_len  = n;
    out->truncated = 0;
    return MODEL_OK;
}

static const char *script_last_error(model_transport_t *t) {
    (void)t;
    return "";
}

static model_transport_t scripted_model = {
    "scripted-mock", script_send, script_last_error, (void *)0
};

/* The armed plan, in words, using only the format specifiers lib/base.c's
 * kprintf actually implements (no 'l' modifier — see include/kernel.h). */
static void show_plan(const char *when) {
    const fault_plan_t *p = fault_plan_get();
    if (!p) {
        kprintf("[fault-diagnose] armed plan %s: NONE\n", when);
        return;
    }
    kprintf("[fault-diagnose] armed plan %s: action=%u vector=%s uses=%u\n",
            when, (unsigned)p->action,
            p->any_vector ? "any" : fault_vector_name(p->vector),
            (unsigned)p->uses);
}
#endif  /* FABLEOS_FAULT_RECOVER == 6 || 7 */
#endif  /* FABLEOS_FAULT_RECOVER != 0 */

static int fault_inject_driver_init(void) {
    kputs("\n[fault-inject] FABLEOS_FAULT_TEST build: raising one fault on "
          "purpose to exercise the IDT.\n");

#if FABLEOS_FAULT_RECOVER == 1
    announce("a #PF is SKIPPED and the machine keeps booting to the prompt");
    {
        fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 0,
                           .vector = 14, .uses = 1 };
        arm(&p, "skip #PF once");
    }
    fault_inject(FABLEOS_FAULT_TEST);
    kputs("[fault-recover] RESULT: control returned past the faulting "
          "instruction. Boot continues.\n");

#elif FABLEOS_FAULT_RECOVER == 2
    announce("a register is WRITTEN in the live frame, then the instruction "
             "is skipped");
    {
        fault_plan_t p = { .action = FAULT_ACT_SET_REG, .reg = FAULT_REG_RAX,
                           .then_skip = 1, .any_vector = 0, .vector = 14,
                           .uses = 1, .value = 0x00000000CAFEBABEULL };
        arm(&p, "set rax=0xCAFEBABE then skip");
    }
    fault_inject(FABLEOS_FAULT_TEST);
    kputs("[fault-recover] RESULT: control returned past the faulting "
          "instruction. Boot continues.\n");

#elif FABLEOS_FAULT_RECOVER == 3
    announce("a set_rip target OUTSIDE the kernel image must be REFUSED");
    {
        const fault_window_t *w = fault_window();
        kprintf("[fault-recover] resume window is [%p, %p)\n",
                (void *)(uintptr_t)(w ? w->code_lo : 0),
                (void *)(uintptr_t)(w ? w->code_hi : 0));
        /* 64 GiB: canonical, unmapped, and nowhere near the loaded image. */
        fault_plan_t bad = { .action = FAULT_ACT_SET_RIP, .any_vector = 1,
                             .uses = 1, .value = FAULT_PF_ADDR };
        arm(&bad, "set_rip 0x1000000000 (outside the image)");
        /* One byte past the end of the image is just as wrong, and is the case
         * an off-by-one in the bounds test would let through. */
        fault_plan_t edge = bad;
        edge.value = w ? w->code_hi : 0;
        arm(&edge, "set_rip code_hi (one past the end)");
        /* The same target, but inside the image, IS accepted — so the refusals
         * above are the window doing its job, not the tool refusing everything.
         * Immediately disarmed: this build is here to prove a refusal. */
        fault_plan_t good = bad;
        good.value = w ? w->code_lo + 0x100 : 0;
        arm(&good, "set_rip code_lo+0x100 (inside the image)");
        fault_plan_clear();
        kputs("[fault-recover] plan cleared; the injected fault must now "
              "HALT.\n");
    }
    fault_inject(FABLEOS_FAULT_TEST);
    kputs("[fault-recover] BUG: this line must never be reached.\n");

#elif FABLEOS_FAULT_RECOVER == 4
    announce("a recovery that does not work must NOT loop forever");
    {
        /* Put the same unmapped address back into the register the faulting
         * instruction dereferences, and re-execute. This can never succeed; the
         * same-RIP counter is the only thing that stops it. */
        fault_plan_t p = { .action = FAULT_ACT_SET_REG, .reg = FAULT_REG_RAX,
                           .then_skip = 0,               /* retry, not skip */
                           .any_vector = 1,
                           .uses = FAULT_PLAN_USES_MAX,
                           .value = FAULT_PF_ADDR };
        arm(&p, "set rax=0x1000000000 and RETRY, 16 uses");
        kprintf("[fault-recover] the kernel must give up after %d attempts at "
                "one address.\n", (int)FAULT_REFAULT_MAX);
    }
    fault_inject(FABLEOS_FAULT_TEST);
    kputs("[fault-recover] BUG: this line must never be reached.\n");

#elif FABLEOS_FAULT_RECOVER == 5
    announce("a fault during a fault_report call must not rewrite the record "
             "being reported");
    {
        fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1,
                           .uses = FAULT_PLAN_USES_MAX };
        arm(&p, "skip, 16 uses");
    }
    fault_inject(FABLEOS_FAULT_TEST);              /* fault #1 */
    {
        size_t na = call_report(1, ring_buf_a, sizeof ring_buf_a);
        kprintf("[fault-recover] read report of fault #1 (%u bytes) and held "
                "it.\n", (unsigned)na);

        fault_inject(FAULT_INJECT_PF_READ);       /* fault #2, mid-diagnosis */
        fault_inject(FAULT_INJECT_UD);            /* fault #3               */

        size_t nb = call_report(1, ring_buf_b, sizeof ring_buf_b);
        int    same = (na == nb);
        for (size_t i = 0; same && i < na; i++)
            if (ring_buf_a[i] != ring_buf_b[i]) same = 0;

        kprintf("[fault-recover] faults so far: %u; newest is #%u (%s)\n",
                (unsigned)fault_count(),
                (unsigned)(fault_last() ? fault_last()->seq : 0),
                fault_last() ? fault_vector_name(fault_last()->regs.vector)
                             : "-");
        kprintf("[fault-recover] RESULT: report of fault #1 re-read after two "
                "further faults is %s (%u vs %u bytes)\n",
                same ? "BYTE-IDENTICAL" : "DIFFERENT - THE RING FAILED",
                (unsigned)na, (unsigned)nb);
    }

#elif FABLEOS_FAULT_RECOVER == 6
    announce("the kernel SURVIVES a fault, ASKS the model what it was, and "
             "arms the fix that comes back");
    {
        /* Bind first: faultchat_bind() clears the module's high-water fault
         * count, so the fault below is unambiguously new to it. */
        faultchat_bind(&scripted_model);

        /* The bootstrap plan. Its only job is to get the machine past the first
         * fault and back into normal kernel context, which is the only place a
         * diagnosis may be run from. One use, so it is spent immediately and
         * whatever the model proposes is not competing with it. */
        fault_plan_t boot = { .action = FAULT_ACT_SKIP, .any_vector = 0,
                              .vector = 14, .uses = 1 };
        arm(&boot, "bootstrap: skip one #PF, purely to survive long enough to "
                   "ask");
    }
    fault_inject(FABLEOS_FAULT_TEST);              /* fault #1 */
    show_plan("after the bootstrap plan was spent");

    kputs("\n[fault-diagnose] back in normal kernel context. The exception "
          "handler ran no part of what follows.\n");
    {
        int rc = faultchat_pump();
        kprintf("[fault-diagnose] pump returned %d (%s)\n", rc,
                faultchat_strerror(rc));
        kprintf("[fault-diagnose] diagnoses=%u fixes armed=%u\n",
                (unsigned)faultchat_diagnoses(),
                (unsigned)faultchat_fixes_armed());
        show_plan("after the model answered");

        kputs("\n[fault-diagnose] ---- the prompt the kernel actually sent, "
              "verbatim ----\n");
        kputs(faultchat_last_prompt());
        kputs("[fault-diagnose] ---- end of prompt ----\n\n");
    }

    kputs("[fault-diagnose] now raising a SECOND page fault. Nothing has been "
          "armed except what the model proposed.\n");
    fault_inject(FAULT_INJECT_PF_READ);           /* fault #2 */
    kprintf("[fault-diagnose] RESULT: fault #2 was handled by the MODEL's plan. "
            "%u faults, %u diagnoses, %u fix armed. Boot continues.\n",
            (unsigned)fault_count(), (unsigned)faultchat_diagnoses(),
            (unsigned)faultchat_fixes_armed());

#elif FABLEOS_FAULT_RECOVER == 7
    announce("a fault raised WHILE a diagnosis is in flight must not recurse "
             "into the network stack");
    {
        faultchat_bind(&scripted_model);
        script_fault = 1;
        /* Enough uses to survive both the injected fault and the one the
         * transport raises from inside itself. */
        fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1,
                           .uses = FAULT_PLAN_USES_MAX };
        arm(&p, "skip, 16 uses");
    }
    fault_inject(FABLEOS_FAULT_TEST);              /* fault #1 */
    {
        int rc = faultchat_pump();
        kprintf("\n[fault-diagnose] pump returned %d (%s)\n", rc,
                faultchat_strerror(rc));
        kprintf("[fault-diagnose] transport entered %d time(s); faults=%u; "
                "fixes armed=%u; diagnosis disabled=%d (%s)\n",
                script_entries, (unsigned)fault_count(),
                (unsigned)faultchat_fixes_armed(),
                faultchat_disabled(), faultchat_disabled_reason());
        show_plan("after the aborted diagnosis");
        kputs("[fault-diagnose] the plan above is still the 16-use bootstrap "
              "one: the model's proposed fix (vector 14, 2 uses) was NOT "
              "applied.\n");
    }

    kputs("\n[fault-diagnose] raising two more faults and pumping after each. "
          "The transport must never be entered again.\n");
    {
        int before = script_entries;
        fault_inject(FAULT_INJECT_PF_READ);
        kprintf("[fault-diagnose] pump -> %d\n", faultchat_pump());
        fault_inject(FAULT_INJECT_UD);
        kprintf("[fault-diagnose] pump -> %d\n", faultchat_pump());
        kprintf("[fault-diagnose] RESULT: transport entries %d -> %d (%s). "
                "%u faults survived, 0 fixes armed. Boot continues.\n",
                before, script_entries,
                script_entries == before ? "UNCHANGED, no recursion"
                                         : "RE-ENTERED - THE LATCH FAILED",
                (unsigned)fault_count());
    }

#else
    fault_inject(FABLEOS_FAULT_TEST);
#endif
    return 0;
}

static const driver_t fault_inject_driver = {
    .name  = "fault-inject",
    .level = DRV_LEVEL_LATE,
    .cls   = DEV_CLASS_NONE,
    .init  = fault_inject_driver_init,
};
REGISTER_DRIVER(fault_inject_driver);
#endif
