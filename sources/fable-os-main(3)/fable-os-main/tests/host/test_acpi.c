/* test_acpi.c — the ACPI tables, the power plan, and the two irreversible tools,
 * on the host.
 *
 * Runs the REAL drivers/acpi/acpi.c, drivers/acpi/power.c and
 * tools/power_tools.c against the REAL core/tool.c, lib/trace.c and net/json.c.
 * Nothing here is a reimplementation of the code under test.
 *
 * WHY THIS SUITE EXISTS IN THIS FORM
 *   Powering a machine off is the one action that cannot be asserted on after
 *   the fact: if it works, the process doing the asserting stops existing. So
 *   the module is built as "decide, then act" (see include/acpi.h) and this file
 *   attacks the deciding half from three directions:
 *
 *   1. THE TABLES ARE SYNTHETIC AND HOSTILE. A 1 MiB fake physical address space
 *      is built byte by byte — RSDP, RSDT, XSDT, FADT, DSDT, SSDT — and then
 *      broken in every way firmware breaks in the wild: wrong signature, wrong
 *      checksum, a length field that lies, a DSDT pointer into unmapped memory,
 *      an RSDP that is not 16-byte aligned, an XSDT that is corrupt so the RSDT
 *      must be used instead. Every byte the parsers read comes from this array,
 *      through the platform backend, so nothing depends on a real machine.
 *
 *   2. THE FALLBACK ORDER IS A PURE FUNCTION AND IS PINNED AS ONE. Which
 *      mechanism is tried first, that the QEMU ports come last, that a fallback
 *      duplicating the ACPI write is elided, that a reboot always ends in a
 *      triple fault: all asserted directly on acpi_power_plan()'s output.
 *
 *   3. THE ORDERING PROMISE IS CHECKED IN THE ONE PLACE IT IS OBSERVABLE. The
 *      contract is that the kernel's trace line reaches the console BEFORE the
 *      write that stops the machine. The recording backend prints each port
 *      write into the same captured console the trace line goes to, so "before"
 *      becomes a comparison of two offsets in one buffer.
 *
 *   Plus the usual for anything model-facing: every argument is untrusted, every
 *   result buffer is canary-guarded, and the input span is deliberately not
 *   NUL-terminated.
 *
 * Standing in for the linker: Mach-O has no __start_/__stop_ section symbols, so
 * power_tools.c exports a named pointer per tool under FABLEOS_HOSTTEST and this
 * file assembles the table core/tool.c expects. Same trick as test_mem_tools.c.
 */

#include "harness.h"
#include "acpi.h"
#include "tool.h"
#include "trace.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* stand in for the linker-collected tool table                          */
/* ====================================================================== */

extern const tool_t *const fableos_hosttool_power_off_tool;
extern const tool_t *const fableos_hosttool_power_reboot_tool;
extern const tool_t *const fableos_hosttool_power_info_tool;

#if defined(__APPLE__)
#  define SYMPFX "_"
#  define TBL_SECTION __attribute__((section("__DATA,__data")))
#else
#  define SYMPFX ""
#  define TBL_SECTION
#endif

const tool_t *__start_tool_table[3] TBL_SECTION = { 0, 0, 0 };
extern const tool_t *const __stop_tool_table[];

__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 24\n");
_Static_assert(sizeof(__start_tool_table) == 24,
               "tool count changed: update the __stop_tool_table byte offset");

static void install_tool_table(void) {
    __start_tool_table[0] = fableos_hosttool_power_off_tool;
    __start_tool_table[1] = fableos_hosttool_power_reboot_tool;
    __start_tool_table[2] = fableos_hosttool_power_info_tool;
}

/* ====================================================================== */
/* a synthetic machine                                                    */
/* ====================================================================== */

/* One flat megabyte, addressed exactly as physical memory: fake_mem[phys]. That
 * covers everything real discovery looks at — the EBDA pointer at 0x40E, the
 * EBDA itself, and the 0xE0000-0xFFFFF BIOS window — with room left over for
 * tables to live at plausible addresses. */
#define FAKE_MEM_SIZE 0x100000u
static uint8_t fake_mem[FAKE_MEM_SIZE];

/* Every port write, memory write, pulse and fault, in the order they happened.
 * io_write and friends also print into the captured console, which is how the
 * "trace line first" ordering assertion is made. */
#define REC_MAX 64
typedef struct {
    char     kind[16];         /* "io", "mem8", "kbd", "triple" */
    uint64_t address;
    uint32_t value;
    uint8_t  width;
} rec_t;

static rec_t  rec[REC_MAX];
static int    rec_n;
static uint32_t io_read_default = 0x60;   /* UART "transmitter idle" */
static uint32_t delay_total_ms;

/* When set, a write of the FADT's ACPI_ENABLE value to the SMI command port
 * makes SCI_EN appear in PM1a_CNT — which is exactly what QEMU's PIIX4 does
 * (hw/isa/apm.c calls acpi_pm1_cnt_update on an APM control write). */
static int smi_flips_sci_en;

/* Programmable port reads: an exact match wins, else io_read_default. */
#define PORTVAL_MAX 8
static struct { uint32_t port; uint32_t value; int set; } portval[PORTVAL_MAX];

static void set_portval(uint32_t port, uint32_t value) {
    for (int i = 0; i < PORTVAL_MAX; i++)
        if (!portval[i].set || portval[i].port == port) {
            portval[i].port = port;
            portval[i].value = value;
            portval[i].set = 1;
            return;
        }
}

static void machine_reset(void) {
    memset(fake_mem, 0, sizeof fake_mem);
    memset(rec, 0, sizeof rec);
    memset(portval, 0, sizeof portval);
    rec_n = 0;
    delay_total_ms = 0;
    io_read_default = 0x60;
    smi_flips_sci_en = 0;
}

static void record(const char *kind, uint64_t address, uint32_t value,
                   uint8_t width) {
    if (rec_n < REC_MAX) {
        snprintf(rec[rec_n].kind, sizeof rec[rec_n].kind, "%s", kind);
        rec[rec_n].address = address;
        rec[rec_n].value   = value;
        rec[rec_n].width   = width;
        rec_n++;
    }
    /* Into the captured console too, so the ordering of a write relative to a
     * trace line and to the prepare hook is a property of one string.
     *
     * The 64-bit address is pre-formatted rather than passed as %llx: kprintf
     * here is the kernel's console API, and lib/base.c's formatter has no length
     * modifiers — it would print "%llx" literally and consume no argument,
     * shifting the two after it. kshim.c fails the suite for exactly this. */
    char addr[24];
    snprintf(addr, sizeof addr, "0x%llx", (unsigned long long)address);
    kprintf("RECORD %s %s <- 0x%x/%u\n", kind, addr,
            (unsigned)value, (unsigned)width);
}

static int fake_mem_read(uint64_t phys, void *dst, size_t len) {
    if (!dst || len == 0)                     return ACPI_EINVAL;
    if (phys >= FAKE_MEM_SIZE)                return ACPI_EINVAL;
    if (len > FAKE_MEM_SIZE - phys)           return ACPI_EINVAL;
    memcpy(dst, fake_mem + phys, len);
    return ACPI_OK;
}

static int fake_mem_write8(uint64_t phys, uint8_t value) {
    record("mem8", phys, value, 8);
    return ACPI_OK;
}

static void fake_io_write(uint32_t port, uint32_t value, uint8_t width) {
    record("io", port, value, width);
    if (smi_flips_sci_en && port == 0xB2 && value == 0xF1)
        set_portval(0x604, ACPI_PM1_CNT_SCI_EN);
}

static uint32_t fake_io_read(uint32_t port, uint8_t width) {
    (void)width;
    for (int i = 0; i < PORTVAL_MAX; i++)
        if (portval[i].set && portval[i].port == port) return portval[i].value;
    return io_read_default;
}

static void fake_kbd_pulse(void) { record("kbd", 0x64, 0xFE, 8); }

/* NOTE: this returns, unlike the real one. The plan's last step is a triple
 * fault precisely because the CPU cannot refuse it; a host process can, and
 * returning is what lets the test see what came after. */
static void fake_triple_fault(void) { record("triple", 0, 0, 0); }

static void fake_delay(uint32_t ms) { delay_total_ms += ms; }

static const acpi_platform_t fake_platform = {
    .mem_read     = fake_mem_read,
    .mem_write8   = fake_mem_write8,
    .io_write     = fake_io_write,
    .io_read      = fake_io_read,
    .kbd_pulse    = fake_kbd_pulse,
    .triple_fault = fake_triple_fault,
    .delay_ms     = fake_delay,
};

/* ====================================================================== */
/* table builders                                                         */
/* ====================================================================== */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* Make `len` bytes at `p` sum to zero by adjusting the standard checksum byte
 * (offset 9 for a table header, offset 8 for an RSDP). */
static void fix_sum(uint8_t *p, size_t len, size_t sum_off) {
    p[sum_off] = 0;
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++) s = (uint8_t)(s + p[i]);
    p[sum_off] = (uint8_t)(0u - s);
}

static void hdr(uint8_t *p, const char *sig, uint32_t len, uint8_t rev) {
    memset(p, 0, 36);
    memcpy(p, sig, 4);
    put32(p + 4, len);
    p[8] = rev;
    memcpy(p + 10, "FABLE ", 6);          /* OEM ID    */
    memcpy(p + 16, "TESTTBL0", 8);        /* OEM table */
    put32(p + 24, 1);                     /* OEM revision */
    memcpy(p + 28, "TEST", 4);
    put32(p + 32, 1);
}

/* An ACPI 1.0 (revision 0) RSDP pointing at `rsdt`. */
static void build_rsdp1(uint64_t at, uint32_t rsdt) {
    uint8_t *p = fake_mem + at;
    memset(p, 0, 36);
    memcpy(p, "RSD PTR ", 8);
    memcpy(p + 9, "FABLE ", 6);
    p[15] = 0;
    put32(p + 16, rsdt);
    fix_sum(p, 20, 8);
}

/* An ACPI 2.0 (revision 2) RSDP with both pointers and both checksums. */
static void build_rsdp2(uint64_t at, uint32_t rsdt, uint64_t xsdt) {
    uint8_t *p = fake_mem + at;
    memset(p, 0, 36);
    memcpy(p, "RSD PTR ", 8);
    memcpy(p + 9, "FABLE ", 6);
    p[15] = 2;
    put32(p + 16, rsdt);
    put32(p + 20, 36);                 /* length */
    put64(p + 24, xsdt);
    fix_sum(p, 20, 8);                 /* the 1.0 checksum first ... */
    fix_sum(p, 36, 32);                /* ... then the extended one  */
}

/* An RSDT or XSDT listing `n` tables. */
static void build_sdt(uint64_t at, int is_xsdt, const uint64_t *entries, int n) {
    uint8_t *p     = fake_mem + at;
    uint32_t entsz = is_xsdt ? 8 : 4;
    uint32_t len   = 36 + (uint32_t)n * entsz;
    hdr(p, is_xsdt ? "XSDT" : "RSDT", len, 1);
    for (int i = 0; i < n; i++) {
        if (is_xsdt) put64(p + 36 + i * 8, entries[i]);
        else         put32(p + 36 + i * 4, (uint32_t)entries[i]);
    }
    fix_sum(p, len, 9);
}

