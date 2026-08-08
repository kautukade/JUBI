/* fs.c — filesystem subsystem bring-up (see include/fs.h). */

#include "fs.h"
#include "vfs.h"
#include "block.h"
#include "kernel.h"
#include "rtc.h"
#include <string.h>

static int disk_mounted;

int fs_disk_mounted(void) { return disk_mounted; }

/* Remove an empty directory. The VFS publishes no vfs_rmdir(), so reach the
 * filesystem's unlink the same sanctioned way tools/vfs_tools.c does: open the
 * parent to obtain its vnode and go through the public ops table. */
static int remove_empty_dir(const char *parent, const char *leaf) {
    file_t *d = vfs_open(parent, O_RDONLY);
    if (!d) return VFS_ENOENT;
    int rc = VFS_EINVAL;
    if (d->node && d->node->type == VNODE_DIR && d->node->ops &&
        d->node->ops->unlink)
        rc = d->node->ops->unlink(d->node, leaf);
    vfs_close(d);
    return rc;
}

int fs_mount_disk(void) {
    block_device_t *bd = block_get(0);
    if (!bd) {
        kputs("fs: no disk attached - nothing this machine builds will "
              "survive a reboot\n");
        return VFS_ENOENT;
    }

    /* A ramfs directory of the same name, so that listing "/" shows where the
     * disk is even though the mount, not this directory, is what a path
     * resolves through (the VFS picks the longest matching mountpoint). */
    vfs_mkdir(FS_DISK_MOUNT);

    int rc = fat32_mount_disk(FS_DISK_MOUNT, bd, 1);
    if (rc != VFS_OK) {
        /* TAKE THE EMPTY DIRECTORY BACK, and take it back BEFORE saying the
         * mount failed, because everything downstream decides "does this machine
         * have durable storage?" by asking whether FS_DISK_MOUNT exists and is a
         * directory — core/capability.c, core/agenda.c and compiler/cc_store.c
         * all do, deliberately, so that they link without fs/ and can be host
         * tested against a synthetic one.
         *
         * Left in place, this ramfs leftover made a machine with an ATTACHED BUT
         * UNMOUNTABLE disk announce "survives a reboot" for all three stores,
         * two lines after saying it had no persistent storage. Worse, the claim
         * reached the MODEL: core/capability.c's panel writes "store /disk/cap
         * (survives reboot)" into capability_call's description, which ships in
         * every request, and net/chat.c's prompt tells the model that if a tool
         * says the store survives a reboot then it survives being switched off.
         * So the machine would teach itself capabilities and lose them, with no
         * error anywhere. Three documented, ordinary configurations reach it: a
         * non-FAT32 volume, a corrupt one, and a non-512-byte sector geometry.
         *
         * unlink through the parent's ops table because the VFS has no
         * vfs_rmdir(); tools/vfs_tools.c reaches deletion the same sanctioned
         * way. Best effort: if it cannot be removed the log says so rather than
         * leaving a silent lie. */
        int gone = remove_empty_dir("/", FS_DISK_MOUNT + 1);
        kprintf("fs: %s could not be mounted at %s (%d) - this boot has no "
                "persistent storage. The disks this machine can see:\n",
                bd->name, FS_DISK_MOUNT, rc);
        if (gone != VFS_OK)
            kprintf("fs: WARNING - the empty %s directory could not be removed "
                    "(%d), so anything that probes for it may still believe this "
                    "machine has durable storage. It does not.\n",
                    FS_DISK_MOUNT, gone);
        block_report();
        return rc;
    }
    disk_mounted = 1;
    kprintf("fs: %s mounted at %s - files written there survive a reboot\n",
            bd->name, FS_DISK_MOUNT);
    return 0;
}

