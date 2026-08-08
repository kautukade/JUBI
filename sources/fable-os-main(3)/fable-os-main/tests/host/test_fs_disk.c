/* test_fs_disk.c — persistence, on the host, against a synthetic disk.
 *
 * WHAT THIS SUITE IS FOR
 *   A filesystem is the one subsystem where a bug is not a crash but a quiet
 *   loss of the operator's data, discovered a week later. Booting QEMU proves a
 *   happy path; it does not prove what happens when the power goes at write
 *   number 37. So the whole stack — block layer, FAT32, and the ATA driver's
 *   register protocol — runs here against backends this file controls
 *   completely, and the interesting cases are the ugly ones:
 *
 *     1. FORMAT produces something that is genuinely FAT32 on the bytes, not
 *        merely something this code can read back.
 *     2. WRITE / READ / DIRECTORIES / LONG NAMES round-trip.
 *     3. PERSISTENCE: everything is dropped, the volume is mounted again from
 *        the same bytes, and the files are still there. That is the whole
 *        point of the branch, so it is tested as a state change, not a claim.
 *     4. UNCLEAN SHUTDOWN: writes stop being applied part-way through, exactly
 *        as they would if QEMU were killed. What must survive is stated in
 *        fs/fat/fat.h and is asserted here, including the case where the crash
 *        lands in the middle of a directory sector.
 *     5. A FULL DISK returns VFS_ENOSPC and leaves a mountable volume.
 *     6. CORRUPT IMAGES ARE REJECTED. Six shapes of broken superblock, each
 *        of which a trusting parser would happily "mount" and then write over.
 *     7. A FAILED READ IS AN ERROR, NEVER ZEROS — asserted against a synthetic
 *        ATA drive that raises ERR, because a disk that silently returns zeros
 *        is worse than one that fails.
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

#include "block.h"
#include "vfs.h"
#include "fs.h"
#include "device.h"
#include "driver.h"
#include "kernel.h"
#include "../../fs/fat/fat.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ====================================================================== */
/* 1. the synthetic disk                                                  */
/* ====================================================================== */

/* 40 MiB: the smallest round size that still yields more than the 65525
 * clusters a legal FAT32 volume needs, so the images this suite makes are the
 * same shape as the one an operator mounts on their Mac. */
#define BIG_SECTORS   (40u * 1024 * 1024 / 512)
#define SMALL_SECTORS (2u * 1024 * 1024 / 512)

typedef struct {
    uint8_t *bytes;
    uint64_t sectors;
    int      writes;          /* sector-writes applied so far        */
    int      reads;
    int      flushes;
    int      power_off_at;    /* stop applying writes at this count  */
    int      tear_at;         /* apply only half of THIS write, then power off */
    int      powered;         /* 0 = the plug has been pulled        */
    int64_t  fail_read_lba;   /* -1 = never                          */
} ramdisk_t;

static ramdisk_t   rd;
static block_device_t bdev;

static int rd_read(block_device_t *bd, uint64_t lba, uint32_t count, void *buf) {
    ramdisk_t *d = bd->priv;
    d->reads += (int)count;
    if (d->fail_read_lba >= 0 && (uint64_t)d->fail_read_lba >= lba &&
        (uint64_t)d->fail_read_lba < lba + count)
        return BLOCK_EIO;                 /* and buf is left untouched */
    memcpy(buf, d->bytes + lba * BLOCK_SECTOR_SIZE,
           (size_t)count * BLOCK_SECTOR_SIZE);
    return BLOCK_OK;
}

static int rd_write(block_device_t *bd, uint64_t lba, uint32_t count,
                    const void *buf) {
    ramdisk_t *d = bd->priv;
    const uint8_t *src = buf;
    for (uint32_t i = 0; i < count; i++) {
        d->writes++;
        if (!d->powered) continue;        /* the OS is told it worked  */
        if (d->tear_at > 0 && d->writes == d->tear_at) {
            /* A sector half-written: the failure a filesystem is not allowed
             * to assume away. */
            memcpy(d->bytes + (lba + i) * BLOCK_SECTOR_SIZE,
                   src + (size_t)i * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE / 2);
            d->powered = 0;
            continue;
        }
        if (d->power_off_at > 0 && d->writes >= d->power_off_at) {
            d->powered = 0;
            continue;
        }
        memcpy(d->bytes + (lba + i) * BLOCK_SECTOR_SIZE,
               src + (size_t)i * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE);
    }
    return BLOCK_OK;
}

static int rd_flush(block_device_t *bd) {
    ((ramdisk_t *)bd->priv)->flushes++;
    return BLOCK_OK;
}

static const block_ops_t rd_ops = { rd_read, rd_write, rd_flush };

static void disk_make(uint32_t sectors) {
    free(rd.bytes);
    memset(&rd, 0, sizeof rd);
    rd.bytes   = calloc(sectors, BLOCK_SECTOR_SIZE);
    rd.sectors = sectors;
    rd.powered = 1;
    rd.fail_read_lba = -1;
    if (!rd.bytes) { printf("out of host memory\n"); exit(2); }

    memset(&bdev, 0, sizeof bdev);
    strncpy(bdev.name, "ram0", BLOCK_NAME_MAX);
    strncpy(bdev.model, "synthetic test disk", sizeof bdev.model - 1);
    bdev.sectors     = sectors;
    bdev.sector_size = BLOCK_SECTOR_SIZE;
    bdev.has_cache   = 1;
    bdev.ops         = &rd_ops;
    bdev.priv        = &rd;
}

/* Bring the filesystem up on the current disk bytes, exactly as a boot would.
 * Called again after a simulated reboot: nothing is carried over but the
 * bytes. */
static int boot_fs(int allow_format) {
    vfs_init();
    block_registry_reset();
    CHECK_EQ(block_register(&bdev), BLOCK_OK);
    CHECK_EQ(fat32_register(), VFS_OK);
    return fat32_mount_disk("/", &bdev, allow_format);
}

/* Pull the plug: no unmount, no flush, no goodbye. */
static void reboot(void) {
    rd.powered      = 1;
    rd.power_off_at = 0;
    rd.tear_at      = 0;
    CHECK_EQ(boot_fs(0), VFS_OK);
}

static int write_file(const char *path, const char *text) {
    file_t *f = vfs_open(path, O_CREAT | O_RDWR | O_TRUNC);
    if (!f) return -1;
    int64_t n = vfs_write(f, text, strlen(text));
    vfs_close(f);
    return (int)n;
}

/* Read a whole file into `buf`. Returns bytes read, or -1 if it would not
 * open. */
