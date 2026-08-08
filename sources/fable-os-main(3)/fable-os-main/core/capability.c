/* capability.c — the capability store, its dispatcher, and its budgets.
 *
 * See include/capability.h for the contract, the calling convention, the
 * composition model and the argument for why a cycle cannot hang the machine.
 * This file is the whole of the mechanism; tools/capability_tools.c is only the
 * model's view of it, and it holds no state of its own.
 *
 * DEFENSIVE POSTURE
 *   Three untrusted inputs meet here, and they are untrusted for three different
 *   reasons:
 *
 *   1. A capability DESCRIPTION arrives from the model, through the tool layer,
 *      as names, docs, parameter lists and program source. Anything it says
 *      about itself is checked here before a byte reaches the disk, because a
 *      record that is written and only validated on the way back out is a record
 *      that can be written once and then fail for ever.
 *
 *   2. A STORED RECORD arrives from a disk that was last written by a machine
 *      that has since been switched off, possibly mid-write. It is checked
 *      against its own hash BEFORE its JSON is parsed, its name is checked
 *      against its filename, and a program is re-assembled before it is
 *      registered. A record that fails any of those never enters the registry;
 *      the backup beside it is tried instead.
 *
 *   3. The REGISTER FILE of a program that has just halted is model-authored
 *      arithmetic. r1/r2 name a span of the scratch arena and r4/r5 name a
 *      capability, so both are bounded here and the name is validated with the
 *      same rule the store uses. dvm_mem_peek() refuses an out-of-range span on
 *      its own, and this file treats a refusal as "no text", never as a short
 *      read of something else.
 */

#include "capability.h"

#include "kernel.h"
#include "trace.h"
#include "vfs.h"
#include "json.h"
#include "fs.h"          /* FS_DISK_MOUNT only; no fs_* function is called */

#include <stdarg.h>
#include <stdio.h>       /* snprintf/vsnprintf, via port/stdio.h when freestanding */

/* ====================================================================== */
/* where the store lives                                                  */
/* ====================================================================== */

/* The store, and the ONE directory a capability program may write in. They are
 * siblings and never nested, which is what makes "a capability cannot rewrite
 * the machine's capabilities" a property of the path rather than of a check:
 * vm/dvm.c confines fs.* to the data root, component by component, so
 * "/disk/cap/foo.cap" is not reachable from a root of "/disk/data". */
#define CAP_STORE_DISK  FS_DISK_MOUNT "/cap"
#define CAP_DATA_DISK   FS_DISK_MOUNT "/data"
#define CAP_STORE_RAM   "/cap"
#define CAP_DATA_RAM    "/data"

#define CAP_SUFFIX      ".cap"
#define CAP_BAKSUFFIX   ".bak"

/* store path + '/' + name + suffix + NUL, kept inside VFS_PATH_MAX. */
#define CAP_PATH_MAX    (VFS_PATH_MAX + 1)

/* The record's fixed prefix: 16 hex digits of FNV-1a-64 and a newline. Read the
 * argument for this shape in capability.h under THE FILE FORMAT — the short
 * version is that it is a fixed layout rather than a grammar, so this file
 * introduces no new parser. */
#define CAP_HASH_HEX    16
#define CAP_PREFIX_LEN  (CAP_HASH_HEX + 1)

/* Format tag inside the JSON. A record from a future format is REJECTED rather
 * than read optimistically: the fields it does not have would silently default. */
#define CAP_FORMAT      1

static char g_store[CAP_PATH_MAX];
static char g_data[CAP_PATH_MAX];
static int  g_persists;
static int  g_rejected;
static int  g_inited;

/* ====================================================================== */
/* the registry                                                           */
/* ====================================================================== */

static cap_t    g_cap[CAP_MAX];
static int      g_ncap;

/* The one program image and the one source buffer. Exactly one capability
 * program is loaded at a time — a composition holds no program, and a tail call
 * happens after its caller has halted — so a second of either would be a second
 * thing to keep in step for no gain. See capability.h's SIZE NOTE. */
static dvm_program_t g_prog;
static char          g_src[CAP_SRC_MAX + 1];
static size_t        g_srclen;
static char          g_prog_name[CAP_NAME_MAX];
static uint32_t      g_prog_version;
static int           g_prog_valid;

/* The record buffer, used for one save or one load, never both at once. */
static char   g_file[CAP_FILE_MAX];
static size_t g_filelen;

/* Scratch for a record being read off disk. Static rather than automatic
 * because cap_t is ~1.4 KiB and the kernel stack is 64 KiB shared with lwIP and
 * mbedTLS; nothing reads a record while another read is in flight (a program
 * frame never nests inside another program frame). */
static cap_t g_scratch;

static const dvm_sys_t *g_sys_override;
static int              g_sys_bound;

void capability_set_sys(const dvm_sys_t *sys) {
    g_sys_override = sys;
    g_sys_bound    = (sys != 0);
}

static const dvm_sys_t *cap_sys(void) {
    return g_sys_bound ? g_sys_override : dvm_sys_kernel();
}

/* ====================================================================== */
/* small helpers                                                          */
/* ====================================================================== */

static size_t zlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copy_z(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void say(char *msg, size_t cap, const char *fmt, ...) {
    if (!msg || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, cap, fmt, ap);
    va_end(ap);
}

/* Append to a bounded buffer, returning the new used length CLAMPED to the
 * buffer. snprintf's return value is the length it WANTED, so accumulating it
 * without clamping walks the offset past the end and hands the next call a
 * negative remaining size. */
static int catf(char *buf, size_t cap, int used, const char *fmt, ...) {
    if (!buf || !cap) return 0;
    if (used < 0) used = 0;
    if ((size_t)used >= cap) return (int)cap - 1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, cap - (size_t)used, fmt, ap);
    va_end(ap);
    if (n < 0) return used;
    used += n;
    return ((size_t)used >= cap) ? (int)cap - 1 : used;
}

/* FNV-1a, 64-bit. Not a cryptographic hash and not claimed to be one: the
 * threat it answers is a half-written sector or a bit-rotted byte, not an
 * adversary who can rewrite the file and recompute the digest. Anything with
 * write access to /disk/cap already has write access to this kernel's image. */
static uint64_t fnv1a(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void put_hex16(char *dst, uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) dst[15 - i] = hex[(v >> (i * 4)) & 0xF];
}

/* -1 when any of the 16 bytes is not a lowercase hex digit. */
static int get_hex16(const char *src, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = src[i];
        int  d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return -1;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return 0;
}

/* ====================================================================== */
/* names, docs, parameters — validated once, here                         */
/* ====================================================================== */

int capability_name_ok(const char *name, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    if (!name || !name[0]) {
        say(why, cap, "a capability needs a name");
        return 0;
    }
    if (!(name[0] >= 'a' && name[0] <= 'z')) {
        say(why, cap, "a name must start with a lowercase letter; this one starts "
                      "with \"%c\"", (name[0] >= 0x20 && name[0] < 0x7F) ? name[0] : '?');
        return 0;
    }
    size_t i = 0;
    for (; i < CAP_NAME_MAX; i++) {
        char c = name[i];
        if (c == '\0') break;
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            say(why, cap, "a name may hold only a-z, 0-9 and _; byte %d of \"%s\" "
                          "is not one of those",
                (int)i, (c >= 0x20 && c < 0x7F) ? name : "(unprintable)");
            return 0;
        }
    }
    if (i >= CAP_NAME_MAX) {
        say(why, cap, "a name may be at most %d characters", CAP_NAME_MAX - 1);
        return 0;
    }
    return 1;
}

/* A doc line is prose the model reads to decide whether to call something, so it
 * is checked like a contract: one line, printable ASCII, non-empty. A newline
 * would break the one-line panel; a control byte would change the JSON-escaped
 * length of the tool schema. Both are refused rather than stripped, because a
 * silently altered doc is a doc that says something the author did not. */
static int doc_ok(const char *doc, char *why, size_t cap) {
    if (!doc || !doc[0]) {
        say(why, cap, "a capability needs a one-line doc: it is what the model "
                      "reads to decide whether to call it");
        return 0;
    }
    for (size_t i = 0; i < CAP_DOC_MAX; i++) {
        unsigned char c = (unsigned char)doc[i];
        if (c == '\0') return 1;
        if (c < 0x20 || c >= 0x7F) {
            say(why, cap, "the doc must be one line of printable ASCII; byte %d "
                          "is 0x%x", (int)i, (unsigned)c);
            return 0;
        }
    }
    say(why, cap, "the doc may be at most %d characters", CAP_DOC_MAX - 1);
    return 0;
}

static int param_name_ok(const char *p, char *why, size_t cap) {
    if (!p || !p[0]) { say(why, cap, "a parameter needs a name"); return 0; }
    for (size_t i = 0; i < CAP_PARAM_NAME_MAX; i++) {
        char c = p[i];
        if (c == '\0') return 1;
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            say(why, cap, "parameter names may hold only a-z, 0-9 and _");
            return 0;
        }
    }
    say(why, cap, "a parameter name may be at most %d characters",
        CAP_PARAM_NAME_MAX - 1);
    return 0;
}

/* Source is stored as a JSON string and assembled on every call, so it is held
 * to printable ASCII plus newline and tab. That bounds JSON escaping at 2x (so
 * CAP_FILE_MAX is provable rather than hoped for) and it refuses the embedded
 * NUL that would truncate the program the assembler sees relative to the program
 * that was checksummed. */
static int src_ok(const char *src, size_t len, char *why, size_t cap) {
    if (!src || !len) {
        say(why, cap, "a program capability needs source; an empty body would "
                      "assemble to nothing and halt");
        return 0;
    }
    if (len > CAP_SRC_MAX) {
        say(why, cap, "the source is %d bytes; the limit is %d",
            (int)len, CAP_SRC_MAX);
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\t') continue;
        if (c < 0x20 || c >= 0x7F) {
            say(why, cap, "the source must be printable ASCII (plus newline and "
                          "tab); byte %d is 0x%x", (int)i, (unsigned)c);
            return 0;
        }
    }
    return 1;
}

/* ====================================================================== */
/* paths                                                                  */
/* ====================================================================== */

