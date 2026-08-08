/* fat.h — FAT32 on a block device: the contract shared inside fs/fat/
 *
 * PURPOSE
 *   The filesystem that makes this machine able to keep what it builds. It
 *   plugs into the VFS through vnode_ops_t exactly like fs/native/ramfs.c, and
 *   reaches the disk only through include/block.h.
 *
 * ============================================================================
 * DECISION: FAT32, and not a bespoke format
 * ============================================================================
 *   A private on-disk format would be perhaps a third of this code and easier
 *   to make crash-safe. FAT32 was chosen anyway, for one reason that outweighs
 *   both: the operator can attach the image on their own machine
 *
 *       $ hdiutil attach -imagekey diskimage-class=CRawDiskImage disk.img
 *
 *   and READ what this kernel wrote, with tools they already trust. On a
 *   machine whose only interface is a language model — where every claim about
 *   what happened arrives as prose — an independent way to check the bytes is
 *   worth a lot more than a few hundred lines. It also means the operator can
 *   put files IN, which is how anything gets onto this machine that did not
 *   come over TLS.
 *
 *   The costs are real and are paid in this file: no journal, no atomic
 *   rename, 8.3 names with a long-name overlay, case-insensitive lookup, and a
 *   4 GiB file ceiling.
 *
 * ============================================================================
 * DECISION: what "survives an unclean shutdown" actually means here
 * ============================================================================
 *   This machine reboots on a fault and gets killed by QEMU, so the question
 *   is not whether it crashes but what a crash costs. FAT32 has no journal, so
 *   the guarantee comes from WRITE ORDER plus the fact that a 512-byte sector
 *   write is atomic on the medium. Every mutation is ordered so that the LAST
 *   write is the one that publishes the change:
 *
 *     create    the long-name entries and the short entry are written as ONE
 *               sector (fat_dir_add refuses to split a set across a sector
 *               boundary), so a file either exists with its whole name or does
 *               not exist at all.
 *     write     new clusters are zeroed, marked end-of-chain and linked; the
 *               data is written; a flush is issued; and ONLY THEN is the
 *               directory entry's size field updated. A directory entry
 *               therefore never covers bytes that have not reached the disk.
 *               Note there is no flush per cluster: nothing can read a cluster
 *               the committed size does not cover, so the ordering that
 *               matters is "everything, then flush, then the size".
 *     truncate  the new (smaller) size is committed first, then the chain is
 *               trimmed. A chain longer than the size is harmless; a size
 *               longer than the chain would not be.
 *     unlink    the directory entry is erased and flushed BEFORE the clusters
 *               are freed, so no live entry can ever point into free space
 *               that a later allocation hands to a different file.
 *
 *   WHAT THAT BUYS: after any crash, every file that existed before the crash
 *   is intact at its pre-crash size with its pre-crash contents. A file being
 *   written at the moment of the crash is either at its old size or its new
 *   one, never at the new size with garbage in the tail.
 *
 *   WHAT IT DOES NOT BUY, stated plainly:
 *     - Clusters allocated but not yet linked, or unlinked but not yet freed,
 *       LEAK. They stay marked in-use until the volume is reformatted. There
 *       is no fsck here. A crash costs at most one file's worth of clusters.
 *     - The last write before the crash can be lost entirely. There is no
 *       O_SYNC contract above this layer to promise otherwise.
 *     - It assumes a sector write is all-or-nothing. That is true of ATA and
 *       of a host file write; it is not true of a host that loses power
 *       mid-page. A torn sector inside a directory is DETECTED, not repaired:
 *       the entry fails its own consistency checks and is skipped.
 *     - The FSInfo free-cluster count is never trusted on mount. It is written
 *       as "unknown" (0xFFFFFFFF) precisely so that a stale count cannot
 *       become an allocator that hands out a cluster somebody is using.
 *
 * DEPENDENCIES
 *   block.h (all storage), vfs.h (the interface it implements), rtc.h (the
 *   wall clock, for timestamps other systems will display), heap, kernel.h.
 *
 * FUTURE EXTENSION POINTS
 *   A cluster-chain cache per inode already exists; a directory-entry hash
 *   would be the next thing if directories ever get large. FAT48/exFAT are not
 *   interesting. A `fsck` that walks every chain and rebuilds the free map
 *   would turn the cluster leak above into a recoverable condition.
 */
#ifndef FS_FAT_H
#define FS_FAT_H

#include <stdint.h>
#include <stddef.h>
#include "block.h"
#include "vfs.h"

