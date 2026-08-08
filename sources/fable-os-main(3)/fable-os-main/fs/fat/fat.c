/* fat.c — the vnode layer: FAT32 seen through vnode_ops_t.
 *
 * Everything above this file (the VFS, the tools, the model) works the same
 * way against a disk as it does against ramfs. That was the promise made in
 * include/vfs.h; this file is where it is kept, and nothing in fs/vfs/vfs.c
 * had to change to keep it.
 *
 * WHAT THIS FILE OWNS
 *   - the in-core inode cache (identity: one vnode per directory entry)
 *   - byte-range read/write across a cluster chain
 *   - the ORDER of every mutation, which is the crash-consistency argument in
 *     fs/fat/fat.h. Every write barrier in this file is load-bearing.
 *
 * THREE PLACES THIS DIFFERS FROM ramfs, deliberately:
 *   - Lookup is case-INSENSITIVE, because FAT is. "Notes.txt" and "notes.txt"
 *     are one file, and creating the second returns VFS_EEXIST.
 *   - Unlinking a file that still has an open handle is REFUSED. ramfs can
 *     keep an unnamed inode alive because its storage is a heap pointer; here
 *     the storage is a cluster chain that the allocator would hand to the next
 *     file, so the open handle would end up reading somebody else's data. A
 *     refusal the caller can see beats silent corruption it cannot.
 *   - A hole longer than FAT_MAX_HOLE is refused rather than zero-filled: the
 *     zero-fill is real disk traffic, and an unbounded one is a way for a
 *     model that seeks to offset 3000000000 to stall the machine.
 */

#include "fat.h"
#include "fs.h"
#include "kernel.h"
#include "rtc.h"
#include <string.h>

/* Largest gap between the end of a file and the offset of a write. */
#define FAT_MAX_HOLE (1024u * 1024u)

static const vnode_ops_t fat_ops;

/* ====================================================================== */
/* 1. inode cache                                                         */
/* ====================================================================== */

static fat_inode_t *icache_find(fat_vol_t *v, uint32_t dir_clus, uint32_t index) {
    for (fat_inode_t *in = v->icache; in; in = in->next)
        if (in->dir_clus == dir_clus && in->dir_index == index) return in;
    return NULL;
}

static void icache_unlink(fat_vol_t *v, fat_inode_t *victim) {
    fat_inode_t **pp = &v->icache;
    while (*pp) {
        if (*pp == victim) { *pp = victim->next; v->icache_n--; return; }
        pp = &(*pp)->next;
    }
}

/* Free a vnode and its inode. Only ever called when the last reference is
 * gone: from fat_reclaim (the VFS saw the count reach zero) or from unlink. */
static void inode_free(fat_inode_t *in) {
    vnode_t *vn = in->vnode;
    kfree(in);
    kfree(vn);
}

/* Drop cached inodes nobody is using. Directories are never evicted: the VFS
 * walks a path by holding a directory vnode across the lookup of its child, so
 * freeing one mid-walk would be a use-after-free. Files are safe — no lookup
 * happens after the leaf is resolved. */
static void icache_trim(fat_vol_t *v) {
    if (v->icache_n <= FAT_ICACHE_SOFT_MAX) return;
    fat_inode_t **pp = &v->icache;
    while (*pp && v->icache_n > FAT_ICACHE_SOFT_MAX) {
        fat_inode_t *in = *pp;
        if (in->vnode->type != VNODE_DIR && in->vnode->obj.refcount == 1) {
            *pp = in->next;
            v->icache_n--;
            inode_free(in);
            continue;
        }
        pp = &in->next;
    }
}

/* DOS date/time -> unix milliseconds, so vfs_stat() reports a wall-clock
 * timestamp that means something across reboots (ramfs reports uptime, which
 * cannot). */
static uint64_t dos_to_ms(uint16_t date, uint16_t time) {
    rtc_time_t t;
    t.year   = 1980 + ((date >> 9) & 0x7F);
    t.month  = (date >> 5) & 0x0F;
    t.day    = date & 0x1F;
    t.hour   = (time >> 11) & 0x1F;
    t.minute = (time >> 5) & 0x3F;
    t.second = (time & 0x1F) * 2;
    if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) return 0;
    int64_t s = rtc_to_unix(&t);
    if (s < 0) return 0;
    return (uint64_t)s * 1000u;
}