static void store_path(char *dst, size_t cap, const char *name,
                       const char *suffix) {
    snprintf(dst, cap, "%s/%s%s", g_store, name, suffix);
}

/* ====================================================================== */
/* the registry                                                           */
/* ====================================================================== */

int capability_count(void) { return g_ncap; }

const cap_t *capability_at(int index) {
    if (index < 0 || index >= g_ncap) return 0;
    return &g_cap[index];
}

const cap_t *capability_find(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_ncap; i++)
        if (streq(g_cap[i].name, name)) return &g_cap[i];
    return 0;
}

static cap_t *cap_slot(const char *name) {
    for (int i = 0; i < g_ncap; i++)
        if (streq(g_cap[i].name, name)) return &g_cap[i];
    return 0;
}

const char *capability_store_root(void) { return g_store[0] ? g_store : CAP_STORE_RAM; }
const char *capability_data_root(void)  { return g_data[0]  ? g_data  : CAP_DATA_RAM; }
int capability_store_persists(void)     { return g_persists; }
int capability_rejected(void)           { return g_rejected; }

/* Any change to the registry invalidates the loaded program image (its source
 * may be the thing that changed) and the panel the model reads. Both are
 * refreshed from ONE place so they cannot disagree with the registry. */
static void panel_render(void);

static void registry_changed(void) {
    g_prog_valid = 0;
    g_prog_name[0] = '\0';
    panel_render();
}

/* ====================================================================== */
/* file I/O                                                               */
/* ====================================================================== */

static int file_read_all(const char *path, char *buf, size_t cap, size_t *len) {
    *len = 0;
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) return CAP_ENOENT;
    size_t got = 0;
    for (;;) {
        if (got >= cap) {                 /* bigger than any legal record */
            vfs_close(f);
            return CAP_ENOSPC;
        }
        int64_t n = vfs_read(f, buf + got, (uint64_t)(cap - got));
        if (n < 0) { vfs_close(f); return CAP_EIO; }
        if (n == 0) break;
        got += (size_t)n;
    }
    vfs_close(f);
    *len = got;
    return CAP_OK;
}

static int file_write_all(const char *path, const char *buf, size_t len) {
    file_t *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    if (!f) return CAP_EIO;
    size_t done = 0;
    while (done < len) {
        int64_t n = vfs_write(f, buf + done, (uint64_t)(len - done));
        if (n <= 0) { vfs_close(f); return CAP_EIO; }
        done += (size_t)n;
    }
    vfs_close(f);
    return CAP_OK;
}

/* The VFS exposes unlink only through vnode_ops_t (there is no vfs_unlink), so
 * reach it the way tools/vfs_tools.c does: open the parent to get its vnode and
 * call the filesystem's own unlink through the public ops table. */
static int file_unlink(const char *dir, const char *leaf) {
    file_t *d = vfs_open(dir, O_RDONLY);
    if (!d) return CAP_ENOENT;
    int rc = CAP_EIO;
    if (d->node && d->node->type == VNODE_DIR && d->node->ops &&
        d->node->ops->unlink)
        rc = (d->node->ops->unlink(d->node, leaf) == VFS_OK) ? CAP_OK : CAP_EIO;
    vfs_close(d);
    return rc;
}

static int file_exists(const char *path) {
    vfs_stat_t st;
    return vfs_stat(path, &st) == VFS_OK;
}

/* ====================================================================== */
/* argument specs: "$0", "#2", "17"                                       */
/* ====================================================================== */

/* Decoded from the record and from the tool's input by the same function, so a
 * spec that is accepted at save time cannot be a spec that is rejected at load
 * time. `nparam` and `step_no` (1-based, 0 when there is no enclosing step)
 * bound the two index forms so a forward or self reference is caught here. */
static int arg_parse(const char *s, int nparam, int step_no, cap_arg_t *out,
                     char *why, size_t cap) {
    if (!s || !s[0]) { say(why, cap, "an empty argument"); return -1; }

    if (s[0] == '$' || s[0] == '#') {
        int base = 0, digits = 0;
        for (size_t i = 1; s[i]; i++) {
            if (s[i] < '0' || s[i] > '9') {
                say(why, cap, "\"%s\" is not a valid argument: %c must be followed "
                              "by digits only", s, s[0]);
                return -1;
            }
            if (digits >= 2) { say(why, cap, "\"%s\": index too large", s); return -1; }
            base = base * 10 + (s[i] - '0');
            digits++;
        }
        if (!digits) {
            say(why, cap, "\"%s\" names no index", s);
            return -1;
        }
        if (s[0] == '$') {
            if (base >= nparam) {
                say(why, cap, "\"%s\" asks for parameter %d, but this capability "
                              "declares %d", s, base, nparam);
                return -1;
            }
            out->kind = CAP_ARG_PARAM;
            out->idx  = (uint8_t)base;
            out->lit  = 0;
            return 0;
        }
        if (base < 1 || (step_no > 0 && base >= step_no)) {
            say(why, cap, "\"%s\" refers to step %d; a step may only use the result "
                          "of an EARLIER step (1..%d here)", s, base,
                step_no > 1 ? step_no - 1 : 0);
            return -1;
        }
        out->kind = CAP_ARG_STEP;
        out->idx  = (uint8_t)base;
        out->lit  = 0;
        return 0;
    }

    /* A decimal literal. Unsigned, because every value in this machine is. */
    uint64_t v = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') {
            say(why, cap, "\"%s\" is not an argument: use a decimal number, $N for "
                          "a parameter, or #N for an earlier step's result", s);
            return -1;
        }
        if (v > 0xFFFFFFFFFFFFFFFFull / 10) {
            say(why, cap, "\"%s\" does not fit in 64 bits", s);
            return -1;
        }
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    out->kind = CAP_ARG_LIT;
    out->idx  = 0;
    out->lit  = v;
    return 0;
}

/* The inverse of arg_parse(), and the ONLY renderer: the record on disk and the
 * listing the model reads are written by the same function, so a step that
 * round-trips through the store reads back as the same text. */
void capability_arg_text(const cap_arg_t *a, char *dst, size_t cap) {
    if (!dst || !cap) return;
    switch (a->kind) {
        case CAP_ARG_PARAM: snprintf(dst, cap, "$%u", (unsigned)a->idx); break;
        case CAP_ARG_STEP:  snprintf(dst, cap, "#%u", (unsigned)a->idx); break;
        default:            snprintf(dst, cap, "%lu", (unsigned long)a->lit); break;
    }
}

#define arg_render capability_arg_text

const char *capability_cmp_text(uint8_t c) {
    switch (c) {
        case CAP_CMP_EQ: return "==";
        case CAP_CMP_NE: return "!=";
        case CAP_CMP_LT: return "<";
        case CAP_CMP_LE: return "<=";
        case CAP_CMP_GT: return ">";
        case CAP_CMP_GE: return ">=";
        default:         return "";
    }
}

static uint8_t cmp_parse(const char *s) {
    if (streq(s, "==")) return CAP_CMP_EQ;
    if (streq(s, "!=")) return CAP_CMP_NE;
    if (streq(s, "<"))  return CAP_CMP_LT;
    if (streq(s, "<=")) return CAP_CMP_LE;
    if (streq(s, ">"))  return CAP_CMP_GT;
    if (streq(s, ">=")) return CAP_CMP_GE;
    return CAP_CMP_NONE;
}

/* ====================================================================== */
/* decoding the parts a stored record and a tool call have in common       */
/* ====================================================================== */

/* A capability description reaches this file two ways — off a disk, and out of
 * the model's tool input — and the parameter list and the step list mean exactly
 * the same thing in both. They are decoded ONCE, here, so a description the tool
 * accepted can never be a description the store rejects on the way back in. */

static int params_parse(const json_value_t *arr, cap_t *out,
                        char *msg, size_t cap) {
    if (arr->type != JSON_ARRAY) {
        say(msg, cap, "\"params\" must be an array of names (use [] for a "
                      "capability that takes no arguments)");
        return CAP_EINVAL;
    }
    size_t np = json_count(arr);
    if (np > CAP_MAX_PARAMS) {
        say(msg, cap, "%d parameters; the limit is %d", (int)np, CAP_MAX_PARAMS);
        return CAP_EINVAL;
    }
    for (size_t i = 0; i < np; i++) {
        json_value_t p;
        char         why[CAP_MSG_MAX];
        if (json_at(arr, i, &p) != JSON_OK ||
            json_str(&p, out->param[i], CAP_PARAM_NAME_MAX, 0) != JSON_OK ||
            !param_name_ok(out->param[i], why, sizeof why)) {
            say(msg, cap, "parameter %d must be a name of at most %d characters "
                          "from a-z, 0-9 and _", (int)i + 1,
                CAP_PARAM_NAME_MAX - 1);
            return CAP_EINVAL;
        }
    }
    out->nparam = (uint8_t)np;
    return CAP_OK;
}

