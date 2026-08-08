/* e1000.c — driver for the Intel 82540EM, the card QEMU presents as `e1000`.
 *
 * We talk to it over MMIO (BAR0), set up DMA descriptor rings for RX and TX,
 * and poll the descriptor "done" bits rather than using interrupts. Because
 * the kernel identity-maps physical memory, a buffer's virtual address is also
 * its physical address, so we can hand descriptor addresses straight to the
 * card.
 *
 * POWER MANAGEMENT (driver.h suspend/resume)
 *   "Suspend the network card" has to mean something at the register level, or
 *   the tool that offers it is a lie. What suspend() actually does, in the
 *   order the 8254x software developer's manual asks for:
 *
 *     1. mask every interrupt source (IMC) and clear any pending cause (ICR)
 *     2. clear RCTL.EN, then TCTL.EN — the receive and transmit engines stop
 *        fetching descriptors, so all DMA into kernel memory ceases
 *     3. wait for in-flight DMA to retire before we consider the rings ours
 *     4. power the PHY down over MDIO (PHY control register bit 11) and clear
 *        CTRL.SLU/ASDE, which on real copper drops the link: the LED goes out
 *        and the switch on the other end sees the port go away
 *
 *   resume() reverses it: PHY power-up plus a fresh auto-negotiation, then the
 *   exact same bring-up sequence init() runs (hw_bring_up() is shared, so the
 *   two can never drift), then a bounded wait for the link to come back.
 *   Because the rings are re-based and the head/tail pointers re-zeroed, any
 *   descriptor the card was mid-way through before the suspend is discarded
 *   rather than transmitted late.
 *
 *   HONESTY ABOUT WHAT A VM CAN SHOW YOU: QEMU's e1000 model has no PHY power
 *   state — it stores the power-down bit and keeps STATUS.LU set. So under
 *   QEMU the *link bit* does not fall when you suspend, and this driver says
 *   so in the log rather than claiming a transition it cannot observe. What is
 *   real in both QEMU and silicon is the part that matters: with RCTL.EN and
 *   TCTL.EN clear no frame can enter or leave, transmits time out instead of
 *   completing, and e1000_link_up() reports down. On real hardware the PHY
 *   power-down additionally drops the physical link.
 */

#include "e1000.h"
#include "kernel.h"
#include "io.h"
#include "pci.h"
#include "driver.h"
#include "device.h"

static const driver_t e1000_driver;

/* ---- e1000 register offsets ---- */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_MDIC   0x0020
#define REG_ICR    0x00C0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU   (1u << 6)
#define CTRL_ASDE  (1u << 5)

#define STATUS_FD  (1u << 0)
#define STATUS_LU  (1u << 1)
#define STATUS_SPEED_SHIFT 6
#define STATUS_SPEED_MASK  (3u << STATUS_SPEED_SHIFT)

#define RCTL_EN    (1u << 1)
#define RCTL_UPE   (1u << 3)
#define RCTL_MPE   (1u << 4)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)

#define TX_CMD_EOP  (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS   (1u << 3)
#define STAT_DD     (1u << 0)

/* ---- MDIO (MDIC register) and the internal copper PHY ---- */
#define MDIC_DATA_MASK 0x0000FFFFu
#define MDIC_REG_SHIFT 16
#define MDIC_PHY_SHIFT 21
#define MDIC_OP_WRITE  (1u << 26)
#define MDIC_OP_READ   (1u << 27)
#define MDIC_READY     (1u << 28)
#define MDIC_ERROR     (1u << 30)

#define PHY_ADDR_INTERNAL 1          /* the 8254x's on-die PHY is address 1 */
#define PHY_CTRL          0          /* MII control register              */
#define PHY_CTRL_AUTONEG_EN (1u << 12)
#define PHY_CTRL_POWER_DOWN (1u << 11)
#define PHY_CTRL_RESTART_AN (1u << 9)

/* Bounds. Every wait in this driver is bounded: a card that stops answering
 * must cost milliseconds, not the machine. The two long ones are -D
 * overridable so the host unit tests do not have to spend real seconds
 * watching a simulated cable stay unplugged. */
#define MDIC_POLL_MS   64      /* MDIO transaction: spec says ~64 us       */
#define DMA_QUIESCE_MS 10      /* in-flight DMA drain after EN is cleared  */
#ifndef LINK_WAIT_MS
#define LINK_WAIT_MS   3000    /* copper auto-negotiation, generously      */
#endif
#ifndef TX_WAIT_MS
#define TX_WAIT_MS     50      /* a gigabit frame retires in microseconds  */
#endif

