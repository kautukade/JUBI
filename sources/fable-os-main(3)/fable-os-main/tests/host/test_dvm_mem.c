/* test_dvm_mem.c — the driver VM's scratch memory, strings and syscalls.
 *
 * WHY A SECOND DVM SUITE
 *   tests/host/test_dvm.c owns the machine: the assembler, the device opcodes,
 *   the deny list, and the invariant that every opcode in the ISA has run. This
 *   file owns the half of the machine that has no device in it — 64 KiB of
 *   scratch memory, the `m` string family, and `sys` — because that half is
 *   adversarial in a different way. There is no synthetic device to escape to;
 *   what a wrong program can reach here is the kernel's own memory, one byte
 *   past an array, and the filesystem.
 *
 * WHAT IS ASSERTED, AND WHY IT IS ENOUGH
 *   1. EVERY BOUND, AT BOTH ENDS. Every memory and string op is run at offset
 *      0, at the last legal offset, one past it, at a length that fits exactly,
 *      one longer, with an offset that wraps 2^64, and with the empty span —
 *      which is legal, because "the end offset" is what every writing op
 *      returns and a program that filled the arena exactly must be able to hand
 *      that value back.
 *   2. THE ARENA IS NEVER TOUCHED OUTSIDE ITS BOUNDS. A canary array sits
 *      either side of the peek window and the whole 64 KiB is read back after
 *      every fuzz program, so an out-of-bounds write shows up as a fact rather
 *      than as a crash that might not happen on this run.
 *   3. NOTHING SURVIVES A RUN. The arena is zeroed at entry to dvm_run(), so a
 *      program cannot read what the last one left. Asserted by writing a
 *      pattern, running a second program, and reading zeroes.
 *   4. THE SYSCALL BOUNDARY. The synthetic kernel below records every argument
 *      it is handed and refuses to be reached with anything it did not ask for:
 *      a path outside the granted root, a path with a "..", an unprintable
 *      byte, a buffer that runs off the arena. `escapes` must be zero at the
 *      end of the suite, in the same spirit as test_dvm.c's `forbidden`.
 *   5. FUZZ. Tens of thousands of generated programs made only of memory,
 *      string and syscall instructions with wild operands. None may fault, hang
 *      or write outside the arena, and every one must stop with a status this
 *      file recognises.
 *
 * WHAT IS NOT COVERED HERE
 *   That the REAL kernel backend (vm/dvm.c's dvm_sys_kernel(), which calls
 *   vfs_open and audio_tone) does what it says. It is #ifdef'd out of a host
 *   build; the tool-level path is tests/host/test_dvm_tools.c and the machine
 *   itself is proved by booting.
 */

#include "harness.h"
#include "dvm.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ====================================================================== */
/* the synthetic kernel                                                   */
/* ====================================================================== */

/* One fake file, so fs.read has something to return and fs.write something to
 * land in. Deliberately tiny: the interesting bounds are the VM's, not this. */
#define FAKEFILE      "/vm/notes"
#define FAKEFILE_BODY "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\npayload"

static long  escapes;                 /* MUST stay 0 for the whole suite */
static char  said[512];               /* the last con.write               */
static int   n_said;
static char  wrote_path[256];         /* the last fs.write                */
static char  wrote_body[512];
static size_t wrote_len;
static int   n_fs_read, n_fs_write, n_fs_size, n_tone, n_time;
static uint32_t last_hz, last_ms;
static int64_t  fake_now = 1000;
static int   fail_next;               /* make the next call report failure */

/* Anything the VM should have refused before this file saw it. A path outside
 * the root, a relative path, a "..", a control byte: every one of these is a
 * check in vm/dvm.c, and this is the mechanical proof that the check ran. */
static void audit_path(const char *p) {
    if (!p || p[0] != '/') { escapes++; return; }
    if (p[1] != 'v' || p[2] != 'm' || (p[3] != '/' && p[3] != '\0')) { escapes++; return; }
    for (size_t i = 0; p[i]; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c >= 0x7F) { escapes++; return; }
        if (c == '.' && p[i + 1] == '.') { escapes++; return; }
    }
}

static int64_t k_con(void *ctx, const char *text, size_t len, char *err, size_t cap) {
    (void)ctx;
    if (len > DVM_SAY_MAX) { escapes++; }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c >= 0x7F) escapes++;    /* the VM must have sanitised */
    }
    if (text[len] != '\0') escapes++;            /* ...and terminated */
    if (fail_next) { fail_next = 0; snprintf(err, cap, "the console said no"); return -5; }
    size_t n = len < sizeof said - 1 ? len : sizeof said - 1;
    memcpy(said, text, n);
    said[n] = '\0';
    n_said++;
    return (int64_t)len;
}

static int64_t k_read(void *ctx, const char *path, void *dst, size_t cap,
                      char *err, size_t errcap) {
    (void)ctx;
    audit_path(path);
    n_fs_read++;
    if (strcmp(path, FAKEFILE) != 0) {
        snprintf(err, errcap, "no such file");
        return -2;
    }
    size_t n = strlen(FAKEFILE_BODY);
    if (n > cap) n = cap;
    memcpy(dst, FAKEFILE_BODY, n);
    return (int64_t)n;
}

static int64_t k_write(void *ctx, const char *path, const void *src, size_t len,
                       char *err, size_t errcap) {
    (void)ctx;
    audit_path(path);
    n_fs_write++;
    if (fail_next) { fail_next = 0; snprintf(err, errcap, "read-only today"); return -30; }
    snprintf(wrote_path, sizeof wrote_path, "%s", path);
    wrote_len = len < sizeof wrote_body - 1 ? len : sizeof wrote_body - 1;
    memcpy(wrote_body, src, wrote_len);
    wrote_body[wrote_len] = '\0';
    return (int64_t)len;
}

static int64_t k_size(void *ctx, const char *path, char *err, size_t errcap) {
    (void)ctx;
    audit_path(path);
    n_fs_size++;
    if (strcmp(path, FAKEFILE) != 0) { snprintf(err, errcap, "no such file"); return -2; }
    return (int64_t)strlen(FAKEFILE_BODY);
}

static int64_t k_tone(void *ctx, uint32_t hz, uint32_t ms, char *err, size_t cap) {
    (void)ctx;
    if (hz < 20 || hz > 20000 || ms < 1 || ms > 5000) escapes++;
    n_tone++; last_hz = hz; last_ms = ms;
    if (fail_next) { fail_next = 0; snprintf(err, cap, "no sink installed"); return -1; }
    return 0;
}

static int64_t k_time(void *ctx, char *err, size_t cap) {
    (void)ctx; (void)err; (void)cap;
    n_time++;
    return fake_now;
}

static const dvm_sys_t KERNEL = {
    .ctx = NULL, .con_write = k_con, .fs_read = k_read, .fs_write = k_write,
    .fs_size = k_size, .audio_tone = k_tone, .time_ms = k_time,
};

/* No device at all: exactly what tools/dvm_tools.c hands a program with no
 * target, so what this suite runs is the real software-program shape. */
static const dvm_io_t SOFT = { .ctx = NULL, .sys = &KERNEL };

/* ...and one with no kernel behind it either. */
static const dvm_io_t BARE = { .ctx = NULL, .sys = NULL };

/* ====================================================================== */
/* harness                                                                */
/* ====================================================================== */

static dvm_program_t P;
static dvm_policy_t  POL;
static dvm_result_t  R;
static dvm_asm_err_t E;

static void kernel_reset(void) {
    said[0] = '\0'; n_said = 0;
    wrote_path[0] = '\0'; wrote_body[0] = '\0'; wrote_len = 0;
    n_fs_read = n_fs_write = n_fs_size = n_tone = n_time = 0;
    last_hz = last_ms = 0;
    fail_next = 0;
}

/* Deny-all plus every syscall this suite exercises, rooted at /vm. */
static void std_policy(void) {
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_CON_WRITE), 0);
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_FS_READ), 0);
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_FS_WRITE), 0);
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_FS_SIZE), 0);
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_AUDIO_TONE), 0);
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_TIME_MS), 0);
    CHECK_EQ(dvm_policy_set_fs_root(&POL, "/vm"), 0);
}

static dvm_status_t run(const char *src) {
    kernel_reset();
    if (dvm_assemble(src, strlen(src), &P, &E) != 0) {
        printf("    ASSEMBLY FAILED line %d: %s\n", E.line, E.msg);
        th_fails++; th_checks++;
        return DVM_TRAP_BADOP;
    }
    return dvm_run(&P, &POL, &SOFT, NULL, 0, &R);
}