static int read_file(const char *path, char *buf, size_t cap) {
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) return -1;
    int64_t n = vfs_read(f, buf, cap - 1);
    vfs_close(f);
    if (n < 0) return (int)n;
    buf[n] = '\0';
    return (int)n;
}

/* ====================================================================== */
/* 2. the block layer                                                     */
/* ====================================================================== */

static int null_read(block_device_t *bd, uint64_t l, uint32_t c, void *b) {
    (void)bd; (void)l; (void)c; (void)b; return BLOCK_OK;
}
static const block_ops_t null_ops = { null_read, NULL, NULL };

static void test_block_registry_rejects_malformed(void) {
    block_registry_reset();
    block_device_t d;

    memset(&d, 0, sizeof d);
    strncpy(d.name, "x0", BLOCK_NAME_MAX);
    d.sectors = 10; d.sector_size = 512;
    CHECK_EQ(block_register(&d), BLOCK_EINVAL);      /* no ops */

    d.ops = &null_ops;
    d.sector_size = 4096;
    CHECK_EQ(block_register(&d), BLOCK_EINVAL);      /* not 512 */

    d.sector_size = 512;
    d.sectors = 0;
    CHECK_EQ(block_register(&d), BLOCK_EINVAL);      /* no capacity */

    d.sectors = 10;
    d.name[0] = '\0';
    CHECK_EQ(block_register(&d), BLOCK_EINVAL);      /* no name */

    strncpy(d.name, "x0", BLOCK_NAME_MAX);
    CHECK_EQ(block_register(&d), BLOCK_OK);
    CHECK_EQ(block_count(), 1);

    block_device_t dup = d;
    dup.next = NULL;
    CHECK_EQ(block_register(&dup), BLOCK_EINVAL);    /* duplicate name */
    CHECK_EQ(block_count(), 1);
    CHECK(block_find("x0") == &d);
    CHECK(block_find("nope") == NULL);
    CHECK(block_get(0) == &d);
    CHECK(block_get(1) == NULL);
    block_registry_reset();
}

static void test_block_io_is_bounds_checked(void) {
    disk_make(64);
    block_registry_reset();
    CHECK_EQ(block_register(&bdev), BLOCK_OK);

    uint8_t buf[BLOCK_SECTOR_SIZE];
    CHECK_EQ(block_read(&bdev, 0, 0, buf), BLOCK_EINVAL);      /* zero count */
    CHECK_EQ(block_read(&bdev, 0, 1, NULL), BLOCK_EINVAL);     /* no buffer  */
    CHECK_EQ(block_read(&bdev, 64, 1, buf), BLOCK_ENOSPC);     /* past end   */
    CHECK_EQ(block_read(&bdev, 63, 2, buf), BLOCK_ENOSPC);     /* runs off   */
    /* The classic: a count large enough to wrap lba+count back into range. */
    CHECK_EQ(block_read(&bdev, 32, 0xFFFFFFFFu, buf), BLOCK_ENOSPC);
    CHECK_EQ(block_read(&bdev, 0, 1, buf), BLOCK_OK);

    bdev.read_only = 1;
    CHECK_EQ(block_write(&bdev, 0, 1, buf), BLOCK_EROFS);
    bdev.read_only = 0;
    CHECK_EQ(block_write(&bdev, 0, 1, buf), BLOCK_OK);
    CHECK(bdev.errors >= 5);
    CHECK_EQ((int)bdev.writes, 1);
    block_registry_reset();
}

/* ====================================================================== */
/* 3. the image really is FAT32                                           */
/* ====================================================================== */

static void test_format_writes_a_real_fat32_superblock(void) {
    disk_make(BIG_SECTORS);
    block_registry_reset();
    block_register(&bdev);
    CHECK_EQ(fat32_format(&bdev, "FABLEOS"), VFS_OK);

    const uint8_t *bs = rd.bytes;
    CHECK_EQ(fat_rd16(bs + 510), 0xAA55);
    CHECK_EQ(bs[0], 0xEB);                                  /* a jump */
    CHECK_EQ(fat_rd16(bs + 11), 512);                       /* bytes/sector */
    CHECK_EQ(fat_rd16(bs + 17), 0);                         /* FAT32: no root ent */
    CHECK_EQ(fat_rd16(bs + 22), 0);                         /* FAT32: FATSz16 = 0 */
    CHECK_EQ(fat_rd16(bs + 19), 0);                         /* FAT32: TotSec16 = 0 */
    CHECK_EQ(fat_rd32(bs + 32), BIG_SECTORS);
    CHECK_EQ(fat_rd32(bs + 44), 2);                         /* root cluster */
    CHECK_EQ(bs[16], 2);                                    /* two FATs */
    CHECK(memcmp(bs + 82, "FAT32   ", 8) == 0);
    CHECK(memcmp(bs + 71, "FABLEOS", 6) == 0);

    /* The backup boot sector other systems fall back to. */
    CHECK(memcmp(rd.bytes, rd.bytes + 6 * 512, 512) == 0);

    /* FSInfo, with the free count declared UNKNOWN on purpose. */
    CHECK_EQ(fat_rd32(rd.bytes + 512), 0x41615252u);
    CHECK_EQ(fat_rd32(rd.bytes + 512 + 484), 0x61417272u);
    CHECK_EQ(fat_rd32(rd.bytes + 512 + 488), 0xFFFFFFFFu);

    /* Both FAT copies start with the media descriptor and an end marker. */
    uint32_t reserved = fat_rd16(bs + 14), fatsz = fat_rd32(bs + 36);
    const uint8_t *fat0 = rd.bytes + (size_t)reserved * 512;
    const uint8_t *fat1 = fat0 + (size_t)fatsz * 512;
    CHECK_EQ(fat_rd32(fat0) & 0x0FFFFFFFu, 0x0FFFFFF8u);
    CHECK_EQ(fat_rd32(fat0 + 4) & 0x0FFFFFFFu, 0x0FFFFFFFu);
    CHECK(fat_rd32(fat0 + 8) >= 0x0FFFFFF8u);               /* root terminated */
    CHECK(memcmp(fat0, fat1, 12) == 0);

    /* And enough clusters to be a legal FAT32 volume rather than a FAT16 one
     * wearing the wrong hat. */
    uint32_t data_start = reserved + 2 * fatsz;
    uint32_t clusters = (BIG_SECTORS - data_start) / bs[13];
    CHECK(clusters >= 65525);
}

/* ====================================================================== */
/* 4. files, directories, names                                           */
/* ====================================================================== */