/* A revision-1 FADT: 116 bytes, no reset register, 32-bit fields only. */
static void build_fadt1(uint64_t at, uint32_t dsdt, uint32_t pm1a, uint32_t pm1b) {
    uint8_t *p = fake_mem + at;
    hdr(p, "FACP", 116, 1);
    memset(p + 36, 0, 116 - 36);
    put32(p + 40, dsdt);
    p[46] = 9;                       /* SCI_INT       */
    put32(p + 48, 0xB2);             /* SMI_CMD       */
    p[52] = 0xF1;                    /* ACPI_ENABLE   */
    p[53] = 0xF0;                    /* ACPI_DISABLE  */
    put32(p + 64, pm1a);
    put32(p + 68, pm1b);
    p[89] = 2;                       /* PM1_CNT_LEN   */
    fix_sum(p, 116, 9);
}

/* A revision-3 FADT: 276 bytes, X_ fields filled in, and a reset register. */
static void build_fadt3(uint64_t at, uint32_t dsdt32, uint64_t xdsdt,
                        uint32_t pm1a32, uint64_t xpm1a,
                        int reset_space, uint64_t reset_addr,
                        uint8_t reset_width, uint8_t reset_val, int reset_flag) {
    uint8_t *p = fake_mem + at;
    hdr(p, "FACP", 276, 3);
    memset(p + 36, 0, 276 - 36);
    put32(p + 40, dsdt32);
    put32(p + 48, 0xB2);
    p[52] = 0xF1;
    put32(p + 64, pm1a32);
    p[89] = 2;
    put32(p + 112, reset_flag ? ACPI_FADT_RESET_REG_SUP : 0);
    /* RESET_REG, a GAS at offset 116 */
    p[116] = (uint8_t)reset_space;
    p[117] = reset_width;
    p[118] = 0;
    p[119] = 1;
    put64(p + 120, reset_addr);
    p[128] = reset_val;
    put64(p + 140, xdsdt);                       /* X_DSDT              */
    if (xpm1a) {                                 /* X_PM1a_CNT_BLK, GAS */
        p[172] = ACPI_SPACE_IO;
        p[173] = 16;
        p[175] = 2;
        put64(p + 176, xpm1a);
    }
    fix_sum(p, 276, 9);
}

/* A DSDT/SSDT whose AML body is `aml`, optionally preceded by `pad` filler
 * bytes so a match can be placed at a chosen offset. */
static void build_aml_table(uint64_t at, const char *sig, const uint8_t *aml,
                            size_t amllen, size_t pad) {
    uint8_t *p   = fake_mem + at;
    uint32_t len = (uint32_t)(36 + pad + amllen);
    hdr(p, sig, len, 2);
    for (size_t i = 0; i < pad; i++) p[36 + i] = 0xA5;
    memcpy(p + 36 + pad, aml, amllen);
    fix_sum(p, len, 9);
}

/* QEMU's shape: Name(_S5, Package(4){0,0,0,0}) */
static const uint8_t S5_ZEROS[] = {
    0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00
};
/* \_S5_ with a root prefix and byte constants: Package(2){5,5} */
static const uint8_t S5_ROOT_FIVES[] = {
    0x08, 0x5C, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 0x05, 0x0A, 0x05
};

/* Standard layout used by most of the discovery tests. */
#define AT_RSDP  0xE0000u
#define AT_RSDT  0x7A000u
#define AT_XSDT  0x7A400u
#define AT_FADT  0x7B000u
#define AT_DSDT  0x7C000u
#define AT_SSDT  0x7E000u

/* QEMU's shape, as read off a live boot: a revision-0 RSDP in the BIOS window,
 * an RSDT with one FADT, PM1a_CNT at 0x604, and \_S5_ = {0,0} in the DSDT.
 *
 * PM1a_CNT is primed with SCI_EN already set, i.e. a board that is already in
 * ACPI mode, so tests that are about the power plan see only the writes the plan
 * asked for. The legacy-mode transition (which is what a real QEMU boot goes
 * through, since SeaBIOS leaves SCI_EN clear) is exercised in test_enable_hw. */
static void build_standard_machine(void) {
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    uint64_t ents[1] = { AT_FADT };
    build_sdt(AT_RSDT, 0, ents, 1);
    set_portval(0x604, ACPI_PM1_CNT_SCI_EN);
    acpi_set_platform(&fake_platform);
    acpi_forget();
}

/* ====================================================================== */
/* 1. checksums                                                           */
/* ====================================================================== */

static void test_checksum(void) {
    uint8_t z[4] = { 0, 0, 0, 0 };
    CHECK_EQ(acpi_checksum(z, 4), 0);
    CHECK(acpi_checksum_ok(z, 4));

    uint8_t b[4] = { 0x10, 0x20, 0x30, 0xA0 };   /* sums to 0x100 == 0 */
    CHECK_EQ(acpi_checksum(b, 4), 0);
    CHECK(acpi_checksum_ok(b, 4));

    b[0]++;                                      /* one bit of rot */
    CHECK_EQ(acpi_checksum(b, 4), 1);
    CHECK(!acpi_checksum_ok(b, 4));

    /* Degenerate arguments must be a failure, never a pass by accident. */
    CHECK(!acpi_checksum_ok(NULL, 4));
    CHECK(!acpi_checksum_ok(b, 0));

    /* A real table header validates over its own declared length, and one
     * flipped byte anywhere inside it fails. */
    machine_reset();
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    CHECK(acpi_checksum_ok(fake_mem + AT_FADT, 116));
    for (int off = 0; off < 116; off += 7) {
        fake_mem[AT_FADT + off] = (uint8_t)(fake_mem[AT_FADT + off] ^ 0xFF);
        CHECK(!acpi_checksum_ok(fake_mem + AT_FADT, 116));
        fake_mem[AT_FADT + off] = (uint8_t)(fake_mem[AT_FADT + off] ^ 0xFF);
    }
    CHECK(acpi_checksum_ok(fake_mem + AT_FADT, 116));
}

/* ====================================================================== */
/* 2. the RSDP                                                            */
/* ====================================================================== */

static void test_rsdp_valid(void) {
    acpi_info_t in;
    machine_reset();
    build_rsdp1(AT_RSDP, 0x12345678);

    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_rsdp_check(fake_mem + AT_RSDP, 36, &in), ACPI_OK);
    CHECK_EQ(in.rsdp_revision, 0);
    CHECK_STR(in.oem_id, "FABLE ");
    CHECK_EQ(in.rsdt_phys, 0x12345678);
    CHECK_EQ(in.xsdt_phys, 0);          /* a 1.0 RSDP has no XSDT to trust */
    /* present is the caller's business, not the validator's. */
    CHECK_EQ(in.present, 0);

    /* 20 bytes is all a revision-0 RSDP needs. */
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_rsdp_check(fake_mem + AT_RSDP, 20, &in), ACPI_OK);

    build_rsdp2(AT_RSDP, 0x11112222, 0x000000007fff0000ull);
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_rsdp_check(fake_mem + AT_RSDP, 36, &in), ACPI_OK);
    CHECK_EQ(in.rsdp_revision, 2);
    CHECK_EQ(in.rsdt_phys, 0x11112222);
    CHECK_EQ(in.xsdt_phys, 0x7fff0000ull);
}

static void test_rsdp_rejects(void) {
    acpi_info_t in;
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    uint8_t *p = fake_mem + AT_RSDP;

    CHECK_EQ(acpi_rsdp_check(NULL, 36, &in), ACPI_EINVAL);
    CHECK_EQ(acpi_rsdp_check(p, 36, NULL), ACPI_EINVAL);
    CHECK_EQ(acpi_rsdp_check(p, 19, &in), ACPI_EINVAL);   /* too short */

    p[3] = 'X';                                          /* "RSDXPTR " */
    CHECK_EQ(acpi_rsdp_check(p, 36, &in), ACPI_EINVAL);
    p[3] = ' ';
    CHECK_EQ(acpi_rsdp_check(p, 36, &in), ACPI_OK);

    p[8]++;                                              /* checksum rot */
    CHECK_EQ(acpi_rsdp_check(p, 36, &in), ACPI_EINVAL);
    p[8]--;

    /* A revision-2 RSDP whose extended checksum is wrong is not usable, even
     * though its first 20 bytes still validate. Believing it would mean
     * following a 64-bit XSDT pointer nobody has vouched for. */
    build_rsdp2(AT_RSDP, AT_RSDT, AT_XSDT);
    CHECK_EQ(acpi_rsdp_check(p, 36, &in), ACPI_OK);
    p[32]++;
    CHECK_EQ(acpi_rsdp_check(p, 36, &in), ACPI_EINVAL);
    p[32]--;

    /* ...and one that lies about its own length is rejected outright, in both
     * directions: too small to be a 2.0 RSDP, or larger than what was read. */
    put32(p + 20, 35);
    fix_sum(p, 36, 32);
    CHECK_EQ(acpi_rsdp_check(p, 36, &in), ACPI_EINVAL);
    put32(p + 20, 0xFFFFFFF0u);
    fix_sum(p, 36, 32);
    CHECK_EQ(acpi_rsdp_check(p, 36, &in), ACPI_EINVAL);
    put32(p + 20, 36);
    fix_sum(p, 36, 32);
    CHECK_EQ(acpi_rsdp_check(p, 36, &in), ACPI_OK);

    /* A 2.0 RSDP that only 20 bytes were read from cannot be validated. */
    CHECK_EQ(acpi_rsdp_check(p, 20, &in), ACPI_EINVAL);
}

/* ====================================================================== */
/* 3. the FADT                                                            */
/* ====================================================================== */

static void test_fadt_rev1(void) {
    acpi_info_t in;
    machine_reset();
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0x608);

    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 116, &in), ACPI_OK);
    CHECK_EQ(in.fadt_revision, 1);
    CHECK_EQ(in.fadt_len, 116);
    CHECK_EQ(in.dsdt_phys, AT_DSDT);
    CHECK_EQ(in.pm1a_cnt, 0x604);
    CHECK_EQ(in.pm1b_cnt, 0x608);
    CHECK_EQ(in.pm1_cnt_len, 2);
    CHECK_EQ(in.smi_cmd, 0xB2);
    CHECK_EQ(in.acpi_enable, 0xF1);
    CHECK_EQ(in.acpi_disable, 0xF0);
    CHECK_EQ(in.sci_int, 9);
    /* A 116-byte FADT stops before the reset register exists. */
    CHECK_EQ(in.reset_supported, 0);
    CHECK_EQ(in.reset_value, 0);
    /* Not this function's business. */
    CHECK_EQ(in.s5_found, 0);
}

