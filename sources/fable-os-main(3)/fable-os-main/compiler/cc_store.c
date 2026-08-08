/* cc_store.c — a compiled program keeps its name, its documentation and its source.
 *
 * WHY THE SOURCE AND NOT THE IMAGE. A saved program is C text, not machine code.
 * Three reasons, in order of weight: a model can read, diff and repair source and
 * cannot do any of those things to 4 KiB of x86; a change to cc_x64.c then
 * improves every saved program at its next call instead of leaving a museum of
 * images built by an older code generator; and the source is a fifth of the size.
 * The cost is a recompile per call, which is milliseconds — cc_call() caches the
 * last image so a loop of calls to one program compiles once.
 *
 * DELIBERATELY THE SAME SHAPE AS core/capability.c's STORE, because a model that
 * has learned one should not have to learn the other: one file per program under
 * a store root, 16 lowercase hex digits of FNV-1a-64, a newline, then a JSON
 * record that net/json.c parses. No new parser exists here — the 17-byte prefix
 * is a fixed layout, not a grammar. A record whose hash does not match, whose
 * JSON is malformed, whose name disagrees with its filename, or whose source no
 * longer compiles is REJECTED, loudly, and never enters the registry.
 *
 * SAVE ORDER IS THE CONSISTENCY GUARANTEE, and it is the same one, for the same
 * reason: the previous version is copied to <name>.bak and flushed FIRST, then
 * <name>.cc is overwritten. An interrupted save leaves either the old version
 * intact or a corrupt current file beside a good backup, and the next cc_init()
 * detects that and falls back.
 *
 * WHAT IS NOT HERE, said plainly: this is NOT core/capability.c's registry. A
 * compiled program is not a cap_t and cannot be a step in a composition or be
 * reached by apps/cap.c, because cap_kind_t has two values and adding a third
 * means editing a file this module does not own. The shapes match so that the
 * merge is small; the merge is reported, not written.
 *
 * ALSO NOT HERE: versions beyond one level of undo, and any rollback verb. There
 * is a backup, cc_init() will fall back to it, and that is all — cc_save() says
 * out loud in its result that the previous version is the one being spent.
 */

#include "cc_int.h"

/* kernel.h before <string.h>; see the note in cc_sym.c. */
#include "kernel.h"
#include "vfs.h"
#include "json.h"
#include "trace.h"
#include "fs.h"          /* FS_DISK_MOUNT only; no fs_* function is called */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The store, and the data root a compiled program's file_* calls land in.
 * Neither name is a prefix of the other, which is asserted at init: "/cc" and
 * "/ccdata" would satisfy "different directory" but not "obviously different
 * directory", and this is a boundary that has to read as one. */
#define CC_STORE_DISK  FS_DISK_MOUNT "/cc"
#define CC_STORE_RAM   "/cc"
#define CC_DATA_DISK   FS_DISK_MOUNT "/work"
#define CC_DATA_RAM    "/work"

static cc_prog_t g_prog[CC_MAX_PROGRAMS];
static int       g_nprog;
static char      g_store[VFS_PATH_MAX + 1];
static char      g_data[VFS_PATH_MAX + 1];
static int       g_persists;
static int       g_rejected;

/* One shared record buffer and one shared source buffer: exactly one program is
 * being saved or loaded at any moment, because this machine is single-threaded
 * and a tool call runs to completion. */
static char g_record[CC_RECORD_MAX + 1];
static char g_src[CC_SRC_MAX + 1];

/* The cached image: which program cc.c's one code buffer currently holds. */
static char     g_img_name[CC_NAME_MAX];
static uint32_t g_img_version;
static uint64_t g_img_serial;

/* ===================================================================== */
/* small helpers                                                         */
/* ===================================================================== */

static void copy_z(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint64_t fnv1a(const char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void hex16(uint64_t v, char *dst) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 16; i++) dst[i] = d[(v >> (60 - 4 * i)) & 0xf];
    dst[16] = '\0';
}