static int steps_parse(const json_value_t *arr, cap_t *out,
                       char *msg, size_t cap) {
    if (arr->type != JSON_ARRAY) {
        say(msg, cap, "\"steps\" must be an array of calls");
        return CAP_EINVAL;
    }
    size_t ns = json_count(arr);
    if (ns < 1 || ns > CAP_MAX_STEPS) {
        say(msg, cap, "a composition has 1..%d steps; this one has %d",
            CAP_MAX_STEPS, (int)ns);
        return CAP_EINVAL;
    }
    for (size_t i = 0; i < ns; i++) {
        json_value_t st, a, g, e;
        cap_step_t  *s = &out->step[i];
        char         tok[24], why[CAP_MSG_MAX], opb[8];

        memset(s, 0, sizeof *s);
        if (json_at(arr, i, &st) != JSON_OK || st.type != JSON_OBJECT ||
            json_get_str(&st, "call", s->call, sizeof s->call) != JSON_OK ||
            !capability_name_ok(s->call, why, sizeof why)) {
            say(msg, cap, "step %d must be an object with a \"call\" naming a "
                          "capability", (int)i + 1);
            return CAP_EINVAL;
        }
        if (json_get(&st, "args", &a) != JSON_OK) {
            a.type = JSON_ARRAY;         /* absent means no arguments */
            a.start = "[]"; a.len = 2;
        }
        if (a.type != JSON_ARRAY || json_count(&a) > CAP_MAX_PARAMS) {
            say(msg, cap, "step %d: \"args\" must be an array of at most %d "
                          "argument strings", (int)i + 1, CAP_MAX_PARAMS);
            return CAP_EINVAL;
        }
        size_t na = json_count(&a);
        for (size_t k = 0; k < na; k++) {
            if (json_at(&a, k, &e) != JSON_OK ||
                json_str(&e, tok, sizeof tok, 0) != JSON_OK ||
                arg_parse(tok, out->nparam, (int)i + 1, &s->arg[k],
                          why, sizeof why) != 0) {
                say(msg, cap, "step %d argument %d: %s", (int)i + 1, (int)k + 1,
                    why[0] ? why : "must be a string: a number, $N or #N");
                return CAP_EINVAL;
            }
        }
        s->narg = (uint8_t)na;
        s->cmp  = CAP_CMP_NONE;

        if (json_get(&st, "when", &g) != JSON_OK) continue;
        if (g.type != JSON_ARRAY || json_count(&g) != 3) {
            say(msg, cap, "step %d: a guard is three strings, [left, op, right]",
                (int)i + 1);
            return CAP_EINVAL;
        }
        if (json_at(&g, 0, &e) != JSON_OK ||
            json_str(&e, tok, sizeof tok, 0) != JSON_OK ||
            arg_parse(tok, out->nparam, (int)i + 1, &s->lhs, why, sizeof why) != 0) {
            say(msg, cap, "step %d guard left side: %s", (int)i + 1,
                why[0] ? why : "must be a string");
            return CAP_EINVAL;
        }
        if (json_at(&g, 1, &e) != JSON_OK ||
            json_str(&e, opb, sizeof opb, 0) != JSON_OK ||
            (s->cmp = cmp_parse(opb)) == CAP_CMP_NONE) {
            say(msg, cap, "step %d guard: the comparison must be one of "
                          "== != < <= > >=", (int)i + 1);
            return CAP_EINVAL;
        }
        if (json_at(&g, 2, &e) != JSON_OK ||
            json_str(&e, tok, sizeof tok, 0) != JSON_OK ||
            arg_parse(tok, out->nparam, (int)i + 1, &s->rhs, why, sizeof why) != 0) {
            say(msg, cap, "step %d guard right side: %s", (int)i + 1,
                why[0] ? why : "must be a string");
            return CAP_EINVAL;
        }
    }
    out->nstep = (uint8_t)ns;
    return CAP_OK;
}

int capability_parse_spec(const char *input, size_t len, cap_t *out,
                          char *src, size_t srccap, size_t *srclen,
                          char *msg, size_t cap) {
    json_value_t in, v, arr;

    if (!out || !src || !srccap) return CAP_EINVAL;
    memset(out, 0, sizeof *out);
    src[0] = '\0';
    if (srclen) *srclen = 0;
    if (msg && cap) msg[0] = '\0';

    if (json_parse(input ? input : "", len, &in) != JSON_OK ||
        in.type != JSON_OBJECT) {
        say(msg, cap, "the arguments are not a JSON object");
        return CAP_EINVAL;
    }
    char why[CAP_MSG_MAX];
    if (json_get_str(&in, "name", out->name, sizeof out->name) != JSON_OK ||
        !capability_name_ok(out->name, why, sizeof why)) {
        say(msg, cap, "\"name\" is required, and at most %d characters of a-z, "
                      "0-9 and _%s%s", CAP_NAME_MAX - 1, why[0] ? ": " : "", why);
        return CAP_EINVAL;
    }
    if (json_get_str(&in, "doc", out->doc, sizeof out->doc) != JSON_OK ||
        !doc_ok(out->doc, why, sizeof why)) {
        say(msg, cap, "\"doc\": %s", why[0] ? why : "required");
        return CAP_EINVAL;
    }
    if (json_get(&in, "params", &arr) == JSON_OK) {
        int rc = params_parse(&arr, out, msg, cap);
        if (rc != CAP_OK) return rc;
    }

    int has_prog  = (json_get(&in, "program", &v)   == JSON_OK);
    int has_steps = (json_get(&in, "steps",   &arr) == JSON_OK);
    if (has_prog == has_steps) {
        say(msg, cap, has_prog
            ? "give either \"program\" or \"steps\", not both: a capability is a "
              "program or a composition of other capabilities"
            : "give either \"program\" (driver-VM source) or \"steps\" (a list of "
              "calls to capabilities that already exist)");
        return CAP_EINVAL;
    }
    if (has_prog) {
        size_t n = 0;
        if (json_str(&v, src, srccap, &n) != JSON_OK) {
            say(msg, cap, "\"program\" must be a string of at most %d bytes",
                CAP_SRC_MAX);
            return CAP_EINVAL;
        }
        if (!src_ok(src, n, why, sizeof why)) {
            say(msg, cap, "\"program\": %s", why);
            return CAP_EINVAL;
        }
        if (srclen) *srclen = n;
        out->kind = CAP_KIND_PROGRAM;
        return CAP_OK;
    }
    out->kind = CAP_KIND_COMPOSE;
    return steps_parse(&arr, out, msg, cap);
}

/* ====================================================================== */
/* serialisation                                                          */
/* ====================================================================== */

/* Write the record into g_file: 16 hex digits of hash, a newline, and the JSON.
 * The hash is computed over the JSON, so it is written last. Returns CAP_OK, or
 * CAP_ENOSPC if the record does not fit (which is a refusal, not a truncation:
 * a truncated record is a corrupt record).
 *
 * The source comes from g_src, NUL-terminated by the one function that fills it,
 * because json_put_string() takes a C string and the alternative — reaching into
 * json_writer_t to splice in an escaped span — would be this file writing its own
 * half of a JSON encoder. json_writer keeps a NUL, so a record is at most
 * CAP_FILE_MAX-1 bytes; file_read_all() relies on that to tell a legal record
 * from one too big to be one. */
static int record_write(const cap_t *c) {
    json_writer_t w;
    json_writer_init(&w, g_file + CAP_PREFIX_LEN, sizeof g_file - CAP_PREFIX_LEN);

    json_put(&w, "{\"cap\":");
    json_put_uint(&w, CAP_FORMAT);
    json_put(&w, ",\"name\":");
    json_put_string(&w, c->name);
    json_put(&w, ",\"doc\":");
    json_put_string(&w, c->doc);
    json_put(&w, ",\"version\":");
    json_put_uint(&w, c->version);
    json_put(&w, ",\"kind\":");
    json_put_string(&w, c->kind == CAP_KIND_COMPOSE ? "compose" : "program");
    json_put(&w, ",\"params\":[");
    for (int i = 0; i < c->nparam; i++) {
        if (i) json_put_char(&w, ',');
        json_put_string(&w, c->param[i]);
    }
    json_put_char(&w, ']');

    if (c->kind == CAP_KIND_COMPOSE) {
        json_put(&w, ",\"steps\":[");
        for (int i = 0; i < c->nstep; i++) {
            const cap_step_t *s = &c->step[i];
            if (i) json_put_char(&w, ',');
            json_put(&w, "{\"call\":");
            json_put_string(&w, s->call);
            json_put(&w, ",\"args\":[");
            for (int a = 0; a < s->narg; a++) {
                char t[24];
                if (a) json_put_char(&w, ',');
                arg_render(&s->arg[a], t, sizeof t);
                json_put_string(&w, t);
            }
            json_put_char(&w, ']');
            if (s->cmp != CAP_CMP_NONE) {
                char l[24], r[24];
                arg_render(&s->lhs, l, sizeof l);
                arg_render(&s->rhs, r, sizeof r);
                json_put(&w, ",\"when\":[");
                json_put_string(&w, l);
                json_put_char(&w, ',');
                json_put_string(&w, capability_cmp_text(s->cmp));
                json_put_char(&w, ',');
                json_put_string(&w, r);
                json_put_char(&w, ']');
            }
            json_put_char(&w, '}');
        }
        json_put_char(&w, ']');
    } else {
        json_put(&w, ",\"src\":");
        json_put_string(&w, g_src);
    }
    json_put_char(&w, '}');

    if (!json_writer_ok(&w)) return CAP_ENOSPC;

    size_t jlen = json_writer_len(&w);
    put_hex16(g_file, fnv1a(g_file + CAP_PREFIX_LEN, jlen));
    g_file[CAP_HASH_HEX] = '\n';
    g_filelen = CAP_PREFIX_LEN + jlen;
    return CAP_OK;
}

/* Parse g_file[0..g_filelen) into `out` (+ `src`). The hash is checked FIRST:
 * a record that failed its checksum is not handed to a parser at all, so a
 * corrupt length field can never become a parse that walks off the end.
 * Returns CAP_OK, CAP_ECORRUPT or CAP_EINVAL, with `msg` set on failure. */
