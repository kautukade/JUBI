/* acpi.h — the firmware tables, and the two irreversible things they enable:
 * turning this machine off and starting it again.
 *
 * PURPOSE
 *   "Reboot yourself" and "shut down" are things every real operating system
 *   does, and until now this one could not. The interface is a sentence, so the
 *   sentence has to work: the model needs syscalls that genuinely end the
 *   machine, not a printed apology. That means ACPI, because on a PC ACPI is the
 *   only *architectural* way to reach soft-off — the mechanism is described by
 *   tables the firmware left in memory, and it differs from board to board.
 *
 *   So this module does the boring, real thing:
 *
 *     RSDP    found by scanning the EBDA and then 0xE0000-0xFFFFF, validated by
 *             checksum (and by the extended checksum when revision >= 2).
 *     RSDT /  walked to enumerate every table the firmware published; the XSDT
 *     XSDT    is preferred when the RSDP is revision 2+ because its entries are
 *             64-bit and the RSDT's are not.
 *     FADT    ("FACP") read for the PM1a/PM1b control blocks, the SMI command
 *             port and its ACPI-enable value, the reset register, and the flag
 *             that says whether that register may be used at all.
 *     DSDT    scanned as AML for the \_S5_ package, which is where the two
 *             SLP_TYP values that mean "off" on THIS board actually live. They
 *             are not architectural constants and cannot be guessed.
 *
 *   Soft-off is then SLP_TYP | SLP_EN written to PM1a_CNT (and PM1b_CNT if the
 *   board has one). Reset is the FADT's reset register, then the 8042 pulse,
 *   then a triple fault.
 *
 * THE QEMU FALLBACKS ARE A SAFETY NET, NOT THE FEATURE
 *   Writing 0x2000 to port 0x604 powers off QEMU, and 0xB004 did on older
 *   versions. Both are in the plan — LAST. They are what happens when the real
 *   tables are missing or lie, and on QEMU they are the same write the ACPI path
 *   already derives (PM1a_CNT is 0x604 there, and PIIX4's SLP_TYP for S5 is 0),
 *   so a passing test proves nothing about which path did the work. That is
 *   exactly why the discovery above is not skipped: acpi_power_plan() reports
 *   the method it chose by name, and power_info tells the operator whether this
 *   machine would go down by ACPI or by a QEMU-specific hack.
 *
 * WHAT THIS MODULE DELIBERATELY DOES NOT DO
 *   No AML interpreter. There is no method evaluation, no namespace, no
 *   operation regions — \_S5_ is located by pattern-matching the byte sequence
 *   that declares it (NameOp, the name, PackageOp, then integer elements), which
 *   is what every early-boot / bootloader-grade ACPI consumer does and is enough
 *   for S5. Anything needing _PTS or _GTS is out of scope and is documented as
 *   such below. No S1-S4, no thermal, no MADT, no interrupt routing.
 *
 * THE SHAPE OF THE CODE: PLAN, THEN EXECUTE
 *   Powering off is untestable by nature — if it works, the process asserting on
 *   it stops existing. So the decision is separated from the act:
 *
 *     acpi_power_plan()   PURE. Turns the discovered acpi_info_t into an ordered
 *                         list of concrete actions ("write 0x2000 to I/O port
 *                         0x604, 16 bits wide"). No hardware is touched, so the
 *                         entire policy — which mechanism wins, what the
 *                         fallback order is, which duplicate writes are elided —
 *                         is asserted on the host in tests/host/test_acpi.c.
 *     acpi_plan_execute() performs those actions through the platform backend.
 *
 *   Everything that reads a table goes through acpi_platform_t as well, so the
 *   parsers are driven against synthetic RSDPs, XSDTs, FADTs and DSDTs built
 *   byte by byte in the tests — including malformed ones.
 *
 * PUBLIC API
 *   Discovery:      acpi_init, acpi_info, acpi_available
 *   Acting:         acpi_power_off, acpi_reboot            (neither returns on
 *                                                           success)
 *   Explaining:     acpi_power_plan, acpi_plan_describe, acpi_method_name
 *   Pure parsing:   acpi_checksum, acpi_checksum_ok, acpi_rsdp_check,
 *                   acpi_parse_fadt, acpi_find_s5
 *   Seams:          acpi_set_platform, acpi_platform, acpi_set_prepare_hook
 *
 * ORDER OF OPERATIONS, AND WHY THE TRACE LINE COMES FIRST
 *   A power-off that leaves no ground truth behind is the exact failure this
 *   project exists to avoid: the operator would be left with a machine that
 *   stopped and a transcript that never said why. There is no "after" to print
 *   in, so the kernel's account of the action is emitted BEFORE the final write,
 *   and the last line in the serial log of a successful shutdown is the trace
 *   line that committed to it. tools/power_tools.c documents the two-line
 *   protocol that makes that honest rather than a claim of success.
 *
 *   Before the write, acpi_power_off()/acpi_reboot() run the clean path: drain
 *   the UART so the final bytes are really on the wire, bring drivers down
 *   through the existing lifecycle (drivers_suspend_all), and print a last line.
 *
 * DEPENDENCIES
 *   Parsing and planning depend on nothing but <stdint.h>/<stddef.h> and the
 *   backend function pointers, which is what lets the same object file compile
 *   freestanding into the kernel and natively for the host tests. The kernel's
 *   default backend adds io.h (port I/O), kernel.h (console, mdelay) and
 *   driver.h (the suspend lifecycle); all of that is compiled out of host
 *   builds.
 *
 * FUTURE EXTENSION POINTS
 *   - S3 suspend-to-RAM wants \_S3_ (the same scan with a different name) plus a
 *     wake vector in the FACS and a resume trampoline; acpi_find_s5() is
 *     deliberately written as "find this 4-character name's package".
 *   - Proper sleep needs the _PTS/_GTS control methods evaluated, i.e. an AML
 *     interpreter. The place for it is behind acpi_power_prepare().
 *   - The MADT (APIC topology) and the HPET are two more entries in the table
 *     list acpi_init() already walks; nothing here needs them yet.
 *   - 0xCF9 bit 2 (the PCI reset control register) is a fourth reboot mechanism,
 *     usually the same one the FADT names on modern chipsets. It slots into
 *     acpi_power_plan() as one more ACPI_ACT_IO_WRITE.
 */