static int unhex16(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int c = (unsigned char)s[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return 0;
        v = v * 16 + (uint64_t)d;
    }
    *out = v;
    return 1;
}

static void path_of(char *dst, size_t cap, const char *name, const char *ext) {
    snprintf(dst, cap, "%s/%s%s", g_store[0] ? g_store : CC_STORE_RAM, name, ext);
}

static int file_exists(const char *path) {
    vfs_stat_t st;
    return vfs_stat(path, &st) == VFS_OK;
}

static int read_whole(const char *path, char *dst, size_t cap, size_t *out_len) {
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) return 0;
    int64_t n = vfs_read(f, dst, (uint64_t)(cap - 1));
    vfs_close(f);
    if (n < 0) return 0;
    dst[n] = '\0';
    *out_len = (size_t)n;
    return 1;
}

static int write_whole(const char *path, const char *src, size_t len) {
    file_t *f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return 0;
    int64_t w = vfs_write(f, src, (uint64_t)len);
    vfs_close(f);
    return w == (int64_t)len;
}

/* The VFS exposes unlink only through vnode_ops_t (there is no vfs_unlink), so
 * reach it the sanctioned way, exactly as core/capability.c and tools/vfs_tools.c
 * do: open the parent directory for its vnode and call the filesystem's own
 * unlink through the public ops table. Truncating the file to nothing instead
 * would leave a zero-length record that the NEXT boot reports as corrupt, for
 * ever — a deletion that prints an error is worse than no deletion. */
static int remove_at(const char *dir, const char *leaf) {
    file_t *d = vfs_open(dir, O_RDONLY);
    if (!d) return 0;
    int rc = 0;
    if (d->node && d->node->type == VNODE_DIR && d->node->ops &&
        d->node->ops->unlink)
        rc = (d->node->ops->unlink(d->node, leaf) == VFS_OK);
    vfs_close(d);
    return rc;
}

/* ===================================================================== */
/* names, docs, parameters                                               */
/* ===================================================================== */

int cc_name_ok(const char *name, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    if (!name || !name[0]) {
        if (why) snprintf(why, cap, "a name is required");
        return 0;
    }
    size_t n = strlen(name);
    if (n >= CC_NAME_MAX) {
        if (why) snprintf(why, cap, "'%c...' is %d characters; the limit is %d",
                          name[0], (int)n, CC_NAME_MAX - 1);
        return 0;
    }
    if (!(name[0] >= 'a' && name[0] <= 'z')) {
        if (why) snprintf(why, cap, "must start with a lowercase letter, not '%c'",
                          name[0]);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') continue;
        if (why) snprintf(why, cap,
                          "'%c' is not allowed in a name (lowercase letters, "
                          "digits and _ only)", c);
        return 0;
    }
    return 1;
}

static int doc_ok(const char *doc, char *why, size_t cap) {
    if (!doc || !doc[0]) {
        snprintf(why, cap, "\"doc\" is required: one line saying what this program "
                           "does and what its parameters mean. It is what a later "
                           "turn reads to decide whether to call it");
        return 0;
    }
    size_t n = strlen(doc);
    if (n >= CC_DOC_MAX) {
        snprintf(why, cap, "\"doc\" is %d characters; the limit is %d",
                 (int)n, CC_DOC_MAX - 1);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)doc[i];
        if (c < 0x20 || c > 0x7e) {
            snprintf(why, cap, "\"doc\" must be one line of printable text; byte "
                               "%d is 0x%02x", (int)i, c);
            return 0;
        }
    }
    return 1;
}

/* ===================================================================== */
/* the panel                                                            */
/* ===================================================================== */

static char *g_panel_dst;
static size_t g_panel_cap;
static char  g_panel[CC_PANEL_CHARS + 1];

/* Append, never overflowing, and never emitting a byte whose JSON escape is
 * longer than itself — that is what keeps the assembled schema a constant size. */