static void test_fadt_rev3_prefers_64bit(void) {
    acpi_info_t in;
    machine_reset();
    /* The 32-bit fields say one thing, the X_ fields another. The 64-bit ones
     * win — that is the entire point of their existing. */
    build_fadt3(AT_FADT, 0x1000, 0x2000, 0x604, 0x504,
                ACPI_SPACE_IO, 0xCF9, 8, 0x0F, 1);

    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 276, &in), ACPI_OK);
    CHECK_EQ(in.fadt_revision, 3);
    CHECK_EQ(in.dsdt_phys, 0x2000);
    CHECK_EQ(in.pm1a_cnt, 0x504);
    CHECK_EQ(in.reset_supported, 1);
    CHECK_EQ(in.reset_reg.space_id, ACPI_SPACE_IO);
    CHECK_EQ(in.reset_reg.address, 0xCF9);
    CHECK_EQ(in.reset_reg.bit_width, 8);
    CHECK_EQ(in.reset_value, 0x0F);

    /* A zeroed X_ field means "use the legacy one", not "the address is 0". */
    machine_reset();
    build_fadt3(AT_FADT, 0x1000, 0, 0x604, 0, ACPI_SPACE_IO, 0xCF9, 8, 0x0F, 1);
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 276, &in), ACPI_OK);
    CHECK_EQ(in.dsdt_phys, 0x1000);
    CHECK_EQ(in.pm1a_cnt, 0x604);
}

static void test_fadt_reset_register_conditions(void) {
    acpi_info_t in;

    /* The flag alone is not enough: a zeroed GAS with the flag set is common. */
    machine_reset();
    build_fadt3(AT_FADT, 0x1000, 0, 0x604, 0, ACPI_SPACE_IO, 0, 8, 0x0F, 1);
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 276, &in), ACPI_OK);
    CHECK_EQ(in.reset_supported, 0);

    /* A usable GAS is not enough either: without the flag the firmware has not
     * said the register may be written. */
    machine_reset();
    build_fadt3(AT_FADT, 0x1000, 0, 0x604, 0, ACPI_SPACE_IO, 0xCF9, 8, 0x0F, 0);
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 276, &in), ACPI_OK);
    CHECK_EQ(in.reset_supported, 0);

    /* A width this machine cannot issue is not usable. */
    machine_reset();
    build_fadt3(AT_FADT, 0x1000, 0, 0x604, 0, ACPI_SPACE_IO, 0xCF9, 7, 0x0F, 1);
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 276, &in), ACPI_OK);
    CHECK_EQ(in.reset_supported, 0);

    /* An address space that is neither memory nor I/O (3 = embedded controller)
     * is not reachable from here. */
    machine_reset();
    build_fadt3(AT_FADT, 0x1000, 0, 0x604, 0, 3, 0x62, 8, 0x0F, 1);
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 276, &in), ACPI_OK);
    CHECK_EQ(in.reset_supported, 0);

    /* An I/O port above 0xFFFF cannot be issued by an x86 out instruction. */
    machine_reset();
    build_fadt3(AT_FADT, 0x1000, 0, 0x604, 0, ACPI_SPACE_IO, 0x10CF9, 8, 0x0F, 1);
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 276, &in), ACPI_OK);
    CHECK_EQ(in.reset_supported, 0);

    /* Memory-mapped reset registers are real (and common on modern boards). */
    machine_reset();
    build_fadt3(AT_FADT, 0x1000, 0, 0x604, 0, ACPI_SPACE_MEM, 0xFED0002Cull,
                8, 0x02, 1);
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_parse_fadt(fake_mem + AT_FADT, 276, &in), ACPI_OK);
    CHECK_EQ(in.reset_supported, 1);
    CHECK_EQ(in.reset_reg.space_id, ACPI_SPACE_MEM);
    CHECK_EQ(in.reset_reg.address, 0xFED0002Cull);
}

static void test_fadt_rejects(void) {
    acpi_info_t in;
    machine_reset();
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    uint8_t *p = fake_mem + AT_FADT;

    CHECK_EQ(acpi_parse_fadt(NULL, 116, &in), ACPI_EINVAL);
    CHECK_EQ(acpi_parse_fadt(p, 116, NULL), ACPI_EINVAL);
    CHECK_EQ(acpi_parse_fadt(p, 35, &in), ACPI_EINVAL);       /* no header   */

    memcpy(p, "DSDT", 4);
    CHECK_EQ(acpi_parse_fadt(p, 116, &in), ACPI_EINVAL);      /* not a FADT  */
    memcpy(p, "FACP", 4);
    CHECK_EQ(acpi_parse_fadt(p, 116, &in), ACPI_OK);

    p[9]++;                                                   /* checksum    */
    CHECK_EQ(acpi_parse_fadt(p, 116, &in), ACPI_EINVAL);
    p[9]--;

    /* A table that declares more bytes than were read cannot be checksummed,
     * so it is refused rather than parsed from a prefix. */
    put32(p + 4, 400);
    fix_sum(p, 116, 9);
    CHECK_EQ(acpi_parse_fadt(p, 116, &in), ACPI_EINVAL);

    /* And one that declares less than a FADT's mandatory fields is not one. */
    put32(p + 4, 40);
    fix_sum(p, 40, 9);
    CHECK_EQ(acpi_parse_fadt(p, 116, &in), ACPI_EINVAL);
}

/* ====================================================================== */
/* 4. \_S5_ in AML                                                        */
/* ====================================================================== */

static void test_s5_shapes(void) {
    uint8_t a = 0xEE, b = 0xEE;

    /* QEMU's own: Package(4){0,0,0,0} — SLP_TYP 0 is soft-off on PIIX4. */
    CHECK(acpi_find_s5(S5_ZEROS, sizeof S5_ZEROS, "_S5_", &a, &b) >= 0);
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 0);

    /* A fully-qualified name and byte constants. */
    a = b = 0xEE;
    CHECK(acpi_find_s5(S5_ROOT_FIVES, sizeof S5_ROOT_FIVES, "_S5_", &a, &b) >= 0);
    CHECK_EQ(a, 5);
    CHECK_EQ(b, 5);

    /* One element: PM1b gets the same sleep type, which is what a board with
     * no PM1b block wants anyway. */
    const uint8_t one[] = { 0x08, '_','S','5','_', 0x12, 0x04, 0x01, 0x0A, 0x07 };
    a = b = 0xEE;
    CHECK(acpi_find_s5(one, sizeof one, "_S5_", &a, &b) >= 0);
    CHECK_EQ(a, 7);
    CHECK_EQ(b, 7);

    /* Different values for the two blocks are honoured independently. */
    const uint8_t two[] = { 0x08, '_','S','5','_', 0x12, 0x06, 0x02,
                            0x0A, 0x02, 0x0A, 0x03 };
    a = b = 0xEE;
    CHECK(acpi_find_s5(two, sizeof two, "_S5_", &a, &b) >= 0);
    CHECK_EQ(a, 2);
    CHECK_EQ(b, 3);

    /* OneOp / ZeroOp mixed with a prefix constant. */
    const uint8_t mixed[] = { 0x08, '_','S','5','_', 0x12, 0x05, 0x02, 0x01, 0x00 };
    a = b = 0xEE;
    CHECK(acpi_find_s5(mixed, sizeof mixed, "_S5_", &a, &b) >= 0);
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 0);

    /* A multi-byte PkgLength (lead byte 0x41 => one extra length byte). */
    const uint8_t multi[] = { 0x08, '_','S','5','_', 0x12, 0x41, 0x08, 0x02,
                              0x0A, 0x04, 0x0A, 0x04 };
    a = b = 0xEE;
    CHECK(acpi_find_s5(multi, sizeof multi, "_S5_", &a, &b) >= 0);
    CHECK_EQ(a, 4);
    CHECK_EQ(b, 4);

    /* The name is a parameter: the same matcher finds \_S3_ for a future
     * suspend-to-RAM path and does not confuse the two. */
    const uint8_t s3[] = { 0x08, '_','S','3','_', 0x12, 0x06, 0x02,
                           0x0A, 0x01, 0x0A, 0x01 };
    a = b = 0xEE;
    CHECK(acpi_find_s5(s3, sizeof s3, "_S3_", &a, &b) >= 0);
    CHECK_EQ(a, 1);
    CHECK_EQ(acpi_find_s5(s3, sizeof s3, "_S5_", &a, &b), ACPI_ENOENT);

    /* The returned offset is the NameOp's, and it survives leading filler. */
    uint8_t padded[64];
    memset(padded, 0xA5, sizeof padded);
    memcpy(padded + 17, S5_ZEROS, sizeof S5_ZEROS);
    CHECK_EQ(acpi_find_s5(padded, sizeof padded, "_S5_", &a, &b), 17);
}

static void test_s5_rejects(void) {
    uint8_t a = 0xEE, b = 0xEE;

    CHECK_EQ(acpi_find_s5(NULL, 12, "_S5_", &a, &b), ACPI_ENOENT);
    CHECK_EQ(acpi_find_s5(S5_ZEROS, sizeof S5_ZEROS, NULL, &a, &b), ACPI_ENOENT);
    CHECK_EQ(acpi_find_s5(S5_ZEROS, 0, "_S5_", &a, &b), ACPI_ENOENT);

    /* An AML name is four characters. A shorter one is a caller bug, and it is
     * refused rather than compared — the comparison reads four bytes. */
    CHECK_EQ(acpi_find_s5(S5_ZEROS, sizeof S5_ZEROS, "", &a, &b), ACPI_EINVAL);
    CHECK_EQ(acpi_find_s5(S5_ZEROS, sizeof S5_ZEROS, "_S5", &a, &b), ACPI_EINVAL);
    /* A longer one is fine: only the first four characters are significant. */
    CHECK(acpi_find_s5(S5_ZEROS, sizeof S5_ZEROS, "_S5_XXX", &a, &b) >= 0);

    /* The name without a NameOp in front of it is a reference, not a
     * declaration, and carries no package to read. */
    const uint8_t ref[] = { 0x5B, '_','S','5','_', 0x12, 0x06, 0x04, 0x00, 0x00 };
    CHECK_EQ(acpi_find_s5(ref, sizeof ref, "_S5_", &a, &b), ACPI_ENOENT);

    /* Declared as something other than a package (a byte constant here). */
    const uint8_t notpkg[] = { 0x08, '_','S','5','_', 0x0A, 0x05 };
    CHECK_EQ(acpi_find_s5(notpkg, sizeof notpkg, "_S5_", &a, &b), ACPI_ENOENT);

    /* An empty package has no sleep type in it. */
    const uint8_t empty[] = { 0x08, '_','S','5','_', 0x12, 0x02, 0x00 };
    CHECK_EQ(acpi_find_s5(empty, sizeof empty, "_S5_", &a, &b), ACPI_ENOENT);

    /* A first element that is not an integer constant at all. */
    const uint8_t nonint[] = { 0x08, '_','S','5','_', 0x12, 0x06, 0x02,
                               0x5B, 0x01, 0x0A, 0x01 };
    CHECK_EQ(acpi_find_s5(nonint, sizeof nonint, "_S5_", &a, &b), ACPI_ENOENT);

    /* SLP_TYP is a 3-bit field. A wider value is not a sleep type — it is a
     * false positive on the NameOp byte — and writing it would spray bits into
     * the rest of PM1a_CNT. */
    const uint8_t toobig[] = { 0x08, '_','S','5','_', 0x12, 0x06, 0x02,
                               0x0A, 0x55, 0x0A, 0x55 };
    CHECK_EQ(acpi_find_s5(toobig, sizeof toobig, "_S5_", &a, &b), ACPI_ENOENT);
    const uint8_t ones[] = { 0x08, '_','S','5','_', 0x12, 0x04, 0x01, 0xFF };
    CHECK_EQ(acpi_find_s5(ones, sizeof ones, "_S5_", &a, &b), ACPI_ENOENT);

    /* The second element being unreadable does not invalidate the first. */
    const uint8_t half[] = { 0x08, '_','S','5','_', 0x12, 0x06, 0x02, 0x0A, 0x03 };
    a = b = 0xEE;
    CHECK(acpi_find_s5(half, sizeof half, "_S5_", &a, &b) >= 0);
    CHECK_EQ(a, 3);
    CHECK_EQ(b, 3);

    /* Truncation at every possible point must be handled without reading past
     * the end. Each prefix is copied into a heap block of EXACTLY its own size,
     * so an over-read is a genuine out-of-bounds access rather than a peek at
     * more of a static array.
     *
     * A prefix of 9 bytes or more is already a complete declaration — the
     * element count says 4 but only the elements actually present are read, and
     * one is enough — so those legitimately match. Shorter ones cannot. */
    for (size_t cut = 1; cut <= sizeof S5_ZEROS; cut++) {
        uint8_t *heap = malloc(cut);
        CHECK(heap != NULL);
        memcpy(heap, S5_ZEROS, cut);
        a = b = 0xEE;
        int off = acpi_find_s5(heap, cut, "_S5_", &a, &b);
        if (cut < 9) {
            CHECK_EQ(off, ACPI_ENOENT);
        } else {
            CHECK_EQ(off, 0);
            CHECK_EQ(a, 0);
            CHECK_EQ(b, 0);
        }
        free(heap);
    }

    /* No output pointers at all is legal — the caller may only want the offset. */
    CHECK(acpi_find_s5(S5_ZEROS, sizeof S5_ZEROS, "_S5_", NULL, NULL) >= 0);
}