static int record_parse(cap_t *out, char *src, size_t srccap, size_t *srclen,
                        char *msg, size_t cap) {
    memset(out, 0, sizeof *out);
    if (src && srccap) src[0] = '\0';
    if (srclen) *srclen = 0;

    if (g_filelen < CAP_PREFIX_LEN + 2) {
        say(msg, cap, "the record is %d bytes, too short to be one",
            (int)g_filelen);
        return CAP_ECORRUPT;
    }
    uint64_t want = 0;
    if (get_hex16(g_file, &want) != 0 || g_file[CAP_HASH_HEX] != '\n') {
        say(msg, cap, "the record does not start with 16 hex digits and a newline, "
                      "so it was not written by this kernel");
        return CAP_ECORRUPT;
    }
    size_t   jlen = g_filelen - CAP_PREFIX_LEN;
    uint64_t got  = fnv1a(g_file + CAP_PREFIX_LEN, jlen);
    if (got != want) {
        say(msg, cap, "checksum mismatch: the record says %lx and its %d bytes "
                      "hash to %lx, so the file was damaged after it was written",
            (unsigned long)want, (int)jlen, (unsigned long)got);
        return CAP_ECORRUPT;
    }
    out->crc = got;

    json_value_t root;
    if (json_parse(g_file + CAP_PREFIX_LEN, jlen, &root) != JSON_OK ||
        root.type != JSON_OBJECT) {
        say(msg, cap, "the record passed its checksum but is not a JSON object, "
                      "which means this kernel wrote something it cannot read");
        return CAP_EINVAL;
    }

    json_value_t v;
    int64_t      fmt = 0;
    if (json_get(&root, "cap", &v) != JSON_OK || json_int(&v, &fmt) != JSON_OK ||
        fmt != CAP_FORMAT) {
        say(msg, cap, "the record is format %d; this kernel reads format %d",
            (int)fmt, CAP_FORMAT);
        return CAP_EINVAL;
    }

    if (json_get_str(&root, "name", out->name, sizeof out->name) != JSON_OK ||
        !capability_name_ok(out->name, 0, 0)) {
        say(msg, cap, "the record has no usable name");
        return CAP_EINVAL;
    }
    if (json_get_str(&root, "doc", out->doc, sizeof out->doc) != JSON_OK ||
        !doc_ok(out->doc, 0, 0)) {
        say(msg, cap, "\"%s\" has no usable doc line", out->name);
        return CAP_EINVAL;
    }
    int64_t ver = 0;
    if (json_get(&root, "version", &v) != JSON_OK ||
        json_int(&v, &ver) != JSON_OK || ver < 1 || ver > 0x7FFFFFFF) {
        say(msg, cap, "\"%s\" has no usable version number", out->name);
        return CAP_EINVAL;
    }
    out->version = (uint32_t)ver;

    if (json_get(&root, "kind", &v) != JSON_OK) {
        say(msg, cap, "\"%s\" does not say what kind of capability it is", out->name);
        return CAP_EINVAL;
    }
    if      (json_str_eq(&v, "program")) out->kind = CAP_KIND_PROGRAM;
    else if (json_str_eq(&v, "compose")) out->kind = CAP_KIND_COMPOSE;
    else {
        say(msg, cap, "\"%s\" has a kind this kernel does not know", out->name);
        return CAP_EINVAL;
    }

    json_value_t params;
    char         sub[CAP_MSG_MAX];
    if (json_get(&root, "params", &params) != JSON_OK) {
        say(msg, cap, "\"%s\" has no parameter list", out->name);
        return CAP_EINVAL;
    }
    if (params_parse(&params, out, sub, sizeof sub) != CAP_OK) {
        say(msg, cap, "\"%s\": %s", out->name, sub);
        return CAP_EINVAL;
    }

    if (out->kind == CAP_KIND_PROGRAM) {
        size_t n = 0;
        if (!src || !srccap) {
            say(msg, cap, "no room was offered for \"%s\" source", out->name);
            return CAP_EINVAL;
        }
        if (json_get(&root, "src", &v) != JSON_OK ||
            json_str(&v, src, srccap, &n) != JSON_OK) {
            say(msg, cap, "\"%s\" is a program but its source is missing or longer "
                          "than the %d bytes this kernel keeps", out->name,
                CAP_SRC_MAX);
            return CAP_EINVAL;
        }
        char why[CAP_MSG_MAX];
        if (!src_ok(src, n, why, sizeof why)) {
            say(msg, cap, "\"%s\" source is not usable: %s", out->name, why);
            return CAP_EINVAL;
        }
        out->src_len = (uint32_t)n;
        if (srclen) *srclen = n;
    } else {
        json_value_t steps;
        if (json_get(&root, "steps", &steps) != JSON_OK) {
            say(msg, cap, "\"%s\" is a composition with no steps", out->name);
            return CAP_EINVAL;
        }
        if (steps_parse(&steps, out, sub, sizeof sub) != CAP_OK) {
            say(msg, cap, "\"%s\": %s", out->name, sub);
            return CAP_EINVAL;
        }
    }
    return CAP_OK;
}

/* ====================================================================== */
/* loading the store                                                      */
/* ====================================================================== */

/* Read one record file and, if everything about it checks out, register it.
 * `expect` is the name the FILENAME claims; a record that disagrees with its own
 * filename is corrupt, because the only thing that ever writes these files
 * derives one from the other. */
static int load_one(const char *path, const char *expect, int from_backup,
                    char *msg, size_t cap) {
    int rc = file_read_all(path, g_file, sizeof g_file, &g_filelen);
    if (rc != CAP_OK) {
        say(msg, cap, "%s could not be read", path);
        return rc;
    }
    size_t srclen = 0;
    rc = record_parse(&g_scratch, g_src, sizeof g_src, &srclen, msg, cap);
    if (rc != CAP_OK) return rc;

    if (expect && !streq(g_scratch.name, expect)) {
        say(msg, cap, "%s holds a capability called \"%s\"; the two disagree, so "
                      "one of them has been tampered with or damaged",
            path, g_scratch.name);
        return CAP_ECORRUPT;
    }

    /* A program is re-assembled before it is registered. "It is in the registry"
     * therefore implies "it went through the assembler on this boot", which is
     * what stops a stored program that the assembler no longer accepts from
     * becoming a call that fails every time it is made. */
    if (g_scratch.kind == CAP_KIND_PROGRAM) {
        dvm_asm_err_t err;
        if (dvm_assemble(g_src, srclen, &g_prog, &err) != 0) {
            say(msg, cap, "\"%s\" no longer assembles: line %d: %s",
                g_scratch.name, err.line, err.msg);
            return CAP_ECORRUPT;
        }
        g_scratch.ninsn = g_prog.ninsn;
        g_prog_valid = 0;               /* the image is scratch, not a cache */
    }

    cap_t *slot = cap_slot(g_scratch.name);
    if (!slot) {
        if (g_ncap >= CAP_MAX) {
            say(msg, cap, "the registry already holds %d capabilities", CAP_MAX);
            return CAP_ENOSPC;
        }
        slot = &g_cap[g_ncap++];
        /* A fresh slot starts at zero. It is not necessarily zeroed already:
         * capability_init() rewinds g_ncap without clearing the array, so slot
         * N may still hold the call counters of whatever used to live there, and
         * a reload would then attribute one capability's failures to another. */
        memset(slot, 0, sizeof *slot);
    }
    uint32_t calls = slot->calls, fails = slot->failures;
    *slot = g_scratch;
    slot->calls       = calls;
    slot->failures    = fails;
    slot->from_backup = (uint8_t)(from_backup ? 1 : 0);

    char bak[CAP_PATH_MAX];
    store_path(bak, sizeof bak, slot->name, CAP_BAKSUFFIX);
    slot->has_backup = file_exists(bak) ? 1 : 0;
    return CAP_OK;
}

/* Does `leaf` end in `suffix`? Used to pick record files out of a directory
 * that a model may also have written notes into. */
static int ends_with(const char *leaf, const char *suffix, char *stem,
                     size_t stemcap) {
    size_t l = zlen(leaf), s = zlen(suffix);
    if (l <= s) return 0;
    for (size_t i = 0; i < s; i++)
        if (leaf[l - s + i] != suffix[i]) return 0;
    if (l - s + 1 > stemcap) return 0;
    for (size_t i = 0; i < l - s; i++) stem[i] = leaf[i];
    stem[l - s] = '\0';
    return 1;
}

void capability_init(void) {
    /* Choose the store. A disk is detected by asking the VFS whether the
     * mountpoint is a directory, rather than by calling into fs/, so this file
     * links into a host test without dragging FAT32, the block layer and an ATA
     * driver behind it. */
    vfs_stat_t st;
    if (vfs_stat(FS_DISK_MOUNT, &st) == VFS_OK && st.type == VNODE_DIR) {
        copy_z(g_store, sizeof g_store, CAP_STORE_DISK);
        copy_z(g_data,  sizeof g_data,  CAP_DATA_DISK);
        g_persists = 1;
    } else {
        copy_z(g_store, sizeof g_store, CAP_STORE_RAM);
        copy_z(g_data,  sizeof g_data,  CAP_DATA_RAM);
        g_persists = 0;
    }
    vfs_mkdir(g_store);       /* idempotent: VFS_EEXIST is the normal answer */
    vfs_mkdir(g_data);

    g_ncap     = 0;
    g_rejected = 0;
    g_inited   = 1;

    char name[VFS_NAME_MAX + 1];
    char stem[CAP_NAME_MAX];
    for (uint32_t i = 0; vfs_readdir(g_store, i, name, sizeof name) == VFS_OK; i++) {
        if (!ends_with(name, CAP_SUFFIX, stem, sizeof stem)) continue;
        if (!capability_name_ok(stem, 0, 0)) {
            g_rejected++;
            kprintf("capability: ignoring \"%s\": not a capability name\n", name);
            continue;
        }
        char path[CAP_PATH_MAX], msg[CAP_MSG_MAX];
        store_path(path, sizeof path, stem, CAP_SUFFIX);
        if (load_one(path, stem, 0, msg, sizeof msg) == CAP_OK) continue;

        /* THE ROLLBACK STORY, running: the current file is unusable, so the
         * backup beside it is tried. This is the case an interrupted save
         * leaves behind, and it is why the backup is written first. */
        g_rejected++;
        kprintf("capability: REJECTED %s: %s\n", name, msg);
        char bak[CAP_PATH_MAX], msg2[CAP_MSG_MAX];
        store_path(bak, sizeof bak, stem, CAP_BAKSUFFIX);
        if (!file_exists(bak)) {
            kprintf("capability: \"%s\" has no backup to fall back to; it is "
                    "not available\n", stem);
            continue;
        }
        if (load_one(bak, stem, 1, msg2, sizeof msg2) == CAP_OK)
            kprintf("capability: \"%s\" recovered from its backup (version %u); it "
                    "runs from there until capability_revert makes it current\n",
                    stem, (unsigned)(cap_slot(stem) ? cap_slot(stem)->version : 0));
        else
            kprintf("capability: \"%s\" backup is unusable too: %s\n", stem, msg2);
    }

    /* A SECOND PASS, BECAUSE THE FIRST ONE CANNOT DO THIS.
     *
     * load_one() re-validates a stored PROGRAM by re-assembling it, and the
     * comment there says what that buys: "it is in the registry" implies "it went
     * through the assembler on this boot", which is what stops a stored program the
     * assembler no longer accepts from becoming a call that fails every time.
     *
     * That guarantee did not extend to a COMPOSITION. steps_parse() checks each
     * step's name syntax and argument specs but never resolves the callee, and both
     * checks that matter — the callee exists, and the step's argument count equals
     * its declared arity — lived only in capability_save(). So a record whose step
     * calls something with the wrong number of arguments loaded cleanly, appeared in
     * capability_list and in the fixed-width discovery panel, and returned CAP_EINVAL
     * on every single call. That state is reachable without tampering: save a
     * caller, replace it with one that does not call the callee, change the callee's
     * arity (permitted, because the guard only looks at the RAM table), and let one
     * byte of the caller's .cap rot — the next boot recovers the OLD version from
     * .bak, which does call it, with the old arity.
     *
     * It has to be a second pass rather than a line in steps_parse(): records load
     * in readdir order, so a callee may legitimately not be registered yet when its
     * caller is read.
     *
     * Refusing rather than repairing, and out loud. A capability that cannot work is
     * worse than one that is absent, because the panel advertises it. */
    for (int i = 0; i < g_ncap;) {
        const cap_t *c    = &g_cap[i];
        int          dead = 0;
        char         why[CAP_MSG_MAX];
        why[0] = '\0';

        if (c->kind == CAP_KIND_COMPOSE) {
            for (int s = 0; s < (int)c->nstep && !dead; s++) {
                const cap_step_t *st = &c->step[s];
                const cap_t *callee = streq(st->call, c->name)
                                          ? c : capability_find(st->call);
                if (!callee) {
                    say(why, sizeof why,
                        "its step %d calls \"%s\", which this machine does not "
                        "have", s + 1, st->call);
                    dead = 1;
                } else if (st->narg != callee->nparam) {
                    say(why, sizeof why,
                        "its step %d calls \"%s\" with %d argument(s) and \"%s\" "
                        "takes %d, so every call would fail",
                        s + 1, st->call, (int)st->narg, st->call,
                        (int)callee->nparam);
                    dead = 1;
                }
            }
        }

        if (!dead) { i++; continue; }

        g_rejected++;
        kprintf("capability: REJECTED \"%s\" after loading: %s\n", c->name, why);
        for (int k = i; k + 1 < g_ncap; k++) g_cap[k] = g_cap[k + 1];
        memset(&g_cap[--g_ncap], 0, sizeof g_cap[0]);
        /* Do not advance i: the slot now holds what used to follow it, and that
         * record has not been checked yet. */
    }

    registry_changed();

    kprintf("capability: %d loaded from %s (%s)", g_ncap, g_store,
            g_persists ? "survives a reboot"
                       : "RAM only - nothing here survives a reboot");
    if (g_rejected) kprintf(", %d rejected", g_rejected);
    kputs("\n");
}