/* ---- on-disk constants ---- */
#define FAT_DIRENT_SIZE   32
#define FAT_FREE          0x00000000u
#define FAT_BAD           0x0FFFFFF7u
#define FAT_EOC           0x0FFFFFF8u   /* >= this ends a chain */
#define FAT_EOC_WRITE     0x0FFFFFFFu   /* what we write to end one */
#define FAT_MASK          0x0FFFFFFFu

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F

/* Largest file FAT32 can describe: the size field is 32 bits. */
#define FAT_MAX_FILE       0xFFFFFFFFu

/* Deleted-entry marker, and the byte it stands in for when a real name starts
 * with it. */
#define FAT_DELETED        0xE5
#define FAT_KANJI_E5       0x05

/* An inode whose directory entry does not exist (the root directory). */
#define FAT_NO_ENTRY       0xFFFFFFFFu

/* How many vnodes to keep cached before evicting unreferenced ones. A cache
 * that only grows is a way for a model that lists a big directory to exhaust
 * the heap; see fat_icache_trim(). */
#define FAT_ICACHE_SOFT_MAX 256

struct fat_inode;

typedef struct fat_vol {
    block_device_t *bd;
    vfs_mount_t    *mnt;

    uint32_t sec_per_clus;
    uint32_t bytes_per_clus;
    uint32_t reserved_sec;
    uint32_t num_fats;
    uint32_t fat_sectors;      /* per FAT copy                        */
    uint32_t total_sectors;
    uint32_t data_start;       /* sector of cluster 2                 */
    uint32_t clusters;         /* count; valid ids are 2..clusters+1  */
    uint32_t root_clus;
    uint32_t fsinfo_sec;       /* 0 = none                            */
    uint32_t alloc_hint;       /* where the next free scan starts     */
    int      read_only;

    /* ONE-SECTOR READ CACHE.
     * Directory scans and FAT walks both read the same sector repeatedly, and
     * without this every 32-byte entry costs a disk transfer. It is
     * write-through: fat_sector_write() updates it when it holds the sector
     * being written, so it can never go stale.
     *
     * THE RULE: a pointer returned by fat_sector_read() is valid only until
     * the next call to ANY fat_sector_* function. Copy what you need out
     * first. Everything in this filesystem obeys that; a new caller that does
     * not will read another sector's bytes and believe them. */
    uint32_t cache_sec;
    int      cache_valid;
    uint8_t  cache[BLOCK_SECTOR_SIZE];

    struct fat_inode *icache;
    uint32_t          icache_n;
} fat_vol_t;

/* An in-core inode. `vnode` carries type/size/mtime for the VFS; everything
 * needed to find the file's bytes and its directory entry again is here. */
typedef struct fat_inode {
    vnode_t  *vnode;
    fat_vol_t *vol;
    uint32_t  first_clus;      /* 0 = no clusters allocated yet       */
    uint32_t  dir_clus;        /* first cluster of the parent dir     */
    uint32_t  dir_index;       /* short entry index within it         */
    uint32_t  dir_first_index; /* first entry of the set (LFN or short) */
    uint8_t   attr;

    /* Sequential-access cursor: which chain position `cur_clus` is. Turns a
     * streaming read from O(n^2) chain walks into O(n). */
    uint32_t  cur_index;
    uint32_t  cur_clus;

    struct fat_inode *next;
} fat_inode_t;

/* A directory entry, decoded. `name` is UTF-8 and always representable: when
 * the long name does not fit VFS_NAME_MAX, the 8.3 short name is used instead,
 * so every entry this filesystem reports can also be looked up and unlinked. */
typedef struct fat_dirent {
    char     name[VFS_NAME_MAX + 1];
    uint32_t index;            /* short entry index                   */
    uint32_t first_index;      /* first entry of the set              */
    uint32_t clus;             /* first cluster of the file           */
    uint32_t size;
    uint8_t  attr;
    uint8_t  raw[FAT_DIRENT_SIZE];
} fat_dirent_t;

/* A directory scan cursor. Carries the chain position so a full scan costs one
 * FAT lookup per cluster rather than one per entry. */
typedef struct fat_dircur {
    uint32_t dir_clus;         /* first cluster of the directory      */
    uint32_t index;            /* next entry index to examine         */
    uint32_t clus;             /* cluster holding `index`, 0 = restart */
    uint32_t clus_index;       /* chain position of `clus`            */
    /* Long-name accumulator. 20 records of 13 code units is the 255-character
     * maximum a FAT long name can reach, and fat_dir_next() writes at
     * (ordinal-1)*13, so this must be 260 and not 256 — at 256 an entry
     * claiming ordinal 20 wrote four code units past the end. */
    uint16_t lfn[20 * 13];
    int      lfn_have;         /* how many ordinals collected         */
    int      lfn_count;        /* how many the last entry announced   */
    uint8_t  lfn_sum;
    uint32_t lfn_first;        /* index of the first LFN entry        */
} fat_dircur_t;