#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>

/* ---- error codes (errno-style negatives, shared space with vfs.h/tool.h) ----
 * NOTE: only -2/-22/-28 have symbolic names in lib/trace.c's table, so
 * ACPI_ENODEV renders as "-19" in a trace line rather than "ENODEV". That is
 * the documented fallback (trace.h: an unknown code is never silently
 * swallowed) and adding a name would mean editing lib/trace.c. */
#define ACPI_OK          0
#define ACPI_ENOENT     -2    /* no RSDP / no such table / no \_S5_ package   */
#define ACPI_ENODEV    -19    /* no mechanism on this platform did anything   */
#define ACPI_EINVAL    -22    /* bad argument, or a table that fails its own
                                 length/checksum/signature validation         */
#define ACPI_ENOSPC    -28    /* plan or description buffer too small         */

/* ---- ACPI Generic Address Structure, decoded ---- */
#define ACPI_SPACE_MEM   0    /* system memory space */
#define ACPI_SPACE_IO    1    /* system I/O space    */

typedef struct acpi_gas {
    uint8_t  space_id;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} acpi_gas_t;

/* ---- PM1 control register bits (ACPI 4.7.3.2.1) ---- */
#define ACPI_PM1_CNT_SCI_EN   0x0001u        /* ACPI mode is on, not legacy  */
#define ACPI_PM1_CNT_SLP_EN   0x2000u        /* write 1: enter SLP_TYP now   */
#define ACPI_PM1_CNT_SLP_TYP_SHIFT 10
#define ACPI_PM1_CNT_SLP_TYP_MASK  0x1C00u   /* bits 12:10                   */

/* ---- FADT flags we care about ---- */
#define ACPI_FADT_RESET_REG_SUP 0x0400u      /* bit 10: reset register usable */

/* Longest OEM ID (6 bytes) plus a NUL, and the 4-byte signatures + NUL. */
#define ACPI_OEMID_MAX 7
#define ACPI_SIG_MAX   5

/* How many table pointers acpi_init() will record. Enumeration continues past
 * this — the FADT is still found — only the recorded list stops growing. */
#define ACPI_MAX_TABLES 32

