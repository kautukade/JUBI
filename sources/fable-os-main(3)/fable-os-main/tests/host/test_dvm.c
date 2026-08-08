/* test_dvm.c — host tests for the driver VM (include/dvm.h, vm/dvm.c).
 *
 * WHAT CAN AND CANNOT BE TESTED HERE
 *   The VM reaches hardware through exactly one struct of function pointers,
 *   dvm_io_t, which is why the whole thing runs natively. This file IS the
 *   hardware: a synthetic device with I/O ports, two MMIO windows (one of them
 *   read-only), PCI config space, and a clear-on-read status register, plus a
 *   log of every access in order. So every opcode's semantics, every bound,
 *   every refusal and the exact sequence of device accesses a program performs
 *   are all asserted here without booting anything. Same arrangement as
 *   tests/host/test_dev_tools.c, for the same reason.
 *
 *   What is NOT covered here, and is proved by booting instead: that
 *   dvm_io_hardware() really issues in/out instructions and dereferences MMIO,
 *   and that hw_delay_us actually waits.
 *
 * THE LOAD-BEARING ASSERTION
 *   The synthetic device counts every access it was never supposed to see: any
 *   port in the compiled-in deny list, any MMIO address outside the two modelled
 *   windows, any PCI function that was not granted. `forbidden` must be zero at
 *   the end of the suite — including after ~40,000 fuzzed programs. That is a
 *   mechanical proof of containment rather than a claim about it: if any check
 *   in vm/dvm.c ever stops working, the backend records the escape.
 */

#include "harness.h"
#include "dvm.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* the synthetic device                                                   */
/* ====================================================================== */

#define PORT_BASE   0xC000u          /* the "BAR" the tests hand out       */
#define PORT_SIZE   0x40u
#define MMIO_BASE   0xFEBC0000ull    /* modelled read/write window         */
#define MMIO_SIZE   0x1000ull
#define MMIO_RO     0xFEBD0000ull    /* modelled read-only window          */
#define MMIO_RO_SZ  0x100ull

/* Registers with behaviour, so a poll loop can be exercised for real. */
#define REG_STATUS  0x10             /* MMIO: bit0 clears after N reads    */
#define REG_LATCH   0x20             /* MMIO: last value written, readable */
#define PORT_COUNT  0x08             /* port: increments on every read     */

typedef enum { A_PW, A_PR, A_MW, A_MR, A_PCI, A_DLY } akind_t;

typedef struct {
    akind_t  k;
    uint64_t addr;
    uint8_t  w;
    uint32_t val;
} acc_t;

#define ALOG_MAX 4096
static acc_t    alog[ALOG_MAX];
static int      nalog;
static long     forbidden;          /* MUST stay 0 for the whole suite */
static long     total_acc;
static uint8_t  pspace[0x10000];    /* port space, byte addressed */
static uint8_t  mspace[MMIO_SIZE];
static uint8_t  mspace_ro[MMIO_RO_SZ];
static int      status_reads;       /* REG_STATUS clears bit0 after 3 reads */
static int      always_busy;        /* when set, REG_STATUS never clears     */
static int      port_counter;
static uint32_t pcicfg[8][64];      /* [devfn index][dword] */
static uint64_t delay_total;

/* Ports the VM must never issue an access to, whatever a program asks for.
 * Mirrors the deny table in vm/dvm.c — deliberately restated here so the test
 * fails if that table is quietly narrowed. */
static int port_is_forbidden(unsigned p) {
    return (p >= 0x20 && p <= 0x21) || (p >= 0x40 && p <= 0x43) ||
           (p >= 0x60 && p <= 0x64) || (p >= 0x70 && p <= 0x71) || p == 0x92 ||
           (p >= 0xA0 && p <= 0xA1) || (p >= 0x2F8 && p <= 0x2FF) ||
           (p >= 0x3F8 && p <= 0x3FF) || (p >= 0xCF8 && p <= 0xCFF) ||
           /* things that destroy state that does not come back, rather than
            * things that crash the machine */
           (p <= 0x0F) || (p >= 0x80 && p <= 0x8F) || (p >= 0xC0 && p <= 0xDF) ||
           (p >= 0x170 && p <= 0x177) || (p >= 0x1F0 && p <= 0x1F7) ||
           (p >= 0x3B0 && p <= 0x3DF) || (p >= 0x3F0 && p <= 0x3F7);
}

static void log_acc(akind_t k, uint64_t addr, uint8_t w, uint32_t val) {
    total_acc++;
    if (nalog < ALOG_MAX) {
        alog[nalog].k = k; alog[nalog].addr = addr;
        alog[nalog].w = w; alog[nalog].val = val;
        nalog++;
    }
}

static void dev_reset(void) {
    nalog = 0;
    memset(pspace, 0, sizeof pspace);
    memset(mspace, 0, sizeof mspace);
    memset(mspace_ro, 0, sizeof mspace_ro);
    status_reads = 0;
    always_busy = 0;
    port_counter = 0;
    delay_total = 0;
}

static uint32_t rd_le(const uint8_t *p, uint8_t w) {
    uint32_t v = 0;
    for (int i = 0; i < w; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static void wr_le(uint8_t *p, uint8_t w, uint32_t v) {
    for (int i = 0; i < w; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void io_port_write(void *ctx, uint16_t port, uint8_t width, uint32_t val) {
    (void)ctx;
    if (port_is_forbidden(port)) { forbidden++; return; }
    log_acc(A_PW, port, width, val);
    wr_le(&pspace[port], width, val);
}

static uint32_t io_port_read(void *ctx, uint16_t port, uint8_t width) {
    (void)ctx;
    if (port_is_forbidden(port)) { forbidden++; return 0xFFFFFFFFu; }
    uint32_t v;
    if (port == PORT_BASE + PORT_COUNT) v = (uint32_t)port_counter++;
    else                                v = rd_le(&pspace[port], width);
    if (width == 1) v &= 0xFF; else if (width == 2) v &= 0xFFFF;
    log_acc(A_PR, port, width, v);
    return v;
}

static void io_mmio_write(void *ctx, uint64_t addr, uint8_t width, uint32_t val) {
    (void)ctx;
    if (addr < MMIO_BASE || addr + width > MMIO_BASE + MMIO_SIZE) { forbidden++; return; }
    log_acc(A_MW, addr, width, val);
    wr_le(&mspace[addr - MMIO_BASE], width, val);
    if (addr - MMIO_BASE == REG_LATCH) status_reads = 0;   /* arms the poll */
}

static uint32_t io_mmio_read(void *ctx, uint64_t addr, uint8_t width) {
    (void)ctx;
    uint32_t v;
    if (addr >= MMIO_BASE && addr + width <= MMIO_BASE + MMIO_SIZE) {
        if (addr - MMIO_BASE == REG_STATUS) {
            /* Busy for three reads, then ready: the shape of a real bring-up. */
            v = (always_busy || status_reads++ < 3) ? 1u : 0u;
        } else {
            v = rd_le(&mspace[addr - MMIO_BASE], width);
        }
    } else if (addr >= MMIO_RO && addr + width <= MMIO_RO + MMIO_RO_SZ) {
        v = rd_le(&mspace_ro[addr - MMIO_RO], width);
    } else {
        forbidden++;
        return 0xFFFFFFFFu;
    }
    if (width == 1) v &= 0xFF; else if (width == 2) v &= 0xFFFF;
    log_acc(A_MR, addr, width, v);
    return v;
}

static uint32_t io_pci_read32(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    (void)ctx;
    if (bus != 0 || dev > 7 || (off & 3)) { forbidden++; return 0xFFFFFFFFu; }
    uint32_t v = pcicfg[dev][off >> 2] + fn;      /* fn folded in, so fn is checked */
    log_acc(A_PCI, ((uint64_t)bus << 8) | ((uint64_t)dev << 3) | fn, 4, v);
    return v;
}

static void io_delay(void *ctx, uint32_t us) {
    (void)ctx;
    delay_total += us;
    log_acc(A_DLY, 0, 0, us);
}

static const dvm_io_t DEV = {
    .ctx = NULL,
    .port_write = io_port_write, .port_read = io_port_read,
    .mmio_write = io_mmio_write, .mmio_read = io_mmio_read,
    .pci_read32 = io_pci_read32, .delay_us  = io_delay,
};

/* A backend with no hooks at all: every access class must trap NO_BACKEND. */
static const dvm_io_t NULLDEV = { 0, 0, 0, 0, 0, 0, 0, 0 };

/* ====================================================================== */
/* harness helpers                                                        */
/* ====================================================================== */

static dvm_program_t P;          /* ~11 KB: static, never on the stack */
static dvm_policy_t  POL;
static dvm_result_t  R;
static dvm_asm_err_t E;

/* Which opcodes have appeared in a program that was actually executed. */
static int op_seen[DVM_OP__COUNT];

static void mark_ops(const dvm_program_t *p) {
    for (uint32_t i = 0; i < p->ninsn; i++)
        if (p->insn[i].op < DVM_OP__COUNT) op_seen[p->insn[i].op] = 1;
}

/* The standard sandbox the tests hand out: this device's ports, its read/write
 * MMIO window, a read-only second window, and two PCI functions. */
static void std_policy(void) {
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_ports(&POL, PORT_BASE, PORT_BASE + PORT_SIZE - 1), 0);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, MMIO_BASE, MMIO_SIZE, DVM_MMIO_RW), 0);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, MMIO_RO, MMIO_RO_SZ, DVM_MMIO_R), 0);
    CHECK_EQ(dvm_policy_allow_pci(&POL, 0, 3, 0), 0);
    CHECK_EQ(dvm_policy_allow_pci(&POL, 0, 4, 1), 0);
    POL.trace = DVM_TRACE_OFF;
}

static int assemble(const char *src) {
    return dvm_assemble(src, strlen(src), &P, &E);
}

/* Assemble + run, expecting the assembly to succeed. */
static dvm_status_t run(const char *src) {
    dev_reset();
    if (assemble(src) != 0) {
        printf("    ASSEMBLY FAILED line %d: %s\n", E.line, E.msg);
        th_fails++; th_checks++;
        return DVM_TRAP_BADOP;
    }
    mark_ops(&P);
    return dvm_run(&P, &POL, &DEV, NULL, 0, &R);
}

