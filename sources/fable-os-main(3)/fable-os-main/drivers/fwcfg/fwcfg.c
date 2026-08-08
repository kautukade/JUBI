/* fwcfg.c — QEMU fw_cfg driver, and the only path a secret takes into this
 * kernel. See include/fwcfg.h for the wire format, the port map, and THE SECRET
 * RULE that governs everything in section 4.
 *
 * LAYOUT OF THIS FILE
 *   1. the port backend (the only thing that touches an I/O port)
 *   2. the protocol — signature, feature bitmap, directory walk, blob read
 *   3. cleaning a secret — trim, then refuse anything unfit for an HTTP header
 *   4. the API key — one RAM buffer, one accessor, one honest log line
 *   5. the driver — REGISTER_DRIVER, the device node, boot-time load
 *
 * Sections 1-4 compile on the host as-is (tests/host/test_fwcfg.c drives them
 * against a synthetic fw_cfg device); section 5 is kernel-only.
 *
 * ============================================================================
 * DECISION: nothing in this file prints, hashes, or fingerprints the key.
 * ============================================================================
 *   Not a prefix, not the last four characters, not a length-and-checksum. A
 *   prefix is the part that identifies the account; a suffix plus a length is a
 *   search key; and a console log on this machine is a serial line the operator
 *   is very likely piping into a file. The only fact printed is a byte count,
 *   and the only reason that is printed at all is that "did the key arrive?"
 *   must be answerable without an oracle that leaks the answer's content.
 */

#include "fwcfg.h"
#include "kernel.h"

#ifndef FABLEOS_HOSTTEST
#  include "io.h"
#  include "driver.h"
#  include "device.h"
#endif

/* ====================================================================== */
/* 1. the port backend                                                    */
/* ====================================================================== */

#ifndef FABLEOS_HOSTTEST
static void port_select(void *ctx, uint16_t key) {
    (void)ctx;
    outw(FWCFG_PORT_SEL, key);
}
static uint8_t port_read8(void *ctx) {
    (void)ctx;
    return inb(FWCFG_PORT_DATA);
}
static const fwcfg_backend_t default_backend = {
    port_select, port_read8, (void *)0
};
#else
/* Host builds have no ports. Every read returns 0xFF, so the signature never
 * matches and fwcfg_present() says no — the same answer a machine with no
 * fw_cfg gives. Tests install a synthetic device with fwcfg_set_backend(). */
static void absent_select(void *ctx, uint16_t key) { (void)ctx; (void)key; }
static uint8_t absent_read8(void *ctx) { (void)ctx; return 0xFF; }
static const fwcfg_backend_t default_backend = {
    absent_select, absent_read8, (void *)0
};
#endif

static fwcfg_backend_t g_be = { 0, 0, 0 };
static int g_be_set = 0;

/* -1 = not probed yet, 0 = absent, 1 = present. */
static int      g_present = -1;
static uint32_t g_features = 0;

static const fwcfg_backend_t *be(void) {
    return g_be_set ? &g_be : &default_backend;
}

void fwcfg_set_backend(const fwcfg_backend_t *b) {
    if (b && b->select && b->read8) {
        g_be = *b;
        g_be_set = 1;
    } else {
        g_be_set = 0;
    }
    /* A different device may answer differently. Do not carry a verdict, or a
     * feature bitmap, across the swap. */
    g_present  = -1;
    g_features = 0;
}

static void sel(uint16_t key) {
    const fwcfg_backend_t *b = be();
    b->select(b->ctx, key);
}

static uint8_t rd8(void) {
    const fwcfg_backend_t *b = be();
    return b->read8(b->ctx);
}

static void rd(void *dst, uint32_t len) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++) p[i] = rd8();
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}

/* ====================================================================== */
/* 2. the protocol                                                        */
/* ====================================================================== */

int fwcfg_present(void) {
    if (g_present >= 0) return g_present;

    uint8_t sig[4];
    sel(FWCFG_SEL_SIGNATURE);
    rd(sig, sizeof sig);
    g_present = memcmp(sig, FWCFG_SIGNATURE, 4) == 0 ? 1 : 0;

    if (g_present) {
        uint8_t id[4];
        sel(FWCFG_SEL_ID);
        rd(id, sizeof id);
        /* The feature bitmap is LITTLE-endian, unlike the file directory. That
         * asymmetry is in the interface, not a bug here. */
        g_features = le32(id);
    }
    return g_present;
}

uint32_t fwcfg_features(void) {
    return fwcfg_present() ? g_features : 0;
}

/* One directory entry, exactly as it appears on the wire. */
#define DIR_ENTRY_BYTES 64
typedef struct {
    uint32_t size;
    uint16_t select;
    char     name[FWCFG_NAME_MAX];
} fwcfg_dirent_t;

