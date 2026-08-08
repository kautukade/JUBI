/* fat_vol.c — the FAT32 volume: sector I/O, the allocation table, the
 * allocator, mount-time validation, and mkfs. See fs/fat/fat.h.
 *
 * Nothing in this file knows what a file is. It deals in sectors, clusters and
 * chains, and it is where every "is this image actually a FAT32 volume"
 * question is answered.
 */

#include "fat.h"
#include "kernel.h"
#include <string.h>

/* One shared page of zeros, for zeroing a freshly allocated cluster and for
 * wiping the reserved area at format time. */
static const uint8_t fat_zero_sector[BLOCK_SECTOR_SIZE];

/* ====================================================================== */
/* 1. sector I/O and the one-sector read cache                            */
/* ====================================================================== */

static void cache_invalidate_range(fat_vol_t *v, uint32_t sec, uint32_t n) {
    if (v->cache_valid && v->cache_sec >= sec && v->cache_sec < sec + n)
        v->cache_valid = 0;
}

int fat_sector_read(fat_vol_t *v, uint32_t sec, const uint8_t **out) {
    if (v->cache_valid && v->cache_sec == sec) { *out = v->cache; return VFS_OK; }
    int rc = block_read(v->bd, sec, 1, v->cache);
    if (rc != BLOCK_OK) {
        /* The cache must not keep bytes from a read that failed: the next
         * caller would get the previous sector's contents under this sector's
         * number, which is the "silently returns the wrong data" failure that
         * include/block.h exists to prevent. */
        v->cache_valid = 0;
        kprintf("fat: read of sector %u failed (%s)\n",
                (unsigned)sec, block_strerror(rc));
        return VFS_EINVAL;
    }
    v->cache_sec   = sec;
    v->cache_valid = 1;
    *out = v->cache;
    return VFS_OK;
}

int fat_sector_read_into(fat_vol_t *v, uint32_t sec, void *dst) {
    const uint8_t *p;
    int rc = fat_sector_read(v, sec, &p);
    if (rc != VFS_OK) return rc;
    memcpy(dst, p, BLOCK_SECTOR_SIZE);
    return VFS_OK;
}

int fat_sector_write(fat_vol_t *v, uint32_t sec, const void *buf) {
    if (v->read_only) return VFS_EINVAL;
    int rc = block_write(v->bd, sec, 1, buf);
    if (rc != BLOCK_OK) {
        cache_invalidate_range(v, sec, 1);
        kprintf("fat: write of sector %u failed (%s)\n",
                (unsigned)sec, block_strerror(rc));
        return VFS_EINVAL;
    }
    /* Keep the cache coherent rather than dropping it: a read-modify-write of
     * consecutive fields in one sector is the common case. */
    if (v->cache_valid && v->cache_sec == sec)
        memcpy(v->cache, buf, BLOCK_SECTOR_SIZE);
    return VFS_OK;
}

int fat_sectors_read(fat_vol_t *v, uint32_t sec, uint32_t n, void *dst) {
    if (n == 0) return VFS_OK;
    int rc = block_read(v->bd, sec, n, dst);
    if (rc != BLOCK_OK) {
        kprintf("fat: read of %u sectors at %u failed (%s)\n",
                (unsigned)n, (unsigned)sec, block_strerror(rc));
        return VFS_EINVAL;
    }
    return VFS_OK;
}

int fat_sectors_write(fat_vol_t *v, uint32_t sec, uint32_t n, const void *src) {
    if (v->read_only) return VFS_EINVAL;
    if (n == 0) return VFS_OK;
    cache_invalidate_range(v, sec, n);
    int rc = block_write(v->bd, sec, n, src);
    if (rc != BLOCK_OK) {
        kprintf("fat: write of %u sectors at %u failed (%s)\n",
                (unsigned)n, (unsigned)sec, block_strerror(rc));
        return VFS_EINVAL;
    }
    return VFS_OK;
}

int fat_barrier(fat_vol_t *v) {
    int rc = block_flush(v->bd);
    if (rc != BLOCK_OK) {
        kprintf("fat: flush failed (%s)\n", block_strerror(rc));
        return VFS_EINVAL;
    }
    return VFS_OK;
}

/* ====================================================================== */
/* 2. clusters and the allocation table                                   */
/* ====================================================================== */

int fat_clus_valid(const fat_vol_t *v, uint32_t clus) {
    return clus >= 2 && clus < v->clusters + 2;
}