/* ====================================================================== */
/* saving, reverting, deleting                                            */
/* ====================================================================== */

/* Would replacing `name` with something of arity `nparam` break a composition
 * that already calls it? This is the whole of "replacing a capability must not
 * break its callers": a step's argument count is fixed in the caller's record,
 * so a callee that changes shape has to be refused BEFORE the working version is
 * touched. Returns 0, or -1 with the offending caller named. */
static int arity_change_breaks_callers(const char *name, int nparam,
                                       char *msg, size_t cap) {
    for (int i = 0; i < g_ncap; i++) {
        const cap_t *c = &g_cap[i];
        if (c->kind != CAP_KIND_COMPOSE) continue;
        for (int s = 0; s < c->nstep; s++) {
            if (!streq(c->step[s].call, name)) continue;
            if (c->step[s].narg == nparam) continue;
            say(msg, cap,
                "\"%s\" is called by \"%s\" step %d with %d argument(s), and the "
                "replacement takes %d. Nothing was changed. Either keep the arity, "
                "or save \"%s\" under a new name and update \"%s\"",
                name, c->name, s + 1, (int)c->step[s].narg, nparam,
                name, c->name);
            return -1;
        }
    }
    return 0;
}

int capability_save(cap_t *spec, const char *src, size_t srclen,
                    char *msg, size_t cap) {
    if (msg && cap) msg[0] = '\0';
    if (!spec) return CAP_EINVAL;
    if (!g_inited) capability_init();

    char why[CAP_MSG_MAX];

    if (!capability_name_ok(spec->name, why, sizeof why)) {
        say(msg, cap, "%s", why);
        return CAP_EINVAL;
    }
    if (!doc_ok(spec->doc, why, sizeof why)) {
        say(msg, cap, "%s", why);
        return CAP_EINVAL;
    }
    if (spec->nparam > CAP_MAX_PARAMS) {
        say(msg, cap, "%d parameters; the limit is %d",
            (int)spec->nparam, CAP_MAX_PARAMS);
        return CAP_EINVAL;
    }
    for (int i = 0; i < spec->nparam; i++) {
        if (!param_name_ok(spec->param[i], why, sizeof why)) {
            say(msg, cap, "parameter %d: %s", i + 1, why);
            return CAP_EINVAL;
        }
        for (int k = 0; k < i; k++)
            if (streq(spec->param[i], spec->param[k])) {
                say(msg, cap, "two parameters are both called \"%s\"",
                    spec->param[i]);
                return CAP_EINVAL;
            }
    }

    const cap_t *old = capability_find(spec->name);
    if (!old && g_ncap >= CAP_MAX) {
        say(msg, cap, "this machine already holds %d capabilities, which is all "
                      "the registry has room for. Delete one first", CAP_MAX);
        return CAP_ENOSPC;
    }

    if (spec->kind == CAP_KIND_PROGRAM) {
        if (!src_ok(src, srclen, why, sizeof why)) {
            say(msg, cap, "%s", why);
            return CAP_EINVAL;
        }
        /* ASSEMBLE BEFORE STORING. A program that does not assemble is not a
         * capability, and finding that out at save time costs one turn where
         * finding it out at call time costs one per call, for ever. */
        dvm_asm_err_t err;
        if (dvm_assemble(src, srclen, &g_prog, &err) != 0) {
            g_prog_valid = 0;
            say(msg, cap, "the program does not assemble, so it was not saved. "
                          "Line %d: %s", err.line, err.msg);
            return CAP_EINVAL;
        }
        g_prog_valid   = 0;
        spec->ninsn    = g_prog.ninsn;
        spec->src_len  = (uint32_t)srclen;
        spec->nstep    = 0;
        /* Park the source where record_write() expects it, NUL-terminated. The
         * pointers alias when this is called from capability_revert(), which
         * loaded the backup into g_src already. */
        if (src != g_src) memcpy(g_src, src, srclen);
        g_src[srclen] = '\0';
        g_srclen      = srclen;
    } else if (spec->kind == CAP_KIND_COMPOSE) {
        if (spec->nstep < 1 || spec->nstep > CAP_MAX_STEPS) {
            say(msg, cap, "a composition has 1..%d steps; this one has %d",
                CAP_MAX_STEPS, (int)spec->nstep);
            return CAP_EINVAL;
        }
        for (int i = 0; i < spec->nstep; i++) {
            const cap_step_t *s = &spec->step[i];
            if (!capability_name_ok(s->call, why, sizeof why)) {
                say(msg, cap, "step %d: %s", i + 1, why);
                return CAP_EINVAL;
            }
            /* A step may call this capability itself — that is how recursion is
             * written, and on a first save the capability does not exist yet.
             * Anything else must already exist: an unknown callee is a typo far
             * more often than it is a forward reference, and a typo that is
             * accepted here becomes a failure at call time instead. */
            int self = streq(s->call, spec->name);
            const cap_t *callee = self ? spec : capability_find(s->call);
            if (!callee) {
                say(msg, cap, "step %d calls \"%s\", which this machine does not "
                              "have. Save it first (capability_list shows what "
                              "exists)", i + 1, s->call);
                return CAP_EINVAL;
            }
            if (s->narg != callee->nparam) {
                say(msg, cap, "step %d calls \"%s\" with %d argument(s); it takes "
                              "%d", i + 1, s->call, (int)s->narg,
                    (int)callee->nparam);
                return CAP_EINVAL;
            }
        }
        spec->src_len = 0;
        spec->ninsn   = 0;
    } else {
        say(msg, cap, "a capability is either a program or a composition");
        return CAP_EINVAL;
    }

    if (old && old->nparam != spec->nparam &&
        arity_change_breaks_callers(spec->name, spec->nparam, msg, cap) != 0)
        return CAP_EINVAL;

    spec->version = old ? old->version + 1 : 1;

    /* THE ORDER BELOW IS THE CONSISTENCY GUARANTEE. See capability.h.
     *
     * 1. copy the current record to <name>.bak and flush it (vfs_close does),
     * 2. only then overwrite <name>.cap.
     *
     * Interrupted between the two, the next boot finds a good .cap (nothing was
     * written yet) or a bad .cap and a good .bak, which capability_init()
     * recovers from. The working version is never the thing at risk. */
    char path[CAP_PATH_MAX], bak[CAP_PATH_MAX];
    store_path(path, sizeof path, spec->name, CAP_SUFFIX);
    store_path(bak,  sizeof bak,  spec->name, CAP_BAKSUFFIX);

    int have_backup = 0;
    if (old) {
        size_t n = 0;
        if (file_read_all(path, g_file, sizeof g_file, &n) == CAP_OK && n) {
            if (file_write_all(bak, g_file, n) != CAP_OK) {
                say(msg, cap, "could not write the backup %s, so the working "
                              "version of \"%s\" was left alone and nothing was "
                              "saved", bak, spec->name);
                return CAP_EIO;
            }
            have_backup = 1;
        }
        /* No readable current file: there is nothing to back up, and saying so
         * is better than inventing a backup of the new version. */
    }

    int rc = record_write(spec);
    if (rc != CAP_OK) {
        say(msg, cap, "the record does not fit in the %d bytes one capability "
                      "gets on disk; nothing was saved", CAP_FILE_MAX);
        return rc;
    }
    if (file_write_all(path, g_file, g_filelen) != CAP_OK) {
        say(msg, cap, "could not write %s. %s", path,
            have_backup ? "The previous version is still in its backup file and "
                          "capability_revert will bring it back"
                        : "Nothing was saved");
        return CAP_EIO;
    }

    /* Register only after the bytes are down, so the registry never advertises
     * something the store does not hold. */
    cap_t *slot = cap_slot(spec->name);
    if (!slot) {
        slot = &g_cap[g_ncap++];
        memset(slot, 0, sizeof *slot);   /* see load_one(): the slot may be dirty */
    }
    uint32_t calls = slot->calls, fails = slot->failures;
    *slot = *spec;
    slot->calls      = calls;
    slot->failures   = fails;
    slot->crc        = fnv1a(g_file + CAP_PREFIX_LEN, g_filelen - CAP_PREFIX_LEN);
    slot->has_backup = (uint8_t)(have_backup || file_exists(bak));

    registry_changed();
    say(msg, cap, "\"%s\" saved as version %u in %s (%s)", slot->name,
        (unsigned)slot->version, path,
        g_persists ? "it will still be here after a reboot"
                   : "RAM only: it will NOT survive a reboot");
    return CAP_OK;
}

