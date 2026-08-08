/* test_e1000.c — host tests for the e1000 driver, and in particular for its
 * power lifecycle.
 *
 * WHAT CAN AND CANNOT BE TESTED HERE
 *   drivers/net/e1000.c reaches the card through exactly two functions,
 *   mmio_read32() and mmio_write32(), plus three PCI lookups. So, in the same
 *   spirit as tests/host/test_dev_tools.c, this file simply *is* the hardware:
 *   drivers/pci/pci.c is not linked, the MMIO accessors are redirected at a
 *   simulated 82540EM register file, and drivers/net/e1000.c is #included
 *   unmodified. Everything above the bus — the bring-up sequence, the MDIO
 *   transactions, the descriptor rings, the suspend/resume state machine, the
 *   bounds on a lying receive descriptor, and the lines the driver prints — is
 *   therefore covered natively, in milliseconds, with no QEMU boot.
 *
 *   The simulated card models REAL SILICON, not QEMU: its link bit follows
 *   CTRL.SLU and the PHY power-down bit, which is precisely the behaviour a VM
 *   cannot show you (QEMU's e1000 has no PHY power state and keeps STATUS.LU
 *   pinned). So the "suspend really drops the link" assertion lives here, and
 *   the "suspend really stops frames, resume really restores TLS" assertion
 *   lives in a boot instead. Between them the feature is covered end to end.
 *
 *   What is NOT covered here, and is proved by booting: that mmio_read32 talks
 *   to a real BAR, and that a real 82540EM answers MDIO the way this file
 *   pretends it does.
 *
 * REGISTER_DRIVER ON A MACH-O HOST
 *   REGISTER_DRIVER places a pointer in a linker section named "driver_table"
 *   and relies on GNU ld's __start_/__stop_ symbols. Mach-O has neither, so the
 *   macro is neutralised below. kernel/drivers.c (the only consumer of those
 *   symbols) is not linked into this test.
 */

#include "harness.h"

#ifdef __APPLE__
/* Apple's <string.h> defines memcpy/... as fortifying macros, which collide
 * with kernel.h's plain prototypes when both land in one TU. */
#undef memcpy
#undef memset
#undef memmove
#undef memcmp
#undef strlen
#endif

/* include/io.h is x86 port I/O written in inline assembly and cannot even be
 * assembled on an arm64 host. Claim its include guard so that the driver's own
 * #include is a no-op, and hand it the two MMIO accessors it actually uses
 * (defined below, pointed at the simulated register file) instead. */
#define IO_H

#include "pci.h"
#include "device.h"
#include "driver.h"
#include "kernel.h"

#include <string.h>

/* ====================================================================== */
/* the simulated 82540EM                                                  */
/* ====================================================================== */

#define FAKE_BAR        0xFEBC0000u
#define FAKE_REG_BYTES  0x6000
#define FAKE_REG_WORDS  (FAKE_REG_BYTES / 4)

/* Register offsets, spelled out again on purpose: this side of the seam is the
 * hardware, so it does not get to share the driver's #defines. If the driver
 * ever writes the wrong offset, these two maps disagree and a test fails. */
#define FR_CTRL   0x0000
#define FR_STATUS 0x0008
#define FR_MDIC   0x0020
#define FR_ICR    0x00C0
#define FR_IMC    0x00D8
#define FR_RCTL   0x0100
#define FR_TCTL   0x0400
#define FR_TIPG   0x0410
#define FR_RDBAL  0x2800
#define FR_RDLEN  0x2808
#define FR_RDH    0x2810
#define FR_RDT    0x2818
#define FR_TDBAL  0x3800
#define FR_TDLEN  0x3808
#define FR_TDH    0x3810
#define FR_TDT    0x3818
#define FR_MTA    0x5200
#define FR_RAL    0x5400
#define FR_RAH    0x5404

#define FR_CTRL_ASDE  (1u << 5)
#define FR_CTRL_SLU   (1u << 6)
#define FR_STATUS_FD  (1u << 0)
#define FR_STATUS_LU  (1u << 1)
#define FR_RCTL_EN    (1u << 1)
#define FR_TCTL_EN    (1u << 1)