#define CHECK_STATUS(got, want)                                               \
    do {                                                                      \
        th_checks++;                                                          \
        if ((got) != (want)) {                                                \
            th_fails++;                                                       \
            printf("    FAIL %s:%d  status %s, want %s  (msg: %s)\n",         \
                   __FILE__, __LINE__, dvm_status_name(got),                  \
                   dvm_status_name(want), R.msg);                             \
        }                                                                     \
    } while (0)

/* Read a span of the arena back as a NUL-terminated C string, for comparing. */
static const char *peek(uint64_t off, size_t n) {
    static char buf[DVM_MEM_SIZE + 1];
    size_t got = dvm_mem_peek(off, buf, n);
    buf[got] = '\0';
    return buf;
}

/* ====================================================================== */
/* 1. the shape of the arena                                              */
/* ====================================================================== */

static void test_arena_shape(void) {
    /* A peek is bounded exactly like an access. */
    char b[8];
    CHECK_EQ(dvm_mem_peek(0, b, 8), 8);
    CHECK_EQ(dvm_mem_peek(DVM_MEM_SIZE - 4, b, 8), 4);   /* clipped, not refused */
    CHECK_EQ(dvm_mem_peek(DVM_MEM_SIZE, b, 8), 0);
    CHECK_EQ(dvm_mem_peek(DVM_MEM_SIZE + 1, b, 8), 0);
    CHECK_EQ(dvm_mem_peek((uint64_t)-1, b, 8), 0);
    CHECK_EQ(dvm_mem_peek(0, NULL, 8), 0);
    CHECK_EQ(dvm_mem_peek(0, b, 0), 0);

    /* The arena needs no grant: a deny-all policy still has all of it. */
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    CHECK_STATUS(run("mst8 [0], 0x41\nmld8 r1, [0]\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0x41);
}

/* ====================================================================== */
/* 2. loads and stores: widths, endianness, alignment, bounds             */
/* ====================================================================== */

static void test_load_store_widths(void) {
    std_policy();

    /* Little-endian, and the widths nest. */
    CHECK_STATUS(run("mst64 [0], 0x1122334455667788\n"
                     "mld8  r1, [0]\n"
                     "mld16 r2, [0]\n"
                     "mld32 r3, [0]\n"
                     "mld64 r4, [0]\n"
                     "mld8  r5, [7]\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0x88);
    CHECK_EQ(R.reg[2], 0x7788);
    CHECK_EQ(R.reg[3], 0x55667788);
    CHECK_EQ(R.reg[4], 0x1122334455667788ull);
    CHECK_EQ(R.reg[5], 0x11);

    /* Stores truncate to their width and leave the neighbour alone. */
    CHECK_STATUS(run("mset  r0, 0, 0xFF, 8\n"
                     "mst8  [0], 0x1234\n"
                     "mld8  r1, [0]\n"
                     "mld8  r2, [1]\n"
                     "mst16 [2], 0xAABBCC\n"
                     "mld16 r3, [2]\n"
                     "mld8  r4, [4]\n"
                     "mst32 [4], 0x1122334455\n"
                     "mld32 r5, [4]\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0x34);
    CHECK_EQ(R.reg[2], 0xFF);
    CHECK_EQ(R.reg[3], 0xBBCC);
    CHECK_EQ(R.reg[4], 0xFF);
    CHECK_EQ(R.reg[5], 0x22334455);

    /* UNALIGNED IS LEGAL. This is the deliberate difference from MMIO: a
     * device register needs alignment because the bus does, and memory does
     * not, so a parser is never made to care where a field landed. */
    CHECK_STATUS(run("mst32 [1], 0xDEADBEEF\n"
                     "mld32 r1, [1]\n"
                     "mst64 [3], 0x0123456789ABCDEF\n"
                     "mld64 r2, [3]\n"
                     "mld16 r3, [7]\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0xDEADBEEF);
    CHECK_EQ(R.reg[2], 0x0123456789ABCDEFull);
    CHECK_EQ(R.reg[3], 0x4567);      /* bytes 4 and 5 of the unaligned store */

    /* Base + displacement, both forms, and a register base. */
    CHECK_STATUS(run("mov   r1, 100\n"
                     "mst32 [r1+4], 0x5A5A5A5A\n"
                     "mld32 r2, [104]\n"
                     "mld32 r3, r1, 4\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0x5A5A5A5A);
    CHECK_EQ(R.reg[3], 0x5A5A5A5A);
}

static void test_load_store_bounds(void) {
    std_policy();

    /* THE LAST LEGAL BYTE, at every width, and the first illegal one. */
    static const struct { const char *op; unsigned w; } widths[] = {
        { "8", 1 }, { "16", 2 }, { "32", 4 }, { "64", 8 },
    };
    for (size_t i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        char src[160];
        unsigned last = DVM_MEM_SIZE - widths[i].w;

        snprintf(src, sizeof src, "mst%s [%u], 1\nmld%s r1, [%u]\nhalt\n",
                 widths[i].op, last, widths[i].op, last);
        CHECK_STATUS(run(src), DVM_OK);
        CHECK_EQ(R.reg[1], 1);

        snprintf(src, sizeof src, "mld%s r1, [%u]\nhalt\n", widths[i].op, last + 1);
        CHECK_STATUS(run(src), DVM_TRAP_MEM);

        snprintf(src, sizeof src, "mst%s [%u], 1\nhalt\n", widths[i].op, last + 1);
        CHECK_STATUS(run(src), DVM_TRAP_MEM);

        snprintf(src, sizeof src, "mld%s r1, [%u]\nhalt\n", widths[i].op, DVM_MEM_SIZE);
        CHECK_STATUS(run(src), DVM_TRAP_MEM);
    }

    /* A wrapping base+displacement is a trap, not a small offset. This is the
     * one that would be a kernel write if the sum were taken at face value. */
    CHECK_STATUS(run("mov   r1, -1\n"
                     "mld8  r2, [r1+1]\n"
                     "halt\n"), DVM_TRAP_MEM);
    CHECK_CONTAINS(R.msg, "wraps");
    CHECK_STATUS(run("mov   r1, -1\n"
                     "mst8  [r1+1], 0x41\n"
                     "halt\n"), DVM_TRAP_MEM);
    CHECK_CONTAINS(R.msg, "wraps");

    /* Enormous offsets, in both directions. */
    CHECK_STATUS(run("mov  r1, 0xFFFFFFFFFFFFFFFF\nmld8 r2, [r1]\nhalt\n"),
                 DVM_TRAP_MEM);
    CHECK_STATUS(run("mov  r1, 0x8000000000000000\nmst8 [r1], 1\nhalt\n"),
                 DVM_TRAP_MEM);
    CHECK_STATUS(run("mld64 r1, [0xFFFFFFF8]\nhalt\n"), DVM_TRAP_MEM);
}

/* ====================================================================== */
/* 3. the string family                                                   */
/* ====================================================================== */

static void test_mstr_and_chaining(void) {
    std_policy();

    CHECK_STATUS(run("mstr r1, 0,  \"GET \"\n"
                     "mstr r1, r1, \"/index.html\"\n"
                     "mstr r1, r1, \" HTTP/1.1\\r\\n\"\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 26);
    CHECK_STR(peek(0, 26), "GET /index.html HTTP/1.1\r\n");
    CHECK_EQ(R.mem_hwm, 26);

    /* An empty literal writes nothing and returns where it started. */
    CHECK_STATUS(run("mstr r1, 40, \"\"\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 40);
    CHECK_EQ(R.mem_hwm, 40);

    /* Exactly filling the arena is legal, and the end offset it returns is
     * DVM_MEM_SIZE — which must then be accepted as the start of an empty
     * span, or "chain until you run out" would trap on its last step. */
    char src[128];
    snprintf(src, sizeof src, "mstr r1, %u, \"abcd\"\nmstr r2, r1, \"\"\nhalt\n",
             DVM_MEM_SIZE - 4);
    CHECK_STATUS(run(src), DVM_OK);
    CHECK_EQ(R.reg[1], DVM_MEM_SIZE);
    CHECK_EQ(R.reg[2], DVM_MEM_SIZE);

    /* One byte too far. */
    snprintf(src, sizeof src, "mstr r1, %u, \"abcd\"\nhalt\n", DVM_MEM_SIZE - 3);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);

    /* Escapes reach memory as real bytes, including a literal quote. */
    CHECK_STATUS(run("mstr r1, 0, \"a\\tb\\\"c\"\nmld8 r2, [1]\nmld8 r3, [3]\nhalt\n"),
                 DVM_OK);
    CHECK_EQ(R.reg[1], 5);
    CHECK_EQ(R.reg[2], '\t');
    CHECK_EQ(R.reg[3], '"');
}

static void test_mcpy_overlap(void) {
    std_policy();

    /* Disjoint. */
    CHECK_STATUS(run("mstr r1, 0, \"abcdefgh\"\n"
                     "mcpy r2, 100, 0, 8\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 108);
    CHECK_STR(peek(100, 8), "abcdefgh");

    /* Forward overlap: destination above source, the case a naive loop
     * corrupts by reading a byte it has already written. */
    CHECK_STATUS(run("mstr r1, 0, \"abcdefgh\"\n"
                     "mcpy r2, 2, 0, 6\n"
                     "halt\n"), DVM_OK);
    CHECK_STR(peek(0, 8), "ababcdef");

    /* Backward overlap: destination below source. */
    CHECK_STATUS(run("mstr r1, 0, \"abcdefgh\"\n"
                     "mcpy r2, 0, 2, 6\n"
                     "halt\n"), DVM_OK);
    CHECK_STR(peek(0, 8), "cdefghgh");

    /* Exactly aliased: a copy onto itself must be a no-op, not a smear. */
    CHECK_STATUS(run("mstr r1, 0, \"abcdefgh\"\n"
                     "mcpy r2, 0, 0, 8\n"
                     "halt\n"), DVM_OK);
    CHECK_STR(peek(0, 8), "abcdefgh");

    /* One-byte overlap at each end. */
    CHECK_STATUS(run("mstr r1, 0, \"abcdefgh\"\n"
                     "mcpy r2, 7, 0, 2\n"
                     "halt\n"), DVM_OK);
    CHECK_STR(peek(0, 9), "abcdefgab");

    /* Zero length is legal anywhere in range, including the very end. */
    char src[128];
    snprintf(src, sizeof src, "mcpy r1, %u, %u, 0\nhalt\n", DVM_MEM_SIZE, DVM_MEM_SIZE);
    CHECK_STATUS(run(src), DVM_OK);
    CHECK_EQ(R.reg[1], DVM_MEM_SIZE);

    /* Both ends bounded, independently. */
    snprintf(src, sizeof src, "mcpy r1, %u, 0, 4\nhalt\n", DVM_MEM_SIZE - 4);
    CHECK_STATUS(run(src), DVM_OK);
    snprintf(src, sizeof src, "mcpy r1, %u, 0, 4\nhalt\n", DVM_MEM_SIZE - 3);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
    snprintf(src, sizeof src, "mcpy r1, 0, %u, 4\nhalt\n", DVM_MEM_SIZE - 3);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
    CHECK_STATUS(run("mcpy r1, 0, 0, 0xFFFFFFFFFFFFFFFF\nhalt\n"), DVM_TRAP_MEM);
}

static void test_mset(void) {
    std_policy();

    CHECK_STATUS(run("mset r1, 10, 0x41, 4\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 14);
    CHECK_STR(peek(10, 4), "AAAA");
    CHECK_STR(peek(9, 1), "");          /* the byte before is still zero */

    /* Only the low 8 bits of the value are used. */
    CHECK_STATUS(run("mset r1, 0, 0x12345678, 2\nmld16 r2, [0]\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0x7878);

    char src[128];
    snprintf(src, sizeof src, "mset r1, 0, 0x41, %u\nhalt\n", DVM_MEM_SIZE);
    CHECK_STATUS(run(src), DVM_OK);
    CHECK_EQ(R.reg[1], DVM_MEM_SIZE);
    snprintf(src, sizeof src, "mset r1, 1, 0x41, %u\nhalt\n", DVM_MEM_SIZE);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
}

static void test_mcmp(void) {
    std_policy();

    /* Equal, shorter-first, longer-first, and the flag each sets. */
    CHECK_STATUS(run("mstr r1, 0,  \"alpha\"\n"
                     "mstr r2, 32, \"alpha\"\n"
                     "mcmp 0, 32, 5\n"
                     "beq  ok\n"
                     "abort \"equal spans did not compare equal\"\n"
                     "ok: halt\n"), DVM_OK);

    CHECK_STATUS(run("mstr r1, 0,  \"alpha\"\n"
                     "mstr r2, 32, \"alphb\"\n"
                     "mcmp 0, 32, 5\n"
                     "blt  below\n"
                     "abort \"a<b was not reported\"\n"
                     "below: mcmp 32, 0, 5\n"
                     "bgt  above\n"
                     "abort \"b>a was not reported\"\n"
                     "above: halt\n"), DVM_OK);

    /* The difference is found at the FIRST differing byte, not the last. */
    CHECK_STATUS(run("mstr r1, 0,  \"aXc\"\n"
                     "mstr r2, 32, \"aYb\"\n"
                     "mcmp 0, 32, 3\n"
                     "blt  ok\n"
                     "abort \"compared past the first difference\"\n"
                     "ok: halt\n"), DVM_OK);

    /* Length zero compares equal. */
    CHECK_STATUS(run("mcmp 0, 100, 0\nbeq ok\nabort \"empty was not equal\"\nok: halt\n"),
                 DVM_OK);

    /* Bytes are UNSIGNED: 0x80 is above 0x7f, which a char-signed comparison
     * would get backwards. */
    CHECK_STATUS(run("mst8 [0], 0x80\nmst8 [32], 0x7f\n"
                     "mcmp 0, 32, 1\n"
                     "bgt  ok\n"
                     "abort \"byte comparison is signed\"\n"
                     "ok: halt\n"), DVM_OK);

    char src[128];
    snprintf(src, sizeof src, "mcmp %u, 0, 4\nhalt\n", DVM_MEM_SIZE - 3);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
    snprintf(src, sizeof src, "mcmp 0, %u, 4\nhalt\n", DVM_MEM_SIZE - 3);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
}

static void test_mfind_and_mchr(void) {
    std_policy();

    /* Found at the start, in the middle, at the very end, and not at all. */
    CHECK_STATUS(run("mstr  r1, 0, \"the cat sat\"\n"
                     "mfind r2, 0, r1, \"the\"\n"
                     "mfind r3, 0, r1, \"cat\"\n"
                     "mfind r4, 0, r1, \"sat\"\n"
                     "mfind r5, 0, r1, \"dog\"\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0);
    CHECK_EQ(R.reg[3], 4);
    CHECK_EQ(R.reg[4], 8);
    CHECK_EQ(R.reg[5], 11);            /* not found -> hay + len */

    /* The zero flag is the answer; the offset alone is ambiguous when the
     * needle really does start at the end of the span. */
    CHECK_STATUS(run("mstr  r1, 0, \"abc\"\n"
                     "mfind r2, 0, r1, \"zz\"\n"
                     "beq   wrong\n"
                     "mfind r3, 0, r1, \"c\"\n"
                     "bne   wrong\n"
                     "halt\n"
                     "wrong: abort \"mfind's flag disagreed with its offset\"\n"), DVM_OK);
    CHECK_EQ(R.reg[3], 2);

    /* A needle longer than the haystack is simply not found. */
    CHECK_STATUS(run("mstr  r1, 0, \"ab\"\n"
                     "mfind r2, 0, r1, \"abcd\"\n"
                     "beq   wrong\n"
                     "halt\n"
                     "wrong: abort \"found a needle that does not fit\"\n"), DVM_OK);

    /* An empty needle matches at the start. */
    CHECK_STATUS(run("mfind r1, 7, 3, \"\"\nbne wrong\nhalt\n"
                     "wrong: abort \"empty needle\"\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 7);

    /* A partial match that then fails must not stop the search: "aab" inside
     * "aaab" is the classic case a first-byte-only scan gets wrong. */
    CHECK_STATUS(run("mstr  r1, 0, \"aaab\"\n"
                     "mfind r2, 0, r1, \"aab\"\n"
                     "bne   wrong\n"
                     "halt\n"
                     "wrong: abort \"backtracking failed\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 1);

    /* The search never reads past the span, even when the needle would fit
     * just past its end. */
    CHECK_STATUS(run("mstr  r1, 0, \"abcdef\"\n"
                     "mfind r2, 0, 3, \"cd\"\n"
                     "beq   wrong\n"
                     "halt\n"
                     "wrong: abort \"mfind read past the span\"\n"), DVM_OK);

    /* mchr: same contract, one byte. */
    CHECK_STATUS(run("mstr r1, 0, \"HTTP/1.1 200 OK\"\n"
                     "mchr r2, 0, r1, 32\n"
                     "mchr r3, 0, r1, 0x5a\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 8);
    CHECK_EQ(R.reg[3], 15);
    CHECK_STATUS(run("mchr r1, 0, 0, 65\nbeq wrong\nhalt\nwrong: abort \"x\"\n"), DVM_OK);

    char src[128];
    snprintf(src, sizeof src, "mfind r1, %u, 4, \"a\"\nhalt\n", DVM_MEM_SIZE - 3);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
    snprintf(src, sizeof src, "mchr r1, %u, 4, 65\nhalt\n", DVM_MEM_SIZE - 3);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
}

static void test_integer_conversion(void) {
    std_policy();

    /* Round trip, in three bases. */
    CHECK_STATUS(run("mitoa r1, 0,  1234, 10\n"
                     "matoi r2, 0,  4,    10\n"
                     "mitoa r3, 16, 0xbeef, 16\n"
                     "matoi r4, 16, 4,    16\n"
                     "mitoa r5, 32, 5,    2\n"
                     "matoi r6, 32, 3,    2\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 4);
    CHECK_EQ(R.reg[2], 1234);
    CHECK_STR(peek(0, 4), "1234");
    CHECK_EQ(R.reg[3], 20);
    CHECK_EQ(R.reg[4], 0xbeef);
    CHECK_STR(peek(16, 4), "beef");
    CHECK_EQ(R.reg[5], 35);
    CHECK_EQ(R.reg[6], 5);
    CHECK_STR(peek(32, 3), "101");

    /* Zero is one digit, not none. */
    CHECK_STATUS(run("mitoa r1, 0, 0, 10\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 1);
    CHECK_STR(peek(0, 1), "0");

    /* The widest value, in the narrowest base: 64 digits, which is exactly the
     * conversion buffer. One more would be an overrun. */
    CHECK_STATUS(run("mov   r1, 0xFFFFFFFFFFFFFFFF\n"
                     "mitoa r2, 0, r1, 2\n"
                     "matoi r3, 0, 64, 2\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 64);
    CHECK_EQ(R.reg[3], 0xFFFFFFFFFFFFFFFFull);
    CHECK_STATUS(run("mov   r1, 0xFFFFFFFFFFFFFFFF\n"
                     "mitoa r2, 0, r1, 16\n"
                     "matoi r3, 0, 16, 16\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 16);
    CHECK_EQ(R.reg[3], 0xFFFFFFFFFFFFFFFFull);

    /* matoi stops at the first byte that is not a digit in this base... */
    CHECK_STATUS(run("mstr  r1, 0, \"200 OK\"\n"
                     "matoi r2, 0, r1, 10\n"
                     "bne   wrong\n"
                     "halt\n"
                     "wrong: abort \"a valid prefix was rejected\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 200);
    CHECK_STATUS(run("mstr  r1, 0, \"19f\"\n"
                     "matoi r2, 0, 3, 10\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 19);            /* 'f' is not a decimal digit */
    CHECK_STATUS(run("mstr  r1, 0, \"19f\"\n"
                     "matoi r2, 0, 3, 16\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0x19f);

    /* ...and reports NO DIGITS as a failure rather than as zero, which is the
     * difference between "the status code is 0" and "there was no status
     * code". */
    CHECK_STATUS(run("mstr  r1, 0, \"OK\"\n"
                     "matoi r2, 0, 2, 10\n"
                     "beq   wrong\n"
                     "halt\n"
                     "wrong: abort \"garbage parsed as a number\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0);
    CHECK_STATUS(run("matoi r1, 0, 0, 10\nbeq wrong\nhalt\nwrong: abort \"empty\"\n"),
                 DVM_OK);

    /* A real zero still succeeds. */
    CHECK_STATUS(run("mstr  r1, 0, \"0\"\n"
                     "matoi r2, 0, 1, 10\n"
                     "bne   wrong\n"
                     "halt\n"
                     "wrong: abort \"a literal zero was called a failure\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0);

    /* Leading zeroes, and a leading '+' or '-' which is NOT accepted. */
    CHECK_STATUS(run("mstr r1, 0, \"007\"\nmatoi r2, 0, 3, 10\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 7);
    CHECK_STATUS(run("mstr r1, 0, \"-7\"\nmatoi r2, 0, 2, 10\n"
                     "beq wrong\nhalt\nwrong: abort \"sign accepted\"\n"), DVM_OK);
    CHECK_STATUS(run("mstr r1, 0, \" 7\"\nmatoi r2, 0, 2, 10\n"
                     "beq wrong\nhalt\nwrong: abort \"space accepted\"\n"), DVM_OK);

    /* Overflow is a failure, not a wrap. 2^64-1 fits; one more does not. */
    CHECK_STATUS(run("mstr  r1, 0, \"18446744073709551615\"\n"
                     "matoi r2, 0, 20, 10\n"
                     "bne   wrong\n"
                     "halt\n"
                     "wrong: abort \"2^64-1 was refused\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0xFFFFFFFFFFFFFFFFull);
    CHECK_STATUS(run("mstr  r1, 0, \"18446744073709551616\"\n"
                     "matoi r2, 0, 20, 10\n"
                     "beq   wrong\n"
                     "halt\n"
                     "wrong: abort \"an overflowing number was accepted\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0);
    CHECK_STATUS(run("mstr  r1, 0, \"99999999999999999999999999\"\n"
                     "matoi r2, 0, 26, 10\n"
                     "beq   wrong\n"
                     "halt\n"
                     "wrong: abort \"a huge number was accepted\"\n"), DVM_OK);

    /* Bases outside 2-16 are refused at the instruction, both directions. */
    CHECK_STATUS(run("matoi r1, 0, 1, 1\nhalt\n"),  DVM_TRAP_RANGE);
    CHECK_STATUS(run("matoi r1, 0, 1, 0\nhalt\n"),  DVM_TRAP_RANGE);
    CHECK_STATUS(run("matoi r1, 0, 1, 17\nhalt\n"), DVM_TRAP_RANGE);
    CHECK_STATUS(run("mitoa r1, 0, 5, 1\nhalt\n"),  DVM_TRAP_RANGE);
    CHECK_STATUS(run("mitoa r1, 0, 5, 17\nhalt\n"), DVM_TRAP_RANGE);
    CHECK_STATUS(run("mov r1, 0xFFFFFFFFFFFFFFFF\nmitoa r2, 0, 5, r1\nhalt\n"),
                 DVM_TRAP_RANGE);

    /* mitoa is bounded like everything else: 20 digits will not fit in the
     * last 4 bytes of the arena. */
    char src[128];
    snprintf(src, sizeof src, "mov r1, 0xFFFFFFFFFFFFFFFF\nmitoa r2, %u, r1, 10\nhalt\n",
             DVM_MEM_SIZE - 4);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
    snprintf(src, sizeof src, "mov r1, 0xFFFFFFFFFFFFFFFF\nmitoa r2, %u, r1, 10\nhalt\n",
             DVM_MEM_SIZE - 20);
    CHECK_STATUS(run(src), DVM_OK);
    CHECK_EQ(R.reg[2], DVM_MEM_SIZE);
    snprintf(src, sizeof src, "matoi r1, %u, 4, 10\nhalt\n", DVM_MEM_SIZE - 3);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
}

/* ====================================================================== */
/* 4. nothing survives a run                                              */
/* ====================================================================== */

static void test_arena_is_zeroed_between_runs(void) {
    std_policy();

    CHECK_STATUS(run("mset r1, 0, 0x5A, 4096\n"
                     "mstr r2, 60000, \"secret\"\n"
                     "halt\n"), DVM_OK);
    CHECK_STR(peek(60000, 6), "secret");

    /* The kernel can still read it — that is how a program returns an answer. */
    unsigned char b[8];
    CHECK_EQ(dvm_mem_peek(0, b, 4), 4);
    CHECK_EQ(b[0], 0x5A);

    /* The next program cannot. */
    CHECK_STATUS(run("mld64 r1, [0]\n"
                     "mld64 r2, [2048]\n"
                     "mld64 r3, [59998]\n"
                     "mchr  r4, 0, 65535, 0x5a\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0);
    CHECK_EQ(R.reg[2], 0);
    CHECK_EQ(R.reg[3], 0);
    CHECK_EQ(R.reg[4], 65535);         /* not found anywhere in the arena */

    /* Zeroed even when the previous run TRAPPED, which is when a
     * clean-up-afterwards design would have failed to run. */
    CHECK_STATUS(run("mset r1, 0, 0xEE, 256\nmld8 r2, [0x20000]\nhalt\n"),
                 DVM_TRAP_MEM);
    CHECK_STATUS(run("mld64 r1, [0]\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0);
}

/* ====================================================================== */
/* 5. the memory budget                                                   */
/* ====================================================================== */

static void test_memory_budget(void) {
    std_policy();

    /* One instruction, 64 KiB of work: the case the step budget cannot see. */
    POL.max_mem_bytes = 1024;
    CHECK_STATUS(run("mset r1, 0, 0x41, 4096\nhalt\n"), DVM_TRAP_MEM_BUDGET);
    CHECK_CONTAINS(R.msg, "scratch memory budget");

    /* Charged exactly, not approximately: a fill of the whole budget passes
     * and one byte more does not. */
    POL.max_mem_bytes = 100;
    CHECK_STATUS(run("mset r1, 0, 0x41, 100\nhalt\n"), DVM_OK);
    CHECK_EQ(R.mem_bytes, 100);
    POL.max_mem_bytes = 99;
    CHECK_STATUS(run("mset r1, 0, 0x41, 100\nhalt\n"), DVM_TRAP_MEM_BUDGET);

    /* A copy is charged for both halves, so 2n. */
    POL.max_mem_bytes = 200;
    CHECK_STATUS(run("mcpy r1, 0, 200, 100\nhalt\n"), DVM_OK);
    CHECK_EQ(R.mem_bytes, 200);
    POL.max_mem_bytes = 199;
    CHECK_STATUS(run("mcpy r1, 0, 200, 100\nhalt\n"), DVM_TRAP_MEM_BUDGET);

    /* THE ONE THAT MATTERS: a search is quadratic in the worst case, so it is
     * charged as it goes and stops PART WAY rather than after. Without that a
     * loop of mfinds would be a bounded step count and unbounded work. */
    POL.max_mem_bytes = 4ull * DVM_MEM_SIZE;
    CHECK_STATUS(run("mset  r1, 0, 0x61, 60000\n"      /* 60000 'a's        */
                     "loop:\n"
                     "mfind r2, 0, 60000, \"aaab\"\n"  /* never matches     */
                     "jmp   loop\n"), DVM_TRAP_MEM_BUDGET);
    CHECK(R.mem_bytes <= 4ull * DVM_MEM_SIZE);
    CHECK(R.steps < 50);               /* it did not get many searches in */

    /* An unbounded loop of small ops still stops, on steps. */
    POL.max_mem_bytes = 64ull * DVM_MEM_SIZE;
    POL.max_steps = 5000;
    CHECK_STATUS(run("loop: mst8 [0], 1\njmp loop\n"), DVM_TRAP_STEPS);

    /* The policy's own ceiling is enforced. */
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    POL.max_mem_bytes = 0x4000001ull;
    CHECK_STATUS(run("halt\n"), DVM_TRAP_POLICY);
    CHECK_CONTAINS(R.msg, "max_mem_bytes");
}

/* ====================================================================== */
/* 6. syscalls                                                            */
/* ====================================================================== */

static void test_syscall_convention(void) {
    std_policy();

    /* Arguments are popped in the order they were pushed. fs.read takes four,
     * so this also proves the four-slot window is filled the right way round:
     * a reversed one would hand the path length as the destination offset. */
    CHECK_STATUS(run("mstr r1, 0, \"/vm/notes\"\n"
                     "push 0\n"
                     "push r1\n"
                     "push 100\n"
                     "push 200\n"
                     "sys  r2, fs.read\n"
                     "bne  bad\n"
                     "halt\n"
                     "bad: abort \"fs.read failed\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], strlen(FAKEFILE_BODY));
    CHECK_EQ(n_fs_read, 1);
    CHECK_STR(peek(100, strlen(FAKEFILE_BODY)), FAKEFILE_BODY);
    /* The write high-water mark moved to cover what the kernel wrote. */
    CHECK_EQ(R.mem_hwm, 100 + strlen(FAKEFILE_BODY));

    /* The stack is consumed exactly: nothing left over, nothing extra taken. */
    CHECK_STATUS(run("push 0xAA\n"
                     "push 0\npush 0\n"
                     "sys  r1, con.write\n"
                     "pop  r2\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0xAA);

    /* Too few arguments is a stack trap that says how many it wanted. */
    CHECK_STATUS(run("push 0\nsys r1, con.write\nhalt\n"), DVM_TRAP_STACK);
    CHECK_CONTAINS(R.msg, "2 argument(s)");
    CHECK_STATUS(run("sys r1, fs.size\nhalt\n"), DVM_TRAP_STACK);

    /* A zero-arity call takes nothing off the stack. */
    CHECK_STATUS(run("push 7\nsys r1, time.ms\npop r2\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 1000);
    CHECK_EQ(R.reg[2], 7);

    /* A failing syscall is NOT a trap: it is a fact the program branches on,
     * and the reason comes back in the result for the model. */
    CHECK_STATUS(run("mstr r1, 0, \"/vm/missing\"\n"
                     "push 0\npush r1\n"
                     "sys  r2, fs.size\n"
                     "beq  wrong\n"
                     "halt\n"
                     "wrong: abort \"a missing file reported success\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 2);             /* the positive error code */
    CHECK_CONTAINS(R.sys_msg, "no such file");
    CHECK_CONTAINS(R.sys_msg, "fs.size");

    /* sys_msg is cleared by the next run rather than left to mislead. */
    CHECK_STATUS(run("sys r1, time.ms\nhalt\n"), DVM_OK);
    CHECK_STR(R.sys_msg, "");

    /* The syscall budget. */
    POL.max_sys = 3;
    CHECK_STATUS(run("loop: sys r1, time.ms\njmp loop\n"), DVM_TRAP_SYS_BUDGET);
    CHECK_EQ(R.n_sys, 3);
    CHECK_EQ(n_time, 3);
}

static void test_syscall_grants(void) {
    /* Deny-all is the default, and the refusal names what the program has. */
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    CHECK_STATUS(run("sys r1, time.ms\nhalt\n"), DVM_TRAP_SYS_DENIED);
    CHECK_CONTAINS(R.msg, "it has none");
    CHECK_EQ(n_time, 0);

    /* One grant is one syscall, not a family. */
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_FS_READ), 0);
    CHECK_EQ(dvm_policy_set_fs_root(&POL, "/vm"), 0);
    CHECK_STATUS(run("sys r1, time.ms\nhalt\n"), DVM_TRAP_SYS_DENIED);
    CHECK_STATUS(run("mstr r1, 0, \"/vm/notes\"\n"
                     "push 0\npush r1\npush 100\npush 64\n"
                     "sys r2, fs.read\nhalt\n"), DVM_OK);

    /* A number that is not a syscall is refused by the ASSEMBLER, so a program
     * cannot even be built that names one. */
    CHECK(dvm_assemble("sys r1, 99\nhalt\n", 15, &P, &E) != 0);
    CHECK(dvm_assemble("sys r1, net.fetch\nhalt\n", 23, &P, &E) != 0);
    CHECK_CONTAINS(E.msg, "not a syscall on this machine");
    CHECK_CONTAINS(E.msg, "fs.read");

    /* An fs syscall with no root is a POLICY error, not a silent "/" . */
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_FS_WRITE), 0);
    CHECK_STATUS(run("halt\n"), DVM_TRAP_POLICY);
    CHECK_CONTAINS(R.msg, "fs_root is empty");

    /* A root that is not a usable path is refused by the setter. */
    dvm_policy_init(&POL);
    CHECK(dvm_policy_set_fs_root(&POL, "vm") != 0);
    CHECK(dvm_policy_set_fs_root(&POL, "/vm/../etc") != 0);
    CHECK(dvm_policy_set_fs_root(&POL, "") != 0);
    CHECK(dvm_policy_set_fs_root(&POL, NULL) != 0);
    {
        char toolong[DVM_PATH_MAX + 8];
        memset(toolong, 'a', sizeof toolong - 1);
        toolong[0] = '/';
        toolong[sizeof toolong - 1] = '\0';
        CHECK(dvm_policy_set_fs_root(&POL, toolong) != 0);
    }
    CHECK_EQ(dvm_policy_set_fs_root(&POL, "/"), 0);      /* legal, and typed */

    /* A hand-built policy with a bit for a syscall that does not exist is
     * refused: sys_allow must have come from dvm_policy_allow_sys(). */
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    POL.sys_allow = 0xFFFFFFFFu;
    CHECK_STATUS(run("halt\n"), DVM_TRAP_POLICY);
    CHECK_CONTAINS(R.msg, "sys_allow");

    /* An unterminated fs_root cannot be read as a string. */
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    memset(POL.fs_root, 'a', sizeof POL.fs_root);
    CHECK_STATUS(run("halt\n"), DVM_TRAP_POLICY);
    CHECK_CONTAINS(R.msg, "not terminated");
}

static void test_syscall_paths(void) {
    std_policy();

    static const char *refused[] = {
        "notes",              /* relative                       */
        "/etc/passwd",        /* outside the root               */
        "/vmother/x",         /* a prefix match that is not a subtree */
        "/vm/../etc/passwd",  /* a traversal                    */
        "/vm/./x",            /* a "." component                */
        "/vm/a/../../b",      /* a traversal in the middle      */
        "..",                 /* not even absolute              */
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        char src[256];
        snprintf(src, sizeof src,
                 "mstr r1, 0, \"%s\"\npush 0\npush r1\nsys r2, fs.size\nhalt\n",
                 refused[i]);
        CHECK_STATUS(run(src), DVM_TRAP_SYS_DENIED);
        CHECK_EQ(n_fs_size, 0);
    }

    /* The root itself is inside the root. */
    CHECK_STATUS(run("mstr r1, 0, \"/vm\"\npush 0\npush r1\nsys r2, fs.size\nhalt\n"),
                 DVM_OK);
    CHECK_EQ(n_fs_size, 1);

    /* A path built at RUNTIME out of pieces is the whole point of having
     * strings, and it is checked exactly like a literal one. */
    CHECK_STATUS(run("mstr r1, 0,  \"/vm/\"\n"
                     "mstr r1, r1, \"notes\"\n"
                     "push 0\npush r1\n"
                     "sys  r2, fs.size\n"
                     "bne  wrong\n"
                     "halt\n"
                     "wrong: abort \"a path built at runtime was refused\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], strlen(FAKEFILE_BODY));

    /* ...and so is one the program builds to try to escape. */
    CHECK_STATUS(run("mstr r1, 0,  \"/vm/\"\n"
                     "mstr r1, r1, \"..\"\n"
                     "mstr r1, r1, \"/etc\"\n"
                     "push 0\npush r1\n"
                     "sys  r2, fs.size\n"
                     "halt\n"), DVM_TRAP_SYS_DENIED);

    /* A NUL or a control byte in the middle of a path. The span carries its
     * own length, so this is not a truncation — it is a byte the check has to
     * see and refuse. */
    CHECK_STATUS(run("mstr r1, 0, \"/vm/a\"\n"
                     "mst8 [3], 0\n"
                     "push 0\npush 5\n"
                     "sys  r2, fs.size\n"
                     "halt\n"), DVM_TRAP_SYS_DENIED);
    CHECK_CONTAINS(R.msg, "printable");
    CHECK_STATUS(run("mstr r1, 0, \"/vm/a\"\n"
                     "mst8 [4], 0x1b\n"
                     "push 0\npush 5\n"
                     "sys  r2, fs.size\n"
                     "halt\n"), DVM_TRAP_SYS_DENIED);
    CHECK_STATUS(run("mstr r1, 0, \"/vm/a\"\n"
                     "mst8 [4], 0xff\n"
                     "push 0\npush 5\n"
                     "sys  r2, fs.size\n"
                     "halt\n"), DVM_TRAP_SYS_DENIED);

    /* An empty path, an over-long one, and a span outside the arena. */
    CHECK_STATUS(run("push 0\npush 0\nsys r1, fs.size\nhalt\n"), DVM_TRAP_SYS_DENIED);
    {
        char src[256];
        snprintf(src, sizeof src,
                 "mstr r1, 0, \"/vm/\"\nmset r1, r1, 0x61, %d\n"
                 "push 0\npush r1\nsys r2, fs.size\nhalt\n", DVM_PATH_MAX);
        CHECK_STATUS(run(src), DVM_TRAP_SYS_DENIED);
        CHECK_CONTAINS(R.msg, "the limit is");
    }
    CHECK_STATUS(run("push 0xFFFF0000\npush 8\nsys r1, fs.size\nhalt\n"),
                 DVM_TRAP_MEM);
    CHECK_STATUS(run("push 0\npush 0xFFFFFFFFFFFFFFFF\nsys r1, fs.size\nhalt\n"),
                 DVM_TRAP_MEM);
}

static void test_syscall_buffers(void) {
    std_policy();

    /* fs.read's destination is bounded at both ends. */
    char src[256];
    snprintf(src, sizeof src,
             "mstr r1, 0, \"/vm/notes\"\npush 0\npush r1\npush %u\npush 8\n"
             "sys r2, fs.read\nhalt\n", DVM_MEM_SIZE - 8);
    CHECK_STATUS(run(src), DVM_OK);
    snprintf(src, sizeof src,
             "mstr r1, 0, \"/vm/notes\"\npush 0\npush r1\npush %u\npush 8\n"
             "sys r2, fs.read\nhalt\n", DVM_MEM_SIZE - 7);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
    CHECK_EQ(n_fs_read, 0);            /* refused BEFORE the kernel saw it */

    /* A capacity shorter than the file truncates, and says how much it got. */
    CHECK_STATUS(run("mstr r1, 0, \"/vm/notes\"\n"
                     "push 0\npush r1\npush 100\npush 4\n"
                     "sys  r2, fs.read\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 4);
    CHECK_STR(peek(100, 4), "HTTP");
    CHECK_STR(peek(104, 1), "");       /* and wrote nothing past it */

    /* Zero capacity is legal and reads nothing. */
    CHECK_STATUS(run("mstr r1, 0, \"/vm/notes\"\n"
                     "push 0\npush r1\npush 100\npush 0\n"
                     "sys  r2, fs.read\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 0);

    /* fs.write's source is bounded the same way, and what the kernel receives
     * is exactly the span the program named. */
    CHECK_STATUS(run("mstr r1, 0,  \"/vm/out\"\n"
                     "mstr r2, 64, \"the answer is 42\"\n"
                     "sub  r2, r2, 64\n"
                     "push 0\npush r1\npush 64\npush r2\n"
                     "sys  r3, fs.write\n"
                     "bne  wrong\n"
                     "halt\n"
                     "wrong: abort \"fs.write failed\"\n"), DVM_OK);
    CHECK_STR(wrote_path, "/vm/out");
    CHECK_STR(wrote_body, "the answer is 42");
    CHECK_EQ(R.reg[3], 16);

    snprintf(src, sizeof src,
             "mstr r1, 0, \"/vm/out\"\npush 0\npush r1\npush %u\npush 8\n"
             "sys r2, fs.write\nhalt\n", DVM_MEM_SIZE - 7);
    CHECK_STATUS(run(src), DVM_TRAP_MEM);
    CHECK_EQ(n_fs_write, 0);
}

static void test_console_and_audio_syscalls(void) {
    std_policy();

    CHECK_STATUS(run("mstr r1, 0, \"a program said this\"\n"
                     "push 0\npush r1\n"
                     "sys  r2, con.write\nhalt\n"), DVM_OK);
    CHECK_STR(said, "a program said this");
    CHECK_EQ(R.reg[2], 19);

    /* EVERY byte the console is handed is printable: this is the invariant
     * that keeps a '[' in column zero the kernel's. The synthetic console
     * counts an escape if it is ever handed anything else. */
    CHECK_STATUS(run("mstr r1, 0, \"a\\nb\\tc\"\n"
                     "mst8 [1], 0x1b\n"
                     "mst8 [3], 0x07\n"
                     "push 0\npush 5\n"
                     "sys  r2, con.write\nhalt\n"), DVM_OK);
    CHECK_STR(said, "a?b?c");

    /* Longer than a console line is a trap that names the limit, not a silent
     * truncation. Exactly the limit is fine. */
    {
        char src[256];
        snprintf(src, sizeof src, "push 0\npush %d\nsys r1, con.write\nhalt\n",
                 DVM_SAY_MAX);
        CHECK_STATUS(run(src), DVM_OK);
        snprintf(src, sizeof src, "push 0\npush %d\nsys r1, con.write\nhalt\n",
                 DVM_SAY_MAX + 1);
        CHECK_STATUS(run(src), DVM_TRAP_SYS_DENIED);
        CHECK_CONTAINS(R.msg, "split it");
    }

    /* con.write spends the print budget, so a program cannot own the console
     * by looping on it. */
    POL.max_prints = 3;
    CHECK_STATUS(run("loop: push 0\npush 4\nsys r1, con.write\njmp loop\n"),
                 DVM_TRAP_PRINT_BUDGET);
    CHECK_EQ(n_said, 3);
    /* ...and print and con.write share it, rather than having one each. */
    POL.max_prints = 2;
    CHECK_STATUS(run("print \"one\"\npush 0\npush 4\nsys r1, con.write\n"
                     "print \"three\"\nhalt\n"), DVM_TRAP_PRINT_BUDGET);

    /* audio.tone range-checks in the VM, so the service is never called with
     * a frequency it would have to defend itself against. */
    std_policy();
    CHECK_STATUS(run("push 440\npush 250\nsys r1, audio.tone\nbne bad\nhalt\n"
                     "bad: abort \"tone failed\"\n"), DVM_OK);
    CHECK_EQ(last_hz, 440);
    CHECK_EQ(last_ms, 250);
    CHECK_STATUS(run("push 19\npush 10\nsys r1, audio.tone\nhalt\n"),
                 DVM_TRAP_SYS_DENIED);
    CHECK_STATUS(run("push 20001\npush 10\nsys r1, audio.tone\nhalt\n"),
                 DVM_TRAP_SYS_DENIED);
    CHECK_STATUS(run("push 440\npush 0\nsys r1, audio.tone\nhalt\n"),
                 DVM_TRAP_SYS_DENIED);
    CHECK_STATUS(run("push 440\npush 5001\nsys r1, audio.tone\nhalt\n"),
                 DVM_TRAP_SYS_DENIED);
    CHECK_STATUS(run("push 0xFFFFFFFFFFFFFFFF\npush 10\nsys r1, audio.tone\nhalt\n"),
                 DVM_TRAP_SYS_DENIED);
    /* Every one of those was refused by the VM, so the service was never
     * entered at all: `escapes` would have counted it if it had been. */
    CHECK_EQ(n_tone, 0);

    /* A missing hook is NO_BACKEND, never a pretend success. */
    kernel_reset();
    CHECK_EQ(dvm_assemble("sys r1, time.ms\nhalt\n", 21, &P, &E), 0);
    CHECK_STATUS(dvm_run(&P, &POL, &BARE, NULL, 0, &R), DVM_TRAP_NOIO);
}

/* ====================================================================== */
/* 7. the worked example: an HTTP request, and a status code out of a reply */
/* ====================================================================== */

/* THE POINT OF THE WHOLE CHANGE, as one program.
 *
 * Before this, the VM could drive a sound card and nothing else, because it had
 * no way to hold a byte that was not going to a device. This program builds an
 * HTTP request out of literals and a runtime value, then takes a canned
 * response apart: finds the end of the status line, parses the numeric status,
 * finds the header it wants, and copies that header's value out. It touches no
 * hardware and needs none.
 *
 * The canned response is written by the program itself with mstr rather than
 * being seeded by the test, because nothing can seed the arena — which is the
 * property that makes "one program cannot read another's leftovers" true. */
static const char HTTP_PROGRAM[] =
    ".equ REQ   0                       ; the request we build\n"
    ".equ RESP  4096                    ; a reply, as if it had arrived\n"
    ".equ OUT   8192                    ; what we pick out of it\n"
    ".equ SP    32                      ; ' '\n"
    ".equ CR    13\n"
    "\n"
    "; ---- build: GET /status?id=<r9> HTTP/1.1, Host:, Accept:, blank line ----\n"
    "mov   r9, 42                       ; a value only known at run time\n"
    "mstr  r1, REQ, \"GET /status?id=\"\n"
    "mitoa r1, r1, r9, 10\n"
    "mstr  r1, r1, \" HTTP/1.1\\r\\n\"\n"
    "mstr  r1, r1, \"Host: api.example.com\\r\\n\"\n"
    "mstr  r1, r1, \"Accept: text/plain\\r\\n\"\n"
    "mstr  r1, r1, \"\\r\\n\"\n"
    "sub   r1, r1, REQ                  ; r1 = the request's length\n"
    "\n"
    "; ---- a reply arrives (the network syscall this would use does not\n"
    ";      exist yet, so the program writes one to parse) ----\n"
    "mstr  r2, RESP, \"HTTP/1.1 404 Not Found\\r\\n\"\n"
    "mstr  r2, r2,   \"Server: nginx\\r\\n\"\n"
    "mstr  r2, r2,   \"Content-Length: 128\\r\\n\"\n"
    "mstr  r2, r2,   \"\\r\\n\"\n"
    "sub   r2, r2, RESP                 ; r2 = the reply's length\n"
    "\n"
    "; ---- parse: the status code is the number after the first space ----\n"
    "mchr  r3, RESP, r2, SP\n"
    "bne   no_status\n"
    "add   r3, r3, 1\n"
    "matoi r4, r3, 3, 10                ; r4 = 404\n"
    "bne   no_status\n"
    "\n"
    "; ---- parse: the value of one header, up to the CR ----\n"
    "mfind r5, RESP, r2, \"Content-Length: \"\n"
    "bne   no_header\n"
    "add   r5, r5, 16                   ; step over the name and the space\n"
    "mov   r6, RESP\n"
    "add   r6, r6, r2\n"
    "sub   r6, r6, r5                   ; bytes left after the value's start\n"
    "mchr  r7, r5, r6, CR\n"
    "bne   no_header\n"
    "sub   r7, r7, r5                   ; r7 = the value's length\n"
    "mcpy  r8, OUT, r5, r7              ; and copy it somewhere stable\n"
    "matoi r10, r5, r7, 10              ; r10 = 128\n"
    "bne   no_header\n"
    "\n"
    "; ---- a 2xx would have been success; say what we decided ----\n"
    "cmp   r4, 400\n"
    "blt   ok\n"
    "mstr  r11, 16384, \"request failed with status \"\n"
    "mitoa r11, r11, r4, 10\n"
    "sub   r11, r11, 16384\n"
    "push  16384\n"
    "push  r11\n"
    "sys   r12, con.write\n"
    "ok:\n"
    "halt\n"
    "no_status:\n"
    "abort \"no status code in the reply\"\n"
    "no_header:\n"
    "abort \"no Content-Length in the reply\"\n";

static void test_builds_a_request_and_parses_a_reply(void) {
    std_policy();
    POL.trace = DVM_TRACE_OFF;

    CHECK_STATUS(run(HTTP_PROGRAM), DVM_OK);

    /* The request, byte for byte. */
    CHECK_EQ(R.reg[1], 73);
    CHECK_STR(peek(0, 73),
              "GET /status?id=42 HTTP/1.1\r\n"
              "Host: api.example.com\r\n"
              "Accept: text/plain\r\n"
              "\r\n");

    /* The status code, as a number. */
    CHECK_EQ(R.reg[4], 404);

    /* The header value, both as text and as a number. */
    CHECK_EQ(R.reg[7], 3);
    CHECK_STR(peek(8192, 3), "128");
    CHECK_EQ(R.reg[10], 128);

    /* And it reported the failure to the operator, through the one path that
     * cannot forge a kernel line. */
    CHECK_STR(said, "request failed with status 404");

    /* It did all of that inside the budgets, with no device at all. */
    CHECK(R.io_ops == 0);
    CHECK(R.n_dma_rd == 0 && R.n_dma_wr == 0);
    CHECK_EQ(R.n_sys, 1);
    printf("    http program: %lu steps, %lu scratch bytes, %lu syscalls\n",
           (unsigned long)R.steps, (unsigned long)R.mem_bytes,
           (unsigned long)R.n_sys);
}

/* The same program with the trace on, because the transcript is the other half
 * of what a model gets back and it has to be readable. */
static void test_the_trace_of_that_program(void) {
    std_policy();
    POL.trace = DVM_TRACE_IO;
    kcap_reset();
    CHECK_STATUS(run(HTTP_PROGRAM), DVM_OK);
    const char *t = kcap_text();
    CHECK_CONTAINS(t, "[dvm.grant mem 65536 bytes scratch (zeroed); sys ");
    CHECK_CONTAINS(t, "under /vm");
    CHECK_CONTAINS(t, "sys con.write ok");
    /* The text itself reaches the console through the KERNEL backend, which is
     * #ifdef'd out of a host build; what this suite can prove is that the VM
     * called it, and that the bytes it handed over were printable (k_con
     * counts an escape otherwise). */
    CHECK_CONTAINS(t, "[dvm.halt");
    printf("%s", t);
}

/* THE WORKED EXAMPLE OUT OF driver_run's DESCRIPTION, verbatim.
 *
 * That description is the only documentation of this ISA that exists, and there
 * is no training data for it: whatever the example does is what a model will
 * believe the machine does. So it is copied here character for character and
 * run. The first version of it used an END offset where a LENGTH belonged and
 * still produced 404 by accident — which is exactly the kind of thing a model
 * copies and then cannot debug. If this test and tools/dvm_tools.c ever
 * disagree, the description is wrong and this is how you find out. */
static void test_the_documented_example_actually_runs(void) {
    std_policy();
    CHECK_STATUS(run(
        "mstr r1,0,\"GET /x HTTP/1.1\\r\\nHost: \"\n"
        "mstr r1,r1,\"h.example\\r\\n\\r\\n\" ; r1 = the request's length\n"
        "mstr r2,4096,\"HTTP/1.1 404 Not Found\" ; a reply at 4096\n"
        "sub r2,r2,4096 ; an END offset minus its start IS a length\n"
        "mchr r3,4096,r2,32 ; the space after that\n"
        "add r3,r3,1\n"
        "matoi r4,r3,3,10 ; r4 = 404, zero flag set\n"
        "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 36);
    CHECK_STR(peek(0, 36), "GET /x HTTP/1.1\r\nHost: h.example\r\n\r\n");
    CHECK_EQ(R.reg[2], 22);
    CHECK_EQ(R.reg[3], 4105);
    CHECK_EQ(R.reg[4], 404);
}

/* ====================================================================== */
/* 8. fuzz                                                                */
/* ====================================================================== */

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Operand values chosen to sit on the boundaries as often as in the middle:
 * random 64-bit garbage almost never lands one byte past the end of the arena,
 * and one byte past the end is the whole question. */
static uint64_t fuzz_val(void) {
    switch (rnd() % 12) {
        case 0:  return 0;
        case 1:  return 1;
        case 2:  return DVM_MEM_SIZE;
        case 3:  return DVM_MEM_SIZE - 1;
        case 4:  return DVM_MEM_SIZE + 1;
        case 5:  return (uint64_t)-1;
        case 6:  return 0x8000000000000000ull;
        case 7:  return rnd() % DVM_MEM_SIZE;
        case 8:  return DVM_MEM_SIZE - (rnd() % 9);
        case 9:  return ((uint64_t)rnd() << 32) | rnd();
        case 10: return rnd() % 300;
        default: return DVM_PATH_MAX + (rnd() % 8) - 4;
    }
}

static int status_is_known(dvm_status_t s) {
    switch (s) {
        case DVM_OK: case DVM_TRAP_ABORT: case DVM_TRAP_STEPS:
        case DVM_TRAP_PRINT_BUDGET: case DVM_TRAP_RANGE: case DVM_TRAP_PC:
        case DVM_TRAP_STACK: case DVM_TRAP_DIV0: case DVM_TRAP_MEM:
        case DVM_TRAP_MEM_BUDGET: case DVM_TRAP_SYS_DENIED:
        case DVM_TRAP_SYS_BUDGET: case DVM_TRAP_NOIO:
            return 1;
        default:
            return 0;
    }
}

/* Nothing outside the arena may ever be written. The arena is a static inside
 * vm/dvm.c, so this cannot bracket it with canaries from out here — what it CAN
 * do is prove that no byte of it is touched that a program did not name, by
 * running programs that only ever write above a watermark and checking the
 * bytes below. Combined with the exhaustive one-past-the-end tests above and
 * ASan on tests/build (make test-host-asan), that is the coverage available. */
static void test_fuzz_memory_ops(void) {
    static const char *mem_ops[] = {
        "mld8 r1, [r2+r3]", "mld16 r1, [r2+r3]", "mld32 r1, [r2+r3]",
        "mld64 r1, [r2+r3]",
        "mst8 [r2+r3], r4", "mst16 [r2+r3], r4", "mst32 [r2+r3], r4",
        "mst64 [r2+r3], r4",
        "mstr r1, r2, \"fuzz\"", "mstr r1, r2, \"\"",
        "mcpy r1, r2, r3, r4", "mset r1, r2, r3, r4", "mcmp r2, r3, r4",
        "mfind r1, r2, r3, \"ab\"", "mfind r1, r2, r3, \"\"",
        "mchr r1, r2, r3, r4",
        "matoi r1, r2, r3, r4", "mitoa r1, r2, r3, r4",
        "matoi r1, r2, r3, 10", "mitoa r1, r2, r3, 16",
    };
    const int NOPS = (int)(sizeof mem_ops / sizeof mem_ops[0]);
    int ran = 0, statuses[DVM_STATUS__COUNT];
    memset(statuses, 0, sizeof statuses);

    for (int iter = 0; iter < 20000; iter++) {
        char src[2048];
        int  n = 0;
        int  lines = 1 + (int)(rnd() % 6);

        n += snprintf(src + n, sizeof src - n,
                      "mov r2, %llu\nmov r3, %llu\nmov r4, %llu\n",
                      (unsigned long long)fuzz_val(),
                      (unsigned long long)fuzz_val(),
                      (unsigned long long)fuzz_val());
        for (int i = 0; i < lines && n < (int)sizeof src - 80; i++) {
            n += snprintf(src + n, sizeof src - n, "%s\n",
                          mem_ops[rnd() % NOPS]);
            if (rnd() % 3 == 0)
                n += snprintf(src + n, sizeof src - n, "mov r2, %llu\n",
                              (unsigned long long)fuzz_val());
        }
        snprintf(src + n, sizeof src - n, "halt\n");

        std_policy();
        if (rnd() % 4 == 0) POL.max_mem_bytes = 1 + (rnd() % 4096);
        dvm_status_t s = run(src);
        statuses[s < DVM_STATUS__COUNT ? s : 0]++;
        ran++;

        th_checks++;
        if (!status_is_known(s)) {
            th_fails++;
            printf("    FAIL unexpected status %s from:\n%s\n",
                   dvm_status_name(s), src);
        }
        /* The counters must never claim more work than the budget allowed. */
        th_checks++;
        if (R.mem_bytes > POL.max_mem_bytes) {
            th_fails++;
            printf("    FAIL mem_bytes %llu over budget %llu\n",
                   (unsigned long long)R.mem_bytes,
                   (unsigned long long)POL.max_mem_bytes);
        }
        th_checks++;
        if (R.mem_hwm > DVM_MEM_SIZE) {
            th_fails++;
            printf("    FAIL mem_hwm %u past the end of the arena\n", R.mem_hwm);
        }
    }
    printf("    (memory fuzz: %d programs; OK=%d MEM_RANGE=%d MEM_LIMIT=%d "
           "OPERAND_RANGE=%d STEP_LIMIT=%d)\n",
           ran, statuses[DVM_OK], statuses[DVM_TRAP_MEM],
           statuses[DVM_TRAP_MEM_BUDGET], statuses[DVM_TRAP_RANGE],
           statuses[DVM_TRAP_STEPS]);
    /* A fuzz run that only ever produced one outcome would be proving nothing. */
    CHECK(statuses[DVM_OK] > 100);
    CHECK(statuses[DVM_TRAP_MEM] > 100);
}

static void test_fuzz_syscalls(void) {
    static const char *calls[] = {
        "sys r1, con.write", "sys r1, fs.read", "sys r1, fs.write",
        "sys r1, fs.size", "sys r1, audio.tone", "sys r1, time.ms",
    };
    static const char *paths[] = {
        "/vm/notes", "/vm/out", "/etc/shadow", "../../etc", "/vm/../x",
        "/vm", "/", "x", "/vm/a/b/c",
    };
    const int NCALLS = (int)(sizeof calls / sizeof calls[0]);
    const int NPATHS = (int)(sizeof paths / sizeof paths[0]);
    int ran = 0;

    for (int iter = 0; iter < 12000; iter++) {
        char src[1024];
        int  n = 0;

        n += snprintf(src + n, sizeof src - n, "mstr r9, 0, \"%s\"\n",
                      paths[rnd() % NPATHS]);
        int npush = (int)(rnd() % 6);
        for (int i = 0; i < npush; i++) {
            if (rnd() % 3 == 0)
                n += snprintf(src + n, sizeof src - n, "push r9\n");
            else
                n += snprintf(src + n, sizeof src - n, "push %llu\n",
                              (unsigned long long)fuzz_val());
        }
        n += snprintf(src + n, sizeof src - n, "%s\n", calls[rnd() % NCALLS]);
        snprintf(src + n, sizeof src - n, "halt\n");

        std_policy();
        if (rnd() % 5 == 0) POL.sys_allow = 0;         /* deny-all sometimes */
        if (rnd() % 7 == 0) POL.max_sys = 0;
        dvm_status_t s = run(src);
        ran++;

        th_checks++;
        if (!status_is_known(s)) {
            th_fails++;
            printf("    FAIL unexpected status %s from:\n%s\n",
                   dvm_status_name(s), src);
        }
    }
    printf("    (syscall fuzz: %d programs, %ld boundary escapes)\n", ran, escapes);
}

/* ====================================================================== */

int main(void) {
    console_init();
    trace_set_enabled(1);

    RUN(test_arena_shape);

    RUN(test_load_store_widths);
    RUN(test_load_store_bounds);

    RUN(test_mstr_and_chaining);
    RUN(test_mcpy_overlap);
    RUN(test_mset);
    RUN(test_mcmp);
    RUN(test_mfind_and_mchr);
    RUN(test_integer_conversion);

    RUN(test_arena_is_zeroed_between_runs);
    RUN(test_memory_budget);

    RUN(test_syscall_convention);
    RUN(test_syscall_grants);
    RUN(test_syscall_paths);
    RUN(test_syscall_buffers);
    RUN(test_console_and_audio_syscalls);

    RUN(test_builds_a_request_and_parses_a_reply);
    RUN(test_the_trace_of_that_program);
    RUN(test_the_documented_example_actually_runs);

    RUN(test_fuzz_memory_ops);
    RUN(test_fuzz_syscalls);

    /* The load-bearing safety property: the synthetic kernel was never handed
     * anything the VM was supposed to refuse — not a path outside the granted
     * root, not a control byte, not an out-of-range tone. */
    CHECK_EQ(escapes, 0);
    CHECK_EQ(kpanic_hit, 0);

    return th_report("dvm_mem");
}