static dvm_status_t run_args(const char *src, const uint64_t *a, int n) {
    dev_reset();
    if (assemble(src) != 0) {
        printf("    ASSEMBLY FAILED line %d: %s\n", E.line, E.msg);
        th_fails++; th_checks++;
        return DVM_TRAP_BADOP;
    }
    mark_ops(&P);
    return dvm_run(&P, &POL, &DEV, a, n, &R);
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

static void chk_acc(int i, akind_t k, uint64_t addr, uint8_t w, uint32_t val) {
    th_checks++;
    if (i >= nalog) {
        th_fails++;
        printf("    FAIL access %d missing (only %d recorded)\n", i, nalog);
        return;
    }
    const acc_t *a = &alog[i];
    if (a->k != k || a->addr != addr || a->w != w || a->val != val) {
        th_fails++;
        printf("    FAIL access %d: got k=%d addr=0x%llx w=%u val=0x%x, "
               "want k=%d addr=0x%llx w=%u val=0x%x\n",
               i, a->k, (unsigned long long)a->addr, a->w, a->val,
               k, (unsigned long long)addr, w, val);
    }
}

/* ====================================================================== */
/* 1. assembler: shape, sugar, listings                                   */
/* ====================================================================== */

static void test_assemble_basic(void) {
    CHECK_EQ(assemble("; a comment\n"
                      "start:\n"
                      "  mov r1, 0x10   # trailing comment\n"
                      "  add r1, r1, 4  // another\n"
                      "  halt\n"), 0);
    CHECK_EQ(P.ninsn, 3);
    CHECK_EQ(P.nlabel, 1);
    CHECK_STR(P.label[0].name, "start");
    CHECK_EQ(P.label[0].pc, 0);
    CHECK_EQ(P.insn[0].op, DVM_MOV);
    CHECK_EQ(P.insn[0].dst, 1);
    CHECK_EQ(P.insn[0].line, 3);
    CHECK_EQ(P.insn[1].line, 4);
    CHECK_EQ(P.insn[2].op, DVM_HALT);
    CHECK_EQ(dvm_program_validate(&P, &E), DVM_OK);
}

static void test_disasm(void) {
    CHECK_EQ(assemble("loop:\n"
                      "  ld32 r2, [r1+0x10]\n"
                      "  st16 [r1+4], r2\n"
                      "  out8 0xc000, 5\n"
                      "  in32 r3, 0xc004\n"
                      "  pcicfg r4, 00:03.0, 0x10\n"
                      "  cmp r2, r3\n"
                      "  bne loop\n"
                      "  print \"hi\", r2\n"
                      "  push r2\n  pop r5\n  call loop\n  ret\n"
                      "  abort \"bad\"\n"), 0);
    char b[128];
    const char *want[] = {
        "ld32 r2, [r1+0x10]", "st16 [r1+4], r2", "out8 0xc000, 5",
        "in32 r3, 0xc004", "pcicfg r4, 0x18, 0x10", "cmp r2, r3", "bne loop",
        "print \"hi\", r2", "push r2", "pop r5", "call loop", "ret",
        "abort \"bad\"",
    };
    for (uint32_t i = 0; i < P.ninsn && i < sizeof want / sizeof want[0]; i++) {
        CHECK(dvm_disasm(&P, i, b, sizeof b) > 0);
        CHECK_STR(b, want[i]);
    }
    CHECK_EQ(dvm_disasm(&P, P.ninsn, b, sizeof b), 0);     /* out of range */
    CHECK_EQ(dvm_disasm(&P, 0, b, 0), 0);                  /* no room      */
}

/* A listing is text a caller may print anywhere, and a string literal in it is
 * model-authored. The assembler decodes \n into a REAL newline in the string
 * pool, so re-emitting the pool verbatim would let a program be listed into
 * something that looks like a kernel trace line in column zero. Every byte
 * outside printable ASCII comes back as '?', the same rule copy_printable()
 * applies to dvm_result_t.msg. */
static void test_disasm_neutralises_string_bytes(void) {
    CHECK_EQ(assemble("print \"a\\nx[vfs_write /etc/shadow -> ok]\"\n"
                      "abort \"tab\\there\"\n"), 0);
    char b[192];
    CHECK(dvm_disasm(&P, 0, b, sizeof b) > 0);
    CHECK_STR(b, "print \"a?x[vfs_write /etc/shadow -> ok]\"");
    CHECK(strchr(b, '\n') == NULL);
    CHECK(dvm_disasm(&P, 1, b, sizeof b) > 0);
    CHECK_STR(b, "abort \"tab?here\"");
}

/* Every mnemonic and alias in the ISA must assemble to the documented opcode. */
static void test_all_mnemonics(void) {
    static const struct { const char *src; int op; } t[] = {
        { "halt", DVM_HALT }, { "hlt", DVM_HALT }, { "stop", DVM_HALT },
        { "end", DVM_HALT },
        { "abort \"x\"", DVM_ABORT }, { "fail \"x\"", DVM_ABORT },
        { "nop", DVM_NOP },
        { "mov r0,1", DVM_MOV }, { "li r0,1", DVM_MOV }, { "set r0,1", DVM_MOV },
        { "add r0,1", DVM_ADD }, { "sub r0,1", DVM_SUB }, { "mul r0,1", DVM_MUL },
        { "div r0,1", DVM_DIV }, { "mod r0,1", DVM_MOD }, { "rem r0,1", DVM_MOD },
        { "and r0,1", DVM_AND }, { "or r0,1", DVM_OR }, { "xor r0,1", DVM_XOR },
        { "not r0,r1", DVM_NOT },
        { "shl r0,1", DVM_SHL }, { "lsl r0,1", DVM_SHL },
        { "shr r0,1", DVM_SHR }, { "lsr r0,1", DVM_SHR },
        { "cmp r0,1", DVM_CMP }, { "test r0,1", DVM_TEST }, { "tst r0,1", DVM_TEST },
        { "L: jmp L", DVM_JMP }, { "L: j L", DVM_JMP }, { "L: b L", DVM_JMP },
        { "L: beq L", DVM_BEQ }, { "L: je L", DVM_BEQ }, { "L: jz L", DVM_BEQ },
        { "L: bz L", DVM_BEQ },
        { "L: bne L", DVM_BNE }, { "L: jne L", DVM_BNE }, { "L: jnz L", DVM_BNE },
        { "L: bnz L", DVM_BNE },
        { "L: blt L", DVM_BLT }, { "L: jb L", DVM_BLT }, { "L: jl L", DVM_BLT },
        { "L: ble L", DVM_BLE }, { "L: jbe L", DVM_BLE }, { "L: jle L", DVM_BLE },
        { "L: bgt L", DVM_BGT }, { "L: ja L", DVM_BGT }, { "L: jg L", DVM_BGT },
        { "L: bge L", DVM_BGE }, { "L: jae L", DVM_BGE }, { "L: jge L", DVM_BGE },
        { "L: call L", DVM_CALL }, { "ret", DVM_RET },
        { "push 1", DVM_PUSH }, { "pop r0", DVM_POP },
        { "out8 1,2", DVM_OUT8 }, { "outb 1,2", DVM_OUT8 },
        { "out16 1,2", DVM_OUT16 }, { "outw 1,2", DVM_OUT16 },
        { "out32 1,2", DVM_OUT32 }, { "outl 1,2", DVM_OUT32 },
        { "outd 1,2", DVM_OUT32 },
        { "in8 r0,1", DVM_IN8 }, { "inb r0,1", DVM_IN8 },
        { "in16 r0,1", DVM_IN16 }, { "inw r0,1", DVM_IN16 },
        { "in32 r0,1", DVM_IN32 }, { "inl r0,1", DVM_IN32 },
        { "ind r0,1", DVM_IN32 },
        { "ld8 r0,[r1]", DVM_LD8 }, { "mr8 r0,[r1]", DVM_LD8 },
        { "ld16 r0,[r1]", DVM_LD16 }, { "mr16 r0,[r1]", DVM_LD16 },
        { "ld32 r0,[r1]", DVM_LD32 }, { "mr32 r0,[r1]", DVM_LD32 },
        { "st8 [r1],r0", DVM_ST8 }, { "mw8 [r1],r0", DVM_ST8 },
        { "st16 [r1],r0", DVM_ST16 }, { "mw16 [r1],r0", DVM_ST16 },
        { "st32 [r1],r0", DVM_ST32 }, { "mw32 [r1],r0", DVM_ST32 },
        { "pcicfg r0,0,0", DVM_PCICFG }, { "pciread r0,0,0", DVM_PCICFG },
        { "pcird r0,0,0", DVM_PCICFG },
        { "delay 1", DVM_DELAY }, { "udelay 1", DVM_DELAY }, { "usleep 1", DVM_DELAY },
        { "print \"x\"", DVM_PRINT }, { "log \"x\"", DVM_PRINT },
        { "mld8 r0,[r1]", DVM_MLD8 }, { "mld16 r0,[r1]", DVM_MLD16 },
        { "mld32 r0,[r1]", DVM_MLD32 }, { "mld64 r0,[r1]", DVM_MLD64 },
        { "mst8 [r1],r0", DVM_MST8 }, { "mst16 [r1],r0", DVM_MST16 },
        { "mst32 [r1],r0", DVM_MST32 }, { "mst64 [r1],r0", DVM_MST64 },
        { "mstr r0,0,\"x\"", DVM_MSTR },
        { "mcpy r0,0,1,2", DVM_MCPY }, { "mmove r0,0,1,2", DVM_MCPY },
        { "mset r0,0,32,2", DVM_MSET }, { "mfill r0,0,32,2", DVM_MSET },
        { "mcmp 0,1,2", DVM_MCMP },
        { "mfind r0,0,8,\"x\"", DVM_MFIND },
        { "mchr r0,0,8,32", DVM_MCHR },
        { "matoi r0,0,3,10", DVM_MATOI }, { "atoi r0,0,3,10", DVM_MATOI },
        { "mitoa r0,0,7,10", DVM_MITOA }, { "itoa r0,0,7,10", DVM_MITOA },
        { "sys r0,time.ms", DVM_SYS }, { "syscall r0,con.write", DVM_SYS },
        { "sys r0,0", DVM_SYS },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (dvm_assemble(t[i].src, strlen(t[i].src), &P, &E) != 0) {
            th_checks++; th_fails++;
            printf("    FAIL \"%s\" did not assemble: line %d: %s\n",
                   t[i].src, E.line, E.msg);
            continue;
        }
        CHECK_EQ(P.insn[P.ninsn - 1].op, t[i].op);
    }
    /* Mnemonics are case-insensitive. */
    CHECK_EQ(assemble("MOV R1, 0xFF\nHALT\n"), 0);
    CHECK_EQ(P.insn[0].op, DVM_MOV);
    CHECK_EQ(P.insn[0].val[0], 0xFF);
}

static void test_number_forms(void) {
    CHECK_EQ(assemble("mov r1, 0x1f\nmov r2, 0b1011\nmov r3, 42\n"
                      "mov r4, -1\nmov r5, 0xdead_beef\nhalt\n"), 0);
    CHECK_EQ(P.insn[0].val[0], 0x1f);
    CHECK_EQ(P.insn[1].val[0], 11);
    CHECK_EQ(P.insn[2].val[0], 42);
    CHECK_EQ(P.insn[3].val[0], (uint64_t)-1);
    CHECK_EQ(P.insn[4].val[0], 0xdeadbeefull);

    /* .equ, in every accepted spelling, including one constant from another. */
    CHECK_EQ(assemble(".equ BAR 0xc000\n.def OFF, 4\n.set SUM OFF\n"
                      "mov r1, BAR\nmov r2, OFF\nmov r3, SUM\nhalt\n"), 0);
    CHECK_EQ(P.insn[0].val[0], 0xc000);
    CHECK_EQ(P.insn[1].val[0], 4);
    CHECK_EQ(P.insn[2].val[0], 4);
}

static void test_address_sugar(void) {
    /* [base+disp], [base], plain pair, and plain base all mean the same thing. */
    CHECK_EQ(assemble("ld32 r1, [r2+0x10]\n"
                      "ld32 r1, r2, 0x10\n"
                      "ld32 r1, [r2]\n"
                      "ld32 r1, r2\n"
                      "st32 [r2+0x10], r3\n"
                      "st32 r2, 0x10, r3\n"
                      "st32 [r2], r3\n"
                      "st32 r2, r3\n"), 0);
    CHECK_EQ(P.insn[0].kind[0], DVM_O_REG); CHECK_EQ(P.insn[0].val[0], 2);
    CHECK_EQ(P.insn[0].kind[1], DVM_O_IMM); CHECK_EQ(P.insn[0].val[1], 0x10);
    CHECK_EQ(P.insn[1].val[1], 0x10);
    CHECK_EQ(P.insn[2].val[1], 0);
    CHECK_EQ(P.insn[3].val[1], 0);
    CHECK_EQ(P.insn[4].val[2], 3);
    CHECK_EQ(P.insn[5].val[2], 3);
    CHECK_EQ(P.insn[6].val[2], 3);
    CHECK_EQ(P.insn[7].val[2], 3);
    /* whitespace inside the brackets, and a port in brackets */
    CHECK_EQ(assemble("ld32 r1, [ r2 + 8 ]\nout8 [0xc000], 1\nin8 r1, [0xc000]\n"), 0);
    CHECK_EQ(P.insn[0].val[1], 8);
    CHECK_EQ(P.insn[1].val[0], 0xc000);
    CHECK_EQ(P.insn[2].val[0], 0xc000);
}

/* Strings and comments: escapes, quotes inside strings, and the three comment
 * markers — including a comment marker that appears inside a string literal and
 * must NOT start a comment. */
static void test_strings_and_comments(void) {
    std_policy();
    POL.trace = DVM_TRACE_IO;
    kcap_reset();
    CHECK_STATUS(run("print \"a ; not a comment # either // really\"\n"
                     "print \"quote:\\\" backslash:\\\\ done\"\n"
                     "print \"tab:\\there\"\n"
                     "halt   ; real comment \"with a quote\n"), DVM_OK);
    CHECK_EQ(R.prints, 3);
    CHECK_CONTAINS(kcap_text(), "a ; not a comment # either // really");
    CHECK_CONTAINS(kcap_text(), "quote:\" backslash:\\ done");
    CHECK_CONTAINS(kcap_text(), "tab:\\x09here");     /* trace.c escapes it */
    POL.trace = DVM_TRACE_OFF;

    /* A string ending in a literal backslash does not swallow the rest. */
    CHECK_EQ(assemble("print \"path\\\\\"\nhalt\n"), 0);
    CHECK_EQ(P.ninsn, 2);
    CHECK_EQ(P.insn[1].op, DVM_HALT);

    /* An abort message survives intact into the result. */
    CHECK_STATUS(run("abort \"reset bit 0x2c never cleared\"\n"), DVM_TRAP_ABORT);
    CHECK_STR(R.msg, "reset bit 0x2c never cleared");

    /* All three comment markers, mid-line and whole-line. */
    CHECK_EQ(assemble("; one\n# two\n// three\nmov r1, 1 ; x\nmov r2, 2 # y\n"
                      "mov r3, 3 // z\nhalt\n"), 0);
    CHECK_EQ(P.ninsn, 4);
}

static void test_bdf_literal(void) {
    CHECK_EQ(assemble("pcicfg r1, 00:03.0, 0\n"
                      "pcicfg r2, 00:1f.3, 4\n"
                      "pcicfg r3, ff:1f.7, 8\n"), 0);
    CHECK_EQ(P.insn[0].val[0], (0 << 8) | (3 << 3) | 0);
    CHECK_EQ(P.insn[1].val[0], (0 << 8) | (0x1f << 3) | 3);
    CHECK_EQ(P.insn[2].val[0], (0xffull << 8) | (0x1f << 3) | 7);
}

/* Precise, line-numbered refusals. This is the assembler's whole job when the
 * model gets it wrong, so every message shape is pinned. */
static void test_assembler_errors(void) {
    static const struct { const char *src; int line; const char *want; } bad[] = {
        { "halt\noutq 1,2\n",             2, "unknown instruction \"outq\"" },
        { "halt\noutq 1,2\n",             2, "out8" },                 /* the hint */
        { "inq r1, 1\n",                  1, "in8" },
        { "ldq r1, [r2]\n",               1, "ld8" },
        { "stq [r2], r1\n",               1, "st8" },
        { "mov r16, 1\n",                 1, "r16" },
        { "mov r16, 1\n",                 1, "does not exist" },
        { "mov r1, r99\n",                1, "does not exist" },
        { "mov r1\n",                     1, "takes 2 operands" },
        { "mov r1, 2, 3\n",               1, "takes 2 operands" },
        { "halt 1\n",                     1, "takes 0 operands" },
        { "add r1\n",                     1, "2 or 3 operands" },
        { "add r1,2,3,4\n",               1, "2 or 3 operands" },
        { "mov 5, r1\n",                  1, "destination must be a register" },
        { "\n\njmp nowhere\n",            3, "no label named \"nowhere\"" },
        { "print \"unterminated\n",       1, "unterminated string" },
        { "ld32 r1, [r2\n",               1, "unterminated '['" },
        { "L:\nL:\nhalt\n",               2, "already defined" },
        { "r1:\nhalt\n",                  1, "register name" },
        { "add:\nhalt\n",                 1, "instruction name" },
        { ".equ r1 5\n",                  1, "register name" },
        { ".equ FOO\n",                   1, ".equ takes a name and a value" },
        { ".equ FOO bar\n",               1, "expected a number" },
        { ".equ FOO 1\n.equ FOO 2\n",     2, "already defined" },
        { ".equ FOO 1\nFOO:\nhalt\n",     2, "already a .equ constant" },
        { "FOO:\n.equ FOO 1\nhalt\n",     2, "already a label" },
        { ".bogus 1\n",                   1, "unknown directive" },
        { "mov r1, 0x1ffffffffffffffff\n",1, "does not fit in 64 bits" },
        { "mov r1, hello\n",              1, "expected a register" },
        { "abort 5\n",                    1, "expected a quoted string" },
        { "print\n",                      1, "string and an optional value" },
        { "print \"a\", 1, 2\n",          1, "string and an optional value" },
        { "print \"a\\q\"\n",             1, "unknown escape" },
        { "ld32 r1, [r2+]\n",             1, "expected [base+displacement]" },
        { "ld32 r1, []\n",                1, "empty address" },
        { "ld32 r1, [r2+1], 4\n",         1, "not both" },
        { "st32 [r2+1], 4, r1\n",         1, "not both" },
        { "pcicfg r1, 0\n",               1, "takes 3 operands" },
        { "",                             0, "no instructions" },
        { "; only a comment\n",           0, "no instructions" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = dvm_assemble(bad[i].src, strlen(bad[i].src), &P, &E);
        th_checks++;
        if (rc == 0) {
            th_fails++;
            printf("    FAIL case %zu assembled but should not: %s\n", i, bad[i].src);
            continue;
        }
        CHECK_EQ(E.line, bad[i].line);
        CHECK_CONTAINS(E.msg, bad[i].want);
        /* A refused program is left empty, never half-built. */
        CHECK_EQ(P.ninsn, 0);
    }
}

static void test_assembler_limits(void) {
    static char big[DVM_SRC_MAX + 4096];

    /* too many instructions */
    int n = 0;
    for (int i = 0; i < DVM_MAX_INSNS + 1; i++) n += snprintf(big + n, sizeof big - n, "nop\n");
    CHECK_EQ(dvm_assemble(big, strlen(big), &P, &E), -1);
    CHECK_CONTAINS(E.msg, "program too long");
    CHECK_EQ(E.line, DVM_MAX_INSNS + 1);

    /* exactly at the limit is fine */
    n = 0;
    for (int i = 0; i < DVM_MAX_INSNS - 1; i++) n += snprintf(big + n, sizeof big - n, "nop\n");
    snprintf(big + n, sizeof big - n, "halt\n");
    CHECK_EQ(dvm_assemble(big, strlen(big), &P, &E), 0);
    CHECK_EQ(P.ninsn, DVM_MAX_INSNS);

    /* source larger than DVM_SRC_MAX */
    memset(big, 'x', sizeof big);
    CHECK_EQ(dvm_assemble(big, DVM_SRC_MAX + 1, &P, &E), -1);
    CHECK_CONTAINS(E.msg, "the limit is");

    /* line longer than DVM_LINE_MAX */
    n = snprintf(big, sizeof big, "halt\nmov r1, ");
    for (int i = 0; i < DVM_LINE_MAX; i++) big[n++] = '0';
    big[n] = '\0';
    CHECK_EQ(dvm_assemble(big, strlen(big), &P, &E), -1);
    CHECK_EQ(E.line, 2);
    CHECK_CONTAINS(E.msg, "the limit is");

    /* too many labels */
    n = 0;
    for (int i = 0; i < DVM_MAX_LABELS + 1; i++)
        n += snprintf(big + n, sizeof big - n, "l%d:\n", i);
    snprintf(big + n, sizeof big - n, "halt\n");
    CHECK_EQ(dvm_assemble(big, strlen(big), &P, &E), -1);
    CHECK_CONTAINS(E.msg, "too many labels");

    /* too many .equ */
    n = 0;
    for (int i = 0; i < DVM_MAX_EQU + 1; i++)
        n += snprintf(big + n, sizeof big - n, ".equ c%d %d\n", i, i);
    snprintf(big + n, sizeof big - n, "halt\n");
    CHECK_EQ(dvm_assemble(big, strlen(big), &P, &E), -1);
    CHECK_CONTAINS(E.msg, "too many .equ");

    /* too many strings */
    n = 0;
    for (int i = 0; i < DVM_MAX_STRINGS + 1; i++)
        n += snprintf(big + n, sizeof big - n, "print \"s\"\n");
    CHECK_EQ(dvm_assemble(big, strlen(big), &P, &E), -1);
    CHECK_CONTAINS(E.msg, "too many strings");

    /* string pool exhaustion */
    n = 0;
    for (int i = 0; i < 8; i++) {
        n += snprintf(big + n, sizeof big - n, "print \"");
        for (int k = 0; k < 100; k++) big[n++] = 'a';
        n += snprintf(big + n, sizeof big - n, "\"\n");
    }
    big[n] = '\0';
    CHECK_EQ(dvm_assemble(big, strlen(big), &P, &E), -1);
    CHECK_CONTAINS(E.msg, "string pool full");

    /* too many operands on one line */
    CHECK_EQ(dvm_assemble("mov 1 2 3 4 5 6 7 8 9", 21, &P, &E), -1);
    CHECK_CONTAINS(E.msg, "too many operands");

    /* label name too long */
    n = 0;
    for (int i = 0; i < DVM_LABEL_MAX + 4; i++) big[n++] = 'a';
    n += snprintf(big + n, sizeof big - n, ":\nhalt\n");
    CHECK_EQ(dvm_assemble(big, strlen(big), &P, &E), -1);
    CHECK_CONTAINS(E.msg, "too long");
}

/* The source arrives as a raw JSON span: no NUL, possibly embedded ones. */
static void test_source_span_not_terminated(void) {
    const char raw[] = "mov r1, 7\nhalt\nGARBAGE THAT IS NOT PART OF THE SPAN";
    CHECK_EQ(dvm_assemble(raw, 15, &P, &E), 0);       /* "mov r1, 7\nhalt\n" */
    CHECK_EQ(P.ninsn, 2);

    const char nul[] = "mov r1, 7\0 mov r1, 9\nhalt\n";
    CHECK_EQ(dvm_assemble(nul, sizeof nul - 1, &P, &E), 0);
    CHECK_EQ(P.ninsn, 2);                              /* the NUL ends line 1 */
    CHECK_EQ(P.insn[0].val[0], 7);

    /* An empty span is a legible refusal, not a crash. */
    CHECK_EQ(dvm_assemble("", 0, &P, &E), -1);
    CHECK_EQ(dvm_assemble(NULL, 0, &P, &E), -1);
    CHECK_EQ(dvm_assemble(NULL, 10, &P, &E), -1);
    CHECK_EQ(dvm_assemble("halt\n", 5, NULL, &E), -1);
}

/* ====================================================================== */
/* 2. opcode semantics                                                    */
/* ====================================================================== */

static void test_alu(void) {
    std_policy();
    CHECK_STATUS(run("mov r1, 20\n"
                     "mov r2, 6\n"
                     "add r3, r1, r2\n"
                     "sub r4, r1, r2\n"
                     "mul r5, r1, r2\n"
                     "div r6, r1, r2\n"
                     "mod r7, r1, r2\n"
                     "and r8, r1, r2\n"
                     "or  r9, r1, r2\n"
                     "xor r10, r1, r2\n"
                     "not r11, r2\n"
                     "shl r12, r1, r2\n"
                     "shr r13, r1, 2\n"
                     "mov r14, r1\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[3], 26);
    CHECK_EQ(R.reg[4], 14);
    CHECK_EQ(R.reg[5], 120);
    CHECK_EQ(R.reg[6], 3);
    CHECK_EQ(R.reg[7], 2);
    CHECK_EQ(R.reg[8], 20 & 6);
    CHECK_EQ(R.reg[9], 20 | 6);
    CHECK_EQ(R.reg[10], 20 ^ 6);
    CHECK_EQ(R.reg[11], ~(uint64_t)6);
    CHECK_EQ(R.reg[12], 20ull << 6);
    CHECK_EQ(R.reg[13], 5);
    CHECK_EQ(R.reg[14], 20);
    CHECK_EQ(R.steps, 15);
    CHECK_EQ(R.io_ops, 0);
}

static void test_alu_edges(void) {
    std_policy();
    /* 64-bit wraparound is defined, not undefined. */
    CHECK_STATUS(run("mov r1, -1\nadd r2, r1, 1\nsub r3, 0, 1\nmul r4, r1, 2\nhalt\n"),
                 DVM_OK);
    CHECK_EQ(R.reg[2], 0);
    CHECK_EQ(R.reg[3], (uint64_t)-1);
    CHECK_EQ(R.reg[4], (uint64_t)-2);

    /* shift counts are masked to 63, so a huge shift is defined behaviour */
    CHECK_STATUS(run("mov r1, 1\nshl r2, r1, 64\nshl r3, r1, 65\nshr r4, r1, 200\nhalt\n"),
                 DVM_OK);
    CHECK_EQ(R.reg[2], 1);          /* 64 & 63 == 0 */
    CHECK_EQ(R.reg[3], 2);
    CHECK_EQ(R.reg[4], 1ull >> (200 & 63));

    /* two-operand shorthand */
    CHECK_STATUS(run("mov r1, 10\nadd r1, 5\nsub r1, 3\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 12);

    CHECK_STATUS(run("mov r1, 1\ndiv r2, r1, 0\nhalt\n"), DVM_TRAP_DIV0);
    CHECK_EQ(R.pc, 1);
    CHECK_EQ(R.line, 2);
    CHECK_CONTAINS(R.msg, "div by zero");

    CHECK_STATUS(run("mov r1, 1\nmod r2, r1, 0\nhalt\n"), DVM_TRAP_DIV0);
    CHECK_CONTAINS(R.msg, "mod by zero");
}

static void test_args_and_registers(void) {
    std_policy();
    uint64_t args[3] = { 0xFEBC0000ull, 7, 9 };
    CHECK_STATUS(run_args("add r3, r1, r2\nhalt\n", args, 3), DVM_OK);
    CHECK_EQ(R.reg[0], 0xFEBC0000ull);
    CHECK_EQ(R.reg[1], 7);
    CHECK_EQ(R.reg[2], 9);
    CHECK_EQ(R.reg[3], 16);
    /* registers past nargs start at zero */
    CHECK_EQ(R.reg[15], 0);

    /* more args than registers is clamped, not an overflow */
    uint64_t many[64];
    for (int i = 0; i < 64; i++) many[i] = (uint64_t)i;
    CHECK_STATUS(run_args("halt\n", many, 64), DVM_OK);
    CHECK_EQ(R.reg[15], 15);
    /* a negative count is treated as none */
    CHECK_STATUS(run_args("halt\n", many, -5), DVM_OK);
    CHECK_EQ(R.reg[0], 0);
    /* a NULL vector with a positive count does not read it */
    CHECK_STATUS(run_args("halt\n", NULL, 8), DVM_OK);
    CHECK_EQ(R.reg[0], 0);
}

static void test_cmp_and_branches(void) {
    std_policy();
    /* the six branches, for a<b, a==b and a>b, exhaustively */
    static const struct { const char *br; int lt, eq, gt; } t[] = {
        { "beq", 0, 1, 0 },
        { "bne", 1, 0, 1 },
        { "blt", 1, 0, 0 },
        { "ble", 1, 1, 0 },
        { "bgt", 0, 0, 1 },
        { "bge", 0, 1, 1 },
    };
    static const struct { const char *a, *b; int which; } v[] = {
        { "1", "2", 0 }, { "2", "2", 1 }, { "3", "2", 2 },
    };
    char src[256];
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        for (size_t k = 0; k < sizeof v / sizeof v[0]; k++) {
            snprintf(src, sizeof src,
                     "cmp %s, %s\n%s taken\nmov r1, 0\nhalt\ntaken:\nmov r1, 1\nhalt\n",
                     v[k].a, v[k].b, t[i].br);
            CHECK_STATUS(run(src), DVM_OK);
            int want = v[k].which == 0 ? t[i].lt : v[k].which == 1 ? t[i].eq : t[i].gt;
            th_checks++;
            if ((int)R.reg[1] != want) {
                th_fails++;
                printf("    FAIL %s with %s,%s: got %d want %d\n",
                       t[i].br, v[k].a, v[k].b, (int)R.reg[1], want);
            }
        }

    /* comparisons are UNSIGNED: -1 is the largest value, not the smallest */
    CHECK_STATUS(run("mov r1, -1\ncmp r1, 1\nbgt big\nmov r2, 0\nhalt\n"
                     "big:\nmov r2, 1\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 1);

    /* jmp is unconditional even with flags set the other way */
    CHECK_STATUS(run("cmp 1, 1\njmp there\nmov r1, 5\nhalt\nthere:\nmov r1, 6\nhalt\n"),
                 DVM_OK);
    CHECK_EQ(R.reg[1], 6);
}

static void test_test_op(void) {
    std_policy();
    CHECK_STATUS(run("mov r1, 0x0f\n"
                     "test r1, 0x08\nbeq zero1\nmov r2, 1\njmp n\nzero1:\nmov r2, 0\n"
                     "n:\ntest r1, 0x10\nbeq zero2\nmov r3, 1\nhalt\nzero2:\nmov r3, 0\nhalt\n"),
                 DVM_OK);
    CHECK_EQ(R.reg[2], 1);      /* bit set     -> not zero -> bne path */
    CHECK_EQ(R.reg[3], 0);      /* bit clear   -> zero              */
}

static void test_stacks(void) {
    std_policy();
    CHECK_STATUS(run("push 1\npush 2\npush 3\npop r1\npop r2\npop r3\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 3);
    CHECK_EQ(R.reg[2], 2);
    CHECK_EQ(R.reg[3], 1);

    CHECK_STATUS(run("pop r1\nhalt\n"), DVM_TRAP_STACK);
    CHECK_CONTAINS(R.msg, "empty data stack");

    char src[1024];
    int n = 0;
    for (int i = 0; i < DVM_DSTACK + 1; i++) n += snprintf(src + n, sizeof src - n, "push 1\n");
    snprintf(src + n, sizeof src - n, "halt\n");
    CHECK_STATUS(run(src), DVM_TRAP_STACK);
    CHECK_CONTAINS(R.msg, "data stack overflow");

    /* call/ret */
    CHECK_STATUS(run("mov r1, 0\ncall bump\ncall bump\nhalt\n"
                     "bump:\nadd r1, 1\nret\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 2);

    CHECK_STATUS(run("ret\n"), DVM_TRAP_STACK);
    CHECK_CONTAINS(R.msg, "empty call stack");

    /* unbounded recursion stops at the call-stack depth */
    CHECK_STATUS(run("call deep\nhalt\ndeep:\ncall deep\nret\n"), DVM_TRAP_STACK);
    CHECK_CONTAINS(R.msg, "call stack overflow");
    CHECK(R.steps <= DVM_CSTACK + 2);
}

static void test_halt_abort_nop(void) {
    std_policy();
    CHECK_STATUS(run("nop\nnop\nhalt\nmov r1, 9\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0);            /* nothing after halt runs */
    CHECK_EQ(R.steps, 3);
    CHECK_STR(R.msg, "");

    CHECK_STATUS(run("mov r1, 1\nabort \"codec never came out of reset\"\nhalt\n"),
                 DVM_TRAP_ABORT);
    CHECK_STR(R.msg, "codec never came out of reset");
    CHECK_EQ(R.pc, 1);
    CHECK_EQ(R.line, 2);
    CHECK_EQ(R.reg[1], 1);            /* state up to the abort is reported */

    /* control characters in an abort message never reach the caller raw */
    CHECK_STATUS(run("abort \"a\\nb\\tc\"\n"), DVM_TRAP_ABORT);
    CHECK_STR(R.msg, "a?b?c");

    /* running off the end without halt is a trap, not a wander */
    CHECK_STATUS(run("mov r1, 1\nmov r2, 2\n"), DVM_TRAP_PC);
    CHECK_CONTAINS(R.msg, "ran past the last instruction");
}

/* ====================================================================== */
/* 3. device access — exact sequences                                     */
/* ====================================================================== */

static void test_port_io(void) {
    std_policy();
    CHECK_STATUS(run("out8  0xc000, 0x12\n"
                     "out16 0xc002, 0x3456\n"
                     "out32 0xc004, 0x89abcdef\n"
                     "in8   r1, 0xc000\n"
                     "in16  r2, 0xc002\n"
                     "in32  r3, 0xc004\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(nalog, 6);
    chk_acc(0, A_PW, 0xc000, 1, 0x12);
    chk_acc(1, A_PW, 0xc002, 2, 0x3456);
    chk_acc(2, A_PW, 0xc004, 4, 0x89abcdef);
    chk_acc(3, A_PR, 0xc000, 1, 0x12);
    chk_acc(4, A_PR, 0xc002, 2, 0x3456);
    chk_acc(5, A_PR, 0xc004, 4, 0x89abcdef);
    CHECK_EQ(R.reg[1], 0x12);
    CHECK_EQ(R.reg[2], 0x3456);
    CHECK_EQ(R.reg[3], 0x89abcdef);
    CHECK_EQ(R.io_ops, 6);
    CHECK_EQ(R.n_port_wr, 3);
    CHECK_EQ(R.n_port_rd, 3);

    /* values wider than the access are truncated the way hardware truncates */
    CHECK_STATUS(run("out8 0xc010, 0x1ff\nout16 0xc012, 0x1ffff\nhalt\n"), DVM_OK);
    chk_acc(0, A_PW, 0xc010, 1, 0xff);
    chk_acc(1, A_PW, 0xc012, 2, 0xffff);

    /* a register can supply the port number */
    CHECK_STATUS(run("mov r1, 0xc020\nout8 r1, 0x5a\nin8 r2, r1\nhalt\n"), DVM_OK);
    chk_acc(0, A_PW, 0xc020, 1, 0x5a);
    chk_acc(1, A_PR, 0xc020, 1, 0x5a);
    CHECK_EQ(R.reg[2], 0x5a);
}

static void test_mmio(void) {
    std_policy();
    uint64_t bar[1] = { MMIO_BASE };
    CHECK_STATUS(run_args("st32 [r0+0x40], 0xdeadbeef\n"
                          "ld32 r1, [r0+0x40]\n"
                          "st16 [r0+0x50], 0x1234\n"
                          "ld16 r2, [r0+0x50]\n"
                          "st8  [r0+0x60], 0x99\n"
                          "ld8  r3, [r0+0x60]\n"
                          "halt\n", bar, 1), DVM_OK);
    chk_acc(0, A_MW, MMIO_BASE + 0x40, 4, 0xdeadbeef);
    chk_acc(1, A_MR, MMIO_BASE + 0x40, 4, 0xdeadbeef);
    chk_acc(2, A_MW, MMIO_BASE + 0x50, 2, 0x1234);
    chk_acc(3, A_MR, MMIO_BASE + 0x50, 2, 0x1234);
    chk_acc(4, A_MW, MMIO_BASE + 0x60, 1, 0x99);
    chk_acc(5, A_MR, MMIO_BASE + 0x60, 1, 0x99);
    CHECK_EQ(R.reg[1], 0xdeadbeef);
    CHECK_EQ(R.reg[2], 0x1234);
    CHECK_EQ(R.reg[3], 0x99);
    CHECK_EQ(R.n_mmio_wr, 3);
    CHECK_EQ(R.n_mmio_rd, 3);

    /* an absolute address works too */
    CHECK_STATUS(run("st32 [0xfebc0080], 1\nld32 r1, [0xfebc0080]\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 1);

    /* the read-only window reads fine and refuses a write */
    CHECK_STATUS(run("ld8 r1, [0xfebd0000]\nhalt\n"), DVM_OK);
    CHECK_STATUS(run("st8 [0xfebd0000], 1\nhalt\n"), DVM_TRAP_MMIO_DENIED);
    CHECK_CONTAINS(R.msg, "read-only");
    CHECK_EQ(R.n_mmio_wr, 0);
}

static void test_pcicfg(void) {
    std_policy();
    pcicfg[3][0]    = 0x100E8086;
    pcicfg[3][0x10 / 4] = 0xFEBC0000;
    pcicfg[4][0]    = 0x24158086;

    CHECK_STATUS(run("pcicfg r1, 00:03.0, 0x00\n"
                     "pcicfg r2, 00:03.0, 0x10\n"
                     "pcicfg r3, 00:04.1, 0x00\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0x100E8086);
    CHECK_EQ(R.reg[2], 0xFEBC0000);
    CHECK_EQ(R.reg[3], 0x24158086 + 1);      /* the model folds fn into the value */
    CHECK_EQ(R.n_pci_rd, 3);
    chk_acc(0, A_PCI, (3 << 3) | 0, 4, 0x100E8086);

    /* a numeric bdf is the same thing */
    CHECK_STATUS(run("pcicfg r1, 0x18, 0\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0x100E8086);

    /* refusals */
    CHECK_STATUS(run("pcicfg r1, 00:05.0, 0\nhalt\n"), DVM_TRAP_PCI_DENIED);
    CHECK_CONTAINS(R.msg, "00:05.0");
    CHECK_STATUS(run("pcicfg r1, 00:03.0, 2\nhalt\n"), DVM_TRAP_ALIGN);
    CHECK_CONTAINS(R.msg, "4-byte aligned");
    CHECK_STATUS(run("pcicfg r1, 00:03.0, 0x100\nhalt\n"), DVM_TRAP_RANGE);
    CHECK_CONTAINS(R.msg, "256-byte config space");
    CHECK_STATUS(run("pcicfg r1, 0x10000, 0\nhalt\n"), DVM_TRAP_RANGE);
    CHECK_CONTAINS(R.msg, "bdf");
    CHECK_EQ(nalog, 0);                     /* none of those reached the device */
}

static void test_delay(void) {
    std_policy();
    CHECK_STATUS(run("delay 100\ndelay 250\nhalt\n"), DVM_OK);
    CHECK_EQ(R.delay_us, 350);
    CHECK_EQ(delay_total, 350);
    chk_acc(0, A_DLY, 0, 0, 100);
    chk_acc(1, A_DLY, 0, 0, 250);

    /* one delay larger than the single-delay cap */
    CHECK_STATUS(run("delay 60000\nhalt\n"), DVM_TRAP_DELAY_BUDGET);
    CHECK_CONTAINS(R.msg, "single-delay limit");
    CHECK_EQ(delay_total, 0);

    /* cumulative budget */
    POL.max_delay_us = 1000;
    CHECK_STATUS(run("delay 400\ndelay 400\ndelay 400\nhalt\n"), DVM_TRAP_DELAY_BUDGET);
    CHECK_CONTAINS(R.msg, "delay budget exhausted");
    CHECK_EQ(R.delay_us, 800);
    CHECK_EQ(delay_total, 800);
}

static void test_print(void) {
    std_policy();
    POL.trace = DVM_TRACE_IO;
    kcap_reset();
    CHECK_STATUS(run("mov r1, 0x2a\nprint \"status\", r1\nprint \"done\"\nhalt\n"), DVM_OK);
    CHECK_EQ(R.prints, 2);
    CHECK_CONTAINS(kcap_text(), "[dvm.print status = 0x2a -> ok]");
    CHECK_CONTAINS(kcap_text(), "[dvm.print done -> ok]");

    POL.max_prints = 1;
    CHECK_STATUS(run("print \"a\"\nprint \"b\"\nhalt\n"), DVM_TRAP_PRINT_BUDGET);
    CHECK_EQ(R.prints, 1);
    POL.trace = DVM_TRACE_OFF;
}

/* The reason this VM exists: poll a status bit until it clears, with a bounded
 * retry count, then program the device. Asserted access-by-access. */
static void test_realistic_bringup(void) {
    std_policy();
    uint64_t bar[1] = { MMIO_BASE };
    const char *src =
        ".equ STATUS 0x10\n"
        ".equ LATCH  0x20\n"
        ".equ BUSY   0x01\n"
        "  st32 [r0+LATCH], 0        ; arm the device\n"
        "  mov r5, 8                 ; retry budget\n"
        "wait:\n"
        "  ld32 r1, [r0+STATUS]\n"
        "  test r1, BUSY\n"
        "  beq ready\n"
        "  delay 50\n"
        "  sub r5, 1\n"
        "  cmp r5, 0\n"
        "  bne wait\n"
        "  abort \"device stayed busy\"\n"
        "ready:\n"
        "  print \"ready after retries left\", r5\n"
        "  st32 [r0+LATCH], 0xcafe\n"
        "  ld32 r2, [r0+LATCH]\n"
        "  halt\n";
    CHECK_STATUS(run_args(src, bar, 1), DVM_OK);
    CHECK_EQ(R.reg[2], 0xcafe);
    CHECK_EQ(R.reg[5], 5);                 /* three busy reads consumed */
    CHECK_EQ(R.n_mmio_rd, 5);              /* 4 status + 1 latch readback */
    CHECK_EQ(R.n_mmio_wr, 2);
    CHECK_EQ(R.delay_us, 150);

    chk_acc(0, A_MW, MMIO_BASE + 0x20, 4, 0);
    chk_acc(1, A_MR, MMIO_BASE + 0x10, 4, 1);
    chk_acc(2, A_DLY, 0, 0, 50);
    chk_acc(3, A_MR, MMIO_BASE + 0x10, 4, 1);
    chk_acc(4, A_DLY, 0, 0, 50);
    chk_acc(5, A_MR, MMIO_BASE + 0x10, 4, 1);
    chk_acc(6, A_DLY, 0, 0, 50);
    chk_acc(7, A_MR, MMIO_BASE + 0x10, 4, 0);
    chk_acc(8, A_MW, MMIO_BASE + 0x20, 4, 0xcafe);
    chk_acc(9, A_MR, MMIO_BASE + 0x20, 4, 0xcafe);
    CHECK_EQ(nalog, 10);

    /* Same program against a device that never becomes ready: it gives up on
     * its own retry budget with a legible message, and touches nothing else. */
    dev_reset();
    always_busy = 1;                      /* the device never comes ready */
    dvm_status_t s = dvm_run(&P, &POL, &DEV, bar, 1, &R);
    CHECK_STATUS(s, DVM_TRAP_ABORT);
    CHECK_STR(R.msg, "device stayed busy");
    CHECK_EQ(R.delay_us, 400);
}

/* ====================================================================== */
/* 4. containment                                                         */
/* ====================================================================== */

static void test_infinite_loop_terminates(void) {
    std_policy();
    POL.max_steps = 5000;
    CHECK_STATUS(run("loop:\njmp loop\n"), DVM_TRAP_STEPS);
    CHECK_EQ(R.steps, 5000);
    CHECK_CONTAINS(R.msg, "instruction budget of 5000 exhausted");

    /* a loop that hammers a device stops on the I/O budget first */
    POL.max_steps = 100000;
    POL.max_io = 10;
    CHECK_STATUS(run("loop:\nin8 r1, 0xc000\njmp loop\n"), DVM_TRAP_IO_BUDGET);
    CHECK_EQ(R.io_ops, 10);
    CHECK_EQ(nalog, 10);

    /* a loop that only delays stops on the delay budget */
    POL.max_io = 4096;
    POL.max_delay_us = 5000;
    CHECK_STATUS(run("loop:\ndelay 1000\njmp loop\n"), DVM_TRAP_DELAY_BUDGET);
    CHECK_EQ(R.delay_us, 5000);

    /* and a busy loop with no I/O at all still stops */
    POL.max_delay_us = 200000;
    POL.max_steps = 777;
    CHECK_STATUS(run("mov r1, 0\nloop:\nadd r1, 1\njmp loop\n"), DVM_TRAP_STEPS);
    CHECK_EQ(R.steps, 777);
}

static void test_denied_ports(void) {
    std_policy();
    /* Every entry in the compiled-in deny list, refused even though the test
     * also tries to allowlist it (the policy itself is then refused). */
    static const struct { unsigned port; const char *why; } deny[] = {
        { 0x20, "PIC" }, { 0x21, "PIC" }, { 0x40, "PIT" }, { 0x43, "PIT" },
        { 0x60, "PS/2" }, { 0x64, "PS/2" }, { 0x70, "CMOS" }, { 0x92, "reset" },
        { 0xa0, "PIC" }, { 0x2f8, "COM2" }, { 0x3f8, "COM1" },
        { 0xcf8, "PCI config" }, { 0xcfc, "PCI config" },
        /* A window derived from a BAR is bounded by alignment and a cap, not by
         * the BAR's real size, so it can be wider than the device. These are the
         * fixed legacy ranges an over-wide window has something to lose by
         * reaching: they can write a disk, bus-master into RAM, or re-mode the
         * console at its legacy address. None of them is ever a PCI BAR. */
        { 0x00, "ISA DMA" }, { 0x0f, "ISA DMA" }, { 0x80, "DMA page" },
        { 0x8f, "DMA page" }, { 0xc0, "ISA DMA" }, { 0xdf, "ISA DMA" },
        { 0x170, "secondary ATA" }, { 0x177, "secondary ATA" },
        { 0x1f0, "primary ATA" }, { 0x1f7, "primary ATA" },
        { 0x3b0, "legacy VGA" }, { 0x3c0, "legacy VGA" }, { 0x3d4, "legacy VGA" },
        { 0x3df, "legacy VGA" }, { 0x3f0, "floppy" }, { 0x3f7, "floppy" },
    };
    char src[128];
    for (size_t i = 0; i < sizeof deny / sizeof deny[0]; i++) {
        snprintf(src, sizeof src, "out8 0x%x, 0xfe\nhalt\n", deny[i].port);
        CHECK_STATUS(run(src), DVM_TRAP_PORT_DENIED);
        CHECK_CONTAINS(R.msg, "never accessible");
        snprintf(src, sizeof src, "in8 r1, 0x%x\nhalt\n", deny[i].port);
        CHECK_STATUS(run(src), DVM_TRAP_PORT_DENIED);
        CHECK_EQ(nalog, 0);
    }
    CHECK_EQ(forbidden, 0);

    /* A port that is merely not granted. */
    CHECK_STATUS(run("out8 0x300, 1\nhalt\n"), DVM_TRAP_PORT_DENIED);
    CHECK_CONTAINS(R.msg, "not in this program's allowed ranges");
    CHECK_CONTAINS(R.msg, "0xc000-0xc03f");

    /* A FULL port table must be reported in full. The refusal message is what
     * the model reads to work out what it IS allowed to touch, and the buffer
     * that renders the list used to be 96 bytes — six of eight ranges — so the
     * last two vanished with no sign that anything had been dropped. */
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    for (int i = 0; i < DVM_MAX_PORT_RANGES; i++)
        CHECK_EQ(dvm_policy_allow_ports(&POL, (uint16_t)(0xc000 + i * 0x40),
                                        (uint16_t)(0xc03f + i * 0x40)), 0);
    CHECK_STATUS(run("out8 0x300, 1\nhalt\n"), DVM_TRAP_PORT_DENIED);
    for (int i = 0; i < DVM_MAX_PORT_RANGES; i++) {
        char want[16];
        snprintf(want, sizeof want, "0x%04x-0x%04x", 0xc000 + i * 0x40,
                 0xc03f + i * 0x40);
        CHECK_CONTAINS(R.msg, want);
    }
    std_policy();

    /* Just outside the granted range, on both sides. */
    CHECK_STATUS(run("in8 r1, 0xbfff\nhalt\n"), DVM_TRAP_PORT_DENIED);
    CHECK_STATUS(run("in8 r1, 0xc040\nhalt\n"), DVM_TRAP_PORT_DENIED);
    CHECK_STATUS(run("in8 r1, 0xc03f\nhalt\n"), DVM_OK);

    /* A wide access that starts inside the range but runs off its end. */
    CHECK_STATUS(run("in32 r1, 0xc03e\nhalt\n"), DVM_TRAP_PORT_DENIED);
    CHECK_STATUS(run("in32 r1, 0xc03c\nhalt\n"), DVM_OK);

    /* A port number that is not a port number at all. */
    CHECK_STATUS(run("in8 r1, 0x10000\nhalt\n"), DVM_TRAP_RANGE);
    CHECK_CONTAINS(R.msg, "0x0000-0xffff");
    CHECK_STATUS(run("in32 r1, 0xffff\nhalt\n"), DVM_TRAP_RANGE);
    CHECK_STATUS(run("mov r1, -1\nin8 r2, r1\nhalt\n"), DVM_TRAP_RANGE);

    /* With no port range granted at all, nothing is reachable. */
    dvm_policy_init(&POL);
    POL.trace = DVM_TRACE_OFF;
    CHECK_STATUS(run("in8 r1, 0xc000\nhalt\n"), DVM_TRAP_PORT_DENIED);
    CHECK_CONTAINS(R.msg, "(none)");
}

static void test_denied_mmio(void) {
    std_policy();
    /* The framebuffer and low memory: refused before any window lookup. */
    static const uint64_t low[] = { 0x0, 0x1000, 0xb8000, 0x100000, 0xfffff0 };
    char src[128];
    for (size_t i = 0; i < sizeof low / sizeof low[0]; i++) {
        snprintf(src, sizeof src, "st32 [0x%llx], 0x41414141\nhalt\n",
                 (unsigned long long)low[i]);
        CHECK_STATUS(run(src), DVM_TRAP_MMIO_DENIED);
        CHECK_CONTAINS(R.msg, "kernel memory");
        snprintf(src, sizeof src, "ld8 r1, [0x%llx]\nhalt\n", (unsigned long long)low[i]);
        CHECK_STATUS(run(src), DVM_TRAP_MMIO_DENIED);
    }

    /* Above the mapped 4 GiB. */
    CHECK_STATUS(run("mov r1, 1\nshl r1, r1, 32\nld32 r2, [r1]\nhalt\n"), DVM_TRAP_RANGE);
    CHECK_CONTAINS(R.msg, "4 GiB");
    CHECK_STATUS(run("mov r1, -4\nld32 r2, [r1]\nhalt\n"), DVM_TRAP_RANGE);

    /* Outside every granted window, on both sides and just past the end. */
    CHECK_STATUS(run("ld32 r1, [0xfebbfffc]\nhalt\n"), DVM_TRAP_MMIO_DENIED);
    CHECK_CONTAINS(R.msg, "not inside any allowed window");
    CHECK_STATUS(run("ld32 r1, [0xfebc1000]\nhalt\n"), DVM_TRAP_MMIO_DENIED);
    CHECK_STATUS(run("ld32 r1, [0xfebc0ffc]\nhalt\n"), DVM_OK);
    CHECK_STATUS(run("ld32 r1, [0xfebc0ffe]\nhalt\n"), DVM_TRAP_MMIO_DENIED);  /* spans */

    /* Alignment. */
    CHECK_STATUS(run("ld32 r1, [0xfebc0002]\nhalt\n"), DVM_TRAP_ALIGN);
    CHECK_CONTAINS(R.msg, "aligned");
    CHECK_STATUS(run("ld16 r1, [0xfebc0001]\nhalt\n"), DVM_TRAP_ALIGN);
    CHECK_STATUS(run("st32 [0xfebc0001], 1\nhalt\n"), DVM_TRAP_ALIGN);
    CHECK_STATUS(run("ld8 r1, [0xfebc0001]\nhalt\n"), DVM_OK);   /* bytes: any */

    /* base+disp is computed then checked, so neither half can smuggle. */
    CHECK_STATUS(run("mov r1, 0xfebc0000\nld32 r2, [r1+0x2000]\nhalt\n"),
                 DVM_TRAP_MMIO_DENIED);
    CHECK_STATUS(run("mov r1, 0xb0000\nld32 r2, [r1+0x8000]\nhalt\n"),
                 DVM_TRAP_MMIO_DENIED);     /* would be 0xb8000 */

    /* The platform block: IOAPIC, HPET, local APIC. Nothing here is a BAR, so
     * refusing it costs nothing, and a caller that over-granted a window cannot
     * hand out interrupt routing by accident. Every edge of the range. */
    static const uint64_t plat[] = {
        0xfec00000ull,        /* IOAPIC, first byte           */
        0xfec00010ull, 0xfed00000ull /* HPET */, 0xfee00000ull /* LAPIC */,
        0xfeefffffull - 3,    /* last dword in the block      */
    };
    for (size_t i = 0; i < sizeof plat / sizeof plat[0]; i++) {
        snprintf(src, sizeof src, "ld32 r1, [0x%llx]\nhalt\n",
                 (unsigned long long)plat[i]);
        CHECK_STATUS(run(src), DVM_TRAP_MMIO_DENIED);
        CHECK_CONTAINS(R.msg, "platform block");
        snprintf(src, sizeof src, "st32 [0x%llx], 1\nhalt\n",
                 (unsigned long long)plat[i]);
        CHECK_STATUS(run(src), DVM_TRAP_MMIO_DENIED);
    }
    /* ...and the byte either side of it is only "not granted", not "denied by
     * the block" — the range must be exactly [0xfec00000, 0xfef00000). */
    CHECK_STATUS(run("ld32 r1, [0xfebffffc]\nhalt\n"), DVM_TRAP_MMIO_DENIED);
    CHECK_CONTAINS(R.msg, "not inside any allowed window");
    CHECK_STATUS(run("ld32 r1, [0xfef00000]\nhalt\n"), DVM_TRAP_MMIO_DENIED);
    CHECK_CONTAINS(R.msg, "not inside any allowed window");
    /* An access that merely SPANS into the block is caught by the block rule,
     * not left to the alignment check further down. */
    CHECK_STATUS(run("ld32 r1, [0xfebffffe]\nhalt\n"), DVM_TRAP_MMIO_DENIED);
    CHECK_CONTAINS(R.msg, "platform block");
    CHECK_EQ(forbidden, 0);
}

static void test_policy_refusals(void) {
    char msg[192];

    /* granting a denied port range is refused at policy time */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_ports(&POL, 0x60, 0x6f), 0);
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "never allowed");
    CHECK_CONTAINS(msg, "PS/2");
    /* and the run refuses too, before a single instruction executes */
    CHECK_EQ(assemble("halt\n"), 0);
    CHECK_STATUS(dvm_run(&P, &POL, &DEV, NULL, 0, &R), DVM_TRAP_POLICY);
    CHECK_EQ(R.steps, 0);

    /* granting a window over the platform block is refused at policy time, so
     * no caller of dvm_policy_allow_mmio() can hand out the IOAPIC even by
     * mistake — the same rule mmio_gate() enforces per access. */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, 0xfec00000ull, 0x1000, DVM_MMIO_RW), 0);
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "platform block");
    /* a window that merely overlaps its bottom edge, too */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, 0xfebf0000ull, 0x20000, DVM_MMIO_RW), 0);
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "platform block");
    /* ...but one that stops exactly at it is fine */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, 0xfebf0000ull, 0x10000, DVM_MMIO_RW), 0);
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_OK);

    /* a range that merely straddles the deny list is refused as well */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_ports(&POL, 0x0000, 0xffff), 0);
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);

    /* MMIO windows in kernel memory or past 4 GiB */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, 0xb8000, 0x1000, DVM_MMIO_RW), 0);
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "kernel memory");

    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, 0xFFFFF000ull, 0x2000, DVM_MMIO_RW), 0);
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "4 GiB");

    /* malformed grants are refused by the helper itself */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_ports(&POL, 0x200, 0x100), -1);      /* inverted   */
    CHECK_EQ(POL.nport, 0);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, MMIO_BASE, 0, DVM_MMIO_RW), -1);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, MMIO_BASE, 0x10, 0), -1); /* no access  */
    CHECK_EQ(dvm_policy_allow_mmio(&POL, MMIO_BASE, 0x10, 0x80), -1);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, 0xFFFFFFFFFFFFFF00ull, 0x200, DVM_MMIO_RW), -1);
    CHECK_EQ(POL.nmmio, 0);
    CHECK_EQ(dvm_policy_allow_pci(&POL, 0, 32, 0), -1);
    CHECK_EQ(dvm_policy_allow_pci(&POL, 0, 0, 8), -1);
    CHECK_EQ(POL.npci, 0);
    CHECK_EQ(dvm_policy_allow_ports(NULL, 0, 1), -1);
    CHECK_EQ(dvm_policy_allow_mmio(NULL, MMIO_BASE, 1, DVM_MMIO_R), -1);
    CHECK_EQ(dvm_policy_allow_pci(NULL, 0, 0, 0), -1);

    /* the tables are finite */
    dvm_policy_init(&POL);
    for (int i = 0; i < DVM_MAX_PORT_RANGES; i++)
        CHECK_EQ(dvm_policy_allow_ports(&POL, (uint16_t)(0x1000 + i * 16),
                                        (uint16_t)(0x100f + i * 16)), 0);
    CHECK_EQ(dvm_policy_allow_ports(&POL, 0x9000, 0x9001), -1);
    for (int i = 0; i < DVM_MAX_MMIO_WINS; i++)
        CHECK_EQ(dvm_policy_allow_mmio(&POL, MMIO_BASE + i * 0x10000, 0x1000,
                                       DVM_MMIO_RW), 0);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, 0xFE000000, 0x1000, DVM_MMIO_RW), -1);
    for (int i = 0; i < DVM_MAX_PCI_FNS; i++)
        CHECK_EQ(dvm_policy_allow_pci(&POL, 0, (uint8_t)i, 0), 0);
    CHECK_EQ(dvm_policy_allow_pci(&POL, 0, 30, 0), -1);

    /* budgets above their ceiling */
    struct { const char *what; uint64_t *field; uint64_t bad; } b64[] = { { 0, 0, 0 } };
    (void)b64;
    dvm_policy_init(&POL);
    POL.max_steps = 0;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "max_steps");
    dvm_policy_init(&POL); POL.max_steps = 50000000ull;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    dvm_policy_init(&POL); POL.max_io = 100000000ull;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "max_io");
    dvm_policy_init(&POL); POL.max_delay_us = 100000000ull;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    dvm_policy_init(&POL); POL.max_single_delay_us = 100000000u;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    dvm_policy_init(&POL); POL.max_prints = 100000u;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    dvm_policy_init(&POL); POL.max_trace = 100000000u;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    dvm_policy_init(&POL); POL.trace = (dvm_trace_t)9;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);

    /* hand-corrupted counts */
    dvm_policy_init(&POL); POL.nport = 99;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    dvm_policy_init(&POL); POL.nmmio = -1;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    dvm_policy_init(&POL); POL.npci = 99;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    /* a window whose flags were corrupted after the helper accepted it */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_mmio(&POL, MMIO_BASE, 0x100, DVM_MMIO_RW), 0);
    POL.mmio[0].flags = 0;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    POL.mmio[0].flags = DVM_MMIO_RW; POL.mmio[0].size = 0;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);

    /* a default policy grants nothing but is otherwise valid */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_OK);
    CHECK_EQ(dvm_policy_check(NULL, msg, sizeof msg), DVM_TRAP_POLICY);
    /* and a NULL message buffer is tolerated */
    CHECK_EQ(dvm_policy_check(&POL, NULL, 0), DVM_OK);
}