#define FR_MDIC_OP_WRITE (1u << 26)
#define FR_MDIC_OP_READ  (1u << 27)
#define FR_MDIC_READY    (1u << 28)
#define FR_MDIC_ERROR    (1u << 30)

#define PHY_POWER_DOWN   (1u << 11)
#define PHY_AUTONEG_EN   (1u << 12)
#define PHY_RESTART_AN   (1u << 9)

/* The MAC the "EEPROM" loaded into RAL/RAH before the driver ever looked. */
static const uint8_t hw_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

static uint32_t regs[FAKE_REG_WORDS];
static uint16_t phy[32];
static int      phy_powered;
static int      phy_answers_mdio;     /* clear it to simulate a dead PHY   */
static int      cable_plugged;        /* clear it and link never settles   */
static int      autoneg_ticks;        /* STATUS reads left before link up  */
static long     bad_access;           /* off the end of the BAR, or unaligned */
static long     imc_writes;
static uint32_t imc_last;
static long     bus_master_enables;
static void   (*tx_kick)(void);       /* wired up after e1000.c is included */

/* Link is up only when the PHY has power, the MAC is asking for link, and
 * auto-negotiation has settled. This is silicon behaviour; QEMU's model keeps
 * STATUS.LU set regardless, which is why the driver reports both. */
static uint32_t fake_status(void) {
    uint32_t s = 0x80080780u;                    /* bits 7:6 = 10b = 1000 Mb/s */
    if (cable_plugged && phy_powered &&
        (regs[FR_CTRL / 4] & FR_CTRL_SLU) && autoneg_ticks == 0)
        s |= FR_STATUS_LU | FR_STATUS_FD;
    return s;
}

static void fake_mdic(uint32_t val) {
    unsigned reg = (val >> 16) & 0x1F;
    uint16_t data = (uint16_t)(val & 0xFFFF);

    if (!phy_answers_mdio) {                      /* MDIO never completes */
        regs[FR_MDIC / 4] = val;
        return;
    }
    if (val & FR_MDIC_OP_READ) {
        regs[FR_MDIC / 4] = (val & ~0xFFFFu) | phy[reg] | FR_MDIC_READY;
        return;
    }
    if (val & FR_MDIC_OP_WRITE) {
        if (reg == 0) {
            phy_powered = (data & PHY_POWER_DOWN) ? 0 : 1;
            if ((data & PHY_AUTONEG_EN) && (data & PHY_RESTART_AN))
                autoneg_ticks = 4;                /* settles a few reads later */
            /* RESTART_AN is self-clearing, like the real part. */
            data = (uint16_t)(data & ~PHY_RESTART_AN);
        }
        phy[reg] = data;
        regs[FR_MDIC / 4] = val | FR_MDIC_READY;
        return;
    }
    regs[FR_MDIC / 4] = val | FR_MDIC_ERROR;
}

static size_t fake_off(const volatile void *addr) {
    uintptr_t a = (uintptr_t)addr;
    if (a < FAKE_BAR || a + 4 > FAKE_BAR + FAKE_REG_BYTES || ((a - FAKE_BAR) & 3)) {
        bad_access++;
        return 0;
    }
    return (size_t)(a - FAKE_BAR);
}

static uint32_t fake_mmio_read32(const volatile void *addr) {
    size_t off = fake_off(addr);
    if (off == FR_STATUS) {
        if (autoneg_ticks > 0) autoneg_ticks--;
        return fake_status();
    }
    return regs[off / 4];
}

static void fake_mmio_write32(volatile void *addr, uint32_t val) {
    size_t off = fake_off(addr);
    regs[off / 4] = val;
    if (off == FR_MDIC) { fake_mdic(val); return; }
    if (off == FR_IMC)  { imc_writes++; imc_last = val; return; }
    /* Writing the tail pointer is what starts a transmit — but only if the
     * transmit engine is enabled. That is the whole point of suspend. */
    if (off == FR_TDT && (regs[FR_TCTL / 4] & FR_TCTL_EN) && tx_kick) tx_kick();
}