int capability_revert(const char *name, char *msg, size_t cap) {
    if (msg && cap) msg[0] = '\0';
    if (!g_inited) capability_init();

    const cap_t *cur = capability_find(name);
    if (!cur) {
        say(msg, cap, "there is no capability called \"%s\"", name ? name : "");
        return CAP_ENOENT;
    }
    char bak[CAP_PATH_MAX];
    store_path(bak, sizeof bak, cur->name, CAP_BAKSUFFIX);
    if (!file_exists(bak)) {
        say(msg, cap, "\"%s\" is version %u and has no backup: there is exactly "
                      "one level of undo, and it has been used or was never "
                      "created", cur->name, (unsigned)cur->version);
        return CAP_ENOENT;
    }

    /* Read the backup into the scratch record, then SAVE it again. Reverting is
     * therefore an ordinary save: the version number goes UP, the thing being
     * reverted away from becomes the new backup, and reverting twice is a redo.
     * See capability.h — nothing here decrements a version. */
    size_t n = 0;
    if (file_read_all(bak, g_file, sizeof g_file, &n) != CAP_OK) {
        say(msg, cap, "the backup %s could not be read", bak);
        return CAP_EIO;
    }
    g_filelen = n;
    size_t srclen = 0;
    int rc = record_parse(&g_scratch, g_src, sizeof g_src, &srclen, msg, cap);
    if (rc != CAP_OK) return rc;
    if (!streq(g_scratch.name, cur->name)) {
        say(msg, cap, "the backup holds \"%s\", not \"%s\"", g_scratch.name,
            cur->name);
        return CAP_ECORRUPT;
    }

    uint32_t was = cur->version, from = g_scratch.version;
    g_srclen = srclen;
    rc = capability_save(&g_scratch, g_src, g_srclen, msg, cap);
    if (rc != CAP_OK) return rc;

    const cap_t *now = capability_find(g_scratch.name);
    say(msg, cap, "\"%s\" rolled back: the code of version %u is now version %u, "
                  "and version %u is the backup. Reverting again would undo this",
        g_scratch.name, (unsigned)from, (unsigned)(now ? now->version : 0),
        (unsigned)was);
    return CAP_OK;
}

int capability_delete(const char *name, char *msg, size_t cap) {
    if (msg && cap) msg[0] = '\0';
    if (!g_inited) capability_init();

    cap_t *slot = cap_slot(name ? name : "");
    if (!slot) {
        say(msg, cap, "there is no capability called \"%s\"", name ? name : "");
        return CAP_ENOENT;
    }
    /* Deleting a capability something else calls is the same class of harm as
     * changing its arity, and gets the same answer: refuse and name the caller. */
    for (int i = 0; i < g_ncap; i++) {
        const cap_t *c = &g_cap[i];
        if (c == slot || c->kind != CAP_KIND_COMPOSE) continue;
        for (int s = 0; s < c->nstep; s++)
            if (streq(c->step[s].call, slot->name)) {
                say(msg, cap, "\"%s\" is called by \"%s\" step %d. Delete or "
                              "change \"%s\" first", slot->name, c->name, s + 1,
                    c->name);
                return CAP_EEXIST;
            }
    }

    char leaf[CAP_NAME_MAX + 8];
    snprintf(leaf, sizeof leaf, "%s%s", slot->name, CAP_SUFFIX);
    int rc1 = file_unlink(g_store, leaf);
    snprintf(leaf, sizeof leaf, "%s%s", slot->name, CAP_BAKSUFFIX);
    file_unlink(g_store, leaf);          /* absent is fine */

    char gone[CAP_NAME_MAX];
    copy_z(gone, sizeof gone, slot->name);
    uint32_t ver = slot->version;

    for (int i = (int)(slot - g_cap); i < g_ncap - 1; i++) g_cap[i] = g_cap[i + 1];
    g_ncap--;
    memset(&g_cap[g_ncap], 0, sizeof g_cap[0]);
    registry_changed();

    if (rc1 != CAP_OK) {
        say(msg, cap, "\"%s\" is gone from the registry, but its file in %s could "
                      "not be removed, so it may come back at the next boot",
            gone, g_store);
        return CAP_EIO;
    }
    say(msg, cap, "\"%s\" (version %u) deleted, backup included", gone,
        (unsigned)ver);
    return CAP_OK;
}

/* ====================================================================== */
/* the discovery panel                                                    */
/* ====================================================================== */

static char  g_panel[CAP_PANEL_CHARS + 1];
static char *g_panel_dst;
static size_t g_panel_dstcap;

/* Everything the model sees about the store, on ONE line of exactly
 * CAP_PANEL_CHARS characters. The fixed width is not tidiness — it is what keeps
 * the assembled tool schema a compile-time constant while its CONTENTS come off
 * a disk. See capability.h under DISCOVERABILITY. */
static void panel_render(void) {
    char tmp[CAP_PANEL_CHARS * 2];
    int  n;

    n = catf(tmp, sizeof tmp, 0, "store %s (%s). ",
             g_store[0] ? g_store : CAP_STORE_RAM,
             g_persists ? "survives reboot" : "RAM ONLY, lost at reboot");

    if (g_ncap == 0) {
        n = catf(tmp, sizeof tmp, n,
                 "NONE SAVED YET - this machine has not been taught anything "
                 "beyond its built-in tools. capability_save is how it learns.");
    } else {
        for (int i = 0; i < g_ncap; i++) {
            const cap_t *c = &g_cap[i];
            char sig[CAP_NAME_MAX + CAP_MAX_PARAMS * (CAP_PARAM_NAME_MAX + 1) + 8];
            char row[CAP_DOC_MAX + sizeof sig + 24];
            int  s = catf(sig, sizeof sig, 0, "%s(", c->name);
            for (int p = 0; p < c->nparam; p++)
                s = catf(sig, sizeof sig, s, "%s%s", p ? "," : "", c->param[p]);
            catf(sig, sizeof sig, s, ")");

            int rl = catf(row, sizeof row, 0, "%s v%u: %s | ", sig,
                          (unsigned)c->version, c->doc);
            /* Reserve room for the "+N more" note, so the list never ends in a
             * half-written entry: a clipped name is a name the model would call
             * and be told does not exist. */
            if ((size_t)(n + rl) + 26 >= CAP_PANEL_CHARS) {
                n = catf(tmp, sizeof tmp, n, "+%d more (capability_list)",
                         g_ncap - i);
                break;
            }
            n = catf(tmp, sizeof tmp, n, "%s", row);
        }
    }
    size_t nn = (n > 0) ? (size_t)n : 0;

    /* Force the invariant rather than trust the writers above: printable ASCII,
     * no '"', no '\\', and no control byte, so the JSON-escaped length of this
     * panel equals its raw length and the schema size cannot move. */
    for (size_t i = 0; i < nn && i < CAP_PANEL_CHARS; i++) {
        unsigned char c = (unsigned char)tmp[i];
        g_panel[i] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\')
                     ? (char)c : '.';
    }
    for (size_t i = (nn < CAP_PANEL_CHARS ? nn : CAP_PANEL_CHARS);
         i < CAP_PANEL_CHARS; i++)
        g_panel[i] = ' ';
    g_panel[CAP_PANEL_CHARS] = '\0';

    /* And push it into the tool description, if one has been bound. Exactly
     * CAP_PANEL_CHARS bytes, never a NUL: the destination is the TAIL of a
     * longer string literal and its terminator is already in place. */
    if (g_panel_dst && g_panel_dstcap >= CAP_PANEL_CHARS)
        memcpy(g_panel_dst, g_panel, CAP_PANEL_CHARS);
}

const char *capability_panel(void) {
    if (!g_panel[0]) panel_render();
    return g_panel;
}

int capability_panel_check(char *msg, size_t cap) {
    const char *p = capability_panel();
    size_t      n = zlen(p);
    if (n != CAP_PANEL_CHARS) {
        say(msg, cap, "the capability panel is %d characters but %d are reserved "
                      "in the tool schema; the two must agree or the schema "
                      "changes size when a capability is saved",
            (int)n, CAP_PANEL_CHARS);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c >= 0x7F || c == '"' || c == '\\') {
            say(msg, cap, "capability panel byte %d is 0x%x, which JSON escaping "
                          "would lengthen", (int)i, (unsigned)c);
            return 0;
        }
    }
    say(msg, cap, "%d characters, as reserved", (int)n);
    return 1;
}

void capability_panel_bind(char *dst, size_t cap) {
    g_panel_dst    = dst;
    g_panel_dstcap = cap;
    panel_render();
}

/* ====================================================================== */
/* the sandbox                                                            */
/* ====================================================================== */

/* Identical for every capability, built here in C, from nothing the record can
 * influence: a saved capability cannot widen its own reach. NO DEVICE ACCESS AT
 * ALL — dvm_policy_init() denies everything and the only thing opened after it
 * is the syscall list in capability.h's THE SANDBOX. */