static void test_create_write_read(void) {
    disk_make(BIG_SECTORS);
    CHECK_EQ(boot_fs(1), VFS_OK);        /* blank disk: formats itself */

    CHECK_EQ(write_file("/hello.txt", "the machine wrote this"), 22);

    char buf[128];
    CHECK_EQ(read_file("/hello.txt", buf, sizeof buf), 22);
    CHECK_STR(buf, "the machine wrote this");

    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/hello.txt", &st), VFS_OK);
    CHECK_EQ((int)st.size, 22);
    CHECK_EQ(st.type, VNODE_FILE);

    /* Overwrite shorter, via O_TRUNC. */
    CHECK_EQ(write_file("/hello.txt", "short"), 5);
    CHECK_EQ(read_file("/hello.txt", buf, sizeof buf), 5);
    CHECK_STR(buf, "short");

    /* Append. */
    file_t *f = vfs_open("/hello.txt", O_RDWR | O_APPEND);
    CHECK(f != NULL);
    CHECK_EQ((int)vfs_write(f, "er", 2), 2);
    vfs_close(f);
    CHECK_EQ(read_file("/hello.txt", buf, sizeof buf), 7);
    CHECK_STR(buf, "shorter");

    /* A file bigger than one cluster and one sector, written in one go. */
    static char big[9000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)('a' + (i % 26));
    f = vfs_open("/big.bin", O_CREAT | O_RDWR);
    CHECK(f != NULL);
    CHECK_EQ((int)vfs_write(f, big, sizeof big), (int)sizeof big);
    vfs_close(f);

    static char back[9000];
    f = vfs_open("/big.bin", O_RDONLY);
    CHECK(f != NULL);
    CHECK_EQ((int)vfs_read(f, back, sizeof back), (int)sizeof big);
    CHECK(memcmp(big, back, sizeof big) == 0);
    /* Read from the middle, across a cluster boundary. */
    vfs_seek(f, 4090, SEEK_SET);
    char mid[32];
    CHECK_EQ((int)vfs_read(f, mid, 32), 32);
    CHECK(memcmp(mid, big + 4090, 32) == 0);
    vfs_close(f);
}

static void test_directories(void) {
    disk_make(BIG_SECTORS);
    CHECK_EQ(boot_fs(1), VFS_OK);

    CHECK_EQ(vfs_mkdir("/notes"), VFS_OK);
    CHECK_EQ(vfs_mkdir("/notes"), VFS_EEXIST);
    CHECK_EQ(vfs_mkdir("/notes/2026"), VFS_OK);
    CHECK_EQ(write_file("/notes/2026/plan.txt", "build a disk driver"), 19);

    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/notes", &st), VFS_OK);
    CHECK_EQ(st.type, VNODE_DIR);
    CHECK_EQ(vfs_stat("/notes/2026/plan.txt", &st), VFS_OK);
    CHECK_EQ((int)st.size, 19);

    /* "." and ".." are on the disk (other systems require them) and are
     * deliberately not visible or resolvable through the VFS. */
    char name[64];
    int seen_dot = 0, seen_2026 = 0, n = 0;
    for (uint32_t i = 0; vfs_readdir("/notes", i, name, sizeof name) == VFS_OK; i++) {
        n++;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) seen_dot = 1;
        if (strcmp(name, "2026") == 0) seen_2026 = 1;
    }
    CHECK_EQ(n, 1);
    CHECK_EQ(seen_dot, 0);
    CHECK_EQ(seen_2026, 1);
    CHECK(vfs_stat("/notes/..", &st) != VFS_OK);
    CHECK(vfs_stat("/notes/2026/../..", &st) != VFS_OK);

    /* A directory with something in it cannot be unlinked; an empty one can,
     * and the unlink goes through the same vnode op the tools use. */
    file_t *rootf = vfs_open("/", O_RDONLY);
    CHECK(rootf != NULL);
    CHECK(rootf->node->ops->unlink(rootf->node, "notes") != VFS_OK);
    CHECK_EQ(vfs_mkdir("/empty"), VFS_OK);
    CHECK_EQ(rootf->node->ops->unlink(rootf->node, "empty"), VFS_OK);
    vfs_close(rootf);
    CHECK_EQ(vfs_stat("/empty", &st), VFS_ENOENT);
    CHECK_EQ(vfs_stat("/notes/2026/plan.txt", &st), VFS_OK);

    reboot();
    CHECK_EQ(vfs_stat("/notes/2026/plan.txt", &st), VFS_OK);
    CHECK_EQ((int)st.size, 19);
    CHECK_EQ(vfs_stat("/empty", &st), VFS_ENOENT);
}

static void test_long_names_and_case(void) {
    disk_make(BIG_SECTORS);
    CHECK_EQ(boot_fs(1), VFS_OK);

    const char *lname = "the-audio-driver-the-model-wrote.dvm";
    CHECK_EQ(write_file("/the-audio-driver-the-model-wrote.dvm", "PROGRAM"), 7);

    char name[64];
    CHECK_EQ(vfs_readdir("/", 0, name, sizeof name), VFS_OK);
    CHECK_STR(name, lname);

    /* FAT is case-insensitive and case-preserving: the same file, and a
     * second create is a collision rather than a second entry. */
    char buf[32];
    CHECK_EQ(read_file("/THE-AUDIO-DRIVER-THE-MODEL-WROTE.DVM", buf, sizeof buf), 7);
    CHECK_STR(buf, "PROGRAM");

    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/The-Audio-Driver-The-Model-Wrote.dvm", &st), VFS_OK);

    /* An 8.3 name needs no long-name records at all; a lowercase one does. */
    CHECK_EQ(write_file("/README.TXT", "plain"), 5);
    CHECK_EQ(write_file("/readme.md", "lower"), 5);
    CHECK_EQ(read_file("/README.TXT", buf, sizeof buf), 5);
    CHECK_STR(buf, "plain");
    CHECK_EQ(read_file("/readme.md", buf, sizeof buf), 5);
    CHECK_STR(buf, "lower");

    /* Many names sharing a prefix: every one needs a distinct 8.3 alias, and
     * the alias generator must not give up after "~4". */
    char path[80];
    for (int i = 0; i < 40; i++) {
        snprintf(path, sizeof path, "/generated-driver-%d.program", i);
        CHECK_EQ(write_file(path, "x"), 1);
    }
    for (int i = 0; i < 40; i++) {
        snprintf(path, sizeof path, "/generated-driver-%d.program", i);
        CHECK_EQ(read_file(path, buf, sizeof buf), 1);
    }

    /* An entry written by Linux or macOS: an all-lowercase 8.3 name stored
     * with NO long-name records and the two case bits set in the reserved
     * byte instead. Reported as "readme.txt", not "README.TXT" — a name the
     * operator did not choose is a name they will not recognise.
     * (Verified against a real image: macOS writes exactly this for a
     * directory called "drivers".) */
    for (size_t off = 0; off + 32 < (size_t)BIG_SECTORS * BLOCK_SECTOR_SIZE; off += 32) {
        if (memcmp(rd.bytes + off, "README  TXT", 11) == 0 &&
            rd.bytes[off + 11] != FAT_ATTR_LFN) {
            rd.bytes[off + 12] = 0x18;          /* base + extension lowercase */
            break;
        }
    }
    reboot();
    int saw_lower = 0;
    for (uint32_t i = 0; vfs_readdir("/", i, name, sizeof name) == VFS_OK; i++)
        if (strcmp(name, "readme.txt") == 0) saw_lower = 1;
    CHECK_EQ(saw_lower, 1);
    CHECK_EQ(read_file("/readme.txt", buf, sizeof buf), 5);
    CHECK_STR(buf, "plain");

    /* A name at exactly the VFS limit round-trips; one byte longer does not. */
    char maxname[VFS_PATH_MAX];
    maxname[0] = '/';
    memset(maxname + 1, 'n', VFS_NAME_MAX);
    maxname[1 + VFS_NAME_MAX] = '\0';
    CHECK_EQ(write_file(maxname, "edge"), 4);
    CHECK_EQ(read_file(maxname, buf, sizeof buf), 4);
}