/* ---- the PCI side ---- */

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    (void)bus; (void)dev; (void)fn; (void)off;
    return 0xFFFFFFFFu;
}
void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    (void)bus; (void)dev; (void)fn; (void)off; (void)v;
}
uint32_t pci_read32(const pci_dev_t *d, uint8_t off) { (void)d; (void)off; return 0; }
void pci_write32(const pci_dev_t *d, uint8_t off, uint32_t v) { (void)d; (void)off; (void)v; }
int pci_find_class(uint8_t c, uint8_t s, pci_dev_t *out) { (void)c; (void)s; (void)out; return 0; }

static int nic_present = 1;

int pci_find_vendor(uint16_t vendor, uint16_t device, pci_dev_t *out) {
    if (!nic_present || vendor != 0x8086 || device != 0x100E) return 0;
    memset(out, 0, sizeof *out);
    out->bus = 0; out->dev = 3; out->func = 0;
    out->vendor = 0x8086; out->device = 0x100E;
    out->class = 0x02; out->subclass = 0x00;
    return 1;
}
void pci_enable_bus_master(const pci_dev_t *d) { (void)d; bus_master_enables++; }
uint32_t pci_bar(const pci_dev_t *d, int bar) {
    (void)d;
    return bar == 0 ? FAKE_BAR : 0;
}

/* ====================================================================== */
/* the driver under test                                                  */
/* ====================================================================== */

#define mmio_read32(a)     fake_mmio_read32((a))
#define mmio_write32(a, v) fake_mmio_write32((a), (v))

/* Real time still passes on the host, so shrink the two waits that are
 * measured in wall-clock milliseconds. The kernel keeps its own defaults. */
#define LINK_WAIT_MS 20
#define TX_WAIT_MS   5

#undef  REGISTER_DRIVER
#define REGISTER_DRIVER(var) extern const driver_t *const __drvptr_unused_##var

#include "../../drivers/net/e1000.c"

/* ====================================================================== */
/* fixture                                                                */
/* ====================================================================== */

static void complete_all_tx(void) {
    for (int i = 0; i < NUM_TX; i++) tx_ring[i].status |= STAT_DD;
}

/* Reset the simulated card to power-on state and run the driver's init. */
static void boot_card(void) {
    memset(regs, 0, sizeof regs);
    memset(phy, 0, sizeof phy);
    phy[0]          = PHY_AUTONEG_EN;
    phy_powered     = 1;
    phy_answers_mdio = 1;
    cable_plugged   = 1;
    autoneg_ticks   = 0;
    bad_access      = 0;
    imc_writes      = 0;
    imc_last        = 0;
    bus_master_enables = 0;
    tx_kick         = complete_all_tx;

    /* The EEPROM has already loaded the station address into RAL/RAH. */
    regs[FR_RAL / 4] = (uint32_t)hw_mac[0] | ((uint32_t)hw_mac[1] << 8)
                     | ((uint32_t)hw_mac[2] << 16) | ((uint32_t)hw_mac[3] << 24);
    regs[FR_RAH / 4] = (uint32_t)hw_mac[4] | ((uint32_t)hw_mac[5] << 8);

    mmio = 0;
    suspended = 0;
    kcap_reset();
    CHECK_EQ(e1000_init(), 0);
}

static uint32_t r(uint32_t off) { return regs[off / 4]; }

/* ====================================================================== */
/* bring-up                                                               */
/* ====================================================================== */