uint32_t fat_clus_sector(const fat_vol_t *v, uint32_t clus) {
    return v->data_start + (clus - 2) * v->sec_per_clus;
}

int fat_get(fat_vol_t *v, uint32_t clus, uint32_t *val) {
    if (!fat_clus_valid(v, clus)) return VFS_EINVAL;
    uint32_t byte = clus * 4;
    uint32_t sec  = v->reserved_sec + byte / BLOCK_SECTOR_SIZE;
    uint32_t off  = byte % BLOCK_SECTOR_SIZE;
    const uint8_t *p;
    int rc = fat_sector_read(v, sec, &p);
    if (rc != VFS_OK) return rc;
    *val = fat_rd32(p + off) & FAT_MASK;
    return VFS_OK;
}

int fat_set(fat_vol_t *v, uint32_t clus, uint32_t val) {
    if (!fat_clus_valid(v, clus)) return VFS_EINVAL;
    uint32_t byte = clus * 4;
    uint32_t off  = byte % BLOCK_SECTOR_SIZE;
    uint32_t sec0 = v->reserved_sec + byte / BLOCK_SECTOR_SIZE;

    uint8_t buf[BLOCK_SECTOR_SIZE];
    int rc = fat_sector_read_into(v, sec0, buf);
    if (rc != VFS_OK) return rc;
    /* The top four bits of a FAT32 entry are reserved and must be preserved. */
    uint32_t cur = fat_rd32(buf + off);
    fat_wr32(buf + off, (cur & 0xF0000000u) | (val & FAT_MASK));

    /* Every FAT copy gets the same bytes. A mirror that drifts is a volume
     * some other operating system will "repair" by picking the wrong one. */
    for (uint32_t i = 0; i < v->num_fats; i++) {
        rc = fat_sector_write(v, sec0 + i * v->fat_sectors, buf);
        if (rc != VFS_OK) return rc;
    }
    return VFS_OK;
}

int fat_chain_next(fat_vol_t *v, uint32_t clus, uint32_t *next) {
    uint32_t val;
    int rc = fat_get(v, clus, &val);
    if (rc != VFS_OK) return rc;
    if (val >= FAT_EOC) { *next = 0; return VFS_OK; }
    if (val < 2 || val >= v->clusters + 2) {
        /* A chain pointing outside the volume is corruption. Refusing is the
         * point: following it would read some other file's data, or the FAT
         * itself, and hand it back as file contents. */
        kprintf("fat: chain from cluster %u points at %u, outside the volume\n",
                (unsigned)clus, (unsigned)val);
        return VFS_EINVAL;
    }
    *next = val;
    return VFS_OK;
}

static int fat_zero_cluster(fat_vol_t *v, uint32_t clus) {
    uint32_t sec = fat_clus_sector(v, clus);
    for (uint32_t i = 0; i < v->sec_per_clus; i++) {
        int rc = fat_sector_write(v, sec + i, fat_zero_sector);
        if (rc != VFS_OK) return rc;
    }
    return VFS_OK;
}

int fat_alloc_cluster(fat_vol_t *v, uint32_t *out) {
    if (v->read_only) return VFS_ENOSPC;
    uint32_t start = v->alloc_hint;
    if (!fat_clus_valid(v, start)) start = 2;

    for (uint32_t n = 0; n < v->clusters; n++) {
        uint32_t c = start + n;
        if (c >= v->clusters + 2) c -= v->clusters;    /* wrap to 2 */
        uint32_t val;
        int rc = fat_get(v, c, &val);
        if (rc != VFS_OK) return rc;
        if (val != FAT_FREE) continue;

        /* Zero BEFORE claiming it. A crash in between leaves a free cluster
         * with zeros in it, which costs nothing; claiming first and crashing
         * would leave a cluster that is marked in use and full of whatever the
         * last file that owned it wrote. */
        rc = fat_zero_cluster(v, c);
        if (rc != VFS_OK) return rc;
        rc = fat_set(v, c, FAT_EOC_WRITE);
        if (rc != VFS_OK) return rc;

        v->alloc_hint = (c + 1 >= v->clusters + 2) ? 2 : c + 1;
        *out = c;
        return VFS_OK;
    }
    return VFS_ENOSPC;
}