/* Bytes of AML read at a time when scanning for \_S5_. Exposed because the scan
 * is chunked and a match must be found even when it straddles two chunks; the
 * host test places one exactly there. */
#define ACPI_SCAN_CHUNK 512

/* Longest byte sequence acpi_find_s5() needs to see to match: NameOp, an
 * optional RootChar, the 4-character name, PackageOp, up to 4 PkgLength bytes,
 * the element count, and two integer elements of up to 5 bytes each. */
#define ACPI_S5_PATTERN_MAX 24

/* Everything discovery found, in one snapshot. Zeroed before use, so a field is
 * 0 exactly when it was absent or unreadable. */
typedef struct acpi_info {
    int      present;             /* a valid RSDP was found                   */
    uint64_t rsdp_phys;
    uint8_t  rsdp_revision;       /* 0 = ACPI 1.0, 2 = 2.0+                   */
    char     oem_id[ACPI_OEMID_MAX];
    uint64_t rsdt_phys;           /* 0 when absent                            */
    uint64_t xsdt_phys;           /* 0 when absent                            */
    int      used_xsdt;           /* which one the walk actually used          */

    int      table_count;         /* tables enumerated (may exceed recorded)   */
    char     table_sig[ACPI_MAX_TABLES][ACPI_SIG_MAX];

    uint64_t fadt_phys;           /* 0 when there is no valid FADT             */
    uint32_t fadt_len;
    uint8_t  fadt_revision;
    uint32_t fadt_flags;

    uint64_t dsdt_phys;
    uint32_t dsdt_len;

    uint32_t pm1a_cnt;            /* I/O port; 0 when absent                   */
    uint32_t pm1b_cnt;
    uint8_t  pm1_cnt_len;         /* bytes; 2 per spec                         */

    uint32_t smi_cmd;             /* SMI command port; 0 when absent           */
    uint8_t  acpi_enable;         /* value that switches the board to ACPI mode */
    uint8_t  acpi_disable;
    uint16_t sci_int;

    int      s5_found;
    uint8_t  slp_typ_a;           /* SLP_TYP for PM1a that means soft-off      */
    uint8_t  slp_typ_b;
    char     s5_from[ACPI_SIG_MAX]; /* which table it came from ("DSDT"/"SSDT") */

    int        reset_supported;   /* FADT flag set AND the GAS is usable       */
    acpi_gas_t reset_reg;
    uint8_t    reset_value;
} acpi_info_t;

/* ====================================================================== */
/* the platform seam                                                      */
/* ====================================================================== */

/* Every access to firmware memory, every port access, and every way of ending
 * the machine goes through here. The kernel installs real hardware; host tests
 * install synthetic tables and a recorder. Any member may be NULL, in which
 * case the corresponding capability is simply absent — that is how a host build
 * starts out, so nothing accidentally "works" against uninitialised memory.
 *
 * mem_read returns 0 on success and negative when the range is not readable on
 * this machine (the kernel's identity map only covers the low 4 GiB). */
typedef struct acpi_platform {
    int      (*mem_read)(uint64_t phys, void *dst, size_t len);
    int      (*mem_write8)(uint64_t phys, uint8_t value);
    void     (*io_write)(uint32_t port, uint32_t value, uint8_t bit_width);
    uint32_t (*io_read)(uint32_t port, uint8_t bit_width);
    void     (*kbd_pulse)(void);        /* 8042: pulse the CPU reset line     */
    void     (*triple_fault)(void);     /* last resort; must not return       */
    void     (*delay_ms)(uint32_t ms);
} acpi_platform_t;

/* Install a backend, or restore this build's default with NULL. */
void                   acpi_set_platform(const acpi_platform_t *p);
const acpi_platform_t *acpi_platform(void);

/* ====================================================================== */
/* discovery                                                              */
/* ====================================================================== */

/* Find and validate the RSDP, walk the RSDT/XSDT, parse the FADT, and extract
 * \_S5_ from the DSDT (or any SSDT, if the DSDT does not declare it). Prints a
 * one-line summary. Returns ACPI_OK when at least a valid RSDP was found, else
 * ACPI_ENOENT. Safe to call twice; the second call re-reads everything. */
int acpi_init(void);

/* The snapshot from the last acpi_init(). Never NULL; all-zero before the first
 * call, so `acpi_info()->present` is the one test a caller needs. */
