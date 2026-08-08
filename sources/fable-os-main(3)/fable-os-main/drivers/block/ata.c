/* ata.c — ATA (IDE) disk driver, programmed I/O, no DMA and no interrupts.
 *
 * PURPOSE
 *   Give this kernel somewhere to put bytes that survives a reboot. It
 *   publishes each disk it finds as a block_device_t (include/block.h); the
 *   filesystem above it never sees a port.
 *
 * WHY PIO AND NOT DMA
 *   This machine runs with IF=0, every PIC line masked, and one thread. A DMA
 *   engine needs an interrupt or a completion poll and a coherent buffer; ATA
 *   PIO needs neither — the CPU moves the bytes through the data register and
 *   the transfer is finished when the function returns. It is slower (~5 MB/s
 *   under QEMU, measured by boot timing, not by claim) and that is a fine
 *   trade for the thing that must not lose the operator's data. nIEN is set on
 *   both channels so the drives never assert IRQ 14/15 at all.
 *
 * WHAT IT SUPPORTS
 *   The two legacy channels (0x1F0/0x3F6 and 0x170/0x376), master and slave on
 *   each: four possible drives. IDENTIFY DEVICE, READ SECTORS (0x20), WRITE
 *   SECTORS (0x30) and FLUSH CACHE (0xE7), all LBA28. LBA28 addresses 128 GiB;
 *   a larger disk is REGISTERED CLAMPED to 128 GiB with a loud line, because
 *   silently exposing sectors the read path cannot address is how a filesystem
 *   comes to write into a wrap-around alias of its own superblock.
 *   ATAPI (CD-ROM) is detected and skipped — it needs a packet interface this
 *   driver does not implement, and pretending a CD is a disk is worse.
 *
 * ERRORS ARE ERRORS
 *   Every wait is bounded by a deadline. Every command checks ERR/DF and reads
 *   the error register. On a timeout the channel gets a software reset so one
 *   bad command cannot wedge the disk for the rest of the boot. Nothing here
 *   ever returns success with a buffer it did not fill: see the argument in
 *   include/block.h.
 *
 * TESTABILITY
 *   All four port primitives go through ata_io_ops_t, so tests/host can hand
 *   the driver a synthetic drive and assert on the register traffic. The
 *   default backend is the real inb/outb and exists only in a kernel build.
 */

#include "block.h"
#include "kernel.h"
#include "device.h"
#include "driver.h"
#include <string.h>

#ifndef FABLEOS_HOSTTEST
#  include "io.h"
#endif

/* ---------------------------------------------------------------------- */
/* 1. the port backend                                                     */
/* ---------------------------------------------------------------------- */

typedef struct ata_io_ops {
    uint8_t (*in8)(uint16_t port);
    void    (*out8)(uint16_t port, uint8_t val);
    void    (*in16_rep)(uint16_t port, void *buf, uint32_t words);
    void    (*out16_rep)(uint16_t port, const void *buf, uint32_t words);
} ata_io_ops_t;

#ifndef FABLEOS_HOSTTEST
static uint8_t hw_in8(uint16_t p) { return inb(p); }
static void    hw_out8(uint16_t p, uint8_t v) { outb(p, v); }
static void    hw_in16_rep(uint16_t p, void *buf, uint32_t words) {
    __asm__ volatile("cld; rep insw" : "+D"(buf), "+c"(words) : "d"(p) : "memory");
}
static void    hw_out16_rep(uint16_t p, const void *buf, uint32_t words) {
    __asm__ volatile("cld; rep outsw" : "+S"(buf), "+c"(words) : "d"(p));
}
static const ata_io_ops_t hw_io = { hw_in8, hw_out8, hw_in16_rep, hw_out16_rep };
#  define ATA_DEFAULT_IO (&hw_io)
#else
/* A host build has no ports. Reading 0xFF everywhere is what an absent
 * controller looks like (the "floating bus"), so a host build that forgets to
 * install a backend finds no drives rather than inventing one. */
static uint8_t absent_in8(uint16_t p) { (void)p; return 0xFF; }
static void    absent_out8(uint16_t p, uint8_t v) { (void)p; (void)v; }
static void    absent_in16_rep(uint16_t p, void *buf, uint32_t words) {
    (void)p; memset(buf, 0xFF, (size_t)words * 2);
}
static void    absent_out16_rep(uint16_t p, const void *buf, uint32_t w) {
    (void)p; (void)buf; (void)w;
}
static const ata_io_ops_t absent_io = { absent_in8, absent_out8,
                                        absent_in16_rep, absent_out16_rep };