static void pan(char *buf, size_t *len, const char *s) {
    while (*s && *len < CC_PANEL_CHARS) {
        unsigned char c = (unsigned char)*s++;
        if (c == '"' || c == '\\' || c < 0x20 || c > 0x7e) c = ' ';
        buf[(*len)++] = (char)c;
    }
}

static void panel_render(void) {
    size_t len = 0;
    char line[256];

    if (g_nprog == 0) {
        snprintf(line, sizeof line,
                 "nothing is saved yet. store %s (%s). Use cc_save to keep a "
                 "program here.",
                 g_store[0] ? g_store : CC_STORE_RAM,
                 g_persists ? "survives a reboot" : "RAM ONLY, lost at reboot");
        pan(g_panel, &len, line);
    } else {
        snprintf(line, sizeof line, "store %s (%s). ",
                 g_store[0] ? g_store : CC_STORE_RAM,
                 g_persists ? "survives a reboot" : "RAM ONLY, lost at reboot");
        pan(g_panel, &len, line);
        int shown = 0;
        for (int i = 0; i < g_nprog; i++) {
            const cc_prog_t *p = &g_prog[i];
            char params[CC_MAX_PARAMS * (CC_PARAM_NAME_MAX + 2) + 2];
            size_t o = 0;
            params[0] = '\0';
            for (int k = 0; k < p->nparam; k++) {
                int w = snprintf(params + o, sizeof params - o, "%s%s",
                                 k ? "," : "", p->param[k]);
                if (w < 0 || (size_t)w >= sizeof params - o) break;
                o += (size_t)w;
            }
            snprintf(line, sizeof line, "%s%s(%s) v%u: %s",
                     shown ? " | " : "", p->name, params,
                     (unsigned)p->version, p->doc);
            /* Stop on a WHOLE entry rather than half a name: a truncated name is
             * a name the model will call and this machine will refuse.
             *
             * The reserve must be honoured on BOTH sides of this test, which is
             * the bug the first version had: it checked whether the NEXT entry
             * would cross the reserve but never guaranteed `len` was already
             * inside it, so the "+N more" tail was itself clipped and the panel
             * ended "more (cc_sou". Adding an entry only when it leaves the
             * reserve intact makes the invariant inductive. */
            const size_t MORE = 32;      /* " | +16 more (cc_source)" is 23 */
            size_t need = strlen(line);
            if (len + need > CC_PANEL_CHARS - MORE && shown) {
                snprintf(line, sizeof line, " | +%d more (cc_source)",
                         g_nprog - shown);
                pan(g_panel, &len, line);
                break;
            }
            pan(g_panel, &len, line);
            shown++;
        }
    }
    while (len < CC_PANEL_CHARS) g_panel[len++] = ' ';
    g_panel[CC_PANEL_CHARS] = '\0';

    if (g_panel_dst && g_panel_cap >= CC_PANEL_CHARS)
        memcpy(g_panel_dst, g_panel, CC_PANEL_CHARS);
}

const char *cc_panel(void) { return g_panel; }

void cc_panel_bind(char *dst, size_t cap) {
    g_panel_dst = dst;
    g_panel_cap = cap;
    panel_render();
}

int cc_panel_check(char *msg, size_t cap) {
    size_t n = strlen(g_panel);
    if (n != CC_PANEL_CHARS) {
        if (msg) snprintf(msg, cap, "the panel rendered %d characters, but %d are "
                                    "reserved in the tool schema",
                          (int)n, CC_PANEL_CHARS);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)g_panel[i];
        if (c == '"' || c == '\\' || c < 0x20 || c > 0x7e) {
            if (msg) snprintf(msg, cap, "byte %d of the panel is 0x%02x, which "
                                        "changes its escaped length", (int)i, c);
            return 0;
        }
    }
    if (msg && cap) msg[0] = '\0';
    return 1;
}

/* ===================================================================== */
/* the registry                                                          */
/* ===================================================================== */

int cc_count(void) { return g_nprog; }

