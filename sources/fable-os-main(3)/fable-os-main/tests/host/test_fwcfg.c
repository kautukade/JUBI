/* test_fwcfg.c — the QEMU fw_cfg driver and the API key path, on the host.
 *
 * Runs the REAL drivers/fwcfg/fwcfg.c. Nothing here reimplements the protocol;
 * what this file supplies is the other end of the wire.
 *
 * Port I/O cannot run in a host process (and `inb`/`outw` do not even assemble
 * on the arm64 Macs this is developed on), so fwcfg.c routes every access
 * through two function pointers — fwcfg_set_backend(), see include/fwcfg.h.
 * This file installs a SYNTHETIC fw_cfg device in their place: a signature, a
 * feature bitmap, and a file directory assembled byte-for-byte in fw_cfg's own
 * big-endian wire format, served one byte at a time from an offset that a
 * selector write rewinds — exactly like the hardware, including the part where
 * reading past the end of a blob yields zeros rather than an error.
 *
 * That fidelity is the point. Every interesting failure of this driver is a
 * LENGTH the host asserted and the guest believed: a directory that claims four
 * billion entries, an entry that claims a four-gigabyte file, a name field with
 * no terminator. On real hardware none of those can be produced on demand; here
 * each one is three lines of struct initialiser.
 *
 * WHAT THIS SUITE IS BUILT TO PROVE
 *
 *   1. The interface is detected, not assumed. A wrong signature, or no device
 *      at all, means "no key" and a machine that still boots — never a hang and
 *      never a read of a blob that was never there.
 *
 *   2. No host-supplied number is believed before it is bounded. An absurd
 *      directory count is refused instead of walked (walking it a byte at a
 *      time is a boot that never finishes, and a hang is the one failure a boot
 *      cannot report). An oversized blob is refused BEFORE a single byte is
 *      copied, so the caller's buffer is untouched.
 *
 *   3. One unusable directory entry does not cost the rest of the directory.
 *      The stream stays in sync because a rejected entry is still consumed.
 *
 *   4. The key is cleaned, and the cleaning is a security property, not
 *      cosmetics. A `.env` file ends in a newline; that newline in an HTTP
 *      header is request splitting. Surrounding whitespace is stripped; an
 *      embedded CR, LF or NUL means the whole key is REFUSED, because a
 *      credential that can inject a header is worse than no credential.
 *
 *   5. Nothing ever prints the key. Not the success line, not any of the six
 *      error lines. Every message this module can emit is captured (kcap_text)
 *      and searched for the key material, including a 3-character prefix of it.
 *
 * NOTE ON THE FIXTURES: every fake key here starts "notkey-", never "sk-ant-".
 * Two reasons, both practical. A repository full of `sk-ant-api03-<95 chars>`
 * string literals trips every secret scanner that exists, and the one that
 * cried wolf is the one nobody reads. And `grep -r sk-ant` over the tree and
 * over every build artifact is the check that proves the key never enters the
 * build (see the README) — test data must not be what makes it noisy.
 */

#include "harness.h"
#include "fwcfg.h"

#include <string.h>

/* ====================================================================== */
/* a synthetic fw_cfg device                                              */
/* ====================================================================== */

#define FF_MAX 8

typedef struct {
    const char *name;        /* directory name (NULL terminates the list)   */
    const char *data;        /* blob contents                               */
    int         len;         /* bytes actually served (-1 -> strlen(data))   */
    uint32_t    claim;       /* size written into the directory (0 -> len)   */
    int         zero_select; /* publish selector 0, which can never be a file */
    int         unterminated;/* fill all 56 name bytes, leaving no NUL       */
} ffile_t;

typedef struct {
    const char *sig;         /* 4 bytes at selector 0; NULL -> device absent */
    uint32_t    features;
    uint32_t    claim_count; /* directory count field (0 -> the real count)  */
    int         serve;       /* entries actually served (-1 -> all of them)  */
    ffile_t     files[FF_MAX];
} fdev_t;

static fdev_t   g_dev;
static uint8_t  g_dir[4 + FF_MAX * 64];
static uint32_t g_dirlen;
static uint16_t g_sel_of[FF_MAX];
static int      g_nfiles;
static uint16_t g_cur;
static uint32_t g_off;