int fs_init(void) {
    vfs_init();

    int rc = ramfs_register();      /* register available filesystems here */
    if (rc != VFS_OK) {
        kprintf("fs: registering ramfs failed (%d)\n", rc);
        return rc;
    }
    rc = fat32_register();
    if (rc != VFS_OK) {
        kprintf("fs: registering fat32 failed (%d)\n", rc);
        return rc;
    }

    rc = vfs_mount("/", "ramfs");
    if (rc != VFS_OK) {
        kprintf("fs: mounting ramfs at / failed (%d)\n", rc);
        return rc;
    }

    /* A machine with no disk is still a working machine; it just forgets.
     *
     * The boot log is appended here rather than from kernel_main so that the
     * proof of persistence lives next to the code that provides it: if the
     * disk mounted, the very next thing this kernel does is read back what a
     * previous, since-powered-off boot wrote. */
    if (fs_mount_disk() == 0) fs_disk_boot_log();
    return 0;
}

/* ---------------------------------------------------------------------- */
/* the boot log: the persistence proof, run on every boot                  */
/* ---------------------------------------------------------------------- */

/* Bounded on purpose. This file is appended to on every single boot, and an
 * unbounded log on a disk the operator cannot easily clean out is a slow leak
 * of their space. At ~40 bytes a line this keeps about a hundred boots. */
#define BOOTLOG_MAX     4096
#define BOOTLOG_SHOW    3
#define BOOTLOG_LS_MAX  16

/* EVERY BYTE THIS FUNCTION PRINTS CAME OFF A DISK, AND THAT MAKES IT UNTRUSTED.
 *
 * Two lines below assemble console output from stored bytes: the boot.log lines it
 * echoes back, and the filenames in the directory listing. Both are writable by the
 * model (tools/vfs_tools.c's write_file has no reserved-path list, and arg_path()
 * rejects only control bytes and NULs) and by the operator mounting the image on
 * their own machine, and both persist. So on every boot this reads attacker-chosen
 * bytes and prints them.
 *
 * Per include/trace.h a kernel statement is a '[' in COLUMN ZERO. The framebuffer
 * console is 128 columns and lib/base.c wraps by resetting the column to zero with
 * no newline, so a stored string long enough to reach the width boundary continues
 * at column zero of the next row. fat_name_ok() rejects /\:*?"<>| and control bytes
 * but ALLOWS '[' and ']', and a boot.log LINE has no character restrictions at all
 * — so a 124-character line ending "[vfs_write /etc/shadow bytes=9999 -> ok]"
 * renders a byte-exact forged kernel trace line, arrow included, in column zero.
 * Both were reproduced on a real FAT32 image and read off the framebuffer.
 *
 * gui/wm.c's set_title() already argued this exact case and concluded that counting
 * bytes against the console width "is not a defence anybody wrote down"; it folds
 * '[' to '{'. This does the same, and tools/agent_tools.c's copy_line() is a third
 * instance of the same rule. Nothing is lost: no reader parses a filename or a log
 * line out of the boot output, and the fold is visible rather than silent. */
static void print_stored(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7F) c = ' ';   /* no newlines: escape route one */
        else if (c == '[')         c = '{';   /* no forged statements: route three */
        else if (c == ']')         c = '}';
        kputc((char)c);
    }
}

static void stamp(char *dst, size_t cap) {
    rtc_time_t t;
    if (rtc_read(&t) == 0 && rtc_format_iso8601(&t, dst, cap) > 0) return;
    /* No clock: say so rather than print a made-up date. */
    strncpy(dst, "no-clock", cap - 1);
    dst[cap - 1] = '\0';
}

