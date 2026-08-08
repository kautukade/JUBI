/* test_fault.c — the fault decoder, the backtrace walkers, the report
 * formatter and the fault tools, run natively.
 *
 * WHAT THIS CAN AND CANNOT COVER
 *   The IDT itself (gate descriptors, the TSS, LIDT, the asm stubs) can only be
 *   verified by booting, and it is — see tests/qemu and the injected faults in
 *   arch/x86_64/fault_inject.c. Everything ELSE about Phase 3 is pure logic over
 *   a fault_frame_t, and pure logic has no business needing a virtual machine to
 *   test. That split is why arch/x86_64/fault.c takes its address bounds as a
 *   fault_window_t parameter instead of reading linker symbols: the same object
 *   file that runs inside the exception handler runs here, aimed at a normal
 *   array.
 *
 *   The stakes justify the effort. On a machine with no shell, the fault report
 *   is the operator's only record of why it died. A decoder that says "write"
 *   when it was a read sends the diagnosis in the wrong direction with no way to
 *   notice, so every bit of every error code is asserted individually.
 *
 * THE SYNTHETIC STACK
 *   build_stack() lays out a real chain of {saved RBP, return address} pairs in
 *   a plain array, exactly as a compiled function prologue would, and points the
 *   window at it. So fault_backtrace() performs genuine dereferences against a
 *   genuine frame chain — the walk is exercised, not simulated. Then the same
 *   array is corrupted in each of the ways a real crash corrupts it (cycles,
 *   descending pointers, misalignment, off-window pointers, garbage return
 *   addresses) to prove the walker terminates rather than hanging or reading
 *   somewhere it should not.
 */

#include "harness.h"
#include "fault.h"
#include "tool.h"
#include "json.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* the tool table, host-side                                              */
/* ====================================================================== */

/* REGISTER_TOOL cannot expand on Mach-O, so tools/fault_tools.c exports a named
 * pointer per tool under FABLEOS_HOSTTEST and this file stands in for the
 * linker, feeding the REAL core/tool.c registry. Same arrangement, and the same
 * assembler trick for __stop_tool_table, as tests/host/test_mem_tools.c. */
extern const tool_t *const fableos_hosttool_fault_status_tool;
extern const tool_t *const fableos_hosttool_fault_report_tool;
extern const tool_t *const fableos_hosttool_fault_history_tool;
extern const tool_t *const fableos_hosttool_fault_decode_tool;
extern const tool_t *const fableos_hosttool_fault_recover_tool;

#if defined(__APPLE__)
#  define SYMPFX "_"
#  define TBL_SECTION __attribute__((section("__DATA,__data")))
#else
#  define SYMPFX ""
#  define TBL_SECTION
#endif

const tool_t *__start_tool_table[5] TBL_SECTION = { 0, 0, 0, 0, 0 };
extern const tool_t *const __stop_tool_table[];

__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 40\n");
_Static_assert(sizeof(__start_tool_table) == 40,
               "tool count changed: update the __stop_tool_table byte offset");

static void tools_bind(void) {
    __start_tool_table[0] = fableos_hosttool_fault_status_tool;
    __start_tool_table[1] = fableos_hosttool_fault_report_tool;
    __start_tool_table[2] = fableos_hosttool_fault_history_tool;
    __start_tool_table[3] = fableos_hosttool_fault_decode_tool;
    __start_tool_table[4] = fableos_hosttool_fault_recover_tool;
}

/* Invoke a tool by name with a raw JSON argument span. */
static int call_tool(const char *name, const char *json,
                     char *buf, size_t cap, tool_result_t *r) {
    const tool_t *t = tool_find(name);
    if (!t) return -1;
    tool_result_init(r, buf, cap);
    tool_call_t c = { .id = "toolu_test", .name = name,
                      .input = json, .input_len = json ? strlen(json) : 0 };
    return t->invoke(&c, r);
}

/* ====================================================================== */
/* vector naming                                                          */
/* ====================================================================== */

static void test_vector_names(void) {
    CHECK_STR(fault_vector_name(0),  "#DE");
    CHECK_STR(fault_vector_name(1),  "#DB");
    CHECK_STR(fault_vector_name(2),  "NMI");
    CHECK_STR(fault_vector_name(3),  "#BP");
    CHECK_STR(fault_vector_name(6),  "#UD");
    CHECK_STR(fault_vector_name(8),  "#DF");
    CHECK_STR(fault_vector_name(13), "#GP");
    CHECK_STR(fault_vector_name(14), "#PF");
    CHECK_STR(fault_vector_name(18), "#MC");

    CHECK_STR(fault_vector_desc(0),  "Divide Error");
    CHECK_STR(fault_vector_desc(14), "Page Fault");
    CHECK_STR(fault_vector_desc(6),  "Invalid Opcode");

    /* Above the architectural range but inside the table, and past the end. */
    CHECK_STR(fault_vector_name(32),  "INT");
    CHECK_STR(fault_vector_name(255), "INT");
    CHECK_STR(fault_vector_name(256), "??");

    /* Never NULL, for any input at all — the report dereferences these from
     * inside an exception handler. */
    for (uint64_t v = 0; v < 300; v++) {
        CHECK(fault_vector_name(v) != NULL);
        CHECK(fault_vector_desc(v) != NULL);
    }
}

/* The error-code asymmetry is the single detail that, got wrong, makes every
 * register in every report read out of the wrong stack slot. Assert the exact
 * architectural set. */
static void test_error_code_vectors(void) {
    const int expect[] = { 8, 10, 11, 12, 13, 14, 17, 21, 29, 30 };
    for (uint64_t v = 0; v < 256; v++) {
        int want = 0;
        for (unsigned i = 0; i < sizeof expect / sizeof expect[0]; i++)
            if ((uint64_t)expect[i] == v) want = 1;
        CHECK_EQ(fault_has_error_code(v), want);
    }
}

static void test_fatality(void) {
    /* Continuable traps. */
    CHECK_EQ(fault_is_fatal(1), 0);          /* #DB */
    CHECK_EQ(fault_is_fatal(3), 0);          /* #BP */
    /* Everything else in the architectural range is fatal in this phase. */
    CHECK_EQ(fault_is_fatal(0),  1);
    CHECK_EQ(fault_is_fatal(6),  1);
    CHECK_EQ(fault_is_fatal(8),  1);
    CHECK_EQ(fault_is_fatal(13), 1);
    CHECK_EQ(fault_is_fatal(14), 1);
    /* An unexpected external interrupt must not kill a working machine. */
    CHECK_EQ(fault_is_fatal(32),  0);
    CHECK_EQ(fault_is_fatal(255), 0);
}

/* ====================================================================== */
/* page-fault error-code decoding                                         */
/* ====================================================================== */

static void test_pf_decode_bits(void) {
    char b[512];

    /* 0x0: read of a page that is not there, from the kernel. */
    fault_decode_error(14, 0x0, b, sizeof b);
    CHECK_CONTAINS(b, "page not present");
    CHECK_CONTAINS(b, "read");
    CHECK_CONTAINS(b, "kernel-mode");

    /* 0x2: write to a page that is not there. The exact code the null-ish
     * write injector produces, and the one a real bug produces most often. */
    fault_decode_error(14, 0x2, b, sizeof b);
    CHECK_CONTAINS(b, "page not present");
    CHECK_CONTAINS(b, "write");
    CHECK_CONTAINS(b, "kernel-mode");

    /* 0x1: the page IS present — a protection violation, a completely
     * different bug from a missing mapping, and the distinction the operator
     * most needs made for them. */
    fault_decode_error(14, 0x1, b, sizeof b);
    CHECK_CONTAINS(b, "protection violation");
    CHECK_CONTAINS(b, "page IS present");
    CHECK(strstr(b, "page not present") == NULL);

    /* 0x4 / 0x5: user-mode. */
    fault_decode_error(14, 0x4, b, sizeof b);
    CHECK_CONTAINS(b, "user-mode");
    CHECK(strstr(b, "kernel-mode") == NULL);

    /* 0x8: a reserved bit set in a paging structure. Shouted, because it means
     * the page tables themselves are corrupt. */
    fault_decode_error(14, 0x9, b, sizeof b);
    CHECK_CONTAINS(b, "RESERVED BIT SET");

    /* 0x10: instruction fetch. */
    fault_decode_error(14, 0x10, b, sizeof b);
    CHECK_CONTAINS(b, "instruction fetch");

    /* Without the I/D bit the decoder must NOT flatly claim a data access:
     * the bit is only reported when EFER.NXE=1, and this kernel never enables
     * NX. Getting this wrong sends a wild-jump diagnosis in exactly the wrong
     * direction. */
    fault_decode_error(14, 0x0, b, sizeof b);
    CHECK_CONTAINS(b, "EFER.NXE");
    CHECK_CONTAINS(b, "CR2");

    /* 0x20 / 0x40: protection key and shadow stack. */
    fault_decode_error(14, 0x21, b, sizeof b);
    CHECK_CONTAINS(b, "protection-key");
    fault_decode_error(14, 0x41, b, sizeof b);
    CHECK_CONTAINS(b, "shadow-stack");

    /* Every combination of the low five bits must produce a present/not-present
     * verdict, a read/write verdict, and a user/kernel verdict — never silence. */
    for (uint64_t e = 0; e < 32; e++) {
        fault_decode_error(14, e, b, sizeof b);
        CHECK(strstr(b, (e & 1) ? "protection violation" : "page not present") != NULL);
        CHECK(strstr(b, (e & 2) ? "write" : "read") != NULL);
        CHECK(strstr(b, (e & 4) ? "user-mode" : "kernel-mode") != NULL);
    }
}

static void test_selector_decode(void) {
    char b[512];

    /* #GP with no selector: the common case (non-canonical address, privileged
     * instruction). Must not be reported as "index 0". */
    fault_decode_error(13, 0, b, sizeof b);
    CHECK_CONTAINS(b, "no segment selector");
    CHECK(strstr(b, "index 0") == NULL);

    /* 0x30 = selector 0x30 = index 6, GDT. This is exactly what the #GP
     * injector produces and what the QEMU run reports. */
    fault_decode_error(13, 0x30, b, sizeof b);
    CHECK_CONTAINS(b, "index 6");
    CHECK_CONTAINS(b, "GDT");

    /* TI set -> LDT. */
    fault_decode_error(13, 0x34, b, sizeof b);
    CHECK_CONTAINS(b, "LDT");

    /* IDT bit wins over TI. */
    fault_decode_error(13, 0x32, b, sizeof b);
    CHECK_CONTAINS(b, "IDT");

    /* External bit. */
    fault_decode_error(13, 0x31, b, sizeof b);
    CHECK_CONTAINS(b, "external");

    /* The same decoding applies to the other selector-error vectors. */
    fault_decode_error(11, 0x30, b, sizeof b);   /* #NP */
    CHECK_CONTAINS(b, "index 6");
    fault_decode_error(12, 0x30, b, sizeof b);   /* #SS */
    CHECK_CONTAINS(b, "index 6");
}

static void test_nonselector_decode(void) {
    char b[512];

    /* #DF's error code is architecturally always zero. Saying "index 0" would
     * be actively misleading. */
    fault_decode_error(8, 0, b, sizeof b);
    CHECK_CONTAINS(b, "always zero");
    CHECK_CONTAINS(b, "RSP");

    /* A vector with no error code at all: the value shown came from the stub,
     * and the report must say so rather than let a zero look like data. */
    fault_decode_error(6, 0, b, sizeof b);
    CHECK_CONTAINS(b, "no error code");
    fault_decode_error(0, 0, b, sizeof b);
    CHECK_CONTAINS(b, "no error code");
}

/* Truncation must never run off the end, and must always NUL-terminate — this
 * runs inside an exception handler where a buffer overrun is unrecoverable. */
static void test_decode_truncation(void) {
    for (size_t cap = 1; cap <= 64; cap++) {
        char *b = malloc(cap + 8);
        memset(b, 0x7E, cap + 8);
        size_t want = fault_decode_error(14, 0x7, b, cap);
        CHECK(want > 0);
        CHECK_EQ(b[cap - 1], 0);                    /* terminated inside cap */
        for (size_t i = cap; i < cap + 8; i++)
            CHECK_EQ((unsigned char)b[i], 0x7E);    /* nothing past the end  */
        free(b);
    }
    /* cap 0 must simply not write. */
    char guard[4] = { 0x11, 0x11, 0x11, 0x11 };
    fault_decode_error(14, 0x7, guard, 0);
    CHECK_EQ(guard[0], 0x11);
}

static void test_rflags_decode(void) {
    char b[256];

    fault_decode_rflags(0, b, sizeof b);
    CHECK_CONTAINS(b, "(none)");
    CHECK_CONTAINS(b, "IOPL=0");

    /* 0x246 = PF | ZF | IF | bit1(reserved, always set). */
    fault_decode_rflags(0x246, b, sizeof b);
    CHECK_CONTAINS(b, "PF");
    CHECK_CONTAINS(b, "ZF");
    CHECK_CONTAINS(b, "IF");
    CHECK(strstr(b, "CF") == NULL);

    /* Interrupts disabled must NOT show IF — the whole point of reading this
     * field in a fault report is to know whether they were on. */
    fault_decode_rflags(0x2, b, sizeof b);
    CHECK(strstr(b, "IF") == NULL);

    fault_decode_rflags(0x3000, b, sizeof b);
    CHECK_CONTAINS(b, "IOPL=3");

    fault_decode_rflags(1ULL << 10, b, sizeof b);
    CHECK_CONTAINS(b, "DF");
}

/* ====================================================================== */
/* the backtrace walkers                                                  */
/* ====================================================================== */