/* Select the directory and read its count. Returns the number of entries that
 * will be read (clamped to nothing — an absurd count is refused outright, since
 * reading 4 GiB of directory a byte at a time is a hang, and a hang is the one
 * failure mode a boot cannot report), or a negative status. */
static int dir_open(void) {
    if (!fwcfg_present()) return FWCFG_ENODEV;

    uint8_t hdr[4];
    sel(FWCFG_SEL_FILE_DIR);
    rd(hdr, sizeof hdr);
    uint32_t count = be32(hdr);
    if (count > FWCFG_MAX_ENTRIES) return FWCFG_EPROTO;
    return (int)count;
}

/* Read the next entry from an open directory. Returns 0 and fills `e`, or -1 if
 * the entry cannot be believed (unterminated name, size past FWCFG_FILE_MAX, or
 * selector 0, which is the signature and can never name a file). A rejected
 * entry is skipped, not fatal: a directory with one bad row still has good ones,
 * and the stream stays in sync because the whole 64 bytes were consumed. */
static int dir_next(fwcfg_dirent_t *e) {
    uint8_t raw[DIR_ENTRY_BYTES];
    rd(raw, sizeof raw);

    uint32_t size = be32(raw);
    uint16_t s    = be16(raw + 4);
    /* raw[6..7] is reserved. */

    if (s == 0)                  return -1;
    if (size > FWCFG_FILE_MAX)   return -1;

    /* The name must be NUL-terminated inside its 56 bytes; anything else is a
     * string this kernel refuses to have an opinion about. */
    int n = -1;
    for (int i = 0; i < FWCFG_NAME_MAX; i++) {
        if (raw[8 + i] == 0) { n = i; break; }
    }
    if (n < 0) return -1;

    e->size   = size;
    e->select = s;
    memcpy(e->name, raw + 8, FWCFG_NAME_MAX);
    e->name[FWCFG_NAME_MAX - 1] = '\0';
    return 0;
}

int fwcfg_file_count(void) {
    int rc = dir_open();
    return rc < 0 ? 0 : rc;
}

/* Bounded, NUL-respecting equality. Not strcmp: `want` comes from a caller and
 * `have` came off a wire, and neither is trusted to be shorter than the field. */
static int name_eq(const char *have, const char *want) {
    int i = 0;
    for (; i < FWCFG_NAME_MAX; i++) {
        if (have[i] != want[i]) return 0;
        if (have[i] == '\0')    return 1;
    }
    return 0;   /* ran out of field without agreeing on a terminator */
}

int fwcfg_read_file(const char *name, void *buf, uint32_t cap,
                    uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!name || !name[0] || !buf || cap == 0) return FWCFG_EINVAL;

    int count = dir_open();
    if (count < 0) return count;

    fwcfg_dirent_t hit;
    int found = 0;
    for (int i = 0; i < count && !found; i++) {
        fwcfg_dirent_t e;
        if (dir_next(&e) != 0) continue;          /* unusable row; keep looking */
        if (name_eq(e.name, name)) { hit = e; found = 1; }
    }
    if (!found) return FWCFG_ENOENT;

    /* Refuse BEFORE reading. The size came from the host; believing it far
     * enough to start copying is how a 4-byte buffer takes 4 GiB. */
    if (hit.size > cap) {
        if (out_len) *out_len = hit.size;
        return FWCFG_E2BIG;
    }

    sel(hit.select);
    rd(buf, hit.size);
    if (out_len) *out_len = hit.size;
    return FWCFG_OK;
}

/* ====================================================================== */
/* 3. cleaning a secret                                                   */
/* ====================================================================== */

/* Trimmable: the bytes a text file wraps its content in. NUL is included
 * because a blob may be padded, and a padded key is still a key. */
static int trimmable(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f' || c == '\0';
}

/* Fit for an HTTP header value: printable ASCII, no space. Excluding space is
 * stricter than RFC 7230 needs, and correct for this one use — a credential
 * with a space in it is a .env quoting mistake, not a credential. */
static int header_safe(uint8_t c) {
    return c >= 0x21 && c <= 0x7E;
}

/* Overwrite in a way the compiler may not elide. */
static void wipe(void *p, uint32_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) v[i] = 0;
}

/* Trim `len` bytes at `buf` in place, NUL-terminate, and return the remaining
 * length; FWCFG_EEMPTY if nothing is left, FWCFG_EINVAL if what is left cannot
 * go in a header. `cap` must exceed `len` so the terminator always has a home.
 * On any failure the buffer is wiped, so a rejected key does not linger. */