static void test_init_brings_the_card_up(void) {
    boot_card();

    CHECK_EQ(bus_master_enables, 1);          /* DMA needs bus mastering  */
    CHECK_EQ(bad_access, 0);                  /* never off the end of BAR0 */

    CHECK(r(FR_RCTL) & FR_RCTL_EN);
    CHECK(r(FR_TCTL) & FR_TCTL_EN);
    CHECK(r(FR_CTRL) & FR_CTRL_SLU);
    CHECK(r(FR_CTRL) & FR_CTRL_ASDE);

    /* Rings are based, sized, and their head/tail are where the manual says. */
    CHECK(r(FR_RDBAL) != 0);
    CHECK_EQ(r(FR_RDLEN), NUM_RX * sizeof(struct rx_desc));
    CHECK_EQ(r(FR_RDH), 0);
    CHECK_EQ(r(FR_RDT), NUM_RX - 1);
    CHECK(r(FR_TDBAL) != 0);
    CHECK_EQ(r(FR_TDLEN), NUM_TX * sizeof(struct tx_desc));
    CHECK_EQ(r(FR_TDH), 0);
    CHECK_EQ(r(FR_TDT), 0);

    /* Unicast filter programmed and marked valid. */
    CHECK(r(FR_RAH) & (1u << 31));
    CHECK_EQ(r(FR_RAL) & 0xFF, hw_mac[0]);
    CHECK_EQ(r(FR_RAH) & 0xFFFF, (uint32_t)hw_mac[4] | ((uint32_t)hw_mac[5] << 8));

    /* Multicast table cleared, all 128 entries. */
    for (int i = 0; i < 128; i++) CHECK_EQ(r(FR_MTA + i * 4), 0u);

    /* Interrupts masked: this kernel polls and IF stays 0. */
    CHECK(imc_writes >= 1);
    CHECK_EQ(imc_last, 0xFFFFFFFFu);

    uint8_t mac_out[6];
    e1000_get_mac(mac_out);
    CHECK_EQ(memcmp(mac_out, hw_mac, 6), 0);

    CHECK_EQ(e1000_link_up(), 1);
    CHECK_EQ(e1000_is_suspended(), 0);
}

static void test_missing_nic_is_a_clean_failure(void) {
    nic_present = 0;
    mmio = 0;
    kcap_reset();
    CHECK_EQ(e1000_init(), -1);
    CHECK_CONTAINS(kcap_text(), "e1000: no supported NIC found");
    nic_present = 1;

    /* With no card mapped, every entry point has to refuse rather than
     * dereference a null BAR. (This kernel does not fault on null.) */
    uint8_t frame[60] = {0};
    uint16_t len = 0;
    CHECK_EQ(e1000_send(frame, sizeof frame), -1);
    CHECK_EQ(e1000_receive(frame, &len), 0);
    CHECK_EQ(e1000_link_up(), 0);
    CHECK_EQ(e1000_suspend(NULL), -1);
    CHECK_EQ(e1000_resume(NULL), -1);
}

static void test_driver_advertises_power_hooks(void) {
    /* The gap this file exists to close: dev_tools reports suspendable=no
     * unless these are non-NULL, and device_suspend refuses outright. */
    CHECK(e1000_driver.init    != NULL);
    CHECK(e1000_driver.suspend != NULL);
    CHECK(e1000_driver.resume  != NULL);
    CHECK_STR(e1000_driver.name, "e1000");
    CHECK_EQ(e1000_driver.cls, DEV_CLASS_NETWORK);
    CHECK_EQ(e1000_driver.level, DRV_LEVEL_DEVICE);
}

static void test_init_publishes_eth0(void) {
    boot_card();
    device_t *d = device_find_by_class(DEV_CLASS_NETWORK, 0);
    CHECK(d != NULL);
    if (!d) return;
    CHECK_STR(d->obj.name, "eth0");
    CHECK_EQ(d->state, DEV_STATE_ACTIVE);
    CHECK(d->driver == (driver_t *)&e1000_driver);
    CHECK_EQ(d->nres, 1);
    CHECK_EQ(d->res[0].type, RES_MMIO);
    CHECK_EQ(d->res[0].base, FAKE_BAR);
}

/* ====================================================================== */
/* suspend                                                                */
/* ====================================================================== */

static void test_suspend_quiesces_the_engines(void) {
    boot_card();
    kcap_reset();
    CHECK_EQ(e1000_suspend(NULL), 0);

    CHECK_EQ(r(FR_RCTL) & FR_RCTL_EN, 0u);       /* receiver stopped      */
    CHECK_EQ(r(FR_TCTL) & FR_TCTL_EN, 0u);       /* transmitter stopped   */
    CHECK_EQ(imc_last, 0xFFFFFFFFu);             /* interrupts masked     */
    CHECK_EQ(e1000_is_suspended(), 1);
    CHECK_EQ(bad_access, 0);

    /* Everything else in RCTL/TCTL is preserved — suspend clears the enable
     * bit, it does not scribble the configuration. */
    CHECK(r(FR_RCTL) & (1u << 15));              /* BAM still set         */
    CHECK(r(FR_TCTL) & (1u << 3));               /* PSP still set         */

    CHECK_CONTAINS(kcap_text(), "rx=off tx=off");
    CHECK_CONTAINS(kcap_text(), "phy=powered down");
}

