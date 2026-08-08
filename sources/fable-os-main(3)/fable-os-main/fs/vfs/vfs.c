/* vfs.c — VFS core: fstype registry, mount table, path resolution, and the
 * filesystem-independent file operations. See include/vfs.h.
 *
 * This file knows nothing about how any filesystem stores data; it only walks
 * paths and dispatches through vnode_ops_t. Keep it that way. */

#include "vfs.h"
#include "kernel.h"
#include <string.h>

static vfs_fstype_t *fstypes;     /* registered filesystem types */
static vfs_mount_t  *mounts;      /* active mounts               */

void vfs_init(void) {
    fstypes = NULL;
    mounts  = NULL;
}

int vfs_register_fstype(vfs_fstype_t *fs) {
    for (vfs_fstype_t *t = fstypes; t; t = t->next)
        if (strcmp(t->name, fs->name) == 0) return VFS_EEXIST;
    fs->next = fstypes;
    fstypes  = fs;
    return VFS_OK;
}

vnode_t *vfs_alloc_vnode(vfs_mount_t *mnt, vnode_type_t type,
                         const vnode_ops_t *ops, void *priv) {
    vnode_t *v = kmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    kobject_init(&v->obj, KOBJ_INODE, type == VNODE_DIR ? "dir" : "file");
    v->type     = type;
    v->mode     = VFS_PERM_R | VFS_PERM_W | (type == VNODE_DIR ? VFS_PERM_X : 0);
    v->ops      = ops;
    v->mount    = mnt;
    v->priv     = priv;
    v->mtime_ms = millis();
    return v;
}

int vfs_mount(const char *mountpoint, const char *fstype) {
    vfs_fstype_t *t = fstypes;
    while (t && strcmp(t->name, fstype) != 0) t = t->next;
    if (!t) return VFS_ENOENT;

    if (!mountpoint || mountpoint[0] != '/') return VFS_EINVAL;
    if (strlen(mountpoint) >= sizeof ((vfs_mount_t *)0)->mountpoint)
        return VFS_EINVAL;

    vfs_mount_t *m = kmalloc(sizeof *m);
    memset(m, 0, sizeof *m);
    kobject_init(&m->obj, KOBJ_MOUNT, fstype);
    strncpy(m->mountpoint, mountpoint, sizeof m->mountpoint - 1);
    m->fstype = t;

    int rc = t->mount(m);
    if (rc != VFS_OK) { kfree(m); return rc; }

    m->next = mounts;
    mounts  = m;
    return VFS_OK;
}

/* Pick the mount whose mountpoint is the longest WHOLE-COMPONENT prefix of
 * `path`, and report how many bytes of `path` it accounts for. Matching raw
 * bytes would route "/dataset/f" into a mount at "/data", so the byte after the
 * prefix has to be a separator or the end of the path. The root mount accounts
 * for zero bytes, not one: stripping its '/' unconditionally is what used to
 * make "Xetc/motd" resolve to /etc/motd. */
static vfs_mount_t *mount_for(const char *path, size_t *prefix_len) {
    vfs_mount_t *best = NULL;
    size_t best_len = 0;
    for (vfs_mount_t *m = mounts; m; m = m->next) {
        size_t len = strlen(m->mountpoint);
        if (len == 1 && m->mountpoint[0] == '/') {
            len = 0;                       /* the root mount strips nothing */
        } else {
            if (strncmp(path, m->mountpoint, len) != 0) continue;
            if (path[len] != '\0' && path[len] != '/') continue;
        }
        if (!best || len >= best_len) { best = m; best_len = len; }
    }
    if (best) *prefix_len = best_len;
    return best;
}

/* Copy the next '/'-delimited component of *pp into comp, advancing *pp past
 * it. Returns 1 on a component, 0 when none remains, -1 if the component is
 * longer than the buffer — truncating would look up a *different* name, and
 * ramfs_create() rejects over-long names on the store side for the same
 * reason, so the two layers agree. */
static int next_component(const char **pp, char *comp, size_t cap) {
    const char *p = *pp;
    while (*p == '/') p++;
    if (!*p) { *pp = p; return 0; }
    size_t n = 0;
    while (*p && *p != '/') { if (n >= cap - 1) return -1; comp[n++] = *p; p++; }
    comp[n] = '\0';
    *pp = p;
    return 1;
}

/* Resolve an absolute path to its vnode, or NULL. */
static vnode_t *resolve(const char *path) {
    if (!path || path[0] != '/') return NULL;   /* absolute paths only */
    if (strlen(path) > VFS_PATH_MAX) return NULL;

    size_t plen = 0;
    vfs_mount_t *m = mount_for(path, &plen);
    if (!m) return NULL;

    const char *rel = path + plen;
    vnode_t *cur = m->root;
    char comp[VFS_NAME_MAX + 1];
    int rc;
    while ((rc = next_component(&rel, comp, sizeof comp)) == 1) {
        if (strcmp(comp, ".") == 0) continue;
        if (cur->type != VNODE_DIR || !cur->ops->lookup) return NULL;
        vnode_t *next = NULL;
        if (cur->ops->lookup(cur, comp, &next) != VFS_OK) return NULL;
        cur = next;
    }
    if (rc < 0) return NULL;                    /* component too long */
    return cur;
}