static void test_hostile_names_are_refused(void) {
    disk_make(BIG_SECTORS);
    CHECK_EQ(boot_fs(1), VFS_OK);

    /* Model output. None of these may create anything, and none may crash. */
    CHECK(vfs_open("/name-with-a-trailing-dot.", O_CREAT | O_RDWR) == NULL);
    CHECK(vfs_open("/trailing space ", O_CREAT | O_RDWR) == NULL);
    CHECK(vfs_open("/a:b", O_CREAT | O_RDWR) == NULL);
    CHECK(vfs_open("/a*b", O_CREAT | O_RDWR) == NULL);
    CHECK(vfs_open("/a\"b", O_CREAT | O_RDWR) == NULL);
    CHECK(vfs_open("/a|b", O_CREAT | O_RDWR) == NULL);
    CHECK(vfs_open("/\x01ctrl", O_CREAT | O_RDWR) == NULL);
    /* Invalid UTF-8, and a 4-byte codepoint FAT's UCS-2 cannot hold. */
    CHECK(vfs_open("/bad\xC3", O_CREAT | O_RDWR) == NULL);
    CHECK(vfs_open("/bad\xF0\x9F\x92\xA9", O_CREAT | O_RDWR) == NULL);
    /* Over-long component: the VFS rejects it before the filesystem sees it. */
    char toolong[VFS_PATH_MAX + 8];
    toolong[0] = '/';
    memset(toolong + 1, 'x', VFS_NAME_MAX + 1);
    toolong[VFS_NAME_MAX + 2] = '\0';
    CHECK(vfs_open(toolong, O_CREAT | O_RDWR) == NULL);

    /* Nothing was created by any of that. */
    char name[64];
    CHECK_EQ(vfs_readdir("/", 0, name, sizeof name), VFS_ENOENT);

    /* Two-byte UTF-8 IS storable, and comes back byte-identical. */
    CHECK_EQ(write_file("/caf\xC3\xA9.txt", "ok"), 2);
    CHECK_EQ(vfs_readdir("/", 0, name, sizeof name), VFS_OK);
    CHECK_STR(name, "caf\xC3\xA9.txt");
}

static void test_unlink_and_truncate(void) {
    disk_make(BIG_SECTORS);
    CHECK_EQ(boot_fs(1), VFS_OK);

    static char big[20000];
    memset(big, 'Z', sizeof big);
    file_t *f = vfs_open("/tmp.bin", O_CREAT | O_RDWR);
    CHECK(f != NULL);
    CHECK_EQ((int)vfs_write(f, big, sizeof big), (int)sizeof big);
    vfs_close(f);

    /* Shrink, and check the bytes past the new end are unreachable. */
    f = vfs_open("/tmp.bin", O_RDWR | O_TRUNC);
    CHECK(f != NULL);
    vfs_close(f);
    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/tmp.bin", &st), VFS_OK);
    CHECK_EQ((int)st.size, 0);
    char buf[64];
    CHECK_EQ(read_file("/tmp.bin", buf, sizeof buf), 0);

    /* Write past the end: the hole must read back as zeros, never as the
     * bytes that used to live in those clusters. */
    f = vfs_open("/tmp.bin", O_RDWR);
    CHECK(f != NULL);
    vfs_seek(f, 5000, SEEK_SET);
    CHECK_EQ((int)vfs_write(f, "tail", 4), 4);
    vfs_close(f);
    static char hole[5100];
    f = vfs_open("/tmp.bin", O_RDONLY);
    CHECK(f != NULL);
    CHECK_EQ((int)vfs_read(f, hole, sizeof hole), 5004);
    vfs_close(f);
    int zeros = 1;
    for (int i = 0; i < 5000; i++) if (hole[i] != 0) zeros = 0;
    CHECK_EQ(zeros, 1);
    CHECK(memcmp(hole + 5000, "tail", 4) == 0);

    /* A hole this filesystem refuses to fill is refused, not attempted. */
    f = vfs_open("/tmp.bin", O_RDWR);
    CHECK(f != NULL);
    vfs_seek(f, 200u * 1024 * 1024, SEEK_SET);
    CHECK(vfs_write(f, "x", 1) < 0);
    vfs_close(f);

    /* Unlink through the tool-visible path, then confirm it is gone and its
     * space came back. */
    CHECK_EQ(write_file("/gone.txt", "bye"), 3);
    CHECK_EQ(vfs_stat("/gone.txt", &st), VFS_OK);
    /* vfs.h exposes no unlink, so go through the vnode op the tools use. */
    file_t *dir = vfs_open("/", O_RDONLY);
    CHECK(dir != NULL);
    CHECK_EQ(dir->node->ops->unlink(dir->node, "gone.txt"), VFS_OK);
    vfs_close(dir);
    CHECK_EQ(vfs_stat("/gone.txt", &st), VFS_ENOENT);

    reboot();
    CHECK_EQ(vfs_stat("/gone.txt", &st), VFS_ENOENT);
    CHECK_EQ(vfs_stat("/tmp.bin", &st), VFS_OK);
    CHECK_EQ((int)st.size, 5004);
}