static int flen(const ffile_t *f) {
    if (f->len >= 0) return f->len;
    return f->data ? (int)strlen(f->data) : 0;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void fake_select(void *ctx, uint16_t key) {
    (void)ctx;
    g_cur = key;
    g_off = 0;          /* selecting rewinds, exactly as the hardware does */
}

static uint8_t fake_read8(void *ctx) {
    (void)ctx;
    uint32_t o = g_off++;

    /* No device: every port read floats high. */
    if (!g_dev.sig) return 0xFF;

    if (g_cur == FWCFG_SEL_SIGNATURE)
        return o < 4 ? (uint8_t)g_dev.sig[o] : 0;
    if (g_cur == FWCFG_SEL_ID)
        return o < 4 ? (uint8_t)(g_dev.features >> (8 * o)) : 0;   /* LE */
    if (g_cur == FWCFG_SEL_FILE_DIR)
        return o < g_dirlen ? g_dir[o] : 0;   /* past the end: zeros */

    for (int i = 0; i < g_nfiles; i++) {
        if (g_sel_of[i] != g_cur) continue;
        int n = flen(&g_dev.files[i]);
        return (int)o < n ? (uint8_t)g_dev.files[i].data[o] : 0;
    }
    return 0xFF;        /* nothing selected there */
}

/* Assemble the directory in wire format and hand the device to the driver. */
static void install(const fdev_t *dev) {
    g_dev = *dev;

    g_nfiles = 0;
    while (g_nfiles < FF_MAX && g_dev.files[g_nfiles].name) g_nfiles++;

    int serve = g_dev.serve < 0 ? g_nfiles : g_dev.serve;
    if (serve > g_nfiles) serve = g_nfiles;

    uint32_t claim = g_dev.claim_count ? g_dev.claim_count : (uint32_t)g_nfiles;

    memset(g_dir, 0, sizeof g_dir);
    put_be32(g_dir, claim);
    g_dirlen = 4;

    for (int i = 0; i < g_nfiles; i++) {
        const ffile_t *f = &g_dev.files[i];
        g_sel_of[i] = f->zero_select ? 0 : (uint16_t)(0x20 + i);
        if (i >= serve) continue;                 /* truncated directory */

        uint8_t *e = g_dir + g_dirlen;
        put_be32(e, f->claim ? f->claim : (uint32_t)flen(f));
        put_be16(e + 4, g_sel_of[i]);
        e[6] = e[7] = 0;                          /* reserved */
        if (f->unterminated) {
            memset(e + 8, 'A', 56);               /* no NUL anywhere in it */
        } else {
            size_t n = strlen(f->name);
            if (n > 55) n = 55;
            memcpy(e + 8, f->name, n);
        }
        g_dirlen += 64;
    }

    static const fwcfg_backend_t be = { fake_select, fake_read8, (void *)0 };
    fwcfg_set_backend(&be);
    g_cur = 0;
    g_off = 0;
}

/* The common case: a QEMU that offers one blob under opt/fableos/apikey. */
static void install_key(const char *raw) {
    fdev_t d = { "QEMU", 0x3, 0, -1, {
        { "etc/e820",           "\x01\x02\x03\x04", 4, 0, 0, 0 },
        { FWCFG_APIKEY_FILE,    raw,               -1, 0, 0, 0 },
        { "opt/fableos/unused",  "later",           -1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&d);
}

/* ====================================================================== */
/* 1. detecting the interface                                             */
/* ====================================================================== */

static void test_absent_device(void) {
    /* No backend installed at all: the host default returns 0xFF for every
     * read, which is what a machine with no fw_cfg does. */
    fwcfg_set_backend(NULL);

    CHECK_EQ(fwcfg_present(), 0);
    CHECK_EQ((int)fwcfg_features(), 0);
    CHECK_EQ(fwcfg_file_count(), 0);

    char buf[16];
    uint32_t n = 123;
    CHECK_EQ(fwcfg_read_file("opt/fableos/apikey", buf, sizeof buf, &n), FWCFG_ENODEV);
    CHECK_EQ((int)n, 0);

    CHECK_EQ(fwcfg_apikey_load(), FWCFG_ENODEV);
    CHECK_STR(fwcfg_apikey(), "");
    CHECK_EQ(fwcfg_apikey_len(), 0);
}

static void test_signature_mismatch(void) {
    /* Something answers on 0x510/0x511, but it is not fw_cfg. Believing it
     * would mean reading a "directory" out of an unrelated device. */
    fdev_t d = { "BOCH", 0x3, 0, -1, {
        { FWCFG_APIKEY_FILE, "notkey-nope", -1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&d);

    CHECK_EQ(fwcfg_present(), 0);
    CHECK_EQ((int)fwcfg_features(), 0);
    CHECK_EQ(fwcfg_file_count(), 0);
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_ENODEV);
    CHECK_STR(fwcfg_apikey(), "");

    /* A short signature is a mismatch too, not a partial match. */
    fdev_t s = { "QEM\0", 0, 0, -1, { { 0, 0, 0, 0, 0, 0 } } };
    install(&s);
    CHECK_EQ(fwcfg_present(), 0);
}

static void test_present(void) {
    install_key("notkey-fake");

    CHECK_EQ(fwcfg_present(), 1);
    CHECK_EQ((int)fwcfg_features(), 3);
    CHECK_EQ(fwcfg_file_count(), 3);

    /* Probing is cached, and repeat calls must not re-read the stream in a way
     * that changes the answer. */
    CHECK_EQ(fwcfg_present(), 1);
    CHECK_EQ(fwcfg_file_count(), 3);
}

static void test_backend_swap_forgets_verdict(void) {
    install_key("notkey-fake");
    CHECK_EQ(fwcfg_present(), 1);

    /* Swapping in a device with no signature must not inherit "present". */
    fwcfg_set_backend(NULL);
    CHECK_EQ(fwcfg_present(), 0);
}

/* ====================================================================== */
/* 2. reading a named blob                                                */
/* ====================================================================== */

static void test_read_found(void) {
    install_key("notkey-fake");

    char buf[32];
    uint32_t n = 0;
    memset(buf, '?', sizeof buf);
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n), FWCFG_OK);
    CHECK_EQ((int)n, 11);
    CHECK_EQ(memcmp(buf, "notkey-fake", 11), 0);

    /* An entry that is not the first one, to prove the walk advances. */
    memset(buf, '?', sizeof buf);
    CHECK_EQ(fwcfg_read_file("opt/fableos/unused", buf, sizeof buf, &n), FWCFG_OK);
    CHECK_EQ((int)n, 5);
    CHECK_EQ(memcmp(buf, "later", 5), 0);

    /* And the first one, after that, to prove selecting rewinds. */
    memset(buf, '?', sizeof buf);
    CHECK_EQ(fwcfg_read_file("etc/e820", buf, sizeof buf, &n), FWCFG_OK);
    CHECK_EQ((int)n, 4);
    CHECK_EQ(memcmp(buf, "\x01\x02\x03\x04", 4), 0);

    /* out_len is optional. */
    CHECK_EQ(fwcfg_read_file("etc/e820", buf, sizeof buf, NULL), FWCFG_OK);
}

static void test_read_exact_fit(void) {
    install_key("0123456789");

    char buf[10];
    uint32_t n = 0;
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n), FWCFG_OK);
    CHECK_EQ((int)n, 10);
    CHECK_EQ(memcmp(buf, "0123456789", 10), 0);
}