const cc_prog_t *cc_at(int i) {
    if (i < 0 || i >= g_nprog) return NULL;
    return &g_prog[i];
}

const cc_prog_t *cc_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_nprog; i++)
        if (strcmp(g_prog[i].name, name) == 0) return &g_prog[i];
    return NULL;
}

const char *cc_store_root(void)  { return g_store[0] ? g_store : CC_STORE_RAM; }
const char *cc_data_root(void)   { return g_data[0]  ? g_data  : CC_DATA_RAM; }
int         cc_store_persists(void) { return g_persists; }
int         cc_rejected(void)       { return g_rejected; }

/* ===================================================================== */
/* the record: write and read                                            */
/* ===================================================================== */

/* Build the on-disk record for `p` with source `src` into g_record. Returns the
 * length, or 0 if it did not fit. */
static size_t record_build(const cc_prog_t *p, const char *src, size_t srclen) {
    json_writer_t w;
    /* The hash prefix is written last, over a placeholder, so the JSON is built
     * once. 17 = 16 hex digits and the newline. */
    json_writer_init(&w, g_record + 17, sizeof g_record - 18);
    json_put(&w, "{\"cc\":1,\"name\":");
    json_put_string(&w, p->name);
    json_put(&w, ",\"doc\":");
    json_put_string(&w, p->doc);
    json_put(&w, ",\"version\":");
    json_put_uint(&w, p->version);
    json_put(&w, ",\"params\":[");
    for (int i = 0; i < p->nparam; i++) {
        if (i) json_put(&w, ",");
        json_put_string(&w, p->param[i]);
    }
    json_put(&w, "],\"src\":");
    {
        /* json_put_string wants a NUL-terminated string; the source span is one
         * (g_src always is), but be explicit rather than lucky. */
        char *tmp = g_src;
        if (src != g_src) {
            if (srclen > CC_SRC_MAX) return 0;
            memcpy(g_src, src, srclen);
            g_src[srclen] = '\0';
        }
        tmp[srclen] = '\0';
        json_put_string(&w, tmp);
    }
    json_put(&w, "}");
    if (!json_writer_ok(&w)) return 0;

    size_t body = json_writer_len(&w);
    uint64_t h = fnv1a(g_record + 17, body);
    char hex[17];
    hex16(h, hex);
    memcpy(g_record, hex, 16);
    g_record[16] = '\n';
    g_record[17 + body] = '\0';
    return 17 + body;
}

/* Parse a record: verify the hash, then the JSON. `p` is filled; `src` (optional)
 * receives the source. Returns 0 with `why` set on any refusal. */
