/* acpi.c — the firmware tables: find them, validate them, believe none of them.
 *
 * See include/acpi.h for the contract, the scope (no AML interpreter) and the
 * reasoning behind plan-then-execute. This file is the half with no side
 * effects on the machine: discovery, parsing, and turning what was discovered
 * into an ordered list of things to try. drivers/acpi/power.c is the half that
 * actually ends the machine.
 *
 * LAYOUT OF THIS FILE
 *   1. the platform seam and its default backend (the only hardware here)
 *   2. little-endian accessors and checksums
 *   3. the RSDP: scan, validate
 *   4. the FADT: parse
 *   5. AML: find \_S5_ and read its integers
 *   6. discovery: acpi_init() ties 3-5 together
 *   7. the plan: pure policy, host-asserted
 *
 * Sections 2, 4, 5 and 7 are pure functions over caller-supplied bytes and
 * compile unchanged on the host. Sections 3 and 6 touch memory only through the
 * platform backend, so the tests drive them against synthetic tables.
 *
 * ============================================================================
 * EVERY BYTE IN HERE IS UNTRUSTED INPUT
 * ============================================================================
 *   Firmware tables are not model output, but they are not kernel source
 *   either: they are attacker-adjacent data written by a BIOS vendor, and this
 *   kernel reads them with no memory protection and no way to recover from a
 *   fault beyond an exception report. A table that declares its own length is a
 *   table that can declare 4 GiB. So:
 *
 *   - Every structure is validated by checksum over its OWN declared length,
 *     and that length is range-checked before it is used for anything.
 *   - Every field read is bounds-checked against the number of bytes actually
 *     read, never against the length the table claims.
 *   - Physical addresses are read through the platform backend, whose kernel
 *     implementation refuses anything outside the low 4 GiB the boot page
 *     tables actually map. A firmware pointer to 0x7fff_ffff_0000 therefore
 *     produces a legible failure instead of a page fault.
 *   - Nothing here allocates. The scan buffer is static and the table list is
 *     fixed-size, because the power path must work when the heap is the thing
 *     that is broken.
 */

#include "acpi.h"

#include <stdio.h>          /* snprintf: port/stdio.h freestanding, libc on host */

#ifdef FABLEOS_HOSTTEST
/* kshim.c supplies these; kernel.h is not includable here (its declarations
 * fight the host libc, see the note in the Makefile about -Iport). */
void kputs(const char *s);
void kprintf(const char *fmt, ...);
#else
#  include "kernel.h"
#  include "io.h"
#endif

/* ====================================================================== */
/* 1. the platform seam                                                   */
/* ====================================================================== */

#ifndef FABLEOS_HOSTTEST

/* The identity map boot/boot.asm builds covers the low 4 GiB with 2 MiB huge
 * pages. Anything at or above that is not mapped at all, and a firmware pointer
 * that lands there must become an error, not a fault. */
#define PHYS_LIMIT 0x0000000100000000ULL

/* NOTE ON MMIO. The low 4 GiB includes device registers (the VGA framebuffer,
 * the e1000 BARs, the LAPIC page), and many of those are clear-on-read — so a
 * read here is not automatically harmless just because it cannot fault. What
 * keeps this safe is where the addresses come from: the two fixed BIOS windows
 * the ACPI spec names, and then only pointers held inside structures that have
 * already passed their own signature and checksum checks. No address the model
 * supplies ever reaches this function; the model's raw-memory tool is
 * tools/mem_tools.c, which is bounded to the kernel image for exactly this
 * reason. */
static int hw_mem_read(uint64_t phys, void *dst, size_t len) {
    if (!dst || len == 0)               return ACPI_EINVAL;
    if (len > PHYS_LIMIT)               return ACPI_EINVAL;
    if (phys > PHYS_LIMIT - len)        return ACPI_EINVAL;   /* no wrap: ordered */
    const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)phys;
    uint8_t                *d = (uint8_t *)dst;
    for (size_t i = 0; i < len; i++) d[i] = s[i];
    return ACPI_OK;
}

static int hw_mem_write8(uint64_t phys, uint8_t value) {
    if (phys >= PHYS_LIMIT) return ACPI_EINVAL;
    *(volatile uint8_t *)(uintptr_t)phys = value;
    return ACPI_OK;
}

static void hw_io_write(uint32_t port, uint32_t value, uint8_t bit_width) {
    if (port > 0xFFFF) return;
    switch (bit_width) {
        case 8:  outb((uint16_t)port, (uint8_t)value);  break;
        case 32: outl((uint16_t)port, value);           break;
        default: outw((uint16_t)port, (uint16_t)value); break;   /* 16 */
    }
}

static uint32_t hw_io_read(uint32_t port, uint8_t bit_width) {
    if (port > 0xFFFF) return 0;
    switch (bit_width) {
        case 8:  return inb((uint16_t)port);
        case 32: return inl((uint16_t)port);
        default: return inw((uint16_t)port);
    }
}