/* ====================================================================== */
/* 5. discovery, end to end, over synthetic firmware                      */
/* ====================================================================== */

static void test_discovery_bios_window(void) {
    build_standard_machine();
    kcap_reset();

    CHECK_EQ(acpi_init(), ACPI_OK);
    const acpi_info_t *in = acpi_info();
    CHECK_EQ(in->present, 1);
    CHECK_EQ(in->rsdp_phys, AT_RSDP);
    CHECK_EQ(in->rsdp_revision, 0);
    CHECK_STR(in->oem_id, "FABLE ");
    CHECK_EQ(in->used_xsdt, 0);
    CHECK_EQ(in->rsdt_phys, AT_RSDT);
    CHECK_EQ(in->table_count, 1);
    CHECK_STR(in->table_sig[0], "FACP");
    CHECK_EQ(in->fadt_phys, AT_FADT);
    CHECK_EQ(in->pm1a_cnt, 0x604);
    CHECK_EQ(in->dsdt_phys, AT_DSDT);
    CHECK_EQ(in->s5_found, 1);
    CHECK_EQ(in->slp_typ_a, 0);
    CHECK_EQ(in->slp_typ_b, 0);
    CHECK_STR(in->s5_from, "DSDT");
    CHECK(acpi_available());

    /* The boot summary has to say the numbers soft-off depends on, or the
     * operator cannot tell a working ACPI path from a lucky fallback. */
    CHECK_CONTAINS(kcap_text(), "acpi: rev 0 \"FABLE \"");
    CHECK_CONTAINS(kcap_text(), "pm1a_cnt=0x604");
    CHECK_CONTAINS(kcap_text(), "s5=yes(a=0 b=0 from DSDT)");
}

static void test_discovery_ebda(void) {
    machine_reset();
    /* The EBDA's segment lives at 0x40E; 0x9FC0 << 4 == 0x9FC00, which is where
     * a PC BIOS really does put it. */
    uint32_t ebda = 0x9FC00;
    fake_mem[0x40E] = 0xC0;
    fake_mem[0x40F] = 0x9F;
    build_rsdp1(ebda + 0x30, AT_RSDT);
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    uint64_t ents[1] = { AT_FADT };
    build_sdt(AT_RSDT, 0, ents, 1);
    acpi_set_platform(&fake_platform);
    acpi_forget();

    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->rsdp_phys, ebda + 0x30);
    CHECK_EQ(acpi_info()->s5_found, 1);

    /* An EBDA segment pointing somewhere impossible is ignored, and the BIOS
     * window is searched instead. */
    fake_mem[0x40E] = 0xFF;
    fake_mem[0x40F] = 0xFF;              /* 0xFFFF0 — above 0xA0000 */
    build_rsdp1(AT_RSDP, AT_RSDT);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->rsdp_phys, AT_RSDP);
}

static void test_discovery_alignment_and_absence(void) {
    /* No tables at all: a legible refusal, not a guess. */
    machine_reset();
    acpi_set_platform(&fake_platform);
    acpi_forget();
    kcap_reset();
    CHECK_EQ(acpi_init(), ACPI_ENOENT);
    CHECK_EQ(acpi_info()->present, 0);
    CHECK(!acpi_available());
    CHECK_CONTAINS(kcap_text(), "acpi: no RSDP");

    /* "Not present" must mean every field is zero, so no consumer can read a
     * pointer that was never vouched for. A revision-2 RSDP whose extended
     * checksum is broken is the case that used to leave residue: its first 20
     * bytes validate, so the parser gets as far as filling in the OEM ID and
     * the RSDT pointer before rejecting it. */
    machine_reset();
    build_rsdp2(AT_RSDP, 0xDEADBEEF, 0xCAFEBABE);
    fake_mem[AT_RSDP + 32]++;                     /* break the 2.0 checksum */
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_ENOENT);
    CHECK_EQ(acpi_info()->present, 0);
    CHECK_EQ(acpi_info()->rsdt_phys, 0);
    CHECK_EQ(acpi_info()->xsdt_phys, 0);
    CHECK_STR(acpi_info()->oem_id, "");

    /* The spec requires 16-byte alignment, and the scan relies on it. An RSDP
     * at an odd offset is not found — worth pinning, because it is the reason
     * scanning 128 KiB is cheap. */
    machine_reset();
    build_rsdp1(AT_RSDP + 8, AT_RSDT);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_ENOENT);

    /* With no platform backend at all (the host default), discovery must fail
     * rather than interpret whatever is at address 0x40E in this process. */
    acpi_set_platform(NULL);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_ENOENT);
    CHECK_EQ(acpi_info()->present, 0);
    acpi_set_platform(&fake_platform);
}

static void test_discovery_xsdt_preferred(void) {
    machine_reset();
    /* Two root tables, listing DIFFERENT FADTs. A revision-2 RSDP means the
     * XSDT is authoritative, so the XSDT's FADT (and its PM1a) must win. */
    build_rsdp2(AT_RSDP, AT_RSDT, AT_XSDT);
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);            /* listed by the XSDT */
    build_fadt1(AT_FADT + 0x200, AT_DSDT, 0x999, 0);    /* listed by the RSDT */
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    uint64_t xents[1] = { AT_FADT };
    uint64_t rents[1] = { AT_FADT + 0x200 };
    build_sdt(AT_XSDT, 1, xents, 1);
    build_sdt(AT_RSDT, 0, rents, 1);
    acpi_forget();

    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->used_xsdt, 1);
    CHECK_EQ(acpi_info()->pm1a_cnt, 0x604);

    /* Corrupt the XSDT's checksum: the walk must fall back to the RSDT and
     * report the other FADT, not give up and not use half of each. */
    fake_mem[AT_XSDT + 9]++;
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->used_xsdt, 0);
    CHECK_EQ(acpi_info()->pm1a_cnt, 0x999);
    CHECK_EQ(acpi_info()->table_count, 1);   /* the aborted walk left no residue */
}

static void test_discovery_s5_in_ssdt(void) {
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    /* A DSDT with no sleep states, and an SSDT that declares them — a real and
     * annoyingly common layout. */
    const uint8_t nothing[] = { 0x10, 0x20, 0x30, 0x40 };
    build_aml_table(AT_DSDT, "DSDT", nothing, sizeof nothing, 0);
    build_aml_table(AT_SSDT, "SSDT", S5_ROOT_FIVES, sizeof S5_ROOT_FIVES, 0);
    uint64_t ents[2] = { AT_FADT, AT_SSDT };
    build_sdt(AT_RSDT, 0, ents, 2);
    acpi_forget();

    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->table_count, 2);
    CHECK_STR(acpi_info()->table_sig[1], "SSDT");
    CHECK_EQ(acpi_info()->s5_found, 1);
    CHECK_EQ(acpi_info()->slp_typ_a, 5);
    CHECK_STR(acpi_info()->s5_from, "SSDT");
}

static void test_discovery_chunked_scan(void) {
    /* The AML scan is chunked (ACPI_SCAN_CHUNK at a time) so a huge DSDT costs
     * no allocation. A match that straddles a chunk boundary must still be
     * found — this is the case an overlap-free implementation gets wrong, and
     * it would silently cost the machine its ACPI power-off path. */
    for (int delta = -4; delta <= 4; delta++) {
        size_t pad = (size_t)(ACPI_SCAN_CHUNK + delta);
        machine_reset();
        build_rsdp1(AT_RSDP, AT_RSDT);
        build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
        build_aml_table(AT_DSDT, "DSDT", S5_ROOT_FIVES, sizeof S5_ROOT_FIVES, pad);
        uint64_t ents[1] = { AT_FADT };
        build_sdt(AT_RSDT, 0, ents, 1);
        acpi_forget();

        CHECK_EQ(acpi_init(), ACPI_OK);
        CHECK_EQ(acpi_info()->s5_found, 1);
        CHECK_EQ(acpi_info()->slp_typ_a, 5);
    }

    /* And one several chunks in, for the plain iteration. */
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS,
                    ACPI_SCAN_CHUNK * 3 + 11);
    uint64_t ents[1] = { AT_FADT };
    build_sdt(AT_RSDT, 0, ents, 1);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->s5_found, 1);
}