static int record_parse(const char *buf, size_t len, const char *expect_name,
                        cc_prog_t *p, char *src, size_t srccap, size_t *srclen,
                        char *why, size_t whycap) {
    if (len < 18 || buf[16] != '\n') {
        snprintf(why, whycap, "the record is shorter than a header or has no "
                              "newline after its checksum");
        return 0;
    }
    uint64_t want;
    if (!unhex16(buf, &want)) {
        snprintf(why, whycap, "the first 16 bytes are not a hexadecimal checksum");
        return 0;
    }
    uint64_t got = fnv1a(buf + 17, len - 17);
    if (got != want) {
        snprintf(why, whycap, "checksum mismatch: the file says %.16s and its "
                              "contents hash to a different value", buf);
        return 0;
    }
    json_value_t root;
    if (json_parse(buf + 17, len - 17, &root) != JSON_OK ||
        root.type != JSON_OBJECT) {
        snprintf(why, whycap, "the record is not a JSON object");
        return 0;
    }
    memset(p, 0, sizeof *p);

    if (json_get_str(&root, "name", p->name, sizeof p->name) != JSON_OK ||
        !cc_name_ok(p->name, NULL, 0)) {
        snprintf(why, whycap, "the record has no usable \"name\"");
        return 0;
    }
    if (expect_name && strcmp(expect_name, p->name) != 0) {
        snprintf(why, whycap, "the file is %s.cc but the record inside names "
                              "\"%s\"; the two disagree", expect_name, p->name);
        return 0;
    }
    if (json_get_str(&root, "doc", p->doc, sizeof p->doc) != JSON_OK) {
        snprintf(why, whycap, "the record has no \"doc\"");
        return 0;
    }
    {
        json_value_t v;
        int64_t iv = 0;
        if (json_get(&root, "version", &v) != JSON_OK || json_int(&v, &iv) != JSON_OK ||
            iv < 1 || iv > 1000000) {
            snprintf(why, whycap, "the record has no usable \"version\"");
            return 0;
        }
        p->version = (uint32_t)iv;
    }
    {
        json_value_t arr;
        if (json_get(&root, "params", &arr) == JSON_OK) {
            if (arr.type != JSON_ARRAY) {
                snprintf(why, whycap, "\"params\" is not an array");
                return 0;
            }
            size_t n = json_count(&arr);
            if (n > CC_MAX_PARAMS) {
                snprintf(why, whycap, "\"params\" has %d entries; the limit is %d",
                         (int)n, CC_MAX_PARAMS);
                return 0;
            }
            for (size_t i = 0; i < n; i++) {
                json_value_t e;
                if (json_at(&arr, i, &e) != JSON_OK ||
                    json_str(&e, p->param[i], CC_PARAM_NAME_MAX, NULL) != JSON_OK) {
                    snprintf(why, whycap, "parameter %d is not a short string",
                             (int)i + 1);
                    return 0;
                }
            }
            p->nparam = (uint8_t)n;
        }
    }
    {
        json_value_t v;
        size_t got_len = 0;
        if (json_get(&root, "src", &v) != JSON_OK || v.type != JSON_STRING) {
            snprintf(why, whycap, "the record has no \"src\"");
            return 0;
        }
        if (src) {
            if (json_str(&v, src, srccap, &got_len) != JSON_OK) {
                snprintf(why, whycap, "\"src\" is longer than %d bytes",
                         (int)srccap - 1);
                return 0;
            }
            if (srclen) *srclen = got_len;
            p->src_len = (uint32_t)got_len;
        } else {
            char probe[1];
            /* Length only: json_str reports ENOSPC but still tells us nothing
             * about the length, so read it into the shared source buffer. */
            (void)probe;
            if (json_str(&v, g_src, sizeof g_src, &got_len) != JSON_OK) {
                snprintf(why, whycap, "\"src\" is longer than %d bytes",
                         (int)sizeof g_src - 1);
                return 0;
            }
            p->src_len = (uint32_t)got_len;
        }
    }
    p->crc = want;
    why[0] = '\0';
    return 1;
}

/* ===================================================================== */
/* loading                                                               */
/* ===================================================================== */

static int load_one(const char *name) {
    char path[VFS_PATH_MAX + 1], why[CC_MSG_MAX];
    size_t len = 0;
    cc_prog_t p;

    for (int attempt = 0; attempt < 2; attempt++) {
        path_of(path, sizeof path, name, attempt ? ".bak" : ".cc");
        if (!file_exists(path)) continue;
        if (!read_whole(path, g_record, sizeof g_record, &len)) {
            kprintf("cc: REJECTED %s - it could not be read\n", path);
            g_rejected++;
            continue;
        }
        if (!record_parse(g_record, len, name, &p, NULL, 0, NULL, why, sizeof why)) {
            kprintf("cc: REJECTED %s - %s\n", path, why);
            g_rejected++;
            continue;
        }
        if (g_nprog >= CC_MAX_PROGRAMS) {
            kprintf("cc: REJECTED %s - the registry holds only %d programs\n",
                    path, CC_MAX_PROGRAMS);
            g_rejected++;
            return 0;
        }
        cc_prog_t *slot = &g_prog[g_nprog++];
        /* Zero first: g_nprog is rewound by cc_init() without clearing the array,
         * so a slot can still hold the previous occupant's call counters. */
        memset(slot, 0, sizeof *slot);
        *slot = p;
        slot->calls = slot->failures = 0;
        if (attempt == 1)
            kprintf("cc: %s recovered from its backup\n", name);
        return 1;
    }
    return 0;
}