/* Build the vnode+inode pair for one directory entry, or return the cached one
 * if this entry already has identity. */
static fat_inode_t *inode_get(fat_vol_t *v, uint32_t dir_clus,
                              const fat_dirent_t *de) {
    fat_inode_t *in = icache_find(v, dir_clus, de->index);
    if (in) return in;

    icache_trim(v);

    vnode_type_t type = (de->attr & FAT_ATTR_DIRECTORY) ? VNODE_DIR : VNODE_FILE;
    in = kmalloc(sizeof *in);
    if (!in) return NULL;
    memset(in, 0, sizeof *in);
    in->vol             = v;
    in->first_clus      = de->clus;
    in->dir_clus        = dir_clus;
    in->dir_index       = de->index;
    in->dir_first_index = de->first_index;
    in->attr            = de->attr;

    vnode_t *vn = vfs_alloc_vnode(v->mnt, type, &fat_ops, in);
    vn->size     = (type == VNODE_DIR) ? 0 : de->size;
    vn->mtime_ms = dos_to_ms(fat_rd16(de->raw + 24), fat_rd16(de->raw + 22));
    if (de->attr & FAT_ATTR_READ_ONLY) vn->mode &= ~(uint32_t)VFS_PERM_W;
    in->vnode = vn;

    in->next   = v->icache;
    v->icache  = in;
    v->icache_n++;
    return in;
}

/* ====================================================================== */
/* 2. the cluster chain                                                   */
/* ====================================================================== */

/* Cluster holding chain position `pos`, using the inode's sequential cursor so
 * a streaming read does not re-walk the chain for every block. Returns VFS_OK
 * with *out set, or VFS_ENOENT when the chain is shorter than that. */
static int chain_at(fat_inode_t *in, uint32_t pos, uint32_t *out) {
    fat_vol_t *v = in->vol;
    uint32_t c, i;
    if (in->cur_clus != 0 && in->cur_index <= pos) {
        c = in->cur_clus; i = in->cur_index;
    } else {
        if (in->first_clus == 0) return VFS_ENOENT;
        c = in->first_clus; i = 0;
    }
    while (i < pos) {
        uint32_t nx;
        int rc = fat_chain_next(v, c, &nx);
        if (rc != VFS_OK) return rc;
        if (nx == 0) return VFS_ENOENT;
        c = nx; i++;
    }
    in->cur_clus  = c;
    in->cur_index = i;
    *out = c;
    return VFS_OK;
}

/* Make the chain long enough to hold `bytes`. New clusters are zeroed and
 * marked end-of-chain by the allocator, then linked to their predecessor —
 * data before metadata, always. */
static int ensure_clusters(fat_inode_t *in, uint64_t bytes) {
    fat_vol_t *v = in->vol;
    if (bytes == 0) return VFS_OK;
    uint32_t need = (uint32_t)((bytes + v->bytes_per_clus - 1) / v->bytes_per_clus);

    if (in->first_clus == 0) {
        uint32_t c;
        int rc = fat_alloc_cluster(v, &c);
        if (rc != VFS_OK) return rc;
        in->first_clus = c;
        in->cur_clus   = c;
        in->cur_index  = 0;
    }

    uint32_t c = in->first_clus;
    for (uint32_t i = 1; i < need; i++) {
        uint32_t nx;
        int rc = fat_chain_next(v, c, &nx);
        if (rc != VFS_OK) return rc;
        if (nx == 0) {
            rc = fat_alloc_cluster(v, &nx);
            if (rc != VFS_OK) return rc;
            rc = fat_set(v, c, nx);            /* now it joins the chain */
            if (rc != VFS_OK) return rc;
            /* No flush between those two, deliberately. One flush per cluster
             * would be ~250 cache-flush commands for a 128 KiB file, which is
             * seconds on a real disk, and it buys nothing: nothing can READ
             * these clusters until the size in the directory entry covers
             * them, and that is written after commit()'s barrier. A crash here
             * leaks clusters (stated in fat.h) and loses nothing. The one
             * place the argument does NOT hold is a DIRECTORY cluster, which a
             * scan walks by chain and not by size — dir_extend() flushes. */
        }
        c = nx;
    }
    return VFS_OK;
}

