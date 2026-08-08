/* test_dvm_tools.c — the model-writes-a-driver loop, driven end to end.
 *
 * WHAT THIS SUITE IS
 *   tools/dvm_tools.c is the seam where a language model's text becomes port
 *   I/O. Everything on the kernel side of that seam is pure logic plus two
 *   function-pointer boundaries, so the whole loop runs natively:
 *
 *     - pci_cfg_read32() is replaced by a synthetic bus modelled on the machine
 *       QEMU presents, including the awkward cases (a claimed device, a
 *       bus-mastering device, a BAR that overlaps COM1, a BAR inside the
 *       interrupt-controller block, two devices with adjacent BARs);
 *     - dvm_io_t is replaced by TLK1, a synthetic device with a reset, a
 *       ready handshake, a rate register that only latches while disabled, a
 *       counter that advances with time, and a 64-bit bus-master DMA engine that
 *       really reads and writes host memory at the address it is given. It is the
 *       device the recorded session in vm/transcripts/dvm_bringup.h brings up;
 *     - net/model_mock.c is the model, and net/chat.c is the REAL turn loop.
 *
 *   So "the model submits a program, the kernel refuses it, the model reads the
 *   access log, fixes the program and the device comes up" is a test, not a
 *   story — and it needs no network, no API key and no QEMU.
 *
 * THE LOAD-BEARING ASSERTIONS
 *   `forbidden` — TLK1 counts every access it should never have seen: any port
 *   outside the window the target's BAR earned, any MMIO outside a granted
 *   window, any PCI function other than the target. It must be zero at the end
 *   of the suite, including after the fuzzers. That is a mechanical proof that a
 *   program can only reach the device it was pointed at.
 *
 *   `cfg_bad` — driver_run now WRITES config space, because a device cannot DMA
 *   without the bus-master bit and the ISA has no config write. So the synthetic
 *   bus checks every write: offset 0x04 only, status bits preserved, no bit
 *   outside {io, mem, bus-master} changed, and only the named function. It must
 *   be zero, which is the proof that "the model still cannot touch config space,
 *   and the kernel touches exactly three bits of one register".
 *
 *   `dma_wild` — NOT required to be zero, and that is the honest part. It counts
 *   the times a program pointed a bus master outside the buffer it was given. On
 *   real hardware every one of those is a write that happens. The VM refuses none
 *   of them, because it polices the CPU and not the DMA engine; there is no
 *   IOMMU. What the suite proves instead is that the kernel's own side is right:
 *   only the named device is made a master, the program is handed exactly one
 *   valid address, and a near-miss overrun is reported with an offset.
 *
 *   NOT PROVED, AND NOT TRUE: that the address a program gives the hardware is
 *   always on the record. The access log is a 256-entry RING against a 1024-access
 *   budget, so it keeps the last 256 events; the serial trace is bounded the other
 *   way and keeps the first 128 or 256. A write between the two windows — 200
 *   polls, the descriptor address, 300 more polls — is in neither. The tests here
 *   use short programs, i.e. only the regime where the claim holds, which is
 *   exactly why the overclaim in tools/dvm_tools.c's header survived this suite.
 *
 * WHAT THIS SUITE CANNOT PROVE
 *   That a real model, with a real API key, writes programs like these. There
 *   is no key compiled into this kernel (the API answers 401), so the assistant
 *   turns in the transcript are authored rather than captured. What is proved
 *   is that when those bytes arrive, the kernel does exactly this. It also
 *   cannot prove dvm_io_hardware() reaches silicon — vm/programs/ac97_boot.c
 *   and tests/qemu/cases/ac97.case do that.
 */

#include "harness.h"

#include "tool.h"
#include "device.h"
#include "driver.h"
#include "kobject.h"
#include "trace.h"
#include "json.h"
#include "pci.h"
#include "dvm.h"
#include "chat.h"
#include "model.h"

#include <stdlib.h>
#include <string.h>

#include "../../vm/transcripts/dvm_bringup.h"

/* ====================================================================== */
/* the synthetic PCI bus                                                  */
/* ====================================================================== */

typedef struct {
    uint8_t  bus, dev, fn;
    uint32_t w[64];                 /* 256 bytes of config space */
} fake_fn_t;

#define FAKE_MAX 32
static fake_fn_t fake[FAKE_MAX];
static int       nfake;

/* CONFIG-SPACE WRITES. There used to be exactly one assertion here — that the
 * count stays zero — because nothing in the driver-VM path was allowed to write
 * config space at all. That is no longer true: driver_run sets PCI command bits
 * so that a device can DMA, which is a deliberate escalation, so the assertion
 * has to become a description of the ONLY write that is permitted instead of a
 * blanket ban. The bus below therefore behaves like real config space (a write
 * sticks and reads back) and, on every write, checks all of:
 *
 *   - the offset is 0x04, the command/status dword, and nothing else. A write to
 *     a BAR would move a live device's window; a write to 0x0c/0x3c would change
 *     cache line size or interrupt routing;
 *   - the upper 16 bits are zero, so no write-1-to-clear STATUS bit is destroyed;
 *   - no bit outside {0,1,2} is ever changed in either direction;
 *   - the function written to is the run's target and no other.
 *
 * cfg_bad counts violations and must be 0 for the whole suite. That is a
 * mechanical proof that "the model still cannot touch config space, and the
 * kernel touches exactly three bits of one register". */
static long      cfg_writes;         /* how many happened (now allowed to be >0) */
static long      cfg_bad;            /* MUST stay 0 for the whole suite          */
static unsigned  cfg_expect_bdf = 0xFFFFu;  /* whose command register may move   */
static uint32_t  cfg_last_val;
static unsigned  cfg_last_off = 0xFFu;

#define CFG_CMD_TOUCHABLE 0x0007u    /* io | mem | bus-master */

static fake_fn_t *fake_add(uint8_t bus, uint8_t dev, uint8_t fn,
                           uint16_t vendor, uint16_t device,
                           uint8_t cls, uint8_t sub) {
    CHECK(nfake < FAKE_MAX);
    if (nfake >= FAKE_MAX) return &fake[FAKE_MAX - 1];
    fake_fn_t *f = &fake[nfake++];
    memset(f, 0, sizeof *f);
    f->bus = bus; f->dev = dev; f->fn = fn;
    f->w[0x00 / 4] = ((uint32_t)device << 16) | vendor;
    f->w[0x04 / 4] = 0x00000001u;                  /* I/O space enabled */
    f->w[0x08 / 4] = ((uint32_t)cls << 24) | ((uint32_t)sub << 16);
    return f;
}

/* Command registers as build_machine() left them, so fresh() can put every
 * function back. Without this the very first driver_run would leave bus mastering
 * on for the whole suite and every later assertion about "the kernel set bit 2"
 * would depend on test ORDER — which is exactly the kind of test that passes
 * while the code is broken. */
static uint16_t cmd_initial[FAKE_MAX];

static void snapshot_cmds(void) {
    for (int i = 0; i < nfake; i++) cmd_initial[i] = (uint16_t)(fake[i].w[0x04 / 4] & 0xFFFFu);
}

static void restore_cmds(void) {
    for (int i = 0; i < nfake; i++)
        fake[i].w[0x04 / 4] = (fake[i].w[0x04 / 4] & 0xFFFF0000u) | cmd_initial[i];
}

/* The command half of one function's config dword 0x04, as it stands now. */
static uint16_t fake_cmd(uint8_t bus, uint8_t dev, uint8_t fn) {
    for (int i = 0; i < nfake; i++)
        if (fake[i].bus == bus && fake[i].dev == dev && fake[i].fn == fn)
            return (uint16_t)(fake[i].w[0x04 / 4] & 0xFFFFu);
    return 0xFFFFu;
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    for (int i = 0; i < nfake; i++)
        if (fake[i].bus == bus && fake[i].dev == dev && fake[i].fn == fn)
            return fake[i].w[(off & 0xFC) >> 2];
    return 0xFFFFFFFFu;
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    cfg_writes++;
    cfg_last_off = off & 0xFCu;
    cfg_last_val = val;

    unsigned bdf = ((unsigned)bus << 8) | ((unsigned)dev << 3) | fn;
    if ((off & 0xFCu) != 0x04u) { cfg_bad++; return; }   /* only command/status */
    if (val >> 16)              { cfg_bad++; return; }   /* status must be left alone */
    if (cfg_expect_bdf != 0xFFFFu && bdf != cfg_expect_bdf) { cfg_bad++; return; }

    for (int i = 0; i < nfake; i++)
        if (fake[i].bus == bus && fake[i].dev == dev && fake[i].fn == fn) {
            uint32_t old = fake[i].w[0x04 / 4];
            if (((old ^ val) & 0xFFFFu) & ~(uint32_t)CFG_CMD_TOUCHABLE) {
                cfg_bad++;                              /* changed a forbidden bit */
                return;
            }
            /* Real hardware: the command half takes the write, the status half is
             * write-1-to-clear and a zero there preserves it. */
            fake[i].w[0x04 / 4] = (old & 0xFFFF0000u) | (val & 0xFFFFu);
            return;
        }
    cfg_bad++;                                          /* wrote to nothing at all */
}

/* ====================================================================== */
/* TLK1: the synthetic device a program is supposed to bring up           */
/* ====================================================================== */

#define TLK_BASE      0xD000u
#define TLK_ID_VALUE  0x4B4C5401u

#define TLK_R_ID      0x00
#define TLK_R_CTL     0x04
#define TLK_R_STAT    0x08
#define TLK_R_RATE    0x0C
#define TLK_R_COUNT   0x10
#define TLK_R_IRQMASK 0x14

/* TLK1's DMA engine — the part that makes the buffer worth having.
 *
 * A 64-bit-capable bus master with a split address register, which is how every
 * real one is built (e1000, AHCI, HD Audio, xHCI all take a LO/HI pair) and which
 * is also what makes this testable: on the host the arena's address does not fit
 * in 32 bits, and a program that wrote only the low half would be pointing the
 * device at nothing — exactly the bug a real 64-bit driver has to avoid.
 *
 * On TLK_R_DMA_CTL bit 0 the device FETCHES len bytes from the address it was
 * given and reports their sum; on bit 1 it WRITES len bytes there. Nothing in the
 * kernel knows these registers exist: the program in the test is what knows, which
 * is the whole point of the split between "generic OS primitive" and "device
 * knowledge". */
#define TLK_R_DMA_LO  0x18
#define TLK_R_DMA_HI  0x1C
#define TLK_R_DMA_LEN 0x20
#define TLK_R_DMA_CTL 0x24
#define TLK_R_DMA_SUM 0x28
#define TLK_R_DMA_N   0x2C

#define TLK_CTL_RESET  0x1
#define TLK_CTL_ENABLE 0x2
#define TLK_ST_READY   0x1
#define TLK_ST_RUN     0x2
#define TLK_DMA_GO     0x1             /* read from memory  */
#define TLK_DMA_PUT    0x2             /* write to memory   */
#define TLK_DMA_FILL   0xE5

#define TLK_DMA_MAXLEN 0x10000u        /* a bounded model, so a fuzzed len is cheap */

typedef struct {
    uint32_t rate, count, irqmask;
    int      ready, running, was_reset;
    uint64_t delay_total, ready_at;
    uint32_t dma_lo, dma_hi, dma_len, dma_sum, dma_n;
    uint64_t dma_addr_seen;            /* the last address it was pointed at */
} tlk_t;

static tlk_t tlk;

/* Bus-master activity, asserted on at the end of the suite. */
static long dma_fetches;      /* transfers the device really performed        */
static long dma_wild;         /* transfers it was asked for outside the arena */

/* A real device would dereference whatever address it was handed; this one is a
 * model inside a test process, so it refuses and counts instead of dying. That
 * refusal is NOT a containment claim about the kernel — real hardware would do
 * the transfer. It is here so the suite (and the fuzzers) can say how often a
 * program pointed the device somewhere it should not have.
 *
 * The permitted region is the arena PLUS its guard bands, because writing into the
 * guard is exactly the accident the guard exists to report and a test that could
 * not stage it would be testing nothing. */
static void tlk_dma_xfer(int write) {
    uint64_t addr = ((uint64_t)tlk.dma_hi << 32) | tlk.dma_lo;
    uint32_t len  = tlk.dma_len > TLK_DMA_MAXLEN ? TLK_DMA_MAXLEN : tlk.dma_len;

    tlk.dma_addr_seen = addr;
    tlk.dma_sum = 0;
    tlk.dma_n   = 0;
    if (!len) return;

    uint64_t abase = 0, asize = 0;
    dvm_dma_region(&abase, &asize);
    if (addr < abase - DVM_DMA_GUARD || addr + len > abase + asize + DVM_DMA_GUARD) {
        dma_wild++;
        return;
    }

    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;
    if (write) {
        for (uint32_t i = 0; i < len; i++) p[i] = TLK_DMA_FILL;
    } else {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < len; i++) sum += p[i];
        tlk.dma_sum = sum;
    }
    tlk.dma_n = len;
    dma_fetches++;
}

/* The sandbox the current run is supposed to be confined to. Anything outside
 * it that reaches this backend is a containment failure, counted and asserted
 * on at the end of the suite. */
static unsigned exp_lo = TLK_BASE, exp_hi = TLK_BASE + 0xFF;
static unsigned exp_bdf = (0 << 8) | (5 << 3) | 0;
static long     forbidden;
static long     tlk_accesses;
static long     tlk_delays;

static void tlk_reset_device(void) {
    memset(&tlk, 0, sizeof tlk);
}

static void tlk_tick_ready(void) {
    if (tlk.was_reset && !tlk.ready && tlk.delay_total >= tlk.ready_at) tlk.ready = 1;
}

static void tlk_port_write(void *ctx, uint16_t port, uint8_t width, uint32_t val) {
    (void)ctx;
    tlk_accesses++;
    if (port < exp_lo || port > exp_hi) { forbidden++; return; }
    unsigned off = port - TLK_BASE;
    switch (off) {
        case TLK_R_CTL:
            if (val & TLK_CTL_RESET) {
                uint64_t d = tlk.delay_total;
                memset(&tlk, 0, sizeof tlk);
                tlk.delay_total = d;
                tlk.was_reset   = 1;
                tlk.ready_at    = d + 2000;      /* 2 ms to come back */
            }
            /* The channel only starts if the device came back ready. */
            if ((val & TLK_CTL_ENABLE) && tlk.ready) tlk.running = 1;
            if (!(val & TLK_CTL_ENABLE)) tlk.running = 0;
            break;
        case TLK_R_RATE:
            /* The bug the recorded session trips over: the divisor latches
             * only while the channel is stopped. A running device swallows the
             * write silently, exactly like real hardware does. */
            if (!tlk.running) tlk.rate = val & (width == 1 ? 0xFFu : width == 2 ? 0xFFFFu : 0xFFFFFFFFu);
            break;
        case TLK_R_IRQMASK: tlk.irqmask = val; break;
        case TLK_R_DMA_LO:  tlk.dma_lo  = val; break;
        case TLK_R_DMA_HI:  tlk.dma_hi  = val; break;
        case TLK_R_DMA_LEN: tlk.dma_len = val; break;
        case TLK_R_DMA_CTL:
            if (val & TLK_DMA_PUT)     tlk_dma_xfer(1);
            else if (val & TLK_DMA_GO) tlk_dma_xfer(0);
            break;
        default: break;                          /* read-only or absent */
    }
}