static void test_discovery_broken_pointers(void) {
    /* A FADT whose DSDT pointer leads outside readable memory. Discovery must
     * report what it does know (the FADT, PM1a) and NOT claim a sleep type. */
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt1(AT_FADT, 0x7FFFFFF0u, 0x604, 0);
    uint64_t ents[1] = { AT_FADT };
    build_sdt(AT_RSDT, 0, ents, 1);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->fadt_phys, AT_FADT);
    CHECK_EQ(acpi_info()->pm1a_cnt, 0x604);
    CHECK_EQ(acpi_info()->s5_found, 0);
    CHECK(!acpi_available());
    CHECK_EQ(kpanic_hit, 0);

    /* A root table full of null and garbage entries. */
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    uint64_t junk[4] = { 0, 0xFFFFFFF0u, 0x1234, AT_FADT };
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    build_sdt(AT_RSDT, 0, junk, 4);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    /* Only the entries with a plausible header are even counted. */
    CHECK_EQ(acpi_info()->table_count, 1);
    CHECK_EQ(acpi_info()->fadt_phys, AT_FADT);
    CHECK_EQ(acpi_info()->s5_found, 1);

    /* An RSDP whose RSDT pointer is nonsense: present, but nothing else. */
    machine_reset();
    build_rsdp1(AT_RSDP, 0xFFFFFFF0u);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->present, 1);
    CHECK_EQ(acpi_info()->fadt_phys, 0);
    CHECK(!acpi_available());
    CHECK_EQ(kpanic_hit, 0);
}

/* ====================================================================== */
/* 6. the plan: which mechanism, in which order                           */
/* ====================================================================== */

/* QEMU's real numbers, as read off a live boot: PM1a_CNT at 0x604 and an
 * \_S5_ sleep type of 0. */
static void info_qemu_like(acpi_info_t *in) {
    memset(in, 0, sizeof *in);
    in->present     = 1;
    in->pm1a_cnt    = 0x604;
    in->pm1_cnt_len = 2;
    in->s5_found    = 1;
    in->slp_typ_a   = 0;
    in->slp_typ_b   = 0;
}

static void test_plan_power_off_ordering(void) {
    acpi_info_t in;
    acpi_plan_t p;

    /* --- a board where the ACPI write happens to BE the QEMU fallback. The
     * duplicate must be elided, or the log could never show which mechanism
     * stopped the machine. --- */
    info_qemu_like(&in);
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_OFF, &p), 2);
    CHECK_EQ(p.step[0].kind, ACPI_ACT_IO_WRITE);
    CHECK_STR(p.step[0].method, "acpi-pm1a");
    CHECK_EQ(p.step[0].address, 0x604);
    CHECK_EQ(p.step[0].value, ACPI_PM1_CNT_SLP_EN);
    CHECK_EQ(p.step[0].bit_width, 16);
    CHECK_STR(p.step[1].method, "qemu-b004");
    CHECK_EQ(p.step[1].address, 0xB004);

    /* --- a board with its own port and a non-zero sleep type: real ACPI
     * first, then BOTH fallbacks, in the documented order. --- */
    info_qemu_like(&in);
    in.pm1a_cnt  = 0x504;
    in.slp_typ_a = 5;
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_OFF, &p), 3);
    CHECK_STR(p.step[0].method, "acpi-pm1a");
    CHECK_EQ(p.step[0].address, 0x504);
    /* SLP_TYP 5 at bits 12:10, plus SLP_EN at bit 13. */
    CHECK_EQ(p.step[0].value, (5u << 10) | 0x2000u);
    CHECK_STR(p.step[1].method, "qemu-604");
    CHECK_EQ(p.step[1].address, 0x604);
    CHECK_STR(p.step[2].method, "qemu-b004");

    /* --- both control blocks: the spec says write both, and a board with a
     * PM1b block will not go down on PM1a alone. --- */
    info_qemu_like(&in);
    in.pm1a_cnt  = 0x504;
    in.pm1b_cnt  = 0x508;
    in.slp_typ_a = 5;
    in.slp_typ_b = 6;
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_OFF, &p), 4);
    CHECK_STR(p.step[0].method, "acpi-pm1a");
    CHECK_STR(p.step[1].method, "acpi-pm1b");
    CHECK_EQ(p.step[1].address, 0x508);
    CHECK_EQ(p.step[1].value, (6u << 10) | 0x2000u);
    CHECK_STR(p.step[2].method, "qemu-604");
    CHECK_STR(p.step[3].method, "qemu-b004");

    /* --- every step is followed by a settle window, or "it did not work"
     * would be concluded before the hardware had a chance to act. --- */
    for (int i = 0; i < p.n; i++) CHECK(p.step[i].settle_ms > 0);
}

static void test_plan_power_off_degraded(void) {
    acpi_info_t in;
    acpi_plan_t p;

    /* No tables at all: the fallbacks are all that is left, and they are
     * offered — a QEMU guest with unreadable firmware should still go down. */
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_OFF, &p), 2);
    CHECK_STR(p.step[0].method, "qemu-604");
    CHECK_STR(p.step[1].method, "qemu-b004");

    /* A PM1 block but no sleep type: SLP_TYP is not guessable, so there is no
     * ACPI step. This is the case that makes \_S5_ extraction load-bearing
     * rather than decorative. */
    info_qemu_like(&in);
    in.s5_found = 0;
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_OFF, &p), 2);
    CHECK_STR(p.step[0].method, "qemu-604");

    /* A sleep type but no PM1 block: nowhere to write it. */
    info_qemu_like(&in);
    in.pm1a_cnt = 0;
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_OFF, &p), 2);
    CHECK_STR(p.step[0].method, "qemu-604");

    /* present == 0 is not overridden by leftover fields. */
    info_qemu_like(&in);
    in.present = 0;
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_OFF, &p), 2);
    CHECK_STR(p.step[0].method, "qemu-604");

    /* PM1_CNT_LEN drives the access width. 4 bytes means a 32-bit write; a
     * board claiming 1 still gets 16, because an 8-bit write would drop
     * SLP_EN (bit 13) and look like a write that worked. */
    info_qemu_like(&in);
    in.pm1a_cnt = 0x504;
    in.pm1_cnt_len = 4;
    acpi_power_plan(&in, ACPI_POWER_OFF, &p);
    CHECK_EQ(p.step[0].bit_width, 32);
    in.pm1_cnt_len = 1;
    acpi_power_plan(&in, ACPI_POWER_OFF, &p);
    CHECK_EQ(p.step[0].bit_width, 16);
    in.pm1_cnt_len = 0;
    acpi_power_plan(&in, ACPI_POWER_OFF, &p);
    CHECK_EQ(p.step[0].bit_width, 16);

    CHECK_EQ(acpi_power_plan(NULL, ACPI_POWER_OFF, &p), ACPI_EINVAL);
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_OFF, NULL), ACPI_EINVAL);
}

static void test_plan_reboot_ordering(void) {
    acpi_info_t in;
    acpi_plan_t p;

    /* The firmware's own register first, then the 8042, then the one the CPU
     * cannot refuse. */
    info_qemu_like(&in);
    in.reset_supported      = 1;
    in.reset_reg.space_id   = ACPI_SPACE_IO;
    in.reset_reg.bit_width  = 8;
    in.reset_reg.address    = 0xCF9;
    in.reset_value          = 0x0F;
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_REBOOT, &p), 3);
    CHECK_STR(p.step[0].method, "acpi-reset");
    CHECK_EQ(p.step[0].kind, ACPI_ACT_IO_WRITE);
    CHECK_EQ(p.step[0].address, 0xCF9);
    CHECK_EQ(p.step[0].value, 0x0F);
    CHECK_EQ(p.step[0].bit_width, 8);
    CHECK_STR(p.step[1].method, "i8042-pulse");
    CHECK_EQ(p.step[1].kind, ACPI_ACT_KBD_PULSE);
    CHECK_STR(p.step[2].method, "triple-fault");
    CHECK_EQ(p.step[2].kind, ACPI_ACT_TRIPLE_FAULT);

    /* A memory-space reset register becomes a byte write, not a port write. */
    in.reset_reg.space_id = ACPI_SPACE_MEM;
    in.reset_reg.address  = 0xFED0002Cull;
    in.reset_value        = 0x02;
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_REBOOT, &p), 3);
    CHECK_EQ(p.step[0].kind, ACPI_ACT_MEM_WRITE8);
    CHECK_EQ(p.step[0].address, 0xFED0002Cull);
    CHECK_EQ(p.step[0].value, 0x02);

    /* No reset register (which is exactly what QEMU's i440fx FADT provides):
     * straight to the 8042, then the triple fault. */
    info_qemu_like(&in);
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_REBOOT, &p), 2);
    CHECK_STR(p.step[0].method, "i8042-pulse");
    CHECK_STR(p.step[1].method, "triple-fault");

    /* Even with nothing at all discovered, a restart is always possible. That
     * asymmetry with power_off is deliberate: the CPU can be made to reset, but
     * it cannot be made to switch off. */
    memset(&in, 0, sizeof in);
    CHECK_EQ(acpi_power_plan(&in, ACPI_POWER_REBOOT, &p), 2);
    CHECK_EQ(p.step[p.n - 1].kind, ACPI_ACT_TRIPLE_FAULT);
}

static void test_plan_describe(void) {
    acpi_info_t in;
    acpi_plan_t p;
    char        buf[256];

    info_qemu_like(&in);
    in.pm1a_cnt = 0x504;
    in.slp_typ_a = 5;
    acpi_power_plan(&in, ACPI_POWER_OFF, &p);

    size_t n = acpi_plan_describe(&p, buf, sizeof buf);
    CHECK_EQ(n, strlen(buf));
    CHECK_CONTAINS(buf, "acpi-pm1a(io 0x504<-0x3400/16)");
    CHECK_CONTAINS(buf, "qemu-604(io 0x604<-0x2000/16)");
    CHECK_CONTAINS(buf, "qemu-b004");

    /* The description is passed to trace.h as an argument, so it must not be
     * able to contribute bracket characters to a kernel line. */
    CHECK(strchr(buf, '[') == NULL);
    CHECK(strchr(buf, ']') == NULL);

    /* Truncation into a small buffer stays NUL-terminated and in bounds. */
    for (size_t cap = 1; cap < 40; cap++) {
        char small[64];
        memset(small, 0x7E, sizeof small);
        size_t got = acpi_plan_describe(&p, small, cap);
        CHECK(got < cap);
        CHECK_EQ(small[got], '\0');
        CHECK_EQ((unsigned char)small[cap], 0x7E);   /* nothing past cap */
    }
    CHECK_EQ(acpi_plan_describe(&p, buf, 0), 0);
    CHECK_EQ(acpi_plan_describe(NULL, buf, sizeof buf), 0);
    CHECK_STR(buf, "");

    /* Reboot steps that carry no operand render as their method name. */
    info_qemu_like(&in);
    acpi_power_plan(&in, ACPI_POWER_REBOOT, &p);
    acpi_plan_describe(&p, buf, sizeof buf);
    CHECK_STR(buf, "i8042-pulse triple-fault");

    CHECK_STR(acpi_action_kind_name(ACPI_ACT_IO_WRITE), "io-write");
    CHECK_STR(acpi_action_kind_name(ACPI_ACT_MEM_WRITE8), "mem-write8");
    CHECK_STR(acpi_action_kind_name(ACPI_ACT_KBD_PULSE), "i8042-pulse");
    CHECK_STR(acpi_action_kind_name(ACPI_ACT_TRIPLE_FAULT), "triple-fault");
    CHECK_STR(acpi_action_kind_name(ACPI_ACT_NONE), "none");
    CHECK_STR(acpi_action_kind_name((acpi_action_kind_t)99), "none");

    /* Only the write kinds carry an operand worth printing beside the method
     * name; "i8042-pulse: i8042-pulse" in a log line would be noise. */
    CHECK(acpi_action_has_operand(ACPI_ACT_IO_WRITE));
    CHECK(acpi_action_has_operand(ACPI_ACT_MEM_WRITE8));
    CHECK(!acpi_action_has_operand(ACPI_ACT_KBD_PULSE));
    CHECK(!acpi_action_has_operand(ACPI_ACT_TRIPLE_FAULT));
    CHECK(!acpi_action_has_operand(ACPI_ACT_NONE));

    char one[64];
    acpi_action_describe(&p.step[0], one, sizeof one);
    CHECK_STR(one, "i8042-pulse");
    CHECK_EQ(acpi_action_describe(NULL, one, sizeof one), 0);
    CHECK_EQ(acpi_action_describe(&p.step[0], one, 0), 0);
}