static void test_bad_arguments(void) {
    std_policy();
    CHECK_EQ(assemble("halt\n"), 0);
    CHECK_STATUS(dvm_run(NULL, &POL, &DEV, NULL, 0, &R), DVM_TRAP_POLICY);
    CHECK_CONTAINS(R.msg, "no program");
    CHECK_STATUS(dvm_run(&P, NULL, &DEV, NULL, 0, &R), DVM_TRAP_POLICY);
    CHECK_CONTAINS(R.msg, "no policy");
    CHECK_STATUS(dvm_run(&P, &POL, NULL, NULL, 0, &R), DVM_TRAP_POLICY);
    CHECK_CONTAINS(R.msg, "no I/O backend");
    CHECK_EQ(dvm_run(&P, &POL, &DEV, NULL, 0, NULL), DVM_TRAP_POLICY);
}

static void test_no_backend(void) {
    std_policy();
    static const struct { const char *src; } t[] = {
        { "out8 0xc000, 1\nhalt\n" }, { "in8 r1, 0xc000\nhalt\n" },
        { "st32 [0xfebc0000], 1\nhalt\n" }, { "ld32 r1, [0xfebc0000]\nhalt\n" },
        { "pcicfg r1, 00:03.0, 0\nhalt\n" }, { "delay 10\nhalt\n" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        CHECK_EQ(assemble(t[i].src), 0);
        CHECK_STATUS(dvm_run(&P, &POL, &NULLDEV, NULL, 0, &R), DVM_TRAP_NOIO);
        CHECK_CONTAINS(R.msg, "no ");
    }
}

/* ====================================================================== */
/* the DMA scratch buffer                                                 */
/* ====================================================================== */

/* WHAT THESE PROVE
 *   The buffer is the VM's one memory grant and the one thing that makes DMA
 *   possible at all, so the claims made about it in include/dvm.h are checked
 *   mechanically rather than believed:
 *
 *     - dvm_dma_region() reports a 256 KiB, page-aligned, low-4-GiB region;
 *     - a grant may only ever name a subrange of THAT region, whatever a caller
 *       passes, and a hand-built policy is re-checked at run time;
 *     - a program's st really changes those bytes, verified by reading them back
 *       through a plain pointer at the address dvm_dma_region() reported. That
 *       is "the buffer is where you say it is", proved rather than asserted;
 *     - the bounds, alignment and budget rules bite, and a buffer access costs
 *       nothing from the DEVICE access budget;
 *     - the guard band notices a write past the end, which is the only warning
 *       this machine can give about a bad DMA address. */

static uint64_t dma_base_of(void) {
    uint64_t b = 0;
    dvm_dma_region(&b, NULL);
    return b;
}

static void dma_policy(void) {
    std_policy();
    uint64_t b = 0, n = 0;
    dvm_dma_region(&b, &n);
    CHECK_EQ(dvm_policy_allow_dma(&POL, b, n), 0);
}

/* Before any run has carried a DMA grant the guard band has never been poisoned,
 * and .bss zeros are not the poison value. A naive check would read that as a
 * 65536-byte overrun on a machine where no DMA has ever been possible. MUST run
 * before every other DMA test, which is why it is first in main(). */
static void test_dma_check_before_any_run(void) {
    char msg[DVM_MSG_MAX];
    msg[0] = 'x';
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 0);
    CHECK_EQ((int)msg[0], 0);
}

