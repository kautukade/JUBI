/* ramfs.c — fable-os native filesystem, RAM-backed.
 *
 * PURPOSE
 *   The first real filesystem behind the VFS: a genuine implementation with its
 *   own inodes, directory entries, and dynamically grown file storage — not a
 *   demo with hardcoded files. It stores everything in the kernel heap.
 *
 * RESPONSIBILITIES
 *   Implement vnode_ops_t (lookup/create/unlink/read/write/readdir/truncate)
 *   and a mount hook, on top of a tree of ram-inodes.
 *
 * DEPENDENCIES
 *   vfs.h (the interface it plugs into), heap (all storage), string helpers.
 *
 * FUTURE EXTENSION POINTS
 *   This is the template for on-disk filesystems: a FAT32/ext2 driver will
 *   implement the same vnode_ops_t but back reads/writes with a block device
 *   instead of kmalloc'd buffers, and the VFS/kernel API stays identical. File
 *   storage grows by doubling; a real allocator/block map slots in here.
 *   RAMFS_MAX_FILE is a per-file bound, not a per-mount one: many small files
 *   can still fill the arena. A superblock byte budget belongs here next.
 */

#include "vfs.h"
#include "kernel.h"
#include <string.h>

/* Largest single file.
 *
 * A file lives in ONE contiguous kmalloc'd buffer that grows by doubling, so
 * writing N bytes transiently needs the old buffer plus a new one of up to 2N —
 * 3N of a 16 MiB arena that is shared with mbedTLS and lwIP. Two things depend
 * on this bound existing:
 *   - kmalloc() cannot fail politely (it panics), so an unbounded file size is
 *     a way for a caller to halt the machine. Refusing the write with
 *     VFS_ENOSPC is a failure the caller can report and recover from.
 *   - Without an upper bound, ramfs_grow()'s `cap *= 2` loop can wrap past
 *     UINT64_MAX and spin forever, which on this kernel (no interrupts, no
 *     scheduler, no watchdog) is an unrecoverable hang.
 */
#define RAMFS_MAX_FILE (1024u * 1024u)

/* A ram-inode: file bytes, or a list of directory entries. */
typedef struct rnode {
    vnode_type_t type;
    uint8_t     *data;      /* file contents (files only) */
    uint64_t     cap;       /* allocated capacity         */
    struct rdirent *kids;   /* children (dirs only)       */
} rnode_t;

typedef struct rdirent {
    char             name[VFS_NAME_MAX + 1];
    vnode_t         *vnode;
    struct rdirent  *next;
} rdirent_t;

static const vnode_ops_t ramfs_ops;   /* forward */

static rnode_t *rnode_new(vnode_type_t type) {
    rnode_t *r = kmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->type = type;
    return r;
}

static int ramfs_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    rnode_t *rd = dir->priv;
    for (rdirent_t *e = rd->kids; e; e = e->next)
        if (strcmp(e->name, name) == 0) { *out = e->vnode; return VFS_OK; }
    return VFS_ENOENT;
}

static int ramfs_create(vnode_t *dir, const char *name, vnode_type_t type,
                        vnode_t **out) {
    rnode_t *rd = dir->priv;
    /* Reject rather than truncate: strncpy'ing a 100-byte name into a 63-byte
     * slot would store a name the caller never asked for, and then collide with
     * every other name sharing that prefix. The VFS rejects the same lengths on
     * the lookup side (next_component), so the two agree. */
    if (strlen(name) > VFS_NAME_MAX) return VFS_EINVAL;
    for (rdirent_t *e = rd->kids; e; e = e->next)
        if (strcmp(e->name, name) == 0) return VFS_EEXIST;

    rnode_t *rn = rnode_new(type);
    vnode_t *vn = vfs_alloc_vnode(dir->mount, type, &ramfs_ops, rn);

    rdirent_t *e = kmalloc(sizeof *e);
    memset(e, 0, sizeof *e);
    strncpy(e->name, name, VFS_NAME_MAX);
    e->vnode = vn;
    e->next  = rd->kids;
    rd->kids = e;

    dir->mtime_ms = millis();
    if (out) *out = vn;
    return VFS_OK;
}

/* Free a vnode's storage and the vnode itself. Only ever called once the last
 * reference is gone — from ramfs_unlink() when no handle is open, or from
 * vfs_close() via the reclaim op when one was. */
static void ramfs_reclaim(vnode_t *n) {
    rnode_t *rn = n->priv;
    if (rn) {
        if (rn->data) kfree(rn->data);
        kfree(rn);
    }
    kfree(n);
}

static int ramfs_unlink(vnode_t *dir, const char *name) {
    rnode_t *rd = dir->priv;
    rdirent_t **pp = &rd->kids;
    while (*pp) {
        rdirent_t *e = *pp;
        if (strcmp(e->name, name) == 0) {
            vnode_t *v  = e->vnode;
            rnode_t *rn = v->priv;
            if (rn->type == VNODE_DIR && rn->kids) return VFS_EINVAL; /* not empty */
            *pp = e->next;
            kfree(e);
            dir->mtime_ms = millis();
            /* The directory entry held the vnode's original reference. Freeing
             * the storage regardless of the refcount is a use-after-free for
             * any handle vfs_open() left holding a reference — which is the
             * entire point of that reference. Drop ours; if a handle survives,
             * the file is unreachable by name and vfs_close() reclaims it. */
            if (kobject_put(&v->obj) == 0) ramfs_reclaim(v);
            return VFS_OK;
        }
        pp = &e->next;
    }
    return VFS_ENOENT;
}