/* ====================================================================== */
/* 7. execution and the clean path                                        */
/* ====================================================================== */

static int  prepare_calls;
static char prepare_reason[128];
static int  prepare_op;

static void recording_prepare(acpi_power_op_t op, const char *reason) {
    prepare_calls++;
    prepare_op = (int)op;
    snprintf(prepare_reason, sizeof prepare_reason, "%s", reason ? reason : "");
    kputs("PREPARE ran\n");
}

/* Where in the captured console did `needle` first appear? -1 if never. */
static long at(const char *needle) {
    const char *p = strstr(kcap_text(), needle);
    return p ? (long)(p - kcap_text()) : -1;
}

static void test_execute_order(void) {
    build_standard_machine();
    CHECK_EQ(acpi_init(), ACPI_OK);

    acpi_set_prepare_hook(recording_prepare);
    prepare_calls = 0;
    kcap_reset();
    rec_n = 0;

    int rc = acpi_power_off("because the operator asked");

    /* Nothing on the host can actually stop the process, so every mechanism is
     * issued and the failure is reported instead of being pretended away. */
    CHECK_EQ(rc, ACPI_ENODEV);
    CHECK_EQ(prepare_calls, 1);
    CHECK_EQ(prepare_op, (int)ACPI_POWER_OFF);
    CHECK_STR(prepare_reason, "because the operator asked");

    /* Both steps of the QEMU-shaped plan were issued, in order. */
    CHECK_EQ(rec_n, 2);
    CHECK_STR(rec[0].kind, "io");
    CHECK_EQ(rec[0].address, 0x604);
    CHECK_EQ(rec[0].value, 0x2000);
    CHECK_EQ(rec[0].width, 16);
    CHECK_EQ(rec[1].address, 0xB004);

    /* THE ORDERING PROMISE: the clean path runs first, each attempt is named
     * BEFORE the write it is about to issue, and a survived attempt says so. */
    CHECK(at("PREPARE ran") >= 0);
    CHECK(at("power: attempt 1/2 acpi-pm1a") > at("PREPARE ran"));
    CHECK(at("RECORD io 0x604") > at("power: attempt 1/2 acpi-pm1a"));
    CHECK(at("acpi-pm1a did not take effect") > at("RECORD io 0x604"));
    CHECK(at("power: attempt 2/2 qemu-b004") > at("acpi-pm1a did not take effect"));
    CHECK(at("RECORD io 0xb004") > at("power: attempt 2/2 qemu-b004"));
    CHECK_CONTAINS(kcap_text(), "none took effect");

    /* Each step's settle window was honoured. */
    CHECK(delay_total_ms >= 200);

    /* The default prepare hook is the real clean path, and says what it does. */
    acpi_set_prepare_hook(NULL);
    kcap_reset();
    (void)acpi_power_off(NULL);
    CHECK_CONTAINS(kcap_text(), "power: shutdown requested");
    CHECK_CONTAINS(kcap_text(), "RAM-backed - nothing to sync");
    CHECK_CONTAINS(kcap_text(), "drivers_suspend_all");
    CHECK_CONTAINS(kcap_text(), "This machine is going down now.");
    CHECK(at("This machine is going down now.") < at("power: attempt 1/2"));

    kcap_reset();
    (void)acpi_reboot("scheduled restart");
    CHECK_CONTAINS(kcap_text(), "power: restart requested: scheduled restart");
    CHECK_CONTAINS(kcap_text(), "going down and coming back");
}

static void test_execute_reboot(void) {
    build_standard_machine();
    CHECK_EQ(acpi_init(), ACPI_OK);
    acpi_set_prepare_hook(NULL);
    kcap_reset();
    rec_n = 0;

    int rc = acpi_reboot("test");

    /* QEMU's i440fx FADT has no reset register, so this is the 8042 pulse then
     * the triple fault — and the triple fault is last. */
    CHECK_EQ(rec_n, 2);
    CHECK_STR(rec[0].kind, "kbd");
    CHECK_STR(rec[1].kind, "triple");
    CHECK(at("power: attempt 1/2 i8042-pulse") < at("RECORD kbd"));
    CHECK(at("power: attempt 2/2 triple-fault") < at("RECORD triple"));
    /* Only reachable because the fake triple fault returns; on a real machine
     * this line does not exist. */
    CHECK_EQ(rc, ACPI_ENODEV);

    /* With a reset register in the FADT, that comes first instead. */
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt3(AT_FADT, AT_DSDT, 0, 0x604, 0, ACPI_SPACE_IO, 0xCF9, 8, 0x0F, 1);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    uint64_t ents[1] = { AT_FADT };
    build_sdt(AT_RSDT, 0, ents, 1);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    CHECK_EQ(acpi_info()->reset_supported, 1);

    rec_n = 0;
    kcap_reset();
    (void)acpi_reboot(NULL);
    CHECK_EQ(rec_n, 3);
    CHECK_STR(rec[0].kind, "io");
    CHECK_EQ(rec[0].address, 0xCF9);
    CHECK_EQ(rec[0].value, 0x0F);
    CHECK_STR(rec[1].kind, "kbd");
    CHECK_STR(rec[2].kind, "triple");

    /* A memory-space reset register goes through mem_write8. */
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt3(AT_FADT, AT_DSDT, 0, 0x604, 0, ACPI_SPACE_MEM, 0xFED0002Cull,
                8, 0x02, 1);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    build_sdt(AT_RSDT, 0, ents, 1);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    rec_n = 0;
    (void)acpi_reboot(NULL);
    CHECK_STR(rec[0].kind, "mem8");
    CHECK_EQ(rec[0].address, 0xFED0002Cull);
}

static void test_execute_no_mechanism(void) {
    /* A machine with no ACPI and no ports: acpi_power_plan() still offers the
     * fallbacks, so to reach the "cannot power off" path the platform itself
     * must be unable to issue a write. */
    static const acpi_platform_t deaf = {
        .mem_read = fake_mem_read, .mem_write8 = NULL, .io_write = NULL,
        .io_read = NULL, .kbd_pulse = NULL, .triple_fault = NULL,
        .delay_ms = NULL,
    };
    machine_reset();
    acpi_set_platform(&deaf);
    acpi_forget();
    acpi_set_prepare_hook(NULL);
    kcap_reset();

    int rc = acpi_power_off("nothing works here");
    CHECK_EQ(rc, ACPI_ENODEV);
    CHECK_CONTAINS(kcap_text(), "none took effect");
    /* And it said so rather than stopping silently. */
    CHECK_CONTAINS(kcap_text(), "cannot turn itself off");

    CHECK_EQ(acpi_plan_execute(NULL), 0);
    acpi_set_platform(&fake_platform);
}

static void test_enable_hw(void) {
    /* A board already in ACPI mode needs no SMI at all. */
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    uint64_t ents[1] = { AT_FADT };
    build_sdt(AT_RSDT, 0, ents, 1);
    acpi_set_platform(&fake_platform);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);

    set_portval(0x604, ACPI_PM1_CNT_SCI_EN);
    rec_n = 0;
    CHECK_EQ(acpi_enable_hw(), ACPI_OK);
    CHECK_EQ(rec_n, 0);                       /* no SMI was issued */

    /* A board in legacy mode that acknowledges — QEMU's behaviour: one write to
     * the SMI command port with the FADT's enable value, and SCI_EN appears. */
    set_portval(0x604, 0);
    smi_flips_sci_en = 1;
    rec_n = 0;
    kcap_reset();
    CHECK_EQ(acpi_enable_hw(), ACPI_OK);
    CHECK_EQ(rec_n, 1);
    CHECK_STR(rec[0].kind, "io");
    CHECK_EQ(rec[0].address, 0xB2);           /* SMI_CMD from the FADT     */
    CHECK_EQ(rec[0].value, 0xF1);             /* ACPI_ENABLE from the FADT */
    CHECK_CONTAINS(kcap_text(), "board is in legacy mode");
    CHECK_CONTAINS(kcap_text(), "power: ACPI mode is on");

    /* A board that never acknowledges: reported, and NOT fatal — plenty of
     * boards honour SLP_EN in legacy mode, so soft-off is still attempted. */
    smi_flips_sci_en = 0;
    set_portval(0x604, 0);
    rec_n = 0;
    kcap_reset();
    CHECK_EQ(acpi_enable_hw(), ACPI_ENODEV);
    CHECK_EQ(rec_n, 1);
    CHECK_STR(rec[0].kind, "io");
    CHECK_EQ(rec[0].address, 0xB2);           /* SMI_CMD from the FADT   */
    CHECK_EQ(rec[0].value, 0xF1);             /* ACPI_ENABLE from the FADT */
    CHECK_CONTAINS(kcap_text(), "never acknowledged ACPI mode");
    /* ...and it is not fatal: soft-off is still attempted afterwards. */
    CHECK_CONTAINS(kcap_text(), "attempting soft-off anyway");

    /* No SMI command port means there is no legacy mode to leave. */
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt1(AT_FADT, AT_DSDT, 0x604, 0);
    put32(fake_mem + AT_FADT + 48, 0);        /* SMI_CMD = 0 */
    fix_sum(fake_mem + AT_FADT, 116, 9);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    build_sdt(AT_RSDT, 0, ents, 1);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    set_portval(0x604, 0);
    rec_n = 0;
    CHECK_EQ(acpi_enable_hw(), ACPI_OK);
    CHECK_EQ(rec_n, 0);

    /* And with no tables at all it is a no-op, not an error. */
    machine_reset();
    acpi_forget();
    CHECK_EQ(acpi_enable_hw(), ACPI_OK);
}

/* ====================================================================== */
/* 8. the tools                                                           */
/* ====================================================================== */

#define GUARD      64
#define GUARD_BYTE 0xA5

static char          g_store[GUARD + TOOL_RESULT_MAX + GUARD];
static char         *g_out = g_store + GUARD;
static tool_result_t g_res;