static uint32_t tlk_port_read(void *ctx, uint16_t port, uint8_t width) {
    (void)ctx;
    tlk_accesses++;
    if (port < exp_lo || port > exp_hi) { forbidden++; return 0xFFFFFFFFu; }
    tlk_tick_ready();
    unsigned off = port - TLK_BASE;
    uint32_t v;
    switch (off) {
        case TLK_R_ID:    v = TLK_ID_VALUE; break;
        case TLK_R_CTL:   v = (uint32_t)((tlk.running ? TLK_CTL_ENABLE : 0)); break;
        case TLK_R_STAT:  v = (uint32_t)((tlk.ready ? TLK_ST_READY : 0) |
                                         (tlk.running ? TLK_ST_RUN : 0)); break;
        case TLK_R_RATE:  v = tlk.rate; break;
        case TLK_R_COUNT: v = tlk.count; break;
        case TLK_R_IRQMASK: v = tlk.irqmask; break;
        case TLK_R_DMA_LO:  v = tlk.dma_lo;  break;
        case TLK_R_DMA_HI:  v = tlk.dma_hi;  break;
        case TLK_R_DMA_LEN: v = tlk.dma_len; break;
        case TLK_R_DMA_SUM: v = tlk.dma_sum; break;
        case TLK_R_DMA_N:   v = tlk.dma_n;   break;
        default: v = 0xFFFFFFFFu; break;         /* nothing decodes there */
    }
    if (width == 1) v &= 0xFFu;
    else if (width == 2) v &= 0xFFFFu;
    return v;
}

/* No MMIO window is granted unless a test points exp_mmio_* at one, so any
 * other MMIO access is a containment failure. The modelled window is a plain
 * 256-byte scratch block: enough to prove ld/st reach it. */
static unsigned char mspace[256];
static uint64_t      exp_mmio_lo, exp_mmio_hi;

static int mmio_in_window(uint64_t addr, uint8_t width) {
    return exp_mmio_hi && addr >= exp_mmio_lo && addr + width - 1 <= exp_mmio_hi;
}

static void tlk_mmio_write(void *ctx, uint64_t addr, uint8_t width, uint32_t val) {
    (void)ctx;
    tlk_accesses++;
    if (!mmio_in_window(addr, width)) { forbidden++; return; }
    for (uint8_t i = 0; i < width; i++)
        mspace[(addr - exp_mmio_lo + i) % sizeof mspace] = (unsigned char)(val >> (8 * i));
}
static uint32_t tlk_mmio_read(void *ctx, uint64_t addr, uint8_t width) {
    (void)ctx;
    tlk_accesses++;
    if (!mmio_in_window(addr, width)) { forbidden++; return 0xFFFFFFFFu; }
    uint32_t v = 0;
    for (uint8_t i = 0; i < width; i++)
        v |= (uint32_t)mspace[(addr - exp_mmio_lo + i) % sizeof mspace] << (8 * i);
    return v;
}

static uint32_t tlk_pci_read32(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn,
                               uint8_t off) {
    (void)ctx;
    tlk_accesses++;
    unsigned bdf = ((unsigned)bus << 8) | ((unsigned)dev << 3) | fn;
    if (bdf != exp_bdf) { forbidden++; return 0xFFFFFFFFu; }
    return pci_cfg_read32(bus, dev, fn, off);
}

static void tlk_delay(void *ctx, uint32_t us) {
    (void)ctx;
    tlk_delays++;
    tlk.delay_total += us;
    if (tlk.running) tlk.count += us / 100;
}

static const dvm_io_t tlk_io = {
    NULL,
    tlk_port_write, tlk_port_read,
    tlk_mmio_write, tlk_mmio_read,
    tlk_pci_read32, tlk_delay,
    NULL,                       /* no syscall backend: see dvm_tools_set_sys */
};

/* ====================================================================== */
/* the module under test                                                  */
/* ====================================================================== */

/* REGISTER_TOOL collects pointers in a linker section; tooltable.h supplies the
 * Mach-O spelling and its boundary symbols, so core/tool.c walks the same table
 * the kernel's linker script builds. */
#include "tooltable.h"

#ifdef __APPLE__
/* Apple's <string.h> defines memcpy/memset/... as fortifying macros, which
 * collide with kernel.h's plain prototypes when both land in one TU. */
#  undef memcpy
#  undef memset
#  undef memmove
#  undef memcmp
#  undef strlen
#endif

#include "../../tools/dvm_tools.c"

/* Exported by the file above for the kernel and for this suite. */
void dvm_tools_set_io(const dvm_io_t *io);
void dvm_tools_reset_attempts(void);

/* ====================================================================== */
/* fixtures                                                               */
/* ====================================================================== */

static driver_t fake_driver = { "e1000fake", 0, DEV_CLASS_NETWORK, 0, 0, 0, 0, 0 };

static void add_node(const char *name, device_class_t cls, driver_t *drv,
                     device_res_type_t rt, uint64_t base) {
    device_t *d = device_create(name, cls);
    if (rt != RES_NONE) device_add_resource(d, rt, base, 0);
    device_register(d);
    if (drv) {
        device_bind_driver(d, drv);
        device_set_state(d, DEV_STATE_ACTIVE);
    }
}