static void test_suspend_drops_the_link(void) {
    boot_card();
    CHECK_EQ(e1000_link_up(), 1);
    kcap_reset();
    CHECK_EQ(e1000_suspend(NULL), 0);

    /* On real silicon both of these fall: the PHY is powered down and the MAC
     * stops asking for link. */
    CHECK_EQ(phy[0] & PHY_POWER_DOWN, PHY_POWER_DOWN);
    CHECK_EQ(r(FR_CTRL) & FR_CTRL_SLU, 0u);
    CHECK_EQ(fake_status() & FR_STATUS_LU, 0u);
    CHECK_EQ(e1000_link_up(), 0);
    CHECK_CONTAINS(kcap_text(), "link=down");
    /* ...so the "this platform does not model PHY power" caveat must NOT be
     * printed here. It is only for hosts whose link bit refuses to fall. */
    CHECK(strstr(kcap_text(), "STATUS.LU still set") == NULL);
}

static void test_transmit_fails_while_suspended(void) {
    boot_card();
    uint8_t frame[60];
    memset(frame, 0xA5, sizeof frame);
    CHECK_EQ(e1000_send(frame, sizeof frame), 0);      /* fine while active */

    CHECK_EQ(e1000_suspend(NULL), 0);
    /* The engine is off, so the card never writes the descriptor-done bit and
     * the send has to give up rather than spin forever. */
    CHECK_EQ(e1000_send(frame, sizeof frame), -1);
    CHECK_EQ(e1000_link_up(), 0);
}

static void test_receive_yields_nothing_while_suspended(void) {
    boot_card();
    uint8_t buf[BUF_SZ];
    uint16_t len = 0;
    /* Nothing has arrived, and nothing can: RCTL.EN is clear. */
    CHECK_EQ(e1000_suspend(NULL), 0);
    CHECK_EQ(e1000_receive(buf, &len), 0);
}

static void test_double_suspend_is_a_no_op(void) {
    boot_card();
    CHECK_EQ(e1000_suspend(NULL), 0);
    uint32_t rctl = r(FR_RCTL), tctl = r(FR_TCTL);
    CHECK_EQ(e1000_suspend(NULL), 0);        /* idempotent, not an error   */
    CHECK_EQ(r(FR_RCTL), rctl);
    CHECK_EQ(r(FR_TCTL), tctl);
    CHECK_EQ(e1000_is_suspended(), 1);
}

static void test_suspend_survives_a_dead_phy(void) {
    boot_card();
    phy_answers_mdio = 0;                    /* MDIO never signals ready   */
    kcap_reset();
    /* A PHY that will not answer is worth saying out loud, but the engines
     * still come down and the suspend still succeeds. */
    CHECK_EQ(e1000_suspend(NULL), 0);
    CHECK_EQ(r(FR_RCTL) & FR_RCTL_EN, 0u);
    CHECK_EQ(r(FR_TCTL) & FR_TCTL_EN, 0u);
    CHECK_CONTAINS(kcap_text(), "no MDIO response");
    phy_answers_mdio = 1;
}

/* ====================================================================== */
/* resume                                                                 */
/* ====================================================================== */

static void test_resume_restores_the_engines_and_the_link(void) {
    boot_card();
    CHECK_EQ(e1000_suspend(NULL), 0);
    kcap_reset();
    CHECK_EQ(e1000_resume(NULL), 0);

    CHECK(r(FR_RCTL) & FR_RCTL_EN);
    CHECK(r(FR_TCTL) & FR_TCTL_EN);
    CHECK(r(FR_CTRL) & FR_CTRL_SLU);
    CHECK_EQ(phy[0] & PHY_POWER_DOWN, 0u);
    CHECK(phy[0] & PHY_AUTONEG_EN);
    CHECK_EQ(e1000_is_suspended(), 0);
    CHECK_EQ(e1000_link_up(), 1);
    CHECK_CONTAINS(kcap_text(), "rx=on tx=on");
    CHECK_CONTAINS(kcap_text(), "phy=powered up");
    CHECK(strstr(kcap_text(), "link did not come back") == NULL);
}