/* Byte-range transfer over a chain that already covers [off, off+len). */
static int rw_bytes(fat_inode_t *in, void *buf, uint64_t off, uint64_t len,
                    int writing) {
    fat_vol_t *v = in->vol;
    uint8_t *p = buf;
    while (len) {
        uint32_t pos    = (uint32_t)(off / v->bytes_per_clus);
        uint32_t within = (uint32_t)(off % v->bytes_per_clus);
        uint32_t clus;
        int rc = chain_at(in, pos, &clus);
        if (rc != VFS_OK) return rc;

        uint64_t n = v->bytes_per_clus - within;
        if (n > len) n = len;

        uint32_t sec  = fat_clus_sector(v, clus) + within / BLOCK_SECTOR_SIZE;
        uint32_t soff = within % BLOCK_SECTOR_SIZE;

        if (soff == 0 && n >= BLOCK_SECTOR_SIZE) {
            uint32_t nsec = (uint32_t)(n / BLOCK_SECTOR_SIZE);
            n = (uint64_t)nsec * BLOCK_SECTOR_SIZE;
            rc = writing ? fat_sectors_write(v, sec, nsec, p)
                         : fat_sectors_read(v, sec, nsec, p);
        } else {
            uint64_t m = BLOCK_SECTOR_SIZE - soff;
            if (m > n) m = n;
            n = m;
            if (writing) {
                uint8_t tmp[BLOCK_SECTOR_SIZE];
                rc = fat_sector_read_into(v, sec, tmp);
                if (rc == VFS_OK) {
                    memcpy(tmp + soff, p, (size_t)n);
                    rc = fat_sector_write(v, sec, tmp);
                }
            } else {
                const uint8_t *sp;
                rc = fat_sector_read(v, sec, &sp);
                if (rc == VFS_OK) memcpy(p, sp + soff, (size_t)n);
            }
        }
        if (rc != VFS_OK) return rc;
        off += n; p += n; len -= n;
    }
    return VFS_OK;
}

/* Zero [from, to). Used to fill a hole created by writing past the end: the
 * clusters may be freshly allocated (already zero) or may be leftovers from a
 * chain that outlived its size across a crash, and the second case would
 * republish a previous file's bytes. */
static int zero_range(fat_inode_t *in, uint64_t from, uint64_t to) {
    uint8_t zeros[BLOCK_SECTOR_SIZE];
    memset(zeros, 0, sizeof zeros);
    while (from < to) {
        uint64_t n = to - from;
        if (n > BLOCK_SECTOR_SIZE) n = BLOCK_SECTOR_SIZE;
        int rc = rw_bytes(in, zeros, from, n, 1);
        if (rc != VFS_OK) return rc;
        from += n;
    }
    return VFS_OK;
}

/* Publish a size/first-cluster change. THIS IS THE COMMIT POINT: it is one
 * sector write, and everything it refers to is already on the disk. */
static int commit(fat_inode_t *in, uint64_t size) {
    fat_vol_t *v = in->vol;
    int rc = fat_barrier(v);
    if (rc != VFS_OK) return rc;
    rc = fat_dir_update(v, in->dir_clus, in->dir_index, in->first_clus,
                        (uint32_t)size);
    if (rc != VFS_OK) return rc;
    return fat_barrier(v);
}

/* ====================================================================== */
/* 3. vnode operations                                                    */
/* ====================================================================== */

static int fat_op_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    fat_inode_t *di = dir->priv;
    fat_vol_t   *v  = di->vol;
    if (dir->type != VNODE_DIR) return VFS_ENOTDIR;
    /* "." and ".." exist on the disk and are deliberately not resolvable: the
     * VFS handles "." itself, and letting ".." through would let a path walk
     * out of a directory in a filesystem whose mount boundary the VFS, not
     * this code, is responsible for. */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return VFS_ENOENT;

    fat_dirent_t de;
    int rc = fat_dir_find(v, di->first_clus, name, &de);
    if (rc != VFS_OK) return rc;

    fat_inode_t *in = inode_get(v, di->first_clus, &de);
    if (!in) return VFS_ENOSPC;
    *out = in->vnode;
    return VFS_OK;
}

