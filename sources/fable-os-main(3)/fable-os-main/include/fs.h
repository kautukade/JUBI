/* fs.h — filesystem subsystem bring-up.
 *
 * PURPOSE
 *   The single entry point that initialises the VFS, registers the available
 *   filesystem implementations, and mounts a root. Keeps kernel_main free of
 *   filesystem-specific knowledge and keeps the VFS core unaware of which
 *   concrete filesystems exist.
 *
 * TWO FILESYSTEMS, TWO PURPOSES
 *   ramfs is mounted at "/": fast, bounded only by the heap, and GONE at the
 *   next boot. FAT32 is mounted at FS_DISK_MOUNT when a disk is attached, and
 *   is the only place on this machine where anything survives. The split is
 *   deliberate and visible in the path — a model that wrote to /tmp has kept
 *   nothing, and a model that wrote to /disk has.
 *
 * FUTURE EXTENSION POINTS
 *   As filesystems are added (ext2, a network filesystem) they get registered
 *   here; mounting them elsewhere needs no API change. If a second disk turns
 *   up, fs_mount_disk() is where it would be given a mountpoint of its own.
 */
#ifndef FS_H
#define FS_H

struct block_device;

/* Where a disk, if there is one, is mounted. */
#define FS_DISK_MOUNT "/disk"

/* Initialise VFS, register filesystems, mount ramfs at "/", and mount the
 * first block device at FS_DISK_MOUNT if one is present. Returns 0 on ok. A
 * machine with no disk still returns 0: persistence is a bonus, not a
 * prerequisite for booting. */
int fs_init(void);

/* Filesystem registration hooks (implemented by each fs). Return VFS_OK, or
 * VFS_EEXIST if that filesystem name is already registered. */
int ramfs_register(void);
int fat32_register(void);

/* Mount `bd` as FAT32 at `mountpoint`. When allow_format is non-zero AND the
 * device is blank (not merely unrecognised), it is formatted first. Returns
 * VFS_OK or a VFS error. */
int fat32_mount_disk(const char *mountpoint, struct block_device *bd,
                     int allow_format);

/* Mount block device 0 at FS_DISK_MOUNT. Called by fs_init(); exposed so a
 * test can drive it. Returns 0 on success, a VFS error otherwise. */
int fs_mount_disk(void);

/* Non-zero once a disk is mounted and this machine can therefore keep what it
 * builds. */
int fs_disk_mounted(void);

/* Append one line to FS_DISK_MOUNT/boot.log and print what earlier boots left
 * there. This is the persistence proof that runs on every boot: every line
 * above the last one was written by a machine that has since been switched
 * off. Prints nothing and does nothing when there is no disk. */
void fs_disk_boot_log(void);

#endif /* FS_H */