static int guards_intact(size_t cap) {
    for (size_t i = 0; i < GUARD; i++)
        if ((unsigned char)g_store[i] != GUARD_BYTE) return 0;
    for (size_t i = GUARD + cap; i < sizeof g_store; i++)
        if ((unsigned char)g_store[i] != GUARD_BYTE) return 0;
    return 1;
}

/* Dispatch `name` with a raw input span that is deliberately NOT
 * NUL-terminated, so a tool that runs off the end reads poison. */
static const char *call_n(const char *name, const char *input, size_t input_len) {
    static char span[16384];

    memset(g_store, GUARD_BYTE, sizeof g_store);
    tool_result_init(&g_res, g_out, TOOL_RESULT_MAX);

    memset(span, 0x5A, sizeof span);
    if (input && input_len) {
        if (input_len > sizeof span) input_len = sizeof span;
        memcpy(span, input, input_len);
    }

    tool_call_t call = {
        .id        = "toolu_test",
        .name      = name,
        .input     = input ? span : NULL,
        .input_len = input ? input_len : 0,
    };
    tool_dispatch(&call, &g_res);
    CHECK(guards_intact(TOOL_RESULT_MAX));
    return g_out;
}

static const char *call(const char *name, const char *input) {
    return call_n(name, input, input ? strlen(input) : 0);
}

static void test_tool_refusals(void) {
    build_standard_machine();
    CHECK_EQ(acpi_init(), ACPI_OK);
    acpi_set_prepare_hook(recording_prepare);

    /* Every one of these must leave the machine alone: no port write, no
     * prepare hook, and a message that says how to do it properly. */
    static const char *const refusals[] = {
        "{}",                                  /* no confirm            */
        "{\"confirm\":false}",                 /* explicitly no         */
        "{\"confirm\":\"true\"}",              /* a string, not a bool  */
        "{\"confirm\":1}",                     /* an int, not a bool    */
        "{\"confirm\":null}",
        "{\"confirm\":[true]}",
        "{\"CONFIRM\":true}",                  /* keys are case-sensitive */
        "{\"reason\":\"do it\"}",
        "{\"confirm\":true",                   /* truncated JSON        */
        "{\"confirm\":tru",
        "[]",                                  /* not an object         */
        "42",
        "\"confirm\"",
        "",
        "   ",
        "null",
        "{\"confirm\":true}}}}",               /* trailing garbage      */
    };

    for (size_t i = 0; i < sizeof refusals / sizeof refusals[0]; i++) {
        for (int t = 0; t < 2; t++) {
            const char *name = t ? "power_reboot" : "power_off";
            rec_n = 0;
            prepare_calls = 0;
            kcap_reset();
            const char *out = call(name, refusals[i]);
            CHECK_EQ(g_res.is_error, 1);
            CHECK_EQ(rec_n, 0);
            CHECK_EQ(prepare_calls, 0);
            CHECK_CONTAINS(out, "REFUSED");
            CHECK_CONTAINS(out, "still running");
            CHECK_CONTAINS(kcap_text(), "refused=");
        }
    }

    /* A NULL input span is not a crash either. */
    rec_n = 0;
    const char *out = call("power_off", NULL);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_EQ(rec_n, 0);
    CHECK_CONTAINS(out, "REFUSED");

    /* An input span with an embedded NUL: the span length is what counts, not
     * the first terminator. */
    rec_n = 0;
    static const char nul_in[] = "{\"confirm\":true\0,\"x\":1}";
    call_n("power_off", nul_in, sizeof nul_in - 1);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_EQ(rec_n, 0);

    acpi_set_prepare_hook(NULL);
}

static void test_tool_power_off_commits(void) {
    build_standard_machine();
    CHECK_EQ(acpi_init(), ACPI_OK);
    acpi_set_prepare_hook(NULL);
    rec_n = 0;
    kcap_reset();

    uint64_t before = trace_emissions();
    const char *out = call("power_off",
        "{\"confirm\":true,\"reason\":\"operator said shut down\"}");

    /* On the host nothing can stop the process, so the tool reports the
     * failure — and the result text was written so that it reads correctly in
     * exactly this case. */
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(out, "POWERING OFF NOW");
    CHECK_CONTAINS(out, "IF YOU ARE READING THIS TEXT, THE SHUTDOWN DID NOT HAPPEN");
    CHECK_CONTAINS(out, "irreversible");
    CHECK_CONTAINS(out, "acpi-pm1a(io 0x604<-0x2000/16)");
    CHECK_CONTAINS(out, "operator said shut down");

    /* THE POINT OF THE WHOLE EXERCISE: ground truth reached the console before
     * the write that would have ended the machine. */
    const char *log = kcap_text();
    CHECK_CONTAINS(log, "[power_off stage=commit method=acpi-pm1a io 0x604<-0x2000/16");
    CHECK(at("[power_off stage=commit") >= 0);
    CHECK(at("RECORD io 0x604") > at("[power_off stage=commit"));
    CHECK(at("PREPARE") < 0);                    /* the real hook, not the stub */
    CHECK(at("power: shutdown requested") > at("[power_off stage=commit"));

    /* Two lines, in the documented order: the commitment, then the failure. */
    CHECK_CONTAINS(log, "[power_off stage=failed steps=2 -> -19]");
    CHECK(at("[power_off stage=failed") > at("RECORD io 0xb004"));
    CHECK_EQ(trace_emissions() - before, 2);

    /* The reason is echoed by the kernel too, so the log says why. */
    CHECK_CONTAINS(log, "power: shutdown requested: operator said shut down");

    /* No reason at all is fine. */
    rec_n = 0;
    out = call("power_off", "{\"confirm\":true}");
    CHECK_CONTAINS(out, "(none given)");
    CHECK_EQ(rec_n, 2);
}

static void test_tool_reboot_commits(void) {
    build_standard_machine();
    CHECK_EQ(acpi_init(), ACPI_OK);
    acpi_set_prepare_hook(NULL);
    rec_n = 0;
    kcap_reset();

    const char *out = call("power_reboot", "{\"confirm\":true,\"reason\":\"kernel update\"}");
    CHECK_CONTAINS(out, "RESTARTING NOW");
    CHECK_CONTAINS(out, "irreversible");
    CHECK_CONTAINS(out, "i8042-pulse triple-fault");
    CHECK_CONTAINS(out, "does not return");

    const char *log = kcap_text();
    /* No operand for the 8042 pulse, so the method names itself once and only
     * once — not "method=i8042-pulse i8042-pulse". */
    CHECK_CONTAINS(log, "[power_reboot stage=commit method=i8042-pulse steps=2 ");
    CHECK_CONTAINS(log, "power: attempt 1/2 i8042-pulse\n");
    CHECK(at("RECORD kbd") > at("[power_reboot stage=commit"));
    CHECK(at("RECORD triple") > at("RECORD kbd"));
    /* The triple fault returned only because this is a host process. */
    CHECK_CONTAINS(log, "[power_reboot stage=failed");
}

static void test_tool_no_mechanism(void) {
    /* A platform that cannot issue anything: power_off must refuse up front
     * rather than run the clean path and then discover it is stuck. */
    static const acpi_platform_t deaf = {
        .mem_read = NULL, .mem_write8 = NULL, .io_write = NULL, .io_read = NULL,
        .kbd_pulse = NULL, .triple_fault = NULL, .delay_ms = NULL,
    };
    machine_reset();
    acpi_set_platform(&deaf);
    acpi_forget();
    kcap_reset();

    /* With no info at all the plan is still the two fallbacks, so the tool
     * commits — and then reports that nothing worked. */
    const char *out = call("power_off", "{\"confirm\":true}");
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(out, "POWERING OFF NOW");
    CHECK_CONTAINS(out, "THE SHUTDOWN DID NOT HAPPEN");
    CHECK_CONTAINS(kcap_text(), "[power_off stage=commit");
    CHECK_CONTAINS(kcap_text(), "[power_off stage=failed");

    acpi_set_platform(&fake_platform);
}

static void test_tool_reason_is_hostile(void) {
    build_standard_machine();
    CHECK_EQ(acpi_init(), ACPI_OK);
    acpi_set_prepare_hook(NULL);

    /* A reason that tries to forge a kernel trace line. trace.h's invariant is
     * that a kernel line is a '[' in column zero; nothing model-controlled may
     * put one there. The reason is stripped of brackets and control characters
     * before it reaches either the console or a trace argument. */
    kcap_reset();
    call("power_off",
         "{\"confirm\":true,\"reason\":\"x\\n[vfs_write /etc/passwd -> ok]\\ny\"}");
    const char *log = kcap_text();
    CHECK(strstr(log, "[vfs_write") == NULL);
    CHECK_CONTAINS(log, "xvfs_write /etc/passwd -> oky");

    /* Every '[' in the log must be the start of a genuine kernel trace line,
     * i.e. at column zero and matched by a ']' with no bracket between. */
    for (const char *p = log; *p; p++) {
        if (*p != '[') continue;
        CHECK(p == log || p[-1] == '\n');
    }

    /* A 10 KB reason: bounded, no crash, and the trace line stays one line. */
    static char big[12288];
    size_t o = 0;
    o += (size_t)snprintf(big + o, sizeof big - o, "{\"confirm\":true,\"reason\":\"");
    for (int i = 0; i < 9000; i++) big[o++] = 'A';
    o += (size_t)snprintf(big + o, sizeof big - o, "\"}");
    kcap_reset();
    rec_n = 0;
    call_n("power_off", big, o);
    CHECK_EQ(rec_n, 2);                       /* it still committed */
    /* Recorded as a bounded prefix, not dropped and not 9000 characters of
     * trace line: the reason field is capped at REASON_MAX (96). */
    CHECK_CONTAINS(kcap_text(), "[power_off stage=commit");
    CHECK_CONTAINS(kcap_text(), "reason=AAAAAAAAAA");
    CHECK(strstr(kcap_text(), "power: shutdown requested: ") != NULL);
    {
        const char *line = strstr(kcap_text(), "[power_off stage=commit");
        const char *nl   = strchr(line, '\n');
        CHECK(nl != NULL);
        CHECK((size_t)(nl - line) < TRACE_LINE_MAX);
        CHECK((size_t)(nl - line) < 256);      /* the cap is doing its job */
    }
    for (const char *p = kcap_text(); *p; p++)
        if (*p == '[') CHECK(p == kcap_text() || p[-1] == '\n');

    /* A reason of exactly the boundary length, and one with only droppable
     * characters, must both be handled without a fault. */
    kcap_reset();
    call("power_off", "{\"confirm\":true,\"reason\":\"\\n\\n\\t[[[]]]\"}");
    CHECK_CONTAINS(kcap_text(), "[power_off stage=commit");
    CHECK_CONTAINS(kcap_text(), "reason=-");    /* nothing printable survived */
}