void cc_init(void) {
    g_nprog = 0;
    g_rejected = 0;
    g_img_name[0] = '\0';
    g_img_version = 0;
    g_img_serial = 0;

    vfs_stat_t st;
    if (vfs_stat(FS_DISK_MOUNT, &st) == VFS_OK && st.type == VNODE_DIR) {
        copy_z(g_store, sizeof g_store, CC_STORE_DISK);
        copy_z(g_data,  sizeof g_data,  CC_DATA_DISK);
        g_persists = 1;
    } else {
        copy_z(g_store, sizeof g_store, CC_STORE_RAM);
        copy_z(g_data,  sizeof g_data,  CC_DATA_RAM);
        g_persists = 0;
    }
    /* The data root must not be inside the store, or a program could rewrite the
     * machine's programs through file_write. Checked rather than assumed. */
    size_t sl = strlen(g_store);
    if (strncmp(g_data, g_store, sl) == 0)
        kprintf("cc: REFUSING file access - the data root %s is inside the store "
                "%s\n", g_data, g_store);

    vfs_mkdir(g_store);
    vfs_mkdir(g_data);

    /* Enumerate the store. A ".cc" file is a program; a ".bak" is only consulted
     * when its ".cc" fails. */
    char name[VFS_NAME_MAX + 1];
    for (uint32_t i = 0; i < 512; i++) {
        int rc = vfs_readdir(g_store, i, name, sizeof name);
        if (rc != VFS_OK) break;
        size_t n = strlen(name);
        if (n < 4 || strcmp(name + n - 3, ".cc") != 0) continue;
        name[n - 3] = '\0';
        if (!cc_name_ok(name, NULL, 0)) {
            kprintf("cc: REJECTED %s.cc - that is not a usable program name\n", name);
            g_rejected++;
            continue;
        }
        load_one(name);
    }

    panel_render();

    kprintf("cc: %d compiled program(s) loaded from %s (%s)%s\n",
            g_nprog, cc_store_root(),
            g_persists ? "survives a reboot" : "RAM only",
            g_rejected ? ", some records were REJECTED" : "");
}

/* ===================================================================== */
/* source                                                                */
/* ===================================================================== */

/* Read `name`'s source into `dst`. Also used internally, in which case `dst` is
 * g_src. */
int cc_source(const char *name, char *dst, size_t cap) {
    if (!name || !dst || cap == 0) return CC_EINVAL;
    dst[0] = '\0';
    if (!cc_find(name)) return CC_ENOENT;

    char path[VFS_PATH_MAX + 1], why[CC_MSG_MAX];
    size_t len = 0;
    cc_prog_t p;
    size_t srclen = 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        path_of(path, sizeof path, name, attempt ? ".bak" : ".cc");
        if (!file_exists(path)) continue;
        if (!read_whole(path, g_record, sizeof g_record, &len)) continue;
        if (!record_parse(g_record, len, name, &p, dst, cap, &srclen,
                          why, sizeof why))
            continue;
        return (int)srclen;
    }
    return CC_ECORRUPT;
}

/* ===================================================================== */
/* save and delete                                                       */
/* ===================================================================== */