#  define ATA_DEFAULT_IO (&absent_io)
#endif

static const ata_io_ops_t *g_io = ATA_DEFAULT_IO;

void ata_set_io(const ata_io_ops_t *ops) { g_io = ops ? ops : ATA_DEFAULT_IO; }

/* ---------------------------------------------------------------------- */
/* 2. register map                                                         */
/* ---------------------------------------------------------------------- */

#define ATA_REG_DATA      0
#define ATA_REG_ERROR     1   /* read  */
#define ATA_REG_FEATURES  1   /* write */
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA0      3
#define ATA_REG_LBA1      4
#define ATA_REG_LBA2      5
#define ATA_REG_DRIVE     6
#define ATA_REG_STATUS    7   /* read; reading it clears a pending IRQ */
#define ATA_REG_COMMAND   7   /* write */

/* Status bits. */
#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_RDY  0x40
#define ATA_SR_BSY  0x80

/* Error register bits worth naming in a message. */
#define ATA_ER_ABRT 0x04
#define ATA_ER_IDNF 0x10
#define ATA_ER_UNC  0x40

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_FLUSH      0xE7
#define ATA_CMD_IDENTIFY   0xEC

/* Device Control register (the control block). nIEN keeps the drive from
 * asserting its interrupt line; SRST is the channel software reset. */
#define ATA_CTL_NIEN 0x02
#define ATA_CTL_SRST 0x04

/* LBA28 tops out here. Also the clamp applied to a bigger disk. */
#define ATA_LBA28_MAX 0x0FFFFFFFu

/* Sectors per command. LBA28 allows 256 (count byte 0); 128 keeps every
 * transfer under 64 KiB, which is a size a caller's buffer plausibly is. */
#define ATA_MAX_XFER 128

/* Bounded waits. Overridable so a host test does not sit through a real
 * five-second spin-up deadline against a synthetic drive that will never
 * answer. */
#ifndef ATA_TIMEOUT_MS
#  define ATA_TIMEOUT_MS 5000     /* a command must complete inside this   */
#endif
#ifndef ATA_PROBE_TIMEOUT_MS
#  define ATA_PROBE_TIMEOUT_MS 500 /* IDENTIFY on a drive that may not exist */
#endif
#ifndef ATA_RESET_TIMEOUT_MS
#  define ATA_RESET_TIMEOUT_MS 1000
#endif

typedef struct ata_drive {
    uint16_t        io;        /* command block base   */
    uint16_t        ctrl;      /* control block base   */
    uint8_t         slave;     /* 0 = master, 1 = slave */
    block_device_t  bdev;
    device_t       *devnode;
} ata_drive_t;

static const driver_t ata_driver;      /* forward: see the bottom of the file */

/* Four possible drives, allocated statically: they are discovered once at boot
 * and live for the life of the machine, and a block_device_t must not move
 * (the registry links it by pointer). */
static ata_drive_t drives[4];
static int         ndrives;

/* ---------------------------------------------------------------------- */
/* 3. primitives                                                           */
/* ---------------------------------------------------------------------- */

static uint8_t ata_status(ata_drive_t *d)     { return g_io->in8(d->io + ATA_REG_STATUS); }
static uint8_t ata_alt_status(ata_drive_t *d) { return g_io->in8(d->ctrl); }

/* ~400ns, the interval the standard requires after a drive select or a command
 * write before the status register means anything. Four control-block reads is
 * the canonical way to spend it without a timer. */
static void ata_settle(ata_drive_t *d) {
    for (int i = 0; i < 4; i++) (void)ata_alt_status(d);
}

/* Wait for BSY to clear. Returns the status byte, or -1 on timeout. The
 * control block is read rather than the command block because reading the
 * command-block status has the side effect of acknowledging an interrupt. */
static int ata_wait_ready(ata_drive_t *d, uint32_t timeout_ms) {
    uint64_t start = millis();
    for (;;) {
        uint8_t st = ata_alt_status(d);
        if (!(st & ATA_SR_BSY)) return st;
        if (millis() - start >= timeout_ms) return -1;
    }
}

/* Wait for DRQ (data ready) with BSY clear. Returns 0, or a negative BLOCK_E*.
 * ERR and DF are checked on every poll: a drive that rejects a command clears
 * BSY with ERR set and never raises DRQ, so a loop that only watches DRQ turns
 * a rejected command into a timeout and loses the reason. */