static void test_sanitize_reason(void) {
    char b[16];

    CHECK_EQ(acpi_sanitize_reason(b, sizeof b, "hello"), 5);
    CHECK_STR(b, "hello");

    CHECK_EQ(acpi_sanitize_reason(b, sizeof b, NULL), 0);
    CHECK_STR(b, "");

    CHECK_EQ(acpi_sanitize_reason(b, sizeof b, "a\nb\t[c]\x7f"), 3);
    CHECK_STR(b, "abc");

    /* Truncation never overflows and always terminates. */
    CHECK_EQ(acpi_sanitize_reason(b, sizeof b, "0123456789abcdefghij"), 15);
    CHECK_STR(b, "0123456789abcde");

    CHECK_EQ(acpi_sanitize_reason(b, 1, "hello"), 0);
    CHECK_STR(b, "");
    CHECK_EQ(acpi_sanitize_reason(NULL, 16, "hello"), 0);
    CHECK_EQ(acpi_sanitize_reason(b, 0, "hello"), 0);
}

static void test_tool_power_info(void) {
    build_standard_machine();
    CHECK_EQ(acpi_init(), ACPI_OK);
    rec_n = 0;
    kcap_reset();

    const char *out = call("power_info", "{}");
    CHECK_EQ(g_res.is_error, 0);
    CHECK_EQ(rec_n, 0);                       /* read-only: nothing was poked */
    CHECK_CONTAINS(out, "ACPI: available (RSDP revision 0, OEM \"FABLE \")");
    CHECK_CONTAINS(out, "RSDT");
    CHECK_CONTAINS(out, "PM1a_CNT:     0x604");
    CHECK_CONTAINS(out, "found in the DSDT: SLP_TYPa=0 SLP_TYPb=0");
    CHECK_CONTAINS(out, "not provided by the firmware");
    CHECK_CONTAINS(out, "power_off would try 2 mechanism(s)");
    CHECK_CONTAINS(out, "acpi-pm1a(io 0x604<-0x2000/16)");
    CHECK_CONTAINS(out, "power_reboot would try 2");
    CHECK_CONTAINS(out, "would use real ACPI soft-off");
    CHECK_CONTAINS(out, "confirm");
    CHECK_CONTAINS(kcap_text(), "[power_info acpi=yes s5=yes reset=no -> 2]");

    /* It ignores its arguments entirely — the most robust possible parse. */
    CHECK_CONTAINS(call("power_info", "not json at all"), "ACPI:");
    CHECK_CONTAINS(call("power_info", NULL), "ACPI:");
    CHECK_CONTAINS(call("power_info", "{\"confirm\":true}"), "ACPI:");
    CHECK_CONTAINS(call("power_info", "{{{{"), "ACPI:");
    CHECK_EQ(rec_n, 0);

    /* And it is honest when there is nothing to report. */
    machine_reset();
    acpi_forget();
    out = call("power_info", "{}");
    CHECK_CONTAINS(out, "ACPI: NOT AVAILABLE");
    CHECK_CONTAINS(out, "no ACPI path");
    CHECK_EQ(g_res.is_error, 0);

    /* A machine with a reset register describes it. */
    machine_reset();
    build_rsdp1(AT_RSDP, AT_RSDT);
    build_fadt3(AT_FADT, AT_DSDT, 0, 0x604, 0, ACPI_SPACE_IO, 0xCF9, 8, 0x0F, 1);
    build_aml_table(AT_DSDT, "DSDT", S5_ZEROS, sizeof S5_ZEROS, 0);
    uint64_t ents[1] = { AT_FADT };
    build_sdt(AT_RSDT, 0, ents, 1);
    acpi_forget();
    CHECK_EQ(acpi_init(), ACPI_OK);
    out = call("power_info", "{}");
    CHECK_CONTAINS(out, "reset reg:    I/O port 0xcf9, 8 bits, write 0xf");
    CHECK_CONTAINS(out, "acpi-reset(io 0xcf9<-0xf/8)");
}

/* ====================================================================== */
/* 9. the advertised contract                                             */
/* ====================================================================== */

static void test_schema(void) {
    static char schema[8192];
    size_t need = tools_build_schema(schema, sizeof schema);
    CHECK(need > 0);
    CHECK(need < sizeof schema);

    /* Every tool is advertised, and the whole array is valid JSON — a
     * malformed schema would corrupt the request body for every tool. */
    json_value_t v;
    CHECK_EQ(json_parse(schema, strlen(schema), &v), JSON_OK);
    CHECK_EQ(v.type, JSON_ARRAY);
    CHECK_EQ(json_count(&v), 3);

    CHECK_CONTAINS(schema, "\"power_off\"");
    CHECK_CONTAINS(schema, "\"power_reboot\"");
    CHECK_CONTAINS(schema, "\"power_info\"");
    /* The description has to warn the model before it calls, not after. */
    CHECK_CONTAINS(schema, "IRREVERSIBLE");
    CHECK_CONTAINS(schema, "confirm");

    /* Each entry must carry a name, a description and an object schema. */
    for (size_t i = 0; i < json_count(&v); i++) {
        json_value_t e, f;
        CHECK_EQ(json_at(&v, i, &e), JSON_OK);
        CHECK_EQ(e.type, JSON_OBJECT);
        CHECK_EQ(json_get(&e, "name", &f), JSON_OK);
        CHECK_EQ(json_get(&e, "description", &f), JSON_OK);
        CHECK_EQ(json_get(&e, "input_schema", &f), JSON_OK);
        CHECK_EQ(f.type, JSON_OBJECT);
    }

    /* Both mutating tools are flagged; the query tool is not. */
    CHECK_TOOL_FLAGS("power_off", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("power_reboot", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("power_info", TOOL_MUTATES, 0);
}

/* ====================================================================== */
/* 10. fuzz                                                               */
/* ====================================================================== */

static uint32_t rng_state = 0x5eed1234;
static uint32_t rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void test_fuzz_parsers(void) {
    /* Random bytes through every parser. Nothing may fault, and anything that
     * reports success must be self-consistent — a parser that accepts garbage
     * and then reports a 4 GiB DSDT would be worse than one that crashes. */
    static uint8_t buf[512];
    acpi_info_t    in;

    for (int iter = 0; iter < 20000; iter++) {
        size_t len = 1 + rng() % sizeof buf;
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rng();

        /* Occasionally start from something that looks real, so the deeper
         * paths are reached rather than bouncing off the signature check. */
        if ((iter & 3) == 0 && len >= 36) memcpy(buf, "RSD PTR ", 8);
        if ((iter & 3) == 1 && len >= 44) memcpy(buf, "FACP", 4);

        memset(&in, 0, sizeof in);
        int rc = acpi_rsdp_check(buf, len, &in);
        CHECK(rc == ACPI_OK || rc == ACPI_EINVAL);
        if (rc == ACPI_OK) CHECK_EQ(in.oem_id[6], '\0');

        memset(&in, 0, sizeof in);
        rc = acpi_parse_fadt(buf, len, &in);
        CHECK(rc == ACPI_OK || rc == ACPI_EINVAL);
        if (rc == ACPI_OK) {
            CHECK(in.fadt_len <= len);
            CHECK(in.fadt_len >= 44);
            if (in.reset_supported) {
                CHECK(in.reset_reg.address != 0);
                CHECK(in.reset_reg.bit_width == 8 ||
                      in.reset_reg.bit_width == 16 ||
                      in.reset_reg.bit_width == 32);
            }
        }

        uint8_t a = 0xEE, b = 0xEE;
        int off = acpi_find_s5(buf, len, "_S5_", &a, &b);
        CHECK(off == ACPI_ENOENT || (off >= 0 && (size_t)off < len));
        if (off >= 0) { CHECK(a <= 7); CHECK(b <= 7); }

        /* A plan built from fuzzed info must stay inside its own bounds. */
        acpi_plan_t p;
        int n = acpi_power_plan(&in, (iter & 1) ? ACPI_POWER_REBOOT
                                               : ACPI_POWER_OFF, &p);
        CHECK(n >= 0 && n <= ACPI_PLAN_MAX);
        CHECK_EQ(n, p.n);
        char d[256];
        size_t dl = acpi_plan_describe(&p, d, sizeof d);
        CHECK_EQ(dl, strlen(d));
        CHECK(strchr(d, '[') == NULL);
    }
    CHECK_EQ(kpanic_hit, 0);
}

static void test_fuzz_tool_input(void) {
    /* Random JSON-ish input into the two irreversible tools. The only outcomes
     * allowed are "refused, machine untouched" and "confirmed, exactly the
     * planned writes". Nothing may fault or smash a canary. */
    build_standard_machine();
    CHECK_EQ(acpi_init(), ACPI_OK);
    acpi_set_prepare_hook(recording_prepare);

    static const char alphabet[] = "{}[]\":,truefalsnl0123456789confirmeason\\ \n\t";
    char input[128];

    for (int iter = 0; iter < 20000; iter++) {
        size_t len = rng() % sizeof input;
        for (size_t i = 0; i < len; i++)
            input[i] = alphabet[rng() % (sizeof alphabet - 1)];

        rec_n = 0;
        prepare_calls = 0;
        const char *name = (iter & 1) ? "power_off" : "power_reboot";
        call_n(name, input, len);

        if (prepare_calls == 0) {
            /* Refused: the machine must be untouched. */
            CHECK_EQ(rec_n, 0);
            CHECK_EQ(g_res.is_error, 1);
        } else {
            /* Confirmed: exactly the plan, and nothing else. */
            CHECK_EQ(prepare_calls, 1);
            CHECK(rec_n == 2 || rec_n == 3);
        }
    }
    CHECK_EQ(kpanic_hit, 0);
    acpi_set_prepare_hook(NULL);
}

/* ====================================================================== */

int main(void) {
    console_init();
    install_tool_table();
    acpi_set_platform(&fake_platform);

    RUN(test_checksum);
    RUN(test_rsdp_valid);
    RUN(test_rsdp_rejects);
    RUN(test_fadt_rev1);
    RUN(test_fadt_rev3_prefers_64bit);
    RUN(test_fadt_reset_register_conditions);
    RUN(test_fadt_rejects);
    RUN(test_s5_shapes);
    RUN(test_s5_rejects);
    RUN(test_discovery_bios_window);
    RUN(test_discovery_ebda);
    RUN(test_discovery_alignment_and_absence);
    RUN(test_discovery_xsdt_preferred);
    RUN(test_discovery_s5_in_ssdt);
    RUN(test_discovery_chunked_scan);
    RUN(test_discovery_broken_pointers);
    RUN(test_plan_power_off_ordering);
    RUN(test_plan_power_off_degraded);
    RUN(test_plan_reboot_ordering);
    RUN(test_plan_describe);
    RUN(test_execute_order);
    RUN(test_execute_reboot);
    RUN(test_execute_no_mechanism);
    RUN(test_enable_hw);
    RUN(test_sanitize_reason);
    RUN(test_tool_refusals);
    RUN(test_tool_power_off_commits);
    RUN(test_tool_reboot_commits);
    RUN(test_tool_no_mechanism);
    RUN(test_tool_reason_is_hostile);
    RUN(test_tool_power_info);
    RUN(test_schema);
    RUN(test_fuzz_parsers);
    RUN(test_fuzz_tool_input);

    return th_report("acpi");
}