/* The strongest property in this file: a resumed card is bit-for-bit the card
 * init() built. If someone adds a register to hw_bring_up() and forgets it in
 * one of the two paths, this fails. (STATUS is computed, not stored, and MDIC
 * legitimately holds the last transaction, so both are excluded.) */
static void test_resume_leaves_the_card_identical_to_init(void) {
    boot_card();
    uint32_t snapshot[FAKE_REG_WORDS];
    memcpy(snapshot, regs, sizeof snapshot);

    CHECK_EQ(e1000_suspend(NULL), 0);
    CHECK_EQ(e1000_resume(NULL), 0);

    int diffs = 0;
    for (size_t i = 0; i < FAKE_REG_WORDS; i++) {
        if (i == FR_MDIC / 4 || i == FR_STATUS / 4) continue;
        if (regs[i] != snapshot[i]) {
            diffs++;
            if (diffs <= 4)
                printf("    reg 0x%04zx: init=0x%08x resumed=0x%08x\n",
                       i * 4, snapshot[i], regs[i]);
        }
    }
    CHECK_EQ(diffs, 0);
}

static void test_resume_reprograms_the_mac_filter(void) {
    boot_card();
    CHECK_EQ(e1000_suspend(NULL), 0);
    /* Pretend the card lost its receive-address registers over the power
     * transition, which is exactly what a real D3 -> D0 cycle does. */
    regs[FR_RAL / 4] = 0;
    regs[FR_RAH / 4] = 0;
    CHECK_EQ(e1000_resume(NULL), 0);

    CHECK(r(FR_RAH) & (1u << 31));
    CHECK_EQ(r(FR_RAL) & 0xFF, hw_mac[0]);
    CHECK_EQ((r(FR_RAL) >> 24) & 0xFF, hw_mac[3]);
    CHECK_EQ(r(FR_RAH) & 0xFF, hw_mac[4]);
    uint8_t mac_out[6];
    e1000_get_mac(mac_out);
    CHECK_EQ(memcmp(mac_out, hw_mac, 6), 0);
}

static void test_resume_discards_a_frame_queued_while_down(void) {
    boot_card();
    CHECK_EQ(e1000_suspend(NULL), 0);

    uint8_t frame[60];
    memset(frame, 0x5A, sizeof frame);
    CHECK_EQ(e1000_send(frame, sizeof frame), -1);   /* queued, never sent */
    CHECK(r(FR_TDT) != 0);                           /* tail did advance   */

    CHECK_EQ(e1000_resume(NULL), 0);
    /* Head and tail are back at zero and every descriptor is marked free, so
     * the stale frame is dropped rather than transmitted late. */
    CHECK_EQ(r(FR_TDH), 0u);
    CHECK_EQ(r(FR_TDT), 0u);
    CHECK_EQ(tx_cur, 0);
    for (int i = 0; i < NUM_TX; i++) CHECK(tx_ring[i].status & STAT_DD);
}

static void test_transmit_works_again_after_resume(void) {
    boot_card();
    CHECK_EQ(e1000_suspend(NULL), 0);
    CHECK_EQ(e1000_resume(NULL), 0);

    uint8_t frame[60];
    memset(frame, 0x3C, sizeof frame);
    for (int i = 0; i < NUM_TX * 3; i++)
        CHECK_EQ(e1000_send(frame, sizeof frame), 0);   /* wraps the ring */
}

static void test_resume_without_suspend_is_a_no_op(void) {
    boot_card();
    uint32_t rctl = r(FR_RCTL);
    CHECK_EQ(e1000_resume(NULL), 0);
    CHECK_EQ(r(FR_RCTL), rctl);
    CHECK_EQ(e1000_is_suspended(), 0);
}

static void test_resume_reports_a_link_that_never_comes_back(void) {
    boot_card();
    CHECK_EQ(e1000_suspend(NULL), 0);
    cable_plugged = 0;          /* unplugged: negotiation never settles */
    kcap_reset();
    /* Still a successful resume — the engines are running, the link is not
     * this driver's to conjure — but it must say so. */
    CHECK_EQ(e1000_resume(NULL), 0);
    CHECK_CONTAINS(kcap_text(), "link did not come back");
    CHECK_EQ(e1000_link_up(), 0);

    /* ...and plugging it back in needs no further driver action: the engines
     * were already running, so the link simply appears. */
    cable_plugged = 1;
    CHECK_EQ(e1000_link_up(), 1);
}