static int fat_op_create(vnode_t *dir, const char *name, vnode_type_t type,
                         vnode_t **out) {
    fat_inode_t *di = dir->priv;
    fat_vol_t   *v  = di->vol;
    if (dir->type != VNODE_DIR) return VFS_ENOTDIR;
    if (v->read_only) return VFS_EINVAL;
    if (!fat_name_ok(name)) return VFS_EINVAL;

    fat_dirent_t de;
    int rc = fat_dir_find(v, di->first_clus, name, &de);
    if (rc == VFS_OK) return VFS_EEXIST;
    if (rc != VFS_ENOENT) return rc;

    uint32_t clus = 0;
    if (type == VNODE_DIR) {
        /* A directory must be a valid directory the instant its name appears,
         * so its cluster with "." and ".." goes down first and is flushed
         * before the parent's entry is written. */
        rc = fat_alloc_cluster(v, &clus);
        if (rc != VFS_OK) return rc;
        rc = fat_dir_init_cluster(v, clus, di->first_clus);
        if (rc == VFS_OK) rc = fat_barrier(v);
        if (rc != VFS_OK) { fat_free_chain(v, clus); return rc; }
    }

    uint8_t attr = (type == VNODE_DIR) ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    rc = fat_dir_add(v, di->first_clus, name, attr, clus, 0, &de);
    if (rc != VFS_OK) {
        if (clus) fat_free_chain(v, clus);
        return rc;
    }
    rc = fat_barrier(v);
    if (rc != VFS_OK) return rc;

    dir->mtime_ms = millis();
    fat_inode_t *in = inode_get(v, di->first_clus, &de);
    if (!in) return VFS_ENOSPC;
    if (out) *out = in->vnode;
    return VFS_OK;
}

static void fat_op_reclaim(vnode_t *n) {
    fat_inode_t *in = n->priv;
    if (!in) { kfree(n); return; }
    icache_unlink(in->vol, in);
    inode_free(in);
}

static int fat_op_unlink(vnode_t *dir, const char *name) {
    fat_inode_t *di = dir->priv;
    fat_vol_t   *v  = di->vol;
    if (dir->type != VNODE_DIR) return VFS_ENOTDIR;
    if (v->read_only) return VFS_EINVAL;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return VFS_EINVAL;

    fat_dirent_t de;
    int rc = fat_dir_find(v, di->first_clus, name, &de);
    if (rc != VFS_OK) return rc;

    if (de.attr & FAT_ATTR_DIRECTORY) {
        int empty = fat_dir_is_empty(v, de.clus);
        if (empty < 0) return empty;
        if (!empty) return VFS_EINVAL;
    }

    fat_inode_t *in = icache_find(v, di->first_clus, de.index);
    /* See the header comment: an open handle keeps the name. */
    if (in && in->vnode->obj.refcount > 1) {
        kprintf("fat: refusing to unlink \"%s\": it is open\n", name);
        return VFS_EINVAL;
    }

    /* Entry first (and flushed inside fat_dir_remove), clusters second. The
     * other order can leave a live name pointing into free space. */
    rc = fat_dir_remove(v, di->first_clus, &de);
    if (rc != VFS_OK) return rc;
    rc = fat_free_chain(v, de.clus);
    if (rc != VFS_OK) return rc;
    rc = fat_barrier(v);
    if (rc != VFS_OK) return rc;

    dir->mtime_ms = millis();
    if (in) {
        vnode_t *vn = in->vnode;
        icache_unlink(v, in);
        if (kobject_put(&vn->obj) == 0) inode_free(in);
    }
    return VFS_OK;
}

static int64_t fat_op_read(vnode_t *n, void *buf, uint64_t off, uint64_t len) {
    fat_inode_t *in = n->priv;
    if (n->type != VNODE_FILE) return VFS_EINVAL;
    if (off >= n->size) return 0;
    uint64_t avail = n->size - off;
    if (len > avail) len = avail;
    if (len == 0) return 0;
    int rc = rw_bytes(in, buf, off, len, 0);
    if (rc != VFS_OK) return rc;
    return (int64_t)len;
}