int fat_free_chain(fat_vol_t *v, uint32_t first) {
    if (first == 0) return VFS_OK;
    uint32_t c = first;
    /* Bounded by the number of clusters on the volume: a corrupt FAT can
     * contain a loop, and an unbounded walk on this kernel (no preemption, no
     * watchdog) is an unrecoverable hang. */
    for (uint32_t guard = 0; guard <= v->clusters; guard++) {
        if (!fat_clus_valid(v, c)) return VFS_OK;
        uint32_t val;
        int rc = fat_get(v, c, &val);
        if (rc != VFS_OK) return rc;
        rc = fat_set(v, c, FAT_FREE);
        if (rc != VFS_OK) return rc;
        /* Freed space is the cheapest place to start looking next time. */
        if (c < v->alloc_hint) v->alloc_hint = c;
        if (val >= FAT_EOC || val < 2) return VFS_OK;
        c = val;
    }
    kprintf("fat: chain from cluster %u loops; stopped freeing it\n",
            (unsigned)first);
    return VFS_EINVAL;
}

/* ====================================================================== */
/* 3. mount-time validation                                              */
/* ====================================================================== */

/* Offsets into the boot sector (BPB + the FAT32 extension). Spelled out rather
 * than expressed as a packed struct: the layout is unaligned, and a name next
 * to a number is easier to check against the specification than a field in a
 * struct whose padding depends on the compiler. */
#define BPB_BytsPerSec  11
#define BPB_SecPerClus  13
#define BPB_RsvdSecCnt  14
#define BPB_NumFATs     16
#define BPB_RootEntCnt  17
#define BPB_TotSec16    19
#define BPB_Media       21
#define BPB_FATSz16     22
#define BPB_SecPerTrk   24
#define BPB_NumHeads    26
#define BPB_HiddSec     28
#define BPB_TotSec32    32
#define BPB_FATSz32     36
#define BPB_ExtFlags    40
#define BPB_FSVer       42
#define BPB_RootClus    44
#define BPB_FSInfo      48
#define BPB_BkBootSec   50
#define BS_DrvNum       64
#define BS_BootSig      66
#define BS_VolID        67
#define BS_VolLab       71
#define BS_FilSysType   82

static int reject(const char *why) {
    kprintf("fat: not mounting: %s\n", why);
    return VFS_EINVAL;
}