static void test_read_missing(void) {
    install_key("notkey-fake");

    char buf[32];
    uint32_t n = 7;
    CHECK_EQ(fwcfg_read_file("opt/fableos/nothing", buf, sizeof buf, &n),
             FWCFG_ENOENT);
    CHECK_EQ((int)n, 0);

    /* A prefix of a real name is not that name. */
    CHECK_EQ(fwcfg_read_file("opt/fableos/apike", buf, sizeof buf, &n),
             FWCFG_ENOENT);
    /* Nor is a name that extends one. */
    CHECK_EQ(fwcfg_read_file("opt/fableos/apikey2", buf, sizeof buf, &n),
             FWCFG_ENOENT);
}

static void test_read_empty(void) {
    install_key("");

    char buf[16];
    uint32_t n = 5;
    memset(buf, '?', sizeof buf);
    /* An empty blob is a real answer at this layer: zero bytes, not an error.
     * It is the KEY layer that has an opinion about it (see test_key_empty). */
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n), FWCFG_OK);
    CHECK_EQ((int)n, 0);
    CHECK_EQ(buf[0], '?');       /* nothing written */
}

static void test_read_oversized(void) {
    install_key("notkey-much-too-long-for-the-buffer");

    char buf[8];
    uint32_t n = 0;
    memset(buf, '?', sizeof buf);
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n), FWCFG_E2BIG);
    /* The size the host claimed, so a caller can say how much was needed... */
    CHECK_EQ((int)n, 35);
    /* ...and NOT one byte copied. Refusing after starting is a buffer overrun. */
    for (size_t i = 0; i < sizeof buf; i++) CHECK_EQ(buf[i], '?');
}