static int64_t fat_op_write(vnode_t *n, const void *buf, uint64_t off,
                            uint64_t len) {
    fat_inode_t *in = n->priv;
    fat_vol_t   *v  = in->vol;
    if (n->type != VNODE_FILE) return VFS_EINVAL;
    if (v->read_only) return VFS_EINVAL;
    if (len == 0) return 0;
    /* off comes from vfs_seek and can be any non-negative int64, so the sum is
     * only formed after both halves are bounded. */
    if (off > FAT_MAX_FILE || len > FAT_MAX_FILE - off) return VFS_ENOSPC;
    if (off > n->size && off - n->size > FAT_MAX_HOLE) {
        kprintf("fat: refusing a %u MiB hole in a file\n",
                (unsigned)((off - n->size) / (1024 * 1024)));
        return VFS_EINVAL;
    }

    int rc = ensure_clusters(in, off + len);
    if (rc != VFS_OK) return rc;
    if (off > n->size) {
        rc = zero_range(in, n->size, off);
        if (rc != VFS_OK) return rc;
    }
    rc = rw_bytes(in, (void *)(uintptr_t)buf, off, len, 1);
    if (rc != VFS_OK) return rc;

    uint64_t newsize = (off + len > n->size) ? off + len : n->size;
    rc = commit(in, newsize);
    if (rc != VFS_OK) return rc;

    n->size     = newsize;
    n->mtime_ms = millis();
    return (int64_t)len;
}

static int fat_op_readdir(vnode_t *dir, uint32_t index, char *name_out, size_t cap) {
    fat_inode_t *di = dir->priv;
    if (dir->type != VNODE_DIR) return VFS_ENOTDIR;
    if (cap == 0) return VFS_EINVAL;

    fat_dircur_t cur;
    fat_dirent_t de;
    fat_dircur_init(&cur, di->first_clus);
    for (;;) {
        int rc = fat_dir_next(di->vol, &cur, &de);
        if (rc < 0) return rc;
        if (rc == 0) return VFS_ENOENT;
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;
        if (index-- == 0) {
            strncpy(name_out, de.name, cap - 1);
            name_out[cap - 1] = '\0';
            return VFS_OK;
        }
    }
}

static int fat_op_truncate(vnode_t *n, uint64_t size) {
    fat_inode_t *in = n->priv;
    fat_vol_t   *v  = in->vol;
    if (n->type != VNODE_FILE) return VFS_EINVAL;
    if (v->read_only) return VFS_EINVAL;
    if (size > FAT_MAX_FILE) return VFS_ENOSPC;
    if (size == n->size) return VFS_OK;

    if (size > n->size) {
        if (size - n->size > FAT_MAX_HOLE) return VFS_EINVAL;
        int rc = ensure_clusters(in, size);
        if (rc != VFS_OK) return rc;
        rc = zero_range(in, n->size, size);
        if (rc != VFS_OK) return rc;
        rc = commit(in, size);
        if (rc != VFS_OK) return rc;
        n->size = size;
        n->mtime_ms = millis();
        return VFS_OK;
    }

    /* Shrinking. Commit the smaller size FIRST: a chain longer than the size
     * is harmless (it leaks clusters), a size longer than the chain is a file
     * that reads off the end of itself. */
    uint32_t old_first = in->first_clus;
    uint32_t keep = size ? (uint32_t)((size + v->bytes_per_clus - 1)
                                      / v->bytes_per_clus) : 0;
    if (keep == 0) in->first_clus = 0;
    int rc = commit(in, size);
    if (rc != VFS_OK) { in->first_clus = old_first; return rc; }
    n->size = size;
    n->mtime_ms = millis();

    /* The cursor may point past the new end of the chain. */
    in->cur_clus = 0; in->cur_index = 0;

    if (keep == 0) {
        rc = fat_free_chain(v, old_first);
    } else {
        uint32_t last;
        rc = chain_at(in, keep - 1, &last);
        if (rc == VFS_OK) {
            uint32_t tail;
            rc = fat_chain_next(v, last, &tail);
            if (rc == VFS_OK && tail != 0) {
                /* Terminate first, then free: never leave a live chain
                 * pointing at a cluster the allocator can hand out. */
                rc = fat_set(v, last, FAT_EOC_WRITE);
                if (rc == VFS_OK) rc = fat_barrier(v);
                if (rc == VFS_OK) rc = fat_free_chain(v, tail);
            }
        } else if (rc == VFS_ENOENT) {
            rc = VFS_OK;                 /* chain was already short enough */
        }
    }
    if (rc != VFS_OK) return rc;
    return fat_barrier(v);
}