int cc_save(const char *name, const char *doc,
            const char *const *params, int nparam,
            const char *src, size_t len, cc_result_t *res,
            char *msg, size_t msgcap) {
    if (msg && msgcap) msg[0] = '\0';
    if (!name || !src || !res || !msg) return CC_EINVAL;

    char why[CC_MSG_MAX];
    if (!cc_name_ok(name, why, sizeof why)) {
        snprintf(msg, msgcap, "\"name\": %s", why);
        return CC_EINVAL;
    }
    if (!doc_ok(doc, why, sizeof why)) {
        snprintf(msg, msgcap, "%s", why);
        return CC_EINVAL;
    }
    if (nparam < 0 || nparam > CC_MAX_PARAMS) {
        snprintf(msg, msgcap, "a program may take at most %d parameters, not %d",
                 CC_MAX_PARAMS, nparam);
        return CC_EINVAL;
    }
    for (int i = 0; i < nparam; i++) {
        if (!params || !params[i] || !params[i][0] ||
            strlen(params[i]) >= CC_PARAM_NAME_MAX) {
            snprintf(msg, msgcap, "parameter %d needs a name of 1..%d characters",
                     i + 1, CC_PARAM_NAME_MAX - 1);
            return CC_EINVAL;
        }
        for (int k = 0; k < i; k++)
            if (strcmp(params[i], params[k]) == 0) {
                snprintf(msg, msgcap, "\"%s\" is used twice as a parameter name",
                         params[i]);
                return CC_EINVAL;
            }
    }
    if (len > CC_SRC_MAX) {
        snprintf(msg, msgcap, "the source is %d bytes; the limit is %d",
                 (int)len, CC_SRC_MAX);
        return CC_ELIMIT;
    }

    /* IT MUST COMPILE, and main() must have the declared arity, BEFORE anything is
     * written. So "it is in the registry" implies "the code generator has seen
     * it", and a saved program can never be one that cannot be called. */
    int rc = cc_compile(src, len, name, res);
    if (rc != CC_OK) {
        snprintf(msg, msgcap, "it does not compile, so nothing was saved");
        return rc;
    }
    if ((int)res->entry_params != nparam) {
        snprintf(msg, msgcap,
                 "main() takes %d parameter(s) but \"params\" names %d. The two "
                 "must agree: \"params\" is what a caller passes and main()'s "
                 "parameters are where they arrive",
                 (int)res->entry_params, nparam);
        return CC_EINVAL;
    }

    const cc_prog_t *existing = cc_find(name);
    if (!existing && g_nprog >= CC_MAX_PROGRAMS) {
        snprintf(msg, msgcap,
                 "this machine holds %d compiled programs, which is the limit. "
                 "Delete one first with cc_delete", CC_MAX_PROGRAMS);
        return CC_ENOSPC;
    }

    cc_prog_t p;
    memset(&p, 0, sizeof p);
    copy_z(p.name, sizeof p.name, name);
    copy_z(p.doc, sizeof p.doc, doc);
    p.version = existing ? existing->version + 1 : 1;
    p.nparam = (uint8_t)nparam;
    for (int i = 0; i < nparam; i++) copy_z(p.param[i], CC_PARAM_NAME_MAX, params[i]);
    p.src_len = (uint32_t)len;

    /* Build the record first: a record that does not fit must not have already
     * spent the backup. */
    size_t reclen = record_build(&p, src, len);
    if (reclen == 0) {
        snprintf(msg, msgcap,
                 "the record for \"%s\" does not fit in %d bytes (the source is "
                 "%d bytes before JSON escaping)", name, CC_RECORD_MAX, (int)len);
        return CC_ENOSPC;
    }

    char cur[VFS_PATH_MAX + 1], bak[VFS_PATH_MAX + 1];
    path_of(cur, sizeof cur, name, ".cc");
    path_of(bak, sizeof bak, name, ".bak");

    /* THE BACKUP FIRST. An interruption after this point leaves a good backup
     * beside whatever the current file became. */
    if (existing && file_exists(cur)) {
        size_t old_len = 0;
        static char old[CC_RECORD_MAX + 1];
        if (read_whole(cur, old, sizeof old, &old_len)) {
            if (!write_whole(bak, old, old_len)) {
                snprintf(msg, msgcap, "the previous version could not be backed up, "
                                      "so nothing was changed");
                return CC_EIO;
            }
        }
    }
    if (!write_whole(cur, g_record, reclen)) {
        snprintf(msg, msgcap, "%s could not be written", cur);
        return CC_EIO;
    }

    cc_prog_t *slot;
    if (existing) {
        slot = &g_prog[existing - g_prog];
    } else {
        slot = &g_prog[g_nprog++];
    }
    memset(slot, 0, sizeof *slot);
    *slot = p;
    slot->calls = slot->failures = 0;

    /* The image in cc.c's buffer is this program's, right now. */
    copy_z(g_img_name, sizeof g_img_name, name);
    g_img_version = p.version;
    g_img_serial  = cc_compile_serial();

    panel_render();

    snprintf(msg, msgcap,
             "saved \"%s\" v%u at %s/%s.cc (%s)%s",
             name, (unsigned)p.version, cc_store_root(), name,
             g_persists ? "it will still be here after a reboot"
                        : "RAM ONLY - this will not survive a reboot",
             existing ? ". The version before it is the ONE level of undo you have; "
                        "saving over it again loses it" : "");
    return CC_OK;
}