static void test_dma_region_shape(void) {
    uint64_t b = 0, n = 0;
    dvm_dma_region(&b, &n);

    CHECK(b != 0);
    CHECK_EQ((int)n, (int)DVM_DMA_SIZE);
    CHECK_EQ((int)(n / 1024), 256);                       /* 256 KiB */
    CHECK_EQ((int)(b % DVM_DMA_ALIGN), 0);                /* page aligned */
    CHECK_EQ((int)DVM_DMA_ALIGN, 4096);
    /* NOT checkable here: that the arena is below 4 GiB. On the host it is an
     * array in a 64-bit test process and its address means nothing to hardware.
     * On the kernel it follows from being .bss in an image linked at 1 MiB, and
     * dvm_policy_check() refuses every grant if it ever stops being true, which
     * is the assertion that survives into the real build. */

    /* Long enough for a second of 48 kHz stereo 16-bit PCM, which is the size
     * argument in the header, with room left for a descriptor list. */
    CHECK(n >= 48000ull * 2 * 2);
    CHECK(n - 48000ull * 2 * 2 >= 32768ull);

    /* Stable: a program that failed can be retried against the same address. */
    uint64_t b2 = 0, n2 = 0;
    dvm_dma_region(&b2, &n2);
    CHECK_EQ((int)(b2 == b), 1);
    CHECK_EQ((int)(n2 == n), 1);

    /* Both pointers are optional. */
    dvm_dma_region(NULL, NULL);
    dvm_dma_region(&b2, NULL);
    CHECK_EQ((int)(b2 == b), 1);
}