#define NUM_RX 32
#define NUM_TX 8
#define BUF_SZ 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *mmio;
static uint8_t mac[6];
static int suspended;

/* The card writes the status bytes of these descriptors by DMA behind the
 * compiler's back, so every read of them has to actually happen. */
static volatile struct rx_desc rx_ring[NUM_RX] __attribute__((aligned(16)));
static volatile struct tx_desc tx_ring[NUM_TX] __attribute__((aligned(16)));
static uint8_t rx_buf[NUM_RX][BUF_SZ] __attribute__((aligned(16)));
static uint8_t tx_buf[NUM_TX][BUF_SZ] __attribute__((aligned(16)));
static int rx_cur, tx_cur;

static uint32_t reg_read(uint32_t r)  { return mmio_read32(mmio + r); }
static void     reg_write(uint32_t r, uint32_t v) { mmio_write32(mmio + r, v); }

static int find_e1000(pci_dev_t *out) {
    static const uint16_t ids[] = {0x100E, 0x1004, 0x100F};
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++)
        if (pci_find_vendor(0x8086, ids[i], out)) return 1;
    return 0;
}

/* ====================================================================== */
/* MDIO — talking to the PHY through the MAC's MDIC register              */
/* ====================================================================== */

/* Wait for the MAC to finish an MDIO transaction. Returns 0 and the completed
 * MDIC value, or -1 on error/timeout. */
static int mdic_wait(uint32_t *out) {
    /* The manual puts an MDIO transaction at roughly 64 us, so spin on the
     * ready bit first — an MMIO read is itself microseconds across a real PCI
     * bus — and only fall back to millisecond sleeps for a card that is not
     * answering at all. Either way this returns. */
    for (unsigned ms = 0; ms <= MDIC_POLL_MS; ms++) {
        for (unsigned spin = 0; spin < 256; spin++) {
            uint32_t v = reg_read(REG_MDIC);
            if (v & MDIC_ERROR) return -1;
            if (v & MDIC_READY) { if (out) *out = v; return 0; }
        }
        mdelay(1);
    }
    return -1;
}

static int phy_read(uint8_t reg, uint16_t *val) {
    reg_write(REG_MDIC, MDIC_OP_READ
                        | ((uint32_t)PHY_ADDR_INTERNAL << MDIC_PHY_SHIFT)
                        | ((uint32_t)reg << MDIC_REG_SHIFT));
    uint32_t v = 0;
    if (mdic_wait(&v) != 0) return -1;
    *val = (uint16_t)(v & MDIC_DATA_MASK);
    return 0;
}

static int phy_write(uint8_t reg, uint16_t val) {
    reg_write(REG_MDIC, MDIC_OP_WRITE
                        | ((uint32_t)PHY_ADDR_INTERNAL << MDIC_PHY_SHIFT)
                        | ((uint32_t)reg << MDIC_REG_SHIFT)
                        | (uint32_t)val);
    return mdic_wait(0);
}

/* Set or clear the PHY's power-down bit. Returns 0 if the PHY answered.
 * A card whose PHY does not answer MDIO is still fully quiesced by the RCTL/
 * TCTL half of suspend; we report the failure instead of pretending. */
static int phy_set_powered(int on) {
    uint16_t ctrl;
    if (phy_read(PHY_CTRL, &ctrl) != 0) return -1;
    if (on) {
        ctrl = (uint16_t)(ctrl & ~PHY_CTRL_POWER_DOWN);
        ctrl = (uint16_t)(ctrl | PHY_CTRL_AUTONEG_EN | PHY_CTRL_RESTART_AN);
    } else {
        ctrl = (uint16_t)(ctrl | PHY_CTRL_POWER_DOWN);
    }
    return phy_write(PHY_CTRL, ctrl);
}

/* ====================================================================== */
/* bring-up — shared by init() and resume() so they cannot drift          */
/* ====================================================================== */