static void test_read_bad_args(void) {
    install_key("notkey-fake");

    char buf[16];
    uint32_t n = 9;
    CHECK_EQ(fwcfg_read_file(NULL, buf, sizeof buf, &n), FWCFG_EINVAL);
    CHECK_EQ((int)n, 0);
    CHECK_EQ(fwcfg_read_file("", buf, sizeof buf, &n), FWCFG_EINVAL);
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, NULL, 16, &n), FWCFG_EINVAL);
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, 0, &n), FWCFG_EINVAL);

    /* A name that cannot fit in a 56-byte field can never match one. */
    char huge[200];
    memset(huge, 'x', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';
    CHECK_EQ(fwcfg_read_file(huge, buf, sizeof buf, &n), FWCFG_ENOENT);
}

/* ====================================================================== */
/* 3. a directory that lies                                               */
/* ====================================================================== */

static void test_dir_truncated(void) {
    /* The count says 6; only two entries are actually on the wire and the rest
     * of the stream is zeros — which is what fw_cfg gives past the end of a
     * blob. The walk must terminate, must find what is really there, and must
     * report the rest as absent rather than as garbage. */
    fdev_t d = { "QEMU", 0x1, 6, 2, {
        { "etc/e820",        "\x01",     1, 0, 0, 0 },
        { FWCFG_APIKEY_FILE, "notkey-t", -1, 0, 0, 0 },
        { "opt/fableos/gone", "nope",     -1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&d);

    CHECK_EQ(fwcfg_present(), 1);
    CHECK_EQ(fwcfg_file_count(), 6);      /* what it claims, honestly reported */

    char buf[32];
    uint32_t n = 0;
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n), FWCFG_OK);
    CHECK_EQ((int)n, 8);
    CHECK_EQ(fwcfg_read_file("opt/fableos/gone", buf, sizeof buf, &n),
             FWCFG_ENOENT);
}

