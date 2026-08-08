/* fwcfg.h — QEMU fw_cfg: the host->guest configuration channel, and the one
 * way a secret reaches this kernel.
 *
 * PURPOSE
 *   Read named blobs the hypervisor hands the guest at boot. The kernel needs
 *   exactly one of them — the Anthropic API key — and needs it to arrive
 *   WITHOUT ever being part of a build artifact.
 *
 *   Before this existed the key was compiled in (`-DFABLEOS_API_KEY`), which put
 *   it inside net.o, kernel.elf, kernel.bin, fableos.iso and the .build-flags
 *   stamp. That is a secret in five files that a `git add -f`, a CI cache or a
 *   shared build directory publishes. A .gitignore is not a safety property.
 *   Now the key travels host -> guest at RUN time, over I/O ports, into RAM
 *   only. `make` cannot bake it in because `make` never sees it.
 *
 * THE HARDWARE
 *   Two I/O ports, on every QEMU x86 machine (i440fx and q35 alike):
 *
 *     0x510  selector, 16-bit write. Selecting a key also rewinds its read
 *            offset to 0 — there is no seek, so a blob is read once, forwards.
 *     0x511  data, 8-bit reads. Reading past the end of a blob returns zeros.
 *
 *   Well-known selectors: 0x0000 signature (4 bytes, "QEMU"), 0x0001 feature
 *   bitmap (u32 LE, bit 1 = the DMA interface), 0x0019 the file directory.
 *
 *   The directory is big-endian on the wire regardless of host endianness:
 *
 *     u32be count
 *     count x { u32be size; u16be select; u16 reserved; char name[56]; }
 *
 *   Custom blobs (`qemu -fw_cfg name=opt/fableos/apikey,file=...`) are required
 *   by QEMU to live under "opt/", which is exactly the namespace reserved for
 *   things firmware does not define. No leading slash.
 *
 *   The DMA interface (0x514) is deliberately NOT used: it needs a physical
 *   address, a control word and a poll loop to move blobs measured in tens of
 *   bytes. Byte-at-a-time reads of a 108-byte key cost microseconds once, at
 *   boot. See FUTURE EXTENSION POINTS.
 *
 * RESPONSIBILITIES
 *   - Detect the interface (signature) and report it honestly when absent.
 *   - Walk the file directory and read one named blob into a caller's buffer,
 *     with every length bounded before it is believed.
 *   - Hold the API key in RAM and hand it to net.c, and to nothing else.
 *
 * PUBLIC API
 *   fwcfg_present / fwcfg_features / fwcfg_file_count  — what is there.
 *   fwcfg_read_file          — read a named blob; every failure is a code.
 *   fwcfg_apikey_load        — read + clean the key; returns its length.
 *   fwcfg_apikey             — the key, or "" when there is none.
 *   fwcfg_apikey_len         — its length, 0 when there is none.
 *   fwcfg_apikey_report      — print one line about the key, never the key.
 *   fwcfg_status_str         — a status code as a sentence fragment.
 *   fwcfg_set_backend        — swap the port I/O for a synthetic device.
 *
 * THE SECRET RULE — this header is where it is enforced
 *   fwcfg_apikey() returns the only copy of the key in the machine. It must
 *   never be printed, traced, written to a vnode, put in a tool result, or
 *   passed to anything that might. It has exactly one caller: the x-api-key
 *   header in net/net.c. Nothing in this module prints it, not even truncated,
 *   not even on an error path — "108 bytes loaded" is the whole diagnostic, and
 *   a length is not a secret.
 *
 *   A key is also refused rather than trusted if it contains anything outside
 *   printable ASCII, because it is spliced into an HTTP header: one CR or LF
 *   and a .env typo becomes request splitting. Surrounding whitespace is
 *   stripped instead of refused, since a `.env` file ends in a newline and that
 *   is not the operator's mistake.
 *
 * DEPENDENCIES
 *   kernel.h (kprintf, memcpy/memcmp). In the kernel additionally io.h for the
 *   ports and driver.h/device.h to publish the device node. Nothing else: the
 *   protocol above is pure logic and compiles on the host as-is, which is how
 *   tests/host/test_fwcfg.c drives a synthetic fw_cfg with no QEMU.
 *
 * FUTURE EXTENSION POINTS
 *   - fwcfg_read_file() is generic: any future blob under opt/fableos (a static
 *     IP, a system prompt, a DVM program to run at boot) is one call, no code.
 *   - The DMA interface belongs here if a blob ever gets big enough to care —
 *     it is a strictly additive fast path behind the same function.
 *   - Writable fw_cfg entries would let the guest hand results back to the
 *     host; nothing needs that yet and it is a channel out, so it stays shut.
 *   - REAL HARDWARE HAS NO fw_cfg. A physical machine booted from fableos.iso
 *     reports "no fw_cfg interface", gets no key, and every request it makes
 *     is answered 401. Giving it one needs a different channel — a prompt on
 *     the console (which puts the key in the scrollback of a machine whose only
 *     interface is a transcript), a file on the boot medium (which puts it back
 *     on disk, the thing this module exists to avoid), or the kernel keyboard
 *     with echo suppressed, which is the one worth building. None of them is
 *     this file's problem: fwcfg_apikey() is the seam, and whatever fills the
 *     buffer must obey THE SECRET RULE above.
 */