/* The grant may only ever be a subrange of the arena. This is the check that
 * stops a caller's miscomputed base becoming a window on the heap. */
static void test_dma_grant_is_bounded_to_the_arena(void) {
    uint64_t b = 0, n = 0;
    dvm_dma_region(&b, &n);

    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_dma(&POL, b, n), 0);              /* all of it */
    CHECK_EQ((int)(POL.dma_base == b), 1);
    CHECK_EQ((int)(POL.dma_size == n), 1);

    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_dma(&POL, b + 4096, 4096), 0);    /* part of it */
    CHECK_EQ((int)(POL.dma_base == b + 4096), 1);

    /* Everything else is refused, and a refusal leaves the policy alone. */
    static const struct { long off; long size; const char *why; } bad[] = {
        {  0,        0, "zero size"                    },
        { -1,        1, "one byte below the arena"      },
        { -4096,  8192, "straddles the low edge"        },
        {  0,       -1, "size wraps"                    },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        dvm_policy_init(&POL);
        CHECK_EQ(dvm_policy_allow_dma(&POL, b + (uint64_t)bad[i].off,
                                      (uint64_t)bad[i].size), -1);
        CHECK_EQ((int)POL.dma_base, 0);
        CHECK_EQ((int)POL.dma_size, 0);
    }
    /* One byte past the end. */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_dma(&POL, b, n + 1), -1);
    CHECK_EQ(dvm_policy_allow_dma(&POL, b + n, 1), -1);
    /* Somewhere else entirely: kernel memory, and the framebuffer. */
    CHECK_EQ(dvm_policy_allow_dma(&POL, 0x1000, 0x1000), -1);
    CHECK_EQ(dvm_policy_allow_dma(&POL, 0xB8000, 0x1000), -1);
    CHECK_EQ(dvm_policy_allow_dma(&POL, 0, DVM_DMA_SIZE), -1);
    CHECK_EQ(dvm_policy_allow_dma(NULL, b, n), -1);
    CHECK_EQ((int)POL.dma_base, 0);

    /* A HAND-BUILT policy that never went through the setter is refused at run
     * time, so the setter is not the only thing standing in the way. */
    char msg[DVM_MSG_MAX];
    dvm_policy_init(&POL);
    POL.dma_base = 0x1000;                 /* kernel memory */
    POL.dma_size = 0x1000;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "not inside the VM's scratch arena");

    dvm_policy_init(&POL);
    POL.dma_base = b;
    POL.dma_size = n + 0x1000;             /* runs off the end */
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "scratch arena");

    /* ...and the same thing reaches a program as a refusal to run. */
    CHECK_EQ(assemble("halt\n"), 0);
    CHECK_STATUS(dvm_run(&P, &POL, &DEV, NULL, 0, &R), DVM_TRAP_POLICY);

    /* An MMIO window may not alias the buffer: it would make the same bytes
     * reachable under the wrong budget and mislabel them in the trace. */
    dvm_policy_init(&POL);
    CHECK_EQ(dvm_policy_allow_dma(&POL, b, n), 0);
    POL.nmmio = 1;                         /* by hand: allow_mmio refuses this */
    POL.mmio[0].base  = b + 0x100;
    POL.mmio[0].size  = 0x100;
    POL.mmio[0].flags = DVM_MMIO_RW;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "overlaps the dma scratch buffer");
}

/* The load-bearing one: bytes a program writes are really in the arena, at the
 * address the caller was told, and readable back by the C side — which is what
 * a device fetching them would see. */
static void test_dma_stores_land_in_the_arena(void) {
    dma_policy();
    uint64_t b = dma_base_of();
    volatile uint8_t *mem = (volatile uint8_t *)(uintptr_t)b;

    for (int i = 0; i < 64; i++) mem[i] = 0;

    /* r0 = the buffer, as dvm_tools passes it. Write a recognisable pattern with
     * each of the three widths, then read one back through the VM too. */
    uint64_t args[1] = { b };
    CHECK_STATUS(run_args("st32 [r0+0],  0x11223344\n"
                          "st16 [r0+4],  0x5566\n"
                          "st8  [r0+6],  0x77\n"
                          "st8  [r0+7],  0x88\n"
                          "st32 [r0+8],  0xdeadbeef\n"
                          "ld32 r5, [r0+0]\n"
                          "ld16 r6, [r0+4]\n"
                          "ld8  r7, [r0+7]\n"
                          "halt\n", args, 1), DVM_OK);

    /* Read the bytes with a plain pointer: this is the proof that the address in
     * r0 names real, writable memory and not something the VM emulated. */
    CHECK_EQ((int)mem[0], 0x44);
    CHECK_EQ((int)mem[1], 0x33);
    CHECK_EQ((int)mem[2], 0x22);
    CHECK_EQ((int)mem[3], 0x11);
    CHECK_EQ((int)mem[4], 0x66);
    CHECK_EQ((int)mem[5], 0x55);
    CHECK_EQ((int)mem[6], 0x77);
    CHECK_EQ((int)mem[7], 0x88);
    CHECK_EQ((int)mem[8], 0xef);
    CHECK_EQ((int)mem[11], 0xde);

    /* ...and the VM reads back what it wrote. */
    CHECK_EQ((int)R.reg[5], 0x11223344);
    CHECK_EQ((int)R.reg[6], 0x5566);
    CHECK_EQ((int)R.reg[7], 0x88);

    /* Buffer traffic is counted separately and costs the device budget nothing:
     * five stores, three loads, and not one device access. */
    CHECK_EQ((int)R.n_dma_wr, 5);
    CHECK_EQ((int)R.n_dma_rd, 3);
    CHECK_EQ((int)R.io_ops, 0);
    CHECK_EQ((int)R.n_mmio_wr, 0);
    CHECK_EQ((int)R.n_mmio_rd, 0);
    /* Nothing reached the device backend at all. */
    CHECK_EQ(nalog, 0);

    /* The last byte of the buffer is reachable; one past it is not. */
    uint64_t n = 0;
    dvm_dma_region(NULL, &n);
    uint64_t a2[2] = { b, n };
    CHECK_STATUS(run_args("sub r2, r1, 4\n"
                          "add r2, r0, r2\n"
                          "st32 [r2+0], 0xa5a5a5a5\n"
                          "halt\n", a2, 2), DVM_OK);
    CHECK_EQ((int)mem[n - 1], 0xa5);
    CHECK_EQ((int)R.n_dma_wr, 1);
}

static void test_dma_bounds_alignment_and_budget(void) {
    dma_policy();
    uint64_t b = 0, n = 0;
    dvm_dma_region(&b, &n);
    uint64_t args[2] = { b, n };

    /* An access that STRADDLES the end must not fall through to the MMIO
     * allowlist. It lands in the buffer's own gate and says which buffer. */
    CHECK_STATUS(run_args("add r2, r0, r1\n"
                          "sub r2, r2, 2\n"
                          "st32 [r2+0], 1\n"
                          "halt\n", args, 2), DVM_TRAP_MMIO_DENIED);
    CHECK_CONTAINS(R.msg, "runs off the end of the dma buffer");
    CHECK_EQ((int)R.n_dma_wr, 0);

    /* Straddling the START is the mirror image, and equally not a fallthrough. */
    CHECK_STATUS(run_args("sub r2, r0, 2\n"
                          "st32 [r2+0], 1\n"
                          "halt\n", args, 2), DVM_TRAP_MMIO_DENIED);
    CHECK_CONTAINS(R.msg, "dma buffer");

    /* Wholly outside the buffer is refused as well, though which trap depends on
     * where the arena sits: in the kernel these addresses are kernel memory, on
     * the host they are above the mapped 4 GiB. What must hold everywhere is that
     * the program is stopped and the buffer counters do not move. */
    CHECK(run_args("add r2, r0, r1\nst8 [r2+0], 1\nhalt\n", args, 2) != DVM_OK);
    CHECK_EQ((int)R.n_dma_wr, 0);
    CHECK(run_args("sub r2, r0, 8\nst32 [r2+0], 1\nhalt\n", args, 2) != DVM_OK);
    CHECK_EQ((int)R.n_dma_wr, 0);

    /* Natural alignment, like a device register. */
    CHECK_STATUS(run_args("st32 [r0+1], 1\nhalt\n", args, 2), DVM_TRAP_ALIGN);
    CHECK_CONTAINS(R.msg, "dma buffer");
    CHECK_STATUS(run_args("st16 [r0+3], 1\nhalt\n", args, 2), DVM_TRAP_ALIGN);
    CHECK_STATUS(run_args("ld32 r5, [r0+2]\nhalt\n", args, 2), DVM_TRAP_ALIGN);
    CHECK_STATUS(run_args("st8  [r0+1], 1\nhalt\n", args, 2), DVM_OK);   /* always ok */

    /* Its own budget, which stops a runaway fill without touching max_io. */
    POL.max_dma_ops = 4;
    CHECK_STATUS(run_args("st8 [r0+0],1\nst8 [r0+1],1\nst8 [r0+2],1\n"
                          "st8 [r0+3],1\nst8 [r0+4],1\nhalt\n", args, 2),
                 DVM_TRAP_DMA_BUDGET);
    CHECK_EQ((int)R.n_dma_wr, 4);
    CHECK_CONTAINS(R.msg, "dma buffer access budget of 4");
    CHECK_EQ((int)strcmp(dvm_status_name(DVM_TRAP_DMA_BUDGET), "DMA_LIMIT"), 0);

    /* A device budget of zero does not stop buffer work: the two are separate. */
    dma_policy();
    POL.max_io = 0;
    CHECK_STATUS(run_args("st32 [r0+0], 0x1234\nld32 r5,[r0+0]\nhalt\n", args, 2), DVM_OK);
    CHECK_EQ((int)R.reg[5], 0x1234);
    /* ...and a device access with that budget still fails. */
    CHECK_STATUS(run_args("out8 0xc000, 1\nhalt\n", args, 2), DVM_TRAP_IO_BUDGET);

    /* A budget above one access per byte of the arena is refused outright. */
    char msg[DVM_MSG_MAX];
    dma_policy();
    POL.max_dma_ops = DVM_DMA_SIZE + 1;
    CHECK_EQ(dvm_policy_check(&POL, msg, sizeof msg), DVM_TRAP_POLICY);
    CHECK_CONTAINS(msg, "max_dma_ops");

    /* With NO grant, the buffer is not reachable at all: the address falls back to
     * the ordinary MMIO rules, which refuse it. */
    std_policy();
    CHECK(run_args("st32 [r0+0], 1\nhalt\n", args, 2) != DVM_OK);
    CHECK_EQ((int)R.n_dma_wr, 0);
    CHECK(run_args("ld32 r5, [r0+0]\nhalt\n", args, 2) != DVM_OK);
    CHECK_EQ((int)R.n_dma_rd, 0);
}