int fat_volume_open(fat_vol_t *v, block_device_t *bd) {
    memset(v, 0, sizeof *v);
    v->bd = bd;

    uint8_t bs[BLOCK_SECTOR_SIZE];
    int rc = block_read(bd, 0, 1, bs);
    if (rc != BLOCK_OK) {
        kprintf("fat: cannot read sector 0 of %s (%s)\n",
                bd->name, block_strerror(rc));
        return VFS_EINVAL;
    }

    if (fat_rd16(bs + 510) != 0xAA55)
        return reject("no 0x55AA boot signature");

    uint32_t bps      = fat_rd16(bs + BPB_BytsPerSec);
    uint32_t spc      = bs[BPB_SecPerClus];
    uint32_t reserved = fat_rd16(bs + BPB_RsvdSecCnt);
    uint32_t nfats    = bs[BPB_NumFATs];
    uint32_t rootent  = fat_rd16(bs + BPB_RootEntCnt);
    uint32_t tot16    = fat_rd16(bs + BPB_TotSec16);
    uint32_t fatsz16  = fat_rd16(bs + BPB_FATSz16);
    uint32_t tot32    = fat_rd32(bs + BPB_TotSec32);
    uint32_t fatsz32  = fat_rd32(bs + BPB_FATSz32);
    uint32_t fsver    = fat_rd16(bs + BPB_FSVer);
    uint32_t rootclus = fat_rd32(bs + BPB_RootClus);
    uint32_t fsinfo   = fat_rd16(bs + BPB_FSInfo);

    if (bps != BLOCK_SECTOR_SIZE)
        return reject("sector size is not 512 bytes");
    if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128)
        return reject("sectors-per-cluster is not a power of two in 1..128");
    if (reserved == 0)
        return reject("zero reserved sectors");
    if (nfats == 0 || nfats > 4)
        return reject("implausible number of FATs");
    /* These three are what tell FAT32 apart from FAT12/FAT16. A FAT16 volume
     * has a fixed-size root directory and a 16-bit FAT size, and parsing one
     * as FAT32 would compute a data area that starts in the middle of the root
     * directory. */
    if (rootent != 0 || fatsz16 != 0 || tot16 != 0)
        return reject("this is a FAT12/FAT16 volume, not FAT32");
    if (fsver != 0)
        return reject("unknown FAT32 version");
    if (fatsz32 == 0)
        return reject("zero-length FAT");
    if (tot32 == 0)
        return reject("volume claims zero sectors");
    if (tot32 > bd->sectors)
        return reject("volume claims more sectors than the disk has");

    /* Overflow-safe: each term is bounded above by tot32, which is bounded by
     * the device size. */
    if (nfats > (0xFFFFFFFFu - reserved) / fatsz32)
        return reject("FAT layout overflows the volume");
    uint32_t data_start = reserved + nfats * fatsz32;
    if (data_start >= tot32)
        return reject("no data area: the FATs fill the volume");

    uint32_t clusters = (tot32 - data_start) / spc;
    if (clusters < 1)
        return reject("volume holds no clusters");
    if (rootclus < 2 || rootclus >= clusters + 2)
        return reject("root directory cluster is outside the volume");
    /* The FAT has to be able to describe every cluster it claims exist. */
    if ((uint64_t)fatsz32 * (BLOCK_SECTOR_SIZE / 4) < (uint64_t)clusters + 2)
        return reject("the FAT is too small for the cluster count");

    v->sec_per_clus   = spc;
    v->bytes_per_clus = spc * BLOCK_SECTOR_SIZE;
    v->reserved_sec   = reserved;
    v->num_fats       = nfats;
    v->fat_sectors    = fatsz32;
    v->total_sectors  = tot32;
    v->data_start     = data_start;
    v->clusters       = clusters;
    v->root_clus      = rootclus;
    v->fsinfo_sec     = (fsinfo != 0 && fsinfo != 0xFFFF && fsinfo < reserved)
                        ? fsinfo : 0;
    v->alloc_hint     = 2;
    v->read_only      = bd->read_only;

    /* Last check, and the one the BPB cannot fake: FAT[0] holds the media
     * descriptor in its low byte with the rest set. An image whose BPB was
     * copied from somewhere but whose FAT was never written fails here. */
    const uint8_t *fatsec;
    if (fat_sector_read(v, reserved, &fatsec) != VFS_OK)
        return reject("cannot read the FAT");
    uint32_t e0 = fat_rd32(fatsec) & FAT_MASK;
    uint32_t e1 = fat_rd32(fatsec + 4) & FAT_MASK;
    if ((e0 & 0x0FFFFF00u) != 0x0FFFFF00u || (e0 & 0xFF) < 0xF0)
        return reject("FAT[0] is not a media descriptor - the image is not "
                      "a formatted FAT32 volume");
    if (e1 < FAT_EOC)
        return reject("FAT[1] is not an end-of-chain marker");

    /* FSInfo's free count is advisory and is NOT trusted: a count left stale
     * by an unclean shutdown would make the allocator hand out a cluster that
     * is in use. Only the "next free" hint is taken, and only as a hint that
     * is verified against the FAT before anything is allocated. */
    if (v->fsinfo_sec) {
        const uint8_t *fi;
        if (fat_sector_read(v, v->fsinfo_sec, &fi) == VFS_OK &&
            fat_rd32(fi) == 0x41615252u && fat_rd32(fi + 484) == 0x61417272u) {
            uint32_t hint = fat_rd32(fi + 492);
            if (fat_clus_valid(v, hint)) v->alloc_hint = hint;
        }
    }

    char label[12];
    memcpy(label, bs + BS_VolLab, 11);
    label[11] = '\0';
    for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = '\0';

    kprintf("fat: %s FAT32, %u MiB, %u clusters of %u bytes, label \"%s\"%s\n",
            bd->name,
            (unsigned)((uint64_t)tot32 * BLOCK_SECTOR_SIZE / (1024 * 1024)),
            (unsigned)clusters, (unsigned)v->bytes_per_clus, label,
            v->read_only ? ", read-only" : "");
    if (clusters < 65525)
        kprintf("fat: note: %u clusters is below the 65525 FAT32 minimum; "
                "other systems may refuse this volume\n", (unsigned)clusters);
    return VFS_OK;
}

/* ====================================================================== */
/* 4. mkfs                                                               */
/* ====================================================================== */

int fat_device_is_blank(block_device_t *bd, uint32_t nsectors) {
    uint8_t buf[BLOCK_SECTOR_SIZE];
    if (nsectors > bd->sectors) nsectors = (uint32_t)bd->sectors;
    for (uint32_t s = 0; s < nsectors; s++) {
        if (block_read(bd, s, 1, buf) != BLOCK_OK) return 0;
        for (int i = 0; i < BLOCK_SECTOR_SIZE; i++)
            if (buf[i] != 0) return 0;
    }
    return 1;
}