static int64_t ramfs_read(vnode_t *n, void *buf, uint64_t off, uint64_t len) {
    if (n->type != VNODE_FILE) return VFS_EINVAL;
    if (off >= n->size) return 0;
    uint64_t avail = n->size - off;
    if (len > avail) len = avail;
    memcpy(buf, ((rnode_t *)n->priv)->data + off, len);
    return (int64_t)len;
}

/* Ensure the file's backing store holds at least `need` bytes. `need` is
 * bounded by the caller against RAMFS_MAX_FILE, which is what keeps the
 * doubling below from wrapping. */
static int ramfs_grow(vnode_t *n, uint64_t need) {
    rnode_t *rn = n->priv;
    if (need <= rn->cap) return VFS_OK;
    if (need > RAMFS_MAX_FILE) return VFS_ENOSPC;
    uint64_t cap = rn->cap ? rn->cap : 64;
    while (cap < need) {
        if (cap > RAMFS_MAX_FILE / 2) { cap = need; break; }
        cap *= 2;
    }
    uint8_t *nd = krealloc(rn->data, (size_t)cap);
    if (!nd) return VFS_ENOSPC;
    memset(nd + rn->cap, 0, (size_t)(cap - rn->cap));
    rn->data = nd;
    rn->cap  = cap;
    return VFS_OK;
}

static int64_t ramfs_write(vnode_t *n, const void *buf, uint64_t off, uint64_t len) {
    if (n->type != VNODE_FILE) return VFS_EINVAL;
    if (len == 0) return 0;
    /* off is a caller-supplied file position (vfs_seek accepts any positive
     * int64), so off + len must not be formed before it is bounded. */
    if (off > RAMFS_MAX_FILE || len > RAMFS_MAX_FILE - off) return VFS_ENOSPC;
    int rc = ramfs_grow(n, off + len);
    if (rc != VFS_OK) return rc;
    rnode_t *rn = n->priv;
    /* A write past EOF leaves a hole. ramfs_grow only zeroes past the old
     * *capacity*, so the hole can still hold bytes from a previous, longer
     * incarnation of this file — zero it explicitly. */
    if (off > n->size) memset(rn->data + n->size, 0, (size_t)(off - n->size));
    memcpy(rn->data + off, buf, len);
    if (off + len > n->size) n->size = off + len;
    n->mtime_ms = millis();
    return (int64_t)len;
}

static int ramfs_readdir(vnode_t *dir, uint32_t index, char *name_out, size_t cap) {
    rnode_t *rd = dir->priv;
    if (cap == 0) return VFS_EINVAL;         /* `cap - 1` below would wrap */
    for (rdirent_t *e = rd->kids; e; e = e->next)
        if (index-- == 0) { strncpy(name_out, e->name, cap - 1); name_out[cap - 1] = '\0'; return VFS_OK; }
    return VFS_ENOENT;
}

static int ramfs_truncate(vnode_t *n, uint64_t size) {
    if (n->type != VNODE_FILE) return VFS_EINVAL;
    if (size > n->size) {
        int rc = ramfs_grow(n, size);
        if (rc != VFS_OK) return rc;
    } else if (size < n->size) {
        /* Zero what is being discarded. Shrinking by only moving `size` leaves
         * the old bytes in the buffer, and a later write past the new EOF
         * republishes them: truncate-to-0 then write at offset 10 would read
         * back ten bytes of the previous contents. */
        rnode_t *rn = n->priv;
        if (rn->data) memset(rn->data + size, 0, (size_t)(n->size - size));
    }
    n->size = size;
    n->mtime_ms = millis();
    return VFS_OK;
}

static const vnode_ops_t ramfs_ops = {
    .lookup   = ramfs_lookup,
    .create   = ramfs_create,
    .unlink   = ramfs_unlink,
    .read     = ramfs_read,
    .write    = ramfs_write,
    .readdir  = ramfs_readdir,
    .truncate = ramfs_truncate,
    .reclaim  = ramfs_reclaim,
};

static int ramfs_mount(vfs_mount_t *mnt) {
    rnode_t *root_rn = rnode_new(VNODE_DIR);
    mnt->root = vfs_alloc_vnode(mnt, VNODE_DIR, &ramfs_ops, root_rn);
    mnt->priv = root_rn;
    return VFS_OK;
}

static vfs_fstype_t ramfs_type = { .name = "ramfs", .mount = ramfs_mount };

/* Called from vfs bring-up to make "ramfs" available. Returns VFS_OK, or
 * VFS_EEXIST if the name is already taken — which would otherwise surface much
 * later and much less legibly as a mount failing with VFS_ENOENT. */
int ramfs_register(void) {
    return vfs_register_fstype(&ramfs_type);
}