/* The half the VM cannot police: a DEVICE writing outside the buffer. Nothing in
 * the ISA can do this, so the test does it the way hardware would — with a raw
 * store, behind the VM's back — and then asks whether the kernel noticed. */
static void test_dma_guard_band_catches_a_device_overrun(void) {
    dma_policy();
    uint64_t b = 0, n = 0;
    dvm_dma_region(&b, &n);
    uint64_t args[2] = { b, n };
    char msg[DVM_MSG_MAX];

    /* A clean run leaves the guard armed and quiet. */
    CHECK_STATUS(run_args("st32 [r0+0], 1\nhalt\n", args, 2), DVM_OK);
    msg[0] = 'x';
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 0);
    CHECK_EQ((int)msg[0], 0);

    /* Writing right up to the last byte of the buffer is NOT an overrun: this is
     * the boundary the guard has to get exactly right, so the loop runs to
     * base+size-1 inclusive and the poison must still be intact afterwards.
     * (Filling all 256 KiB byte-by-byte would cost ~1.5 M steps; the last 8 KiB
     * is the part adjacent to the guard and so the part that matters.) */
    POL.max_steps = 200000;
    CHECK_STATUS(run_args("sub r2, r1, 8192\n"            /* r2 = first offset  */
                          "loop:\n"
                          "  add r3, r0, r2\n"
                          "  st8 [r3+0], 0x5a\n"
                          "  add r2, r2, 1\n"
                          "  cmp r2, r1\n"
                          "  blt loop\n"
                          "halt\n", args, 2), DVM_OK);
    CHECK_EQ((int)R.n_dma_wr, 8192);
    CHECK_EQ((int)((volatile uint8_t *)(uintptr_t)b)[n - 1], 0x5a);
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 0);
    dma_policy();

    /* Now a "device" that was given a length one descriptor too long. */
    CHECK_STATUS(run_args("st32 [r0+0], 1\nhalt\n", args, 2), DVM_OK);
    ((volatile uint8_t *)(uintptr_t)(b + n))[0] = 0x01;      /* +0 past the end */
    ((volatile uint8_t *)(uintptr_t)(b + n))[7] = 0x02;
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 1);
    CHECK_CONTAINS(msg, "past the end");
    CHECK_CONTAINS(msg, "+0");

    /* Reported once, then re-armed: the next run does not inherit the verdict. */
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 0);

    /* An address BELOW the buffer is the other common mistake, and reads as such. */
    CHECK_STATUS(run_args("st32 [r0+0], 1\nhalt\n", args, 2), DVM_OK);
    ((volatile uint8_t *)(uintptr_t)(b - 1))[0] = 0x03;
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 1);
    CHECK_CONTAINS(msg, "BELOW the start");
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 0);

    /* Both at once is reported as both. */
    CHECK_STATUS(run_args("st32 [r0+0], 1\nhalt\n", args, 2), DVM_OK);
    ((volatile uint8_t *)(uintptr_t)(b + n))[3] = 0x04;
    ((volatile uint8_t *)(uintptr_t)(b - 8))[0] = 0x05;
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 1);
    CHECK_CONTAINS(msg, "AND");
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 0);

    /* A run with no DMA grant does not re-arm, so evidence from a device that is
     * still mastering is not erased by an unrelated program. */
    std_policy();
    ((volatile uint8_t *)(uintptr_t)(b + n))[0] = 0x06;
    CHECK_STATUS(run("halt\n"), DVM_OK);
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 1);
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 0);
}

/* The grant is announced before the program runs, so the operator's transcript
 * records that this run could have caused DMA. */
static void test_dma_grant_is_traced(void) {
    dma_policy();
    uint64_t b = 0, nb = 0;
    dvm_dma_region(&b, &nb);
    uint64_t args[2] = { b, nb };

    POL.trace = DVM_TRACE_IO;
    kcap_reset();
    CHECK_STATUS(run_args("st32 [r0+0], 0x99\nout8 0xc000, 1\nhalt\n", args, 2), DVM_OK);
    CHECK_CONTAINS(kcap_text(), "[dvm.grant dma 0x");
    CHECK_CONTAINS(kcap_text(), "master into it");
    CHECK_CONTAINS(kcap_text(), "dma=1 (rd=0 wr=1)");
    /* At trace=io the buffer stores themselves stay quiet, so a fill cannot
     * spend the line budget and silence the device accesses that matter. */
    CHECK(strstr(kcap_text(), "st32 dma+") == NULL);
    CHECK_CONTAINS(kcap_text(), "out8 port=0xc000");

    /* At trace=all they appear, with an offset rather than a raw address. */
    POL.trace = DVM_TRACE_ALL;
    kcap_reset();
    CHECK_STATUS(run_args("st32 [r0+8], 0x99\nld32 r5,[r0+8]\nhalt\n", args, 2), DVM_OK);
    CHECK_CONTAINS(kcap_text(), "st32 dma+0x8 val=0x99");
    CHECK_CONTAINS(kcap_text(), "ld32 dma+0x8 r5=0x99");

    /* No grant, and the trace says so in as many words. */
    std_policy();
    POL.trace = DVM_TRACE_IO;
    kcap_reset();
    CHECK_STATUS(run("halt\n"), DVM_OK);
    CHECK_CONTAINS(kcap_text(), "[dvm.grant dma none -> ok]");
}

/* ====================================================================== */
/* scratch memory, strings and syscalls — the shape, in this suite        */
/* ====================================================================== */

/* The deep coverage of this family (every bound, every off-by-one, overlapping
 * copies, conversion edges, fuzz) is tests/host/test_dvm_mem.c. What belongs
 * HERE is that the instructions exist, execute, and are counted — this suite
 * owns the "every opcode in the ISA ran" invariant at the bottom of main(). */

static char     sysdev_said[256];
static int64_t  sysdev_time;

static int64_t sd_con_write(void *ctx, const char *text, size_t len,
                            char *err, size_t errcap) {
    (void)ctx; (void)err; (void)errcap;
    size_t n = len < sizeof sysdev_said - 1 ? len : sizeof sysdev_said - 1;
    memcpy(sysdev_said, text, n);
    sysdev_said[n] = '\0';
    return (int64_t)len;
}
static int64_t sd_time_ms(void *ctx, char *err, size_t errcap) {
    (void)ctx; (void)err; (void)errcap;
    return sysdev_time;
}

static const dvm_sys_t SYSDEV = {
    .ctx = NULL, .con_write = sd_con_write, .time_ms = sd_time_ms,
};

/* The synthetic device again, this time with a kernel behind it. */
static const dvm_io_t DEVSYS = {
    .ctx = NULL,
    .port_write = io_port_write, .port_read = io_port_read,
    .mmio_write = io_mmio_write, .mmio_read = io_mmio_read,
    .pci_read32 = io_pci_read32, .delay_us  = io_delay,
    .sys = &SYSDEV,
};

/* ---------------------------------------------------------------------
 * ONE PROGRAM AT A TIME
 *
 * The DMA guard bands and the scratch arena are single, machine-wide, and both
 * are (re)initialised at dvm_run() entry. So a nested dvm_run() does not share
 * them with the outer program, it DESTROYS the outer program's copy while the
 * outer program is still executing. That was reachable through a syscall: a
 * driver_run program is granted audio.tone, and once the model has installed its
 * own audio driver the sink IS a driver program, so servicing the syscall meant
 * starting a second run.
 *
 * The damage was silent in both directions. Scratch went to zeros with no trap,
 * so every later mld read 0 and the tool reported the program's answer as NUL
 * bytes. And the guard bands were re-poisoned, erasing an overrun the outer
 * program's device had ALREADY committed — which mattered far more than the
 * diagnostic, because callers turn an overrun into a spent attempt rather than
 * progress, so erasing it gave unlimited retries to a program corrupting memory.
 *
 * These tests drive the mechanism directly: a syscall hook that itself calls
 * dvm_run. That is exactly what k_audio_tone does, without needing core/audio.c
 * here (tests/host/test_audio.c covers the real audio path).
 * ------------------------------------------------------------------ */

static int      reent_attempts;      /* how many times the hook tried to nest */
static dvm_status_t reent_status;    /* what the nested dvm_run returned      */
static char     reent_msg[DVM_MSG_MAX];
static int      reent_inner_ran;     /* did the inner program execute at all? */

static int64_t reent_tone(void *ctx, uint32_t hz, uint32_t ms,
                          char *err, size_t cap) {
    (void)ctx; (void)hz; (void)ms;
    reent_attempts++;

    /* A second, unrelated program, with its own policy and result. The inner
     * program stores a byte in scratch, which is how the test can tell whether
     * it executed. */
    static dvm_program_t inner;
    static dvm_policy_t  ipol;
    static dvm_result_t  ires;
    dvm_asm_err_t ierr;
    const char *isrc = "mst8 [0], 0x5a\nhalt\n";
    if (dvm_assemble(isrc, strlen(isrc), &inner, &ierr) != 0) {
        snprintf(err, cap, "inner assembly failed");
        return -1;
    }
    dvm_policy_init(&ipol);
    ipol.trace = DVM_TRACE_OFF;
    /* Carry a DMA grant, because that is what makes the nested run re-arm the
     * guard bands — the half of this defect that erased evidence. */
    uint64_t dbase = 0, dsize = 0;
    dvm_dma_region(&dbase, &dsize);
    dvm_policy_allow_dma(&ipol, dbase, dsize);

    reent_status = dvm_run(&inner, &ipol, &NULLDEV, NULL, 0, &ires);
    snprintf(reent_msg, sizeof reent_msg, "%s", ires.msg);
    reent_inner_ran = (reent_status == DVM_OK);
    if (reent_status != DVM_OK) {
        snprintf(err, cap, "%s", ires.msg);
        return -1;
    }
    return 0;
}

static const dvm_sys_t REENT_SYS = {
    .ctx = NULL, .con_write = sd_con_write, .audio_tone = reent_tone,
    .time_ms = sd_time_ms,
};
static const dvm_io_t REENT_IO = {
    .ctx = NULL,
    .port_write = io_port_write, .port_read = io_port_read,
    .mmio_write = io_mmio_write, .mmio_read = io_mmio_read,
    .pci_read32 = io_pci_read32, .delay_us  = io_delay,
    .sys = &REENT_SYS,
};

static void reent_reset(void) {
    reent_attempts = 0;
    reent_status = DVM_OK;
    reent_msg[0] = '\0';
    reent_inner_ran = 0;
}

static void test_a_nested_run_is_refused(void) {
    reent_reset();
    std_policy();
    dvm_policy_allow_sys(&POL, DVM_SYS_AUDIO_TONE);
    POL.max_sys = 8;
    POL.max_mem_bytes = 4096;

    CHECK_EQ(assemble("push 440\npush 20\nsys r1, audio.tone\nhalt\n"), 0);
    dev_reset();
    CHECK_STATUS(dvm_run(&P, &POL, &REENT_IO, NULL, 0, &R), DVM_OK);

    /* The hook really was called — otherwise this test proves nothing. */
    CHECK_EQ(reent_attempts, 1);
    /* ...and the nested run was refused, before doing anything. */
    CHECK_EQ((int)reent_status, (int)DVM_TRAP_REENTRY);
    CHECK_EQ(reent_inner_ran, 0);
    CHECK_EQ((int)strcmp(dvm_status_name(DVM_TRAP_REENTRY), "REENTRY"), 0);

    /* The refusal has to be a sentence the model can act on: it must name the
     * cause and say what to do instead, not just fail. */
    CHECK_CONTAINS(reent_msg, "one driver program at a time");
    CHECK_CONTAINS(reent_msg, "after the run finishes");

    /* The whole sentence has to SURVIVE the trip back to the program. sys_fail()
     * prefixes the syscall name and truncates to DVM_MSG_MAX, and the clause
     * that says what to do instead is at the end — so it is the first thing lost
     * if the message is too long. Assert the model really receives it. */
    CHECK_CONTAINS(R.sys_msg, "audio.tone");
    CHECK_CONTAINS(R.sys_msg, "after the run finishes");

    /* The documented convention (driver_run's ISA text): zero flag set means it
     * worked, otherwise rd holds an error code. So the program can branch on it
     * rather than believing a tone played. */
    CHECK(R.reg[1] != 0);
}

/* The property the refusal exists to protect, measured rather than argued: the
 * outer program's scratch memory must still be there after the syscall. */
static void test_scratch_survives_a_syscall(void) {
    reent_reset();
    std_policy();
    dvm_policy_allow_sys(&POL, DVM_SYS_AUDIO_TONE);
    POL.max_sys = 8;
    POL.max_mem_bytes = 4096;

    CHECK_EQ(assemble("mst32 [0], 0xdeadbeef\n"
                      "mld32 r1, [0]\n"          /* before the syscall */
                      "push 440\npush 20\n"
                      "sys r3, audio.tone\n"
                      "mld32 r2, [0]\n"          /* after it           */
                      "halt\n"), 0);
    dev_reset();
    CHECK_STATUS(dvm_run(&P, &POL, &REENT_IO, NULL, 0, &R), DVM_OK);

    CHECK_EQ(reent_attempts, 1);
    CHECK_EQ(R.reg[1], 0xdeadbeefu);
    CHECK_EQ(R.reg[2], 0xdeadbeefu);   /* was 0 before the guard existed */

    /* And the caller's window is intact too: dvm_mem_peek must show the bytes
     * the program stored, not a zeroed arena. put_scratch() in dvm_tools.c
     * dumps mem_hwm bytes from here, so a zeroed arena made the tool report the
     * program's answer as NULs. */
    unsigned char peek[4] = { 1, 2, 3, 4 };
    CHECK_EQ((int)dvm_mem_peek(0, peek, 4), 4);
    CHECK_EQ(peek[0], 0xef);
    CHECK_EQ(peek[1], 0xbe);
    CHECK_EQ(peek[2], 0xad);
    CHECK_EQ(peek[3], 0xde);
}

/* The other half: a syscall must not re-arm the DMA guard bands, because that
 * erases an overrun this run's device has already committed. Damage the guard
 * by hand first, then make the syscall, then check the damage is still
 * reportable. */
