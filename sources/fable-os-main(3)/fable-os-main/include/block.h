/* block.h — the block device layer: fixed-size sector I/O, one interface.
 *
 * PURPOSE
 *   Put one thin, testable layer between anything that stores bytes durably
 *   (a filesystem) and the silicon that holds them (ATA today; AHCI, NVMe, a
 *   virtio disk or a RAM disk later). A filesystem addresses LBAs and never
 *   touches a device register; a disk driver moves sectors and knows nothing
 *   about files.
 *
 * ARCHITECTURE
 *   Two halves, exactly like vfs.h:
 *     - the public API below (block_read/write/flush + the registry): what a
 *       filesystem calls. It validates every argument BEFORE the driver sees
 *       it, so a driver never has to defend against a bad LBA.
 *     - block_ops_t: the per-driver interface. A driver publishes a device by
 *       filling in a block_device_t and calling block_register().
 *
 * A FAILED READ IS NEVER A ZERO-FILLED BUFFER
 *   This is the whole reason the layer exists in this shape. A disk that
 *   silently returns zeros is worse than one that fails: the filesystem above
 *   parses the zeros, decides the directory is empty, and writes over the
 *   operator's data. So every entry point returns a negative BLOCK_E* on
 *   failure and the caller MUST check it; the contents of `buf` after a failed
 *   read are unspecified and must not be used. Errors are counted per device
 *   (block_device_t.errors) so a flaky disk is visible rather than inferred.
 *
 * SECTOR SIZE
 *   BLOCK_SECTOR_SIZE (512) is the unit of everything here. A device with a
 *   different physical sector size must present 512-byte logical sectors or
 *   not register at all — the layer refuses to register anything else rather
 *   than let a filesystem compute offsets against a size it guessed.
 *
 * DEPENDENCIES
 *   kobject.h (identity/lifetime), device.h (the device-model node a driver
 *   optionally attaches), kernel.h for kprintf. No filesystem knowledge.
 *
 * FUTURE EXTENSION POINTS
 *   - Partitions: an MBR/GPT parser can register a second block_device_t whose
 *     ops offset into the parent. Nothing above changes.
 *   - Asynchronous I/O: the ops are synchronous because this kernel is polled
 *     and single-threaded. A completion callback goes in block_ops_t.
 *   - A buffer cache belongs here, not in a filesystem. It is deliberately
 *     absent: see the ordering argument in fs/fat/fat.h — write-through with
 *     explicit flushes is what makes crash behaviour statable.
 */
#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include "kobject.h"

/* The one sector size this layer speaks. */
#define BLOCK_SECTOR_SIZE 512

/* Longest device name, excluding the NUL ("ata0", "ram0", ...). */
#define BLOCK_NAME_MAX 15

/* Error codes: 0 = success, everything else negative and errno-shaped. */
#define BLOCK_OK          0
#define BLOCK_EIO        -5    /* the device reported a hardware error   */
#define BLOCK_ENXIO      -6    /* no such device / it went away          */
#define BLOCK_EINVAL     -22   /* bad LBA, count, or buffer              */
#define BLOCK_ENOSPC     -28   /* request runs off the end of the device */
#define BLOCK_EROFS      -30   /* write to a read-only device            */
#define BLOCK_ETIMEDOUT  -110  /* the device stopped answering           */

struct block_device;

/* Per-driver operations. All are synchronous and return 0 or a negative
 * BLOCK_E*. `count` is a sector count and is never 0 (the layer checks). A
 * driver may assume lba + count is within the device: the layer checked. */
typedef struct block_ops {
    int (*read)(struct block_device *bd, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *bd, uint64_t lba, uint32_t count, const void *buf);
    /* Push volatile write cache to the medium. Optional: a device with no
     * cache leaves it NULL and block_flush() succeeds. A device WITH a cache
     * that leaves it NULL silently breaks every ordering guarantee a
     * filesystem thinks it has, so implement it or set has_cache = 0. */
    int (*flush)(struct block_device *bd);
} block_ops_t;

typedef struct block_device {
    kobject_t          obj;              /* MUST be first (see kobject.h)  */
    char               name[BLOCK_NAME_MAX + 1];
    char               model[41];        /* what the hardware calls itself */
    uint64_t           sectors;          /* capacity, in 512-byte sectors  */
    uint32_t           sector_size;      /* always BLOCK_SECTOR_SIZE       */
    int                read_only;
    int                has_cache;        /* device has a volatile cache    */
    const block_ops_t *ops;
    void              *priv;             /* driver-private state           */
    struct device     *dev;              /* device-model node, or NULL     */

    /* Counters. Cheap, and the only way to tell "the filesystem did nothing"
     * apart from "the disk swallowed it". */
    uint64_t           reads, writes, flushes, errors;

    struct block_device *next;           /* registry chain                 */
} block_device_t;

/* ---- registry ---- */

/* Publish a device. Returns BLOCK_OK, or BLOCK_EINVAL for a malformed one
 * (no ops->read, zero capacity, a sector size that is not 512, an empty or
 * duplicate name). Rejecting is deliberate: a half-filled block_device_t that
 * registers becomes a filesystem mounting something that cannot answer. */
int block_register(block_device_t *bd);

/* Enumeration. index is 0-based; NULL past the end. */
block_device_t *block_get(int index);
block_device_t *block_find(const char *name);
int             block_count(void);

/* Forget every registered device. For tests only — nothing frees hardware. */
void block_registry_reset(void);

/* ---- I/O ---- */

/* Read/write `count` sectors starting at `lba`. Return BLOCK_OK or a negative
 * BLOCK_E*. On any error the buffer contents are unspecified. */
int block_read(block_device_t *bd, uint64_t lba, uint32_t count, void *buf);
int block_write(block_device_t *bd, uint64_t lba, uint32_t count, const void *buf);

/* Make every write issued before this call durable. BLOCK_OK if the device has
 * no cache to flush. */
int block_flush(block_device_t *bd);

/* Human-readable form of a BLOCK_E* code, for logs and model-facing errors. */
const char *block_strerror(int err);

/* Print one line per registered device (capacity, model, counters). */
void block_report(void);

#endif /* BLOCK_H */