/* A synthetic stack of real {saved rbp, return address} frames. Frame i lives
 * at slot 2*i, its saved-RBP points at frame i+1 (a HIGHER address, as on a
 * real downward-growing stack), and its return address is CODE_BASE + i. */
#define STACK_SLOTS 64
#define CODE_BASE   0x00100000ULL
#define CODE_TOP    0x00200000ULL

/* Bytes that stand in for the faulting instruction, placed inside a window
 * aimed at this array so the capture path performs a real read. */
static unsigned char fake_code[64];

static uint64_t       stk[STACK_SLOTS];
static fault_window_t win;

static uint64_t slot_addr(int i) { return (uint64_t)(uintptr_t)&stk[i]; }

static void build_stack(int frames) {
    memset(stk, 0, sizeof stk);
    for (int i = 0; i < frames; i++) {
        stk[2 * i]     = slot_addr(2 * (i + 1));      /* saved RBP -> caller */
        stk[2 * i + 1] = CODE_BASE + 0x100u * (unsigned)(i + 1);  /* ret addr */
    }
    /* Terminate: the outermost frame's saved RBP is zero, which is neither
     * 8-aligned-and-in-window nor greater than the current frame. */
    stk[2 * frames] = 0;

    /* code_lo..code_hi is a purely synthetic range: the walkers only COMPARE
     * against it, never dereference it, which is precisely the property that
     * makes a bogus return address safe to reject. image_lo..image_hi is the
     * one range capture really reads from, so it points at a real array. */
    win.code_lo  = CODE_BASE;
    win.code_hi  = CODE_TOP;
    win.image_lo = (uint64_t)(uintptr_t)fake_code;
    win.image_hi = win.image_lo + sizeof fake_code;
    win.stack_lo = slot_addr(0);
    win.stack_hi = slot_addr(0) + sizeof stk;
}

static void test_backtrace_walks_a_real_chain(void) {
    build_stack(5);
    uint64_t out[FAULT_BT_MAX];
    int n = fault_backtrace(slot_addr(0), &win, out, FAULT_BT_MAX);
    CHECK_EQ(n, 5);
    for (int i = 0; i < n; i++)
        CHECK_EQ(out[i], CODE_BASE + 0x100u * (unsigned)(i + 1));
}

static void test_backtrace_respects_max(void) {
    build_stack(5);
    uint64_t out[FAULT_BT_MAX];
    for (int max = 1; max <= 5; max++) {
        memset(out, 0, sizeof out);
        CHECK_EQ(fault_backtrace(slot_addr(0), &win, out, max), max);
        CHECK_EQ(out[max - 1], CODE_BASE + 0x100u * (unsigned)max);
    }
    CHECK_EQ(fault_backtrace(slot_addr(0), &win, out, 0), 0);
    CHECK_EQ(fault_backtrace(slot_addr(0), &win, out, -1), 0);
}

/* Every one of these is a real corruption pattern, and every one of them must
 * terminate. A backtrace that hangs turns a diagnosable crash into a hung VM. */
static void test_backtrace_survives_corruption(void) {
    uint64_t out[FAULT_BT_MAX];

    /* A cycle: frame 1 points back at frame 0. The strictly-increasing rule is
     * what stops this spinning forever. */
    build_stack(5);
    stk[2] = slot_addr(0);
    int n = fault_backtrace(slot_addr(0), &win, out, FAULT_BT_MAX);
    CHECK(n >= 1);
    CHECK(n <= 2);

    /* A self-referential frame. */
    build_stack(5);
    stk[0] = slot_addr(0);
    n = fault_backtrace(slot_addr(0), &win, out, FAULT_BT_MAX);
    CHECK_EQ(n, 1);

    /* A misaligned frame pointer: never dereferenced. */
    build_stack(5);
    CHECK_EQ(fault_backtrace(slot_addr(0) + 1, &win, out, FAULT_BT_MAX), 0);
    CHECK_EQ(fault_backtrace(slot_addr(0) + 7, &win, out, FAULT_BT_MAX), 0);

    /* Frame pointers outside the stack window: never dereferenced. */
    build_stack(5);
    CHECK_EQ(fault_backtrace(win.stack_lo - 4096, &win, out, FAULT_BT_MAX), 0);
    CHECK_EQ(fault_backtrace(win.stack_hi + 4096, &win, out, FAULT_BT_MAX), 0);
    CHECK_EQ(fault_backtrace(0, &win, out, FAULT_BT_MAX), 0);
    CHECK_EQ(fault_backtrace(UINT64_MAX & ~7ULL, &win, out, FAULT_BT_MAX), 0);

    /* The very last qword of the window: reading the return address at +8 would
     * step outside, so the walk must refuse rather than read one slot over. */
    build_stack(5);
    CHECK_EQ(fault_backtrace(win.stack_hi - 8, &win, out, FAULT_BT_MAX), 0);

    /* A return address that is not code: the chain stops there rather than
     * reporting a plausible-looking lie. */
    build_stack(5);
    stk[3] = 0xDEADBEEF00000000ULL;
    CHECK_EQ(fault_backtrace(slot_addr(0), &win, out, FAULT_BT_MAX), 1);

    /* An empty window refuses everything. */
    fault_window_t empty = { 0, 0, 0, 0, 0, 0 };
    CHECK_EQ(fault_backtrace(slot_addr(0), &empty, out, FAULT_BT_MAX), 0);

    /* NULL arguments. */
    CHECK_EQ(fault_backtrace(slot_addr(0), NULL, out, FAULT_BT_MAX), 0);
    CHECK_EQ(fault_backtrace(slot_addr(0), &win, NULL, FAULT_BT_MAX), 0);
}

static void test_backtrace_scan(void) {
    uint64_t out[FAULT_BT_MAX];

    /* The scan is deliberately indiscriminate: it reports every code-looking
     * qword, which for the synthetic stack is every return-address slot. */
    build_stack(5);
    int n = fault_backtrace_scan(slot_addr(0), &win, out, FAULT_BT_MAX);
    CHECK_EQ(n, 5);
    for (int i = 0; i < n; i++)
        CHECK_EQ(out[i], CODE_BASE + 0x100u * (unsigned)(i + 1));

    /* It must find candidates even when every frame pointer is destroyed —
     * that is the entire reason it exists. */
    build_stack(5);
    for (int i = 0; i < 5; i++) stk[2 * i] = 0xAAAAAAAAAAAAAAAAULL;
    CHECK_EQ(fault_backtrace(slot_addr(0), &win, out, FAULT_BT_MAX), 1);
    CHECK_EQ(fault_backtrace_scan(slot_addr(0), &win, out, FAULT_BT_MAX), 5);

    /* Misaligned start is rounded up, not refused. */
    build_stack(5);
    CHECK(fault_backtrace_scan(slot_addr(0) + 1, &win, out, FAULT_BT_MAX) >= 4);

    /* Out of window / degenerate. */
    CHECK_EQ(fault_backtrace_scan(win.stack_hi, &win, out, FAULT_BT_MAX), 0);
    CHECK_EQ(fault_backtrace_scan(UINT64_MAX, &win, out, FAULT_BT_MAX), 0);
    CHECK_EQ(fault_backtrace_scan(slot_addr(0), &win, out, 0), 0);
}

/* ====================================================================== */
/* capture                                                                */
/* ====================================================================== */

static void frame_defaults(fault_frame_t *f) {
    memset(f, 0, sizeof *f);
    f->cs     = 0x08;
    f->rflags = 0x2;
}

static void test_capture_copies_the_frame(void) {
    build_stack(4);
    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 14; f.error_code = 0x2; f.cr2 = 0xFFFF000000000000ULL;
    f.rip = CODE_BASE + 0x40; f.rsp = slot_addr(0); f.rbp = slot_addr(0);
    f.rax = 0x1111; f.r15 = 0x2222;

    fault_record_t rec;
    fault_capture(&f, &win, 12345, &rec);

    CHECK_EQ(rec.valid, 1);
    CHECK_EQ(rec.uptime_ms, 12345);
    CHECK_EQ(rec.fatal, 1);
    CHECK_EQ(rec.regs.vector, 14);
    CHECK_EQ(rec.regs.error_code, 0x2);
    CHECK_EQ(rec.regs.rax, 0x1111);
    CHECK_EQ(rec.regs.r15, 0x2222);
    CHECK_EQ(rec.regs.cr2, 0xFFFF000000000000ULL);
    CHECK_EQ(rec.bt_source, FAULT_BT_FRAME);
    CHECK_EQ(rec.bt_count, 4);

    /* A continuable trap is recorded as such. */
    f.vector = 3;
    fault_capture(&f, &win, 1, &rec);
    CHECK_EQ(rec.fatal, 0);
}

static void test_capture_reads_instruction_bytes(void) {
    for (unsigned i = 0; i < sizeof fake_code; i++) fake_code[i] = (unsigned char)(i + 1);

    build_stack(3);
    /* Aim the image window at fake_code; leave code_lo/hi where they are so the
     * backtrace logic is unaffected. */
    win.image_lo = (uint64_t)(uintptr_t)fake_code;
    win.image_hi = win.image_lo + sizeof fake_code;

    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 6;
    f.rip = win.image_lo;
    f.rbp = slot_addr(0);
    f.rsp = slot_addr(0);

    fault_record_t rec;
    fault_capture(&f, &win, 0, &rec);
    CHECK_EQ(rec.code_valid, 1);
    CHECK_EQ(rec.code_len, FAULT_CODE_BYTES);
    for (int i = 0; i < FAULT_CODE_BYTES; i++)
        CHECK_EQ(rec.code[i], (unsigned char)(i + 1));

    /* Near the end of the window the read is CLIPPED, not refused and not run
     * past the end. */
    f.rip = win.image_hi - 5;
    fault_capture(&f, &win, 0, &rec);
    CHECK_EQ(rec.code_valid, 1);
    CHECK_EQ(rec.code_len, 5);

    /* Outside the window entirely: refused, and flagged. */
    f.rip = win.image_hi + 0x1000;
    fault_capture(&f, &win, 0, &rec);
    CHECK_EQ(rec.code_valid, 0);
    CHECK_EQ(rec.code_len, 0);

    f.rip = 0;
    fault_capture(&f, &win, 0, &rec);
    CHECK_EQ(rec.code_valid, 0);
}

static void test_capture_falls_back_to_a_scan(void) {
    build_stack(5);
    for (int i = 0; i < 5; i++) stk[2 * i] = 0xAAAAAAAAAAAAAAAAULL;  /* trash RBP */

    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 13;
    f.rip = CODE_BASE + 0x10;
    f.rbp = slot_addr(0);
    f.rsp = slot_addr(0);

    fault_record_t rec;
    fault_capture(&f, &win, 0, &rec);
    CHECK_EQ(rec.bt_source, FAULT_BT_SCAN);
    CHECK_EQ(rec.bt_count, 5);

    /* Nothing recoverable at all -> honestly reports nothing. */
    memset(stk, 0, sizeof stk);
    f.rbp = 0;
    f.rsp = win.stack_hi;
    fault_capture(&f, &win, 0, &rec);
    CHECK_EQ(rec.bt_count, 0);
    CHECK_EQ(rec.bt_source, FAULT_BT_NONE);
}

static void test_capture_is_defensive(void) {
    fault_record_t rec;
    memset(&rec, 0xFF, sizeof rec);
    fault_capture(NULL, &win, 0, &rec);
    CHECK_EQ(rec.valid, 0);            /* zeroed, not left as garbage */

    memset(&rec, 0xFF, sizeof rec);
    fault_frame_t f;
    frame_defaults(&f);
    fault_capture(&f, NULL, 0, &rec);
    CHECK_EQ(rec.valid, 0);

    fault_capture(&f, &win, 0, NULL);  /* must not crash */
    CHECK(1);
}

/* ====================================================================== */
/* the report                                                             */
/* ====================================================================== */

static void make_pf_record(fault_record_t *rec) {
    build_stack(3);
    win.image_lo = (uint64_t)(uintptr_t)fake_code;
    win.image_hi = win.image_lo + sizeof fake_code;
    fake_code[0] = 0x48; fake_code[1] = 0xC7; fake_code[2] = 0x00;

    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 14;
    f.error_code = 0x2;
    f.cr2 = 0x0000001000000000ULL;
    f.rip = win.image_lo;
    f.rsp = slot_addr(0);
    f.rbp = slot_addr(0);
    f.rflags = 0x10046;
    f.rax = 0xDEADBEEF;
    f.r12 = 0xCAFEBABE;

    fault_capture(&f, &win, 4, rec);
    rec->seq = 1;
}

