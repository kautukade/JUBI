/* vfs.h — Virtual File System: the stable, filesystem-independent API.
 *
 * PURPOSE
 *   Give the kernel one way to name, open, and manipulate files and directories
 *   regardless of which filesystem implementation backs them (a RAM filesystem
 *   today; FAT32/ext2 on a real disk later) — the kernel API never changes.
 *
 * ARCHITECTURE
 *   Three layers, cleanly separated:
 *     - The public API below (vfs_open/read/write/seek/mkdir/stat/...): what the
 *       rest of the kernel calls. Path resolution and handle management live in
 *       fs/vfs/vfs.c and are filesystem-agnostic.
 *     - vnode_ops_t: the per-filesystem driver interface. A filesystem plugs in
 *       by registering a vfs_fstype and implementing these node operations.
 *     - The implementation (e.g. fs/native/ramfs.c): private to itself; the VFS
 *       only ever touches it through vnode_ops_t.
 *
 * RESPONSIBILITIES
 *   files, directories, metadata, mounting, file handles, read/write/seek, and
 *   a permission field carried now for future enforcement.
 *
 * PATHS ARE REJECTED, NEVER TRUNCATED
 *   Every path handed to this API must be absolute. A path longer than
 *   VFS_PATH_MAX, or a single component longer than VFS_NAME_MAX, is an error —
 *   the VFS never silently shortens one, because a shortened path names a
 *   *different* file and "created the wrong thing and said ok" is the one
 *   failure a caller cannot detect.
 *
 * DEPENDENCIES
 *   kobject.h (identity/lifetime for vnodes/mounts/handles), heap, kernel.h.
 *
 * FUTURE EXTENSION POINTS
 *   The mount table supports several mounts (longest whole-component prefix
 *   wins), so mounting a disk filesystem under /mnt later needs no API change.
 *   Owner ids can be enforced once processes/users exist. A block device layer
 *   will let on-disk filesystems reuse this exact vnode_ops_t.
 */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include "kobject.h"

/* ---- error codes (errno-style negatives; 0 = success) ---- */
#define VFS_OK        0
#define VFS_ENOENT   -2    /* no such file or directory   */
#define VFS_EEXIST   -17   /* already exists              */
#define VFS_ENOTDIR  -20   /* a path component isn't a dir*/
#define VFS_EINVAL   -22   /* bad argument               */
#define VFS_ENOSPC   -28   /* out of space               */

/* ---- open() flags ----
 * The low two bits are the access mode and ARE enforced: vfs_read() on an
 * O_WRONLY handle and vfs_write() on an O_RDONLY handle both return
 * VFS_EINVAL. (The per-vnode VFS_PERM_* bits below are not enforced yet — they
 * need an owner to check against.) */
#define O_ACCMODE 0x0003
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

/* ---- seek() whence ---- */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ---- permission bits (stored now, enforced once users exist) ---- */
#define VFS_PERM_R 0x4
#define VFS_PERM_W 0x2
#define VFS_PERM_X 0x1

/* Longest single path component, and longest whole path, both excluding the
 * NUL. Anything longer is VFS_EINVAL. Callers that build paths of their own
 * (tools/vfs_tools.c) size their buffers against VFS_PATH_MAX rather than
 * against a number copied out of fs/vfs/vfs.c. */
#define VFS_NAME_MAX 63
#define VFS_PATH_MAX 127

typedef enum { VNODE_FILE = 1, VNODE_DIR = 2 } vnode_type_t;

typedef struct vfs_stat {
    vnode_type_t type;
    uint64_t     size;
    uint32_t     mode;      /* permission bits */
    uint64_t     mtime_ms;
} vfs_stat_t;

struct vnode;
struct vfs_mount;

/* Per-filesystem node operations — the implementation interface. Offsets/lengths
 * are byte counts; read/write return bytes transferred or a negative error. */
typedef struct vnode_ops {
    int     (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int     (*create)(struct vnode *dir, const char *name, vnode_type_t type,
                      struct vnode **out);
    int     (*unlink)(struct vnode *dir, const char *name);
    int64_t (*read)(struct vnode *n, void *buf, uint64_t off, uint64_t len);
    int64_t (*write)(struct vnode *n, const void *buf, uint64_t off, uint64_t len);
    int     (*readdir)(struct vnode *dir, uint32_t index, char *name_out, size_t cap);
    int     (*truncate)(struct vnode *n, uint64_t size);
    /* Release a vnode whose last reference has just gone away: free the
     * fs-private state AND the vnode itself. Called by the VFS from
     * vfs_close() only when kobject_put() reports 0, i.e. the name is already
     * gone and this was the final open handle. Optional — a filesystem whose
     * vnodes are immortal leaves it NULL. */
    void    (*reclaim)(struct vnode *n);
} vnode_ops_t;

/* An in-core node (inode). fs-private state hangs off `priv`. */
typedef struct vnode {
    kobject_t          obj;
    vnode_type_t       type;
    uint32_t           mode;      /* permission bits */
    uint64_t           size;
    uint64_t           mtime_ms;
    const vnode_ops_t *ops;
    struct vfs_mount  *mount;
    void              *priv;
} vnode_t;

/* A registered filesystem type. */
typedef struct vfs_fstype {
    const char *name;
    int (*mount)(struct vfs_mount *mnt);   /* build superblock, set mnt->root */
    struct vfs_fstype *next;
} vfs_fstype_t;

/* A mounted filesystem instance. */
typedef struct vfs_mount {
    kobject_t          obj;
    char               mountpoint[64];
    vfs_fstype_t      *fstype;
    vnode_t           *root;
    void              *priv;       /* fs-private superblock */
    struct vfs_mount  *next;
} vfs_mount_t;

/* An open file handle. */
typedef struct file {
    kobject_t obj;
    vnode_t  *node;
    uint64_t  pos;
    int       flags;
} file_t;

/* ---- public, filesystem-independent API ---- */
void vfs_init(void);
int  vfs_register_fstype(vfs_fstype_t *fs);
int  vfs_mount(const char *mountpoint, const char *fstype);

file_t *vfs_open(const char *path, int flags);
void    vfs_close(file_t *f);
int64_t vfs_read(file_t *f, void *buf, uint64_t len);
int64_t vfs_write(file_t *f, const void *buf, uint64_t len);
int64_t vfs_seek(file_t *f, int64_t off, int whence);

int  vfs_mkdir(const char *path);
int  vfs_stat(const char *path, vfs_stat_t *out);
int  vfs_readdir(const char *path, uint32_t index, char *name_out, size_t cap);

/* ---- helper for filesystem implementations ---- */
vnode_t *vfs_alloc_vnode(vfs_mount_t *mnt, vnode_type_t type,
                         const vnode_ops_t *ops, void *priv);

#endif /* VFS_H */