/* A bus with one good target and one of every awkward case. */
static void build_machine(void) {
    fake_fn_t *f;

    fake_add(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00);            /* host bridge, no BAR */

    f = fake_add(0, 3, 0, 0x8086, 0x100E, 0x02, 0x00);        /* claimed NIC */
    f->w[0x10 / 4] = 0xFE900000u;

    /* Already a bus master before anyone asked. Targetable (a driver that cannot
     * DMA cannot play a sound), but the kernel must notice that it did not set
     * that bit and so must not clear it either. */
    f = fake_add(0, 4, 0, 0x8086, 0x2415, 0x04, 0x01);
    f->w[0x04 / 4] = 0x0007u;                                 /* io+mem+busmaster */
    f->w[0x10 / 4] = 0xC000u | 1u;

    f = fake_add(0, 5, 0, 0x1234, 0x5678, 0x08, 0x80);        /* THE TARGET */
    f->w[0x10 / 4] = TLK_BASE | 1u;

    f = fake_add(0, 6, 0, 0x1234, 0x0006, 0x08, 0x80);        /* BAR over COM1 */
    f->w[0x10 / 4] = 0x03F8u | 1u;

    f = fake_add(0, 7, 0, 0x1234, 0x0007, 0x08, 0x80);        /* BAR in the APIC block */
    f->w[0x10 / 4] = 0xFEC01000u;

    f = fake_add(0, 8, 0, 0x1234, 0x0008, 0x08, 0x80);        /* the target's neighbour */
    f->w[0x10 / 4] = 0xD100u | 1u;

    f = fake_add(0, 9, 0, 0x1234, 0x0009, 0x08, 0x80);        /* clamped to 64 bytes */
    f->w[0x10 / 4] = 0xE000u | 1u;

    f = fake_add(0, 10, 0, 0x1234, 0x000A, 0x08, 0x80);       /* ...by this one */
    f->w[0x10 / 4] = 0xE040u | 1u;

    f = fake_add(0, 11, 0, 0x1234, 0x000B, 0x08, 0x80);       /* an MMIO device */
    f->w[0x10 / 4] = 0xFE9D0000u;

    f = fake_add(0, 12, 0, 0x1234, 0x000C, 0x08, 0x80);       /* MMIO, targetable */
    f->w[0x10 / 4] = 0xFEB00000u;

    f = fake_add(0, 13, 0, 0x1234, 0x000D, 0x08, 0x80);       /* nonsense I/O BAR */
    f->w[0x10 / 4] = 0x1F000000u | 1u;

    add_node("pci00:00.0", DEV_CLASS_BUS,     NULL,         RES_NONE,   0);
    add_node("pci00:03.0", DEV_CLASS_NETWORK, &fake_driver, RES_MMIO,   0xFE900000);
    add_node("pci00:04.0", DEV_CLASS_AUDIO,   NULL,         RES_IOPORT, 0xC000);
    add_node("pci00:05.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, TLK_BASE);
    add_node("pci00:06.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0x03F8);
    add_node("pci00:07.0", DEV_CLASS_SYSTEM,  NULL,         RES_MMIO,   0xFEC01000);
    add_node("pci00:08.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0xD100);
    add_node("pci00:09.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0xE000);
    add_node("pci00:0a.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0xE040);
    add_node("pci00:0b.0", DEV_CLASS_SYSTEM,  NULL,         RES_MMIO,   0xFE9D0000);
    /* The awkward one: the PCI node for 00:0b.0 is driverless, but a DIFFERENT
     * registered device owns the same window and has a driver — exactly how
     * pci00:03.0/eth0 look on the real machine. */
    add_node("snd0",       DEV_CLASS_AUDIO,   &fake_driver, RES_MMIO,   0xFE9D0000);
    add_node("pci00:0c.0", DEV_CLASS_SYSTEM,  NULL,         RES_MMIO,   0xFEB00000);
    add_node("pci00:0d.0", DEV_CLASS_SYSTEM,  NULL,         RES_IOPORT, 0x1F000000);
    add_node("pci00:1f.0", DEV_CLASS_SYSTEM,  NULL,         RES_NONE,   0);  /* absent */
    add_node("serial0",    DEV_CLASS_SERIAL,  NULL,         RES_IOPORT, 0x3F8);
}

/* The machine this kernel actually boots, on bus 1 so it coexists with the
 * fixture above. Every address here was read out of QEMU's own monitor
 * (`info pci`, after the BIOS programmed the BARs) on the same command line
 * tests/qemu uses, so the derivation below is checked against the real thing
 * rather than against something invented to make it look good:
 *
 *   00:01.1 IDE   BAR4 I/O  0xc540 [0xc54f]        (16 bytes)
 *   00:02.0 VGA   BAR0 mem  0xfd000000 [0xfdffffff] (16 MiB, prefetchable)
 *                 BAR2 mem  0xfebf0000 [0xfebf0fff] (4 KiB)
 *   00:03.0 e1000 BAR0 mem  0xfebc0000 [0xfebdffff] (128 KiB)
 *                 BAR1 I/O  0xc500 [0xc53f]         (64 bytes)
 *   00:04.0 AC97  BAR0 I/O  0xc000 [0xc3ff]         (1 KiB)
 *                 BAR1 I/O  0xc400 [0xc4ff]         (256 bytes)
 */
static void build_qemu_replica(void) {
    fake_fn_t *f;

    fake_add(1, 0, 0, 0x8086, 0x1237, 0x06, 0x00);
    fake_add(1, 1, 0, 0x8086, 0x7000, 0x06, 0x01);
    f = fake_add(1, 1, 1, 0x8086, 0x7010, 0x01, 0x01);
    f->w[0x20 / 4] = 0xC540u | 1u;                        /* BAR4 */
    fake_add(1, 1, 3, 0x8086, 0x7113, 0x06, 0x80);
    f = fake_add(1, 2, 0, 0x1234, 0x1111, 0x03, 0x00);
    f->w[0x10 / 4] = 0xFD000000u | 0x8u;                  /* prefetchable */
    f->w[0x18 / 4] = 0xFEBF0000u;
    f = fake_add(1, 3, 0, 0x8086, 0x100E, 0x02, 0x00);
    f->w[0x10 / 4] = 0xFEBC0000u;
    f->w[0x14 / 4] = 0xC500u | 1u;
    f = fake_add(1, 4, 0, 0x8086, 0x2415, 0x04, 0x01);
    f->w[0x10 / 4] = 0xC000u | 1u;
    f->w[0x14 / 4] = 0xC400u | 1u;

    add_node("pci01:00.0", DEV_CLASS_BUS,     NULL, RES_NONE,   0);
    add_node("pci01:01.1", DEV_CLASS_STORAGE, NULL, RES_IOPORT, 0xC540);
    add_node("pci01:02.0", DEV_CLASS_DISPLAY, NULL, RES_MMIO,   0xFD000000);
    add_node("pci01:03.0", DEV_CLASS_NETWORK, NULL, RES_MMIO,   0xFEBC0000);
    add_node("pci01:04.0", DEV_CLASS_AUDIO,   NULL, RES_IOPORT, 0xC000);
}

/* ====================================================================== */
/* calling a tool                                                         */
/* ====================================================================== */

static char          rbuf[TOOL_RESULT_MAX];
static tool_result_t rres;

/* `cap` mirrors the real budget a tool gets from chat.c (CHAT_TOOL_RESULT_CAP)
 * when it matters, and the full TOOL_RESULT_MAX when a test wants to read the
 * whole answer.
 *
 * tool_dispatch() returns TOOL_OK whenever the HANDLER RAN, even when the
 * handler refused the request (that is the contract in tool.h — the model still
 * gets an answer). These helpers fold `is_error` into the return value, so a
 * test can say "this call must fail" in one place. */
static int call_cap(const char *name, const char *input, size_t cap) {
    if (cap > sizeof rbuf) cap = sizeof rbuf;
    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = input;
    c.input_len = strlen(input);
    tool_result_init(&rres, rbuf, cap);
    kcap_reset();
    int rc = tool_dispatch(&c, &rres);
    if (rc != TOOL_OK) return rc;
    return rres.is_error ? TOOL_EINVAL : TOOL_OK;
}

static int call(const char *name, const char *input) {
    return call_cap(name, input, sizeof rbuf);
}

/* Raw span form: the input is NOT NUL-terminated and may contain anything. */
static int call_raw(const char *name, const char *input, size_t len) {
    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = input;
    c.input_len = len;
    tool_result_init(&rres, rbuf, sizeof rbuf);
    kcap_reset();
    int rc = tool_dispatch(&c, &rres);
    if (rc != TOOL_OK) return rc;
    return rres.is_error ? TOOL_EINVAL : TOOL_OK;
}

/* Build {"target":"pci00:05.0","source":"..."} from an UNESCAPED program. */
static char progbuf[24000];
static const char *run_input(const char *src) {
    static char in[24000];
    size_t n = json_escape(progbuf, sizeof progbuf, src);   /* body, no quotes */
    CHECK(n < sizeof progbuf);
    snprintf(in, sizeof in, "{\"target\":\"pci00:05.0\",\"source\":\"%s\"}", progbuf);
    return in;
}

static const char *asm_input(const char *src) {
    static char in[24000];
    size_t n = json_escape(progbuf, sizeof progbuf, src);
    CHECK(n < sizeof progbuf);
    snprintf(in, sizeof in, "{\"source\":\"%s\"}", progbuf);
    return in;
}

static long acc_base;

static void fresh(void) {
    tlk_reset_device();
    dvm_tools_reset_attempts();
    dvm_tools_set_io(&tlk_io);
    exp_lo      = TLK_BASE;
    exp_hi      = TLK_BASE + 0xFF;
    exp_bdf     = (0 << 8) | (5 << 3) | 0;
    exp_mmio_lo = exp_mmio_hi = 0;
    cfg_expect_bdf = 0xFFFFu;
    restore_cmds();
    memset(mspace, 0, sizeof mspace);
    acc_base = tlk_accesses;      /* tlk_accesses is a whole-suite total */
}

/* Device accesses since the last fresh(). */
static long acc_delta(void) { return tlk_accesses - acc_base; }

/* ====================================================================== */
/* programs used by the direct-dispatch tests                             */
/* ====================================================================== */

/* The recorded session's programs, un-escaped, so the same bytes drive both
 * the direct tests and the replay. Decoded from the transcript at startup so
 * there is exactly one copy of each program in the tree. */
static char prog_v1[8192];
static char prog_v2[8192];
static char prog_bad[512];

static void decode_transcript_programs(void) {
    /* The transcript holds JSON; pull the program back out of it the same way
     * the tool does, which also proves the transcript is well-formed. */
    static const char *bodies[3] = { TR_ROUND2, TR_ROUND3, TR_STUCK1 };
    char *dst[3] = { prog_v1, prog_v2, prog_bad };
    size_t cap[3] = { sizeof prog_v1, sizeof prog_v2, sizeof prog_bad };

    for (int i = 0; i < 3; i++) {
        json_value_t root, content, blk, type, input, src;
        CHECK_EQ(json_parse(bodies[i], strlen(bodies[i]), &root), JSON_OK);
        CHECK_EQ(json_msg_content(&root, &content), JSON_OK);
        int found = 0;
        for (size_t b = 0; b < json_count(&content); b++) {
            if (json_at(&content, b, &blk) != JSON_OK) continue;
            if (json_get(&blk, "type", &type) != JSON_OK) continue;
            if (!json_str_eq(&type, "tool_use")) continue;
            CHECK_EQ(json_get(&blk, "input", &input), JSON_OK);
            CHECK_EQ(json_get(&input, "source", &src), JSON_OK);
            CHECK_EQ(json_str(&src, dst[i], cap[i], NULL), JSON_OK);
            found = 1;
        }
        CHECK(found);
    }
    CHECK(strstr(prog_v1, "out32 r5, CTL_ENABLE") != NULL);
    CHECK(strstr(prog_v2, "out16 r3, RATE_DIV") != NULL);
    /* The whole point of the pair: the fix is the ORDER of those two writes. */
    CHECK(strstr(prog_v1, "out32 r5, CTL_ENABLE") < strstr(prog_v1, "out16 r3, RATE_DIV"));
    CHECK(strstr(prog_v2, "out16 r3, RATE_DIV") < strstr(prog_v2, "out32 r5, CTL_ENABLE"));
}

/* ====================================================================== */
/* 1. driver_targets                                                      */
/* ====================================================================== */

static void test_targets(void) {
    fresh();

    CHECK_EQ(call("driver_targets", "{}"), TOOL_OK);
    CHECK_EQ(rres.is_error, 0);
    CHECK_CONTAINS(rbuf, "target=pci00:05.0 00:05.0 1234:5678");
    CHECK_CONTAINS(rbuf, "usable: yes");
    CHECK_CONTAINS(rbuf, "ports 0xd000-0xd0ff; mmio none; pci 00:05.0");
    CHECK_CONTAINS(rbuf, "r0=0xd000");
    CHECK_CONTAINS(rbuf, "r6=bdf 0x28");
    /* A claimed device is not offered at all without include_unusable. */
    CHECK(strstr(rbuf, "pci00:03.0") == NULL);
    CHECK_CONTAINS(rbuf, "PCI function(s) known");
    /* The kernel says how many, in a line the model cannot forge. */
    CHECK_CONTAINS(kcap_text(), "[driver_targets known=");

    /* The neighbour clamp: 00:09.0 is 64 bytes because 00:0a.0 starts there. */
    CHECK_CONTAINS(rbuf, "ports 0xe000-0xe03f");
    /* ...while an isolated I/O BAR gets the PCI maximum of 256 bytes. */
    CHECK_CONTAINS(rbuf, "ports 0xd100-0xd1ff");
    /* A memory BAR is granted by its alignment, capped at 1 MiB, and stopped
     * at the next BAR on the bus (0xfebc0000 here). */
    CHECK_CONTAINS(rbuf, "mmio 0xfeb00000+0xc0000");
}

static void test_targets_refusals(void) {
    fresh();

    CHECK_EQ(call("driver_targets", "{\"include_unusable\":true,\"limit\":64}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "driver \"e1000fake\" is already bound to pci00:03.0");
    CHECK_CONTAINS(rbuf, "pci00:04.0");
    CHECK_CONTAINS(rbuf, "no programmed BAR");
    CHECK_CONTAINS(rbuf, "interrupt-controller block");
    CHECK_CONTAINS(rbuf, "COM1");          /* the VM's own words, via dvm_policy_check */

    /* Bad arguments are refused, not guessed at. */
    CHECK(call("driver_targets", "{\"limit\":0}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "error: \"limit\" must be an integer 1-64");
    CHECK(call("driver_targets", "{\"limit\":\"lots\"}") != TOOL_OK);
    CHECK(call("driver_targets", "{\"include_unusable\":7}") != TOOL_OK);
    CHECK(call("driver_targets", "[]") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "input must be a JSON object");
}

/* The windows the model would really be handed on the machine this kernel
 * boots. Exact numbers, including where the inference is imprecise, because
 * "roughly the right window" is not a security property. */
static void test_qemu_bus_layout(void) {
    fresh();
    CHECK_EQ(call("driver_targets", "{\"limit\":64}"), TOOL_OK);

    /* IDE: BAR4 is 16 bytes; alignment only proves 64, and nothing else on the
     * bus sits above 0xc540, so 64 is what is granted. Over by 48 bytes of
     * unallocated I/O space — the price of not writing config space to size a
     * BAR. */
    CHECK_CONTAINS(rbuf, "target=pci01:01.1");
    CHECK_CONTAINS(rbuf, "ports 0xc540-0xc57f; mmio none; pci 01:01.1");

    /* e1000: BAR1 is exactly 64 bytes and the clamp finds it exactly, because
     * the IDE BAR starts 64 bytes later. BAR0 is 128 KiB; alignment says 256
     * KiB and the VGA BAR2 above it cuts that to 192 KiB. */
    CHECK_CONTAINS(rbuf, "ports 0xc500-0xc53f; mmio 0xfebc0000+0x30000");

    /* AC97: BAR0 is 1 KiB but I/O windows are capped at the PCI maximum of
     * 256 bytes, so the mixer block is reachable and the rest is not. BAR1 is
     * 256 bytes and comes out exact. */
    CHECK_CONTAINS(rbuf, "ports 0xc000-0xc0ff,0xc400-0xc4ff");

    /* VGA: NOT LISTED AS USABLE, AND NO WINDOW IS EVER PRINTED FOR IT.
     *
     * This is the one that matters most on this machine. Its BAR0 is the stdvga
     * VRAM linear aperture and its BAR2+0x400 is the legacy CRTC/sequencer
     * block, so the window this arithmetic WOULD have produced
     * ("mmio 0xfd000000+0x100000,0xfebf0000+0x10000") is read-write access to
     * the operator's console at an address vm/dvm.c's deny list cannot recognise
     * as the console — a second door to the same character memory it guards at
     * 0xb8000. A program with it can repaint the transcript and forge a
     * [bracketed] kernel line. So the tool refuses the whole device class, and
     * this asserts that the numbers never appear in anything the model reads. */
    CHECK(strstr(rbuf, "0xfd000000") == NULL);
    CHECK(strstr(rbuf, "0xfebf0000") == NULL);

    /* And the BAR-less functions QEMU's chipset is mostly made of. */
    CHECK_EQ(call("driver_run", "{\"target\":\"pci01:00.0\",\"source\":\"halt\"}"),
             TOOL_EINVAL);
    CHECK_CONTAINS(rbuf, "no programmed BAR");
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* 2. driver_assemble                                                     */
/* ====================================================================== */

static void test_assemble(void) {
    fresh();

    CHECK_EQ(call("driver_assemble",
                  "{\"source\":\"mov r1, 5\\nadd r1, 2\\nhalt\\n\",\"listing\":true}"),
             TOOL_OK);
    CHECK_EQ(rres.is_error, 0);
    CHECK_CONTAINS(rbuf, "ok: 3 instructions");
    CHECK_CONTAINS(rbuf, "add r1, r1, 2");        /* the shorthand, expanded */
    CHECK_CONTAINS(rbuf, "this only parsed the program");

    /* An error names the line and quotes it back. */
    CHECK(call("driver_assemble",
               "{\"source\":\"mov r1, 5\\noutq 0x10, r1\\nhalt\\n\"}") != TOOL_OK);
    CHECK_EQ(rres.is_error, 1);
    CHECK_CONTAINS(rbuf, "assembly failed at line 2");
    CHECK_CONTAINS(rbuf, "outq");
    CHECK_CONTAINS(rbuf, "line 2: outq 0x10, r1");

    /* An undefined label is caught before anything can run. */
    CHECK(call("driver_assemble", "{\"source\":\"jmp nowhere\\nhalt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "assembly failed at line 1");

    /* Assembling never touches the device. */
    CHECK_EQ(acc_delta(), 0);

    /* Argument validation. */
    CHECK(call("driver_assemble", "{}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"source\" is required");
    CHECK(call("driver_assemble", "{\"source\":42}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"source\" must be a string");
    CHECK(call("driver_assemble", "{\"source\":\"\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"source\" is empty");
    CHECK(call("driver_assemble", "{\"source\":\"halt\",\"listing\":\"yes\"}") != TOOL_OK);
}

static void test_assemble_oversized(void) {
    fresh();
    /* A program larger than the whole source budget is a clean refusal, not a
     * buffer problem. */
    static char big[DVM_SRC_MAX * 2];
    static char in[DVM_SRC_MAX * 2 + 64];
    size_t n = 0;
    while (n < sizeof big - 8) { memcpy(big + n, "nop\\n", 5); n += 5; }
    big[n] = '\0';
    snprintf(in, sizeof in, "{\"source\":\"%s\"}", big);

    CHECK(call("driver_assemble", in) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "longer than");
    CHECK_EQ(acc_delta(), 0);
}

/* ====================================================================== */
/* 3. driver_run: the good program                                        */
/* ====================================================================== */

static void test_run_success(void) {
    fresh();

    CHECK_EQ(call_cap("driver_run", run_input(prog_v2), CHAT_TOOL_RESULT_CAP), TOOL_OK);
    CHECK_EQ(rres.is_error, 0);

    CHECK_CONTAINS(rbuf, "target=pci00:05.0 00:05.0 1234:5678 attempt 1 of 5");
    CHECK_CONTAINS(rbuf, "sandbox: ports 0xd000-0xd0ff; mmio none; pci 00:05.0");
    CHECK_CONTAINS(rbuf, "status=OK");

    /* The device really is up: ready, running, rate latched, counter moving. */
    CHECK_EQ(tlk.ready, 1);
    CHECK_EQ(tlk.running, 1);
    CHECK_EQ(tlk.rate, 0x100u);
    CHECK(tlk.count > 0);

    /* ...and the program's own output channel says the same thing. */
    CHECK_CONTAINS(rbuf, "r8=0x4b4c5401");    /* the identity it read back */
    CHECK_CONTAINS(rbuf, "r11=0x100");        /* the rate, read back        */
    CHECK_CONTAINS(rbuf, "r12=0x3");          /* status: ready | running    */
    CHECK_CONTAINS(rbuf, "r0=0xd000");        /* the BAR the kernel handed in */

    /* The kernel's ground truth, not the model's. */
    CHECK_CONTAINS(kcap_text(), "[dvm.grant ports 0xd000-0xd0ff -> ok]");
    CHECK_CONTAINS(kcap_text(), "[dvm.grant pci 00:05.0 -> ok]");
    CHECK_CONTAINS(kcap_text(), "[dvm.halt");
    CHECK_CONTAINS(kcap_text(), "[driver_run pci00:05.0 attempt=1");
    CHECK_CONTAINS(kcap_text(), "-> ok]");

    /* The whole answer fitted the budget chat.c really gives a tool. */
    CHECK_EQ(rres.truncated, 0);
    CHECK(rres.len < CHAT_TOOL_RESULT_CAP);

    /* A clean run clears the attempt count. */
    CHECK_EQ((int)attempts_used("pci00:05.0"), 0);
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* 4. driver_run: the failing program, and what comes back                */
/* ====================================================================== */

static void test_run_failure_feedback(void) {
    fresh();

    CHECK(call_cap("driver_run", run_input(prog_v1), CHAT_TOOL_RESULT_CAP) != TOOL_OK);
    CHECK_EQ(rres.is_error, 1);

    /* Everything the model needs to fix it, in one answer. */
    CHECK_CONTAINS(rbuf, "status=ABORT at line");
    CHECK_CONTAINS(rbuf, "rate did not stick");
    CHECK_CONTAINS(rbuf, "instruction: abort");
    CHECK_CONTAINS(rbuf, "abort \"rate did not stick\"");   /* its own source line */
    CHECK_CONTAINS(rbuf, "4 attempt(s) left on this target");

    /* The access log is the evidence: the rate write, then the readback of 0. */
    CHECK_CONTAINS(rbuf, "out16 0xd00c <= 0x100");
    CHECK_CONTAINS(rbuf, "in16 0xd00c => 0x0");

    /* The device was left enabled but unconfigured — which is the truth. */
    CHECK_EQ(tlk.running, 1);
    CHECK_EQ(tlk.rate, 0u);

    CHECK_EQ((int)attempts_used("pci00:05.0"), 1);
    CHECK_EQ(forbidden, 0);
    CHECK_CONTAINS(kcap_text(), "[driver_run pci00:05.0 attempt=1 ABORT line=");
    CHECK_CONTAINS(kcap_text(), "-> EINVAL]");

    /* Exactly what the model is handed to revise from — the same bytes chat.c
     * puts in the tool_result. Printed on request, because a reviewer should be
     * able to read it rather than take this suite's word for it. */
    if (getenv("DVM_SHOW_TRANSCRIPT"))
        printf("\n----- the tool_result the model reads after attempt 1 -----\n%s"
               "-----------------------------------------------------------\n", rbuf);
}

/* ====================================================================== */
/* 5. driver_run: the sandbox holds                                       */
/* ====================================================================== */

static void test_run_denied_port(void) {
    fresh();

    /* Inside the BAR, then one byte past the end of it. */
    CHECK(call("driver_run", run_input(
        "    in32 r1, r0\n"
        "    mov  r2, 0xd100\n"
        "    in8  r3, r2\n"
        "    halt\n")) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=PORT_DENIED");
    CHECK_CONTAINS(rbuf, "0xd100");
    CHECK_CONTAINS(rbuf, "instruction: in8 r3, r2");
    /* The legal access before it still shows, so the model can see how far it got. */
    CHECK_CONTAINS(rbuf, "in32 0xd000 => 0x4b4c5401");
    CHECK_EQ(forbidden, 0);            /* the neighbour was never touched */

    /* The compiled-in deny list, reached through the tool. */
    fresh();
    CHECK(call("driver_run", run_input("    out8 0x64, 0xfe\n    halt\n")) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=PORT_DENIED");
    CHECK_CONTAINS(rbuf, "PS/2");
    CHECK_EQ(forbidden, 0);

    /* MMIO, when no MMIO window was granted. */
    fresh();
    CHECK(call("driver_run", run_input("    ld32 r1, [0xfebd0000]\n    halt\n")) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=MMIO_DENIED");
    CHECK_EQ(forbidden, 0);

    /* Another device's config space. */
    fresh();
    CHECK(call("driver_run", run_input("    pcicfg r1, 00:03.0, 0x00\n    halt\n")) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=PCI_DENIED");
    CHECK_EQ(forbidden, 0);

    /* Its own function is allowed, and reads real config space. */
    fresh();
    CHECK_EQ(call("driver_run", run_input(
        "    pcicfg r1, r6, 0x00\n"
        "    halt\n")), TOOL_OK);
    CHECK_CONTAINS(rbuf, "r1=0x56781234");
    CHECK_CONTAINS(rbuf, "pcicfg 00:05.0 off 0x00 => 0x56781234");
    CHECK_EQ(forbidden, 0);
}

/* A memory BAR earns an MMIO window, and ld/st reach it — the other half of
 * the sandbox, which the port tests never touch. */
static void test_run_mmio_window(void) {
    fresh();
    exp_mmio_lo = 0xFEB00000ull;
    exp_mmio_hi = 0xFEB00000ull + 0xC0000ull - 1;
    exp_bdf     = (0 << 8) | (12 << 3) | 0;

    CHECK_EQ(call("driver_run",
                  "{\"target\":\"pci00:0c.0\",\"source\":\""
                  "    st32 [r0+0x10], 0xa5a5\\n"
                  "    ld32 r1, [r0+0x10]\\n"
                  "    ld8  r2, [r0+0x11]\\n"
                  "    halt\\n\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "sandbox: ports none; mmio 0xfeb00000+0xc0000");
    CHECK_CONTAINS(rbuf, "r1=0xa5a5");
    CHECK_CONTAINS(rbuf, "r2=0xa5");
    CHECK_CONTAINS(rbuf, "st32 0xfeb00010 <= 0xa5a5");
    CHECK_CONTAINS(rbuf, "ld32 0xfeb00010 => 0xa5a5");

    /* One byte past the granted window is refused, and never reaches the
     * device — even though the device would happily have answered. */
    CHECK(call("driver_run",
               "{\"target\":\"pci00:0c.0\",\"source\":\""
               "    ld32 r1, [0xfebc0000]\\n"
               "    halt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=MMIO_DENIED");
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* the two things a driver program cannot live without                    */
/* ====================================================================== */

/* 1. THE BUFFER ARRIVES WHERE THE DOCUMENTATION SAYS IT DOES.
 *
 * driver_run's description tells the model "r7 holds the PHYSICAL address of a DMA
 * scratch buffer and r8 holds that buffer's size". That sentence is the whole
 * interface, so it is checked against dvm_dma_region() rather than trusted. */
static void test_dma_buffer_arrives_in_r7_r8(void) {
    fresh();

    uint64_t base = 0, size = 0;
    dvm_dma_region(&base, &size);
    CHECK(base != 0);
    CHECK_EQ((int)size, (int)DVM_DMA_SIZE);

    char want[128];

    /* driver_targets publishes it before a program is ever written. */
    CHECK_EQ(call("driver_targets", "{}"), TOOL_OK);
    snprintf(want, sizeof want, "dma 0x%lx+0x%lx", (unsigned long)base,
             (unsigned long)size);
    CHECK_CONTAINS(rbuf, want);
    snprintf(want, sizeof want, "r7=dma 0x%lx r8=0x%lx", (unsigned long)base,
             (unsigned long)size);
    CHECK_CONTAINS(rbuf, want);

    /* ...and the program really receives those two values, in those two
     * registers. `mov` them somewhere else so they come back in the result even
     * though r7/r8 would be reported anyway. */
    CHECK_EQ(call("driver_run",
                  "{\"target\":\"pci00:05.0\",\"source\":\""
                  "    mov r10, r7\\n"
                  "    mov r11, r8\\n"
                  "    in32 r1, r0\\n"        /* touch the device: this is progress */
                  "    halt\\n\"}"), TOOL_OK);
    snprintf(want, sizeof want, "r7=0x%lx", (unsigned long)base);
    CHECK_CONTAINS(rbuf, want);
    snprintf(want, sizeof want, "r8=0x%lx", (unsigned long)size);
    CHECK_CONTAINS(rbuf, want);
    snprintf(want, sizeof want, "r10=0x%lx", (unsigned long)base);
    CHECK_CONTAINS(rbuf, want);
    snprintf(want, sizeof want, "r11=0x%lx", (unsigned long)size);
    CHECK_CONTAINS(rbuf, want);

    /* The size is the one the header argues for: a second of 48 kHz stereo
     * 16-bit PCM, with room left for a descriptor list. */
    CHECK(size >= 48000ull * 2 * 2);
    CHECK_EQ((int)(base % DVM_DMA_ALIGN), 0);
    CHECK_EQ(forbidden, 0);
}

/* 2. A PROGRAM'S STORES REALLY CHANGE THAT MEMORY.
 *
 * Written through the tool, read back through a plain pointer at the address the
 * tool published. If this passes, "the buffer is where I say it is" is a fact. */
static void test_dma_buffer_is_real_memory(void) {
    fresh();
    uint64_t base = 0, size = 0;
    dvm_dma_region(&base, &size);
    volatile uint8_t *mem = (volatile uint8_t *)(uintptr_t)base;

    for (int i = 0; i < 32; i++) mem[i] = 0;

    CHECK_EQ(call("driver_run",
                  "{\"target\":\"pci00:05.0\",\"source\":\""
                  "    st32 [r7+0],  0x04030201\\n"
                  "    st32 [r7+4],  0x08070605\\n"
                  "    st16 [r7+8],  0x0a09\\n"
                  "    st8  [r7+10], 0x0b\\n"
                  "    ld32 r10, [r7+0]\\n"
                  "    in32 r1, r0\\n"
                  "    halt\\n\"}"), TOOL_OK);

    for (int i = 0; i < 11; i++) CHECK_EQ((int)mem[i], i + 1);
    CHECK_CONTAINS(rbuf, "r10=0x4030201");

    /* Reported as buffer traffic, not as device traffic. */
    CHECK_CONTAINS(rbuf, "5 dma buffer accesses (rd 1 wr 4)");
    /* ...and it is not in the DEVICE access log, because it is not a device
     * access: nothing appeared on a bus. */
    CHECK(strstr(rbuf, "st32 0x") == NULL);

    /* The whole buffer is reachable, and one byte past it is not. */
    CHECK_EQ(call("driver_run",
                  "{\"target\":\"pci00:05.0\",\"source\":\""
                  "    sub r9, r8, 1\\n"
                  "    add r9, r7, r9\\n"
                  "    st8 [r9+0], 0xc3\\n"
                  "    in32 r1, r0\\n"
                  "    halt\\n\"}"), TOOL_OK);
    CHECK_EQ((int)mem[size - 1], 0xc3);

    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\""
               "    add r9, r7, r8\\n"
               "    st8 [r9+0], 1\\n"
               "    halt\\n\"}") != TOOL_OK);
    CHECK_EQ(forbidden, 0);
}

/* 3. THE DEVICE REALLY READS IT. The end-to-end claim, and the only one that
 * matters for audio: a program writes bytes into the buffer, hands the DEVICE the
 * physical address of those bytes, and the device fetches them.
 *
 * The program below is hand-written here (not in vm/programs/) and is the only
 * thing that knows TLK1's DMA registers exist. The kernel supplied a buffer, an
 * address, and the bus-master bit; every device-specific decision — split
 * LO/HI address, byte length, the go bit — is in the program text. */
static void test_a_device_really_dmas_out_of_the_buffer(void) {
    fresh();
    uint64_t base = 0;
    dvm_dma_region(&base, NULL);

    long fetches_before = dma_fetches;
    long wild_before    = dma_wild;

    /* 256 bytes of ramp 0..255, then point the engine at it. Sum(0..255) = 32640
     * = 0x7f80, which is the value the DEVICE computes from memory the PROGRAM
     * wrote — it cannot be produced any other way. */
    CHECK_EQ(call_cap("driver_run",
                  "{\"target\":\"pci00:05.0\",\"max_steps\":20000,\"source\":\""
                  "    .equ DMA_LO  0x18\\n"
                  "    .equ DMA_HI  0x1c\\n"
                  "    .equ DMA_LEN 0x20\\n"
                  "    .equ DMA_CTL 0x24\\n"
                  "    .equ DMA_SUM 0x28\\n"
                  "    .equ DMA_N   0x2c\\n"
                  "    mov r2, 0\\n"
                  "fill:\\n"
                  "    add r3, r7, r2\\n"
                  "    st8 [r3+0], r2\\n"
                  "    add r2, r2, 1\\n"
                  "    cmp r2, 256\\n"
                  "    blt fill\\n"
                  "    add r4, r0, DMA_LO\\n"
                  "    and r5, r7, 0xffffffff\\n"
                  "    out32 r4, r5\\n"
                  "    add r4, r0, DMA_HI\\n"
                  "    shr r5, r7, 32\\n"
                  "    out32 r4, r5\\n"
                  "    add r4, r0, DMA_LEN\\n"
                  "    out32 r4, 256\\n"
                  "    add r4, r0, DMA_CTL\\n"
                  "    out32 r4, 1\\n"
                  "    add r4, r0, DMA_SUM\\n"
                  "    in32 r10, r4\\n"
                  "    add r4, r0, DMA_N\\n"
                  "    in32 r11, r4\\n"
                  "    cmp r11, 256\\n"
                  "    bne nope\\n"
                  "    halt\\n"
                  "nope:\\n"
                  "    abort \\\"the engine did not transfer 256 bytes\\\"\\n\"}",
                  CHAT_TOOL_RESULT_CAP), TOOL_OK);

    /* The device performed exactly one transfer, from inside the arena. */
    CHECK_EQ(dma_fetches - fetches_before, 1);
    CHECK_EQ(dma_wild - wild_before, 0);
    CHECK_EQ((int)(tlk.dma_addr_seen == base), 1);
    CHECK_EQ((int)tlk.dma_n, 256);
    CHECK_EQ((int)tlk.dma_sum, 32640);

    /* ...and the program read that back out of the device, so the proof arrives
     * in the model's own answer rather than only in the test's variables. */
    CHECK_CONTAINS(rbuf, "r10=0x7f80");
    CHECK_CONTAINS(rbuf, "r11=0x100");
    CHECK_CONTAINS(rbuf, "status=OK");

    /* The address the device was handed is on the record, in the access log, as
     * the mitigation in dvm.h claims. */
    char want[64];
    CHECK_EQ(call("driver_trace", "{\"offset\":0,\"limit\":8}"), TOOL_OK);
    snprintf(want, sizeof want, "out32 0xd018 <= 0x%lx",
             (unsigned long)(base & 0xFFFFFFFFul));
    CHECK_CONTAINS(rbuf, want);
    snprintf(want, sizeof want, "out32 0xd01c <= 0x%lx",
             (unsigned long)(base >> 32));
    CHECK_CONTAINS(rbuf, want);

    /* No guard-band damage: the engine stayed inside the buffer. */
    CHECK(strstr(rbuf, "DMA WENT OUT OF BOUNDS") == NULL);
    CHECK_EQ(forbidden, 0);
}

/* 4. THE KERNEL SETS BUS MASTERING, FOR ONE DEVICE, AND TAKES IT BACK. */
static void test_bus_master_enable(void) {
    fresh();

    /* 00:05.0 starts with I/O decode only. */
    CHECK_EQ((int)fake_cmd(0, 5, 0), 0x0001);
    uint16_t nb4 = fake_cmd(0, 8, 0), nb12 = fake_cmd(0, 12, 0);

    cfg_expect_bdf = (0 << 8) | (5 << 3) | 0;      /* nobody else's register may move */
    long writes_before = cfg_writes;

    CHECK_EQ(call("driver_run",
                  "{\"target\":\"pci00:05.0\",\"source\":\""
                  "    in32 r1, r0\\n"
                  "    halt\\n\"}"), TOOL_OK);

    /* Bit 2 is set; bit 0 was already there so it is not claimed as new; bit 1 is
     * NOT set, because this device has no memory BAR and so no memory window. */
    CHECK_EQ((int)fake_cmd(0, 5, 0), 0x0005);
    CHECK_CONTAINS(rbuf, "pci cmd 0x0001 -> 0x0005: kernel set bus-master(bit2)");
    CHECK_CONTAINS(rbuf, "this device can DMA now");
    CHECK(cfg_writes > writes_before);
    CHECK_EQ(cfg_bad, 0);

    /* Only the named function changed. */
    CHECK_EQ((int)fake_cmd(0, 8, 0), (int)nb4);
    CHECK_EQ((int)fake_cmd(0, 12, 0), (int)nb12);

    /* A program that TRAPS gets the bit taken away again — a half-configured DMA
     * engine with a live master bit is the state worth undoing. */
    fresh();
    CHECK_EQ((int)fake_cmd(0, 5, 0), 0x0001);         /* fresh() restored it */
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\""
               "    in32 r1, r0\\n"
               "    abort \\\"not finished\\\"\\n\"}") != TOOL_OK);
    CHECK_EQ((int)fake_cmd(0, 5, 0), 0x0001);
    CHECK_CONTAINS(rbuf, "the run failed, so bus-master(bit2) was cleared again");

    /* An MMIO-only device gets bit 1 as well, because without memory decode its
     * BAR answers nothing and the program's own ld/st would read 0xff. */
    fresh();
    exp_mmio_lo = 0xFEB00000ull;
    exp_mmio_hi = 0xFEB00000ull + 0xC0000ull - 1;
    exp_bdf     = (0 << 8) | (12 << 3) | 0;
    cfg_expect_bdf = exp_bdf;
    CHECK_EQ(call("driver_run",
                  "{\"target\":\"pci00:0c.0\",\"source\":\""
                  "    ld32 r1, [r0+0x10]\\n"
                  "    halt\\n\"}"), TOOL_OK);
    CHECK_EQ((int)(fake_cmd(0, 12, 0) & 0x7u), 0x6u | 0x1u);   /* io was preset */
    CHECK_CONTAINS(rbuf, "mem(bit1)");
    CHECK_CONTAINS(rbuf, "bus-master(bit2)");

    /* A device already mastering is left exactly as it was found. */
    fresh();
    cfg_expect_bdf = (0 << 8) | (4 << 3) | 0;
    CHECK_EQ((int)fake_cmd(0, 4, 0), 0x0007);
    CHECK_EQ(call("driver_run", "{\"target\":\"pci00:04.0\",\"source\":\"halt\"}"),
             TOOL_OK);
    CHECK_EQ((int)fake_cmd(0, 4, 0), 0x0007);
    CHECK_CONTAINS(rbuf, "bus mastering was already enabled");
    CHECK_CONTAINS(rbuf, "kernel set none");

    /* The program still cannot touch config space itself: the opcode does not
     * exist, and every spelling of it is an assembler error. */
    fresh();
    CHECK(call("driver_assemble",
               "{\"source\":\"pciwrite r1, 00:05.0, 4, 6\\nhalt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "unknown instruction");
    CHECK(call("driver_assemble",
               "{\"source\":\"pcicfgwrite 00:05.0, 4, 6\\nhalt\\n\"}") != TOOL_OK);

    CHECK_EQ(cfg_bad, 0);
    CHECK_EQ(forbidden, 0);
}

/* 5. THE GUARD BAND REPORTS A DEVICE THAT OVERRAN THE BUFFER, in the model's own
 * answer, with enough detail to fix the descriptor that caused it.
 *
 * This is staged the way it really happens: the program gives the engine a length
 * that runs past the end of the buffer, and the DEVICE — not the program — does the
 * writing. The VM cannot refuse it, which is the point; all the kernel can do is
 * notice afterwards and say so. */
static const char *overrun_program(unsigned len, unsigned from_end) {
    static char src[1024];
    /* Point the engine `from_end` bytes before the end of the buffer and ask it to
     * write `len` bytes, so len - from_end bytes land in the guard band. */
    snprintf(src, sizeof src,
             "{\"target\":\"pci00:05.0\",\"source\":\""
             "    add r9, r7, r8\\n"
             "    sub r9, r9, %u\\n"
             "    add r4, r0, 0x18\\n"
             "    and r5, r9, 0xffffffff\\n"
             "    out32 r4, r5\\n"
             "    add r4, r0, 0x1c\\n"
             "    shr r5, r9, 32\\n"
             "    out32 r4, r5\\n"
             "    add r4, r0, 0x20\\n"
             "    out32 r4, %u\\n"
             "    add r4, r0, 0x24\\n"
             "    out32 r4, 2\\n"
             "    halt\\n\"}", from_end, len);
    return src;
}

/* 6. AND THE PART THAT IS NOT CONTAINED, asserted rather than glossed over.
 *
 * driver_run's description tells the model that only the buffer is a safe address.
 * It is not able to ENFORCE that, and a test suite that only demonstrated the
 * pleasant cases would be helping the code lie. So: a program that hands the
 * engine a completely wrong physical address is accepted, runs to halt, and the
 * kernel notices nothing — because nothing in a machine without an IOMMU can.
 * On real hardware that transfer happens. The device model here refuses to
 * dereference it and counts it instead, which is a property of the TEST, not of
 * the kernel. */
static void test_the_vm_cannot_police_dma(void) {
    fresh();
    long wild_before = dma_wild;

    /* 0x1000 is kernel memory. The VM refuses ld/st there — proved below — but it
     * has no opinion whatever about the same number being written into a device's
     * descriptor register, because that is just a dword going to an allowed port. */
    CHECK_EQ(call("driver_run",
                  "{\"target\":\"pci00:05.0\",\"source\":\""
                  "    add r4, r0, 0x18\\n"
                  "    out32 r4, 0x1000\\n"        /* address: kernel memory      */
                  "    add r4, r0, 0x1c\\n"
                  "    out32 r4, 0\\n"
                  "    add r4, r0, 0x20\\n"
                  "    out32 r4, 256\\n"
                  "    add r4, r0, 0x24\\n"
                  "    out32 r4, 2\\n"             /* ...and tell it to WRITE     */
                  "    halt\\n\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=OK");
    /* The kernel did not object, and the guard band saw nothing: the transfer was
     * nowhere near the buffer. */
    CHECK(strstr(rbuf, "OUT OF BOUNDS") == NULL);
    CHECK_EQ(dma_wild - wild_before, 1);

    /* The one thing that IS true: the address is on the record, in the access log
     * the model and the operator both get. */
    CHECK_CONTAINS(rbuf, "out32 0xd018 <= 0x1000");
    CHECK_CONTAINS(kcap_text(), "out32 port=0xd018 val=0x1000");

    /* ...and the CPU-side rule really is enforced, so the contrast is exact: the
     * same program cannot read or write 0x1000 itself. */
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\""
               "    st32 [0x1000], 1\\n    halt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "MMIO_DENIED");
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\""
               "    ld32 r1, [0x1000]\\n    halt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "MMIO_DENIED");

    fresh();
    CHECK_EQ(forbidden, 0);
}

static void test_guard_band_is_reported_to_the_model(void) {
    fresh();
    uint64_t base = 0, size = 0;
    dvm_dma_region(&base, &size);
    volatile uint8_t *mem = (volatile uint8_t *)(uintptr_t)base;
    long wild_at_entry = dma_wild;

    /* A clean transfer that stops exactly at the last byte says nothing about
     * bounds — and really does write the last byte, which is the boundary the
     * guard has to get right. */
    CHECK_EQ(call("driver_run", overrun_program(64, 64)), TOOL_OK);
    CHECK_EQ((int)mem[size - 1], TLK_DMA_FILL);
    CHECK_EQ((int)mem[size - 64], TLK_DMA_FILL);
    CHECK(strstr(rbuf, "OUT OF BOUNDS") == NULL);

    /* Now 64 bytes starting 48 from the end: 16 bytes land past it. */
    CHECK(call("driver_run", overrun_program(64, 48)) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "DMA WENT OUT OF BOUNDS");
    CHECK_CONTAINS(rbuf, "past the end");
    CHECK_CONTAINS(rbuf, "+0");           /* the very first guard byte was hit */
    CHECK_CONTAINS(rbuf, "descriptor length or count");
    /* The kernel says so in its own voice too, in a line the model cannot forge. */
    CHECK_CONTAINS(kcap_text(), "[driver_run.dma pci00:05.0");

    /* driver_trace carries the same verdict for the run it describes. */
    CHECK_EQ(call("driver_trace", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "dma went out of bounds");
    CHECK_CONTAINS(rbuf, "pci cmd 0x");

    /* Reported once: the guard is re-armed, so the next clean run is clean. */
    CHECK_EQ(call("driver_run", overrun_program(16, 16)), TOOL_OK);
    CHECK(strstr(rbuf, "OUT OF BOUNDS") == NULL);

    /* An overrun is an error result, so the attempt is spent and the model is told
     * how many are left rather than being allowed to repeat it silently. */
    CHECK(call("driver_run", overrun_program(4096, 16)) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "DMA WENT OUT OF BOUNDS");
    CHECK_EQ(call("driver_run", overrun_program(16, 16)), TOOL_OK);

    fresh();
    CHECK_EQ(forbidden, 0);
    /* Every transfer in this test stayed inside the arena or its guard band: the
     * program derived every address from r7, which is what driver_run tells a model
     * to do. dma_wild only moves when a program computes an address of its own,
     * which test_the_vm_cannot_police_dma does on purpose. */
    CHECK_EQ(dma_wild, wild_at_entry);
}

/* ====================================================================== */
/* driver_install: the step that turns a driver into an audio output      */
/* ====================================================================== */

/* A play program written against the PUBLISHED contract and nothing else.
 *
 * It knows TLK1's register map, which the kernel does not and must not: r0 is
 * where its BAR landed, and every offset from it is device knowledge that arrived
 * with the program. From the kernel it uses only what AUDIO_VM_CONTRACT promises
 * — r7 is the buffer, r9 is how many bytes of PCM are in it THIS TIME — which is
 * the whole claim being tested: that the contract is sufficient to write a driver
 * against, without the driver's author being told anything about this machine
 * beyond it.
 *
 * The length check at the end matters. Reaching `halt` having done nothing is the
 * cheapest wrong program there is, so this one reads the transfer count back out
 * of the device and aborts if it is not r9 — which is also how a real play
 * program discovers that it started the engine with a stale length. */
static const char *PLAY_PROGRAM =
    "    .equ DMA_LO  0x18\n"
    "    .equ DMA_HI  0x1c\n"
    "    .equ DMA_LEN 0x20\n"
    "    .equ DMA_CTL 0x24\n"
    "    .equ DMA_N   0x2c\n"
    "    add r4, r0, DMA_LO\n"
    "    and r5, r7, 0xffffffff\n"
    "    out32 r4, r5\n"
    "    add r4, r0, DMA_HI\n"
    "    shr r5, r7, 32\n"
    "    out32 r4, r5\n"
    "    add r4, r0, DMA_LEN\n"
    "    out32 r4, r9\n"
    "    add r4, r0, DMA_CTL\n"
    "    out32 r4, 1\n"
    "    add r4, r0, DMA_N\n"
    "    in32 r12, r4\n"
    "    cmp r12, r9\n"
    "    bne shortfall\n"
    "    halt\n"
    "shortfall:\n"
    "    abort \"the engine did not take all the samples\"\n";

/* {"target":..,"source":..,"rate":..,"channels":..} from an unescaped program. */
static const char *install_input(const char *src, unsigned rate, unsigned ch) {
    static char in[24000];
    size_t n = json_escape(progbuf, sizeof progbuf, src);
    CHECK(n < sizeof progbuf);
    snprintf(in, sizeof in,
             "{\"target\":\"pci00:05.0\",\"rate\":%u,\"channels\":%u,\"source\":\"%s\"}",
             rate, ch, progbuf);
    return in;
}

/* THE HEADLINE, ON THE HOST: a program the model wrote becomes the machine's
 * audio output, and a DEVICE then reads the kernel's samples out of memory.
 *
 * Every link in the chain is the real one — the real tool dispatch, the real
 * sandbox, the real dvm, the real audio service, the real synthesiser — with a
 * synthetic device at the far end. The proof is not "the call returned ok": it is
 * that TLK1's DMA engine dereferenced the arena and computed a checksum over
 * bytes NOBODY IN THIS TEST WROTE. Those bytes came from audio_synth via
 * audio_tone, so a matching sum can only mean the samples reached the device. */
static void test_install_lets_a_model_driver_play(void) {
    fresh();
    audio_reset();

    uint64_t base = 0;
    dvm_dma_region(&base, NULL);
    long fetches_before = dma_fetches;

    /* Nothing can play before the install: that is the condition being fixed. */
    CHECK_EQ(audio_available(), 0);
    CHECK(audio_tone(440, 50) != AUDIO_OK);

    /* What driver_assemble counts is what driver_install must report. This is
     * not decoration: the first live run of this tool printed
     * "insns=3739147998" — 0xDEDEDEDE, mm/heap.c's use-after-free poison — into
     * a kernel trace line, because the count was read out of the program image
     * at the wrong moment. A number nobody cross-checks is a number that can be
     * garbage for a long time without anyone noticing. */
    CHECK_EQ(call("driver_assemble", asm_input(PLAY_PROGRAM)), TOOL_OK);
    char want_insns[64];
    {
        const char *p = strstr(rbuf, " instructions");
        CHECK(p != NULL);
        const char *q = p;
        while (q > rbuf && q[-1] >= '0' && q[-1] <= '9') q--;
        int n = (int)strtol(q, NULL, 10);
        CHECK(n > 5 && n < 100);
        snprintf(want_insns, sizeof want_insns, "program: %d instructions", n);
    }

    CHECK_EQ(call("driver_install", install_input(PLAY_PROGRAM, 48000, 2)), TOOL_OK);
    CHECK_CONTAINS(rbuf, want_insns);
    CHECK_CONTAINS(kcap_text(), "insns=");
    CHECK(strstr(kcap_text(), "insns=3739147998") == NULL);   /* 0xDEDEDEDE */
    CHECK_CONTAINS(rbuf, "is now this machine's audio sink");
    CHECK_CONTAINS(rbuf, "48000 Hz, 2 channel(s)");
    CHECK_CONTAINS(rbuf, "NOTHING HAS BEEN PLAYED YET");
    /* It must fit the size a model is actually handed. chat.c gives a tool
     * CHAT_TOOL_RESULT_CAP (1024), not TOOL_RESULT_MAX, and this family already
     * lost a live turn to a specification that arrived cut off mid-word. The
     * last sentence is the instruction to call audio_tone, so losing the tail is
     * exactly the part that must not be lost. */
    CHECK(strlen(rbuf) < CHAT_TOOL_RESULT_CAP);
    CHECK_EQ(rbuf[strlen(rbuf) - 1], '\n');
    /* The kernel's own line, in column zero, that the model cannot forge. */
    CHECK_CONTAINS(kcap_text(), "[driver_install pci00:05.0 sink=vmaudio");

    /* Registering is not playing, and the tool says so rather than implying it. */
    CHECK_EQ(audio_available(), 1);
    CHECK_EQ(dma_fetches - fetches_before, 0);

    /* Now ask for a sound the way anything else on the machine would. */
    kcap_reset();
    CHECK_EQ(audio_tone_ex(440, 100, 8000, AUDIO_WAVE_SINE), AUDIO_OK);

    /* 100 ms of 48 kHz stereo 16-bit = 4800 frames = 19200 bytes. */
    const uint32_t want_bytes = 48000u * 100u / 1000u * 2u * 2u;
    CHECK_EQ(dma_fetches - fetches_before, 1);
    CHECK_EQ((int)(tlk.dma_addr_seen == base), 1);
    CHECK_EQ((int)tlk.dma_n, (int)want_bytes);

    /* THE PART THAT CANNOT BE FAKED. Sum the bytes the synthesiser left in the
     * arena and compare with the sum the DEVICE computed while reading them. The
     * test never writes those bytes and never tells the device an address; the
     * program did the second and audio.c did the first. */
    const volatile uint8_t *pcm = (const volatile uint8_t *)(uintptr_t)base;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < want_bytes; i++) sum += pcm[i];
    CHECK_EQ((int)tlk.dma_sum, (int)sum);
    /* ...and those samples are a real signal, not a buffer of zeroes that would
     * make any sum match. A 440 Hz sine at amplitude 8000 must swing. */
    CHECK(sum != 0);

    /* A second sound re-runs the same program with a new length, which is the
     * property that makes it a SINK and not a one-shot. */
    CHECK_EQ(audio_tone_ex(880, 50, 8000, AUDIO_WAVE_SQUARE), AUDIO_OK);
    CHECK_EQ(dma_fetches - fetches_before, 2);
    CHECK_EQ((int)tlk.dma_n, (int)(48000u * 50u / 1000u * 2u * 2u));

    /* The device tree tells the operator who is driving the card. */
    device_t *d = dev_by_name("pci00:05.0");
    CHECK(d != NULL);
    CHECK(d->driver != NULL);
    CHECK_EQ(strcmp(d->driver->name, "vmaudio"), 0);

    CHECK_EQ(forbidden, 0);
    audio_reset();
    fresh();
}

/* A play program that fails must hand its own words back to whoever asked for the
 * sound. Silence reported as success is the failure mode this whole family exists
 * to prevent. */
static void test_a_failed_play_program_says_why(void) {
    fresh();
    audio_reset();

    static const char *sulks =
        "    add r4, r0, 0x00\n"
        "    in32 r5, r4\n"
        "    abort \"the codec never came out of reset\"\n";
    CHECK_EQ(call("driver_install", install_input(sulks, 44100, 1)), TOOL_OK);
    CHECK_EQ(audio_available(), 1);

    CHECK_EQ(audio_tone(440, 20), AUDIO_EIO);
    CHECK_CONTAINS(audio_last_error(), "the codec never came out of reset");
    CHECK_CONTAINS(audio_last_error(), "ABORT");

    /* The sink STAYS installed after a failed sound: the model's next move is to
     * install a corrected program, and having to re-do the bring-up first would
     * be a worse machine. */
    CHECK_EQ(audio_available(), 1);

    /* And the cheapest wrong program of all — reach halt, touch nothing — is
     * refused as a play rather than reported as a sound. */
    CHECK_EQ(call("driver_install", install_input("    halt\n", 48000, 2)), TOOL_OK);
    CHECK_EQ(audio_tone(440, 20), AUDIO_EIO);
    CHECK_CONTAINS(audio_last_error(), "without touching the device");

    audio_reset();
    fresh();
}

/* Everything driver_install refuses, and the rule that a refusal costs the
 * operator nothing: whatever sink was working before is still working after. */
static void test_install_refusals(void) {
    fresh();
    audio_reset();

    /* Install a working sink first, so every refusal below can be checked
     * against it still being there. */
    CHECK_EQ(call("driver_install", install_input(PLAY_PROGRAM, 48000, 2)), TOOL_OK);
    CHECK_EQ(audio_tone(440, 20), AUDIO_OK);

    static const struct { const char *in; const char *want; } bad[] = {
        { "not json at all",                       "must be a JSON object" },
        { "{\"source\":\"halt\",\"rate\":48000,\"channels\":2}",
                                                   "\"target\" is required" },
        { "{\"target\":\"pci00:05.0\",\"rate\":48000,\"channels\":2}",
                                                   "\"source\" is required" },
        { "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"channels\":2}",
                                                   "\"rate\" is required" },
        { "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"rate\":48000}",
                                                   "\"channels\" is required" },
        { "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"rate\":3,\"channels\":2}",
                                                   "\"rate\" must be" },
        { "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"rate\":48000,\"channels\":9}",
                                                   "\"channels\" must be" },
        { "{\"target\":\"nosuchdevice\",\"source\":\"halt\",\"rate\":48000,\"channels\":2}",
                                                   "no device named" },
        { "{\"target\":\"pci00:05.0\",\"source\":\"frobnicate r1\",\"rate\":48000,"
          "\"channels\":2}",                       "assembly failed" },
        { "{\"target\":\"pci00:05.0\",\"source\":\"\",\"rate\":48000,\"channels\":2}",
                                                   "\"source\" is empty" },
        /* The console is not a target here either — the rule belongs to the
         * device, not to the tool that happens to be asking. */
        { "{\"target\":\"pci01:02.0\",\"source\":\"halt\",\"rate\":48000,"
          "\"channels\":2}",                       "no display device is targetable" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(call("driver_install", bad[i].in) != TOOL_OK);
        CHECK_CONTAINS(rbuf, bad[i].want);
        /* THE INVARIANT: a refusal did not cost the operator the working sink. */
        CHECK_EQ(audio_available(), 1);
        CHECK_EQ(audio_tone(440, 10), AUDIO_OK);
    }

    /* A refused install must not have touched the device on the way out. An
     * assembly error in particular is decided before any hardware access. */
    fresh();
    CHECK(call("driver_install",
               "{\"target\":\"pci00:05.0\",\"source\":\"frobnicate r1\","
               "\"rate\":48000,\"channels\":2}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "the device was not touched");
    CHECK_EQ(acc_delta(), 0);

    audio_reset();
    fresh();
}

/* Re-installing a corrected program on the card that is already the sink is the
 * normal shape of the loop, and it used to be impossible: the sink binds a driver
 * to the device node, and target_blocked() refuses a device with a driver bound.
 * That would have made the FIRST install the only one. */
static void test_install_can_be_corrected(void) {
    fresh();
    audio_reset();

    /* A first program that is wrong in a way only a real sound reveals. */
    static const char *wrong =
        "    add r4, r0, 0x24\n"
        "    out32 r4, 0\n"          /* starts nothing */
        "    halt\n";
    CHECK_EQ(call("driver_install", install_input(wrong, 48000, 2)), TOOL_OK);
    CHECK_EQ(audio_tone(440, 20), AUDIO_OK);   /* it touched the device, so it
                                                * counts as a play... */
    CHECK_EQ((int)tlk.dma_n, 0);               /* ...but nothing was transferred */

    /* The correction goes in without releasing anything by hand. */
    CHECK_EQ(call("driver_install", install_input(PLAY_PROGRAM, 48000, 2)), TOOL_OK);
    CHECK_CONTAINS(rbuf, "replaced the previous sink");
    CHECK_EQ(audio_tone(440, 20), AUDIO_OK);
    CHECK(tlk.dma_n > 0);

    /* Only ONE device is bound, and it is bound once: a replace must not leave
     * the old binding behind. */
    device_t *d = dev_by_name("pci00:05.0");
    CHECK(d != NULL && d->driver != NULL);
    CHECK_EQ(strcmp(d->driver->name, "vmaudio"), 0);

    audio_reset();
    /* Released cleanly, the card is targetable again — a driver program can be
     * run against it as if the sink had never existed. */
    fresh();
    CHECK_EQ(call("driver_run", run_input("    halt\n")), TOOL_OK);
    audio_reset();
    fresh();
}

/* Bus mastering is set by C, for the one named function, and — unlike driver_run
 * — is LEFT set, because the sink DMAs on every note from here on. */
static void test_install_enables_bus_mastering_permanently(void) {
    fresh();
    audio_reset();

    CHECK_EQ((int)fake_cmd(0, 5, 0), 0x0001);          /* I/O decode only */
    uint16_t n8 = fake_cmd(0, 8, 0), nc = fake_cmd(0, 12, 0);

    CHECK_EQ(call("driver_install", install_input(PLAY_PROGRAM, 48000, 2)), TOOL_OK);
    CHECK_CONTAINS(rbuf, "bus-master(bit2)");
    CHECK_CONTAINS(rbuf, "LEAVES it set");

    CHECK_EQ((int)fake_cmd(0, 5, 0), 0x0005);          /* bit 2 added, bit 0 kept */
    CHECK_EQ((int)fake_cmd(0, 8, 0), (int)n8);         /* neighbours untouched   */
    CHECK_EQ((int)fake_cmd(0, 12, 0), (int)nc);

    /* Still set after a sound, and after a FAILED sound: taking it back would
     * stop a transfer that may still be in flight. */
    CHECK_EQ(audio_tone(440, 20), AUDIO_OK);
    CHECK_EQ((int)fake_cmd(0, 5, 0), 0x0005);

    /* A refused install does NOT leave the bit set behind it. */
    fresh();
    audio_reset();
    CHECK_EQ((int)fake_cmd(0, 5, 0), 0x0001);
    CHECK(call("driver_install",
               "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"rate\":48000,"
               "\"channels\":2}") == TOOL_OK);
    /* (that one is accepted — `halt` is a valid image; it fails at PLAY time,
     *  which test_a_failed_play_program_says_why covers.) */

    audio_reset();
    fresh();
}

static void test_run_bounded(void) {
    fresh();
    /* An unbounded loop stops on the cycle limit, not on the operator. */
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"loop:\\n jmp loop\\n\","
               "\"max_steps\":500}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=STEP_LIMIT");
    CHECK_CONTAINS(rbuf, "ran 500 steps");

    /* A delay longer than the budget is refused mid-run, with the budget named. */
    fresh();
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"loop:\\n delay 50000\\n jmp loop\\n\","
               "\"delay_budget_ms\":10}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=DELAY_LIMIT");

    /* And the knobs themselves are bounded. */
    fresh();
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"halt\","
               "\"delay_budget_ms\":9999}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"delay_budget_ms\" must be 0-2000");
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"halt\","
               "\"max_steps\":0}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"max_steps\" must be 1-1000000");
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\"halt\","
               "\"trace\":\"loud\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"trace\" must be");
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* 6. driver_run: target validation                                       */
/* ====================================================================== */

/* A PROGRAM WITH NO DEVICE, end to end through the tool.
 *
 * This is the mode that lifts the ceiling: no target, so no ports, no MMIO, no
 * PCI function and no DMA grant — just the scratch arena and the strings. The
 * things worth pinning here are the ones the VM's own suite cannot see: that
 * driver_run builds that sandbox at all, that it does not try to set a bus
 * master bit on a device that does not exist, and that what the program left in
 * scratch memory comes back to the model. */
static void test_run_with_no_device(void) {
    fresh();
    long writes_before = cfg_writes;

    CHECK_EQ(call("driver_run",
                  "{\"source\":\"mstr r1,0,\\\"status=\\\"\\n"
                  "mitoa r1,r1,404,10\\n"
                  "mstr r1,r1,\\\" (not found)\\\"\\n"
                  "halt\\n\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "no device (software program)");
    CHECK_CONTAINS(rbuf, "no device;");
    CHECK_CONTAINS(rbuf, "scratch memory");
    /* The program's answer, read back out of the arena. */
    CHECK_CONTAINS(rbuf, "status=404 (not found)");
    CHECK_CONTAINS(rbuf, "r1=0x16");
    /* No device was touched, and nothing was said about bus mastering. */
    CHECK_EQ(forbidden, 0);
    CHECK_EQ(cfg_writes, writes_before);
    th_checks++;
    if (strstr(rbuf, "pci cmd")) {
        th_fails++;
        printf("    FAIL a program with no device reported a config-space write\n");
    }

    /* A device opcode in that program reaches no backend rather than some
     * other device's registers. */
    CHECK(call("driver_run", "{\"source\":\"in8 r1,0xc000\\nhalt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "PORT_DENIED");
    CHECK_EQ(forbidden, 0);
}

static void test_run_target_validation(void) {
    fresh();

    /* "target" is OPTIONAL now, and omitting it is a MODE, not an error: the
     * program runs with no device sandbox at all. A bare `halt` still fails,
     * but for the reason every other do-nothing program fails — it made no
     * progress — and the message says which. */
    CHECK_EQ(call("driver_run", "{\"source\":\"halt\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "no device (software program)");
    CHECK_CONTAINS(rbuf, "touching memory or calling the kernel");
    CHECK_CONTAINS(rbuf, "no device;");

    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"source\" is required");

    CHECK(call("driver_run", "{\"target\":\"pci99:99.9\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no device named");

    /* The bare address form pci_list prints also resolves — through the
     * registry, so an address with no device node is still refused. */
    CHECK_EQ(call("driver_run", "{\"target\":\"00:05.0\",\"source\":\"halt\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "target=pci00:05.0");
    CHECK(call("driver_run", "{\"target\":\"00:1e.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no device named");

    /* A registered device that is not PCI has no BARs to derive a window from. */
    CHECK(call("driver_run", "{\"target\":\"serial0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "not a PCI device");

    /* A node whose function has since gone away. */
    CHECK(call("driver_run", "{\"target\":\"pci00:1f.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "nothing is answering");

    /* Claimed and unusable BARs are refused here too, not just listed as unusable
     * by driver_targets. */
    CHECK(call("driver_run", "{\"target\":\"pci00:03.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "already bound");
    /* A device that was ALREADY bus-mastering is targetable now — and the kernel
     * says so, and says it will not take away a bit it did not set. */
    CHECK_EQ(call("driver_run", "{\"target\":\"pci00:04.0\",\"source\":\"halt\"}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "bus mastering was already enabled");
    CHECK_EQ(fake_cmd(0, 4, 0) & 0x7u, 0x7u);      /* untouched, still 0x0007 */
    CHECK(call("driver_run", "{\"target\":\"pci00:06.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "not permitted");
    CHECK(call("driver_run", "{\"target\":\"pci00:07.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "interrupt-controller block");
    CHECK(call("driver_run", "{\"target\":\"pci00:00.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no programmed BAR");

    /* An I/O BAR whose base does not fit x86's 16-bit I/O space: refused, not
     * truncated into a window over some other device. */
    CHECK(call("driver_run", "{\"target\":\"pci00:0d.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "outside the 64 KiB");

    /* A driverless PCI node whose BAR is nonetheless driven by another device.
     * This is the shape of pci00:03.0 vs eth0 on the real machine, and getting
     * it wrong would let a program poke a live NIC mid-conversation. */
    CHECK(call("driver_run", "{\"target\":\"pci00:0b.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "already driven by \"e1000fake\" as device \"snd0\"");

    /* None of those reached the device, and none of them cost an attempt on a
     * target that was never run against. */
    CHECK_EQ(forbidden, 0);
    CHECK_EQ((int)attempts_used("pci00:03.0"), 0);
}

/* ====================================================================== */
/* 7. garbage in                                                          */
/* ====================================================================== */

static void test_garbage_input(void) {
    fresh();

    /* Truncated JSON. */
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":\"hal") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "input must be a JSON object");

    /* Not an object. */
    CHECK(call("driver_run", "\"halt\"") != TOOL_OK);
    CHECK(call("driver_run", "") != TOOL_OK);
    CHECK(call("driver_run", "null") != TOOL_OK);

    /* Wrong types everywhere. */
    CHECK(call("driver_run", "{\"target\":5,\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"target\" must be a string");
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":[1,2]}") != TOOL_OK);
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":null}") != TOOL_OK);

    /* An absurd device name cannot match and cannot overrun. */
    {
        static char in[2048];
        int n = snprintf(in, sizeof in, "{\"target\":\"");
        for (int i = 0; i < 1500; i++) in[n++] = 'x';
        snprintf(in + n, sizeof in - (size_t)n, "\",\"source\":\"halt\"}");
        CHECK(call("driver_run", in) != TOOL_OK);
        CHECK_CONTAINS(rbuf, "no such device");
    }

    /* A source span that is not NUL-terminated and holds an embedded NUL. The
     * NUL ends the line as far as the assembler is concerned, so this program
     * is `mov r1, 1` with no halt after it: a bounded trap, not a crash, and
     * the model is told exactly that. */
    {
        static const char raw[] =
            "{\"target\":\"pci00:05.0\",\"source\":\"mov r1, 1\\u0000halt\"}TRAILING";
        CHECK(call_raw("driver_run", raw, strlen(raw) - 8) != TOOL_OK);
        CHECK_EQ(rres.is_error, 1);
        CHECK_CONTAINS(rbuf, "status=BAD_PC");
        CHECK_CONTAINS(rbuf, "every path must end in halt or abort");
    }

    /* Bytes that are not text at all. */
    {
        char noise[512];
        for (size_t i = 0; i < sizeof noise; i++) noise[i] = (char)(i * 7 + 3);
        CHECK(call_raw("driver_run", noise, sizeof noise) != TOOL_OK);
    }

    CHECK_EQ(acc_delta(), 0);
    CHECK_EQ(forbidden, 0);
    CHECK_EQ(kpanic_hit, 0);
}

/* ====================================================================== */
/* 8. the attempt bound                                                   */
/* ====================================================================== */

static void test_attempt_budget(void) {
    fresh();

    const char *in = run_input("    out8 0x64, 0xfe\n    halt\n");
    static char keep[24000];
    snprintf(keep, sizeof keep, "%s", in);

    for (int i = 1; i <= 5; i++) {
        CHECK(call("driver_run", keep) != TOOL_OK);
        char want[64];
        snprintf(want, sizeof want, "attempt %d of 5", i);
        CHECK_CONTAINS(rbuf, want);
        CHECK_CONTAINS(rbuf, "status=PORT_DENIED");
        CHECK_EQ((int)attempts_used("pci00:05.0"), i);
    }
    CHECK_CONTAINS(rbuf, "no attempts left on this target");

    /* The sixth is refused without assembling or running anything. */
    long before = tlk_accesses;
    CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "5 attempts against pci00:05.0 have all failed");
    CHECK_CONTAINS(rbuf, "Stop here");
    CHECK_EQ(tlk_accesses, before);   /* nothing ran */
    CHECK_EQ((int)attempts_used("pci00:05.0"), 5);

    /* A different target has its own budget. */
    CHECK(call("driver_run", "{\"target\":\"pci00:08.0\",\"source\":\"halt\"}") == TOOL_OK);

    /* An assembly failure spends an attempt too: it is still a turn spent
     * against that device. */
    dvm_tools_reset_attempts();
    CHECK_EQ(call("driver_run", run_input(prog_v2)), TOOL_OK);   /* a full log */
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":\"frobnicate\"}")
          != TOOL_OK);
    CHECK_CONTAINS(rbuf, "attempt 1 of 5");
    CHECK_CONTAINS(rbuf, "the device was not touched");
    CHECK_EQ((int)attempts_used("pci00:05.0"), 1);
    /* ...and the trace of the attempt that did not run is empty, rather than
     * the previous run's accesses filed under this attempt's verdict. */
    CHECK_EQ(call("driver_trace", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "status=BAD_PROGRAM");
    CHECK_CONTAINS(rbuf, "no device events were recorded");

    /* A success wipes the slate. */
    CHECK_EQ(call("driver_run", run_input(prog_v2)), TOOL_OK);
    CHECK_EQ((int)attempts_used("pci00:05.0"), 0);
    CHECK_EQ(forbidden, 0);
}

/* The budget must not be something the model can clear for itself. Two ways it
 * could, both closed here:
 *
 *   (a) a one-instruction `halt`. It reaches halt, so it used to be recorded as
 *       a success and zeroed the failure count — while touching nothing. Four
 *       failures, one no-op, repeat, forever, on the stock machine with a single
 *       target. This is the one that mattered.
 *   (b) target churn. attempts_used() reports 0 for a name the 8-entry table has
 *       forgotten, so evicting a nearly-exhausted target handed its budget back.
 */
static void test_budget_is_not_self_service(void) {
    fresh();

    const char *deny = run_input("    out8 0x64, 0xfe\n    halt\n");
    static char keep[24000];
    snprintf(keep, sizeof keep, "%s", deny);

    /* (a) a no-op halt is NOT convergence. */
    for (int i = 1; i <= 4; i++) CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_EQ((int)attempts_used("pci00:05.0"), 4);

    long before = tlk_accesses;
    CHECK_EQ(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":\"halt\"}"),
             TOOL_OK);                            /* the run itself still succeeds */
    CHECK_CONTAINS(rbuf, "status=OK");
    CHECK_CONTAINS(rbuf, "without touching the device");
    CHECK_EQ(tlk_accesses, before);               /* it really did nothing */
    CHECK_EQ((int)attempts_used("pci00:05.0"), 5);/* ...and it SPENT the attempt */

    CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "5 attempts against pci00:05.0 have all failed");

    /* A program that really does speak to the device still clears the count —
     * the budget is a brake on non-convergence, not a cap on work. */
    dvm_tools_reset_attempts();
    for (int i = 1; i <= 4; i++) CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_EQ((int)attempts_used("pci00:05.0"), 4);
    CHECK_EQ(call("driver_run", run_input(prog_v2)), TOOL_OK);
    CHECK_EQ((int)attempts_used("pci00:05.0"), 0);

    /* (b) churn. Only 8 functions on the fixture bus are targetable, and the
     * table holds 8, so the eviction path cannot be reached through the tool
     * here — it needs a machine with nine targetable devices, which is exactly
     * why it went unnoticed. Drive attempts_record() directly instead: it is
     * the function under test, and the two invariants are about it alone.
     *
     * Real targets are used for the names so that attempts_used() is being asked
     * the same question driver_run asks it. */
    dvm_tools_reset_attempts();

    static const char *names[] = {
        "pci00:05.0", "pci00:08.0", "pci00:09.0", "pci00:0a.0",
        "pci00:0c.0", "pci01:01.1", "pci01:03.0", "pci01:04.0",
    };
    const int NN = (int)(sizeof names / sizeof names[0]);   /* == the table size */

    /* INVARIANT 1 — no inherited history. Fill every slot to 3 failures, then
     * let a ninth target arrive. It must start from its OWN first failure: the
     * old code wrote the new name over the evicted entry but computed
     * `fails = evicted->fails + 1`, so a never-before-attempted device could be
     * refused with "5 attempts against ... have all failed" having had one. */
    for (int rep = 0; rep < 3; rep++)
        for (int i = 0; i < NN; i++) attempts_record(names[i], 0);
    for (int i = 0; i < NN; i++) CHECK_EQ((int)attempts_used(names[i]), 3);

    attempts_record("pci00:0e.0", 0);
    CHECK_EQ((int)attempts_used("pci00:0e.0"), 1);      /* not 4 */

    /* INVARIANT 2 — a nearly-exhausted target is not the one forgotten.
     * attempts_used() reports 0 for a name the table no longer holds, so
     * evicting the entry closest to its budget would hand that budget straight
     * back, and naming eight other devices once each would be a reset the model
     * performs on itself. Eviction takes the LEAST-failed entry instead. */
    dvm_tools_reset_attempts();
    for (int i = 0; i < 4; i++) attempts_record(names[0], 0);   /* A: 4 failures */
    for (int i = 1; i < NN; i++) attempts_record(names[i], 0);  /* others: 1 each */
    CHECK_EQ((int)attempts_used(names[0]), 4);

    for (int i = 0; i < 16; i++) {                 /* churn a further 16 targets */
        char nm[32];
        snprintf(nm, sizeof nm, "pci07:%02x.0", i);
        attempts_record(nm, 0);
        CHECK_EQ((int)attempts_used(nm), 1);       /* every newcomer starts at 1 */
        CHECK_EQ((int)attempts_used(names[0]), 4); /* and A never loses its count */
    }

    /* ...and the refusal really fires through the tool after the churn. */
    CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_EQ((int)attempts_used(names[0]), 5);
    CHECK(call("driver_run", keep) != TOOL_OK);
    CHECK_CONTAINS(rbuf, "5 attempts against pci00:05.0 have all failed");
    CHECK_EQ(forbidden, 0);
}

/* The display adapter is the operator's console, and its BARs are a second
 * decode path to the character memory vm/dvm.c's deny list protects at 0xb8000.
 * driver_run's description promises a program "can only reach the device you
 * named - not ... the console"; this is what makes that true. */
/* THE MODEL'S RESULT IS 1024 BYTES, NOT 4096, AND A ROW IS ~300 OF THEM.
 *
 * Every test in this file above used TOOL_RESULT_MAX (4096) because that is what
 * a host suite can conveniently read. The model never sees 4096: net/chat.c keeps
 * one shared result buffer of CHAT_TOOL_RESULT_CAP = 1024 bytes. So the listing a
 * model actually reads is four times smaller than the one every test checked, and
 * that is how the following survived:
 *
 * DRV_ROW_MARGIN was 200 bytes and was checked BEFORE a row was emitted. The
 * footer is ~240 bytes and a usable row is ~300. At len ~= 780 the check passed,
 * the row was written, the result hard-truncated MID-TOKEN, and neither the
 * "...more not shown" line nor the "targets: N known, M can be driven, K shown"
 * summary was written at all. The model was handed a listing that stopped in the
 * middle of a word, with no count and nothing saying anything was missing. On a
 * machine with several unclaimed functions the sound card could be one of the ones
 * that fell off, and nothing told the model to look further. The advice that DID
 * print at larger caps — 'raise "limit"' — could not work either, because the
 * binding constraint was bytes.
 *
 * These tests run at the REAL cap. */
static void test_the_listing_a_model_reads_is_not_truncated_mid_row(void) {
    fresh();

    /* Everything: the 17-function synthetic bus plus the reasons. This is the
     * largest answer this tool can be asked for. */
    CHECK_EQ(call_cap("driver_targets", "{\"limit\":64,\"include_unusable\":true}",
                      CHAT_TOOL_RESULT_CAP), TOOL_OK);

    /* THE FOOTER IS PRESENT. It was not, before. */
    CHECK_CONTAINS(rbuf, "targets: ");
    CHECK_CONTAINS(rbuf, "PCI function(s) known");

    /* NOT CUT MID-ROW. Every line that starts a row must be complete, which for
     * this tool means the result ends with a newline and the last line is one of
     * the footer lines rather than half of a device row. */
    size_t n = strlen(rbuf);
    CHECK(n > 0);
    if (n) CHECK_EQ(rbuf[n - 1], '\n');
    CHECK(strstr(rbuf, "[result truncated") == NULL);

    /* It says how many were left out, and it says so with a NUMBER. */
    CHECK_CONTAINS(rbuf, "more matched but this result is full");
    /* ...and the advice is one that can actually work. */
    CHECK_CONTAINS(rbuf, "\"offset\"");
    CHECK(strstr(rbuf, "more not shown (raise \"limit\")") == NULL);

    /* At least one whole usable row got through: a listing that reserves so much
     * for its footer that it shows nothing would pass every check above and be
     * useless. */
    CHECK_CONTAINS(rbuf, "target=");
    CHECK_CONTAINS(rbuf, "usable: yes");
}

/* And the rest is REACHABLE, which is the half that makes the honesty useful. */
static void test_offset_pages_through_every_function(void) {
    fresh();

    /* Page through with the offset the tool itself suggests, and collect every
     * target name. The whole bus must appear, and no name twice. */
    char seen[64][40];
    int  nseen = 0;
    int  guard = 0;
    int  offset = 0;

    for (;;) {
        char input[96];
        snprintf(input, sizeof input,
                 "{\"limit\":64,\"include_unusable\":true,\"offset\":%d}", offset);
        CHECK_EQ(call_cap("driver_targets", input, CHAT_TOOL_RESULT_CAP), TOOL_OK);
        CHECK_CONTAINS(rbuf, "targets: ");

        int this_page = 0;
        for (const char *p = rbuf; (p = strstr(p, "target=")) != NULL; ) {
            p += 7;
            char name[40];
            size_t k = 0;
            while (p[k] && p[k] != ' ' && k < sizeof name - 1) { name[k] = p[k]; k++; }
            name[k] = '\0';
            for (int j = 0; j < nseen; j++)
                CHECK(strcmp(seen[j], name) != 0);      /* no row appears twice */
            if (nseen < 64) snprintf(seen[nseen++], 40, "%s", name);
            this_page++;
        }
        CHECK(this_page > 0);                            /* progress every call */
        offset += this_page;

        if (strstr(rbuf, "more matched but this result is full") == NULL &&
            strstr(rbuf, "more not shown") == NULL) break;
        if (++guard > 32) { CHECK(0); break; }           /* no infinite paging */
    }

    printf("    (paged the whole bus in %d call(s): %d function(s))\n",
           guard + 1, nseen);

    /* The number of rows collected must equal what the summary claimed exists. */
    const char *sum = strstr(rbuf, "targets: ");
    CHECK(sum != NULL);
    if (sum) {
        int known = atoi(sum + 9);
        CHECK_EQ(nseen, known);
        CHECK(known > 3);        /* i.e. more than one page: the point of the test */
    }

    /* An offset past the end is not an error; it is an empty page with a summary,
     * which is what lets a caller stop without guessing. */
    CHECK_EQ(call_cap("driver_targets",
                      "{\"include_unusable\":true,\"offset\":4096}",
                      CHAT_TOOL_RESULT_CAP), TOOL_OK);
    CHECK_CONTAINS(rbuf, "targets: ");
    CHECK(strstr(rbuf, "target=") == NULL);

    /* And a bad offset is refused with a sentence, not clamped silently. */
    CHECK(call_cap("driver_targets", "{\"offset\":-1}", CHAT_TOOL_RESULT_CAP)
          != TOOL_OK);
    CHECK_CONTAINS(rbuf, "\"offset\"");
    CHECK(call_cap("driver_targets", "{\"offset\":99999}", CHAT_TOOL_RESULT_CAP)
          != TOOL_OK);
}

/* A USABLE ROW STILL COMES FIRST. The reason the tool sorts usable before
 * unusable is that the row a model can act on must not be the one that falls off
 * the end; reserving the footer must not have quietly broken that. */
static void test_a_usable_target_is_on_the_first_page(void) {
    fresh();
    CHECK_EQ(call_cap("driver_targets", "{\"include_unusable\":true}",
                      CHAT_TOOL_RESULT_CAP), TOOL_OK);
    const char *first_usable   = strstr(rbuf, "usable: yes");
    const char *first_unusable = strstr(rbuf, "usable: no");
    CHECK(first_usable != NULL);
    if (first_usable && first_unusable) CHECK(first_usable < first_unusable);
}

/* Page through driver_targets until `name`'s row is in rbuf, and leave it there.
 *
 * A single driver_targets result cannot hold the whole bus: one row is up to
 * DRV_ROW_MAX bytes and the tool reserves DRV_FOOTER_RESERVE so the summary
 * always prints, so even at TOOL_RESULT_MAX a long listing pages. A test that
 * wants a specific device's row has to ask for it the way a model would, and
 * doing that here means these tests also prove the paging works from the caller's
 * side. Returns 1 if the row was found. */
static int page_to_target(const char *name, size_t cap) {
    char needle[64];
    snprintf(needle, sizeof needle, "target=%s", name);
    for (int offset = 0, guard = 0; guard < 64; guard++) {
        char input[96];
        snprintf(input, sizeof input,
                 "{\"limit\":64,\"include_unusable\":true,\"offset\":%d}", offset);
        if (call_cap("driver_targets", input, cap) != TOOL_OK) return 0;
        if (strstr(rbuf, needle)) return 1;
        int rows = 0;
        for (const char *q = rbuf; (q = strstr(q, "target=")) != NULL; q += 7) rows++;
        if (rows == 0) return 0;
        offset += rows;
        if (strstr(rbuf, "more matched but this result is full") == NULL &&
            strstr(rbuf, "more not shown") == NULL) return 0;
    }
    return 0;
}

static void test_console_is_not_a_target(void) {
    fresh();

    /* driver_targets lists it, and says no. Paged for, because one result cannot
     * carry seventeen functions — see page_to_target(). */
    CHECK_EQ(page_to_target("pci01:02.0", sizeof rbuf), 1);
    CHECK_CONTAINS(rbuf, "target=pci01:02.0");
    CHECK_CONTAINS(rbuf, "display controller");
    CHECK_CONTAINS(rbuf, "operator's console");
    /* No window is printed for it, at either of its two addresses. */
    CHECK(strstr(rbuf, "0xfd000000") == NULL);
    CHECK(strstr(rbuf, "0xfebf0000") == NULL);

    /* driver_run refuses before assembling, running or spending an attempt. */
    long before = tlk_accesses;
    CHECK(call("driver_run",
               "{\"target\":\"pci01:02.0\",\"source\":"
               "\"st16 [r0+3200], 0x0f5b\\nhalt\\n\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no display device is targetable");
    CHECK_EQ(tlk_accesses, before);
    CHECK_EQ((int)attempts_used("pci01:02.0"), 0);

    /* driver_assemble is unaffected: syntax checking touches no hardware and
     * names no target, so there is nothing to protect there. */
    CHECK_EQ(call("driver_assemble", "{\"source\":\"st16 [r0+0], 1\\nhalt\\n\"}"),
             TOOL_OK);
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */
/* 9. driver_trace                                                        */
/* ====================================================================== */

static void test_trace_tool(void) {
    fresh();

    /* Nothing has run in this process state? The log is per-run, so run first. */
    CHECK_EQ(call("driver_run", run_input(prog_v2)), TOOL_OK);

    CHECK_EQ(call("driver_trace", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "last run: target=pci00:05.0 attempt 1 status=OK");
    CHECK_CONTAINS(rbuf, "in32 0xd000 => 0x4b4c5401");
    CHECK_CONTAINS(rbuf, "shown ");

    /* Paging from the start. */
    CHECK_EQ(call("driver_trace", "{\"offset\":0,\"limit\":2}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "(events 1-2)");

    /* ...and from the end. */
    CHECK_EQ(call("driver_trace", "{\"limit\":3,\"tail\":true}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "shown 3 of ");

    CHECK(call("driver_trace", "{\"offset\":9999}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "past the end");
    CHECK(call("driver_trace", "{\"limit\":0}") != TOOL_OK);
    CHECK(call("driver_trace", "{\"tail\":\"yes\"}") != TOOL_OK);

    /* More events than the ring holds: the answer says so rather than lying
     * about which access was first. */
    fresh();
    CHECK(call("driver_run",
               "{\"target\":\"pci00:05.0\",\"source\":\""
               "    mov r9, 400\\n"
               "loop:\\n"
               "    in32 r1, r0\\n"
               "    sub  r9, 1\\n"
               "    cmp  r9, 0\\n"
               "    bne  loop\\n"
               "    halt\\n\"}") == TOOL_OK);
    CHECK_EQ(call("driver_trace", "{\"limit\":2}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "400 events happened; the last 256 are kept");
    CHECK_CONTAINS(rbuf, "(events 145-146)");
    CHECK_EQ(forbidden, 0);
}

static void test_trace_before_any_run(void) {
    /* Fresh state is asserted by running this before anything else touches
     * g_last — see the order in main(). */
    CHECK(call("driver_trace", "{}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no program has been run yet");
}

/* ====================================================================== */
/* 10. no I/O backend                                                     */
/* ====================================================================== */

static void test_no_backend(void) {
    fresh();
    dvm_tools_set_io(NULL);           /* dvm_io_hardware() is NULL on the host */
    CHECK(call("driver_run", "{\"target\":\"pci00:05.0\",\"source\":\"halt\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no hardware I/O backend");
    dvm_tools_set_io(&tlk_io);
}

/* ====================================================================== */
/* 11. the recorded session, replayed through the real turn loop          */
/* ====================================================================== */

static void queue(const char *body) { CHECK_EQ(model_mock_queue(200, body), MODEL_OK); }

static void test_golden_transcript(void) {
    fresh();
    model_mock_reset();
    chat_reset();
    chat_set_max_rounds(0);
    chat_init(model_mock_transport());
    kcap_reset();

    queue(TR_ROUND1);
    queue(TR_ROUND2);
    queue(TR_ROUND3);
    queue(TR_ROUND4);

    CHECK_EQ(chat_ask(TR_SENTENCE), CHAT_OK);

    /* Four round trips: three tool rounds and the answer. */
    CHECK_EQ((int)model_mock_request_count(), 4);
    CHECK_EQ((int)chat_last_rounds(), 4);
    CHECK_EQ((int)chat_last_tool_calls(), 3);
    CHECK_EQ((int)model_mock_pending(), 0);

    const char *console = kcap_text();

    /* --- what the kernel did, in the kernel's own words --- */
    CHECK_CONTAINS(console, "[driver_targets known=");
    CHECK_CONTAINS(console, "[dvm.grant ports 0xd000-0xd0ff -> ok]");
    CHECK_CONTAINS(console, "[dvm.grant mmio none -> ok]");
    CHECK_CONTAINS(console, "[dvm.grant pci 00:05.0 -> ok]");
    CHECK_CONTAINS(console, "[dvm.trap");
    CHECK_CONTAINS(console, "ABORT: rate did not stick");
    CHECK_CONTAINS(console, "[driver_run pci00:05.0 attempt=1 ABORT");
    CHECK_CONTAINS(console, "[driver_run pci00:05.0 attempt=2");
    CHECK_CONTAINS(console, "[dvm.halt");
    /* The program's own print lines, for the operator. */
    CHECK_CONTAINS(console, "[dvm.print id = 0x4b4c5401 -> ok]");
    CHECK_CONTAINS(console, "[dvm.print count =");

    /* --- the target list survived chat.c's 1 KiB result clip WITH the row the
     * model has to act on: that is why usable targets are listed first --- */
    const char *req2 = model_mock_request(1);
    CHECK(req2 != NULL);
    CHECK_CONTAINS(req2, "target=pci00:05.0");
    CHECK_CONTAINS(req2, "usable: yes  sandbox: ports 0xd000-0xd0ff");

    /* --- what the model saw between the two attempts --- */
    const char *req3 = model_mock_request(2);
    CHECK(req3 != NULL);
    CHECK_CONTAINS(req3, "status=ABORT");
    CHECK_CONTAINS(req3, "rate did not stick");
    CHECK_CONTAINS(req3, "out16 0xd00c <= 0x100");   /* the write... */
    CHECK_CONTAINS(req3, "in16 0xd00c => 0x0");      /* ...and the readback */
    CHECK_CONTAINS(req3, "attempt(s) left on this target");
    /* ...marked as a failure, so the model knows the action did not happen. */
    CHECK_CONTAINS(req3, "\"is_error\":true");

    /* --- and what the last request carried: a successful run --- */
    const char *req4 = model_mock_request(3);
    CHECK(req4 != NULL);
    CHECK_CONTAINS(req4, "status=OK - the program reached halt");
    CHECK_CONTAINS(req4, "r11=0x100");

    /* --- the device is genuinely up --- */
    CHECK_EQ(tlk.ready, 1);
    CHECK_EQ(tlk.running, 1);
    CHECK_EQ(tlk.rate, 0x100u);
    CHECK(tlk.count > 0);
    CHECK_EQ(forbidden, 0);
    /* The kernel wrote the command register (that is how the device got to be a
     * bus master), but only ever the three bits it is allowed to. */
    CHECK_EQ(cfg_bad, 0);

    /* The operator's whole view of the session, on request. */
    if (getenv("DVM_SHOW_TRANSCRIPT")) {
        printf("\n----- session as the operator saw it -----\n> %s\n%s"
               "------------------------------------------\n",
               TR_SENTENCE, console);
    }

    /* The closing claim is checked against the assembler, so the recorded prose
     * cannot drift away from the recorded program. */
    CHECK_EQ(call("driver_assemble", asm_input(prog_v2)), TOOL_OK);
    CHECK_CONTAINS(rbuf, "ok: 39 instructions");
    CHECK(strstr(TR_ROUND4, "39 instructions") != NULL);

    /* --- and it replays identically --- */
    tlk_reset_device();
    dvm_tools_reset_attempts();
    model_mock_reset();
    chat_reset();
    queue(TR_ROUND1);
    queue(TR_ROUND2);
    queue(TR_ROUND3);
    queue(TR_ROUND4);
    kcap_reset();
    CHECK_EQ(chat_ask(TR_SENTENCE), CHAT_OK);
    CHECK_EQ(tlk.rate, 0x100u);
    CHECK_EQ(tlk.running, 1);
    CHECK_CONTAINS(kcap_text(), "[driver_run pci00:05.0 attempt=2");
}

/* ====================================================================== */
/* 12. the model that never converges                                     */
/* ====================================================================== */

static void test_never_converges(void) {
    fresh();
    model_mock_reset();
    chat_reset();
    chat_init(model_mock_transport());
    kcap_reset();

    queue(TR_STUCK1);
    queue(TR_STUCK2);
    queue(TR_STUCK3);
    queue(TR_STUCK4);
    queue(TR_STUCK5);
    queue(TR_STUCK6);
    queue(TR_STUCK_GIVE_UP);

    CHECK_EQ(chat_ask(TR_SENTENCE), CHAT_OK);
    CHECK_EQ((int)model_mock_request_count(), 7);
    CHECK_EQ((int)model_mock_pending(), 0);

    /* Five ran and were refused by the VM; the sixth never reached it. */
    CHECK_CONTAINS(kcap_text(), "[driver_run pci00:05.0 attempt=5 PORT_DENIED");
    CHECK(strstr(kcap_text(), "attempt=6") == NULL);

    const char *last_tool_req = model_mock_request(6);
    CHECK(last_tool_req != NULL);
    CHECK_CONTAINS(last_tool_req, "5 attempts against pci00:05.0 have all failed");

    /* The device was never touched: every one of those programs died on the
     * denied port before its first legal access... */
    CHECK_EQ(tlk.running, 0);
    CHECK_EQ(forbidden, 0);

    /* ...and the turn still ended with the model answering the operator. */
    CHECK_CONTAINS(kcap_text(), "I cannot bring that device up");
}

/* The other bound: a model that keeps calling tools forever is stopped by
 * chat.c, and the turn is abandoned rather than answered blind. */
static void test_round_cap(void) {
    fresh();
    model_mock_reset();
    chat_reset();
    chat_init(model_mock_transport());
    chat_set_max_rounds(2);
    kcap_reset();

    queue(TR_STUCK1);
    queue(TR_STUCK2);
    CHECK_EQ(chat_ask(TR_SENTENCE), CHAT_ELIMIT);
    CHECK_CONTAINS(kcap_text(), "stopped after 2 tool rounds");
    chat_set_max_rounds(0);
}

/* ====================================================================== */
/* 13. the model's syscall surface still fits the request                 */
/* ====================================================================== */

static void test_schema(void) {
    static char schema[CHAT_TOOLS_BYTES];
    size_t n = tools_build_schema(schema, sizeof schema);
    CHECK(n > 0);
    CHECK(n < sizeof schema);

    json_value_t v;
    CHECK_EQ(json_parse(schema, n, &v), JSON_OK);
    CHECK_EQ((int)v.type, (int)JSON_ARRAY);
    CHECK_EQ((int)json_count(&v), 5);          /* only this family is linked here */

    CHECK_CONTAINS(schema, "driver_targets");
    CHECK_CONTAINS(schema, "driver_assemble");
    CHECK_CONTAINS(schema, "driver_run");
    CHECK_CONTAINS(schema, "driver_install");
    CHECK_CONTAINS(schema, "driver_trace");
    /* The ISA the model has to write in is in the schema, not in its memory. */
    CHECK_CONTAINS(schema, "out8|out16|out32 port,a");
    CHECK_CONTAINS(schema, "pcicfg rd,bdf,off");

    printf("    (this family's schema: %u bytes of the %u-byte budget)\n",
           (unsigned)n, (unsigned)CHAT_TOOLS_BYTES);

    /* Only driver_run mutates the machine. */
    CHECK_TOOL_FLAGS("driver_run", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("driver_targets", ~0u, 0);
    CHECK_TOOL_FLAGS("driver_assemble", ~0u, 0);
    CHECK_TOOL_FLAGS("driver_trace", ~0u, 0);
}

/* ====================================================================== */
/* 14. fuzz: nothing the model can say may escape the sandbox             */
/* ====================================================================== */

static uint32_t rng_state = 0x13572468u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_fuzz_programs(void) {
    static const char *frag[] = {
        "halt", "nop", "mov r%u, 0x%x", "add r%u, r%u, 3", "in8 r%u, 0x%x",
        "out8 0x%x, r%u", "in32 r%u, r0", "out32 r0, r%u", "ld32 r%u, [0x%x]",
        "st16 [r%u+0x%x], r1", "pcicfg r%u, 0x%x, 0x%x", "delay %u",
        "cmp r%u, r%u", "bne top", "jmp top", "call top", "ret", "push r%u",
        "pop r%u", "div r%u, r1, r2", "shl r%u, r%u, %u", "abort \"x\"",
        "print \"p\", r%u", "top:", "; comment", ".equ K 0x%x", "@@@garbage",
        "in16 r%u, r%u", "st8 [r0+0x%x], 0x%x", "test r%u, 0x%x",
        /* The DMA path, fuzzed as hard as the rest of the ISA: stores and loads at
         * arbitrary offsets from the buffer base (so most of them are off the end),
         * and writes into TLK1's descriptor registers with garbage addresses and
         * lengths, so the engine is repeatedly pointed at nonsense. None of that may
         * fault, hang, panic, or reach anything outside the sandbox. */
        "st32 [r7+0x%x], r%u", "ld32 r%u, [r7+0x%x]", "st8 [r7+%u], 0x%x",
        "add r9, r7, r%u", "st16 [r9+0x%x], r1", "sub r9, r7, 0x%x",
        "out32 r0, r7", "and r5, r7, 0x%x", "shr r5, r7, %u",
        "mov r4, 0xd018", "mov r4, 0xd01c", "mov r4, 0xd020", "mov r4, 0xd024",
        "out32 r4, r%u", "out32 r4, 0x%x", "in32 r%u, r4",
    };
    const int nfrag = (int)(sizeof frag / sizeof frag[0]);

    int ran = 0, refused = 0;
    for (int iter = 0; iter < 3000; iter++) {
        char src[2048];
        int  n = 0;
        int  lines = 1 + (int)(rnd() % 24);
        for (int i = 0; i < lines && n < (int)sizeof src - 80; i++) {
            const char *f = frag[rnd() % (unsigned)nfrag];
            n += snprintf(src + n, sizeof src - (size_t)n, f,
                          rnd() % 20, rnd() % 0x12000, rnd() % 64, rnd() % 40000);
            n += snprintf(src + n, sizeof src - (size_t)n, "\n");
        }
        fresh();
        int rc = call("driver_run", run_input(src));
        if (rc == TOOL_OK) ran++; else refused++;

        /* Whatever it was, it stayed inside. */
        CHECK_EQ(forbidden, 0);
        CHECK_EQ(kpanic_hit, 0);
        CHECK_EQ(cfg_bad, 0);
        CHECK(rres.len < rres.cap);
    }
    printf("    (3000 fuzzed programs: %d halted, %d refused, forbidden=%ld, "
           "dma transfers=%ld, wild dma=%ld)\n",
           ran, refused, forbidden, dma_fetches, dma_wild);
    CHECK_EQ(forbidden, 0);
    /* `dma_wild` is NOT required to be zero, and that is the honest result. Every
     * one of those is a fuzzed program that pointed a bus master outside the buffer
     * it was given; a real device would have performed the write. The VM refused
     * none of them, because it cannot: it polices the CPU, not the DMA engine. What
     * IS required is everything above — no fault, no panic, nothing outside the
     * sandbox, and no config-space write beyond the three permitted bits. */
}

static void test_fuzz_inputs(void) {
    static const char *shapes[] = {
        "{}", "[]", "null", "true", "0", "\"x\"", "{\"target\":", "{,}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",}",
        "{\"target\":\"pci00:05.0\",\"source\":{\"a\":1}}",
        "{\"target\":{},\"source\":\"halt\"}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"max_steps\":-1}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"max_steps\":99999999999}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"delay_budget_ms\":-5}",
        "{\"target\":\"pci00:05.0\",\"source\":\"halt\",\"trace\":42}",
        "{\"TARGET\":\"pci00:05.0\",\"SOURCE\":\"halt\"}",
    };
    const int n = (int)(sizeof shapes / sizeof shapes[0]);

    for (int i = 0; i < n; i++) {
        fresh();
        for (int t = 0; t < 4; t++) {
            const char *names[4] = { "driver_run", "driver_assemble",
                                     "driver_targets", "driver_trace" };
            call(names[t], shapes[i]);
            CHECK_EQ(kpanic_hit, 0);
            CHECK(rres.len < rres.cap);
        }
    }

    /* Every prefix of a valid call: truncation must never be mistaken for a
     * shorter but valid request. */
    const char *good = "{\"target\":\"pci00:05.0\",\"source\":\"mov r1,1\\nhalt\"}";
    for (size_t len = 0; len < strlen(good); len++) {
        fresh();
        CHECK(call_raw("driver_run", good, len) != TOOL_OK);
        CHECK_EQ(kpanic_hit, 0);
    }
    CHECK_EQ(forbidden, 0);
}

/* ====================================================================== */

int main(void) {
    console_init();
    build_machine();
    build_qemu_replica();
    snapshot_cmds();
    decode_transcript_programs();
    dvm_tools_set_io(&tlk_io);

    RUN(test_trace_before_any_run);      /* must precede any run */
    RUN(test_targets);
    RUN(test_targets_refusals);
    RUN(test_qemu_bus_layout);
    RUN(test_assemble);
    RUN(test_assemble_oversized);
    RUN(test_run_success);
    RUN(test_run_failure_feedback);
    RUN(test_run_denied_port);
    RUN(test_run_mmio_window);

    RUN(test_dma_buffer_arrives_in_r7_r8);
    RUN(test_dma_buffer_is_real_memory);
    RUN(test_a_device_really_dmas_out_of_the_buffer);
    RUN(test_bus_master_enable);
    RUN(test_the_vm_cannot_police_dma);
    RUN(test_guard_band_is_reported_to_the_model);

    RUN(test_install_lets_a_model_driver_play);
    RUN(test_a_failed_play_program_says_why);
    RUN(test_install_refusals);
    RUN(test_install_can_be_corrected);
    RUN(test_install_enables_bus_mastering_permanently);

    RUN(test_run_bounded);
    RUN(test_run_target_validation);
    RUN(test_run_with_no_device);
    RUN(test_garbage_input);
    RUN(test_attempt_budget);
    RUN(test_budget_is_not_self_service);
    RUN(test_the_listing_a_model_reads_is_not_truncated_mid_row);
    RUN(test_offset_pages_through_every_function);
    RUN(test_a_usable_target_is_on_the_first_page);
    RUN(test_console_is_not_a_target);
    RUN(test_trace_tool);
    RUN(test_no_backend);
    RUN(test_golden_transcript);
    RUN(test_never_converges);
    RUN(test_round_cap);
    RUN(test_schema);
    RUN(test_fuzz_programs);
    RUN(test_fuzz_inputs);

    printf("    (device accesses: %ld, delays: %ld, forbidden: %ld, "
           "config writes: %ld, dma transfers: %ld, wild dma: %ld)\n",
           tlk_accesses, tlk_delays, forbidden, cfg_writes, dma_fetches, dma_wild);
    CHECK_EQ(forbidden, 0);
    /* Config writes now happen on purpose. What must never happen is a write to
     * anything but three bits of one function's command register. */
    CHECK_EQ(cfg_bad, 0);
    CHECK(cfg_writes > 0);          /* ...and the escalation really is exercised */

    return th_report("dvm_tools");
}