static void test_a_syscall_cannot_erase_dma_evidence(void) {
    reent_reset();

    /* Consume any pending report so the check below is about THIS test. */
    char scratch[DVM_MSG_MAX];
    (void)dvm_dma_check(scratch, sizeof scratch);

    std_policy();
    dvm_policy_allow_sys(&POL, DVM_SYS_AUDIO_TONE);
    POL.max_sys = 8;
    uint64_t dbase = 0, dsize = 0;
    dvm_dma_region(&dbase, &dsize);
    CHECK_EQ(dvm_policy_allow_dma(&POL, dbase, dsize), 0);

    CHECK_EQ(assemble("push 440\npush 20\nsys r1, audio.tone\nhalt\n"), 0);
    dev_reset();

    /* dvm_run arms the bands at entry, so the damage has to be done from inside
     * the run to model a device overrunning mid-flight. The syscall hook is the
     * only code that runs inside, so scribble from there — after it has tried
     * (and failed) to nest, which is the ordering that used to lose the
     * evidence. Writing one byte just past the end of the arena is exactly what
     * a descriptor length one too large produces. */
    volatile unsigned char *overrun = (volatile unsigned char *)(uintptr_t)(dbase + dsize);
    CHECK_STATUS(dvm_run(&P, &POL, &REENT_IO, NULL, 0, &R), DVM_OK);
    CHECK_EQ(reent_attempts, 1);
    CHECK_EQ((int)reent_status, (int)DVM_TRAP_REENTRY);

    /* Now damage it and confirm the report still arrives. If a nested run had
     * been allowed it would have re-poisoned this byte and the check would come
     * back clean — which is the bug, stated as a test. */
    *overrun = 0x77;
    char msg[DVM_MSG_MAX];
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 1);
    CHECK_CONTAINS(msg, "past the end");

    /* Reported once, then re-armed. */
    CHECK_EQ(dvm_dma_check(msg, sizeof msg), 0);
}

/* A refused nested run must leave NOTHING locked. If the guard flag leaked, the
 * next ordinary run on the machine would be refused too, and driver_run would
 * be permanently broken after one audio.tone. */
static void test_the_guard_does_not_leak(void) {
    reent_reset();
    std_policy();
    dvm_policy_allow_sys(&POL, DVM_SYS_AUDIO_TONE);
    POL.max_sys = 8;
    CHECK_EQ(assemble("push 440\npush 20\nsys r1, audio.tone\nhalt\n"), 0);
    dev_reset();
    CHECK_STATUS(dvm_run(&P, &POL, &REENT_IO, NULL, 0, &R), DVM_OK);
    CHECK_EQ(reent_attempts, 1);

    /* An ordinary run, straight afterwards, must be completely unaffected. */
    std_policy();
    CHECK_STATUS(run("mov r1, 7\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 7u);

    /* And a validation failure must not lock it either: those return before the
     * flag is ever set, but that is a property worth pinning, because moving the
     * flag one line earlier would break it silently. */
    dvm_policy_t bad;
    dvm_policy_init(&bad);
    bad.max_steps = 0;                     /* structurally invalid */
    dvm_result_t r2;
    CHECK(dvm_run(&P, &bad, &REENT_IO, NULL, 0, &r2) != DVM_OK);
    std_policy();
    CHECK_STATUS(run("mov r2, 9\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 9u);
}

static void test_scratch_memory_and_strings(void) {
    std_policy();

    /* Build "GET /x" out of two literals and a byte, then find and slice it. */
    CHECK_STATUS(run("mstr  r1, 0, \"GET \"\n"       /* r1 = 4              */
                     "mstr  r2, r1, \"/x\"\n"        /* r2 = 6              */
                     "mchr  r3, 0, r2, 0x2f\n"       /* '/' at offset 4     */
                     "mcpy  r4, 16, 0, r2\n"         /* copy the lot to 16  */
                     "mcmp  0, 16, r2\n"             /* and it is identical */
                     "beq   same\n"
                     "abort \"copy differs\"\n"
                     "same:\n"
                     "mset  r5, 32, 0x41, 4\n"       /* "AAAA" at 32        */
                     "mld32 r6, [32]\n"              /* little-endian gather */
                     "mst64 [40], r6\n"
                     "mld64 r7, [40]\n"
                     "mitoa r8, 48, 200, 10\n"       /* "200" at 48         */
                     "matoi r9, 48, 3, 10\n"
                     "halt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 4);
    CHECK_EQ(R.reg[2], 6);
    CHECK_EQ(R.reg[3], 4);
    CHECK_EQ(R.reg[4], 22);
    CHECK_EQ(R.reg[5], 36);
    CHECK_EQ(R.reg[6], 0x41414141u);
    CHECK_EQ(R.reg[7], 0x41414141u);
    CHECK_EQ(R.reg[8], 51);
    CHECK_EQ(R.reg[9], 200);
    CHECK(R.mem_bytes > 0);
    CHECK(R.n_mem_rd > 0 && R.n_mem_wr > 0);

    /* mfind, both outcomes, and the flag that distinguishes them. */
    CHECK_STATUS(run("mstr  r1, 0, \"HTTP/1.1 404 Not Found\"\n"
                     "mfind r2, 0, r1, \"404\"\n"
                     "bne   nope\n"
                     "mfind r3, 0, r1, \"zzz\"\n"
                     "beq   nope\n"
                     "halt\n"
                     "nope:\n"
                     "abort \"mfind disagreed with itself\"\n"), DVM_OK);
    CHECK_EQ(R.reg[2], 9);
    CHECK_EQ(R.reg[3], 22);           /* not found == the end of the span */

    /* The arena is zeroed between runs, so the bytes above are gone. */
    CHECK_STATUS(run("mld64 r1, [0]\nmld64 r2, [16]\nhalt\n"), DVM_OK);
    CHECK_EQ(R.reg[1], 0);
    CHECK_EQ(R.reg[2], 0);

    /* And out of bounds is a trap, not a wrap. */
    CHECK_STATUS(run("mld8 r1, [0x10000]\nhalt\n"), DVM_TRAP_MEM);
    CHECK_CONTAINS(R.msg, "outside the 65536-byte arena");
}

static void test_syscalls_reach_the_kernel(void) {
    std_policy();
    POL.trace = DVM_TRACE_OFF;
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_CON_WRITE), 0);
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_TIME_MS), 0);

    sysdev_said[0] = '\0';
    sysdev_time = 1234;

    dev_reset();
    CHECK_EQ(assemble("mstr r1, 0, \"hello from a program\"\n"
                      "push 0\n"
                      "push r1\n"
                      "sys  r2, con.write\n"
                      "bne  bad\n"
                      "sys  r3, time.ms\n"
                      "bne  bad\n"
                      "halt\n"
                      "bad:\n"
                      "abort \"a syscall failed\"\n"), 0);
    mark_ops(&P);
    CHECK_STATUS(dvm_run(&P, &POL, &DEVSYS, NULL, 0, &R), DVM_OK);
    CHECK_STR(sysdev_said, "hello from a program");
    CHECK_EQ(R.reg[2], 20);
    CHECK_EQ(R.reg[3], 1234);
    CHECK_EQ(R.n_sys, 2);

    /* A syscall that was not granted is refused by name, and the message says
     * what the program does have. */
    dev_reset();
    CHECK_EQ(assemble("push 0\npush 0\nsys r1, fs.size\nhalt\n"), 0);
    CHECK_STATUS(dvm_run(&P, &POL, &DEVSYS, NULL, 0, &R), DVM_TRAP_SYS_DENIED);
    CHECK_CONTAINS(R.msg, "fs.size");
    CHECK_CONTAINS(R.msg, "con.write,time.ms");

    /* A granted syscall with no backend hook is NO_BACKEND, never a pretend
     * success. */
    dev_reset();
    CHECK_EQ(dvm_policy_allow_sys(&POL, DVM_SYS_AUDIO_TONE), 0);
    CHECK_EQ(assemble("push 440\npush 10\nsys r1, audio.tone\nhalt\n"), 0);
    CHECK_STATUS(dvm_run(&P, &POL, &DEVSYS, NULL, 0, &R), DVM_TRAP_NOIO);

    /* ...and with no kernel at all, likewise. */
    dev_reset();
    CHECK_EQ(assemble("sys r1, time.ms\nhalt\n"), 0);
    CHECK_STATUS(dvm_run(&P, &POL, &DEV, NULL, 0, &R), DVM_TRAP_NOIO);
    CHECK_CONTAINS(R.msg, "no kernel backend");
}

/* A program that never went through the assembler must not be able to escape:
 * the validator re-checks everything the assembler guarantees. */
static void test_validator_rejects_handbuilt(void) {
    std_policy();
    static dvm_program_t bad;

    memset(&bad, 0, sizeof bad);
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);    /* no instructions */
    CHECK_EQ(dvm_program_validate(NULL, &E), DVM_TRAP_BADOP);

    /* opcode out of range */
    CHECK_EQ(assemble("halt\n"), 0);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].op = 250;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    CHECK_CONTAINS(E.msg, "does not exist");
    CHECK_STATUS(dvm_run(&bad, &POL, &DEV, NULL, 0, &R), DVM_TRAP_BADOP);
    CHECK_EQ(R.steps, 0);

    /* register index out of range */
    CHECK_EQ(assemble("mov r1, r2\nhalt\n"), 0);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].val[0] = 99;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_RANGE);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].dst = 99;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_RANGE);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].dst = DVM_NOREG;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    CHECK_CONTAINS(E.msg, "no destination register");

    /* branch target outside the program — the headline containment property */
    CHECK_EQ(assemble("L:\njmp L\n"), 0);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].val[0] = 9999;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_PC);
    CHECK_CONTAINS(E.msg, "outside the program");
    CHECK_STATUS(dvm_run(&bad, &POL, &DEV, NULL, 0, &R), DVM_TRAP_PC);
    CHECK_EQ(R.steps, 0);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].val[0] = 1;                     /* == ninsn: one past the end */
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_PC);

    /* an operand kind the instruction cannot take */
    CHECK_EQ(assemble("out8 0xc000, 1\nhalt\n"), 0);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].kind[0] = DVM_O_PC;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].kind[0] = DVM_O_STR;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].kind[0] = 77;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);

    /* a branch whose operand is not a target at all */
    CHECK_EQ(assemble("L:\njmp L\n"), 0);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].kind[0] = DVM_O_IMM;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);

    /* a string index outside the pool */
    CHECK_EQ(assemble("abort \"x\"\n"), 0);
    memcpy(&bad, &P, sizeof bad);
    bad.insn[0].val[0] = 31;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    memcpy(&bad, &P, sizeof bad);
    bad.stroff[0] = DVM_STRPOOL + 4;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    /* an unterminated pool */
    memcpy(&bad, &P, sizeof bad);
    memset(bad.strpool, 'A', sizeof bad.strpool);
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);

    /* corrupt table counts */
    memcpy(&bad, &P, sizeof bad);
    bad.ninsn = DVM_MAX_INSNS + 1;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    memcpy(&bad, &P, sizeof bad);
    bad.nstr = DVM_MAX_STRINGS + 1;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    memcpy(&bad, &P, sizeof bad);
    bad.nlabel = DVM_MAX_LABELS + 1;
    CHECK_EQ(dvm_program_validate(&bad, &E), DVM_TRAP_BADOP);
    /* validate tolerates a NULL error sink */
    CHECK_EQ(dvm_program_validate(&bad, NULL), DVM_TRAP_BADOP);
}

/* ====================================================================== */
/* 5. the execution trace                                                 */
/* ====================================================================== */

static void test_trace_io_level(void) {
    std_policy();
    POL.trace = DVM_TRACE_IO;
    kcap_reset();
    uint64_t bar[1] = { MMIO_BASE };
    CHECK_STATUS(run_args("out8 0xc000, 0x12\n"
                          "in8 r1, 0xc000\n"
                          "st32 [r0+4], 0xabcd\n"
                          "ld32 r2, [r0+4]\n"
                          "pcicfg r3, 00:03.0, 0\n"
                          "delay 25\n"
                          "halt\n", bar, 1), DVM_OK);
    const char *t = kcap_text();

    /* the sandbox is stated before anything runs */
    CHECK_CONTAINS(t, "[dvm.grant ports 0xc000-0xc03f -> ok]");
    CHECK_CONTAINS(t, "[dvm.grant mmio 0xfebc0000+0x1000rw,0xfebd0000+0x100r -> ok]");
    CHECK_CONTAINS(t, "[dvm.grant pci 00:03.0,00:04.1 -> ok]");
    CHECK_CONTAINS(t, "[dvm.grant dma none -> ok]");
    CHECK_CONTAINS(t, "[dvm.run insns=7 args=1");

    /* every device access, with the value that crossed the bus */
    CHECK_CONTAINS(t, "out8 port=0xc000 val=0x12 -> ok]");
    CHECK_CONTAINS(t, "in8 port=0xc000 r1=0x12 -> ok]");
    CHECK_CONTAINS(t, "st32 addr=0xfebc0004 val=0xabcd -> ok]");
    CHECK_CONTAINS(t, "ld32 addr=0xfebc0004 r2=0xabcd -> ok]");
    CHECK_CONTAINS(t, "pcicfg 00:03.0 off=0x00 r3=0x100e8086 -> ok]");
    CHECK_CONTAINS(t, "delay 25 us (total 25) -> ok]");
    CHECK_CONTAINS(t, "[dvm.halt pc=006 ln=7 steps=7 io=5");

    /* line numbers are the model's source lines, so it can fix its program */
    CHECK_CONTAINS(t, "pc=000 ln=1 out8");
    CHECK_CONTAINS(t, "pc=005 ln=6 delay");

    /* ALU is not traced at this level */
    th_checks++;
    if (strstr(t, "ln=1 mov")) { th_fails++; printf("    FAIL IO level traced an ALU op\n"); }
}

static void test_trace_all_level(void) {
    std_policy();
    POL.trace = DVM_TRACE_ALL;
    kcap_reset();
    CHECK_STATUS(run("mov r1, 3\n"
                     "cmp r1, 3\n"
                     "beq skip\n"
                     "nop\n"
                     "skip:\n"
                     "push r1\n"
                     "pop r2\n"
                     "call bump\n"
                     "halt\n"
                     "bump:\n"
                     "add r3, r1, 1\n"
                     "ret\n"), DVM_OK);
    const char *t = kcap_text();
    CHECK_CONTAINS(t, "[dvm pc=000 ln=1 mov r1=0x3 -> ok]");
    CHECK_CONTAINS(t, "[dvm pc=001 ln=2 cmp 0x3,0x3 eq=1 below=0 -> ok]");
    CHECK_CONTAINS(t, "[dvm pc=002 ln=3 beq taken pc->004 -> ok]");
    CHECK_CONTAINS(t, "[dvm pc=004 ln=6 push 0x3 depth=1 -> ok]");
    CHECK_CONTAINS(t, "[dvm pc=005 ln=7 pop r2=0x3 -> ok]");
    CHECK_CONTAINS(t, "[dvm pc=006 ln=8 call pc->008 depth=1 -> ok]");
    CHECK_CONTAINS(t, "[dvm pc=008 ln=11 add r3=0x4 -> ok]");
    CHECK_CONTAINS(t, "[dvm pc=009 ln=12 ret pc->007 depth=0 -> ok]");

    /* a not-taken branch is just as visible as a taken one */
    kcap_reset();
    CHECK_STATUS(run("cmp 1, 2\nbeq away\nhalt\naway:\nhalt\n"), DVM_OK);
    CHECK_CONTAINS(kcap_text(), "beq not-taken pc->002");
}

static void test_trace_traps_and_budget(void) {
    std_policy();
    POL.trace = DVM_TRACE_IO;

    kcap_reset();
    CHECK_STATUS(run("out8 0x64, 0xfe\nhalt\n"), DVM_TRAP_PORT_DENIED);
    CHECK_CONTAINS(kcap_text(), "[dvm.trap pc=000 ln=1 PORT_DENIED:");
    CHECK_CONTAINS(kcap_text(), "reset line");
    CHECK_CONTAINS(kcap_text(), "-> EINVAL]");
    CHECK_CONTAINS(kcap_text(), "[dvm.stop PORT_DENIED steps=1");

    kcap_reset();
    POL.max_steps = 50;
    CHECK_STATUS(run("loop:\njmp loop\n"), DVM_TRAP_STEPS);
    CHECK_CONTAINS(kcap_text(), "[dvm.trap pc=000 ln=2 STEP_LIMIT:");
    CHECK_CONTAINS(kcap_text(), "[dvm.stop STEP_LIMIT steps=50");

    /* the trace budget goes quiet loudly, and never eats the trap line */
    kcap_reset();
    POL.max_steps = 100000;
    POL.max_trace = 8;
    POL.trace = DVM_TRACE_ALL;
    CHECK_STATUS(run("mov r1, 200\nloop:\nsub r1, 1\ncmp r1, 0\nbne loop\n"
                     "out8 0x64, 1\nhalt\n"), DVM_TRAP_PORT_DENIED);
    CHECK_CONTAINS(kcap_text(), "[dvm.trace line budget 8 reached");
    CHECK_CONTAINS(kcap_text(), "PORT_DENIED");
    CHECK(R.trace_dropped > 100);

    /* trace off means silence, including for traps */
    kcap_reset();
    POL.trace = DVM_TRACE_OFF;
    POL.max_trace = 512;
    CHECK_STATUS(run("out8 0x64, 1\nhalt\n"), DVM_TRAP_PORT_DENIED);
    CHECK_STR(kcap_text(), "");
    /* the result still carries the whole story */
    CHECK_CONTAINS(R.msg, "never accessible");
}