static void test_report_contains_everything_that_matters(void) {
    fault_record_t rec;
    make_pf_record(&rec);

    char out[FAULT_REPORT_MAX];
    size_t n = fault_format_report(&rec, out, sizeof out);
    CHECK(n > 0);
    CHECK(n < sizeof out);              /* the budget is actually sufficient */

    /* Identity of the fault. */
    CHECK_CONTAINS(out, "#PF");
    CHECK_CONTAINS(out, "vector 14");
    CHECK_CONTAINS(out, "Page Fault");

    /* The error code, raw AND in words. Raw alone is not a diagnosis. */
    CHECK_CONTAINS(out, "0x0000000000000002");
    CHECK_CONTAINS(out, "page not present");
    CHECK_CONTAINS(out, "write");
    CHECK_CONTAINS(out, "pushed by the CPU");

    /* The faulting address, and every part of the machine state the task
     * requires: RIP, CS, RFLAGS, RSP, SS, CR2, and all 15 GPRs. */
    CHECK_CONTAINS(out, "CR2");
    CHECK_CONTAINS(out, "0x0000001000000000");
    CHECK_CONTAINS(out, "RIP");
    CHECK_CONTAINS(out, "CS: 0x0008");
    CHECK_CONTAINS(out, "SS: 0x0000");
    CHECK_CONTAINS(out, "RFLAGS");
    CHECK_CONTAINS(out, "RSP");
    const char *gpr[] = { "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP",
                          "R8 ", "R9 ", "R10", "R11", "R12", "R13", "R14",
                          "R15" };
    for (unsigned i = 0; i < sizeof gpr / sizeof gpr[0]; i++)
        CHECK_CONTAINS(out, gpr[i]);
    CHECK_CONTAINS(out, "00000000deadbeef");
    CHECK_CONTAINS(out, "00000000cafebabe");

    /* The bytes at the faulting instruction. */
    CHECK_CONTAINS(out, "code @RIP");
    CHECK_CONTAINS(out, "48 c7 00");

    /* And the backtrace. */
    CHECK_CONTAINS(out, "backtrace");
    CHECK_CONTAINS(out, "RBP chain");
    CHECK_CONTAINS(out, "#0");
    CHECK_CONTAINS(out, "0000000000100100");
}

static void test_report_labels_a_scanned_backtrace(void) {
    build_stack(5);
    for (int i = 0; i < 5; i++) stk[2 * i] = 0xAAAAAAAAAAAAAAAAULL;

    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 13;
    f.error_code = 0x30;
    f.rip = CODE_BASE + 0x10;
    f.rbp = slot_addr(0);
    f.rsp = slot_addr(0);

    fault_record_t rec;
    fault_capture(&f, &win, 0, &rec);
    rec.seq = 2;

    char out[FAULT_REPORT_MAX];
    fault_format_report(&rec, out, sizeof out);

    /* Candidates must never be presented as a real call chain. */
    CHECK_CONTAINS(out, "CANDIDATES");
    CHECK(strstr(out, "RBP chain") == NULL);
    CHECK_CONTAINS(out, "index 6");         /* selector decoding in-line */
    CHECK_CONTAINS(out, "#GP");
}

static void test_report_says_when_rip_is_unreadable(void) {
    build_stack(3);
    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 14;
    f.error_code = 0;
    f.rip = 0x0000001000000000ULL;
    f.cr2 = 0x0000001000000000ULL;      /* CR2 == RIP: an instruction fetch */
    f.rbp = slot_addr(0);
    f.rsp = slot_addr(0);

    fault_record_t rec;
    fault_capture(&f, &win, 0, &rec);
    rec.seq = 3;

    char out[FAULT_REPORT_MAX];
    fault_format_report(&rec, out, sizeof out);
    CHECK_CONTAINS(out, "unavailable");
    /* With NX disabled the error code's I/D bit is always zero, so RIP == CR2
     * is the only reliable signal that this was a fetch. The report must make
     * that call rather than leaving it to the reader. */
    CHECK_CONTAINS(out, "CR2 == RIP");
    CHECK_CONTAINS(out, "FETCHING");
}

static void test_report_handles_no_fault_and_truncation(void) {
    char out[FAULT_REPORT_MAX];

    fault_record_t empty;
    memset(&empty, 0, sizeof empty);
    fault_format_report(&empty, out, sizeof out);
    CHECK_CONTAINS(out, "no fault");

    fault_format_report(NULL, out, sizeof out);
    CHECK_CONTAINS(out, "no fault");

    /* Truncation at every small capacity: always terminated, never overrun. */
    fault_record_t rec;
    make_pf_record(&rec);
    for (size_t cap = 1; cap <= 400; cap += 7) {
        char *b = malloc(cap + 8);
        memset(b, 0x7E, cap + 8);
        size_t want = fault_format_report(&rec, b, cap);
        CHECK(want > cap - 1);                  /* it really did truncate */
        CHECK_EQ(b[cap - 1], 0);
        for (size_t i = cap; i < cap + 8; i++)
            CHECK_EQ((unsigned char)b[i], 0x7E);
        free(b);
    }
    char guard[4] = { 0x22, 0x22, 0x22, 0x22 };
    fault_format_report(&rec, guard, 0);
    CHECK_EQ(guard[0], 0x22);
}

/* Every vector must produce a complete, sane report — including the reserved
 * ones, which is exactly when a report is least likely to have been tried. */
static void test_report_for_every_vector(void) {
    for (uint64_t v = 0; v < 256; v++) {
        build_stack(2);
        fault_frame_t f;
        frame_defaults(&f);
        f.vector = v;
        f.error_code = 0x1234;
        f.rip = CODE_BASE + 8;
        f.rbp = slot_addr(0);
        f.rsp = slot_addr(0);

        fault_record_t rec;
        fault_capture(&f, &win, v, &rec);
        rec.seq = (uint32_t)v + 1;

        char out[FAULT_REPORT_MAX];
        size_t n = fault_format_report(&rec, out, sizeof out);
        CHECK(n > 0);
        CHECK(n < sizeof out);
        CHECK_CONTAINS(out, "KERNEL FAULT");
        CHECK_CONTAINS(out, fault_vector_name(v));
    }
}

/* ====================================================================== */
/* the live record                                                        */
/* ====================================================================== */

static void test_live_record(void) {
    fault_reset();
    CHECK_EQ(fault_occurred(), 0);
    CHECK_EQ(fault_count(), 0);
    CHECK(fault_last() == NULL);
    CHECK_CONTAINS(fault_last_report(), "no fault");

    build_stack(3);
    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 14; f.error_code = 0x2; f.cr2 = 0x1000;
    f.rip = CODE_BASE + 4; f.rbp = slot_addr(0); f.rsp = slot_addr(0);

    fault_note(&f, &win, 99);
    CHECK_EQ(fault_occurred(), 1);
    CHECK_EQ(fault_count(), 1);
    CHECK(fault_last() != NULL);
    CHECK_EQ(fault_last()->seq, 1);
    CHECK_EQ(fault_last()->uptime_ms, 99);
    CHECK_CONTAINS(fault_last_report(), "#PF");

    /* seq advances; the slot holds the most recent. */
    f.vector = 6;
    fault_note(&f, &win, 100);
    CHECK_EQ(fault_count(), 2);
    CHECK_EQ(fault_last()->seq, 2);
    CHECK_EQ(fault_last()->regs.vector, 6);
    CHECK_CONTAINS(fault_last_report(), "#UD");

    fault_reset();
    CHECK_EQ(fault_occurred(), 0);
}

/* ====================================================================== */
/* the tools                                                              */
/* ====================================================================== */

static void test_tool_registration(void) {
    CHECK_EQ(tool_count(), 5);
    CHECK(tool_find("fault_status")  != NULL);
    CHECK(tool_find("fault_report")  != NULL);
    CHECK(tool_find("fault_history") != NULL);
    CHECK(tool_find("fault_decode")  != NULL);
    CHECK(tool_find("fault_recover") != NULL);

    /* EXACTLY ONE tool in this family may mutate anything, and it is the one
     * that arms a recovery. Everything else is a pure read of a snapshot. If a
     * later change makes a diagnostic tool start writing, or forgets to flag a
     * new mutating one, this is the tripwire. */
    for (int i = 0; i < tool_count(); i++) {
        const tool_t *t = tool_at(i);
        CHECK(t->description  != NULL);
        CHECK(t->input_schema != NULL);
        CHECK(t->invoke       != NULL);
        int should_mutate = strcmp(t->name, "fault_recover") == 0;
        CHECK_EQ((t->flags & TOOL_MUTATES) != 0, should_mutate);
    }

    /* No tool may expose the injector. Arming a recovery is safe because it
     * cannot cause a fault; causing one on request never becomes safe. */
    CHECK(tool_find("fault_inject") == NULL);
    CHECK(tool_find("fault_crash")  == NULL);
}