#ifndef FWCFG_H
#define FWCFG_H

#include <stdint.h>

/* ---- ports and well-known selectors ---- */
#define FWCFG_PORT_SEL    0x510
#define FWCFG_PORT_DATA   0x511
#define FWCFG_SEL_SIGNATURE 0x0000
#define FWCFG_SEL_ID        0x0001
#define FWCFG_SEL_FILE_DIR  0x0019
#define FWCFG_SIGNATURE   "QEMU"

/* fw_cfg's own limit: the name field of a directory entry is 56 bytes and must
 * be NUL-terminated inside it. */
#define FWCFG_NAME_MAX    56

/* Sanity caps. QEMU's default is 32 file slots and this kernel reads one blob
 * of ~100 bytes, so both are orders of magnitude of headroom — they exist to
 * bound a directory that a broken (or hostile) host says is enormous, not to
 * express a real limit. */
#define FWCFG_MAX_ENTRIES 512
#define FWCFG_FILE_MAX    65536

/* The key blob and the buffer it lands in. Anthropic keys are ~108 characters;
 * 512 leaves room for a longer scheme without leaving room for a mistake. */
#define FWCFG_APIKEY_FILE "opt/fableos/apikey"
#define FWCFG_KEY_MAX     512

/* Status codes. Negative is a failure, and every one of them is a sentence the
 * boot log can print (fwcfg_status_str). fwcfg_apikey_load() returns a POSITIVE
 * length on success instead of FWCFG_OK. */
enum {
    FWCFG_OK     =  0,
    FWCFG_ENODEV = -1,   /* no fw_cfg interface: signature absent or wrong    */
    FWCFG_ENOENT = -2,   /* no directory entry with that name                 */
    FWCFG_E2BIG  = -3,   /* blob is larger than the buffer (or than FILE_MAX) */
    FWCFG_EINVAL = -4,   /* bad arguments, or content that may not be used    */
    FWCFG_EPROTO = -5,   /* the directory itself is not believable            */
    FWCFG_EEMPTY = -6,   /* the blob exists and is empty (after cleaning)     */
};

/* ---- injectable port backend ----
 * Port I/O cannot run in a host process (and does not assemble at all on the
 * arm64 Macs this is developed on), so every access goes through these two
 * function pointers. tests/host/test_fwcfg.c installs a synthetic fw_cfg
 * device; the kernel installs 0x510/0x511. Passing NULL restores the default.
 * Swapping the backend also forgets any cached probe result. */
typedef struct fwcfg_backend {
    void    (*select)(void *ctx, uint16_t key);
    uint8_t (*read8)(void *ctx);
    void    *ctx;
} fwcfg_backend_t;

void fwcfg_set_backend(const fwcfg_backend_t *b);

/* 1 if the interface answered with the "QEMU" signature. Probes once and
 * caches; cheap to call repeatedly. */
int      fwcfg_present(void);

/* The feature bitmap (selector 0x0001), 0 when absent. Bit 1 is DMA. */
uint32_t fwcfg_features(void);

/* Number of entries the file directory claims, clamped to FWCFG_MAX_ENTRIES.
 * 0 when there is no interface or the directory is not believable. */
int      fwcfg_file_count(void);

/* Read the blob named `name` into `buf`. On FWCFG_OK, *out_len is the number of
 * bytes written (0 is a legal answer: an empty blob is not an error here).
 * On FWCFG_E2BIG, *out_len is set to the size the directory claimed, so a
 * caller can say how much would have been needed. Nothing is written to `buf`
 * unless the whole blob fits. `out_len` may be NULL. */
int      fwcfg_read_file(const char *name, void *buf, uint32_t cap,
                         uint32_t *out_len);

/* Load FWCFG_APIKEY_FILE into this module's RAM buffer, strip surrounding
 * whitespace, and refuse anything that is not printable ASCII. Returns the
 * key's length (> 0) or a negative status. Safe to call more than once; a
 * failed call leaves no key behind. */
int      fwcfg_apikey_load(void);

/* The key. "" when there is none — never NULL, so a caller cannot forget to
 * check and splice a null pointer into a request. READ THE SECRET RULE ABOVE
 * before you do anything with this besides sending it to Anthropic. */
const char *fwcfg_apikey(void);
int         fwcfg_apikey_len(void);

/* One line for the boot log about the key: how many bytes, or why there are
 * none. Takes fwcfg_apikey_load()'s return value. Prints no key material. */
void        fwcfg_apikey_report(int rc);

/* "no fw_cfg interface", "not present", ... — for logs. Never NULL. */
const char *fwcfg_status_str(int rc);

#endif /* FWCFG_H */