static int clean_secret(char *buf, uint32_t len, uint32_t cap) {
    if (!buf || cap <= len) return FWCFG_EINVAL;

    uint32_t start = 0, end = len;
    while (start < end && trimmable((uint8_t)buf[start])) start++;
    while (end > start && trimmable((uint8_t)buf[end - 1])) end--;

    uint32_t n = end - start;
    if (n == 0) { wipe(buf, cap); return FWCFG_EEMPTY; }

    for (uint32_t i = 0; i < n; i++) {
        if (!header_safe((uint8_t)buf[start + i])) { wipe(buf, cap); return FWCFG_EINVAL; }
    }

    if (start > 0) memmove(buf, buf + start, n);
    wipe(buf + n, cap - n);
    return (int)n;
}

/* ====================================================================== */
/* 4. the API key                                                         */
/* ====================================================================== */

/* The only copy in the machine. Not const, not exported, and never passed
 * anywhere but net.c's x-api-key header. */
static char g_key[FWCFG_KEY_MAX];
static int  g_key_len = 0;

int fwcfg_apikey_load(void) {
    wipe(g_key, sizeof g_key);
    g_key_len = 0;

    uint32_t n = 0;
    int rc = fwcfg_read_file(FWCFG_APIKEY_FILE, g_key, sizeof g_key - 1, &n);
    if (rc != FWCFG_OK) {
        /* E2BIG may have left nothing, but a partial read on some future path
         * must not leave key bytes in a buffer the accessor still points at. */
        wipe(g_key, sizeof g_key);
        return rc;
    }

    int len = clean_secret(g_key, n, sizeof g_key);
    if (len < 0) return len;      /* already wiped by clean_secret */

    g_key_len = len;
    return len;
}

const char *fwcfg_apikey(void) {
    return g_key_len > 0 ? g_key : "";
}

int fwcfg_apikey_len(void) {
    return g_key_len;
}

const char *fwcfg_status_str(int rc) {
    switch (rc) {
        case FWCFG_OK:     return "ok";
        case FWCFG_ENODEV: return "no fw_cfg interface on this machine";
        case FWCFG_ENOENT: return "not supplied by the host";
        case FWCFG_E2BIG:  return "too large to be a key";
        case FWCFG_EINVAL: return "contains characters that cannot go in an "
                                  "HTTP header";
        case FWCFG_EPROTO: return "the fw_cfg file directory is malformed";
        case FWCFG_EEMPTY: return "empty";
        default:           return "unknown error";
    }
}

void fwcfg_apikey_report(int rc) {
    if (rc > 0) {
        /* A length, and nothing else. See the DECISION at the top of the file. */
        kprintf("fwcfg: api key: %d bytes loaded from fw_cfg (%s)\n",
                rc, FWCFG_APIKEY_FILE);
        return;
    }
    kprintf("fwcfg: no api key (%s: %s) - requests will go out unauthenticated "
            "and the API will answer 401\n",
            FWCFG_APIKEY_FILE, fwcfg_status_str(rc));
}

/* ====================================================================== */
/* 5. the driver                                                          */
/* ====================================================================== */

#ifndef FABLEOS_HOSTTEST

static const driver_t fwcfg_driver;

/* DRV_LEVEL_EARLY, immediately after the serial console: the key has to be in
 * RAM before net_init() builds a request, and a configuration channel is the
 * kind of thing later drivers will want to read their own settings from. It
 * needs nothing itself except two I/O ports, which exist from reset. */
static int fwcfg_init(void) {
    device_t *d = device_create("fwcfg0", DEV_CLASS_SYSTEM);
    device_add_resource(d, RES_IOPORT, FWCFG_PORT_SEL, 2);
    device_register(d);
    device_bind_driver(d, (driver_t *)&fwcfg_driver);

    if (!fwcfg_present()) {
        /* Not QEMU, or a machine type without the interface. The kernel boots,
         * talks, and cannot authenticate — which is exactly the state it is in
         * with no key, and is reported the same way. */
        kprintf("fwcfg: no QEMU fw_cfg interface at 0x%x - no runtime "
                "configuration channel on this machine\n", FWCFG_PORT_SEL);
        device_set_state(d, DEV_STATE_FAILED);
        fwcfg_apikey_report(FWCFG_ENODEV);
        return -1;
    }

    kprintf("fwcfg: QEMU fw_cfg at 0x%x/0x%x, %d file(s), features 0x%x\n",
            FWCFG_PORT_SEL, FWCFG_PORT_DATA, fwcfg_file_count(),
            (unsigned)fwcfg_features());
    device_set_state(d, DEV_STATE_ACTIVE);

    fwcfg_apikey_report(fwcfg_apikey_load());
    return 0;
}

static const driver_t fwcfg_driver = {
    .name  = "fwcfg",
    .level = DRV_LEVEL_EARLY,
    .cls   = DEV_CLASS_SYSTEM,
    .init  = fwcfg_init,
};
REGISTER_DRIVER(fwcfg_driver);

#endif /* !FABLEOS_HOSTTEST */