/* ====================================================================== */
/* 5. THE DELIVERABLE: it is still there after a reboot                   */
/* ====================================================================== */

static void test_survives_a_reboot(void) {
    disk_make(BIG_SECTORS);
    CHECK_EQ(boot_fs(1), VFS_OK);

    CHECK_EQ(vfs_mkdir("/drivers"), VFS_OK);
    CHECK_EQ(write_file("/drivers/ac97.dvm", "OUT 0x18 0x02\nHALT\n"), 19);
    CHECK_EQ(write_file("/notes.txt", "I found an unclaimed AC97 codec."), 32);

    static char big[70000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7 + i / 251);
    file_t *f = vfs_open("/blob.bin", O_CREAT | O_RDWR);
    CHECK(f != NULL);
    CHECK_EQ((int)vfs_write(f, big, sizeof big), (int)sizeof big);
    vfs_close(f);

    /* --- power off, power on. Nothing in RAM carries over. --- */
    reboot();

    char buf[128];
    CHECK_EQ(read_file("/notes.txt", buf, sizeof buf), 32);
    CHECK_STR(buf, "I found an unclaimed AC97 codec.");
    CHECK_EQ(read_file("/drivers/ac97.dvm", buf, sizeof buf), 19);
    CHECK_STR(buf, "OUT 0x18 0x02\nHALT\n");

    static char back[70000];
    f = vfs_open("/blob.bin", O_RDONLY);
    CHECK(f != NULL);
    CHECK_EQ((int)vfs_read(f, back, sizeof back), (int)sizeof big);
    CHECK(memcmp(big, back, sizeof big) == 0);
    vfs_close(f);

    /* And it can still be added to after the reboot. */
    CHECK_EQ(write_file("/drivers/second.dvm", "MORE"), 4);
    reboot();
    CHECK_EQ(read_file("/drivers/second.dvm", buf, sizeof buf), 4);
    CHECK_EQ(read_file("/drivers/ac97.dvm", buf, sizeof buf), 19);
}

/* ====================================================================== */
/* 6. unclean shutdown                                                    */
/* ====================================================================== */

/* Cut the power in the middle of writing a second file, then mount again. The
 * contract from fs/fat/fat.h: the file that was already committed is intact,
 * the volume mounts, and the half-written file is either absent or at a size
 * whose bytes were all really written. */
static void test_unclean_shutdown_keeps_committed_files(void) {
    for (int cut = 4; cut <= 40; cut += 4) {
        disk_make(BIG_SECTORS);
        CHECK_EQ(boot_fs(1), VFS_OK);
        CHECK_EQ(write_file("/keep.txt", "committed before the crash"), 26);

        static char payload[30000];
        memset(payload, 'W', sizeof payload);
        rd.power_off_at = rd.writes + cut;

        file_t *f = vfs_open("/victim.bin", O_CREAT | O_RDWR);
        if (f) { (void)vfs_write(f, payload, sizeof payload); vfs_close(f); }

        reboot();

        char buf[64];
        CHECK_EQ(read_file("/keep.txt", buf, sizeof buf), 26);
        CHECK_STR(buf, "committed before the crash");

        /* The victim: absent, or present with every byte it claims. */
        vfs_stat_t st;
        if (vfs_stat("/victim.bin", &st) == VFS_OK) {
            static char got[30000];
            file_t *g = vfs_open("/victim.bin", O_RDONLY);
            CHECK(g != NULL);
            int64_t n = vfs_read(g, got, sizeof got);
            vfs_close(g);
            CHECK_EQ((int)n, (int)st.size);
            int all_w = 1;
            for (int64_t i = 0; i < n; i++) if (got[i] != 'W') all_w = 0;
            CHECK_EQ(all_w, 1);
        }
        /* And the volume is still usable afterwards. */
        CHECK_EQ(write_file("/after.txt", "still works"), 11);
    }
}

/* A sector that was only half written — the failure a filesystem may not
 * assume away. The volume must still mount and the committed file must still
 * read, whatever the torn sector was part of. */
static void test_torn_sector_does_not_destroy_the_volume(void) {
    for (int cut = 3; cut <= 30; cut += 3) {
        disk_make(BIG_SECTORS);
        CHECK_EQ(boot_fs(1), VFS_OK);
        CHECK_EQ(write_file("/keep.txt", "committed"), 9);
        CHECK_EQ(vfs_mkdir("/d"), VFS_OK);

        rd.tear_at = rd.writes + cut;
        char path[32];
        snprintf(path, sizeof path, "/d/torn-%d.txt", cut);
        (void)write_file(path, "half of this may be lost");

        reboot();

        char buf[64];
        CHECK_EQ(read_file("/keep.txt", buf, sizeof buf), 9);
        CHECK_STR(buf, "committed");

        /* Whatever survived of the torn file, reading the directory must not
         * produce a name that cannot then be opened. */
        char name[64];
        for (uint32_t i = 0; vfs_readdir("/d", i, name, sizeof name) == VFS_OK; i++) {
            char p2[VFS_PATH_MAX];
            snprintf(p2, sizeof p2, "/d/%s", name);
            vfs_stat_t st;
            CHECK_EQ(vfs_stat(p2, &st), VFS_OK);
        }
        CHECK_EQ(write_file("/d/after.txt", "still works"), 11);
    }
}

/* ====================================================================== */
/* 7. a full disk                                                         */
/* ====================================================================== */

static void test_full_disk_reports_enospc(void) {
    disk_make(SMALL_SECTORS);
    block_registry_reset();
    block_register(&bdev);
    CHECK_EQ(fat32_format(&bdev, "SMALL"), VFS_OK);
    CHECK_EQ(boot_fs(0), VFS_OK);

    CHECK_EQ(write_file("/first.txt", "before the disk filled up"), 25);

    static char chunk[16384];
    memset(chunk, 'F', sizeof chunk);
    file_t *f = vfs_open("/filler.bin", O_CREAT | O_RDWR);
    CHECK(f != NULL);
    int64_t total = 0, n;
    int hit_enospc = 0;
    for (int i = 0; i < 4096; i++) {
        n = vfs_write(f, chunk, sizeof chunk);
        if (n < 0) { CHECK_EQ((int)n, VFS_ENOSPC); hit_enospc = 1; break; }
        total += n;
    }
    vfs_close(f);
    CHECK_EQ(hit_enospc, 1);
    CHECK(total > 1024 * 1024);            /* it really did fill the disk */

    /* A full disk refuses new files with an error, and does not corrupt. */
    CHECK(write_file("/nope.bin", "x") <= 0 || vfs_stat("/nope.bin", NULL) != VFS_OK);

    reboot();
    char buf[64];
    CHECK_EQ(read_file("/first.txt", buf, sizeof buf), 25);
    CHECK_STR(buf, "before the disk filled up");
    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/filler.bin", &st), VFS_OK);
    CHECK_EQ((int)st.size, (int)total);

    /* Deleting the filler gives the space back. */
    file_t *dir = vfs_open("/", O_RDONLY);
    CHECK(dir != NULL);
    CHECK_EQ(dir->node->ops->unlink(dir->node, "filler.bin"), VFS_OK);
    vfs_close(dir);
    f = vfs_open("/again.bin", O_CREAT | O_RDWR);
    CHECK(f != NULL);
    CHECK_EQ((int)vfs_write(f, chunk, sizeof chunk), (int)sizeof chunk);
    vfs_close(f);
}