/* Split `path` into its parent directory vnode and the final component. Both
 * halves are bounded and neither is ever truncated: a truncated parent would
 * resolve to some *other* directory and the create would silently land in the
 * wrong place. */
static int resolve_parent(const char *path, vnode_t **parent, char *leaf, size_t cap) {
    if (!path || path[0] != '/') return VFS_EINVAL;
    if (strlen(path) > VFS_PATH_MAX) return VFS_EINVAL;

    /* Find the last '/'. */
    const char *slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    if (!slash) return VFS_EINVAL;

    /* Copy the leaf name. */
    const char *name = slash + 1;
    if (!*name) return VFS_EINVAL;
    size_t n = 0;
    while (name[n]) { if (n >= cap - 1) return VFS_EINVAL; leaf[n] = name[n]; n++; }
    leaf[n] = '\0';

    /* Parent path is everything up to the slash (or "/" itself). */
    char dir[VFS_PATH_MAX + 1];
    size_t dlen = (size_t)(slash - path);
    if (dlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    else {
        if (dlen >= sizeof dir) return VFS_EINVAL;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
    }
    vnode_t *p = resolve(dir);
    if (!p) return VFS_ENOENT;
    if (p->type != VNODE_DIR) return VFS_ENOTDIR;
    *parent = p;
    return VFS_OK;
}

file_t *vfs_open(const char *path, int flags) {
    vnode_t *v = resolve(path);

    if (!v && (flags & O_CREAT)) {
        vnode_t *parent;
        char leaf[VFS_NAME_MAX + 1];
        if (resolve_parent(path, &parent, leaf, sizeof leaf) != VFS_OK) return NULL;
        if (!parent->ops->create) return NULL;
        if (parent->ops->create(parent, leaf, VNODE_FILE, &v) != VFS_OK) return NULL;
    }
    if (!v) return NULL;

    if (v->type == VNODE_FILE && (flags & O_TRUNC) && v->ops->truncate) {
        v->ops->truncate(v, 0);
    }

    file_t *f = kmalloc(sizeof *f);
    memset(f, 0, sizeof *f);
    kobject_init(&f->obj, KOBJ_FILE, "fd");
    kobject_get(&v->obj);                       /* handle holds a reference */
    f->node  = v;
    f->flags = flags;
    f->pos   = (flags & O_APPEND) ? v->size : 0;
    return f;
}

void vfs_close(file_t *f) {
    if (!f) return;
    vnode_t *v = f->node;
    kfree(f);
    /* Dropping the last reference means the name is already gone (an unlink
     * happened while this handle was open) and we are the last owner, so the
     * filesystem gets to reclaim the storage. Ignoring this return is how a
     * refcount becomes decorative. */
    if (v && kobject_put(&v->obj) == 0 && v->ops && v->ops->reclaim)
        v->ops->reclaim(v);
}

int64_t vfs_read(file_t *f, void *buf, uint64_t len) {
    if (!f || !f->node || !buf) return VFS_EINVAL;
    if (!f->node->ops->read) return VFS_EINVAL;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return VFS_EINVAL;
    int64_t n = f->node->ops->read(f->node, buf, f->pos, len);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

int64_t vfs_write(file_t *f, const void *buf, uint64_t len) {
    if (!f || !f->node || !buf) return VFS_EINVAL;
    if (!f->node->ops->write) return VFS_EINVAL;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return VFS_EINVAL;
    if (f->flags & O_APPEND) f->pos = f->node->size;
    int64_t n = f->node->ops->write(f->node, buf, f->pos, len);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

int64_t vfs_seek(file_t *f, int64_t off, int whence) {
    if (!f || !f->node) return VFS_EINVAL;
    int64_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int64_t)f->pos; break;
        case SEEK_END: base = (int64_t)f->node->size; break;
        default: return VFS_EINVAL;
    }
    int64_t np = base + off;
    if (np < 0) return VFS_EINVAL;
    f->pos = (uint64_t)np;
    return np;
}

int vfs_mkdir(const char *path) {
    if (resolve(path)) return VFS_EEXIST;
    vnode_t *parent;
    char leaf[VFS_NAME_MAX + 1];
    int rc = resolve_parent(path, &parent, leaf, sizeof leaf);
    if (rc != VFS_OK) return rc;
    if (!parent->ops->create) return VFS_EINVAL;
    vnode_t *out;
    return parent->ops->create(parent, leaf, VNODE_DIR, &out);
}

int vfs_stat(const char *path, vfs_stat_t *out) {
    if (!out) return VFS_EINVAL;
    vnode_t *v = resolve(path);
    if (!v) return VFS_ENOENT;
    out->type     = v->type;
    out->size     = v->size;
    out->mode     = v->mode;
    out->mtime_ms = v->mtime_ms;
    return VFS_OK;
}

int vfs_readdir(const char *path, uint32_t index, char *name_out, size_t cap) {
    /* cap == 0 would reach the filesystem as a `cap - 1` bound that underflows
     * to SIZE_MAX, i.e. an unbounded copy into the caller's buffer. */
    if (!name_out || cap == 0) return VFS_EINVAL;
    vnode_t *v = resolve(path);
    if (!v) return VFS_ENOENT;
    if (v->type != VNODE_DIR || !v->ops->readdir) return VFS_ENOTDIR;
    return v->ops->readdir(v, index, name_out, cap);
}