const acpi_info_t *acpi_info(void);

/* Nonzero when a valid RSDP was found AND soft-off can be attempted through
 * real ACPI (a PM1 control block and an \_S5_ sleep type). */
int acpi_available(void);

/* Discard the snapshot. For tests that want a clean slate. */
void acpi_forget(void);

/* ====================================================================== */
/* pure parsing — no hardware, no allocation, host-tested                 */
/* ====================================================================== */

/* Sum of `len` bytes, modulo 256. An ACPI structure is valid only when this is
 * zero over its whole declared length. */
uint8_t acpi_checksum(const void *bytes, size_t len);
int     acpi_checksum_ok(const void *bytes, size_t len);   /* 1 = sums to 0 */

/* Validate a candidate RSDP held in `buf`. Checks the "RSD PTR " signature, the
 * 20-byte ACPI 1.0 checksum, and — when the revision is 2 or greater — the
 * declared length and the checksum over all of it. On success fills `out`
 * (revision, oem_id, rsdt_phys, xsdt_phys) and returns ACPI_OK; `out->present`
 * is NOT set, since that is the caller's business. Returns ACPI_EINVAL for
 * anything that fails validation, including a 2.0 RSDP whose length is absurd.
 * `len` may be as short as 20. */
int acpi_rsdp_check(const void *buf, size_t len, acpi_info_t *out);

/* Parse a FADT ("FACP") body of `len` bytes into `out`, filling the PM1, SMI,
 * reset and DSDT fields. Requires a 36-byte header with a valid length and a
 * checksum of zero, and prefers the 64-bit X_ fields over their 32-bit
 * predecessors when a revision >= 3 table provides them. A field that is absent
 * or nonsensical is left at zero rather than guessed. Returns ACPI_OK or
 * ACPI_EINVAL. Does not touch `out`'s \_S5_ fields. */
int acpi_parse_fadt(const void *buf, size_t len, acpi_info_t *out);

/* Find `name` (exactly 4 characters, e.g. "_S5_") declared as a package of
 * integers in a block of AML, and return its first two elements — the SLP_TYP
 * values for PM1a and PM1b. Matches NameOp [RootChar] name PackageOp PkgLength
 * NumElements, then decodes ZeroOp/OneOp/OnesOp/BytePrefix/WordPrefix/
 * DWordPrefix elements. A package with one element yields the same value for
 * both. Returns the offset of the match (>= 0), ACPI_ENOENT when there is no
 * match, or ACPI_EINVAL if `name` is not four characters long. */
int acpi_find_s5(const void *aml, size_t len, const char *name,
                 uint8_t *slp_typ_a, uint8_t *slp_typ_b);

/* ====================================================================== */
/* the power plan                                                         */
/* ====================================================================== */

typedef enum acpi_action_kind {
    ACPI_ACT_NONE = 0,
    ACPI_ACT_IO_WRITE,       /* out(port=address, value, bit_width)         */
    ACPI_ACT_MEM_WRITE8,     /* *(volatile uint8_t *)address = value        */
    ACPI_ACT_KBD_PULSE,      /* 8042: 0x64 <- 0xFE, pulsing CPU reset       */
    ACPI_ACT_TRIPLE_FAULT,   /* zero the IDT and fault; never returns       */
} acpi_action_kind_t;

/* One step. `method` is a short kernel-source string ("acpi-pm1a", "qemu-604")
 * that the trace line and power_info report verbatim, so the operator can tell
 * which mechanism actually took the machine down. */
typedef struct acpi_action {
    acpi_action_kind_t kind;
    const char        *method;
    uint64_t           address;
    uint32_t           value;
    uint8_t            bit_width;
    uint32_t           settle_ms;   /* wait this long before the next step */
} acpi_action_t;

#define ACPI_PLAN_MAX 6

typedef struct acpi_plan {
    int            n;
    acpi_action_t  step[ACPI_PLAN_MAX];
} acpi_plan_t;

typedef enum acpi_power_op {
    ACPI_POWER_OFF = 0,
    ACPI_POWER_REBOOT = 1,
} acpi_power_op_t;