static void test_dir_absurd_count(void) {
    /* Four billion entries at 64 bytes each, read a byte at a time, is a boot
     * that never finishes. Refuse the directory instead of walking it. */
    fdev_t d = { "QEMU", 0x1, 0xFFFFFFFFu, -1, {
        { FWCFG_APIKEY_FILE, "notkey-t", -1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&d);

    CHECK_EQ(fwcfg_present(), 1);
    CHECK_EQ(fwcfg_file_count(), 0);

    char buf[32];
    uint32_t n = 1;
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n),
             FWCFG_EPROTO);
    CHECK_EQ((int)n, 0);
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EPROTO);
    CHECK_STR(fwcfg_apikey(), "");

    /* One past the cap is still refused; the cap itself is not. */
    fdev_t over = { "QEMU", 0x1, FWCFG_MAX_ENTRIES + 1, 1, {
        { FWCFG_APIKEY_FILE, "notkey-t", -1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&over);
    CHECK_EQ(fwcfg_file_count(), 0);

    fdev_t at = { "QEMU", 0x1, FWCFG_MAX_ENTRIES, 1, {
        { FWCFG_APIKEY_FILE, "notkey-t", -1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&at);
    CHECK_EQ(fwcfg_file_count(), FWCFG_MAX_ENTRIES);
    CHECK_EQ(fwcfg_apikey_load(), 8);
}

static void test_dir_absurd_entry_size(void) {
    /* The entry claims 4 GiB. It must be rejected as an entry, not attempted
     * as a read — and the good entry after it must still be found, which only
     * happens if the bad row was consumed rather than bailed out of. */
    fdev_t d = { "QEMU", 0x1, 0, -1, {
        { "etc/huge",        "x", 1, 0xFFFFFFFFu, 0, 0 },
        { FWCFG_APIKEY_FILE, "notkey-ok", -1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&d);

    char buf[32];
    uint32_t n = 0;
    CHECK_EQ(fwcfg_read_file("etc/huge", buf, sizeof buf, &n), FWCFG_ENOENT);
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n), FWCFG_OK);
    CHECK_EQ((int)n, 9);
    CHECK_EQ(fwcfg_apikey_load(), 9);

    /* Exactly FWCFG_FILE_MAX is legal; one more is not. The blob itself is
     * short here — what is under test is which claims survive the filter. */
    fdev_t at = { "QEMU", 0x1, 0, -1, {
        { "etc/big", "x", 1, FWCFG_FILE_MAX, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&at);
    /* Accepted as an entry, then refused for the buffer — E2BIG, not ENOENT. */
    CHECK_EQ(fwcfg_read_file("etc/big", buf, sizeof buf, &n), FWCFG_E2BIG);
    CHECK_EQ((int)n, FWCFG_FILE_MAX);

    fdev_t over = { "QEMU", 0x1, 0, -1, {
        { "etc/big", "x", 1, FWCFG_FILE_MAX + 1, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&over);
    CHECK_EQ(fwcfg_read_file("etc/big", buf, sizeof buf, &n), FWCFG_ENOENT);
}

static void test_dir_unterminated_name(void) {
    /* 56 bytes of name and no NUL. Treating that as a C string reads off the
     * end of the entry; the row is dropped and the next one still works. */
    fdev_t d = { "QEMU", 0x1, 0, -1, {
        { "etc/bad",         "x",         1, 0, 0, 1 },
        { FWCFG_APIKEY_FILE, "notkey-ok", -1, 0, 0, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&d);

    char buf[64];
    uint32_t n = 0;
    /* Neither the intended name nor the 56 A's it actually carries can match. */
    CHECK_EQ(fwcfg_read_file("etc/bad", buf, sizeof buf, &n), FWCFG_ENOENT);
    CHECK_EQ(fwcfg_read_file("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                             buf, sizeof buf, &n), FWCFG_ENOENT);
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n), FWCFG_OK);
    CHECK_EQ((int)n, 9);
}

static void test_dir_zero_selector(void) {
    /* Selector 0 is the signature. An entry naming it is either corruption or
     * an attempt to make a read of "the key" return "QEMU". */
    fdev_t d = { "QEMU", 0x1, 0, -1, {
        { FWCFG_APIKEY_FILE, "notkey-t", -1, 0, 1, 0 },
        { 0, 0, 0, 0, 0, 0 },
    } };
    install(&d);

    char buf[32];
    uint32_t n = 0;
    CHECK_EQ(fwcfg_read_file(FWCFG_APIKEY_FILE, buf, sizeof buf, &n),
             FWCFG_ENOENT);
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_ENOENT);
    CHECK_STR(fwcfg_apikey(), "");
}

/* ====================================================================== */
/* 4. the key: cleaning, refusing, and never leaking                      */
/* ====================================================================== */

static void test_key_plain(void) {
    install_key("notkey-api03-plain");
    CHECK_EQ(fwcfg_apikey_load(), 18);
    CHECK_STR(fwcfg_apikey(), "notkey-api03-plain");
    CHECK_EQ(fwcfg_apikey_len(), 18);
}

static void test_key_trailing_whitespace(void) {
    /* THE CASE THAT MATTERS. `printf '%s' "$key" > file` is careful; every
     * other way of producing that file — a here-doc, `echo`, copying a line out
     * of .env — ends in a newline. Unstripped, that newline terminates the
     * x-api-key header early and the rest of the key becomes the start of an
     * attacker-chosen header. */
    install_key("notkey-api03-nl\n");
    CHECK_EQ(fwcfg_apikey_load(), 15);
    CHECK_STR(fwcfg_apikey(), "notkey-api03-nl");

    install_key("notkey-api03-crlf\r\n");
    CHECK_EQ(fwcfg_apikey_load(), 17);
    CHECK_STR(fwcfg_apikey(), "notkey-api03-crlf");

    /* Leading whitespace too, and several kinds of it, and a NUL-padded blob. */
    install_key(" \t\r\n notkey-api03-pad \t\n\n");
    CHECK_EQ(fwcfg_apikey_load(), 16);
    CHECK_STR(fwcfg_apikey(), "notkey-api03-pad");

    ffile_t padded = { FWCFG_APIKEY_FILE, "notkey-api03-z\0\0\0", 17, 0, 0, 0 };
    fdev_t d = { "QEMU", 0x1, 0, -1, { { 0 } } };
    d.files[0] = padded;
    install(&d);
    CHECK_EQ(fwcfg_apikey_load(), 14);
    CHECK_STR(fwcfg_apikey(), "notkey-api03-z");
}

static void test_key_realistic_length(void) {
    /* The shape of the real thing: 108 characters, which is the number the boot
     * log reports and the only number it is allowed to report. */
    char key[109];
    memcpy(key, "notkey-api03-", 13);
    for (int i = 13; i < 108; i++) key[i] = (char)('A' + (i % 26));
    key[108] = '\0';

    install_key(key);
    CHECK_EQ(fwcfg_apikey_load(), 108);
    CHECK_EQ(fwcfg_apikey_len(), 108);
    CHECK_STR(fwcfg_apikey(), key);
}

static void test_key_empty(void) {
    install_key("");
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EEMPTY);
    CHECK_STR(fwcfg_apikey(), "");
    CHECK_EQ(fwcfg_apikey_len(), 0);

    /* Nothing but whitespace is the same thing: an empty .env value. */
    install_key("   \r\n\t  \n");
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EEMPTY);
    CHECK_STR(fwcfg_apikey(), "");
}

static void test_key_oversized(void) {
    static char big[FWCFG_KEY_MAX + 64];
    memset(big, 'k', sizeof big - 1);
    big[sizeof big - 1] = '\0';

    install_key(big);
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_E2BIG);
    CHECK_STR(fwcfg_apikey(), "");
    CHECK_EQ(fwcfg_apikey_len(), 0);

    /* The largest key that still fits, to pin the boundary from the other side. */
    static char fits[FWCFG_KEY_MAX];
    memset(fits, 'k', sizeof fits - 1);
    fits[sizeof fits - 1] = '\0';
    install_key(fits);
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_KEY_MAX - 1);
    CHECK_EQ(fwcfg_apikey_len(), FWCFG_KEY_MAX - 1);
}

static void test_key_header_injection_refused(void) {
    /* An embedded CRLF is request splitting: the key would end the x-api-key
     * header and everything after it would be headers of the attacker's
     * choosing. Trimming the ends is not enough — this one is REFUSED. */
    install_key("notkey\r\nX-Evil: 1");
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EINVAL);
    CHECK_STR(fwcfg_apikey(), "");

    install_key("notkey\nX-Evil: 1");
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EINVAL);
    CHECK_STR(fwcfg_apikey(), "");

    /* An embedded NUL truncates the header at the C level instead. */
    ffile_t nul = { FWCFG_APIKEY_FILE, "notkey\0X-Evil: 1", 16, 0, 0, 0 };
    fdev_t d = { "QEMU", 0x1, 0, -1, { { 0 } } };
    d.files[0] = nul;
    install(&d);
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EINVAL);
    CHECK_STR(fwcfg_apikey(), "");

    /* A space in the middle is a .env quoting mistake, not a credential. */
    install_key("notkey then some prose");
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EINVAL);
    CHECK_STR(fwcfg_apikey(), "");

    /* So is a byte outside ASCII (a smart-quote pasted from a web page). */
    install_key("notkey\xe2\x80\x9d");
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EINVAL);
    CHECK_STR(fwcfg_apikey(), "");
}