/* ====================================================================== */
/* 8. corrupt images are rejected, not parsed                             */
/* ====================================================================== */

static void expect_rejected(const char *what) {
    kcap_reset();
    int rc = boot_fs(0);
    if (rc == VFS_OK) {
        th_checks++; th_fails++;
        printf("    FAIL mounted a %s image that should have been rejected\n", what);
        return;
    }
    th_checks++;
    /* And it said why, so an operator is not left guessing. */
    CHECK_CONTAINS(kcap_text(), "fat: ");
}

static void test_corrupt_images_are_rejected(void) {
    /* All zeros: no signature at all. */
    disk_make(BIG_SECTORS);
    expect_rejected("blank");

    /* Random bytes with a valid signature: everything else is nonsense. */
    disk_make(BIG_SECTORS);
    unsigned seed = 12345;
    for (int i = 0; i < 4096; i++) {
        seed = seed * 1103515245u + 12345u;
        rd.bytes[i] = (uint8_t)(seed >> 16);
    }
    rd.bytes[510] = 0x55; rd.bytes[511] = 0xAA;
    expect_rejected("random");

    /* A well-formed FAT16 superblock. A parser that only checks the signature
     * computes a data area starting inside the root directory. */
    disk_make(BIG_SECTORS);
    {
        uint8_t *b = rd.bytes;
        b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
        fat_wr16(b + 11, 512); b[13] = 4; fat_wr16(b + 14, 1); b[16] = 2;
        fat_wr16(b + 17, 512);              /* root entries: FAT16 */
        fat_wr16(b + 22, 200);              /* FATSz16: FAT16      */
        fat_wr32(b + 32, BIG_SECTORS);
        fat_wr16(b + 510, 0xAA55);
    }
    expect_rejected("FAT16");

    /* FAT32 shape, but the volume claims more sectors than the disk holds —
     * a truncated image. */
    disk_make(BIG_SECTORS);
    block_registry_reset(); block_register(&bdev);
    CHECK_EQ(fat32_format(&bdev, "T"), VFS_OK);
    fat_wr32(rd.bytes + 32, BIG_SECTORS * 4);
    expect_rejected("truncated");

    /* FAT32 shape, but the FAT was never written (all zeros): FAT[0] is not a
     * media descriptor. This is the check the BPB cannot fake. */
    disk_make(BIG_SECTORS);
    block_registry_reset(); block_register(&bdev);
    CHECK_EQ(fat32_format(&bdev, "T"), VFS_OK);
    {
        uint32_t reserved = fat_rd16(rd.bytes + 14);
        uint32_t fatsz    = fat_rd32(rd.bytes + 36);
        memset(rd.bytes + (size_t)reserved * 512, 0, (size_t)fatsz * 512);
    }
    expect_rejected("unformatted-FAT");

    /* FAT32 shape, but sectors-per-cluster is not a power of two. */
    disk_make(BIG_SECTORS);
    block_registry_reset(); block_register(&bdev);
    CHECK_EQ(fat32_format(&bdev, "T"), VFS_OK);
    rd.bytes[13] = 3;
    expect_rejected("bad-spc");

    /* A disk whose sector 0 cannot even be read. */
    disk_make(BIG_SECTORS);
    block_registry_reset(); block_register(&bdev);
    CHECK_EQ(fat32_format(&bdev, "T"), VFS_OK);
    rd.fail_read_lba = 0;
    expect_rejected("unreadable");
    rd.fail_read_lba = -1;

    /* None of the above may have written anything: a rejected image is left
     * exactly as it was found. That is what makes a mistake recoverable. */
    disk_make(BIG_SECTORS);
    rd.bytes[510] = 0x55; rd.bytes[511] = 0xAA;
    int writes_before = rd.writes;
    (void)boot_fs(0);
    CHECK_EQ(rd.writes, writes_before);
}

/* A non-blank disk holding something unrecognised is NOT reformatted, even
 * when formatting is allowed. */
static void test_unrecognised_disk_is_not_erased(void) {
    disk_make(BIG_SECTORS);
    memset(rd.bytes, 0x77, 8 * BLOCK_SECTOR_SIZE);
    kcap_reset();
    CHECK(boot_fs(1) != VFS_OK);
    CHECK_CONTAINS(kcap_text(), "leaving it alone");
    for (int i = 0; i < 8 * BLOCK_SECTOR_SIZE; i++)
        if (rd.bytes[i] != 0x77) { CHECK_EQ(i, -1); break; }

    /* A blank one IS formatted, which is what makes `make run` just work. */
    disk_make(BIG_SECTORS);
    kcap_reset();
    CHECK_EQ(boot_fs(1), VFS_OK);
    CHECK_CONTAINS(kcap_text(), "is blank - formatting it");
    CHECK_EQ(write_file("/proof.txt", "formatted on first boot"), 23);
}

/* ====================================================================== */
/* 9. the ATA driver, against a synthetic drive                           */
/* ====================================================================== */

#define ATA_TIMEOUT_MS       30
#define ATA_PROBE_TIMEOUT_MS 30
#define ATA_RESET_TIMEOUT_MS 30
#undef  REGISTER_DRIVER
#define REGISTER_DRIVER(var) extern const driver_t *const __drvptr_unused_##var
#include "../../drivers/block/ata.c"

/* A drive on the primary channel, master. Everything else on both channels is
 * a floating bus (0xFF), which is what an empty IDE controller looks like. */
#define SIM_SECTORS 64
static struct {
    int      present;
    uint8_t  reg[8];
    uint8_t  status;
    uint8_t  error;
    uint8_t  bytes[SIM_SECTORS * BLOCK_SECTOR_SIZE];
    uint16_t xfer[256];
    int      xfer_pos;          /* words consumed of the current sector */
    int      remaining;         /* sectors left in the command          */
    uint64_t lba;
    int      writing;
    int      flushes;
    int      resets;
    int      fail_next;         /* raise ERR on the next command        */
    int      fail_mid;          /* raise ERR after this many sectors    */
    int      hang;              /* the drive stops answering entirely   */
} sim;