static int ata_wait_drq(ata_drive_t *d, uint32_t timeout_ms) {
    uint64_t start = millis();
    for (;;) {
        uint8_t st = ata_alt_status(d);
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return BLOCK_EIO;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return BLOCK_OK;
        if (millis() - start >= timeout_ms) return BLOCK_ETIMEDOUT;
    }
}

/* Software-reset the channel. Both drives on it are reset; that is what the
 * bit does. Called only after a timeout, where the alternative is leaving the
 * channel wedged for the rest of the boot. */
static void ata_soft_reset(ata_drive_t *d) {
    g_io->out8(d->ctrl, ATA_CTL_SRST | ATA_CTL_NIEN);
    ata_settle(d);
    mdelay(2);
    g_io->out8(d->ctrl, ATA_CTL_NIEN);
    ata_settle(d);
    (void)ata_wait_ready(d, ATA_RESET_TIMEOUT_MS);
}

static void ata_report_error(ata_drive_t *d, const char *what, uint8_t st) {
    uint8_t er = g_io->in8(d->io + ATA_REG_ERROR);
    kprintf("ata: %s failed on %s (status 0x%x, error 0x%x%s%s%s)\n",
            what, d->bdev.name, st, er,
            (er & ATA_ER_UNC)  ? " uncorrectable" : "",
            (er & ATA_ER_IDNF) ? " sector-not-found" : "",
            (er & ATA_ER_ABRT) ? " aborted" : "");
}

/* Select the drive and load an LBA28 address + sector count. `count` is 1..256
 * and is written as a byte, where 0 means 256. */