static void test_tool_status_with_no_fault(void) {
    fault_reset();
    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    CHECK_EQ(call_tool("fault_status", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "faults since boot: 0");
    /* On the host the IDT is by definition absent, and the tool must say so
     * rather than implying exceptions are being caught. */
    CHECK_CONTAINS(buf, "NO IDT");
}

static void test_tool_status_after_a_fault(void) {
    fault_reset();
    build_stack(3);
    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 14; f.error_code = 0x7; f.cr2 = 0xABCD000;
    f.rip = CODE_BASE + 0x20; f.rbp = slot_addr(0); f.rsp = slot_addr(0);
    fault_note(&f, &win, 777);

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    CHECK_EQ(call_tool("fault_status", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "faults since boot: 1");
    CHECK_CONTAINS(buf, "#PF");
    CHECK_CONTAINS(buf, "protection violation");
    CHECK_CONTAINS(buf, "write");
    CHECK_CONTAINS(buf, "user-mode");
    CHECK_CONTAINS(buf, "0x000000000abcd000");
    CHECK_CONTAINS(buf, "fatal");

    /* Junk arguments are ignored, not fatal: a no-argument tool must be the
     * most robust thing in the registry. */
    CHECK_EQ(call_tool("fault_status", "not json at all", buf, sizeof buf, &r),
             TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(call_tool("fault_status", NULL, buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
}

static void test_tool_report_is_the_same_bytes(void) {
    fault_reset();
    build_stack(3);
    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 13; f.error_code = 0x30;
    f.rip = CODE_BASE + 0x30; f.rbp = slot_addr(0); f.rsp = slot_addr(0);
    fault_note(&f, &win, 5);

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    CHECK_EQ(call_tool("fault_report", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(r.truncated, 0);

    /* The whole point: what the operator saw on the console and what the model
     * is told are one string, not two renderings that could disagree. */
    CHECK_STR(buf, fault_last_report());

    fault_reset();
    CHECK_EQ(call_tool("fault_report", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_CONTAINS(buf, "No fault has been captured");
}

/* The report has to fit the model's result budget with room to spare, or the
 * most important tool in the family silently truncates the diagnosis. */
static void test_report_fits_the_tool_budget(void) {
    fault_reset();
    build_stack(FAULT_BT_MAX);
    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 14;
    f.error_code = 0xFFFF;               /* every decodable bit set */
    f.cr2 = UINT64_MAX;
    f.rip = CODE_BASE; f.rbp = slot_addr(0); f.rsp = slot_addr(0);
    f.rax = f.rbx = f.rcx = f.rdx = UINT64_MAX;
    f.rsi = f.rdi = f.r8 = f.r9 = UINT64_MAX;
    f.r10 = f.r11 = f.r12 = f.r13 = f.r14 = f.r15 = UINT64_MAX;
    f.rflags = UINT64_MAX;
    fault_note(&f, &win, UINT64_MAX);

    CHECK(strlen(fault_last_report()) < FAULT_REPORT_MAX - 1);

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    call_tool("fault_report", "{}", buf, sizeof buf, &r);
    CHECK_EQ(r.truncated, 0);

}

static void test_tool_decode_arguments(void) {
    char buf[TOOL_RESULT_MAX];
    tool_result_t r;

    /* The happy path, both argument spellings for error_code. */
    CHECK_EQ(call_tool("fault_decode", "{\"vector\":14,\"error_code\":\"0x2\"}",
                       buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "#PF");
    CHECK_CONTAINS(buf, "page not present");
    CHECK_CONTAINS(buf, "write");

    CHECK_EQ(call_tool("fault_decode", "{\"vector\":14,\"error_code\":2}",
                       buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "write");

    /* Decimal string. */
    CHECK_EQ(call_tool("fault_decode", "{\"vector\":13,\"error_code\":\"48\"}",
                       buf, sizeof buf, &r), TOOL_OK);
    CHECK_CONTAINS(buf, "index 6");

    /* error_code is optional and defaults to 0. */
    CHECK_EQ(call_tool("fault_decode", "{\"vector\":6}", buf, sizeof buf, &r),
             TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "#UD");
    CHECK_CONTAINS(buf, "does NOT push");

    /* Continuability is reported. */
    CHECK_EQ(call_tool("fault_decode", "{\"vector\":3}", buf, sizeof buf, &r),
             TOOL_OK);
    CHECK_CONTAINS(buf, "continuable");
}

/* Model output is untrusted. Every one of these must produce a legible error
 * the model can recover from, and never a wrong answer. */
static void test_tool_decode_rejects_bad_input(void) {
    static const char *bad[] = {
        NULL,
        "",
        "not json",
        "[1,2,3]",
        "\"a string\"",
        "42",
        "{}",                                       /* vector missing      */
        "{\"vector\":\"14\"}",                      /* vector as a string  */
        "{\"vector\":-1}",
        "{\"vector\":256}",
        "{\"vector\":99999999999999999999}",
        "{\"vector\":14,\"error_code\":-1}",
        "{\"vector\":14,\"error_code\":true}",
        "{\"vector\":14,\"error_code\":\"0xZZ\"}",
        "{\"vector\":14,\"error_code\":\"\"}",
        "{\"vector\":14,\"error_code\":\"2 bytes\"}",
        "{\"vector\":14,\"error_code\":\"0xFFFFFFFFFFFFFFFFF\"}",  /* overflow */
        "{\"vector\":14,",                          /* truncated JSON      */
    };
    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = call_tool("fault_decode", bad[i], buf, sizeof buf, &r);
        CHECK_EQ(rc, TOOL_OK);              /* a bad argument is not a crash */
        CHECK_EQ(r.is_error, 1);
        CHECK_CONTAINS(buf, "refused");
        CHECK_CONTAINS(buf, "vector");      /* restates the accepted shape  */
    }

    /* Boundary values that must be ACCEPTED. */
    CHECK_EQ(call_tool("fault_decode", "{\"vector\":0}", buf, sizeof buf, &r),
             TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(call_tool("fault_decode", "{\"vector\":255}", buf, sizeof buf, &r),
             TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(call_tool("fault_decode",
                       "{\"vector\":14,\"error_code\":\"0xFFFFFFFFFFFFFFFF\"}",
                       buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
}

/* A tiny result buffer must never be overrun, and the model must always be told
 * the answer was clipped. */
static void test_tool_output_bounds(void) {
    fault_reset();
    build_stack(4);
    fault_frame_t f;
    frame_defaults(&f);
    f.vector = 14; f.error_code = 0x2;
    f.rip = CODE_BASE; f.rbp = slot_addr(0); f.rsp = slot_addr(0);
    fault_note(&f, &win, 1);

    static const char *names[] = { "fault_status", "fault_report",
                                   "fault_history", "fault_decode",
                                   "fault_recover" };
    for (unsigned t = 0; t < sizeof names / sizeof names[0]; t++) {
        for (size_t cap = 1; cap <= 300; cap += 11) {
            char *b = malloc(cap + 8);
            memset(b, 0x5A, cap + 8);
            tool_result_t r;
            call_tool(names[t], "{\"vector\":14,\"error_code\":\"0x2\"}",
                      b, cap, &r);
            CHECK(r.len < cap);
            CHECK_EQ(b[r.len], 0);
            for (size_t i = cap; i < cap + 8; i++)
                CHECK_EQ((unsigned char)b[i], 0x5A);
            free(b);
        }
    }
}

/* What the model is actually offered has to be well-formed JSON and has to
 * contain these tools, or the whole family is invisible however well it works. */
static void test_advertised_schema(void) {
    static char schema[65536];
    size_t want = tools_build_schema(schema, sizeof schema);
    CHECK(want > 0);
    CHECK(want < sizeof schema);

    json_value_t root;
    CHECK_EQ(json_parse(schema, strlen(schema), &root), JSON_OK);
    CHECK_EQ(root.type, JSON_ARRAY);
    CHECK_EQ((int)json_count(&root), tool_count());

    CHECK_CONTAINS(schema, "fault_status");
    CHECK_CONTAINS(schema, "fault_report");
    CHECK_CONTAINS(schema, "fault_history");
    CHECK_CONTAINS(schema, "fault_decode");
    CHECK_CONTAINS(schema, "fault_recover");
    /* The injector must never appear in the model's vocabulary. */
    CHECK(strstr(schema, "inject") == NULL);

    /* Every entry's input_schema must itself be a valid JSON object. */
    for (size_t i = 0; i < json_count(&root); i++) {
        json_value_t blk, sch;
        CHECK_EQ(json_at(&root, i, &blk), JSON_OK);
        CHECK_EQ(json_get(&blk, "input_schema", &sch), JSON_OK);
        CHECK_EQ(sch.type, JSON_OBJECT);
    }
}

/* Dispatch through the real registry, not just the handler, so the trace line
 * the operator sees is exercised too. */
static void test_dispatch_and_unknown_name(void) {
    fault_reset();
    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    tool_result_init(&r, buf, sizeof buf);

    tool_call_t c = { .id = "toolu_1", .name = "fault_status",
                      .input = "{}", .input_len = 2 };
    CHECK_EQ(tool_dispatch(&c, &r), TOOL_OK);
    CHECK_CONTAINS(buf, "faults since boot: 0");

    tool_result_init(&r, buf, sizeof buf);
    c.name = "fault_inject";              /* must not exist */
    CHECK_EQ(tool_dispatch(&c, &r), TOOL_ENOENT);
}

/* Deterministic fuzz over the decoder and the report: no input may produce an
 * unterminated buffer, an overrun, or a hang. */
static void test_fuzz(void) {
    uint64_t s = 0x243F6A8885A308D3ULL;
    for (int iter = 0; iter < 20000; iter++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;

        char b[512];
        memset(b, 0x33, sizeof b);
        size_t cap = (size_t)(s % sizeof b) + 1;
        fault_decode_error(s % 300, s, b, cap);
        CHECK(memchr(b, 0, cap) != NULL);      /* terminated inside cap */

        memset(b, 0x33, sizeof b);
        cap = (size_t)((s >> 8) % sizeof b) + 1;
        fault_decode_rflags(s, b, cap);
        CHECK(memchr(b, 0, cap) != NULL);
    }

    /* And the walkers, against wholly random frame pointers. Every call must
     * return, and must never report more than it was asked for. */
    build_stack(6);
    for (int iter = 0; iter < 20000; iter++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        uint64_t out[FAULT_BT_MAX];
        int n = fault_backtrace(s, &win, out, FAULT_BT_MAX);
        CHECK(n >= 0 && n <= FAULT_BT_MAX);
        n = fault_backtrace_scan(s, &win, out, FAULT_BT_MAX);
        CHECK(n >= 0 && n <= FAULT_BT_MAX);

        /* Random pointers INTO the stack array: still bounded, still returns. */
        uint64_t p = win.stack_lo + (s % (sizeof stk));
        n = fault_backtrace(p, &win, out, FAULT_BT_MAX);
        CHECK(n >= 0 && n <= FAULT_BT_MAX);
    }
}

/* ====================================================================== */
/* the instruction-length decoder                                         */
/* ====================================================================== */

/* Every encoding below was assembled and read back with
 *     x86_64-elf-objdump -D -b binary -m i386:x86-64
 * so the expected lengths are a disassembler's opinion, not the author's. The
 * decoder was additionally cross-checked against every instruction in the
 * linked kernel (119,970 of them: 119,966 correct, 4 refused, 0 wrong) and
 * against all 376 ModRM/SIB addressing forms of `mov (X),%rax`. */
#define CHK_LEN(want, ...)                                                     \
    do {                                                                       \
        static const unsigned char b_[] = { __VA_ARGS__ };                     \
        int got_ = x86_insn_length(b_, (int)sizeof b_);                        \
        th_checks++;                                                           \
        if (got_ != (want)) {                                                  \
            th_fails++;                                                        \
            printf("    FAIL %s:%d  x86_insn_length(%s) = %d, want %d\n",      \
                   __FILE__, __LINE__, #__VA_ARGS__, got_, (want));            \
        }                                                                      \
    } while (0)

static void test_insn_length_the_injector_forms(void) {
    /* THE ONES THAT MATTER MOST: exactly what arch/x86_64/fault_inject.c
     * produces, because those are the instructions a recovery will be asked to
     * step over first. */
    CHK_LEN(3,  0xf7, 0x7d, 0xec);                    /* idivl -0x14(%rbp) #DE */
    CHK_LEN(1,  0xcc);                                /* int3               #BP */
    CHK_LEN(2,  0x0f, 0x0b);                          /* ud2                #UD */
    CHK_LEN(2,  0x8e, 0xd8);                          /* mov %eax,%ds       #GP */
    CHK_LEN(3,  0x48, 0x8b, 0x00);                    /* mov (%rax),%rax    #PF */
    CHK_LEN(7,  0x48, 0xc7, 0x00, 0x34, 0x12, 0x00, 0x00);  /* movq imm,(%rax) */
    CHK_LEN(10, 0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00);

    /* F6/F7 are the trap in this whole exercise: /0 and /1 carry an immediate
     * and /2../7 do not. An IDIV mis-read as a TEST over-skips by four bytes. */
    CHK_LEN(2, 0xf7, 0xf9);                           /* idiv %ecx           */
    CHK_LEN(6, 0xf7, 0xc1, 0x01, 0x00, 0x00, 0x00);   /* test $1,%ecx        */
    CHK_LEN(3, 0xf6, 0xc1, 0x01);                     /* test $1,%cl         */
    CHK_LEN(2, 0xf6, 0xd9);                           /* neg %cl             */
}

static void test_insn_length_common_encodings(void) {
    CHK_LEN(1, 0xc3);                                 /* ret                 */
    CHK_LEN(1, 0x90);                                 /* nop                 */
    CHK_LEN(1, 0x55);                                 /* push %rbp           */
    CHK_LEN(1, 0xc9);                                 /* leave               */
    CHK_LEN(1, 0xf4);                                 /* hlt                 */
    CHK_LEN(1, 0x99);                                 /* cltd                */
    CHK_LEN(3, 0x48, 0x89, 0xe5);                     /* mov %rsp,%rbp       */
    CHK_LEN(4, 0x48, 0x83, 0xec, 0x20);               /* sub $0x20,%rsp      */
    CHK_LEN(3, 0x8b, 0x45, 0xfc);                     /* mov -0x4(%rbp),%eax */
    CHK_LEN(7, 0x48, 0x8d, 0x05, 0, 0, 0, 0);         /* lea 0x0(%rip),%rax  */
    CHK_LEN(5, 0xe8, 0, 0, 0, 0);                     /* call rel32          */
    CHK_LEN(5, 0xe9, 0, 0, 0, 0);                     /* jmp rel32           */
    CHK_LEN(2, 0xeb, 0x05);                           /* jmp rel8            */
    CHK_LEN(2, 0x74, 0x05);                           /* je rel8             */
    CHK_LEN(6, 0x0f, 0x84, 0, 0, 0, 0);               /* je rel32            */
    CHK_LEN(4, 0x66, 0x89, 0x45, 0xfe);               /* mov %ax,-0x2(%rbp)  */
    CHK_LEN(4, 0xf3, 0x0f, 0x1e, 0xfa);               /* endbr64             */
    CHK_LEN(1, 0xec);                                 /* in (%dx),%al        */
    CHK_LEN(2, 0xe6, 0x80);                           /* out %al,$0x80       */
    CHK_LEN(3, 0x0f, 0x20, 0xc0);                     /* mov %cr0,%rax       */
    CHK_LEN(2, 0x0f, 0xa2);                           /* cpuid               */
    CHK_LEN(2, 0x0f, 0x31);                           /* rdtsc               */
    CHK_LEN(4, 0x48, 0x0f, 0xbe, 0xc0);               /* movsbq %al,%rax     */
    CHK_LEN(3, 0x0f, 0xb6, 0xc0);                     /* movzbl %al,%eax     */
    CHK_LEN(6, 0xff, 0x25, 0, 0, 0, 0);               /* jmp *0x0(%rip)      */
    CHK_LEN(2, 0xff, 0xd0);                           /* call *%rax          */
    CHK_LEN(3, 0x41, 0xff, 0xd3);                     /* call *%r11          */
    CHK_LEN(5, 0x68, 0, 0, 0, 0);                     /* push $imm32         */
    CHK_LEN(2, 0x6a, 0x05);                           /* push $imm8          */
    CHK_LEN(3, 0xc2, 0x08, 0x00);                     /* ret $8              */
    CHK_LEN(2, 0x0f, 0x05);                           /* syscall             */
    CHK_LEN(2, 0xcd, 0x03);                           /* int $3              */
    CHK_LEN(6, 0x48, 0xa9, 0x01, 0, 0, 0);            /* test $1,%rax        */
    CHK_LEN(3, 0x0f, 0x44, 0xc1);                     /* cmove %ecx,%eax     */
    CHK_LEN(3, 0x0f, 0x94, 0xc0);                     /* sete %al            */
    CHK_LEN(3, 0x0f, 0xaf, 0xc1);                     /* imul %ecx,%eax      */
    CHK_LEN(2, 0x0f, 0xc8);                           /* bswap %eax          */
    CHK_LEN(5, 0x66, 0x0f, 0xba, 0xe0, 0x01);         /* bt $1,%ax           */
    CHK_LEN(5, 0xf0, 0x48, 0x0f, 0xb1, 0x08);         /* lock cmpxchg        */
    CHK_LEN(2, 0xd1, 0xe0);                           /* shl %eax            */
    CHK_LEN(3, 0xc1, 0xe0, 0x04);                     /* shl $4,%eax         */
    CHK_LEN(3, 0x48, 0x63, 0xc0);                     /* movslq %eax,%rax    */
    CHK_LEN(6, 0x69, 0xc0, 0x34, 0x12, 0, 0);         /* imul $0x1234,...    */
    CHK_LEN(3, 0x6b, 0xc0, 0x05);                     /* imul $5,...         */
    CHK_LEN(2, 0x84, 0xc0);                           /* test %al,%al        */
    CHK_LEN(2, 0x3c, 0x05);                           /* cmp $5,%al          */
    CHK_LEN(5, 0x3d, 0x00, 0x01, 0, 0);               /* cmp $0x100,%eax     */
    CHK_LEN(6, 0x48, 0x3d, 0x00, 0x01, 0, 0);         /* cmp $0x100,%rax     */
    CHK_LEN(4, 0x66, 0x3d, 0x00, 0x01);               /* cmp $0x100,%ax      */
}

/* ModRM/SIB/displacement is the part of x86 that is exactly regular, and it is
 * the part a hand-rolled decoder gets wrong most quietly. Each of these was
 * read back from objdump. */
static void test_insn_length_addressing_forms(void) {
    CHK_LEN(3, 0x48, 0x8b, 0xc1);                     /* mod=3               */
    CHK_LEN(3, 0x48, 0x8b, 0x00);                     /* mod=0 rm=0          */
    CHK_LEN(7, 0x48, 0x8b, 0x05, 0, 0, 0, 0);         /* mod=0 rm=5 RIP-rel  */
    CHK_LEN(4, 0x48, 0x8b, 0x40, 0x08);               /* mod=1 disp8         */
    CHK_LEN(7, 0x48, 0x8b, 0x80, 0, 1, 0, 0);         /* mod=2 disp32        */
    CHK_LEN(4, 0x48, 0x8b, 0x04, 0x08);               /* mod=0 SIB, base=rax */
    CHK_LEN(8, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0);   /* mod=0 SIB base=none */
    CHK_LEN(5, 0x48, 0x8b, 0x44, 0x24, 0x08);         /* mod=1 SIB disp8     */
    CHK_LEN(8, 0x48, 0x8b, 0x84, 0x24, 0, 1, 0, 0);   /* mod=2 SIB disp32    */
    /* mod=1 with a base-less SIB still takes disp8, not disp32: the "no base"
     * rule only applies when mod is 0. Getting this backwards is a three-byte
     * error, which is a resume in the middle of the next instruction. */
    CHK_LEN(5, 0x48, 0x8b, 0x44, 0x25, 0x08);
}

static void test_insn_length_refuses_what_it_cannot_know(void) {
    /* A refusal is a correct answer. Each of these must be one. */
    CHK_LEN(0, 0x0f, 0x38, 0x00, 0xc0);               /* 0F 38 three-byte map */
    CHK_LEN(0, 0x0f, 0x3a, 0x0f, 0xc0, 0x01);         /* 0F 3A three-byte map */
    CHK_LEN(0, 0xc5, 0xf8, 0x57, 0xc0);               /* VEX2 vxorps          */
    CHK_LEN(0, 0xc4, 0xe2, 0x7d, 0xc0);               /* VEX3                 */
    CHK_LEN(0, 0x62, 0xf1, 0x7c, 0x48, 0x28, 0xc0);   /* EVEX vmovaps         */
    CHK_LEN(0, 0x0f, 0x10, 0xc0);                     /* movups (no SSE here) */
    CHK_LEN(0, 0xa3, 0, 0, 0, 0, 0, 0, 0, 0);         /* mov %eax,moffs64     */
    CHK_LEN(0, 0xc8, 0x10, 0x00, 0x00);               /* enter                */
    CHK_LEN(0, 0x06);                                 /* invalid in 64-bit    */
    CHK_LEN(0, 0x0e);
    CHK_LEN(0, 0x9a);                                 /* far call, invalid    */
    CHK_LEN(0, 0xea);                                 /* far jmp, invalid     */

    /* Truncated: the bytes we were given do not contain a whole instruction. */
    CHK_LEN(0, 0x48);                                 /* REX with no opcode   */
    CHK_LEN(0, 0x48, 0x8b);                           /* no ModRM             */
    CHK_LEN(0, 0x48, 0x8b, 0x04);                     /* ModRM needs a SIB    */
    CHK_LEN(0, 0x48, 0xc7, 0x00, 0x34, 0x12);         /* imm32 clipped        */
    CHK_LEN(0, 0x0f);                                 /* escape with no map2  */

    /* A 66 prefix on a rel32 branch: AMD and Intel disagree about whether the
     * displacement narrows. Refuse rather than pick a side. */
    CHK_LEN(0, 0x66, 0xe8, 0, 0, 0, 0);
    CHK_LEN(0, 0x66, 0x0f, 0x84, 0, 0, 0, 0);

    /* Nothing longer than the architectural limit ever decodes. */
    CHK_LEN(0, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
               0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90);

    /* Degenerate inputs. */
    CHECK_EQ(x86_insn_length(NULL, 4), 0);
    unsigned char one = 0xc3;
    CHECK_EQ(x86_insn_length(&one, 0),  0);
    CHECK_EQ(x86_insn_length(&one, -1), 0);
    CHECK_EQ(x86_insn_length(&one, 1),  1);
}

/* No byte string, however hostile, may make the decoder read past `avail`,
 * return a negative number, or return something longer than it was given. */
static void test_insn_length_fuzz(void) {
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (int iter = 0; iter < 200000; iter++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;

        unsigned char buf[24];
        memset(buf, 0xA5, sizeof buf);
        int avail = (int)(s % 16u) + 1;
        uint64_t bits = s;
        for (int i = 0; i < avail; i++) {
            buf[i] = (unsigned char)(bits & 0xFF);
            bits = bits * 6364136223846793005ULL + 1442695040888963407ULL;
        }
        int n = x86_insn_length(buf, avail);
        CHECK(n >= 0 && n <= avail && n <= 15);
        /* Trailing guard bytes untouched — the decoder never writes. */
        for (int i = avail; i < 24; i++) CHECK_EQ(buf[i], 0xA5);
    }
}

/* ====================================================================== */
/* registers                                                              */
/* ====================================================================== */

static void test_register_mapping(void) {
    static const char *const names[FAULT_REG_COUNT] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    };
    for (int i = 0; i < FAULT_REG_COUNT; i++) {
        CHECK_STR(fault_reg_name(i), names[i]);
        CHECK_EQ(fault_reg_lookup(names[i]), i);
    }
    CHECK_STR(fault_reg_name(-1), "?");
    CHECK_STR(fault_reg_name(FAULT_REG_COUNT), "?");

    /* Spellings a model will actually produce. */
    CHECK_EQ(fault_reg_lookup("RAX"),  FAULT_REG_RAX);
    CHECK_EQ(fault_reg_lookup("%rdi"), FAULT_REG_RDI);
    CHECK_EQ(fault_reg_lookup("%R15"), FAULT_REG_R15);
    /* And ones it must not get away with: a 32-bit name is a different
     * register width and would silently mean something else. */
    CHECK_EQ(fault_reg_lookup("eax"),   -1);
    CHECK_EQ(fault_reg_lookup("r16"),   -1);
    CHECK_EQ(fault_reg_lookup("r1"),    -1);
    CHECK_EQ(fault_reg_lookup("ra"),    -1);
    CHECK_EQ(fault_reg_lookup("rax "),  -1);
    CHECK_EQ(fault_reg_lookup(""),      -1);
    CHECK_EQ(fault_reg_lookup("rip"),   -1);
    CHECK_EQ(fault_reg_lookup(NULL),    -1);

    /* Every index must land on a distinct field of the frame, and the mapping
     * must be the architectural one. Writing a unique value through each slot
     * and reading the struct back proves both at once. */
    fault_frame_t f;
    memset(&f, 0, sizeof f);
    for (int i = 0; i < FAULT_REG_COUNT; i++) {
        uint64_t *p = fault_reg_slot(&f, i);
        CHECK(p != NULL);
        *p = 0x1000ULL + (uint64_t)i;
    }
    CHECK_EQ(f.rax, 0x1000); CHECK_EQ(f.rcx, 0x1001);
    CHECK_EQ(f.rdx, 0x1002); CHECK_EQ(f.rbx, 0x1003);
    CHECK_EQ(f.rsp, 0x1004); CHECK_EQ(f.rbp, 0x1005);
    CHECK_EQ(f.rsi, 0x1006); CHECK_EQ(f.rdi, 0x1007);
    CHECK_EQ(f.r8,  0x1008); CHECK_EQ(f.r9,  0x1009);
    CHECK_EQ(f.r10, 0x100a); CHECK_EQ(f.r11, 0x100b);
    CHECK_EQ(f.r12, 0x100c); CHECK_EQ(f.r13, 0x100d);
    CHECK_EQ(f.r14, 0x100e); CHECK_EQ(f.r15, 0x100f);

    CHECK(fault_reg_slot(&f, -1) == NULL);
    CHECK(fault_reg_slot(&f, FAULT_REG_COUNT) == NULL);
    CHECK(fault_reg_slot(NULL, 0) == NULL);
}

/* ====================================================================== */
/* fault_apply_plan — the function that decides whether the machine lives  */
/* ====================================================================== */

/* A frame that faulted on `mov (%rax),%rax` at CODE_BASE + 0x40. The bytes
 * live in fake_code and the window's image range points at them, exactly as
 * fault_capture would have left a record. */
static const unsigned char PF_INSN[3] = { 0x48, 0x8b, 0x00 };   /* 3 bytes */

static void pf_frame(fault_frame_t *f) {
    build_stack(3);
    frame_defaults(f);
    f->vector     = 14;
    f->error_code = 0x0;
    f->rip        = CODE_BASE + 0x40;
    f->cr2        = 0x0000001000000000ULL;
    f->rax        = 0x0000001000000000ULL;
    f->rbp        = slot_addr(0);
    f->rsp        = slot_addr(0);
}

static void test_apply_skip(void) {
    fault_frame_t f;
    pf_frame(&f);
    fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1, .uses = 1 };

    char    why[160];
    uint8_t ilen = 0;
    CHECK_EQ(fault_apply_plan(&f, &win, &p, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_OK);
    CHECK_EQ(ilen, 3);
    CHECK_EQ(f.rip, CODE_BASE + 0x43);
    CHECK_CONTAINS(why, "skipped");
    CHECK_CONTAINS(why, "3-byte");
    /* Nothing else in the frame may move. */
    CHECK_EQ(f.rax, 0x0000001000000000ULL);
    CHECK_EQ(f.rsp, slot_addr(0));
}

static void test_apply_set_register(void) {
    char    why[160];
    uint8_t ilen;

    /* set + retry: the register changes, RIP does not. */
    fault_frame_t f;
    pf_frame(&f);
    fault_plan_t retry = { .action = FAULT_ACT_SET_REG, .reg = FAULT_REG_RAX,
                           .then_skip = 0, .any_vector = 1, .uses = 1,
                           .value = 0x00100200 };
    CHECK_EQ(fault_apply_plan(&f, &win, &retry, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_OK);
    CHECK_EQ(f.rax, 0x00100200);
    CHECK_EQ(f.rip, CODE_BASE + 0x40);
    CHECK_CONTAINS(why, "re-executed");

    /* set + skip: both change. */
    pf_frame(&f);
    fault_plan_t skip = retry;
    skip.then_skip = 1;
    skip.reg       = FAULT_REG_R12;
    skip.value     = 0xDEADBEEF;
    CHECK_EQ(fault_apply_plan(&f, &win, &skip, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_OK);
    CHECK_EQ(f.r12, 0xDEADBEEF);
    CHECK_EQ(f.rip, CODE_BASE + 0x43);
    CHECK_CONTAINS(why, "r12");

    /* RSP is refused. The saved RSP is what IRETQ reloads; a plan that can put
     * a wrong value there can manufacture the double fault this whole module
     * exists to survive. */
    pf_frame(&f);
    fault_plan_t bad_rsp = retry;
    bad_rsp.reg = FAULT_REG_RSP;
    CHECK_EQ(fault_apply_plan(&f, &win, &bad_rsp, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_BAD_PLAN);
    CHECK_EQ(f.rsp, slot_addr(0));
    CHECK_CONTAINS(why, "rsp");

    /* "Retry" reuses the faulting RIP rather than computing one, so it is the
     * one path that could resume outside the image without ever patching RIP.
     * It must be refused there too: a machine already executing outside its own
     * text is not one a register write can rescue. */
    pf_frame(&f);
    f.rip = 0x0000001000000000ULL;
    fault_frame_t untouched = f;
    CHECK_EQ(fault_apply_plan(&f, &win, &retry, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_RIP_WINDOW);
    CHECK_EQ(memcmp(&f, &untouched, sizeof f), 0);
    CHECK_CONTAINS(why, "outside the kernel image");

    /* An out-of-range register index refuses and writes nothing. */
    pf_frame(&f);
    fault_plan_t bad_reg = retry;
    bad_reg.reg = 99;
    CHECK_EQ(fault_apply_plan(&f, &win, &bad_reg, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_BAD_PLAN);
    CHECK_EQ(f.rax, 0x0000001000000000ULL);
}

/* When a set-and-skip cannot compute the skip, the register must NOT have been
 * written: a refusal has to leave the frame exactly as the CPU left it, or the
 * caller cannot safely halt afterwards. */
static void test_apply_refusal_is_atomic(void) {
    static const unsigned char junk[3] = { 0x0f, 0x38, 0x00 };  /* undecodable */
    char    why[160];
    uint8_t ilen;

    fault_frame_t f;
    pf_frame(&f);
    fault_frame_t before = f;
    fault_plan_t p = { .action = FAULT_ACT_SET_REG, .reg = FAULT_REG_RAX,
                       .then_skip = 1, .any_vector = 1, .uses = 1,
                       .value = 0x1234 };
    CHECK_EQ(fault_apply_plan(&f, &win, &p, junk, sizeof junk,
                              why, sizeof why, &ilen), FAULT_FIX_UNDECODABLE);
    CHECK_EQ(memcmp(&f, &before, sizeof f), 0);
    CHECK_CONTAINS(why, "step over");
}

static void test_apply_set_rip_and_the_window(void) {
    char    why[160];
    uint8_t ilen;
    fault_frame_t f;

    /* Inside the image: accepted. */
    pf_frame(&f);
    fault_plan_t good = { .action = FAULT_ACT_SET_RIP, .any_vector = 1,
                          .uses = 1, .value = CODE_BASE + 0x800 };
    CHECK_EQ(fault_apply_plan(&f, &win, &good, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_OK);
    CHECK_EQ(f.rip, CODE_BASE + 0x800);

    /* THE CHECK THAT MATTERS: outside [code_lo, code_hi) is refused, and the
     * frame is untouched. Resuming outside the loaded image is not recovery,
     * it is executing whatever happens to be there. */
    static const uint64_t outside[] = {
        0,                          /* the null page: mapped here, still wrong */
        CODE_BASE - 1,
        CODE_TOP,                   /* half-open: the top is NOT inside        */
        CODE_TOP + 0x1000,
        0x0000001000000000ULL,      /* the unmapped canonical address          */
        0xFFFFFFFFFFFFFFFFULL,
        0x0000800000000000ULL,      /* non-canonical                           */
    };
    for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        pf_frame(&f);
        fault_frame_t before = f;
        fault_plan_t bad = good;
        bad.value = outside[i];
        CHECK_EQ(fault_apply_plan(&f, &win, &bad, PF_INSN, sizeof PF_INSN,
                                  why, sizeof why, &ilen),
                 FAULT_FIX_RIP_WINDOW);
        CHECK_EQ(memcmp(&f, &before, sizeof f), 0);
        CHECK_CONTAINS(why, "outside the kernel image");
    }

    /* A skip whose landing address would fall outside is refused the same way:
     * RIP + length is just as much a produced address as an explicit jump. */
    pf_frame(&f);
    f.rip = CODE_TOP - 2;                     /* + 3 bytes lands past the top */
    fault_plan_t skip = { .action = FAULT_ACT_SKIP, .any_vector = 1, .uses = 1 };
    CHECK_EQ(fault_apply_plan(&f, &win, &skip, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_RIP_WINDOW);
    CHECK_EQ(f.rip, CODE_TOP - 2);

    /* And an RIP whose skip would wrap the address space. */
    pf_frame(&f);
    f.rip = 0xFFFFFFFFFFFFFFFFULL;
    CHECK_EQ(fault_apply_plan(&f, &win, &skip, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_RIP_WINDOW);
}

static void test_apply_refusals(void) {
    char    why[160];
    uint8_t ilen;
    fault_frame_t f;

    fault_plan_t skip = { .action = FAULT_ACT_SKIP, .any_vector = 1, .uses = 1 };

    /* #DF is never recoverable, whatever the plan says. Its RSP is the broken
     * thing; IRETQ would put it straight back. */
    pf_frame(&f);
    f.vector = 8;
    fault_frame_t before = f;
    CHECK_EQ(fault_apply_plan(&f, &win, &skip, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_VECTOR);
    CHECK_EQ(memcmp(&f, &before, sizeof f), 0);
    CHECK_CONTAINS(why, "#DF");

    /* ... and #MC, for the same shape of reason. */
    pf_frame(&f);
    f.vector = 18;
    CHECK_EQ(fault_apply_plan(&f, &win, &skip, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_VECTOR);

    /* Vector eligibility outranks an explicit refusal, so a #DF report always
     * says "never resumable" rather than "the model declined". */
    pf_frame(&f);
    f.vector = 8;
    fault_plan_t refuse = { .action = FAULT_ACT_REFUSE, .any_vector = 1,
                            .uses = 1 };
    CHECK_EQ(fault_apply_plan(&f, &win, &refuse, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_VECTOR);

    /* No plan at all. */
    pf_frame(&f);
    CHECK_EQ(fault_apply_plan(&f, &win, NULL, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_NO_PLAN);
    fault_plan_t none = { .action = FAULT_ACT_NONE, .any_vector = 1, .uses = 1 };
    CHECK_EQ(fault_apply_plan(&f, &win, &none, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_NO_PLAN);

    /* An explicit refusal is reported as one, and is NOT the same answer as
     * "nobody decided anything". */
    pf_frame(&f);
    CHECK_EQ(fault_apply_plan(&f, &win, &refuse, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_REFUSED);

    /* Exhausted. */
    pf_frame(&f);
    fault_plan_t spent = skip;
    spent.uses = 0;
    CHECK_EQ(fault_apply_plan(&f, &win, &spent, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_EXHAUSTED);

    /* Scoped to the wrong vector. */
    pf_frame(&f);
    fault_plan_t scoped = skip;
    scoped.any_vector = 0;
    scoped.vector     = 6;
    CHECK_EQ(fault_apply_plan(&f, &win, &scoped, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_WRONG_VECTOR);
    CHECK_CONTAINS(why, "#UD");
    /* Scoped to the right one. */
    pf_frame(&f);
    scoped.vector = 14;
    CHECK_EQ(fault_apply_plan(&f, &win, &scoped, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_OK);

    /* No readable bytes at RIP: nothing to decode, so nothing to skip. */
    pf_frame(&f);
    CHECK_EQ(fault_apply_plan(&f, &win, &skip, NULL, 0,
                              why, sizeof why, &ilen), FAULT_FIX_NO_CODE);

    /* Garbage action. */
    pf_frame(&f);
    fault_plan_t weird = skip;
    weird.action = 200;
    CHECK_EQ(fault_apply_plan(&f, &win, &weird, PF_INSN, sizeof PF_INSN,
                              why, sizeof why, &ilen), FAULT_FIX_BAD_PLAN);

    /* Null arguments must be refused, not dereferenced. */
    CHECK_EQ(fault_apply_plan(NULL, &win, &skip, PF_INSN, 3, why, sizeof why,
                              &ilen), FAULT_FIX_BAD_PLAN);
    pf_frame(&f);
    CHECK_EQ(fault_apply_plan(&f, NULL, &skip, PF_INSN, 3, why, sizeof why,
                              &ilen), FAULT_FIX_BAD_PLAN);
    /* Optional out-parameters really are optional. */
    pf_frame(&f);
    CHECK_EQ(fault_apply_plan(&f, &win, &skip, PF_INSN, 3, NULL, 0, NULL),
             FAULT_FIX_OK);
}

static void test_plan_check(void) {
    fault_window_t w = win;

    fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1, .uses = 1 };
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_OK);

    CHECK_EQ(fault_plan_check(NULL, &w), FAULT_FIX_BAD_PLAN);

    p.uses = 0;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_BAD_PLAN);
    p.uses = FAULT_PLAN_USES_MAX + 1;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_BAD_PLAN);
    p.uses = FAULT_PLAN_USES_MAX;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_OK);
    p.uses = 1;

    p.action = FAULT_ACT_COUNT;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_BAD_PLAN);
    p.action = FAULT_ACT_NONE;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_NO_PLAN);

    /* A plan cannot be scoped to a vector it could never help with. */
    p.action     = FAULT_ACT_SKIP;
    p.any_vector = 0;
    p.vector     = 8;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_VECTOR);
    p.vector = 18;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_VECTOR);
    p.vector = 14;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_OK);
    p.vector = 999;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_BAD_PLAN);
    p.any_vector = 1;

    p.action = FAULT_ACT_SET_REG;
    p.reg    = FAULT_REG_RSP;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_BAD_PLAN);
    p.reg = FAULT_REG_COUNT;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_BAD_PLAN);
    p.reg = FAULT_REG_RAX;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_OK);

    p.action = FAULT_ACT_SET_RIP;
    p.value  = CODE_BASE + 4;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_OK);
    p.value = CODE_TOP;
    CHECK_EQ(fault_plan_check(&p, &w), FAULT_FIX_RIP_WINDOW);
    /* With no window published there is nothing to validate against, so a
     * set_rip cannot be armed at all. */
    p.value = CODE_BASE + 4;
    CHECK_EQ(fault_plan_check(&p, NULL), FAULT_FIX_RIP_WINDOW);
}

/* Fuzz the whole decision surface: no plan, however malformed, may make
 * fault_apply_plan return OK with a resume address outside the window. */
static void test_apply_plan_fuzz(void) {
    uint64_t s = 0xB5026F5AA96619E9ULL;
    build_stack(3);

    for (int iter = 0; iter < 60000; iter++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;

        fault_frame_t f;
        frame_defaults(&f);
        f.vector = s % 40;
        f.rip    = (s & 1) ? CODE_BASE + (s % 0x1000) : s;
        f.rbp    = slot_addr(0);
        f.rsp    = slot_addr(0);

        unsigned char code[FAULT_CODE_BYTES];
        uint64_t bits = s * 2654435761u;
        for (unsigned i = 0; i < sizeof code; i++) {
            code[i] = (unsigned char)bits;
            bits = bits * 6364136223846793005ULL + 1ULL;
        }

        fault_plan_t p;
        memset(&p, 0, sizeof p);
        p.action     = (uint8_t)(s % 7);          /* includes out-of-range   */
        p.reg        = (uint8_t)((s >> 8) % 20);  /* includes out-of-range   */
        p.then_skip  = (uint8_t)((s >> 16) & 1);
        p.any_vector = (uint8_t)((s >> 17) & 1);
        p.vector     = (s >> 18) % 300;
        p.value      = s ^ 0xA5A5A5A5A5A5A5A5ULL;
        p.uses       = (uint32_t)((s >> 24) % 4);

        char        why[FAULT_DETAIL_MAX];
        uint8_t     ilen = 0;
        uint64_t    rip_before = f.rip;
        fault_fix_t rc = fault_apply_plan(&f, &win, &p,
                                          (s & 2) ? code : NULL,
                                          (s & 2) ? sizeof code : 0,
                                          why, sizeof why, &ilen);
        CHECK(rc >= 0 && rc < FAULT_FIX_COUNT);
        CHECK(memchr(why, 0, sizeof why) != NULL);   /* always terminated */

        if (rc == FAULT_FIX_OK) {
            /* THE INVARIANT. Whatever the plan asked for, if this returned OK
             * then RIP is somewhere the kernel is actually loaded. */
            CHECK(f.rip >= win.code_lo && f.rip < win.code_hi);
            CHECK(fault_is_recoverable(f.vector));
        } else {
            CHECK_EQ(f.rip, rip_before);             /* refusals change nothing */
        }
    }
}

/* ====================================================================== */
/* the live path: ring, disposition, re-fault counter                     */
/* ====================================================================== */

/* For the LIVE tests the code window has to be the same range the instruction
 * bytes live in, because that is the situation on the real machine: RIP is
 * inside the loaded image and RIP + a length has to stay inside it. build_stack
 * aims code_lo/hi at a purely synthetic range, which is right for the backtrace
 * tests and wrong here, so narrow it onto fake_code. */
static void live_window(void) {
    build_stack(3);
    win.code_lo = win.image_lo;
    win.code_hi = win.image_hi;
    /* A whole array of decodable 3-byte `mov (%rax),%rax`, so a fault anywhere
     * in it captures an instruction the length decoder is certain about. */
    for (unsigned i = 0; i + sizeof PF_INSN <= sizeof fake_code;
         i += sizeof PF_INSN)
        memcpy(fake_code + i, PF_INSN, sizeof PF_INSN);
}

/* The window has to be published for a set_rip plan to be armable, exactly as
 * idt_init() publishes it on the real machine. */
static void publish_window(void) {
    live_window();
    fault_set_window(&win);
}

/* Take one fault at `rip` on `vector` and run the whole live path over it,
 * exactly as isr_dispatch() does. Returns the disposition. */
static fault_fix_t take_fault(fault_frame_t *f, uint64_t vector, uint64_t rip) {
    live_window();
    frame_defaults(f);
    f->vector = vector;
    f->rip    = rip;
    f->rbp    = slot_addr(0);
    f->rsp    = slot_addr(0);
    fault_note(f, &win, 1000 + (rip & 0xFFFF));
    return fault_recover(f, &win);
}

static void test_live_recovery_resumes(void) {
    fault_reset();
    publish_window();

    /* With nothing armed, a fatal fault stays fatal. That is the default and it
     * must not have changed. */
    fault_frame_t f;
    CHECK_EQ(take_fault(&f, 14, win.image_lo), FAULT_FIX_NO_PLAN);
    CHECK_EQ(fault_last()->recovered, 0);
    CHECK_CONTAINS(fault_last_report(), "no-plan");
    CHECK_CONTAINS(fault_last_report(), "HALTED");

    /* Arm a skip and take the same fault: now it resumes, past a 3-byte
     * instruction, and the report says so. */
    fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1, .uses = 1 };
    CHECK_EQ(fault_plan_arm(&p), FAULT_FIX_OK);
    CHECK(fault_plan_get() != NULL);

    CHECK_EQ(take_fault(&f, 14, win.image_lo), FAULT_FIX_OK);
    CHECK_EQ(f.rip, win.image_lo + 3);
    CHECK_EQ(fault_last()->recovered, 1);
    CHECK_EQ(fault_last()->insn_len, 3);
    CHECK_EQ(fault_last()->resume_rip, win.image_lo + 3);
    CHECK_CONTAINS(fault_last_report(), "RESUMED");
    CHECK_CONTAINS(fault_last_report(), "skipped the 3-byte instruction");
    /* The captured registers are still the ones that FAULTED, not the patched
     * ones — a record, not a rationalisation. */
    CHECK_EQ(fault_last()->regs.rip, win.image_lo);

    /* One use, so the plan disarmed itself. */
    CHECK(fault_plan_get() == NULL);
    CHECK_EQ(take_fault(&f, 14, win.image_lo), FAULT_FIX_NO_PLAN);
}

static void test_live_trap_still_resumes_untouched(void) {
    fault_reset();
    publish_window();

    fault_frame_t f;
    CHECK_EQ(take_fault(&f, 3, win.image_lo), FAULT_FIX_TRAP);   /* #BP */
    CHECK_EQ(f.rip, win.image_lo);                 /* nothing was modified */
    CHECK_EQ(fault_last()->recovered, 1);
    CHECK_CONTAINS(fault_last_report(), "trap");
}

/* THE LOOP GUARD. A "set a register and retry" plan that does not actually fix
 * anything re-faults at exactly the same RIP, forever. The kernel must stop. */
static void test_refault_counter_stops_a_loop(void) {
    fault_reset();
    publish_window();

    fault_plan_t p = { .action = FAULT_ACT_SET_REG, .reg = FAULT_REG_RAX,
                       .then_skip = 0,                  /* retry, do not skip */
                       .any_vector = 1, .uses = FAULT_PLAN_USES_MAX,
                       .value = 0x0000001000000000ULL };
    CHECK_EQ(fault_plan_arm(&p), FAULT_FIX_OK);

    fault_frame_t f;
    for (int i = 1; i <= FAULT_REFAULT_MAX; i++) {
        CHECK_EQ(take_fault(&f, 14, win.image_lo), FAULT_FIX_OK);
        CHECK_EQ(f.rip, win.image_lo);              /* retried, so no progress */
        CHECK_EQ(fault_refaults(), (uint32_t)i);
    }
    /* The next one is refused, and the machine is told exactly why. */
    CHECK_EQ(take_fault(&f, 14, win.image_lo), FAULT_FIX_REFAULT);
    CHECK_EQ(fault_last()->recovered, 0);
    CHECK_CONTAINS(fault_last_report(), "refault");
    CHECK_CONTAINS(fault_last_report(), "giving up");
    /* And it stays refused however many more arrive: the loop cannot restart
     * itself just by running longer. */
    CHECK_EQ(take_fault(&f, 14, win.image_lo), FAULT_FIX_REFAULT);
    CHECK_EQ(take_fault(&f, 14, win.image_lo), FAULT_FIX_REFAULT);

    /* A fault somewhere else resets the count: this guard is about one address
     * repeating, not about a machine that faults a lot. */
    CHECK_EQ(take_fault(&f, 14, win.image_lo + 8), FAULT_FIX_OK);
    CHECK_EQ(fault_refaults(), 1);
    CHECK_EQ(fault_refault_rip(), win.image_lo + 8);
}

static void test_plan_uses_are_consumed(void) {
    fault_reset();
    publish_window();

    fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1, .uses = 3 };
    CHECK_EQ(fault_plan_arm(&p), FAULT_FIX_OK);

    fault_frame_t f;
    /* Different addresses each time, so the re-fault guard stays out of it. */
    CHECK_EQ(take_fault(&f, 14, win.image_lo + 0),  FAULT_FIX_OK);
    CHECK_EQ(fault_plan_get()->uses, 2);
    CHECK_EQ(take_fault(&f, 14, win.image_lo + 8),  FAULT_FIX_OK);
    CHECK_EQ(fault_plan_get()->uses, 1);
    CHECK_EQ(take_fault(&f, 14, win.image_lo + 16), FAULT_FIX_OK);
    CHECK(fault_plan_get() == NULL);
    CHECK_EQ(take_fault(&f, 14, win.image_lo + 24), FAULT_FIX_NO_PLAN);
}

static void test_plan_arm_validates_against_the_live_window(void) {
    fault_reset();
    publish_window();

    /* An in-image target is armable. */
    fault_plan_t good = { .action = FAULT_ACT_SET_RIP, .any_vector = 1,
                          .uses = 1, .value = 0 };
    good.value = win.image_lo + 0x10;
    CHECK_EQ(fault_plan_arm(&good), FAULT_FIX_OK);
    fault_plan_clear();

    /* An out-of-image one is refused AT ARM TIME, and nothing is armed. */
    fault_plan_t bad = good;
    bad.value = 0x0000001000000000ULL;
    CHECK_EQ(fault_plan_arm(&bad), FAULT_FIX_RIP_WINDOW);
    CHECK(fault_plan_get() == NULL);

    /* A refused arm must not disturb a plan that is already there. */
    CHECK_EQ(fault_plan_arm(&good), FAULT_FIX_OK);
    CHECK_EQ(fault_plan_arm(&bad),  FAULT_FIX_RIP_WINDOW);
    CHECK(fault_plan_get() != NULL);
    CHECK_EQ(fault_plan_get()->value, win.image_lo + 0x10);

    fault_reset();
    CHECK(fault_window() == NULL);
    CHECK_EQ(fault_plan_arm(&good), FAULT_FIX_RIP_WINDOW);
}

/* The report grew a disposition block, and that block is the part most likely
 * to be extended later. Measure the longest one the kernel can actually
 * produce — a set-and-skip, whose detail line names a register, a 64-bit value
 * and two addresses, on a fault with every register set — and assert real
 * headroom rather than "it fit this time". A report that silently loses its
 * last lines loses the backtrace and the verdict, which are the two things
 * worth having. */
static void test_report_budget_with_a_disposition(void) {
    fault_reset();
    publish_window();

    fault_plan_t p = { .action = FAULT_ACT_SET_REG, .reg = FAULT_REG_R15,
                       .then_skip = 1, .any_vector = 1, .uses = 1,
                       .value = UINT64_MAX };
    CHECK_EQ(fault_plan_arm(&p), FAULT_FIX_OK);

    fault_frame_t f;
    live_window();
    frame_defaults(&f);
    f.vector     = 14;
    f.error_code = 0xFFFF;
    f.cr2        = UINT64_MAX;
    f.rflags     = UINT64_MAX;
    f.rip        = win.image_lo;
    f.rbp        = slot_addr(0);
    f.rsp        = slot_addr(0);
    f.rax = f.rbx = f.rcx = f.rdx = f.rsi = f.rdi = UINT64_MAX;
    f.r8 = f.r9 = f.r10 = f.r11 = f.r12 = f.r13 = f.r14 = f.r15 = UINT64_MAX;
    fault_note(&f, &win, UINT64_MAX);
    CHECK_EQ(fault_recover(&f, &win), FAULT_FIX_OK);
    CHECK_CONTAINS(fault_last_report(), "set r15 = 0xffffffffffffffff");
    CHECK_CONTAINS(fault_last_report(), "RESUMED");

    size_t used = strlen(fault_last_report());
    printf("      [longest report: %zu bytes; %d spare in FAULT_REPORT_MAX, "
           "%d spare in TOOL_RESULT_MAX]\n",
           used, (int)(FAULT_REPORT_MAX - used), (int)(TOOL_RESULT_MAX - used));
    CHECK(used + 256 < FAULT_REPORT_MAX);
    CHECK(used + 256 < TOOL_RESULT_MAX);

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    call_tool("fault_report", "{}", buf, sizeof buf, &r);
    CHECK_EQ(r.truncated, 0);
    CHECK_STR(buf, fault_last_report());
}

/* ====================================================================== */
/* the ring                                                               */
/* ====================================================================== */

static void test_ring_retains_and_wraps(void) {
    fault_reset();
    publish_window();

    CHECK_EQ(fault_ring_len(), 0);
    CHECK(fault_ring_at(0) == NULL);
    CHECK(fault_by_seq(1) == NULL);

    fault_frame_t f;
    for (uint32_t i = 1; i <= FAULT_RING_SIZE; i++) {
        take_fault(&f, 14, win.image_lo + i * 4);
        CHECK_EQ(fault_ring_len(), i);
        CHECK_EQ(fault_last()->seq, i);
        CHECK(fault_by_seq(i) != NULL);
        CHECK_EQ(fault_by_seq(i)->regs.rip, win.image_lo + i * 4);
    }

    /* Newest first. */
    for (uint32_t back = 0; back < FAULT_RING_SIZE; back++) {
        const fault_record_t *rec = fault_ring_at(back);
        CHECK(rec != NULL);
        CHECK_EQ(rec->seq, FAULT_RING_SIZE - back);
    }
    CHECK(fault_ring_at(FAULT_RING_SIZE) == NULL);

    /* A fault_note that cannot capture anything must not consume a slot.
     * fault_capture zeroes its destination first, so entering the ring with
     * nothing to record would trade the oldest retained fault for an empty
     * hole — the opposite of what the ring is for. */
    uint32_t before_count = fault_count();
    fault_note(NULL, &win, 1);
    fault_note(&f, NULL, 1);
    CHECK_EQ(fault_count(), before_count);
    CHECK_EQ(fault_ring_len(), FAULT_RING_SIZE);
    CHECK(fault_by_seq(1) != NULL);
    CHECK_EQ(fault_last()->seq, before_count);

    /* One more evicts the oldest and nothing else. */
    take_fault(&f, 6, win.image_lo + 0x28);
    CHECK_EQ(fault_ring_len(), FAULT_RING_SIZE);
    CHECK(fault_by_seq(1) == NULL);
    CHECK(fault_by_seq(2) != NULL);
    CHECK(fault_by_seq(FAULT_RING_SIZE + 1) != NULL);
    CHECK_EQ(fault_by_seq(FAULT_RING_SIZE + 1)->regs.vector, 6);
    CHECK_EQ(fault_count(), FAULT_RING_SIZE + 1);
    CHECK(fault_by_seq(0) == NULL);
    CHECK(fault_by_seq(9999) == NULL);
}

/* THE REASON THE RING EXISTS. A caller holding a record — which is exactly what
 * the fault_report tool does — must not have it rewritten by a fault that
 * happens while it is being read. */
static void test_ring_survives_a_fault_during_a_report(void) {
    fault_reset();
    publish_window();
    fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1,
                       .uses = FAULT_PLAN_USES_MAX };
    CHECK_EQ(fault_plan_arm(&p), FAULT_FIX_OK);

    fault_frame_t f;
    take_fault(&f, 14, win.image_lo);                 /* seq 1 */

    /* Begin a fault_report call and keep the answer. */
    char         buf1[TOOL_RESULT_MAX];
    tool_result_t r;
    CHECK_EQ(call_tool("fault_report", "{\"seq\":1}", buf1, sizeof buf1, &r),
             TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf1, "fault #1");

    /* Also hold the record itself, as the kernel-side caller would. */
    const fault_record_t *held = fault_by_seq(1);
    CHECK(held != NULL);
    uint64_t held_rip = held->regs.rip;

    /* Now the machine faults again, mid-diagnosis, twice. */
    take_fault(&f, 6,  win.image_lo + 0x10);          /* seq 2 */
    take_fault(&f, 13, win.image_lo + 0x20);          /* seq 3 */
    CHECK_EQ(fault_last()->seq, 3);

    /* The held pointer is still fault #1, unchanged. */
    CHECK(held == fault_by_seq(1));
    CHECK_EQ(held->seq, 1);
    CHECK_EQ(held->regs.rip, held_rip);
    CHECK_EQ(held->regs.vector, 14);

    /* And re-asking for the same report gives byte-identical text. Before the
     * ring, this second call would have described fault #3. */
    char buf2[TOOL_RESULT_MAX];
    CHECK_EQ(call_tool("fault_report", "{\"seq\":1}", buf2, sizeof buf2, &r),
             TOOL_OK);
    CHECK_STR(buf2, buf1);
    CHECK(strstr(buf2, "#GP") == NULL);

    /* The newest report is a different fault and says so. */
    char buf3[TOOL_RESULT_MAX];
    CHECK_EQ(call_tool("fault_report", "{}", buf3, sizeof buf3, &r), TOOL_OK);
    CHECK_CONTAINS(buf3, "fault #3");
    CHECK_CONTAINS(buf3, "#GP");
}

/* ====================================================================== */
/* the recovery tools                                                     */
/* ====================================================================== */

static void test_tool_report_by_seq(void) {
    fault_reset();
    publish_window();
    fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1,
                       .uses = FAULT_PLAN_USES_MAX };
    fault_plan_arm(&p);

    fault_frame_t f;
    take_fault(&f, 14, win.image_lo);
    take_fault(&f, 6,  win.image_lo + 0x20);

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;

    CHECK_EQ(call_tool("fault_report", "{\"seq\":1}", buf, sizeof buf, &r),
             TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "#PF");

    CHECK_EQ(call_tool("fault_report", "{\"seq\":2}", buf, sizeof buf, &r),
             TOOL_OK);
    CHECK_CONTAINS(buf, "#UD");
    /* seq 2 is also the newest, so it must be the exact console bytes. */
    CHECK_STR(buf, fault_last_report());

    /* A seq that never existed, and one that is out of range. */
    static const char *bad[] = {
        "{\"seq\":99}", "{\"seq\":0}", "{\"seq\":-1}", "{\"seq\":\"1\"}",
        "{\"seq\":true}", "{\"seq\":1.5e400}",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_EQ(call_tool("fault_report", bad[i], buf, sizeof buf, &r),
                 TOOL_OK);
        CHECK_EQ(r.is_error, 1);
    }

    /* Junk that is not an object at all falls back to the newest report rather
     * than failing: fault_report used to take no arguments and must stay usable
     * that way. */
    CHECK_EQ(call_tool("fault_report", "not json", buf, sizeof buf, &r),
             TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(call_tool("fault_report", NULL, buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
}

static void test_tool_history(void) {
    fault_reset();
    publish_window();

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    CHECK_EQ(call_tool("fault_history", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_CONTAINS(buf, "No CPU exception");

    fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1,
                       .uses = FAULT_PLAN_USES_MAX };
    fault_plan_arm(&p);
    fault_frame_t f;
    take_fault(&f, 14, win.image_lo);
    take_fault(&f, 6,  win.image_lo + 0x10);
    take_fault(&f, 13, win.image_lo + 0x20);

    CHECK_EQ(call_tool("fault_history", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "#1");
    CHECK_CONTAINS(buf, "#2");
    CHECK_CONTAINS(buf, "#3");
    CHECK_CONTAINS(buf, "#PF");
    CHECK_CONTAINS(buf, "#UD");
    CHECK_CONTAINS(buf, "#GP");
    CHECK_CONTAINS(buf, "ok");

    /* Even after wrapping it never claims more than it kept. */
    for (int i = 0; i < 20; i++) take_fault(&f, 14, win.image_lo + (i % 8) * 4);
    CHECK_EQ(call_tool("fault_history", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.truncated, 0);
    CHECK(strstr(buf, " #1 ") == NULL);
}

static void test_tool_recover_arms_a_plan(void) {
    fault_reset();
    publish_window();

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;

    CHECK_EQ(call_tool("fault_recover", "{\"action\":\"skip\"}",
                       buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "armed");
    CHECK(fault_plan_get() != NULL);
    CHECK_EQ(fault_plan_get()->action, FAULT_ACT_SKIP);
    CHECK_EQ(fault_plan_get()->uses, 1);
    CHECK_EQ(fault_plan_get()->any_vector, 1);

    /* And it really is the plan the handler will use. */
    fault_frame_t f;
    CHECK_EQ(take_fault(&f, 14, win.image_lo), FAULT_FIX_OK);
    CHECK_EQ(f.rip, win.image_lo + 3);

    CHECK_EQ(call_tool("fault_recover",
                       "{\"action\":\"set_register\",\"register\":\"%R12\","
                       "\"value\":\"0xDEAD\",\"then\":\"skip\",\"vector\":14,"
                       "\"uses\":4}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(fault_plan_get()->action, FAULT_ACT_SET_REG);
    CHECK_EQ(fault_plan_get()->reg, FAULT_REG_R12);
    CHECK_EQ(fault_plan_get()->value, 0xDEAD);
    CHECK_EQ(fault_plan_get()->then_skip, 1);
    CHECK_EQ(fault_plan_get()->any_vector, 0);
    CHECK_EQ(fault_plan_get()->vector, 14);
    CHECK_EQ(fault_plan_get()->uses, 4);

    char args[128];
    snprintf(args, sizeof args,
             "{\"action\":\"set_rip\",\"value\":\"0x%llx\"}",
             (unsigned long long)(win.image_lo + 0x10));
    CHECK_EQ(call_tool("fault_recover", args, buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(fault_plan_get()->action, FAULT_ACT_SET_RIP);
    CHECK_EQ(fault_plan_get()->value, win.image_lo + 0x10);

    CHECK_EQ(call_tool("fault_recover", "{\"action\":\"refuse\"}",
                       buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(fault_plan_get()->action, FAULT_ACT_REFUSE);

    CHECK_EQ(call_tool("fault_recover", "{\"action\":\"none\"}",
                       buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(buf, "cleared");
    CHECK(fault_plan_get() == NULL);
}

/* The model is untrusted and this tool steers a CPU. Every one of these must
 * produce a legible refusal, must not arm anything, and must not crash. */
static void test_tool_recover_rejects_bad_input(void) {
    static const char *bad[] = {
        NULL, "", "not json", "[1,2,3]", "42", "\"skip\"", "{}",
        "{\"action\":\"skip\"",                       /* truncated JSON     */
        "{\"action\":14}",                            /* wrong type         */
        "{\"action\":\"jump\"}",                      /* unknown action     */
        "{\"action\":\"SKIP\"}",                      /* case matters       */
        "{\"action\":\"skip \"}",
        "{\"action\":\"skipskipskipskipskipskipskip\"}",
        "{\"actin\":\"skip\"}",                       /* unknown field      */
        "{\"action\":\"skip\",\"reg\":\"rax\"}",      /* near-miss field    */
        "{\"action\":\"skip\",\"register\":\"rax\"}", /* not for this action*/
        "{\"action\":\"skip\",\"value\":\"0x1\"}",
        "{\"action\":\"skip\",\"then\":\"retry\"}",
        "{\"action\":\"set_rip\",\"then\":\"skip\",\"value\":1}",
        "{\"action\":\"set_register\"}",              /* no register        */
        "{\"action\":\"set_register\",\"register\":\"rax\"}",   /* no value */
        "{\"action\":\"set_register\",\"value\":\"0x1\"}",      /* no reg   */
        "{\"action\":\"set_register\",\"register\":\"rsp\",\"value\":0}",
        "{\"action\":\"set_register\",\"register\":\"eax\",\"value\":0}",
        "{\"action\":\"set_register\",\"register\":\"rip\",\"value\":0}",
        "{\"action\":\"set_register\",\"register\":7,\"value\":0}",
        "{\"action\":\"set_register\",\"register\":\"rax\",\"value\":-1}",
        "{\"action\":\"set_register\",\"register\":\"rax\",\"value\":\"0xZZ\"}",
        "{\"action\":\"set_register\",\"register\":\"rax\",\"value\":\"\"}",
        "{\"action\":\"set_register\",\"register\":\"rax\",\"value\":0,"
            "\"then\":\"maybe\"}",
        "{\"action\":\"set_rip\"}",                   /* no value           */
        "{\"action\":\"set_rip\",\"value\":0}",       /* outside the image  */
        "{\"action\":\"set_rip\",\"value\":\"0x1000000000\"}",
        "{\"action\":\"set_rip\",\"value\":\"0xFFFFFFFFFFFFFFFF\"}",
        "{\"action\":\"skip\",\"uses\":0}",
        "{\"action\":\"skip\",\"uses\":17}",
        "{\"action\":\"skip\",\"uses\":-1}",
        "{\"action\":\"skip\",\"uses\":\"lots\"}",
        "{\"action\":\"skip\",\"vector\":256}",
        "{\"action\":\"skip\",\"vector\":-1}",
        "{\"action\":\"skip\",\"vector\":8}",         /* #DF is never OK    */
        "{\"action\":\"skip\",\"vector\":18}",        /* #MC likewise       */
        "{\"action\":\"none\",\"uses\":2}",           /* none takes nothing */
    };

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fault_reset();
        publish_window();
        fault_plan_t sentinel = { .action = FAULT_ACT_REFUSE,
                                  .any_vector = 1, .uses = 1 };
        CHECK_EQ(fault_plan_arm(&sentinel), FAULT_FIX_OK);

        int rc = call_tool("fault_recover", bad[i], buf, sizeof buf, &r);
        CHECK_EQ(rc, TOOL_OK);              /* a bad argument is not a crash */
        CHECK_EQ(r.is_error, 1);
        CHECK_CONTAINS(buf, "refused");
        /* Nothing was armed and the previous plan was left exactly as it was. */
        CHECK(fault_plan_get() != NULL);
        CHECK_EQ(fault_plan_get()->action, FAULT_ACT_REFUSE);
    }
    fault_reset();
}

/* A 10,000-character argument, embedded NULs, and deep nesting: none of it may
 * corrupt anything, and all of it must come back as a refusal. */
static void test_tool_recover_hostile_input(void) {
    fault_reset();
    publish_window();

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;

    static char big[12000];
    memcpy(big, "{\"action\":\"", 11);
    memset(big + 11, 'a', sizeof big - 20);
    memcpy(big + sizeof big - 9, "\",\"x\":1}", 9);
    CHECK_EQ(call_tool("fault_recover", big, buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK(fault_plan_get() == NULL);

    /* An embedded NUL in the raw span: the input is NOT NUL-terminated, so the
     * tool must respect input_len and json.h must reject the escape. */
    static const char nul_input[] = "{\"action\":\"sk\0ip\"}";
    const tool_t *t = tool_find("fault_recover");
    CHECK(t != NULL);
    tool_result_init(&r, buf, sizeof buf);
    tool_call_t c = { .id = "toolu_x", .name = "fault_recover",
                      .input = nul_input, .input_len = sizeof nul_input - 1 };
    CHECK_EQ(t->invoke(&c, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK(fault_plan_get() == NULL);

    /* Deep nesting. */
    static char deep[4096];
    size_t k = 0;
    memcpy(deep + k, "{\"action\":", 10); k += 10;
    for (int i = 0; i < 300 && k < sizeof deep - 400; i++) deep[k++] = '[';
    deep[k++] = '1';
    for (int i = 0; i < 300 && k < sizeof deep - 8; i++) deep[k++] = ']';
    deep[k++] = '}';
    deep[k] = '\0';
    CHECK_EQ(call_tool("fault_recover", deep, buf, sizeof buf, &r), TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK(fault_plan_get() == NULL);
}

/* trace.h's ground-truth line is the only proof the operator gets that a tool
 * ran at all. Every path through every handler here must emit exactly one —
 * not zero on a refusal, and not two when a helper also logs. */
static void test_exactly_one_trace_line_per_call(void) {
    static const char *calls[][2] = {
        { "fault_status",  "{}"                                            },
        { "fault_history", "{}"                                            },
        { "fault_report",  "{}"                                            },
        { "fault_report",  "{\"seq\":1}"                                   },
        { "fault_report",  "{\"seq\":9999}"                                },
        { "fault_report",  "{\"seq\":\"x\"}"                               },
        { "fault_decode",  "{\"vector\":14}"                               },
        { "fault_decode",  "{}"                                            },
        { "fault_recover", "{\"action\":\"skip\"}"                         },
        { "fault_recover", "{\"action\":\"none\"}"                         },
        { "fault_recover", "{\"action\":\"nope\"}"                         },
        { "fault_recover", "{\"action\":\"set_rip\",\"value\":1}"          },
        { "fault_recover", "not json"                                      },
    };

    fault_reset();
    publish_window();
    fault_frame_t f;
    fault_plan_t p = { .action = FAULT_ACT_SKIP, .any_vector = 1, .uses = 1 };
    fault_plan_arm(&p);
    take_fault(&f, 14, win.image_lo);

    char buf[TOOL_RESULT_MAX];
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        trace_reset();
        kcap_reset();
        tool_result_t r;
        tool_result_init(&r, buf, sizeof buf);
        tool_call_t c = { .id = "toolu_t", .name = calls[i][0],
                          .input = calls[i][1],
                          .input_len = strlen(calls[i][1]) };
        CHECK_EQ(tool_dispatch(&c, &r), TOOL_OK);
        CHECK_EQ(trace_count(), 1);
        CHECK_CONTAINS(kcap_text(), " -> ");
    }
}

static void test_tool_status_shows_the_plan(void) {
    fault_reset();
    publish_window();

    char buf[TOOL_RESULT_MAX];
    tool_result_t r;
    CHECK_EQ(call_tool("fault_status", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_CONTAINS(buf, "none armed");
    CHECK_CONTAINS(buf, "resume window");

    call_tool("fault_recover",
              "{\"action\":\"set_register\",\"register\":\"rax\","
              "\"value\":\"0x10\",\"uses\":3}", buf, sizeof buf, &r);
    CHECK_EQ(call_tool("fault_status", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_CONTAINS(buf, "set rax = 0x10");
    CHECK_CONTAINS(buf, "re-execute");
    CHECK_CONTAINS(buf, "uses left     : 3");

    /* After a recovery the status names the outcome, not just "fatal". */
    fault_frame_t f;
    take_fault(&f, 14, win.image_lo);
    CHECK_EQ(call_tool("fault_status", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_CONTAINS(buf, "recovered");
    CHECK_CONTAINS(buf, "outcome");

    /* And a repeating fault is called out loudly. */
    take_fault(&f, 14, win.image_lo);
    CHECK_EQ(call_tool("fault_status", "{}", buf, sizeof buf, &r), TOOL_OK);
    CHECK_CONTAINS(buf, "REPEATING");
}

int main(void) {
    tools_bind();
    console_init();

    RUN(test_vector_names);
    RUN(test_error_code_vectors);
    RUN(test_fatality);

    RUN(test_pf_decode_bits);
    RUN(test_selector_decode);
    RUN(test_nonselector_decode);
    RUN(test_decode_truncation);
    RUN(test_rflags_decode);

    RUN(test_backtrace_walks_a_real_chain);
    RUN(test_backtrace_respects_max);
    RUN(test_backtrace_survives_corruption);
    RUN(test_backtrace_scan);

    RUN(test_capture_copies_the_frame);
    RUN(test_capture_reads_instruction_bytes);
    RUN(test_capture_falls_back_to_a_scan);
    RUN(test_capture_is_defensive);

    RUN(test_report_contains_everything_that_matters);
    RUN(test_report_labels_a_scanned_backtrace);
    RUN(test_report_says_when_rip_is_unreadable);
    RUN(test_report_handles_no_fault_and_truncation);
    RUN(test_report_for_every_vector);

    RUN(test_live_record);

    RUN(test_insn_length_the_injector_forms);
    RUN(test_insn_length_common_encodings);
    RUN(test_insn_length_addressing_forms);
    RUN(test_insn_length_refuses_what_it_cannot_know);
    RUN(test_insn_length_fuzz);

    RUN(test_register_mapping);

    RUN(test_apply_skip);
    RUN(test_apply_set_register);
    RUN(test_apply_refusal_is_atomic);
    RUN(test_apply_set_rip_and_the_window);
    RUN(test_apply_refusals);
    RUN(test_plan_check);
    RUN(test_apply_plan_fuzz);

    RUN(test_live_recovery_resumes);
    RUN(test_live_trap_still_resumes_untouched);
    RUN(test_refault_counter_stops_a_loop);
    RUN(test_plan_uses_are_consumed);
    RUN(test_plan_arm_validates_against_the_live_window);
    RUN(test_report_budget_with_a_disposition);

    RUN(test_ring_retains_and_wraps);
    RUN(test_ring_survives_a_fault_during_a_report);

    RUN(test_tool_registration);
    RUN(test_tool_status_with_no_fault);
    RUN(test_tool_status_after_a_fault);
    RUN(test_tool_report_is_the_same_bytes);
    RUN(test_report_fits_the_tool_budget);
    RUN(test_tool_decode_arguments);
    RUN(test_tool_decode_rejects_bad_input);
    RUN(test_tool_output_bounds);
    RUN(test_tool_report_by_seq);
    RUN(test_tool_history);
    RUN(test_tool_recover_arms_a_plan);
    RUN(test_tool_recover_rejects_bad_input);
    RUN(test_tool_recover_hostile_input);
    RUN(test_exactly_one_trace_line_per_call);
    RUN(test_tool_status_shows_the_plan);

    RUN(test_advertised_schema);
    RUN(test_dispatch_and_unknown_name);
    RUN(test_fuzz);

    return th_report("fault");
}