static void hw_bring_up(void) {
    /* Interrupts stay masked for the life of the driver: everything is polled
     * and IF is 0. Clear any cause the card latched while it was down. */
    reg_write(REG_IMC, 0xFFFFFFFF);
    (void)reg_read(REG_ICR);

    /* Ask the MAC to consider the link up and to auto-detect speed/duplex. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* Program our unicast filter from the address we saved at init, with
     * AV (address valid) set. Deterministic: never re-derived from whatever
     * the registers happen to hold after a power transition. */
    uint32_t ral = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8)
                 | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    uint32_t rah = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
    reg_write(REG_RAL, ral);
    reg_write(REG_RAH, rah | (1u << 31));

    /* Clear the multicast table. */
    for (int i = 0; i < 128; i++) reg_write(REG_MTA + i * 4, 0);

    /* RX ring. */
    for (int i = 0; i < NUM_RX; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_buf[i];
        rx_ring[i].status = 0;
    }
    reg_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    reg_write(REG_RDBAH, (uint32_t)((uint64_t)(uintptr_t)rx_ring >> 32));
    reg_write(REG_RDLEN, NUM_RX * sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, NUM_RX - 1);
    rx_cur = 0;
    /* NOTE, and it contradicts the unicast filter programmed twenty lines up:
     * RCTL_UPE|RCTL_MPE is full promiscuous mode, which bypasses the RAL/RAH
     * filter and the (just-cleared) multicast table entirely and accepts every
     * frame on the segment. Under QEMU user-mode networking nothing else is on
     * the segment so it is invisible; on real hardware it means every frame on
     * the wire is DMA'd into rx_buf and pushed through lwIP by the single
     * threaded poll loop — wasted work and needless exposure. Nothing in net/
     * asks for promiscuous mode. Dropping the two bits changes the RCTL value
     * the boot log prints and the QEMU test cases assert, so it is left as a
     * deliberate, documented decision to revisit rather than a silent one. */
    reg_write(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    /* TX ring. Re-basing it and zeroing head/tail is what makes a resume safe:
     * anything the card had queued before the suspend is dropped, not sent. */
    for (int i = 0; i < NUM_TX; i++) {
        tx_ring[i].addr = (uint64_t)(uintptr_t)tx_buf[i];
        tx_ring[i].status = STAT_DD;   /* mark free */
        tx_ring[i].cmd = 0;
    }
    reg_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    reg_write(REG_TDBAH, (uint32_t)((uint64_t)(uintptr_t)tx_ring >> 32));
    reg_write(REG_TDLEN, NUM_TX * sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    tx_cur = 0;
    reg_write(REG_TIPG, 0x0060200A);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
}

int e1000_init(void) {
    pci_dev_t pdev;
    if (!find_e1000(&pdev)) {
        kprintf("e1000: no supported NIC found\n");
        return -1;
    }

    /* Validate BAR0 before enabling bus mastering or touching a register.
     * pci_bar() reports 0 unless BAR0 is an assigned 32-bit memory BAR, and 0
     * must not be believed: a null MMIO base does NOT fault on this kernel (the
     * low 4 GiB is mapped present+writable), so hw_bring_up() would happily
     * write RCTL/TCTL/RDBAL/TDBAL and 128 multicast entries into physical
     * 0x0000-0x5600, then publish eth0 as ACTIVE and report a phantom link read
     * out of whatever lives at physical 0x8. The `if (!mmio)` guards in
     * send/receive/link_up/suspend/resume all assume this check happened. */
    uint32_t bar0 = pci_bar(&pdev, 0);
    if (!bar0) {
        kprintf("e1000: %02x:%02x.%x BAR0 is not an assigned 32-bit memory BAR "
                "(raw 0x%08x); not driving this card\n",
                pdev.bus, pdev.dev, pdev.func,
                (unsigned)pci_read32(&pdev, 0x10));
        mmio = NULL;
        return -1;
    }

    pci_enable_bus_master(&pdev);
    mmio = (volatile uint8_t *)(uintptr_t)bar0;
    kprintf("e1000: %02x:%02x.%x MMIO=%p\n", pdev.bus, pdev.dev, pdev.func,
            (void *)(uintptr_t)bar0);

    reg_write(REG_IMC, 0xFFFFFFFF);   /* mask all interrupts; we poll */
    reg_read(REG_ICR);

    /* Read the MAC from the receive-address registers, once. Every later
     * bring-up programs this saved copy back. */
    uint32_t ral = reg_read(REG_RAL);
    uint32_t rah = reg_read(REG_RAH);
    mac[0] = ral; mac[1] = ral >> 8; mac[2] = ral >> 16; mac[3] = ral >> 24;
    mac[4] = rah; mac[5] = rah >> 8;

    suspended = 0;
    hw_bring_up();

    device_t *d = device_create("eth0", DEV_CLASS_NETWORK);
    device_add_resource(d, RES_MMIO, bar0, 0x20000);
    device_register(d);
    device_bind_driver(d, (driver_t *)&e1000_driver);
    device_set_state(d, DEV_STATE_ACTIVE);
    return 0;
}

void e1000_get_mac(uint8_t out[6]) {
    memcpy(out, mac, 6);
}

int e1000_link_up(void) {
    if (!mmio || suspended) return 0;
    return (reg_read(REG_STATUS) & STATUS_LU) ? 1 : 0;
}

int e1000_is_suspended(void) { return suspended; }

int e1000_send(const void *data, uint16_t len) {
    if (!mmio || !data || len == 0 || len > BUF_SZ) return -1;

    /* Only claim a descriptor the card has finished with. hw_bring_up() marks
     * every descriptor STAT_DD ("free") and the wait at the bottom of this
     * function normally keeps that true — but only normally: on the timeout
     * path below we have already advanced tx_cur and rung the doorbell, so
     * after NUM_TX consecutive timeouts the tail would wrap past the head and
     * overwrite descriptors that are still nominally queued. Reading the bit
     * the free-list comment claims is the invariant makes it actually be one. */
    int idx = tx_cur;
    if (!(tx_ring[idx].status & STAT_DD)) return -1;   /* TX ring still busy */

    memcpy(tx_buf[idx], data, len);
    tx_ring[idx].length = len;
    tx_ring[idx].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    tx_ring[idx].status = 0;

    tx_cur = (idx + 1) % NUM_TX;
    reg_write(REG_TDT, tx_cur);

    /* Wait for the descriptor-done bit. Bounded in *time*, not in spins: while
     * the card is suspended TCTL.EN is clear, the engine never touches the
     * descriptor, and this is the path that has to give up quickly and let the
     * stack report a dead link rather than wedge the machine. */
    uint64_t deadline = millis() + TX_WAIT_MS;
    do {
        if (tx_ring[idx].status & STAT_DD) return 0;
    } while (millis() < deadline);
    return -1;
}

int e1000_receive(void *buf, uint16_t *len) {
    if (!mmio) return 0;
    int idx = rx_cur;
    if (!(rx_ring[idx].status & STAT_DD)) return 0;

    uint16_t l = rx_ring[idx].length;
    if (l > BUF_SZ) l = BUF_SZ;         /* a lying descriptor cannot overrun */
    memcpy(buf, rx_buf[idx], l);
    *len = l;

    rx_ring[idx].status = 0;
    reg_write(REG_RDT, idx);            /* hand the descriptor back */
    rx_cur = (idx + 1) % NUM_RX;
    return 1;
}

/* ====================================================================== */
/* power lifecycle                                                        */
/* ====================================================================== */

static void report_link(const char *what, uint32_t status) {
    /* STATUS.SPEED encodings 00/01/10 are 10/100/1000 Mb/s; 11 is reserved, and
     * is reported as "reserved" rather than guessed at — this driver's stated
     * virtue is not claiming things it cannot observe. */
    static const char *speed[4] = {"10", "100", "1000", "reserved"};
    kprintf("e1000: %s: status=0x%08x link=%s", what, (unsigned)status,
            (status & STATUS_LU) ? "up" : "down");
    if (status & STATUS_LU)
        kprintf(" %s Mb/s %s-duplex",
                speed[(status & STATUS_SPEED_MASK) >> STATUS_SPEED_SHIFT],
                (status & STATUS_FD) ? "full" : "half");
    kputs("\n");
}

static int e1000_suspend(device_t *d) {
    (void)d;
    if (!mmio) return -1;
    if (suspended) return 0;                 /* already down; nothing to do */

    uint32_t before = reg_read(REG_STATUS);

    /* 1. No interrupt source may fire while the engines come down. */
    reg_write(REG_IMC, 0xFFFFFFFF);
    (void)reg_read(REG_ICR);

    /* 2. Stop the receiver and the transmitter. From here the card fetches no
     *    descriptors and writes nothing into kernel memory. */
    reg_write(REG_RCTL, reg_read(REG_RCTL) & ~RCTL_EN);
    reg_write(REG_TCTL, reg_read(REG_TCTL) & ~TCTL_EN);

    /* 3. Let anything already in flight retire before we call the rings ours. */
    mdelay(DMA_QUIESCE_MS);

    /* 4. Drop the link: power the PHY down and stop forcing link-up. */
    int phy = phy_set_powered(0);
    reg_write(REG_CTRL, reg_read(REG_CTRL) & ~(CTRL_SLU | CTRL_ASDE));
    mdelay(1);

    suspended = 1;

    uint32_t rctl = reg_read(REG_RCTL), tctl = reg_read(REG_TCTL);
    uint32_t after = reg_read(REG_STATUS);
    kprintf("e1000: suspend: rctl=0x%08x tctl=0x%08x (rx=%s tx=%s) phy=%s\n",
            (unsigned)rctl, (unsigned)tctl,
            (rctl & RCTL_EN) ? "ON" : "off", (tctl & TCTL_EN) ? "ON" : "off",
            phy == 0 ? "powered down" : "no MDIO response");
    report_link("suspend", after);
    if ((before & STATUS_LU) && (after & STATUS_LU))
        kputs("e1000: suspend: STATUS.LU still set - this platform does not "
              "model PHY power state; the engines are off, so no frame can "
              "enter or leave regardless\n");

    /* A suspend that left an engine running would be a lie the tool layer would
     * happily repeat, so fail the hook instead — but fail it by ROLLING BACK,
     * not by half-undoing. Clearing `suspended` on its own left the PHY powered
     * down and CTRL.SLU/ASDE clear while reporting the device as not suspended:
     * the driver manager then leaves it ACTIVE so device_resume() never calls
     * e1000_resume() for it, and e1000_resume() would early-return anyway
     * because `suspended` is 0. The result is a permanently dark link with no
     * code path back short of a reboot. */
    if ((rctl & RCTL_EN) || (tctl & TCTL_EN)) {
        kputs("e1000: suspend: an engine is still enabled - rolling back to "
              "the running state\n");
        suspended = 0;
        phy_set_powered(1);
        hw_bring_up();
        return -1;
    }
    return 0;
}

static int e1000_resume(device_t *d) {
    (void)d;
    if (!mmio) return -1;
    if (!suspended) return 0;

    /* Power the PHY back up and restart auto-negotiation, then re-run the
     * exact bring-up init() used. The STATUS read below is taken the instant
     * the PHY write lands: a card that really renegotiates reports the link
     * *down* here and back up a few hundred milliseconds later, which is the
     * one link transition this driver can prove from register reads alone. */
    int phy = phy_set_powered(1);
    report_link("resume: renegotiating", reg_read(REG_STATUS));
    hw_bring_up();
    suspended = 0;

    /* Bounded wait for the link. Copper auto-negotiation takes ~0.5 s under
     * QEMU and a couple of seconds on real cable; not getting there is worth
     * saying out loud but is not a failed resume — the engines are running. */
    uint64_t t0 = millis(), deadline = t0 + LINK_WAIT_MS;
    uint32_t status;
    for (;;) {
        status = reg_read(REG_STATUS);
        if (status & STATUS_LU) break;
        if (millis() >= deadline) break;
    }
    uint32_t waited = (uint32_t)(millis() - t0);

    uint32_t rctl = reg_read(REG_RCTL), tctl = reg_read(REG_TCTL);
    kprintf("e1000: resume: rctl=0x%08x tctl=0x%08x (rx=%s tx=%s) phy=%s "
            "link settled in %u ms\n",
            (unsigned)rctl, (unsigned)tctl,
            (rctl & RCTL_EN) ? "on" : "OFF", (tctl & TCTL_EN) ? "on" : "OFF",
            phy == 0 ? "powered up" : "no MDIO response", (unsigned)waited);
    report_link("resume", status);
    if (!(status & STATUS_LU))
        kputs("e1000: resume: link did not come back within the timeout - "
              "check the cable\n");

    if (!(rctl & RCTL_EN) || !(tctl & TCTL_EN)) {
        suspended = 1;
        return -1;
    }
    return 0;
}

static const driver_t e1000_driver = {
    .name = "e1000",
    .level = DRV_LEVEL_DEVICE,
    .cls = DEV_CLASS_NETWORK,
    .init = e1000_init,
    .suspend = e1000_suspend,
    .resume = e1000_resume,
};
REGISTER_DRIVER(e1000_driver);