static void ata_setup(ata_drive_t *d, uint64_t lba, uint32_t count) {
    g_io->out8(d->io + ATA_REG_DRIVE,
               (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
    ata_settle(d);
    g_io->out8(d->io + ATA_REG_FEATURES, 0);
    g_io->out8(d->io + ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));
    g_io->out8(d->io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    g_io->out8(d->io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    g_io->out8(d->io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
}

/* ---------------------------------------------------------------------- */
/* 4. read / write / flush                                                 */
/* ---------------------------------------------------------------------- */

/* One command's worth of sectors: 1..ATA_MAX_XFER, never crossing the device
 * end (block.c checked that) . */
static int ata_xfer(ata_drive_t *d, uint64_t lba, uint32_t count,
                    void *buf, int writing) {
    if (ata_wait_ready(d, ATA_TIMEOUT_MS) < 0) {
        kprintf("ata: %s stuck busy before command\n", d->bdev.name);
        ata_soft_reset(d);
        return BLOCK_ETIMEDOUT;
    }
    ata_setup(d, lba, count);
    g_io->out8(d->io + ATA_REG_COMMAND,
               writing ? ATA_CMD_WRITE_PIO : ATA_CMD_READ_PIO);
    ata_settle(d);

    uint8_t *p = buf;
    for (uint32_t s = 0; s < count; s++) {
        int rc = ata_wait_drq(d, ATA_TIMEOUT_MS);
        if (rc != BLOCK_OK) {
            uint8_t st = ata_status(d);
            if (rc == BLOCK_EIO) ata_report_error(d, writing ? "write" : "read", st);
            else {
                kprintf("ata: %s timed out %s lba %u\n", d->bdev.name,
                        writing ? "writing" : "reading", (unsigned)lba);
                ata_soft_reset(d);
            }
            return rc;
        }
        if (writing) g_io->out16_rep(d->io + ATA_REG_DATA, p, BLOCK_SECTOR_SIZE / 2);
        else         g_io->in16_rep(d->io + ATA_REG_DATA, p, BLOCK_SECTOR_SIZE / 2);
        p += BLOCK_SECTOR_SIZE;
        ata_settle(d);
    }

    /* The transfer is not over when the last word goes out: the drive raises
     * BSY to commit it, and only then reports whether it worked. Skipping this
     * is how a write that the drive rejected returns success. */
    int st = ata_wait_ready(d, ATA_TIMEOUT_MS);
    if (st < 0) { ata_soft_reset(d); return BLOCK_ETIMEDOUT; }
    if ((uint8_t)st & (ATA_SR_ERR | ATA_SR_DF)) {
        ata_report_error(d, writing ? "write" : "read", (uint8_t)st);
        return BLOCK_EIO;
    }
    return BLOCK_OK;
}

static int ata_bd_read(block_device_t *bd, uint64_t lba, uint32_t count, void *buf) {
    ata_drive_t *d = bd->priv;
    uint8_t *p = buf;
    while (count) {
        uint32_t n = count > ATA_MAX_XFER ? ATA_MAX_XFER : count;
        int rc = ata_xfer(d, lba, n, p, 0);
        if (rc != BLOCK_OK) return rc;
        lba += n; count -= n; p += (size_t)n * BLOCK_SECTOR_SIZE;
    }
    return BLOCK_OK;
}

static int ata_bd_write(block_device_t *bd, uint64_t lba, uint32_t count,
                        const void *buf) {
    ata_drive_t *d = bd->priv;
    const uint8_t *p = buf;
    while (count) {
        uint32_t n = count > ATA_MAX_XFER ? ATA_MAX_XFER : count;
        /* The cast drops const for one shared transfer routine; ata_xfer with
         * writing=1 only ever reads through it. */
        int rc = ata_xfer(d, lba, n, (void *)(uintptr_t)p, 1);
        if (rc != BLOCK_OK) return rc;
        lba += n; count -= n; p += (size_t)n * BLOCK_SECTOR_SIZE;
    }
    return BLOCK_OK;
}

static int ata_bd_flush(block_device_t *bd) {
    ata_drive_t *d = bd->priv;
    if (ata_wait_ready(d, ATA_TIMEOUT_MS) < 0) { ata_soft_reset(d); return BLOCK_ETIMEDOUT; }
    g_io->out8(d->io + ATA_REG_DRIVE, (uint8_t)(0xE0 | (d->slave << 4)));
    ata_settle(d);
    g_io->out8(d->io + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    ata_settle(d);
    int st = ata_wait_ready(d, ATA_TIMEOUT_MS);
    if (st < 0) { ata_soft_reset(d); return BLOCK_ETIMEDOUT; }
    if ((uint8_t)st & (ATA_SR_ERR | ATA_SR_DF)) {
        ata_report_error(d, "flush", (uint8_t)st);
        return BLOCK_EIO;
    }
    return BLOCK_OK;
}

static const block_ops_t ata_block_ops = {
    .read  = ata_bd_read,
    .write = ata_bd_write,
    .flush = ata_bd_flush,
};

/* ---------------------------------------------------------------------- */
/* 5. discovery                                                            */
/* ---------------------------------------------------------------------- */

/* IDENTIFY strings are 16-bit words with the two bytes swapped, space padded
 * and not NUL terminated. */
static void ata_copy_string(char *dst, const uint16_t *id, int first, int words,
                            size_t cap) {
    size_t n = 0;
    for (int i = 0; i < words && n + 2 < cap; i++) {
        dst[n++] = (char)(id[first + i] >> 8);
        dst[n++] = (char)(id[first + i] & 0xFF);
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\0')) n--;
    dst[n] = '\0';
}

/* Probe one (channel, drive). Returns 1 if a usable ATA disk was registered. */
static int ata_probe_drive(uint16_t io, uint16_t ctrl, int slave, int index) {
    ata_drive_t *d = &drives[ndrives];
    memset(d, 0, sizeof *d);
    d->io = io; d->ctrl = ctrl; d->slave = (uint8_t)slave;
    /* The name has to exist before anything can log about this drive. */
    d->bdev.name[0] = 'a'; d->bdev.name[1] = 't'; d->bdev.name[2] = 'a';
    d->bdev.name[3] = (char)('0' + index); d->bdev.name[4] = '\0';

    /* Interrupts off on this channel before touching anything else. */
    g_io->out8(ctrl, ATA_CTL_NIEN);

    g_io->out8(io + ATA_REG_DRIVE, (uint8_t)(0xA0 | (slave << 4)));
    ata_settle(d);

    /* A channel with no controller at all reads 0xFF forever ("floating
     * bus"). Sending IDENTIFY into that and waiting for BSY to clear is a
     * guaranteed timeout per drive, so check first. */
    uint8_t st = ata_status(d);
    if (st == 0xFF || st == 0x00) return 0;

    g_io->out8(io + ATA_REG_SECCOUNT, 0);
    g_io->out8(io + ATA_REG_LBA0, 0);
    g_io->out8(io + ATA_REG_LBA1, 0);
    g_io->out8(io + ATA_REG_LBA2, 0);
    g_io->out8(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_settle(d);

    st = ata_status(d);
    if (st == 0) return 0;                      /* nothing there */

    if (ata_wait_ready(d, ATA_PROBE_TIMEOUT_MS) < 0) return 0;

    /* An ATAPI or SATA device answers IDENTIFY with a signature in the LBA
     * mid/high registers instead of data. Neither speaks the command set
     * below, so leave them alone and say so. */
    uint8_t sig_mid  = g_io->in8(io + ATA_REG_LBA1);
    uint8_t sig_high = g_io->in8(io + ATA_REG_LBA2);
    if (sig_mid != 0 || sig_high != 0) {
        kprintf("ata: %s is not an ATA disk (signature %x%x) - skipped\n",
                d->bdev.name, sig_high, sig_mid);
        return 0;
    }

    if (ata_wait_drq(d, ATA_PROBE_TIMEOUT_MS) != BLOCK_OK) {
        kprintf("ata: %s answered IDENTIFY with no data - skipped\n", d->bdev.name);
        return 0;
    }

    uint16_t id[256];
    g_io->in16_rep(io + ATA_REG_DATA, id, 256);

    uint64_t sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    /* Word 83 bit 10 says the drive supports 48-bit addressing, in which case
     * words 100..103 hold the real capacity. This driver only issues LBA28
     * commands, so the number is read purely to notice that the disk is bigger
     * than it can reach and to say so. */
    uint64_t lba48 = 0;
    if (id[83] & (1u << 10))
        lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16)
              | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    if (lba48 > sectors) sectors = lba48;
    if (sectors == 0) {
        kprintf("ata: %s reports zero capacity - skipped\n", d->bdev.name);
        return 0;
    }
    if (sectors > ATA_LBA28_MAX) {
        kprintf("ata: %s is %u MiB but this driver is LBA28 - clamped to 128 GiB\n",
                d->bdev.name, (unsigned)(sectors / 2048));
        sectors = ATA_LBA28_MAX;
    }

    ata_copy_string(d->bdev.model, id, 27, 20, sizeof d->bdev.model);
    d->bdev.sectors     = sectors;
    d->bdev.sector_size = BLOCK_SECTOR_SIZE;
    d->bdev.has_cache   = 1;               /* hence ata_bd_flush */
    d->bdev.ops         = &ata_block_ops;
    d->bdev.priv        = d;

    int rc = block_register(&d->bdev);
    if (rc != BLOCK_OK) {
        kprintf("ata: %s could not be registered (%s)\n",
                d->bdev.name, block_strerror(rc));
        return 0;
    }

    kprintf("ata: %s %u MiB (%u sectors) PIO LBA28 at 0x%x%s: %s\n",
            d->bdev.name, (unsigned)(sectors / 2048), (unsigned)sectors,
            io, slave ? " slave" : " master", d->bdev.model);

    /* Publish it into the device model, bound to this driver: an unbound
     * storage device in the boot tree reads as hardware nobody is driving,
     * which is exactly the signal the model uses to decide it should write a
     * driver for something. */
    d->devnode = device_create(d->bdev.name, DEV_CLASS_STORAGE);
    if (d->devnode) {
        device_add_resource(d->devnode, RES_IOPORT, io, 8);
        device_register(d->devnode);
        device_bind_driver(d->devnode, (driver_t *)&ata_driver);
        device_set_state(d->devnode, DEV_STATE_ACTIVE);
        d->bdev.dev = d->devnode;
    }
    ndrives++;
    return 1;
}

static int ata_init(void) {
    static const uint16_t chan_io[2]   = { 0x1F0, 0x170 };
    static const uint16_t chan_ctrl[2] = { 0x3F6, 0x376 };

    ndrives = 0;
    int found = 0;
    for (int c = 0; c < 2; c++)
        for (int s = 0; s < 2; s++)
            found += ata_probe_drive(chan_io[c], chan_ctrl[c], s, c * 2 + s);

    if (!found)
        kputs("ata: no ATA disks found (nothing will survive a reboot)\n");
    return 0;
}

/* No probe/remove/suspend/resume hooks. Discovery happens once in init() and
 * the disks then live for the life of the machine: there is no hot-plug, and a
 * suspend hook that spun a disk down would have to be paired with a resume
 * that spun it back up before the next flush, which is a promise this polled
 * driver cannot keep today. An absent hook the manager skips is honest; an
 * empty one that returns 0 claims the work was done. */
static const driver_t ata_driver = {
    .name  = "ata",
    .level = DRV_LEVEL_DEVICE,
    .cls   = DEV_CLASS_STORAGE,
    .init  = ata_init,
};
REGISTER_DRIVER(ata_driver);