/* The 8042's command port 0xFE pulses the CPU's RESET line. Wait for the input
 * buffer to drain first (status bit 1), bounded, or a controller that is busy
 * for good swallows the command and we fall through to the triple fault. */
static void hw_kbd_pulse(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) break;
        __asm__ volatile("pause");
    }
    outb(0x64, 0xFE);
}

/* The last resort, and the only one that cannot fail: point the CPU at an IDT
 * with no room for any gate, then raise an exception. The CPU cannot deliver
 * it, cannot deliver the resulting #DF either, and a triple fault is defined to
 * reset the machine. Deliberately NOT a hlt loop — this is called only after
 * every orderly mechanism has already been tried, and a machine that was asked
 * to restart must restart. */
static void hw_triple_fault(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt =
        { 0, 0 };
    __asm__ volatile("cli");
    __asm__ volatile("lidt %0" : : "m"(null_idt));
    __asm__ volatile("int3");
    for (;;) __asm__ volatile("hlt");     /* unreachable */
}

static void hw_delay_ms(uint32_t ms) { mdelay(ms); }

static const acpi_platform_t hw_platform = {
    .mem_read     = hw_mem_read,
    .mem_write8   = hw_mem_write8,
    .io_write     = hw_io_write,
    .io_read      = hw_io_read,
    .kbd_pulse    = hw_kbd_pulse,
    .triple_fault = hw_triple_fault,
    .delay_ms     = hw_delay_ms,
};
#define DEFAULT_PLATFORM (&hw_platform)

#else  /* FABLEOS_HOSTTEST */

/* A host process has no physical memory and no ports. Every capability is
 * absent, so discovery reports "no ACPI" instead of interpreting whatever
 * happens to be at address 0x40E in the test binary. Tests install their own. */
static const acpi_platform_t null_platform = { 0, 0, 0, 0, 0, 0, 0 };
#define DEFAULT_PLATFORM (&null_platform)

#endif

static const acpi_platform_t *g_platform = DEFAULT_PLATFORM;

void acpi_set_platform(const acpi_platform_t *p) {
    g_platform = p ? p : DEFAULT_PLATFORM;
}

const acpi_platform_t *acpi_platform(void) { return g_platform; }

/* Read `len` bytes of physical memory, or fail. */
static int mem(uint64_t phys, void *dst, size_t len) {
    const acpi_platform_t *p = g_platform;
    if (!p || !p->mem_read) return ACPI_ENODEV;
    return p->mem_read(phys, dst, len);
}

/* ====================================================================== */
/* 2. little-endian accessors, checksums, tiny string helpers             */
/* ====================================================================== */

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* No <string.h> and no kernel.h memcmp: one of the two is unavailable in each
 * build, and four bytes do not justify the include gymnastics. */
static int eqn(const uint8_t *p, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}