/* Build the ordered list of things to try, from `in` alone. PURE: reads no
 * hardware and writes none. Real ACPI first, platform hacks last, and a step
 * that would repeat an earlier step's exact write is elided.
 *
 * ACPI_POWER_OFF may return a plan of length 0 — a machine with no ACPI tables
 * and no QEMU fallback ports genuinely cannot be turned off, and saying so is
 * better than pretending. ACPI_POWER_REBOOT always ends with a triple fault,
 * so its plan is never empty and never survivable.
 *
 * Returns the number of steps, or ACPI_EINVAL for bad arguments. */
int acpi_power_plan(const acpi_info_t *in, acpi_power_op_t op, acpi_plan_t *out);

/* Render one step as "io 0x604<-0x2000/16", "mem 0xfed00040<-0x1/8", or just
 * the kind's name for the ones that take no operand. Always NUL-terminates;
 * returns the bytes written excluding the NUL. */
size_t acpi_action_describe(const acpi_action_t *a, char *dst, size_t cap);

/* Render a plan as "acpi-pm1a(io 0x604<-0x2000/16) qemu-b004(...)" for a tool
 * result or a log line. Always NUL-terminates; returns the number of bytes
 * written excluding the NUL. Contains no bracket characters, so it is safe to
 * pass to trace.h as an argument. */
size_t acpi_plan_describe(const acpi_plan_t *p, char *dst, size_t cap);

/* Human name for a step kind ("io-write", "triple-fault", ...). Never NULL. */
const char *acpi_action_kind_name(acpi_action_kind_t k);

/* Nonzero when a step writes a value to an address, so a description of it
 * carries an operand worth printing next to the method name. Zero for the steps
 * that are fully named by their method alone (the 8042 pulse, the triple
 * fault) — printing "i8042-pulse: i8042-pulse" for those would be noise. */
int acpi_action_has_operand(acpi_action_kind_t k);

/* Perform every step in order, through the platform backend, waiting
 * settle_ms after each. Returns the number of steps actually performed; a step
 * whose backend is missing is skipped and not counted. If this returns at all,
 * the machine is still running and every mechanism failed. */
int acpi_plan_execute(const acpi_plan_t *p);

/* ====================================================================== */
/* acting                                                                 */
/* ====================================================================== */

/* Switch the board from legacy to ACPI mode if the FADT says that is needed:
 * write acpi_enable to the SMI command port and wait for SCI_EN to appear in
 * PM1a_CNT. Returns ACPI_OK when ACPI mode is on (including when it already
 * was, or when no SMI command port exists — nothing to do), or ACPI_ENODEV when
 * the board never acknowledged. Best effort: soft-off is attempted regardless,
 * because plenty of boards accept SLP_EN in legacy mode. */
int acpi_enable_hw(void);

/* Bring the machine down cleanly and then end it. In order: run the prepare
 * hook (drain the UART, drivers_suspend_all(), print the final line), enable
 * ACPI mode if required, then execute the plan.
 *
 * DOES NOT RETURN on success. A return value is a failure report:
 *   ACPI_ENODEV  every mechanism was tried and the machine is still running.
 *   ACPI_EINVAL  no plan could be built.
 * `reason` is copied into the final line and may be NULL. It is UNTRUSTED (it
 * comes from the model), so it is length-bounded and stripped of control
 * characters before printing. */
int acpi_power_off(const char *reason);

/* The same, for a restart. The plan ends in a triple fault, so this never
 * returns; the return type exists only so a caller cannot forget that the
 * power-off twin can. */
int acpi_reboot(const char *reason);

/* Replace the clean-shutdown step. The default implementation is the real one
 * (UART drain + drivers_suspend_all + the final line); host tests install a
 * recorder to prove it ran BEFORE the first port write. NULL restores the
 * default. `op` says which transition is in progress. */
typedef void (*acpi_prepare_fn)(acpi_power_op_t op, const char *reason);
void acpi_set_prepare_hook(acpi_prepare_fn fn);

/* Copy `reason` into `dst` with every control character, DEL and bracket
 * dropped, truncating to cap-1. Exposed because both the final console line and
 * the tool's trace argument need the same treatment, and because it is worth
 * testing against a 10 KB reason full of newlines. Returns the length written. */
size_t acpi_sanitize_reason(char *dst, size_t cap, const char *reason);

#endif /* ACPI_H */