/* ====================================================================== */
/* 6. fuzzing                                                             */
/* ====================================================================== */

static uint32_t rnd_state = 0x1234567u;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 8) & 0xFFFFFF;
}

/* Whatever a program does, these must hold. */
static void check_bounded(dvm_status_t s) {
    th_checks++;
    if (s < 0 || s >= DVM_STATUS__COUNT) {
        th_fails++;
        printf("    FAIL status %d out of range\n", (int)s);
    }
    CHECK(R.steps <= POL.max_steps);
    CHECK(R.io_ops <= POL.max_io);
    CHECK(R.delay_us <= POL.max_delay_us);
    CHECK(R.prints <= POL.max_prints);
    CHECK_EQ(forbidden, 0);
    CHECK_EQ(kpanic_hit, 0);
}

/* Raw bytes as a program: must never crash, and must never assemble into
 * something that escapes. */
static void test_fuzz_bytes(void) {
    static char src[512];
    std_policy();
    POL.max_steps = 2000;
    int assembled = 0;
    for (int iter = 0; iter < 4000; iter++) {
        size_t len = 1 + (rnd() % 400);
        for (size_t i = 0; i < len; i++) {
            uint32_t r = rnd();
            src[i] = (r % 3 == 0) ? "movaddr0123456789[]+,;\"\n :.xhalt"[r % 32]
                                  : (char)(r & 0xFF);
        }
        src[len] = (char)0xA5;                 /* no NUL: the span is the bound */
        if (dvm_assemble(src, len, &P, &E) == 0) {
            assembled++;
            mark_ops(&P);
            dev_reset();
            check_bounded(dvm_run(&P, &POL, &DEV, NULL, 0, &R));
        } else {
            CHECK(E.line >= 0);
            CHECK(E.msg[0] != '\0');
        }
        CHECK_EQ((unsigned char)src[len], 0xA5);   /* nobody walked off the end */
    }
    printf("    (raw-byte fuzz: %d/4000 assembled)\n", assembled);
}

/* Token soup from the real vocabulary: far more likely to assemble, so this is
 * what actually exercises the interpreter with hostile programs. */
static void test_fuzz_tokens(void) {
    static const char *word[] = {
        "mov", "add", "sub", "mul", "div", "mod", "and", "or", "xor", "not",
        "shl", "shr", "cmp", "test", "jmp", "beq", "bne", "blt", "ble", "bgt",
        "bge", "call", "ret", "push", "pop", "halt", "abort", "nop", "print",
        "delay", "out8", "out16", "out32", "in8", "in16", "in32",
        "ld8", "ld16", "ld32", "st8", "st16", "st32", "pcicfg",
        "r0", "r1", "r2", "r15", "r16", "0", "1", "4", "0x10", "0xc000",
        "0xfebc0000", "0xb8000", "0x64", "0xcf8", "-1", "0xffffffffffffffff",
        "[r0]", "[r1+4]", "[0xfebc0000]", "[0xb8000]", "00:03.0", "00:05.0",
        "L1:", "L1", "L2:", "L2", "\"msg\"", ".equ", "FOO", "loop:", "loop",
        ",", "", "#c", ";c",
    };
    const int NW = (int)(sizeof word / sizeof word[0]);
    static char src[4096];
    std_policy();
    POL.max_steps = 3000;
    POL.max_io = 200;
    int assembled = 0, ran = 0;

    for (int iter = 0; iter < 20000; iter++) {
        int n = 0, lines = 1 + (int)(rnd() % 12);
        src[0] = '\0';
        for (int l = 0; l < lines && n < (int)sizeof src - 64; l++) {
            int toks = 1 + (int)(rnd() % 4);
            for (int k = 0; k < toks; k++)
                n += snprintf(src + n, sizeof src - n, "%s ", word[rnd() % NW]);
            n += snprintf(src + n, sizeof src - n, "\n");
        }
        if (dvm_assemble(src, strlen(src), &P, &E) == 0) {
            assembled++;
            CHECK_EQ(dvm_program_validate(&P, &E), DVM_OK);
            mark_ops(&P);
            dev_reset();
            check_bounded(dvm_run(&P, &POL, &DEV, NULL, 0, &R));
            ran++;
        } else {
            CHECK(E.msg[0] != '\0');
            CHECK(E.line >= 0 && E.line <= 64);
        }
    }
    printf("    (token fuzz: %d/20000 assembled and ran %d)\n", assembled, ran);
}

/* Programs generated from real instruction templates with hostile operands.
 * Unlike the soup above these assemble most of the time, so this is the fuzzer
 * that drives the interpreter through the front door — loops, denied addresses,
 * wild registers and all. Every line gets a label, so any branch resolves and
 * backward branches make loops the step limit then has to stop. */
static void test_fuzz_generated(void) {
    static const char *val[] = {
        "0", "1", "2", "4", "0x10", "0x20", "63", "64", "255", "0xffff",
        "0x10000", "0xc000", "0xc03f", "0xc040", "0x64", "0xcf8", "0x3f8",
        "0xb8000", "0xfebc0000", "0xfebc0004", "0xfebc0ffe", "0xfebd0000",
        "0xfebbfffc", "0xffffffff", "-1", "r0", "r1", "r2", "r3", "r15",
    };
    static const char *bdf[] = { "00:03.0", "00:04.1", "00:05.0", "0x18", "r1", "0xffff" };
    const int NV = (int)(sizeof val / sizeof val[0]);
    const int NB = (int)(sizeof bdf / sizeof bdf[0]);
    static char src[8192];
    static int  status_hist[DVM_STATUS__COUNT];

    std_policy();
    POL.max_steps = 3000;
    POL.max_io    = 64;
    POL.max_delay_us = 5000;

    int assembled = 0;
    for (int iter = 0; iter < 8000; iter++) {
        int n = 0, lines = 1 + (int)(rnd() % 14);
        for (int l = 0; l < lines && n < (int)sizeof src - 128; l++) {
            n += snprintf(src + n, sizeof src - n, "L%d: ", l);
            const char *a = val[rnd() % NV], *b = val[rnd() % NV], *c = val[rnd() % NV];
            int tgt = (int)(rnd() % (unsigned)lines);
            switch (rnd() % 23) {
                case 0:  n += snprintf(src+n, sizeof src-n, "mov r%d, %s\n", (int)(rnd()%16), a); break;
                case 1:  n += snprintf(src+n, sizeof src-n, "add r%d, %s, %s\n", (int)(rnd()%16), a, b); break;
                case 2:  n += snprintf(src+n, sizeof src-n, "div r%d, %s, %s\n", (int)(rnd()%16), a, b); break;
                case 3:  n += snprintf(src+n, sizeof src-n, "mod r%d, %s, %s\n", (int)(rnd()%16), a, b); break;
                case 4:  n += snprintf(src+n, sizeof src-n, "shl r%d, %s, %s\n", (int)(rnd()%16), a, b); break;
                case 5:  n += snprintf(src+n, sizeof src-n, "cmp %s, %s\n", a, b); break;
                case 6:  n += snprintf(src+n, sizeof src-n, "test %s, %s\n", a, b); break;
                case 7:  n += snprintf(src+n, sizeof src-n, "jmp L%d\n", tgt); break;
                case 8:  n += snprintf(src+n, sizeof src-n, "bne L%d\n", tgt); break;
                case 9:  n += snprintf(src+n, sizeof src-n, "beq L%d\n", tgt); break;
                case 10: n += snprintf(src+n, sizeof src-n, "call L%d\n", tgt); break;
                case 11: n += snprintf(src+n, sizeof src-n, "ret\n"); break;
                case 12: n += snprintf(src+n, sizeof src-n, "push %s\n", a); break;
                case 13: n += snprintf(src+n, sizeof src-n, "pop r%d\n", (int)(rnd()%16)); break;
                case 14: n += snprintf(src+n, sizeof src-n, "out8 %s, %s\n", a, b); break;
                case 15: n += snprintf(src+n, sizeof src-n, "out32 %s, %s\n", a, b); break;
                case 16: n += snprintf(src+n, sizeof src-n, "in16 r%d, %s\n", (int)(rnd()%16), a); break;
                case 17: n += snprintf(src+n, sizeof src-n, "ld32 r%d, [%s+%s]\n", (int)(rnd()%16), a, b); break;
                case 18: n += snprintf(src+n, sizeof src-n, "st8 [%s+%s], %s\n", a, b, c); break;
                case 19: n += snprintf(src+n, sizeof src-n, "pcicfg r%d, %s, %s\n", (int)(rnd()%16), bdf[rnd()%NB], a); break;
                case 20: n += snprintf(src+n, sizeof src-n, "delay %s\n", a); break;
                case 21: n += snprintf(src+n, sizeof src-n, "print \"p\", %s\n", a); break;
                default: n += snprintf(src+n, sizeof src-n, "halt\n"); break;
            }
        }
        if (dvm_assemble(src, strlen(src), &P, &E) != 0) continue;
        assembled++;
        CHECK_EQ(dvm_program_validate(&P, &E), DVM_OK);
        mark_ops(&P);
        dev_reset();
        dvm_status_t s = dvm_run(&P, &POL, &DEV, NULL, 0, &R);
        check_bounded(s);
        if (s >= 0 && s < DVM_STATUS__COUNT) status_hist[s]++;
    }
    printf("    (generated fuzz: %d/8000 assembled;", assembled);
    for (int i = 0; i < DVM_STATUS__COUNT; i++)
        if (status_hist[i]) printf(" %s=%d", dvm_status_name(i), status_hist[i]);
    printf(")\n");
    CHECK(assembled > 5000);
    /* Hostile programs must reach the interesting refusals, not just halt. */
    CHECK(status_hist[DVM_TRAP_PORT_DENIED] > 0);
    CHECK(status_hist[DVM_TRAP_MMIO_DENIED] > 0);
    CHECK(status_hist[DVM_TRAP_STEPS] > 0);
    CHECK(status_hist[DVM_OK] > 0);
}

/* Structurally valid programs built directly, bypassing the assembler, with
 * random operands: the interpreter's own bounds are what has to hold. */
static void test_fuzz_programs(void) {
    static dvm_program_t prog;
    std_policy();
    POL.max_steps = 2000;
    POL.max_io    = 128;

    for (int iter = 0; iter < 8000; iter++) {
        memset(&prog, 0, sizeof prog);
        prog.nstr = 1;
        prog.stroff[0] = 0;
        prog.strpool[0] = 'x';
        prog.strpool[1] = '\0';
        prog.strused = 2;
        uint32_t n = 1 + (rnd() % 32);
        prog.ninsn = n;
        for (uint32_t i = 0; i < n; i++) {
            dvm_insn_t *in = &prog.insn[i];
            in->op   = (uint8_t)(rnd() % DVM_OP__COUNT);
            in->dst  = (uint8_t)(rnd() % DVM_NREGS);
            in->line = i + 1;
            int fmt_lbl = (in->op >= DVM_JMP && in->op <= DVM_CALL);
            int fmt_str = (in->op == DVM_ABORT || in->op == DVM_PRINT);
            for (int k = 0; k < 3; k++) {
                uint32_t r = rnd() % 3;
                if (k == 0 && fmt_lbl) { in->kind[0] = DVM_O_PC; in->val[0] = rnd() % n; }
                else if (k == 0 && fmt_str) { in->kind[0] = DVM_O_STR; in->val[0] = 0; }
                else if (r == 0) { in->kind[k] = DVM_O_NONE; in->val[k] = 0; }
                else if (r == 1) { in->kind[k] = DVM_O_REG; in->val[k] = rnd() % DVM_NREGS; }
                else {
                    in->kind[k] = DVM_O_IMM;
                    switch (rnd() % 6) {
                        case 0: in->val[k] = rnd(); break;
                        case 1: in->val[k] = 0xB8000; break;
                        case 2: in->val[k] = MMIO_BASE + (rnd() % 0x2000); break;
                        case 3: in->val[k] = 0xC000 + (rnd() % 0x100); break;
                        case 4: in->val[k] = (uint64_t)-1; break;
                        default: in->val[k] = rnd() % 0x10000; break;
                    }
                }
            }
        }
        /* The validator is allowed to refuse these; if it accepts, running one
         * must still respect every bound. */
        if (dvm_program_validate(&prog, &E) != DVM_OK) continue;
        mark_ops(&prog);
        dev_reset();
        check_bounded(dvm_run(&prog, &POL, &DEV, NULL, 0, &R));
    }
}

/* Take a good program and corrupt one byte of its encoding at a time. */
static void test_fuzz_corruption(void) {
    static dvm_program_t good, bad;
    std_policy();
    POL.max_steps = 500;
    CHECK_EQ(assemble("start:\n"
                      "  mov r1, 0xc000\n"
                      "  out8 r1, 0x5a\n"
                      "  in8 r2, r1\n"
                      "  ld32 r3, [0xfebc0000]\n"
                      "  st32 [0xfebc0004], r3\n"
                      "  pcicfg r4, 00:03.0, 0\n"
                      "  cmp r2, 0\n"
                      "  bne start\n"
                      "  print \"p\", r3\n"
                      "  halt\n"), 0);
    memcpy(&good, &P, sizeof good);

    unsigned char *base = (unsigned char *)&good;
    size_t span = (size_t)((unsigned char *)&good.insn[good.ninsn] - base);
    for (int iter = 0; iter < 6000; iter++) {
        memcpy(&bad, &good, sizeof bad);
        int nflip = 1 + (int)(rnd() % 3);
        for (int f = 0; f < nflip; f++) {
            size_t off = rnd() % span;
            ((unsigned char *)&bad)[off] = (unsigned char)(rnd() & 0xFF);
        }
        dev_reset();
        dvm_status_t s = dvm_run(&bad, &POL, &DEV, NULL, 0, &R);
        check_bounded(s);
    }
}

/* ====================================================================== */

int main(void) {
    console_init();
    trace_set_enabled(1);

    /* Must be first: it is the only chance to observe the never-armed guard. */
    RUN(test_dma_check_before_any_run);

    RUN(test_assemble_basic);
    RUN(test_disasm);
    RUN(test_disasm_neutralises_string_bytes);
    RUN(test_all_mnemonics);
    RUN(test_number_forms);
    RUN(test_address_sugar);
    RUN(test_strings_and_comments);
    RUN(test_bdf_literal);
    RUN(test_assembler_errors);
    RUN(test_assembler_limits);
    RUN(test_source_span_not_terminated);

    RUN(test_alu);
    RUN(test_alu_edges);
    RUN(test_args_and_registers);
    RUN(test_cmp_and_branches);
    RUN(test_test_op);
    RUN(test_stacks);
    RUN(test_halt_abort_nop);

    RUN(test_port_io);
    RUN(test_mmio);
    RUN(test_pcicfg);
    RUN(test_delay);
    RUN(test_print);
    RUN(test_realistic_bringup);

    RUN(test_infinite_loop_terminates);
    RUN(test_denied_ports);
    RUN(test_denied_mmio);
    RUN(test_policy_refusals);
    RUN(test_bad_arguments);
    RUN(test_no_backend);

    RUN(test_dma_region_shape);
    RUN(test_dma_grant_is_bounded_to_the_arena);
    RUN(test_dma_stores_land_in_the_arena);
    RUN(test_a_nested_run_is_refused);
    RUN(test_scratch_survives_a_syscall);
    RUN(test_a_syscall_cannot_erase_dma_evidence);
    RUN(test_the_guard_does_not_leak);
    RUN(test_dma_bounds_alignment_and_budget);
    RUN(test_dma_guard_band_catches_a_device_overrun);
    RUN(test_dma_grant_is_traced);

    RUN(test_scratch_memory_and_strings);
    RUN(test_syscalls_reach_the_kernel);

    RUN(test_validator_rejects_handbuilt);

    RUN(test_trace_io_level);
    RUN(test_trace_all_level);
    RUN(test_trace_traps_and_budget);

    RUN(test_fuzz_bytes);
    RUN(test_fuzz_tokens);
    RUN(test_fuzz_generated);
    RUN(test_fuzz_programs);
    RUN(test_fuzz_corruption);

    /* Opcode coverage: every instruction in the ISA must have appeared in a
     * program that was actually executed, not merely assembled. */
    int missing = 0;
    for (int i = 0; i < DVM_OP__COUNT; i++)
        if (!op_seen[i]) { missing++; printf("    UNCOVERED opcode: %s\n", dvm_op_name(i)); }
    CHECK_EQ(missing, 0);

    /* The load-bearing safety property of the whole module. */
    printf("  (device accesses: %ld, forbidden: %ld)\n", total_acc, forbidden);
    CHECK_EQ(forbidden, 0);
    CHECK_EQ(kpanic_hit, 0);
    CHECK(total_acc > 500);

    return th_report("dvm");
}