/* ---- little-endian accessors. The on-disk structures are unaligned, so
 * every field goes through these rather than through a packed struct. ---- */
static inline uint16_t fat_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t fat_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void fat_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}
static inline void fat_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);         p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ---- fat_vol.c: the volume, the FAT, and the allocator ---- */

/* Read a sector into the volume cache. See the cache rule above. */
int fat_sector_read(fat_vol_t *v, uint32_t sec, const uint8_t **out);
/* Read a sector into the caller's own buffer (no lifetime rule attached). */
int fat_sector_read_into(fat_vol_t *v, uint32_t sec, void *dst);
int fat_sector_write(fat_vol_t *v, uint32_t sec, const void *buf);
/* Multi-sector transfers, straight to the device (cache kept coherent). */
int fat_sectors_read(fat_vol_t *v, uint32_t sec, uint32_t n, void *dst);
int fat_sectors_write(fat_vol_t *v, uint32_t sec, uint32_t n, const void *src);
int fat_barrier(fat_vol_t *v);      /* everything written so far is durable */

uint32_t fat_clus_sector(const fat_vol_t *v, uint32_t clus);
int fat_clus_valid(const fat_vol_t *v, uint32_t clus);

int fat_get(fat_vol_t *v, uint32_t clus, uint32_t *val);
int fat_set(fat_vol_t *v, uint32_t clus, uint32_t val);
/* Next cluster in the chain. *next is 0 when the chain ends. */
int fat_chain_next(fat_vol_t *v, uint32_t clus, uint32_t *next);
/* Allocate one cluster, zero it, and mark it end-of-chain. Not linked to
 * anything yet: the caller links it, which is the ordering that matters. */
int fat_alloc_cluster(fat_vol_t *v, uint32_t *out);
int fat_free_chain(fat_vol_t *v, uint32_t first);

/* Validate a FAT32 volume on `bd` and fill `v`. Returns VFS_OK, or VFS_EINVAL
 * with a printed reason if the image is not a FAT32 volume this code will
 * touch. Never guesses. */
int fat_volume_open(fat_vol_t *v, block_device_t *bd);

/* Lay down a fresh FAT32 volume covering the whole device. Destroys
 * everything on it. `label` may be NULL. */
int fat32_format(block_device_t *bd, const char *label);

/* Is this device blank (the first `n` sectors all zero)? The one condition
 * under which fs/fs.c will format a disk without being asked. */
int fat_device_is_blank(block_device_t *bd, uint32_t nsectors);

/* ---- fat_dir.c: directory entries and names ---- */

void fat_dircur_init(fat_dircur_t *c, uint32_t dir_clus);
/* 1 = produced an entry, 0 = end of directory, <0 = VFS error. */
int  fat_dir_next(fat_vol_t *v, fat_dircur_t *c, fat_dirent_t *out);
int  fat_dir_find(fat_vol_t *v, uint32_t dir_clus, const char *name,
                  fat_dirent_t *out);
int  fat_dir_add(fat_vol_t *v, uint32_t dir_clus, const char *name,
                 uint8_t attr, uint32_t first_clus, uint32_t size,
                 fat_dirent_t *out);
int  fat_dir_remove(fat_vol_t *v, uint32_t dir_clus, const fat_dirent_t *de);
/* Patch size / first cluster / write time of an existing entry. This is the
 * commit point of a write, and it is one sector. */
int  fat_dir_update(fat_vol_t *v, uint32_t dir_clus, uint32_t index,
                    uint32_t first_clus, uint32_t size);
/* 1 = the directory holds nothing but "." and "..". */
int  fat_dir_is_empty(fat_vol_t *v, uint32_t dir_clus);
/* Write "." and ".." into a freshly allocated, zeroed directory cluster. */
int  fat_dir_init_cluster(fat_vol_t *v, uint32_t clus, uint32_t parent_clus);
/* Is `name` something this filesystem can store? */
int  fat_name_ok(const char *name);
/* DOS date/time for "now", from the RTC (1980-01-01 when it cannot be read). */
void fat_now(uint16_t *date, uint16_t *time);

#endif /* FS_FAT_H */