static int cap_sandbox(dvm_policy_t *pol, uint64_t steps, uint64_t delay_us,
                       dvm_trace_t trace, char *msg, size_t cap) {
    dvm_policy_init(pol);
    pol->max_steps            = steps;
    pol->max_io               = 0;
    pol->max_dma_ops          = 0;
    pol->max_mem_bytes        = CAP_RUN_MEM_BYTES;
    pol->max_sys              = CAP_RUN_SYS;
    pol->max_prints           = CAP_RUN_PRINTS;
    pol->max_delay_us         = delay_us;
    pol->max_single_delay_us  = CAP_RUN_SINGLE_DELAY_US;
    pol->trace                = trace;
    pol->max_trace            = 16;

    dvm_policy_allow_sys(pol, DVM_SYS_CON_WRITE);
    dvm_policy_allow_sys(pol, DVM_SYS_FS_READ);
    dvm_policy_allow_sys(pol, DVM_SYS_FS_WRITE);
    dvm_policy_allow_sys(pol, DVM_SYS_FS_SIZE);
    dvm_policy_allow_sys(pol, DVM_SYS_TIME_MS);
    if (dvm_policy_set_fs_root(pol, capability_data_root()) != 0) {
        say(msg, cap, "the VM refused \"%s\" as a filesystem root, which is a "
                      "kernel bug", capability_data_root());
        return -1;
    }
    char pmsg[DVM_MSG_MAX];
    if (dvm_policy_check(pol, pmsg, sizeof pmsg) != DVM_OK) {
        say(msg, cap, "the VM refused the capability sandbox, which is a kernel "
                      "bug rather than a problem with this capability: %s", pmsg);
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* invocation                                                             */
/* ====================================================================== */

typedef struct {
    cap_result_t *res;
    uint32_t      depth;        /* composition frames currently open */
    uint64_t      steps_left;
    uint64_t      delay_left;
} cap_ctx_t;

/* Append one hop to the call chain, which is what turns "recursion limit" into
 * something a model can act on. Once it is full it ends in " ..." exactly once,
 * so a 64-deep runaway still reports a readable head of the path. */
static void chain_add(cap_result_t *res, const char *name) {
    size_t n = zlen(res->chain);
    size_t need = zlen(name) + (n ? 4 : 0);

    if (n + need + 1 > sizeof res->chain) {
        const char *ell = " ...";
        size_t      el  = 4;
        if (n >= el && streq(res->chain + n - el, ell)) return;
        if (n + el + 1 > sizeof res->chain) n = sizeof res->chain - el - 1;
        copy_z(res->chain + n, sizeof res->chain - n, ell);
        return;
    }
    if (n) { copy_z(res->chain + n, sizeof res->chain - n, " -> "); n += 4; }
    copy_z(res->chain + n, sizeof res->chain - n, name);
}

/* Read the loaded source for `c` into g_prog, using the one image as a cache.
 * The cache is keyed on name AND version, and every registry change drops it, so
 * it can never serve the code of a capability that has been replaced. It also
 * pins one version for the duration of a call chain, which is the semantics a
 * recursive tail call wants. */
static int program_load(const cap_t *c, char *msg, size_t cap) {
    if (g_prog_valid && g_prog_version == c->version && streq(g_prog_name, c->name))
        return CAP_OK;

    /* From the backup when that is where this record came from — see cap_t's
     * from_backup. Reading metadata from one file and source from another is a
     * mistake that presents as "the capability you were just told was recovered
     * is corrupt". */
    char path[CAP_PATH_MAX];
    store_path(path, sizeof path, c->name,
               c->from_backup ? CAP_BAKSUFFIX : CAP_SUFFIX);

    g_prog_valid = 0;
    if (file_read_all(path, g_file, sizeof g_file, &g_filelen) != CAP_OK) {
        say(msg, cap, "\"%s\" is in the registry but %s could not be read; the "
                      "store has changed under this kernel", c->name, path);
        return CAP_EIO;
    }
    size_t srclen = 0;
    int rc = record_parse(&g_scratch, g_src, sizeof g_src, &srclen, msg, cap);
    if (rc != CAP_OK) return rc;
    if (!streq(g_scratch.name, c->name) || g_scratch.version != c->version) {
        say(msg, cap, "%s now holds \"%s\" version %u, not \"%s\" version %u",
            path, g_scratch.name, (unsigned)g_scratch.version, c->name,
            (unsigned)c->version);
        return CAP_ECORRUPT;
    }
    dvm_asm_err_t err;
    if (dvm_assemble(g_src, srclen, &g_prog, &err) != 0) {
        say(msg, cap, "\"%s\" no longer assembles: line %d: %s", c->name,
            err.line, err.msg);
        return CAP_ECORRUPT;
    }
    g_srclen = srclen;
    copy_z(g_prog_name, sizeof g_prog_name, c->name);
    g_prog_version = c->version;
    g_prog_valid   = 1;
    return CAP_OK;
}

/* Collect the text result a program left behind: r1 = offset, r2 = length in the
 * scratch arena. Both are model arithmetic, so dvm_mem_peek() is asked for the
 * span and a refusal (a span outside the arena) is treated as NO TEXT, never as
 * a short read of something else. Bytes are forced to printable ASCII here
 * because they end up in a tool result the model reads back. */
static void collect_text(cap_result_t *res, const dvm_result_t *dr) {
    uint64_t off = dr->reg[CAP_REG_TEXT_OFF];
    uint64_t len = dr->reg[CAP_REG_TEXT_LEN];

    res->text[0]  = '\0';
    res->text_len = 0;
    if (!len) return;

    size_t want = (len > CAP_TEXT_MAX - 1) ? CAP_TEXT_MAX - 1 : (size_t)len;
    size_t got  = dvm_mem_peek(off, res->text, want);
    for (size_t i = 0; i < got; i++) {
        unsigned char ch = (unsigned char)res->text[i];
        if (ch == '\n' || ch == '\t') { res->text[i] = ' '; continue; }
        if (ch < 0x20 || ch >= 0x7F) res->text[i] = '.';
    }
    res->text[got] = '\0';
    res->text_len  = got;
}

/* The name of a tail-call target, as bytes the program built in scratch memory.
 * Validated with capability_name_ok(), the same rule the store uses, so a
 * program cannot name something the store could not hold. */
static int tail_name(const dvm_result_t *dr, char *out, size_t cap,
                     char *msg, size_t msgcap) {
    uint64_t off = dr->reg[CAP_REG_TAIL_NAME_OFF];
    uint64_t len = dr->reg[CAP_REG_TAIL_NAME_LEN];

    out[0] = '\0';
    if (len == 0 || len > cap - 1) {
        say(msg, msgcap, "it asked for a tail call with a name %lu bytes long; "
                         "1..%d is the range (r5 is the length)",
            (unsigned long)len, (int)cap - 1);
        return -1;
    }
    size_t got = dvm_mem_peek(off, out, (size_t)len);
    if (got != (size_t)len) {
        say(msg, msgcap, "it asked for a tail call whose name is at scratch offset "
                         "%lu+%lu, which is outside the %u-byte arena",
            (unsigned long)off, (unsigned long)len, (unsigned)DVM_MEM_SIZE);
        return -1;
    }
    out[got] = '\0';
    char why[CAP_MSG_MAX];
    if (!capability_name_ok(out, why, sizeof why)) {
        say(msg, msgcap, "it asked for a tail call to a name that is not usable: %s",
            why);
        return -1;
    }
    return 0;
}

/* Names of what exists, for an ENOENT message. A model that misremembers a name
 * gets the real list back, exactly as core/tool.c does for a tool. */
static void list_names(char *dst, size_t cap) {
    int n = 0;
    dst[0] = '\0';
    if (!g_ncap) { copy_z(dst, cap, "none are saved"); return; }
    for (int i = 0; i < g_ncap; i++) {
        if ((size_t)n + zlen(g_cap[i].name) + 6 >= cap) {
            catf(dst, cap, n, " ...");
            return;
        }
        n = catf(dst, cap, n, "%s%s", n ? ", " : "", g_cap[i].name);
    }
}

static int invoke_one(cap_ctx_t *ctx, const char *name, const uint64_t *args,
                      int nargs, uint64_t *value);

/* One composition: run the steps in order, honouring guards, wiring arguments.
 * This is the ONE place that recurses, which is why CAP_MAX_DEPTH is the bound
 * on kernel stack and why it is checked before the frame is entered. */
static int run_compose(cap_ctx_t *ctx, const cap_t *c, const uint64_t *args,
                       uint64_t *value) {
    cap_result_t *res = ctx->res;
    uint64_t      stepval[CAP_MAX_STEPS];
    uint8_t       ran[CAP_MAX_STEPS];
    int           last = -1;

    if (ctx->depth + 1 > CAP_MAX_DEPTH) {
        /* THE CHAIN IS NOT IN THIS SENTENCE, and that is deliberate. It used to
         * be, and at depth 8 it filled all 224 bytes and pushed out the half of
         * the message that says what to DO — a model was told "recursion limit"
         * with a path and no advice. The chain is a field of its own
         * (cap_result_t.chain) and the tool prints it on its own line. */
        res->rc = CAP_EDEPTH;
        say(res->msg, sizeof res->msg,
            "\"%s\" nests compositions more than %d deep. A composition that "
            "calls itself needs a \"when\" guard that eventually fails, or an "
            "argument that changes on the way down", c->name, CAP_MAX_DEPTH);
        return CAP_EDEPTH;
    }
    ctx->depth++;
    if (ctx->depth > res->depth_used) res->depth_used = ctx->depth;

    for (int i = 0; i < CAP_MAX_STEPS; i++) { stepval[i] = 0; ran[i] = 0; }

    for (int i = 0; i < c->nstep; i++) {
        const cap_step_t *s = &c->step[i];
        uint64_t          a[CAP_MAX_PARAMS];

        /* Resolve one spec. A reference to a step that a guard skipped is a
         * refusal with both step numbers, not a silent zero: a zero here would
         * be an argument the author never wrote. */
        #define RESOLVE(spec, out)                                              \
            do {                                                                \
                const cap_arg_t *_s = (spec);                                   \
                if (_s->kind == CAP_ARG_PARAM) (out) = args[_s->idx];           \
                else if (_s->kind == CAP_ARG_STEP) {                            \
                    if (!ran[_s->idx - 1]) {                                    \
                        res->rc = CAP_EINVAL;                                   \
                        say(res->msg, sizeof res->msg,                          \
                            "\"%s\" step %d uses #%d, but step %d was skipped "  \
                            "by its own guard and produced no result",          \
                            c->name, i + 1, (int)_s->idx, (int)_s->idx);        \
                        ctx->depth--;                                           \
                        return CAP_EINVAL;                                      \
                    }                                                           \
                    (out) = stepval[_s->idx - 1];                               \
                } else (out) = _s->lit;                                         \
            } while (0)

        if (s->cmp != CAP_CMP_NONE) {
            uint64_t l, r;
            int      take;
            RESOLVE(&s->lhs, l);
            RESOLVE(&s->rhs, r);
            switch (s->cmp) {
                case CAP_CMP_EQ: take = (l == r); break;
                case CAP_CMP_NE: take = (l != r); break;
                case CAP_CMP_LT: take = (l <  r); break;
                case CAP_CMP_LE: take = (l <= r); break;
                case CAP_CMP_GT: take = (l >  r); break;
                default:         take = (l >= r); break;
            }
            if (!take) continue;
        }

        for (int k = 0; k < s->narg; k++) RESOLVE(&s->arg[k], a[k]);
        #undef RESOLVE

        uint64_t v = 0;
        int rc = invoke_one(ctx, s->call, a, s->narg, &v);
        if (rc != CAP_OK) { ctx->depth--; return rc; }
        stepval[i] = v;
        ran[i]     = 1;
        last       = i;
    }

    ctx->depth--;
    if (last < 0) {
        /* Every step's guard was false. That is a legal outcome, not a failure —
         * it is how a recursive composition terminates — and the result is 0
         * with the text cleared, so nothing stale is reported as an answer. */
        res->text[0]  = '\0';
        res->text_len = 0;
        *value = 0;
        return CAP_OK;
    }
    *value = stepval[last];
    return CAP_OK;
}

/* One capability, to completion, tail calls included. The tail-call path is a
 * LOOP and not recursion: nothing of the caller has to survive it (see
 * capability.h), so a chain of tail calls costs no kernel stack and is bounded
 * only by CAP_MAX_INVOCATIONS. */
static int invoke_one(cap_ctx_t *ctx, const char *name, const uint64_t *args,
                      int nargs, uint64_t *value) {
    cap_result_t *res = ctx->res;
    char          next[CAP_NAME_MAX];
    uint64_t      nextargs[CAP_MAX_PARAMS];

    for (;;) {
        cap_t *c = cap_slot(name);
        if (!c) {
            char have[160];
            list_names(have, sizeof have);
            res->rc = CAP_ENOENT;
            say(res->msg, sizeof res->msg,
                "there is no capability called \"%s\". Saved: %s",
                name, have);
            return CAP_ENOENT;
        }
        if (nargs != c->nparam) {
            res->rc = CAP_EINVAL;
            say(res->msg, sizeof res->msg,
                "\"%s\" takes %d argument(s) and was called with %d", c->name,
                (int)c->nparam, nargs);
            return CAP_EINVAL;
        }

        /* The invocation budget is checked HERE, after the callee is known, so
         * the failure is attributed to the capability that was about to run
         * rather than to nobody. It is the counter an unguarded tail-call loop
         * hits, and it only ever goes up within one top-level call. */
        if (res->invocations >= CAP_MAX_INVOCATIONS) {
            res->rc = CAP_EBUDGET;
            say(res->msg, sizeof res->msg,
                "%d capability invocations in one call, which is the limit. "
                "Something in this chain calls itself without a condition that "
                "ends it; \"%s\" was next", CAP_MAX_INVOCATIONS, c->name);
            c->failures++;
            return CAP_EBUDGET;
        }

        res->invocations++;
        chain_add(res, c->name);
        c->calls++;

        if (c->kind == CAP_KIND_COMPOSE) {
            int rc = run_compose(ctx, c, args, value);
            if (rc != CAP_OK) c->failures++;
            /* A composition has no program of its own, so it cannot tail-call. */
            return rc;
        }

        /* ---- a program capability ---- */
        if (ctx->steps_left == 0 || ctx->delay_left == 0) {
            res->rc = CAP_EBUDGET;
            say(res->msg, sizeof res->msg,
                "this call has spent its whole tree budget (%lu VM instructions "
                "and %lu us of delay, summed across every capability in the "
                "chain) before reaching \"%s\"",
                (unsigned long)CAP_TREE_STEPS, (unsigned long)CAP_TREE_DELAY_US,
                c->name);
            c->failures++;
            return CAP_EBUDGET;
        }

        char msg[CAP_MSG_MAX];
        int  rc = program_load(c, msg, sizeof msg);
        if (rc != CAP_OK) {
            res->rc = rc;
            say(res->msg, sizeof res->msg, "%s", msg);
            c->failures++;
            return rc;
        }

        uint64_t steps = ctx->steps_left < CAP_RUN_STEPS ? ctx->steps_left
                                                         : CAP_RUN_STEPS;
        uint64_t delay = ctx->delay_left < CAP_RUN_DELAY_US ? ctx->delay_left
                                                            : CAP_RUN_DELAY_US;
        dvm_policy_t pol;
        /* Only the FIRST run of a call traces itself. The grant block is
         * identical for every capability in the tree — it is built from nothing
         * the record can influence — so repeating it sixty-three times says
         * nothing new and buries the operator. Every invocation still gets its
         * own kernel-authored cap.call line below, and con.write is unaffected
         * because it goes out through the backend rather than the trace level. */
        dvm_trace_t trace = (res->invocations == 1) ? DVM_TRACE_IO : DVM_TRACE_OFF;
        if (cap_sandbox(&pol, steps, delay, trace, msg, sizeof msg) != 0) {
            res->rc = CAP_ETRAP;
            say(res->msg, sizeof res->msg, "%s", msg);
            c->failures++;
            return CAP_ETRAP;
        }

        dvm_io_t io;
        memset(&io, 0, sizeof io);       /* every device hook NULL, on purpose */
        io.sys = cap_sys();

        dvm_result_t dr;
        dvm_status_t st = dvm_run(&g_prog, &pol, &io, args, nargs, &dr);

        res->steps    += dr.steps;
        res->delay_us += dr.delay_us;
        res->sys_calls += dr.n_sys;
        ctx->steps_left = (ctx->steps_left > dr.steps) ? ctx->steps_left - dr.steps : 0;
        ctx->delay_left = (ctx->delay_left > dr.delay_us) ? ctx->delay_left - dr.delay_us : 0;

        if (st != DVM_OK) {
            res->rc = CAP_ETRAP;
            say(res->msg, sizeof res->msg,
                "\"%s\" v%u stopped at source line %u with %s: %s%s%s",
                c->name, (unsigned)c->version, (unsigned)dr.line,
                dvm_status_name(st), dr.msg,
                dr.sys_msg[0] ? " | last syscall: " : "", dr.sys_msg);
            trace_err(CAP_ETRAP, "cap.call", "%s v%u depth=%u %s", c->name,
                      (unsigned)c->version, (unsigned)ctx->depth,
                      dvm_status_name(st));
            c->failures++;
            return CAP_ETRAP;
        }

        *value      = dr.reg[CAP_REG_RESULT];
        res->value  = *value;
        collect_text(res, &dr);

        if (dr.reg[CAP_REG_TAIL] != CAP_TAILCALL) {
            trace_ret((long)*value, "cap.call", "%s v%u depth=%u", c->name,
                      (unsigned)c->version, (unsigned)ctx->depth);
            return CAP_OK;
        }

        /* A tail call. Resolve it fully before anything is committed, so a
         * malformed request fails as this capability's failure rather than as a
         * mysterious ENOENT one frame later. */
        if (tail_name(&dr, next, sizeof next, msg, sizeof msg) != 0) {
            res->rc = CAP_EINVAL;
            say(res->msg, sizeof res->msg, "\"%s\" v%u: %s", c->name,
                (unsigned)c->version, msg);
            c->failures++;
            return CAP_EINVAL;
        }
        int nn = 0;
        for (int i = 0; i < CAP_MAX_PARAMS; i++)
            nextargs[i] = dr.reg[CAP_REG_TAIL_ARG0 + i];
        const cap_t *callee = capability_find(next);
        nn = callee ? callee->nparam : 0;
        if (!callee) {
            char have[160];
            list_names(have, sizeof have);
            res->rc = CAP_ENOENT;
            say(res->msg, sizeof res->msg,
                "\"%s\" v%u tail-called \"%s\", which this machine does not have. "
                "Saved: %s", c->name, (unsigned)c->version, next, have);
            c->failures++;
            return CAP_ENOENT;
        }
        trace_ret((long)*value, "cap.tail", "%s v%u -> %s", c->name,
                  (unsigned)c->version, next);

        /* Loop instead of recursing. `name`/`args` are rebound to the callee. */
        name  = next;
        args  = nextargs;
        nargs = nn;
    }
}

int capability_invoke(const char *name, const uint64_t *args, int nargs,
                      cap_result_t *out) {
    if (!out) return CAP_EINVAL;
    memset(out, 0, sizeof *out);
    if (!g_inited) capability_init();

    if (nargs < 0 || nargs > CAP_MAX_PARAMS) {
        out->rc = CAP_EINVAL;
        say(out->msg, sizeof out->msg,
            "%d arguments; a capability takes at most %d", nargs, CAP_MAX_PARAMS);
        return CAP_EINVAL;
    }
    if (!name || !name[0]) {
        out->rc = CAP_EINVAL;
        say(out->msg, sizeof out->msg, "no capability was named");
        return CAP_EINVAL;
    }
    if (!cap_sys()) {
        out->rc = CAP_ETRAP;
        say(out->msg, sizeof out->msg,
            "this build has no kernel syscall backend, so a capability cannot be "
            "run at all");
        return CAP_ETRAP;
    }

    uint64_t     a[CAP_MAX_PARAMS];
    for (int i = 0; i < CAP_MAX_PARAMS; i++) a[i] = (i < nargs && args) ? args[i] : 0;

    cap_ctx_t ctx;
    ctx.res        = out;
    ctx.depth      = 0;
    ctx.steps_left = CAP_TREE_STEPS;
    ctx.delay_left = CAP_TREE_DELAY_US;

    uint64_t v = 0;
    int rc = invoke_one(&ctx, name, a, nargs, &v);
    out->rc    = rc;
    if (rc == CAP_OK) {
        out->value = v;
        out->msg[0] = '\0';
    }
    return rc;
}
