/* block.c — the block device registry and the validating I/O entry points.
 *
 * Everything interesting about this file is in include/block.h. What lives
 * here is the argument checking that a disk driver therefore does not have to
 * repeat, and the counters that make a silent disk visible.
 *
 * There is deliberately no cache and no I/O scheduler: this kernel is polled
 * and single-threaded, and a cache here would quietly re-order the writes
 * fs/fat/ orders on purpose to survive a crash (see fs/fat/fat.h).
 */

#include "block.h"
#include "kernel.h"
#include <string.h>

static block_device_t *devices;   /* registration order; index 0 is first */
static int             ndevices;

int block_register(block_device_t *bd) {
    if (!bd || !bd->ops || !bd->ops->read) return BLOCK_EINVAL;
    if (bd->sectors == 0) return BLOCK_EINVAL;
    if (bd->sector_size != BLOCK_SECTOR_SIZE) return BLOCK_EINVAL;
    if (bd->name[0] == '\0') return BLOCK_EINVAL;
    /* The name is what a mount, a log line and an operator all use to say
     * which disk they mean. Two devices answering to one name makes every one
     * of those ambiguous. */
    for (block_device_t *d = devices; d; d = d->next)
        if (strcmp(d->name, bd->name) == 0) return BLOCK_EINVAL;

    kobject_init(&bd->obj, KOBJ_DEVICE, bd->name);
    bd->reads = bd->writes = bd->flushes = bd->errors = 0;

    /* Append, so block_get(0) is stable as more disks appear. */
    block_device_t **pp = &devices;
    while (*pp) pp = &(*pp)->next;
    bd->next = NULL;
    *pp = bd;
    ndevices++;
    return BLOCK_OK;
}

block_device_t *block_get(int index) {
    if (index < 0) return NULL;
    for (block_device_t *d = devices; d; d = d->next)
        if (index-- == 0) return d;
    return NULL;
}

block_device_t *block_find(const char *name) {
    if (!name) return NULL;
    for (block_device_t *d = devices; d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;
    return NULL;
}

int block_count(void) { return ndevices; }

void block_registry_reset(void) { devices = NULL; ndevices = 0; }

/* One bounds check for both directions. `lba + count` is formed only after
 * count has been bounded, so a caller passing UINT32_MAX cannot wrap it into a
 * small in-range value and read someone else's sectors. */
static int check_range(block_device_t *bd, uint64_t lba, uint32_t count,
                       const void *buf) {
    if (!bd || !bd->ops) return BLOCK_ENXIO;
    if (!buf || count == 0) return BLOCK_EINVAL;
    if (lba >= bd->sectors) return BLOCK_ENOSPC;
    if ((uint64_t)count > bd->sectors - lba) return BLOCK_ENOSPC;
    return BLOCK_OK;
}

int block_read(block_device_t *bd, uint64_t lba, uint32_t count, void *buf) {
    int rc = check_range(bd, lba, count, buf);
    if (rc != BLOCK_OK) { if (bd) bd->errors++; return rc; }
    rc = bd->ops->read(bd, lba, count, buf);
    if (rc != BLOCK_OK) { bd->errors++; return rc; }
    bd->reads += count;
    return BLOCK_OK;
}

int block_write(block_device_t *bd, uint64_t lba, uint32_t count, const void *buf) {
    int rc = check_range(bd, lba, count, buf);
    if (rc != BLOCK_OK) { if (bd) bd->errors++; return rc; }
    if (bd->read_only) { bd->errors++; return BLOCK_EROFS; }
    if (!bd->ops->write) { bd->errors++; return BLOCK_EROFS; }
    rc = bd->ops->write(bd, lba, count, buf);
    if (rc != BLOCK_OK) { bd->errors++; return rc; }
    bd->writes += count;
    return BLOCK_OK;
}

int block_flush(block_device_t *bd) {
    if (!bd || !bd->ops) return BLOCK_ENXIO;
    if (!bd->ops->flush) return BLOCK_OK;
    int rc = bd->ops->flush(bd);
    if (rc != BLOCK_OK) { bd->errors++; return rc; }
    bd->flushes++;
    return BLOCK_OK;
}

const char *block_strerror(int err) {
    switch (err) {
        case BLOCK_OK:         return "ok";
        case BLOCK_EIO:        return "device I/O error";
        case BLOCK_ENXIO:      return "no such device";
        case BLOCK_EINVAL:     return "invalid request";
        case BLOCK_ENOSPC:     return "past the end of the device";
        case BLOCK_EROFS:      return "device is read-only";
        case BLOCK_ETIMEDOUT:  return "device timed out";
        default:               return "unknown block error";
    }
}

void block_report(void) {
    if (!devices) { kputs("block: no block devices\n"); return; }
    for (block_device_t *d = devices; d; d = d->next) {
        /* MiB rather than bytes: a 128 GiB disk in bytes overflows the
         * kernel's %u, and the number nobody can read is not worth printing. */
        uint64_t mib = (d->sectors * BLOCK_SECTOR_SIZE) / (1024 * 1024);
        kprintf("block: %s  %u MiB (%u sectors)  %s  %s\n",
                d->name, (unsigned)mib, (unsigned)d->sectors,
                d->read_only ? "read-only" : "read-write",
                d->model[0] ? d->model : "unknown model");
    }
}