static void sim_load_sector(void) {
    memcpy(sim.xfer, sim.bytes + sim.lba * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE);
    sim.xfer_pos = 0;
}

static void sim_identify(void) {
    memset(sim.xfer, 0, sizeof sim.xfer);
    sim.xfer[0] = 0x0040;
    /* Model string: 40 bytes, space padded, two bytes per word with the pair
     * swapped — exactly the layout the standard specifies and the driver has
     * to undo. */
    char model[41];
    memset(model, ' ', 40);
    model[40] = '\0';
    memcpy(model, "FABLEOS SYNTHETIC DISK", sizeof "FABLEOS SYNTHETIC DISK" - 1);
    for (int i = 0; i < 20; i++)
        sim.xfer[27 + i] = (uint16_t)(((uint8_t)model[i * 2] << 8)
                                      | (uint8_t)model[i * 2 + 1]);
    sim.xfer[60] = SIM_SECTORS;
    sim.xfer[61] = 0;
    sim.xfer_pos = 0;
    sim.remaining = 1;
}

/* Only the master exists. A slave-selected command block answers 0 (the
 * "nothing here" status), which is what a real controller does for a drive
 * that is not plugged in. */
static int sim_selected_slave(void) { return (sim.reg[ATA_REG_DRIVE] & 0x10) != 0; }

static uint8_t sim_in8(uint16_t port) {
    if (port == 0x1F0 + ATA_REG_STATUS || port == 0x3F6) {
        if (!sim.present) return 0xFF;
        if (sim.hang) return ATA_SR_BSY;          /* never finishes */
        if (sim_selected_slave()) return 0x00;
        return sim.status;
    }
    if (port == 0x1F0 + ATA_REG_ERROR) return sim.error;
    if (port >= 0x1F0 && port <= 0x1F7) return sim.reg[port - 0x1F0];
    return 0xFF;                                  /* the other channel */
}

static void sim_out8(uint16_t port, uint8_t v) {
    if (port == 0x3F6) {
        if (v & ATA_CTL_SRST) {
            sim.resets++;
            sim.remaining = 0;
            if (!sim.hang) sim.status = ATA_SR_RDY;
        }
        return;
    }
    if (port < 0x1F0 || port > 0x1F7) return;
    if (!sim.present || sim.hang) return;
    if (port != 0x1F0 + ATA_REG_COMMAND) { sim.reg[port - 0x1F0] = v; return; }
    if (sim_selected_slave()) return;             /* no such drive */

    sim.error = 0;
    if (sim.fail_next) {
        sim.fail_next = 0;
        sim.status = ATA_SR_RDY | ATA_SR_ERR;
        sim.error  = 0x40;                        /* uncorrectable */
        return;
    }
    sim.lba = ((uint64_t)(sim.reg[ATA_REG_DRIVE] & 0x0F) << 24)
            | ((uint64_t)sim.reg[ATA_REG_LBA2] << 16)
            | ((uint64_t)sim.reg[ATA_REG_LBA1] << 8)
            | sim.reg[ATA_REG_LBA0];
    int count = sim.reg[ATA_REG_SECCOUNT];
    if (count == 0) count = 256;

    switch (v) {
        case ATA_CMD_IDENTIFY:
            sim_identify();
            sim.writing = 0;
            sim.status = ATA_SR_RDY | ATA_SR_DRQ;
            break;
        case ATA_CMD_READ_PIO:
            sim.remaining = count;
            sim.writing   = 0;
            sim_load_sector();
            sim.status = ATA_SR_RDY | ATA_SR_DRQ;
            break;
        case ATA_CMD_WRITE_PIO:
            sim.remaining = count;
            sim.writing   = 1;
            sim.xfer_pos  = 0;
            sim.status = ATA_SR_RDY | ATA_SR_DRQ;
            break;
        case ATA_CMD_FLUSH:
            sim.flushes++;
            sim.status = ATA_SR_RDY;
            break;
        default:
            sim.status = ATA_SR_RDY | ATA_SR_ERR;
            sim.error  = 0x04;                    /* aborted */
            break;
    }
}

static void sim_sector_done(void) {
    if (sim.writing)
        memcpy(sim.bytes + sim.lba * BLOCK_SECTOR_SIZE, sim.xfer, BLOCK_SECTOR_SIZE);
    sim.lba++;
    sim.remaining--;
    if (sim.fail_mid > 0 && --sim.fail_mid == 0) {
        sim.status = ATA_SR_RDY | ATA_SR_ERR;
        sim.error  = 0x10;                        /* sector not found */
        sim.remaining = 0;
        return;
    }
    if (sim.remaining > 0) {
        if (!sim.writing) sim_load_sector();
        sim.xfer_pos = 0;
        sim.status = ATA_SR_RDY | ATA_SR_DRQ;
    } else {
        sim.status = ATA_SR_RDY;
    }
}

static void sim_in16_rep(uint16_t port, void *buf, uint32_t words) {
    if (port != 0x1F0 || !sim.present) { memset(buf, 0xFF, words * 2); return; }
    memcpy(buf, sim.xfer + sim.xfer_pos, words * 2);
    sim.xfer_pos += (int)words;
    if (sim.xfer_pos >= 256) sim_sector_done();
}

static void sim_out16_rep(uint16_t port, const void *buf, uint32_t words) {
    if (port != 0x1F0 || !sim.present) return;
    memcpy(sim.xfer + sim.xfer_pos, buf, words * 2);
    sim.xfer_pos += (int)words;
    if (sim.xfer_pos >= 256) sim_sector_done();
}

static const ata_io_ops_t sim_io = { sim_in8, sim_out8, sim_in16_rep, sim_out16_rep };

static void sim_reset(int present) {
    memset(&sim, 0, sizeof sim);
    sim.present = present;
    sim.status  = ATA_SR_RDY;
    for (size_t i = 0; i < sizeof sim.bytes; i++)
        sim.bytes[i] = (uint8_t)(i * 31 + (i >> 9));
    ata_set_io(&sim_io);
    block_registry_reset();
}