void fs_disk_boot_log(void) {
    if (!disk_mounted) return;

    const char *path = FS_DISK_MOUNT "/boot.log";
    file_t *f = vfs_open(path, O_RDWR | O_CREAT);
    if (!f) {
        kprintf("fs: could not open %s\n", path);
        return;
    }

    char *buf = kmalloc(BOOTLOG_MAX + 1);
    if (!buf) { vfs_close(f); return; }
    int64_t n = vfs_read(f, buf, BOOTLOG_MAX);
    if (n < 0) n = 0;
    buf[n] = '\0';

    unsigned boots = 0;
    for (int64_t i = 0; i < n; i++) if (buf[i] == '\n') boots++;

    kputs("\n--- persistence: /disk survives a reboot ---\n");
    if (boots == 0) {
        kprintf("%s is empty: this is the first boot on this disk\n", path);
    } else {
        kprintf("%s holds %u line(s) written by earlier boots:\n", path,
                boots);
        /* Show the last few, oldest first. */
        int64_t starts[BOOTLOG_SHOW];
        int nstart = 0;
        int64_t line_start = 0;
        for (int64_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (nstart < BOOTLOG_SHOW) starts[nstart++] = line_start;
                else {
                    for (int k = 1; k < BOOTLOG_SHOW; k++) starts[k - 1] = starts[k];
                    starts[BOOTLOG_SHOW - 1] = line_start;
                }
                line_start = i + 1;
            }
        }
        for (int k = 0; k < nstart; k++) {
            int64_t s = starts[k];
            int64_t e = s;
            while (e < n && buf[e] != '\n') e++;
            char save = buf[e];
            buf[e] = '\0';
            kputs("  | ");
            print_stored(buf + s);       /* stored bytes: see print_stored() */
            kputc('\n');
            buf[e] = save;
        }
    }

    /* Roll over rather than grow without limit. Reopening with O_TRUNC keeps
     * this on the public VFS API instead of reaching into vnode_ops_t. */
    if (n >= BOOTLOG_MAX - 128) {
        kprintf("%s reached %u bytes - starting it again\n", path, (unsigned)n);
        vfs_close(f);
        f = vfs_open(path, O_RDWR | O_TRUNC);
        if (!f) { kfree(buf); return; }
        boots = 0;
    }

    char when[40];
    stamp(when, sizeof when);
    char line[96];
    int len = 0;
    const char *pre = "boot ";
    for (const char *p = pre; *p; p++) line[len++] = *p;
    /* Small unsigned decimal by hand: the kernel's vsnprintf is reduced and
     * this is a fixed, tiny format. */
    char num[12];
    int nl = 0;
    unsigned bn = boots + 1;
    do { num[nl++] = (char)('0' + bn % 10); bn /= 10; } while (bn);
    while (nl) line[len++] = num[--nl];
    line[len++] = ' ';
    for (const char *p = when; *p && len < (int)sizeof line - 2; p++) line[len++] = *p;
    line[len++] = '\n';

    vfs_seek(f, 0, SEEK_END);
    int64_t w = vfs_write(f, line, (uint64_t)len);
    if (w != len) kprintf("fs: appending to %s failed (%d)\n", path, (int)w);
    else {
        line[len - 1] = '\0';
        kprintf("appended \"%s\" - it will be here after the next reboot\n", line);
    }
    vfs_close(f);
    kfree(buf);

    /* What is actually on the disk right now. This is the other half of the
     * proof: the names here were put there by an earlier boot, or by the
     * operator mounting the image on their own machine — the traffic goes both
     * ways, which is most of why the format is FAT32 and not something of our
     * own. Bounded, because a directory the model has been filling for a week
     * should not scroll the console away. */
    kprintf("ls %s :", FS_DISK_MOUNT);
    char name[VFS_NAME_MAX + 1];
    uint32_t i = 0;
    for (; i < BOOTLOG_LS_MAX; i++) {
        if (vfs_readdir(FS_DISK_MOUNT, i, name, sizeof name) != VFS_OK) break;
        kputc(' ');
        print_stored(name);              /* a filename is untrusted too */
    }
    if (i == BOOTLOG_LS_MAX &&
        vfs_readdir(FS_DISK_MOUNT, i, name, sizeof name) == VFS_OK)
        kputs(" ...");
    kputs("\n--------------------------------------------\n");
}