/* Microsoft's own size table, which is what every other implementation
 * expects to see. Small volumes get 512-byte clusters so that a test-sized
 * image still has enough clusters to be a legal FAT32. */
static uint32_t choose_spc(uint64_t sectors) {
    uint64_t mb = sectors / 2048;
    if (mb <= 260)   return 1;
    if (mb <= 8192)  return 8;
    if (mb <= 16384) return 16;
    if (mb <= 32768) return 32;
    return 64;
}

int fat32_format(block_device_t *bd, const char *label) {
    if (!bd) return VFS_EINVAL;
    if (bd->read_only) {
        kprintf("fat: cannot format %s: it is read-only\n", bd->name);
        return VFS_EINVAL;
    }

    uint64_t total64 = bd->sectors;
    if (total64 > 0x0FFFFFFFull) total64 = 0x0FFFFFFFull;   /* LBA28 ceiling */
    uint32_t total = (uint32_t)total64;

    const uint32_t reserved = 32;
    const uint32_t nfats    = 2;
    uint32_t spc = choose_spc(total);

    if (total <= reserved + nfats + spc * 2) {
        kprintf("fat: %s is too small to hold a FAT32 volume\n", bd->name);
        return VFS_EINVAL;
    }

    /* Solve for a FAT big enough to describe the clusters that are left once
     * the FAT itself is subtracted. Converges upward in a handful of steps;
     * the guard is there so a pathological size cannot spin forever. */
    uint32_t avail = total - reserved;
    uint32_t fatsz = 1, clusters = 0;
    for (int guard = 0; guard < 64; guard++) {
        if (avail <= nfats * fatsz) { clusters = 0; break; }
        clusters = (avail - nfats * fatsz) / spc;
        uint32_t need = ((clusters + 2) * 4 + BLOCK_SECTOR_SIZE - 1) / BLOCK_SECTOR_SIZE;
        if (need <= fatsz) break;
        fatsz = need;
    }
    if (clusters < 2) {
        kprintf("fat: %s is too small to hold a FAT32 volume\n", bd->name);
        return VFS_EINVAL;
    }

    uint32_t data_start = reserved + nfats * fatsz;

    /* Wipe the reserved area and both FATs. Everything else (the data area) is
     * left alone: the FAT says it is free, and zeroing 128 MiB through PIO to
     * prove it would cost half a minute of boot. A cluster's contents are
     * zeroed when it is allocated (fat_alloc_cluster), so no stale bytes can
     * reach a file. */
    uint32_t chunk_sectors = 64;
    uint8_t *zeros = kcalloc(chunk_sectors, BLOCK_SECTOR_SIZE);
    if (!zeros) return VFS_ENOSPC;
    for (uint32_t s = 0; s < data_start; s += chunk_sectors) {
        uint32_t n = data_start - s;
        if (n > chunk_sectors) n = chunk_sectors;
        int rc = block_write(bd, s, n, zeros);
        if (rc != BLOCK_OK) {
            kprintf("fat: format failed wiping sector %u (%s)\n",
                    (unsigned)s, block_strerror(rc));
            kfree(zeros);
            return VFS_EINVAL;
        }
    }
    /* And the first data cluster, which becomes the root directory. */
    for (uint32_t i = 0; i < spc; i += chunk_sectors) {
        uint32_t n = spc - i;
        if (n > chunk_sectors) n = chunk_sectors;
        if (block_write(bd, data_start + i, n, zeros) != BLOCK_OK) {
            kfree(zeros);
            return VFS_EINVAL;
        }
    }
    kfree(zeros);

    uint8_t sec[BLOCK_SECTOR_SIZE];
    memset(sec, 0, sizeof sec);

    /* Boot sector. The jump and the OEM name matter: some tools refuse a
     * volume whose first byte is not a jump instruction. */
    sec[0] = 0xEB; sec[1] = 0x58; sec[2] = 0x90;
    memcpy(sec + 3, "MSWIN4.1", 8);
    fat_wr16(sec + BPB_BytsPerSec, BLOCK_SECTOR_SIZE);
    sec[BPB_SecPerClus] = (uint8_t)spc;
    fat_wr16(sec + BPB_RsvdSecCnt, (uint16_t)reserved);
    sec[BPB_NumFATs] = (uint8_t)nfats;
    fat_wr16(sec + BPB_RootEntCnt, 0);
    fat_wr16(sec + BPB_TotSec16, 0);
    sec[BPB_Media] = 0xF8;
    fat_wr16(sec + BPB_FATSz16, 0);
    fat_wr16(sec + BPB_SecPerTrk, 63);
    fat_wr16(sec + BPB_NumHeads, 255);
    fat_wr32(sec + BPB_HiddSec, 0);
    fat_wr32(sec + BPB_TotSec32, total);
    fat_wr32(sec + BPB_FATSz32, fatsz);
    fat_wr16(sec + BPB_ExtFlags, 0);
    fat_wr16(sec + BPB_FSVer, 0);
    fat_wr32(sec + BPB_RootClus, 2);
    fat_wr16(sec + BPB_FSInfo, 1);
    fat_wr16(sec + BPB_BkBootSec, 6);
    sec[BS_DrvNum]  = 0x80;
    sec[BS_BootSig] = 0x29;
    /* A volume id that differs between two images made a second apart, without
     * inventing a random source: the wall clock if there is one, the boot
     * clock if there is not. */
    fat_wr32(sec + BS_VolID, (uint32_t)millis() ^ 0x54414C4Bu);
    memset(sec + BS_VolLab, ' ', 11);
    {
        const char *l = label ? label : "FABLEOS";
        for (int i = 0; i < 11 && l[i]; i++) {
            char c = l[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            sec[BS_VolLab + i] = (uint8_t)c;
        }
    }
    memcpy(sec + BS_FilSysType, "FAT32   ", 8);
    fat_wr16(sec + 510, 0xAA55);

    if (block_write(bd, 0, 1, sec) != BLOCK_OK) return VFS_EINVAL;
    if (block_write(bd, 6, 1, sec) != BLOCK_OK) return VFS_EINVAL;  /* backup */

    /* FSInfo. free_count is written as "unknown" on purpose — see fat.h. */
    memset(sec, 0, sizeof sec);
    fat_wr32(sec + 0,   0x41615252u);
    fat_wr32(sec + 484, 0x61417272u);
    fat_wr32(sec + 488, 0xFFFFFFFFu);        /* free count: unknown */
    fat_wr32(sec + 492, 3);                  /* next free hint      */
    fat_wr32(sec + 508, 0xAA550000u);
    if (block_write(bd, 1, 1, sec) != BLOCK_OK) return VFS_EINVAL;
    if (block_write(bd, 7, 1, sec) != BLOCK_OK) return VFS_EINVAL;

    /* FAT[0] = media descriptor, FAT[1] = end of chain, FAT[2] = the root
     * directory's single cluster, terminated. */
    memset(sec, 0, sizeof sec);
    fat_wr32(sec + 0, 0x0FFFFFF8u);
    fat_wr32(sec + 4, 0x0FFFFFFFu);
    fat_wr32(sec + 8, FAT_EOC_WRITE);
    for (uint32_t i = 0; i < nfats; i++)
        if (block_write(bd, reserved + i * fatsz, 1, sec) != BLOCK_OK)
            return VFS_EINVAL;

    /* A volume-label entry in the root directory, so the operator's file
     * manager shows a name instead of "Untitled". */
    memset(sec, 0, sizeof sec);
    memset(sec, ' ', 11);
    {
        const char *l = label ? label : "FABLEOS";
        for (int i = 0; i < 11 && l[i]; i++) {
            char c = l[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            sec[i] = (uint8_t)c;
        }
    }
    sec[11] = FAT_ATTR_VOLUME_ID;
    {
        uint16_t d, t;
        fat_now(&d, &t);
        fat_wr16(sec + 22, t);
        fat_wr16(sec + 24, d);
    }
    if (block_write(bd, data_start, 1, sec) != BLOCK_OK) return VFS_EINVAL;

    if (block_flush(bd) != BLOCK_OK) {
        kprintf("fat: format wrote everything but the flush failed\n");
        return VFS_EINVAL;
    }

    kprintf("fat: formatted %s as FAT32: %u clusters of %u bytes, "
            "%u-sector FAT x%u\n", bd->name, (unsigned)clusters,
            (unsigned)(spc * BLOCK_SECTOR_SIZE), (unsigned)fatsz,
            (unsigned)nfats);
    return VFS_OK;
}