static void test_ata_finds_and_reads_a_disk(void) {
    sim_reset(1);
    CHECK_EQ(ata_init(), 0);
    CHECK_EQ(block_count(), 1);

    block_device_t *bd = block_get(0);
    CHECK(bd != NULL);
    CHECK_STR(bd->name, "ata0");
    CHECK_EQ((int)bd->sectors, SIM_SECTORS);
    CHECK_CONTAINS(bd->model, "FABLEOS SYNTHETIC DISK");

    uint8_t buf[BLOCK_SECTOR_SIZE * 3];
    CHECK_EQ(block_read(bd, 5, 3, buf), BLOCK_OK);
    CHECK(memcmp(buf, sim.bytes + 5 * BLOCK_SECTOR_SIZE, sizeof buf) == 0);

    memset(buf, 0xAB, sizeof buf);
    CHECK_EQ(block_write(bd, 10, 2, buf), BLOCK_OK);
    CHECK(memcmp(sim.bytes + 10 * BLOCK_SECTOR_SIZE, buf, 2 * BLOCK_SECTOR_SIZE) == 0);

    CHECK_EQ(block_flush(bd), BLOCK_OK);
    CHECK_EQ(sim.flushes, 1);
}

static void test_ata_reports_errors_instead_of_zeros(void) {
    sim_reset(1);
    CHECK_EQ(ata_init(), 0);
    block_device_t *bd = block_get(0);
    CHECK(bd != NULL);

    /* Four sectors' worth: the multi-sector case below really transfers into
     * all of it, and a one-sector buffer here is a stack overflow the driver
     * would be blamed for. */
    uint8_t buf[BLOCK_SECTOR_SIZE * 4];
    memset(buf, 0x5A, sizeof buf);

    /* The drive rejects the command outright. */
    sim.fail_next = 1;
    CHECK_EQ(block_read(bd, 3, 1, buf), BLOCK_EIO);
    /* The buffer was NOT quietly zero-filled, and the caller was told. */
    CHECK_EQ(buf[0], 0x5A);
    CHECK_CONTAINS(kcap_text(), "uncorrectable");

    /* The drive fails half way through a multi-sector transfer. */
    sim.fail_mid = 2;
    CHECK_EQ(block_read(bd, 0, 4, buf), BLOCK_EIO);

    /* A write the drive refuses is reported as a failure, not as success. */
    sim.fail_next = 1;
    CHECK_EQ(block_write(bd, 1, 1, buf), BLOCK_EIO);

    /* And a flush that fails is not swallowed either. */
    sim.fail_next = 1;
    CHECK_EQ(block_flush(bd), BLOCK_EIO);

    CHECK(bd->errors >= 4);

    /* After all that, the driver still works: the error path left the channel
     * usable rather than wedged. */
    CHECK_EQ(block_read(bd, 3, 1, buf), BLOCK_OK);
    CHECK(memcmp(buf, sim.bytes + 3 * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE) == 0);
}

static void test_ata_with_no_disk_registers_nothing(void) {
    sim_reset(0);
    kcap_reset();
    CHECK_EQ(ata_init(), 0);
    CHECK_EQ(block_count(), 0);
    CHECK_CONTAINS(kcap_text(), "no ATA disks found");
}

/* A drive that stops answering must time out and reset the channel, not hang
 * the machine — this kernel has no preemption and no watchdog. */
static void test_ata_times_out_on_a_dead_drive(void) {
    sim_reset(1);
    CHECK_EQ(ata_init(), 0);
    block_device_t *bd = block_get(0);
    CHECK(bd != NULL);

    uint8_t buf[BLOCK_SECTOR_SIZE];
    sim.hang = 1;                            /* busy for ever */
    int before = sim.resets;
    CHECK_EQ(block_read(bd, 0, 1, buf), BLOCK_ETIMEDOUT);
    CHECK(sim.resets > before);
    CHECK_CONTAINS(kcap_text(), "stuck busy");

    /* And once the drive comes back, so does the driver. */
    sim.hang = 0;
    sim.status = ATA_SR_RDY;
    CHECK_EQ(block_read(bd, 0, 1, buf), BLOCK_OK);
}

/* The FAT32 filesystem, on top of the real ATA driver, on top of the
 * synthetic drive: one end-to-end path with no test doubles in the middle
 * except the silicon. */
static void test_fat_on_top_of_ata(void) {
    sim_reset(1);
    CHECK_EQ(ata_init(), 0);
    block_device_t *bd = block_get(0);
    CHECK(bd != NULL);

    /* 32 KiB is a legal layout but far below the 65525 clusters FAT32 is
     * defined for, so mounting says so out loud rather than pretending. */
    kcap_reset();
    CHECK_EQ(fat32_format(bd, "TINY"), VFS_OK);
    vfs_init();
    CHECK_EQ(fat32_register(), VFS_OK);
    CHECK_EQ(fat32_mount_disk("/", bd, 0), VFS_OK);
    CHECK_CONTAINS(kcap_text(), "below the 65525");

    /* And it works: a file written through the VFS, down the FAT32 code, down
     * the block layer, through the ATA register protocol, lands in the
     * simulated drive's platter bytes and comes back. */
    CHECK_EQ(write_file("/ata.txt", "through the whole stack"), 23);
    char buf[64];
    CHECK_EQ(read_file("/ata.txt", buf, sizeof buf), 23);
    CHECK_STR(buf, "through the whole stack");

    /* Mounted again from the same platter, with nothing carried over. */
    vfs_init();
    CHECK_EQ(fat32_register(), VFS_OK);
    CHECK_EQ(fat32_mount_disk("/", bd, 0), VFS_OK);
    CHECK_EQ(read_file("/ata.txt", buf, sizeof buf), 23);
    CHECK_STR(buf, "through the whole stack");
}

/* ====================================================================== */

int main(void) {
    console_init();

    RUN(test_block_registry_rejects_malformed);
    RUN(test_block_io_is_bounds_checked);
    RUN(test_format_writes_a_real_fat32_superblock);
    RUN(test_create_write_read);
    RUN(test_directories);
    RUN(test_long_names_and_case);
    RUN(test_hostile_names_are_refused);
    RUN(test_unlink_and_truncate);
    RUN(test_survives_a_reboot);
    RUN(test_unclean_shutdown_keeps_committed_files);
    RUN(test_torn_sector_does_not_destroy_the_volume);
    RUN(test_full_disk_reports_enospc);
    RUN(test_corrupt_images_are_rejected);
    RUN(test_unrecognised_disk_is_not_erased);
    RUN(test_ata_finds_and_reads_a_disk);
    RUN(test_ata_reports_errors_instead_of_zeros);
    RUN(test_ata_with_no_disk_registers_nothing);
    RUN(test_ata_times_out_on_a_dead_drive);
    RUN(test_fat_on_top_of_ata);

    free(rd.bytes);
    return th_report("fs_disk");
}