static void test_a_full_power_cycle_repeats_cleanly(void) {
    boot_card();
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(e1000_suspend(NULL), 0);
        CHECK_EQ(e1000_link_up(), 0);
        CHECK_EQ(e1000_resume(NULL), 0);
        CHECK_EQ(e1000_link_up(), 1);
    }
    uint8_t frame[128];
    memset(frame, 0x11, sizeof frame);
    CHECK_EQ(e1000_send(frame, sizeof frame), 0);
    CHECK_EQ(bad_access, 0);
}

/* ====================================================================== */
/* data path bounds                                                       */
/* ====================================================================== */

static void test_send_rejects_an_oversized_frame(void) {
    boot_card();
    static uint8_t big[BUF_SZ + 1];
    memset(big, 0x77, sizeof big);
    CHECK_EQ(e1000_send(big, BUF_SZ + 1), -1);
    CHECK_EQ(e1000_send(big, BUF_SZ), 0);
}

static void test_receive_clamps_a_lying_descriptor(void) {
    boot_card();

    /* A card (or a corrupted ring) claiming a 60000-byte frame in a 2048-byte
     * buffer must not be allowed to write past the caller's buffer. */
    static struct { uint8_t buf[BUF_SZ]; uint8_t guard[256]; } sink;
    memset(&sink, 0xEE, sizeof sink);

    rx_ring[rx_cur].length = 60000;
    rx_ring[rx_cur].status = STAT_DD;

    uint16_t len = 0;
    CHECK_EQ(e1000_receive(sink.buf, &len), 1);
    CHECK_EQ(len, BUF_SZ);
    for (unsigned i = 0; i < sizeof sink.guard; i++) CHECK_EQ(sink.guard[i], 0xEE);
}

static void test_receive_walks_the_ring_and_returns_descriptors(void) {
    boot_card();
    uint8_t buf[BUF_SZ];
    uint16_t len = 0;

    CHECK_EQ(e1000_receive(buf, &len), 0);        /* nothing pending */

    for (int i = 0; i < NUM_RX * 2; i++) {
        int idx = rx_cur;
        rx_buf[idx][0] = (uint8_t)(i + 1);
        rx_ring[idx].length = 64;
        rx_ring[idx].status = STAT_DD;
        CHECK_EQ(e1000_receive(buf, &len), 1);
        CHECK_EQ(len, 64);
        CHECK_EQ(buf[0], (uint8_t)(i + 1));
        CHECK_EQ(r(FR_RDT), (uint32_t)idx);       /* descriptor handed back */
        CHECK_EQ(rx_ring[idx].status, 0);         /* and marked empty       */
    }
    CHECK_EQ(rx_cur, 0);                          /* wrapped exactly twice  */
}

int main(void) {
    RUN(test_init_brings_the_card_up);
    RUN(test_missing_nic_is_a_clean_failure);
    RUN(test_driver_advertises_power_hooks);
    RUN(test_init_publishes_eth0);

    RUN(test_suspend_quiesces_the_engines);
    RUN(test_suspend_drops_the_link);
    RUN(test_transmit_fails_while_suspended);
    RUN(test_receive_yields_nothing_while_suspended);
    RUN(test_double_suspend_is_a_no_op);
    RUN(test_suspend_survives_a_dead_phy);

    RUN(test_resume_restores_the_engines_and_the_link);
    RUN(test_resume_leaves_the_card_identical_to_init);
    RUN(test_resume_reprograms_the_mac_filter);
    RUN(test_resume_discards_a_frame_queued_while_down);
    RUN(test_transmit_works_again_after_resume);
    RUN(test_resume_without_suspend_is_a_no_op);
    RUN(test_resume_reports_a_link_that_never_comes_back);
    RUN(test_a_full_power_cycle_repeats_cleanly);

    RUN(test_send_rejects_an_oversized_frame);
    RUN(test_receive_clamps_a_lying_descriptor);
    RUN(test_receive_walks_the_ring_and_returns_descriptors);
    return th_report("e1000");
}