static void test_key_reload_leaves_nothing_stale(void) {
    /* A good key, then a machine with none. The accessor must not still be
     * handing out the old one — that is how a "no key, expect 401" test run
     * silently authenticates with the previous run's credential. */
    install_key("notkey-api03-first");
    CHECK_EQ(fwcfg_apikey_load(), 18);
    CHECK_STR(fwcfg_apikey(), "notkey-api03-first");

    install_key("");
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EEMPTY);
    CHECK_STR(fwcfg_apikey(), "");

    install_key("notkey-api03-first");
    CHECK_EQ(fwcfg_apikey_load(), 18);
    install_key("bad\nkey");
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_EINVAL);
    CHECK_STR(fwcfg_apikey(), "");

    install_key("notkey-api03-first");
    CHECK_EQ(fwcfg_apikey_load(), 18);
    fwcfg_set_backend(NULL);
    CHECK_EQ(fwcfg_apikey_load(), FWCFG_ENODEV);
    CHECK_STR(fwcfg_apikey(), "");
}

/* ====================================================================== */
/* 5. nothing prints the key                                              */
/* ====================================================================== */

/* Every message this module can emit, checked against the key material that
 * produced it. A prefix is included because a "safe" partial disclosure of an
 * API key is the part that identifies the account. */
static void report_and_check(const char *raw, const char *secret,
                             const char *must_contain) {
    install_key(raw);
    kcap_reset();
    int rc = fwcfg_apikey_load();
    fwcfg_apikey_report(rc);

    const char *log = kcap_text();
    CHECK_CONTAINS(log, must_contain);
    CHECK(strstr(log, secret) == NULL);

    /* ...and no recognisable fragment of it either. */
    char frag[8];
    size_t n = strlen(secret);
    if (n > 7) n = 7;
    memcpy(frag, secret, n);
    frag[n] = '\0';
    if (n >= 3) CHECK(strstr(log, frag) == NULL);

    /* A kernel trace line is a '[' in column zero (include/trace.h), and only
     * lib/trace.c may put one there. These lines are driver prose, so none of
     * them may start a line with '['. */
    CHECK(log[0] != '[');
    for (const char *p = log; p[0]; p++)
        if (p[0] == '\n') CHECK(p[1] != '[');
}