int cc_delete(const char *name, char *msg, size_t msgcap) {
    if (msg && msgcap) msg[0] = '\0';
    if (!name) return CC_EINVAL;
    const cc_prog_t *p = cc_find(name);
    if (!p) {
        snprintf(msg, msgcap, "there is no compiled program called \"%s\"", name);
        return CC_ENOENT;
    }
    char leaf[CC_NAME_MAX + 8];
    snprintf(leaf, sizeof leaf, "%s.cc", name);
    remove_at(cc_store_root(), leaf);
    snprintf(leaf, sizeof leaf, "%s.bak", name);
    remove_at(cc_store_root(), leaf);

    int idx = (int)(p - g_prog);
    for (int i = idx; i + 1 < g_nprog; i++) g_prog[i] = g_prog[i + 1];
    g_nprog--;
    memset(&g_prog[g_nprog], 0, sizeof g_prog[g_nprog]);

    if (strcmp(g_img_name, name) == 0) g_img_name[0] = '\0';
    panel_render();
    snprintf(msg, msgcap, "deleted \"%s\" and its backup", name);
    return CC_OK;
}

/* ===================================================================== */
/* calling a saved program                                               */
/* ===================================================================== */

int cc_call(const char *name, const uint64_t *args, int nargs, uint64_t fuel,
            cc_result_t *res) {
    if (!res) return CC_EINVAL;
    memset(res, 0, sizeof *res);
    if (!name) return CC_EINVAL;

    cc_prog_t *p = NULL;
    for (int i = 0; i < g_nprog; i++)
        if (strcmp(g_prog[i].name, name) == 0) { p = &g_prog[i]; break; }
    if (!p) {
        res->rc = CC_ENOENT;
        return CC_ENOENT;
    }
    if (nargs != p->nparam) {
        res->rc = CC_EINVAL;
        return CC_EINVAL;
    }

    p->calls++;

    /* Reuse the image only if it is provably still this program's: cc.c bumps a
     * serial on every successful compile, so an intervening cc_compile from
     * anywhere invalidates the cache without this file having to know about it. */
    int cached = (strcmp(g_img_name, name) == 0 &&
                  g_img_version == p->version &&
                  g_img_serial == cc_compile_serial() &&
                  cc_have_image());
    if (!cached) {
        int got = cc_source(name, g_src, sizeof g_src);
        if (got < 0) {
            p->failures++;
            res->rc = got;
            return got;
        }
        int rc = cc_compile(g_src, (size_t)got, name, res);
        if (rc != CC_OK) {
            p->failures++;
            g_img_name[0] = '\0';
            return rc;
        }
        if ((int)res->entry_params != p->nparam) {
            p->failures++;
            res->rc = CC_EINVAL;
            return CC_EINVAL;
        }
        copy_z(g_img_name, sizeof g_img_name, name);
        g_img_version = p->version;
        g_img_serial  = cc_compile_serial();
    }

    int rc = cc_run(args, nargs, fuel, res);
    if (rc != CC_OK) p->failures++;
    return rc;
}