static const vnode_ops_t fat_ops = {
    .lookup   = fat_op_lookup,
    .create   = fat_op_create,
    .unlink   = fat_op_unlink,
    .read     = fat_op_read,
    .write    = fat_op_write,
    .readdir  = fat_op_readdir,
    .truncate = fat_op_truncate,
    .reclaim  = fat_op_reclaim,
};

/* ====================================================================== */
/* 4. mount                                                               */
/* ====================================================================== */

/* vfs_mount() takes a mountpoint and a filesystem NAME and nothing else — it
 * is the VFS's business which filesystem, not which disk. Rather than change
 * that signature (and every caller and test with it), the disk is handed over
 * here immediately before the call. Single-threaded, one mount at a time, and
 * cleared on the way out so a second mount cannot inherit the first's disk. */
static block_device_t *pending_bd;

static int fat32_do_mount(vfs_mount_t *mnt) {
    if (!pending_bd) {
        kputs("fat: mount attempted with no block device selected\n");
        return VFS_EINVAL;
    }
    fat_vol_t *v = kmalloc(sizeof *v);
    if (!v) return VFS_ENOSPC;
    int rc = fat_volume_open(v, pending_bd);
    if (rc != VFS_OK) { kfree(v); return rc; }
    v->mnt = mnt;

    /* The root directory has no directory entry anywhere, so it is keyed with
     * FAT_NO_ENTRY and fat_dir_update() knows to leave it alone. */
    fat_inode_t *in = kmalloc(sizeof *in);
    if (!in) { kfree(v); return VFS_ENOSPC; }
    memset(in, 0, sizeof *in);
    in->vol             = v;
    in->first_clus      = v->root_clus;
    in->dir_clus        = 0;
    in->dir_index       = FAT_NO_ENTRY;
    in->dir_first_index = FAT_NO_ENTRY;
    in->attr            = FAT_ATTR_DIRECTORY;

    vnode_t *root = vfs_alloc_vnode(mnt, VNODE_DIR, &fat_ops, in);
    in->vnode  = root;
    v->icache  = in;
    v->icache_n = 1;

    mnt->priv = v;
    mnt->root = root;
    return VFS_OK;
}

static vfs_fstype_t fat32_type = { .name = "fat32", .mount = fat32_do_mount };

int fat32_register(void) { return vfs_register_fstype(&fat32_type); }

int fat32_mount_disk(const char *mountpoint, block_device_t *bd, int allow_format) {
    if (!bd) return VFS_EINVAL;

    pending_bd = bd;
    int rc = vfs_mount(mountpoint, "fat32");
    pending_bd = NULL;
    if (rc == VFS_OK) return VFS_OK;

    if (!allow_format) return rc;

    /* Formatting is only ever done to a disk that is demonstrably empty. A
     * disk that holds SOMETHING this code cannot read is far more likely to be
     * an image the operator cares about than one they wanted erased, and
     * "helpfully" reformatting it is unrecoverable. */
    if (!fat_device_is_blank(bd, 8)) {
        kprintf("fat: %s holds data that is not a FAT32 volume - leaving it "
                "alone. Erase it deliberately if you meant to reuse it.\n",
                bd->name);
        return rc;
    }
    kprintf("fat: %s is blank - formatting it as FAT32\n", bd->name);
    rc = fat32_format(bd, "FABLEOS");
    if (rc != VFS_OK) return rc;

    pending_bd = bd;
    rc = vfs_mount(mountpoint, "fat32");
    pending_bd = NULL;
    return rc;
}