static void test_report_lines(void) {
    /* success: a length, and nothing else */
    install_key("notkey-api03-secretsecret");
    kcap_reset();
    int rc = fwcfg_apikey_load();
    fwcfg_apikey_report(rc);
    CHECK_CONTAINS(kcap_text(), "api key: 25 bytes loaded from fw_cfg");
    CHECK_CONTAINS(kcap_text(), FWCFG_APIKEY_FILE);
    CHECK(strstr(kcap_text(), "notkey") == NULL);

    /* absent: honest, and points at 401 so the operator is not surprised */
    fwcfg_set_backend(NULL);
    kcap_reset();
    fwcfg_apikey_report(fwcfg_apikey_load());
    CHECK_CONTAINS(kcap_text(), "no api key");
    CHECK_CONTAINS(kcap_text(), "401");

    /* every rejection path, key material never echoed */
    report_and_check("", "notkey", "empty");
    report_and_check("   \n", "notkey", "empty");
    report_and_check("notkey-api03-inject\nX-Evil: 1", "notkey-api03-inject",
                     "no api key");
    report_and_check("notkey-api03-with space", "notkey-api03-with", "no api key");

    static char big[FWCFG_KEY_MAX + 64];
    memset(big, 'z', sizeof big - 1);
    memcpy(big, "notkey-api03-toobig", 19);
    big[sizeof big - 1] = '\0';
    report_and_check(big, "notkey-api03-toobig", "no api key");
}

static void test_status_strings(void) {
    /* A log line that says "(null)" is a log line nobody trusts. */
    for (int rc = -8; rc <= 1; rc++) {
        const char *s = fwcfg_status_str(rc);
        CHECK(s != NULL);
        CHECK(s[0] != '\0');
    }
    CHECK_CONTAINS(fwcfg_status_str(FWCFG_ENODEV), "fw_cfg");
    CHECK_CONTAINS(fwcfg_status_str(FWCFG_EEMPTY), "empty");
}

int main(void) {
    console_init();

    RUN(test_absent_device);
    RUN(test_signature_mismatch);
    RUN(test_present);
    RUN(test_backend_swap_forgets_verdict);

    RUN(test_read_found);
    RUN(test_read_exact_fit);
    RUN(test_read_missing);
    RUN(test_read_empty);
    RUN(test_read_oversized);
    RUN(test_read_bad_args);

    RUN(test_dir_truncated);
    RUN(test_dir_absurd_count);
    RUN(test_dir_absurd_entry_size);
    RUN(test_dir_unterminated_name);
    RUN(test_dir_zero_selector);

    RUN(test_key_plain);
    RUN(test_key_trailing_whitespace);
    RUN(test_key_realistic_length);
    RUN(test_key_empty);
    RUN(test_key_oversized);
    RUN(test_key_header_injection_refused);
    RUN(test_key_reload_leaves_nothing_stale);

    RUN(test_report_lines);
    RUN(test_status_strings);

    return th_report("fwcfg");
}