static void copy_sig(char *dst, const uint8_t *src) {      /* 4 chars + NUL */
    for (int i = 0; i < 4; i++) {
        uint8_t c = src[i];
        dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    dst[4] = '\0';
}

uint8_t acpi_checksum(const void *bytes, size_t len) {
    const uint8_t *p = (const uint8_t *)bytes;
    uint8_t sum = 0;
    if (!p) return 0xFF;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum;
}

int acpi_checksum_ok(const void *bytes, size_t len) {
    if (!bytes || len == 0) return 0;
    return acpi_checksum(bytes, len) == 0;
}

/* Scratch for streaming reads. Static, never allocated: see the header comment.
 * Sized so an \_S5_ match that straddles two chunks is still contained in one
 * window (ACPI_SCAN_CHUNK step, ACPI_S5_PATTERN_MAX of overlap). */
static uint8_t scanbuf[ACPI_SCAN_CHUNK + ACPI_S5_PATTERN_MAX];

/* Checksum a physical range without buffering all of it. */
static int checksum_phys_ok(uint64_t phys, uint32_t len) {
    uint8_t  sum = 0;
    uint32_t off = 0;
    if (len == 0) return 0;
    while (off < len) {
        uint32_t want = len - off;
        if (want > ACPI_SCAN_CHUNK) want = ACPI_SCAN_CHUNK;
        if (mem(phys + off, scanbuf, want) != ACPI_OK) return 0;
        for (uint32_t i = 0; i < want; i++) sum = (uint8_t)(sum + scanbuf[i]);
        off += want;
    }
    return sum == 0;
}

/* ====================================================================== */
/* 3. the RSDP                                                            */
/* ====================================================================== */

/* An ACPI 2.0+ RSDP is 36 bytes. 64 is read per candidate so a firmware that
 * declares a slightly longer one still validates; anything longer than this is
 * rejected rather than trusted. */
#define RSDP_READ 64

int acpi_rsdp_check(const void *buf, size_t len, acpi_info_t *out) {
    const uint8_t *b = (const uint8_t *)buf;
    if (!b || !out || len < 20)            return ACPI_EINVAL;
    if (!eqn(b, "RSD PTR ", 8))            return ACPI_EINVAL;
    if (!acpi_checksum_ok(b, 20))          return ACPI_EINVAL;

    out->rsdp_revision = b[15];
    for (int i = 0; i < 6; i++) {
        uint8_t c = b[9 + i];
        out->oem_id[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    out->oem_id[6] = '\0';
    out->rsdt_phys  = le32(b + 16);
    out->xsdt_phys  = 0;

    if (out->rsdp_revision >= 2) {
        if (len < 36) return ACPI_EINVAL;
        uint32_t tlen = le32(b + 20);
        /* A 2.0 RSDP that lies about its own size is not a 2.0 RSDP. */
        if (tlen < 36 || tlen > len)     return ACPI_EINVAL;
        if (!acpi_checksum_ok(b, tlen))  return ACPI_EINVAL;
        out->xsdt_phys = le64(b + 24);
    }
    return ACPI_OK;
}

/* Scan a 16-byte-aligned physical window for a valid RSDP. Returns its address
 * or 0. The RSDP is required by the spec to be 16-byte aligned, which is what
 * makes a linear scan of 128 KiB cheap. */
static uint64_t rsdp_scan(uint64_t from, uint64_t to, acpi_info_t *out) {
    uint8_t buf[RSDP_READ];
    for (uint64_t a = from; a + 20 <= to; a += 16) {
        size_t want = RSDP_READ;
        if (a + want > to) want = (size_t)(to - a);
        if (mem(a, buf, want) != ACPI_OK) continue;
        if (!eqn(buf, "RSD PTR ", 8))     continue;
        if (acpi_rsdp_check(buf, want, out) != ACPI_OK) continue;
        return a;
    }
    return 0;
}

/* ====================================================================== */
/* 4. the FADT                                                            */
/* ====================================================================== */

/* Field offsets, ACPI 6.x table 5-33. Named rather than commented so a mistake
 * is visible at the use site. */
#define FADT_DSDT32        40
#define FADT_SCI_INT       46
#define FADT_SMI_CMD       48
#define FADT_ACPI_ENABLE   52
#define FADT_ACPI_DISABLE  53
#define FADT_PM1A_CNT32    64
#define FADT_PM1B_CNT32    68
#define FADT_PM1_CNT_LEN   89
#define FADT_FLAGS        112
#define FADT_RESET_REG    116        /* GAS, 12 bytes */
#define FADT_RESET_VALUE  128
#define FADT_X_DSDT       140
#define FADT_X_PM1A_CNT   172        /* GAS */
#define FADT_X_PM1B_CNT   184        /* GAS */
#define FADT_MIN_LEN       44        /* header + FIRMWARE_CTRL + DSDT        */
#define ACPI_HDR_LEN       36

static void gas_parse(const uint8_t *p, acpi_gas_t *g) {
    g->space_id    = p[0];
    g->bit_width   = p[1];
    g->bit_offset  = p[2];
    g->access_size = p[3];
    g->address     = le64(p + 4);
}

/* A GAS is usable as a register only if it names a space we can reach, at a
 * non-zero address, with a width we can actually issue. */
static int gas_usable(const acpi_gas_t *g) {
    if (!g || g->address == 0) return 0;
    if (g->space_id != ACPI_SPACE_IO && g->space_id != ACPI_SPACE_MEM) return 0;
    if (g->bit_width != 8 && g->bit_width != 16 && g->bit_width != 32) return 0;
    if (g->space_id == ACPI_SPACE_IO && g->address > 0xFFFF) return 0;
    return 1;
}

int acpi_parse_fadt(const void *buf, size_t len, acpi_info_t *out) {
    const uint8_t *b = (const uint8_t *)buf;
    if (!b || !out || len < ACPI_HDR_LEN)  return ACPI_EINVAL;
    if (!eqn(b, "FACP", 4))                return ACPI_EINVAL;

    uint32_t tlen = le32(b + 4);
    if (tlen < FADT_MIN_LEN || tlen > len) return ACPI_EINVAL;
    if (!acpi_checksum_ok(b, tlen))        return ACPI_EINVAL;

    out->fadt_revision = b[8];
    out->fadt_len      = tlen;

    /* --- the legacy 32-bit fields, always present in a valid FADT --- */
    out->dsdt_phys = le32(b + FADT_DSDT32);
    if (tlen >= FADT_PM1A_CNT32 + 4) out->pm1a_cnt = le32(b + FADT_PM1A_CNT32);
    if (tlen >= FADT_PM1B_CNT32 + 4) out->pm1b_cnt = le32(b + FADT_PM1B_CNT32);
    if (tlen >= FADT_SCI_INT + 2)    out->sci_int  = le16(b + FADT_SCI_INT);
    if (tlen >= FADT_SMI_CMD + 4)    out->smi_cmd  = le32(b + FADT_SMI_CMD);
    if (tlen >= FADT_ACPI_ENABLE + 1)  out->acpi_enable  = b[FADT_ACPI_ENABLE];
    if (tlen >= FADT_ACPI_DISABLE + 1) out->acpi_disable = b[FADT_ACPI_DISABLE];
    if (tlen >= FADT_PM1_CNT_LEN + 1)  out->pm1_cnt_len  = b[FADT_PM1_CNT_LEN];
    if (tlen >= FADT_FLAGS + 4)        out->fadt_flags   = le32(b + FADT_FLAGS);

    /* --- the 64-bit extensions win when the table is long enough to have
     * them and they are actually filled in. Firmware that provides both is
     * required to make them agree; firmware that zeroes the X_ field means
     * "use the legacy one", which is why a zero address is never adopted. --- */
    if (tlen >= FADT_X_DSDT + 8) {
        uint64_t x = le64(b + FADT_X_DSDT);
        if (x) out->dsdt_phys = x;
    }
    if (tlen >= FADT_X_PM1A_CNT + 12) {
        acpi_gas_t g;
        gas_parse(b + FADT_X_PM1A_CNT, &g);
        if (gas_usable(&g) && g.space_id == ACPI_SPACE_IO)
            out->pm1a_cnt = (uint32_t)g.address;
    }
    if (tlen >= FADT_X_PM1B_CNT + 12) {
        acpi_gas_t g;
        gas_parse(b + FADT_X_PM1B_CNT, &g);
        if (gas_usable(&g) && g.space_id == ACPI_SPACE_IO)
            out->pm1b_cnt = (uint32_t)g.address;
    }

    /* --- the reset register. Two independent conditions: the firmware must
     * say the register exists (flags bit 10) AND the descriptor must be one we
     * can issue. Either alone is not enough — plenty of tables carry a zeroed
     * GAS with the flag set. --- */
    if (tlen >= FADT_RESET_VALUE + 1) {
        gas_parse(b + FADT_RESET_REG, &out->reset_reg);
        out->reset_value = b[FADT_RESET_VALUE];
        out->reset_supported = (out->fadt_flags & ACPI_FADT_RESET_REG_SUP) &&
                               gas_usable(&out->reset_reg);
    }
    return ACPI_OK;
}

/* ====================================================================== */
/* 5. AML: \_S5_                                                          */
/* ====================================================================== */

/* AML opcodes this reads. There is no interpreter here — see acpi.h. */
#define AML_ZERO_OP     0x00
#define AML_ONE_OP      0x01
#define AML_BYTE_PREFIX 0x0A
#define AML_WORD_PREFIX 0x0B
#define AML_DWORD_PREFIX 0x0C
#define AML_QWORD_PREFIX 0x0E
#define AML_NAME_OP     0x08
#define AML_PACKAGE_OP  0x12
#define AML_ONES_OP     0xFF
#define AML_ROOT_CHAR   0x5C     /* '\' */
#define AML_PARENT_CHAR 0x5E     /* '^' */

/* Decode one integer-valued AML term at `p`. Returns its encoded length in
 * bytes, or 0 if this is not an integer constant. */
static int aml_int(const uint8_t *p, size_t avail, uint64_t *out) {
    if (avail == 0) return 0;
    switch (p[0]) {
        case AML_ZERO_OP:  *out = 0;  return 1;
        case AML_ONE_OP:   *out = 1;  return 1;
        case AML_ONES_OP:  *out = ~0ull; return 1;
        case AML_BYTE_PREFIX:
            if (avail < 2) return 0;
            *out = p[1]; return 2;
        case AML_WORD_PREFIX:
            if (avail < 3) return 0;
            *out = le16(p + 1); return 3;
        case AML_DWORD_PREFIX:
            if (avail < 5) return 0;
            *out = le32(p + 1); return 5;
        case AML_QWORD_PREFIX:
            if (avail < 9) return 0;
            *out = le64(p + 1); return 9;
        default: return 0;
    }
}

int acpi_find_s5(const void *aml, size_t len, const char *name,
                 uint8_t *slp_typ_a, uint8_t *slp_typ_b) {
    const uint8_t *p = (const uint8_t *)aml;
    if (!p || !name) return ACPI_ENOENT;
    /* An AML name is exactly four characters (shorter ones are padded with
     * '_'). Checked rather than assumed: the comparison below reads four bytes,
     * and a caller passing "" would otherwise read past a string literal. */
    for (int k = 0; k < 4; k++)
        if (name[k] == '\0') return ACPI_EINVAL;

    for (size_t i = 0; i < len; i++) {
        if (p[i] != AML_NAME_OP) continue;

        size_t j = i + 1;
        /* An optional fully-qualified prefix: "\_S5_" or "^^_S5_". */
        if (j < len && p[j] == AML_ROOT_CHAR) j++;
        while (j < len && p[j] == AML_PARENT_CHAR) j++;

        if (j + 4 > len || !eqn(p + j, name, 4)) continue;
        j += 4;

        if (j >= len || p[j] != AML_PACKAGE_OP) continue;
        j++;

        /* PkgLength: the top two bits of the lead byte say how many more
         * length bytes follow. The value itself is not needed — the element
         * count and the elements are self-describing — and trusting it would
         * mean trusting a length field, which this file does not do. */
        if (j >= len) continue;
        j += 1 + (size_t)(p[j] >> 6);

        if (j >= len) continue;
        uint8_t nelem = p[j++];
        if (nelem == 0) continue;

        /* Elements 0 and 1 are SLP_TYPa and SLP_TYPb. SLP_TYP is a 3-bit
         * field, so anything wider is not a sleep type: that is a false
         * positive on the NameOp byte, and the scan continues rather than
         * writing a garbage value into a power-management register. */
        uint8_t  v[2] = { 0, 0 };
        int      got  = 0;
        for (int e = 0; e < 2 && e < (int)nelem && j < len; e++) {
            uint64_t val = 0;
            int      n   = aml_int(p + j, len - j, &val);
            if (n == 0 || val > 7) { got = 0; break; }
            v[e] = (uint8_t)val;
            j += (size_t)n;
            got++;
        }
        if (got == 0) continue;

        if (slp_typ_a) *slp_typ_a = v[0];
        if (slp_typ_b) *slp_typ_b = (got >= 2) ? v[1] : v[0];
        return (int)i;
    }
    return ACPI_ENOENT;
}

/* ====================================================================== */
/* 6. discovery                                                           */
/* ====================================================================== */

static acpi_info_t g_acpi;

const acpi_info_t *acpi_info(void) { return &g_acpi; }

void acpi_forget(void) {
    uint8_t *p = (uint8_t *)&g_acpi;
    for (size_t i = 0; i < sizeof g_acpi; i++) p[i] = 0;
}

int acpi_available(void) {
    return g_acpi.present && g_acpi.s5_found &&
           (g_acpi.pm1a_cnt != 0 || g_acpi.pm1b_cnt != 0);
}

/* Read and validate one table header. Fills sig (5 bytes) and *len_out. */
static int table_header(uint64_t phys, char *sig, uint32_t *len_out) {
    uint8_t h[ACPI_HDR_LEN];
    if (phys == 0) return ACPI_EINVAL;
    if (mem(phys, h, sizeof h) != ACPI_OK) return ACPI_EINVAL;
    uint32_t tlen = le32(h + 4);
    /* 16 MiB is far larger than any real table and keeps the streaming
     * checksum bounded on a machine whose firmware is lying. */
    if (tlen < ACPI_HDR_LEN || tlen > (16u << 20)) return ACPI_EINVAL;
    copy_sig(sig, h);
    *len_out = tlen;
    return ACPI_OK;
}

/* Look for \_S5_ in one table's AML body, chunked so an arbitrarily large DSDT
 * is scanned without allocating. */
static int scan_aml_for_s5(uint64_t phys, uint32_t len, const char *sig) {
    if (len <= ACPI_HDR_LEN) return ACPI_ENOENT;
    uint64_t base      = phys + ACPI_HDR_LEN;
    uint32_t remaining = len - ACPI_HDR_LEN;

    for (uint32_t off = 0; off < remaining; off += ACPI_SCAN_CHUNK) {
        uint32_t want = remaining - off;
        if (want > sizeof scanbuf) want = sizeof scanbuf;
        if (mem(base + off, scanbuf, want) != ACPI_OK) return ACPI_EINVAL;

        uint8_t a = 0, b = 0;
        if (acpi_find_s5(scanbuf, want, "_S5_", &a, &b) >= 0) {
            g_acpi.s5_found  = 1;
            g_acpi.slp_typ_a = a;
            g_acpi.slp_typ_b = b;
            for (int k = 0; k < ACPI_SIG_MAX; k++) g_acpi.s5_from[k] = sig[k];
            return ACPI_OK;
        }
        if (want < sizeof scanbuf) break;      /* that was the tail */
    }
    return ACPI_ENOENT;
}

/* Parse a table we have already header-validated, if it is one we want.
 *
 * The whole FADT is buffered rather than read field by field, because
 * acpi_parse_fadt() must checksum it and a checksum over bytes that were read
 * twice is not a checksum of anything. 512 is comfortably past the 276 bytes of
 * an ACPI 6.5 FADT plus any padding a vendor has added; a longer one is refused
 * rather than truncated, since parsing a prefix and then verifying the prefix's
 * own checksum would be verifying nothing. */
static void consider_table(uint64_t phys, const char *sig, uint32_t len) {
    if (!eqn((const uint8_t *)sig, "FACP", 4)) return;
    if (g_acpi.fadt_phys) return;                  /* first valid one wins */

    static uint8_t fadt[512];
    if (len > sizeof fadt) {
        kputs("acpi: FADT is implausibly long - ignoring it\n");
        return;
    }
    if (mem(phys, fadt, len) != ACPI_OK) return;
    if (acpi_parse_fadt(fadt, len, &g_acpi) == ACPI_OK)
        g_acpi.fadt_phys = phys;
}

static void record_sig(const char *sig) {
    if (g_acpi.table_count < ACPI_MAX_TABLES)
        for (int k = 0; k < ACPI_SIG_MAX; k++)
            g_acpi.table_sig[g_acpi.table_count][k] = sig[k];
    g_acpi.table_count++;
}

/* Walk an RSDT (32-bit entries) or XSDT (64-bit entries). */
static int walk_sdt(uint64_t phys, int is_xsdt) {
    char     sig[ACPI_SIG_MAX];
    uint32_t len = 0;
    if (table_header(phys, sig, &len) != ACPI_OK) return ACPI_EINVAL;
    if (!eqn((const uint8_t *)sig, is_xsdt ? "XSDT" : "RSDT", 4)) return ACPI_EINVAL;
    if (!checksum_phys_ok(phys, len)) return ACPI_EINVAL;

    uint32_t entsz = is_xsdt ? 8 : 4;
    uint32_t n     = (len - ACPI_HDR_LEN) / entsz;

    for (uint32_t i = 0; i < n; i++) {
        uint8_t e[8];
        if (mem(phys + ACPI_HDR_LEN + (uint64_t)i * entsz, e, entsz) != ACPI_OK)
            continue;
        uint64_t tphys = is_xsdt ? le64(e) : le32(e);

        char     tsig[ACPI_SIG_MAX];
        uint32_t tlen = 0;
        if (table_header(tphys, tsig, &tlen) != ACPI_OK) continue;
        record_sig(tsig);
        consider_table(tphys, tsig, tlen);
    }
    return ACPI_OK;
}

int acpi_init(void) {
    acpi_forget();

    /* The EBDA's segment address lives at 0x40E in the BIOS data area; its
     * first KiB is the first place the spec says to look. */
    uint8_t  ebda_seg[2];
    uint64_t found = 0;
    if (mem(0x40E, ebda_seg, 2) == ACPI_OK) {
        uint64_t ebda = (uint64_t)le16(ebda_seg) << 4;
        /* Below 0xA0000 or it is not the EBDA. */
        if (ebda >= 0x400 && ebda < 0xA0000)
            found = rsdp_scan(ebda, ebda + 1024, &g_acpi);
    }
    if (!found) found = rsdp_scan(0xE0000, 0x100000, &g_acpi);

    if (!found) {
        /* A candidate that passed the signature check and then failed the
         * extended checksum will have left fields behind. Zero the snapshot so
         * "not present" means every field is 0, and no consumer can read a
         * pointer that was never vouched for. */
        acpi_forget();
        kputs("acpi: no RSDP in the EBDA or 0xE0000-0xFFFFF "
              "(no firmware tables on this machine)\n");
        return ACPI_ENOENT;
    }
    g_acpi.present   = 1;
    g_acpi.rsdp_phys = found;

    /* An XSDT's entries are 64-bit and it is the authoritative list on any
     * revision-2 RSDP; the RSDT is the fallback for ACPI 1.0 firmware. */
    int walked = ACPI_EINVAL;
    if (g_acpi.rsdp_revision >= 2 && g_acpi.xsdt_phys) {
        walked = walk_sdt(g_acpi.xsdt_phys, 1);
        if (walked == ACPI_OK) g_acpi.used_xsdt = 1;
    }
    if (walked != ACPI_OK && g_acpi.rsdt_phys) {
        g_acpi.table_count = 0;
        walked = walk_sdt(g_acpi.rsdt_phys, 0);
    }

    /* \_S5_ normally lives in the DSDT; some firmware puts sleep states in an
     * SSDT instead, so every SSDT is a fallback. */
    if (g_acpi.dsdt_phys) {
        char     sig[ACPI_SIG_MAX];
        uint32_t len = 0;
        if (table_header(g_acpi.dsdt_phys, sig, &len) == ACPI_OK &&
            eqn((const uint8_t *)sig, "DSDT", 4)) {
            g_acpi.dsdt_len = len;
            scan_aml_for_s5(g_acpi.dsdt_phys, len, sig);
        }
    }
    if (!g_acpi.s5_found && walked == ACPI_OK) {
        uint64_t root  = g_acpi.used_xsdt ? g_acpi.xsdt_phys : g_acpi.rsdt_phys;
        uint32_t entsz = g_acpi.used_xsdt ? 8 : 4;
        char     rsig[ACPI_SIG_MAX];
        uint32_t rlen = 0;
        if (table_header(root, rsig, &rlen) == ACPI_OK) {
            uint32_t n = (rlen - ACPI_HDR_LEN) / entsz;
            for (uint32_t i = 0; i < n && !g_acpi.s5_found; i++) {
                uint8_t e[8];
                if (mem(root + ACPI_HDR_LEN + (uint64_t)i * entsz, e, entsz) != ACPI_OK)
                    continue;
                uint64_t tphys = g_acpi.used_xsdt ? le64(e) : le32(e);
                char     tsig[ACPI_SIG_MAX];
                uint32_t tlen = 0;
                if (table_header(tphys, tsig, &tlen) != ACPI_OK) continue;
                if (!eqn((const uint8_t *)tsig, "SSDT", 4)) continue;
                scan_aml_for_s5(tphys, tlen, tsig);
            }
        }
    }

    /* One summary line, then one line of the numbers soft-off depends on.
     * kprintf's grammar is %s %c %d %u %x %p and nothing else (kernel.h), and
     * %x is 32-bit, so 64-bit addresses are rendered with snprintf first. */
    char addrs[96];
    snprintf(addrs, sizeof addrs, "rsdp=0x%llx %s=0x%llx fadt=0x%llx dsdt=0x%llx",
             (unsigned long long)g_acpi.rsdp_phys,
             g_acpi.used_xsdt ? "xsdt" : "rsdt",
             (unsigned long long)(g_acpi.used_xsdt ? g_acpi.xsdt_phys
                                                   : g_acpi.rsdt_phys),
             (unsigned long long)g_acpi.fadt_phys,
             (unsigned long long)g_acpi.dsdt_phys);
    kprintf("acpi: rev %u \"%s\", %d table(s), %s\n",
            (unsigned)g_acpi.rsdp_revision, g_acpi.oem_id,
            g_acpi.table_count, addrs);

    if (g_acpi.fadt_phys)
        kprintf("acpi: pm1a_cnt=0x%x pm1b_cnt=0x%x len=%u smi=0x%x "
                "s5=%s(a=%u b=%u from %s) reset=%s\n",
                (unsigned)g_acpi.pm1a_cnt, (unsigned)g_acpi.pm1b_cnt,
                (unsigned)g_acpi.pm1_cnt_len, (unsigned)g_acpi.smi_cmd,
                g_acpi.s5_found ? "yes" : "NO",
                (unsigned)g_acpi.slp_typ_a, (unsigned)g_acpi.slp_typ_b,
                g_acpi.s5_found ? g_acpi.s5_from : "-",
                g_acpi.reset_supported ? "yes" : "no");
    else
        kputs("acpi: no valid FADT - soft-off must fall back to a "
              "platform-specific port\n");

    return ACPI_OK;
}

/* ====================================================================== */
/* 7. the plan                                                            */
/* ====================================================================== */

const char *acpi_action_kind_name(acpi_action_kind_t k) {
    switch (k) {
        case ACPI_ACT_IO_WRITE:     return "io-write";
        case ACPI_ACT_MEM_WRITE8:   return "mem-write8";
        case ACPI_ACT_KBD_PULSE:    return "i8042-pulse";
        case ACPI_ACT_TRIPLE_FAULT: return "triple-fault";
        case ACPI_ACT_NONE:         break;
    }
    return "none";
}

int acpi_action_has_operand(acpi_action_kind_t k) {
    return k == ACPI_ACT_IO_WRITE || k == ACPI_ACT_MEM_WRITE8;
}

/* Append a step, unless an earlier step would already have done exactly this.
 * The de-duplication is what stops the QEMU fallbacks from re-issuing the write
 * the ACPI path already derived — on QEMU, PM1a_CNT *is* 0x604 and S5's SLP_TYP
 * *is* 0, so without this the plan would contain the same write twice and the
 * log could not show which mechanism was responsible. */
static void plan_add(acpi_plan_t *p, acpi_action_kind_t kind, const char *method,
                     uint64_t address, uint32_t value, uint8_t bit_width,
                     uint32_t settle_ms) {
    if (p->n >= ACPI_PLAN_MAX) return;
    for (int i = 0; i < p->n; i++) {
        const acpi_action_t *s = &p->step[i];
        if (s->kind == kind && s->address == address && s->value == value &&
            s->bit_width == bit_width)
            return;
    }
    p->step[p->n].kind      = kind;
    p->step[p->n].method    = method;
    p->step[p->n].address   = address;
    p->step[p->n].value     = value;
    p->step[p->n].bit_width = bit_width;
    p->step[p->n].settle_ms = settle_ms;
    p->n++;
}

/* PM1_CNT is a 16-bit register on every real board; PM1_CNT_LEN says so in
 * bytes. A board claiming 4 gets a 32-bit write, anything else gets 16 — an
 * 8-bit write would truncate SLP_EN (bit 13) away entirely, which would look
 * like a successful write that did nothing. */
static uint8_t pm1_width(const acpi_info_t *in) {
    return (in->pm1_cnt_len == 4) ? 32 : 16;
}

int acpi_power_plan(const acpi_info_t *in, acpi_power_op_t op, acpi_plan_t *out) {
    if (!in || !out) return ACPI_EINVAL;
    out->n = 0;
    for (int i = 0; i < ACPI_PLAN_MAX; i++) {
        out->step[i].kind      = ACPI_ACT_NONE;
        out->step[i].method    = "none";
        out->step[i].address   = 0;
        out->step[i].value     = 0;
        out->step[i].bit_width = 0;
        out->step[i].settle_ms = 0;
    }

    if (op == ACPI_POWER_REBOOT) {
        /* 1. the register the firmware itself nominated. */
        if (in->reset_supported) {
            if (in->reset_reg.space_id == ACPI_SPACE_IO)
                plan_add(out, ACPI_ACT_IO_WRITE, "acpi-reset",
                         in->reset_reg.address, in->reset_value,
                         in->reset_reg.bit_width, 100);
            else
                plan_add(out, ACPI_ACT_MEM_WRITE8, "acpi-reset",
                         in->reset_reg.address, in->reset_value, 8, 100);
        }
        /* 2. the keyboard controller, on every PC since 1984. */
        plan_add(out, ACPI_ACT_KBD_PULSE, "i8042-pulse", 0x64, 0xFE, 8, 200);
        /* 3. and the one that cannot be refused. */
        plan_add(out, ACPI_ACT_TRIPLE_FAULT, "triple-fault", 0, 0, 0, 0);
        return out->n;
    }

    /* ---- soft-off ---- */
    uint8_t w = pm1_width(in);

    /* 1. real ACPI: SLP_TYP from this board's \_S5_, plus SLP_EN, into the
     * control block(s) the FADT named. Both blocks are written when both
     * exist — the spec requires it, and a board with a PM1b block will not go
     * down on PM1a alone. */
    if (in->present && in->s5_found && in->pm1a_cnt)
        plan_add(out, ACPI_ACT_IO_WRITE, "acpi-pm1a", in->pm1a_cnt,
                 ((uint32_t)in->slp_typ_a << ACPI_PM1_CNT_SLP_TYP_SHIFT) |
                     ACPI_PM1_CNT_SLP_EN, w, 100);
    if (in->present && in->s5_found && in->pm1b_cnt)
        plan_add(out, ACPI_ACT_IO_WRITE, "acpi-pm1b", in->pm1b_cnt,
                 ((uint32_t)in->slp_typ_b << ACPI_PM1_CNT_SLP_TYP_SHIFT) |
                     ACPI_PM1_CNT_SLP_EN, w, 100);

    /* 2. the safety net, and nothing more than that. Both of these are QEMU's
     * PIIX4 PM1a control block at its two historical addresses with SLP_TYP=0
     * (soft-off on that chipset). On QEMU with working tables, step 1 above
     * already IS the 0x604 write and plan_add() elides the duplicate. */
    plan_add(out, ACPI_ACT_IO_WRITE, "qemu-604", 0x604,
             ACPI_PM1_CNT_SLP_EN, 16, 100);
    plan_add(out, ACPI_ACT_IO_WRITE, "qemu-b004", 0xB004,
             ACPI_PM1_CNT_SLP_EN, 16, 100);
    return out->n;
}

size_t acpi_action_describe(const acpi_action_t *a, char *dst, size_t cap) {
    int n = 0;
    if (!dst || cap == 0) return 0;
    dst[0] = '\0';
    if (!a) return 0;

    switch (a->kind) {
        case ACPI_ACT_IO_WRITE:
            n = snprintf(dst, cap, "io 0x%llx<-0x%x/%u",
                         (unsigned long long)a->address,
                         (unsigned)a->value, (unsigned)a->bit_width);
            break;
        case ACPI_ACT_MEM_WRITE8:
            n = snprintf(dst, cap, "mem 0x%llx<-0x%x/8",
                         (unsigned long long)a->address, (unsigned)a->value);
            break;
        default:
            n = snprintf(dst, cap, "%s", acpi_action_kind_name(a->kind));
            break;
    }
    if (n < 0) { dst[0] = '\0'; return 0; }
    return ((size_t)n >= cap) ? cap - 1 : (size_t)n;
}

size_t acpi_plan_describe(const acpi_plan_t *p, char *dst, size_t cap) {
    if (!dst || cap == 0) return 0;
    dst[0] = '\0';
    if (!p) return 0;

    size_t o = 0;
    for (int i = 0; i < p->n && i < ACPI_PLAN_MAX; i++) {
        char body[64], one[128];
        acpi_action_describe(&p->step[i], body, sizeof body);
        if (acpi_action_has_operand(p->step[i].kind))
            snprintf(one, sizeof one, "%s%s(%s)", i ? " " : "",
                     p->step[i].method, body);
        else
            snprintf(one, sizeof one, "%s%s", i ? " " : "", p->step[i].method);

        for (size_t k = 0; one[k]; k++) {
            if (o + 1 >= cap) { dst[o] = '\0'; return o; }
            dst[o++] = one[k];
        }
        dst[o] = '\0';
    }
    return o;
}
