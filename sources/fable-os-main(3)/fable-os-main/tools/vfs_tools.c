/* vfs_tools.c — the filesystem family of the model's syscall surface.
 *
 * PURPOSE
 *   Give the model real, bounded access to the kernel's VFS. Six tools:
 *
 *       read_file    read bytes out of a file, with paging for large files
 *       write_file   create/overwrite/append a file
 *       list_dir     enumerate a directory, with type and size per entry
 *       stat_path    metadata for one path (also: "does this exist?")
 *       make_dir     create a directory, optionally with its parents
 *       delete_path  remove a file or an empty directory
 *
 *   Why exactly these: they are the closure of "inspect, then change, then
 *   verify". stat_path/list_dir let the model look before it leaps; read_file
 *   lets it verify after; write_file/make_dir/delete_path are the only three
 *   that mutate, and each is marked TOOL_MUTATES so a future read-only mode or
 *   confirmation prompt has a single hook. There is no rename and no recursive
 *   delete on purpose — see DEFERRED at the bottom.
 *
 *   Every call ends in exactly one kernel-emitted trace line carrying the real
 *   arguments and the real return code, e.g.
 *
 *       [vfs_write /etc/motd mode=overwrite bytes=34 -> ok]
 *       [vfs_read /etc/motd off=0 -> 33]
 *       [vfs_unlink /tmp/scratch type=file size=12 -> ok]
 *       [vfs_stat /nope -> ENOENT]
 *
 *   That line is the operator's only way to know an action happened, because
 *   this machine has no shell to inspect it with (see trace.h).
 *
 * SAFETY MODEL
 *   `call->input` is model output: untrusted, not NUL-terminated, possibly
 *   malformed. Nothing here indexes it directly — json.h does all the reading,
 *   always with a length. On top of that every argument is validated before it
 *   reaches the VFS:
 *
 *     - paths must be a JSON string, non-empty, absolute, <= VFST_PATH_MAX-1
 *       bytes, free of embedded NULs and control bytes, with every component
 *       <= VFS_NAME_MAX. `..` is rejected outright and `.`/`//` are folded
 *       away, so what reaches the VFS is a canonical path.
 *     - the path bound is deliberately 127, below the 128-byte parent-path
 *       buffer inside fs/vfs/vfs.c, so a long path can never be silently
 *       truncated into a *different* directory.
 *     - numbers must be JSON integers in an explicit range; a negative offset
 *       or a bogus length is an error message, never a wild seek.
 *     - a write is capped at VFST_WRITE_MAX bytes and its staging buffer is
 *       sized from the (already bounded) raw JSON span, so the model cannot
 *       make the kernel allocate more than that.
 *
 *   Every rejection becomes a sentence the model can read and act on next turn,
 *   plus an `-> EINVAL` trace line. No *argument* this file validates can panic
 *   the machine.
 *
 *   ONE HONEST EXCEPTION, because it is not this file's to fix: read_file and
 *   write_file kmalloc a staging buffer, and mm/heap.c's kmalloc does not return
 *   NULL on exhaustion — it panics (`kmalloc: out of memory`), which halts the
 *   machine with interrupts off and no way to say why. The `if (!buf)` ENOSPC
 *   branches below are therefore UNREACHABLE today. They are kept rather than
 *   deleted because they are the correct handling and become live the moment the
 *   allocator learns to fail; what is wrong is the allocator's contract, not
 *   these branches. Sizes here are bounded (VFST_WRITE_MAX, and a read buffer
 *   sized from an already-clamped span) so the model cannot aim at this, but a
 *   fragmented arena can still reach it.
 *
 * OUTPUT BOUNDS
 *   Results share one caller-owned buffer (TOOL_RESULT_MAX = 4096). read_file
 *   and list_dir therefore truncate — and say so, in the result text, with the
 *   offset to resume from. A silently-clipped prefix that the model then
 *   reasons about as if complete is the failure mode being designed out.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "vfs.h"
#include "kernel.h"

#include <stdarg.h>
#include <stdio.h>      /* vsnprintf / snprintf, via port/stdio.h freestanding */
#include <string.h>

/* ---------------------------------------------------------------------- */
/* bounds                                                                  */
/* ---------------------------------------------------------------------- */

/* Max path length including the NUL. Held below the 128-byte parent-path
 * buffer in fs/vfs/vfs.c so resolve_parent() can never truncate ours. */
#define VFST_PATH_MAX     128

/* Largest single write, after JSON unescaping. */
#define VFST_WRITE_MAX    (64 * 1024)

/* Largest number of directory entries considered in one call. */
#define VFST_LIST_MAX     512

/* Bytes of the result buffer held back for the header/footer of read_file. */
#define VFST_HDR_RESERVE  384

/* Longest argument-error sentence we build. */
#define VFST_ERR_MAX      192

/* REGISTER_TOOL (tool.h) places a pointer in the linker-collected "tool_table"
 * section. Mach-O — the host-test target on macOS — rejects a bare section name
 * and has no __start_/__stop_ symbols, so the host build uses the equivalent
 * "__DATA,tool_table" spelling and tests/host/test_vfs_tools.c supplies the
 * matching boundary symbols. The kernel build takes the normal path untouched. */
#if defined(FABLEOS_HOSTTEST) && defined(__APPLE__)
#  define REGISTER_VFS_TOOL(v)                                                 \
      static const tool_t *const __toolptr_##v                                 \
          __attribute__((used, section("__DATA,tool_table"))) = &(v)
#else
#  define REGISTER_VFS_TOOL(v) REGISTER_TOOL(v)
#endif

/* ---------------------------------------------------------------------- */
/* failure reporting                                                       */
/* ---------------------------------------------------------------------- */

/* One place where a recoverable failure turns into (a) a sentence for the model
 * and (b) the kernel's ground-truth trace line. `targs` is what appears between
 * the op and the arrow. Always returns `err` so handlers can `return fail(...)`. */
static int fail(tool_result_t *r, const char *op, const char *targs, int err,
                const char *fmt, ...) {
    va_list ap;
    r->is_error = 1;
    va_start(ap, fmt);
    {
        /* Big enough for the longest sentence below with a maximum-length path
         * interpolated into it; vsnprintf clamps regardless. */
        char msg[VFST_ERR_MAX + VFST_PATH_MAX + 64];
        vsnprintf(msg, sizeof msg, fmt, ap);
        tool_result_printf(r, "%s", msg);
    }
    va_end(ap);
    trace_err(err, op, "%s", targs ? targs : "");
    return err;
}

/* An argument was rejected before any path was established, so the trace line
 * carries the reason instead: `[vfs_write bad-args: ... -> EINVAL]`. */
static int fail_args(tool_result_t *r, const char *op, const char *why) {
    char targs[VFST_ERR_MAX + 16];
    snprintf(targs, sizeof targs, "bad-args: %s", why);
    return fail(r, op, targs, TOOL_EINVAL, "%s", why);
}

/* ---------------------------------------------------------------------- */
/* argument decoding — every one of these treats its input as hostile      */
/* ---------------------------------------------------------------------- */

/* An empty object, used when the model sends no input at all, so the failure
 * the model sees is the precise "missing argument" message rather than a vague
 * parse error. Static storage: the span must outlive this function. */
static const char empty_object[] = "{}";

/* Validate the raw input span as a JSON object. 0 on success. */
static int parse_input(const tool_call_t *c, json_value_t *out,
                       char *err, size_t errcap) {
    if (!c || !c->input || c->input_len == 0) {
        out->type  = JSON_OBJECT;
        out->start = empty_object;
        out->len   = 2;
        return 0;
    }
    if (json_parse(c->input, c->input_len, out) != JSON_OK) {
        snprintf(err, errcap, "tool input is not valid JSON");
        return -1;
    }
    if (out->type != JSON_OBJECT) {
        snprintf(err, errcap, "tool input must be a JSON object");
        return -1;
    }
    return 0;
}

/* True for a byte that must never appear in a path we hand to the VFS. */
static int is_ctrl(unsigned char c) { return c < 0x20 || c == 0x7f; }

/* Decode, validate and canonicalise a path argument.
 *
 * Canonical form: absolute, single '/' separators, no trailing '/' (except the
 * root itself), no "." components, no ".." at all. Returns 0 on success; on
 * failure writes a model-readable reason into `err`. */
static int arg_path(const json_value_t *in, const char *key,
                    char *out, size_t cap, char *err, size_t errcap) {
    json_value_t v;
    char raw[VFST_PATH_MAX];
    size_t rawlen = 0;

    int rc = json_get(in, key, &v);
    if (rc == JSON_ENOENT) {
        snprintf(err, errcap, "missing required argument \"%s\" (an absolute "
                              "path string such as \"/etc/motd\")", key);
        return -1;
    }
    if (rc != JSON_OK || v.type != JSON_STRING) {
        snprintf(err, errcap, "argument \"%s\" must be a string", key);
        return -1;
    }

    rc = json_str(&v, raw, sizeof raw, &rawlen);
    if (rc == JSON_ENOSPC) {
        snprintf(err, errcap, "argument \"%s\" is too long (limit %d bytes)",
                 key, (int)(VFST_PATH_MAX - 1));
        return -1;
    }
    if (rc != JSON_OK) {
        snprintf(err, errcap, "argument \"%s\" is not a decodable JSON string", key);
        return -1;
    }
    if (rawlen == 0) {
        snprintf(err, errcap, "argument \"%s\" must not be empty", key);
        return -1;
    }
    if (strlen(raw) != rawlen) {
        snprintf(err, errcap, "argument \"%s\" contains an embedded NUL byte", key);
        return -1;
    }
    for (size_t i = 0; i < rawlen; i++) {
        if (is_ctrl((unsigned char)raw[i])) {
            snprintf(err, errcap,
                     "argument \"%s\" contains a control character at offset %u",
                     key, (unsigned)i);
            return -1;
        }
    }
    if (raw[0] != '/') {
        snprintf(err, errcap,
                 "argument \"%s\" must be an absolute path starting with '/'", key);
        return -1;
    }

    /* Fold the path into canonical form one component at a time. */
    size_t o = 0;
    size_t i = 0;
    while (i < rawlen) {
        while (i < rawlen && raw[i] == '/') i++;
        size_t start = i;
        while (i < rawlen && raw[i] != '/') i++;
        size_t clen = i - start;
        if (clen == 0) continue;                          /* trailing slashes */
        if (clen == 1 && raw[start] == '.') continue;     /* "." is a no-op   */
        if (clen == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            snprintf(err, errcap,
                     "argument \"%s\" must not contain \"..\"; use a full "
                     "absolute path instead", key);
            return -1;
        }
        if (clen > VFS_NAME_MAX) {
            snprintf(err, errcap,
                     "argument \"%s\" has a path component longer than %d bytes",
                     key, (int)VFS_NAME_MAX);
            return -1;
        }
        if (o + 1 + clen + 1 > cap) {                     /* cannot happen     */
            snprintf(err, errcap, "argument \"%s\" is too long", key);
            return -1;
        }
        out[o++] = '/';
        memcpy(out + o, raw + start, clen);
        o += clen;
    }
    if (o == 0) { out[o++] = '/'; }                       /* the root itself   */
    out[o] = '\0';
    return 0;
}

/* Optional integer argument, range-checked. Absent => `dflt`. */
static int arg_int(const json_value_t *in, const char *key, int64_t dflt,
                   int64_t lo, int64_t hi, int64_t *out,
                   char *err, size_t errcap) {
    json_value_t v;
    int rc = json_get(in, key, &v);
    if (rc == JSON_ENOENT) { *out = dflt; return 0; }
    if (rc != JSON_OK || v.type != JSON_NUMBER) {
        snprintf(err, errcap, "argument \"%s\" must be an integer", key);
        return -1;
    }
    int64_t n;
    if (json_int(&v, &n) != JSON_OK) {
        snprintf(err, errcap,
                 "argument \"%s\" must be a plain integer (no exponent, "
                 "no overflow)", key);
        return -1;
    }
    if (n < lo || n > hi) {
        snprintf(err, errcap, "argument \"%s\" must be between %ld and %ld",
                 key, (long)lo, (long)hi);
        return -1;
    }
    *out = n;
    return 0;
}

/* Optional boolean argument. Absent => `dflt`. */
static int arg_bool(const json_value_t *in, const char *key, int dflt, int *out,
                    char *err, size_t errcap) {
    json_value_t v;
    int rc = json_get(in, key, &v);
    if (rc == JSON_ENOENT) { *out = dflt; return 0; }
    if (rc != JSON_OK || v.type != JSON_BOOL) {
        snprintf(err, errcap, "argument \"%s\" must be true or false", key);
        return -1;
    }
    return json_bool(&v, out) == JSON_OK ? 0
         : (snprintf(err, errcap, "argument \"%s\" must be true or false", key), -1);
}

/* Optional string-enum argument; yields the index into `opts`. */
static int arg_enum(const json_value_t *in, const char *key,
                    const char *const *opts, int nopts, int dflt, int *out,
                    char *err, size_t errcap) {
    json_value_t v;
    int rc = json_get(in, key, &v);
    if (rc == JSON_ENOENT) { *out = dflt; return 0; }
    if (rc == JSON_OK && v.type == JSON_STRING) {
        for (int i = 0; i < nopts; i++)
            if (json_str_eq(&v, opts[i])) { *out = i; return 0; }
    }
    /* Name the legal values: the whole point is that the model can retry. */
    {
        char list[96];
        size_t o = 0;
        list[0] = '\0';
        for (int i = 0; i < nopts && o + 1 < sizeof list; i++) {
            int n = snprintf(list + o, sizeof list - o, "%s\"%s\"",
                             i ? ", " : "", opts[i]);
            /* snprintf returns the length it WOULD have written, so `o` can
             * overshoot the buffer on the last option that does not fit. The
             * loop guard above already stops it being used as an offset again,
             * but the bound belongs next to the arithmetic rather than one line
             * up: keep it local so the next edit cannot separate them. */
            if (n < 0) break;
            o += (size_t)n;
            if (o >= sizeof list) break;
        }
        snprintf(err, errcap, "argument \"%s\" must be one of %s", key, list);
    }
    return -1;
}

/* ---------------------------------------------------------------------- */
/* small helpers                                                           */
/* ---------------------------------------------------------------------- */

static const char *type_name(vnode_type_t t) {
    return t == VNODE_DIR ? "dir" : "file";
}

/* Split a canonical path into its parent directory and final component.
 * Returns 0, or -1 for the root (which has no parent). */
static int split_parent(const char *canon, char *dir, size_t dcap,
                        char *leaf, size_t lcap) {
    const char *slash = NULL;
    for (const char *p = canon; *p; p++) if (*p == '/') slash = p;
    if (!slash || slash[1] == '\0') return -1;            /* "/" or malformed */

    size_t llen = strlen(slash + 1);
    if (llen + 1 > lcap) return -1;
    memcpy(leaf, slash + 1, llen + 1);

    size_t dlen = (size_t)(slash - canon);
    if (dlen == 0) {                                      /* parent is root   */
        if (dcap < 2) return -1;
        dir[0] = '/'; dir[1] = '\0';
        return 0;
    }
    if (dlen + 1 > dcap) return -1;
    memcpy(dir, canon, dlen);
    dir[dlen] = '\0';
    return 0;
}

/* Number of entries in a directory, saturating at VFST_LIST_MAX. */
static uint32_t dir_entries(const char *path) {
    char name[VFS_NAME_MAX + 1];
    uint32_t n = 0;
    while (n < VFST_LIST_MAX && vfs_readdir(path, n, name, sizeof name) == VFS_OK)
        n++;
    return n;
}

/* Append raw file bytes to the result as text.
 *
 * The result is delivered to the model as a string, so a NUL would silently cut
 * it short and the other C0 controls are noise. Both are rendered as '.', and
 * the count is reported to the model rather than hidden. Bytes >= 0x80 pass
 * through: the content is assumed to be UTF-8, which json.h escapes correctly.
 * Returns the number of bytes consumed (< n means the buffer filled up). */
static size_t emit_bytes(tool_result_t *r, const uint8_t *src, size_t n,
                         size_t *replaced) {
    size_t w = 0;
    if (!r->buf || r->cap == 0) return 0;
    while (w < n && r->len + 1 < r->cap) {
        uint8_t c = src[w];
        if (c != '\n' && c != '\t' && is_ctrl(c)) { c = '.'; (*replaced)++; }
        r->buf[r->len++] = (char)c;
        w++;
    }
    r->buf[r->len] = '\0';
    if (w < n) r->truncated = 1;
    return w;
}

/* ====================================================================== */
/* read_file                                                              */
/* ====================================================================== */

static int t_read_file(const tool_call_t *call, tool_result_t *r) {
    json_value_t in;
    char err[VFST_ERR_MAX];
    char path[VFST_PATH_MAX];
    int64_t offset, want;

    if (parse_input(call, &in, err, sizeof err) != 0)
        return fail_args(r, "vfs_read", err);
    if (arg_path(&in, "path", path, sizeof path, err, sizeof err) != 0)
        return fail_args(r, "vfs_read", err);
    if (arg_int(&in, "offset", 0, 0, (int64_t)1 << 40, &offset, err, sizeof err) != 0)
        return fail_args(r, "vfs_read", err);
    if (arg_int(&in, "max_bytes", 0, 0, (int64_t)1 << 40, &want, err, sizeof err) != 0)
        return fail_args(r, "vfs_read", err);

    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK)
        return fail(r, "vfs_read", path, VFS_ENOENT,
                    "no such file: %s. Use list_dir on its parent directory to "
                    "see what is actually there.", path);
    if (st.type == VNODE_DIR)
        return fail(r, "vfs_read", path, VFS_EINVAL,
                    "%s is a directory, not a file. Use list_dir instead.", path);

    /* How much content can share the result buffer with the header. */
    size_t budget = (r->cap > VFST_HDR_RESERVE + 1)
                  ? r->cap - VFST_HDR_RESERVE - 1 : 0;
    if (budget == 0)
        return fail(r, "vfs_read", path, VFS_ENOSPC,
                    "the result buffer is too small to return any file content");
    if (want == 0 || (uint64_t)want > budget) want = (int64_t)budget;

    uint64_t remaining = ((uint64_t)offset < st.size) ? st.size - (uint64_t)offset : 0;
    if ((uint64_t)want > remaining) want = (int64_t)remaining;

    if (remaining == 0) {
        tool_result_printf(r, "%s: file is %lu bytes; offset %lu is at or past "
                              "the end, so 0 bytes were read.",
                           path, (unsigned long)st.size, (unsigned long)offset);
        trace_ret(0, "vfs_read", "%s off=%lu", path, (unsigned long)offset);
        return TOOL_OK;
    }

    uint8_t *tmp = want > 0 ? kmalloc((size_t)want) : NULL;
    if (want > 0 && !tmp)
        return fail(r, "vfs_read", path, VFS_ENOSPC,
                    "out of kernel memory reading %s; retry with a smaller "
                    "max_bytes.", path);

    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) {
        if (tmp) kfree(tmp);
        return fail(r, "vfs_read", path, VFS_ENOENT, "could not open %s", path);
    }
    vfs_seek(f, offset, SEEK_SET);
    int64_t got = want > 0 ? vfs_read(f, tmp, (uint64_t)want) : 0;
    vfs_close(f);
    if (got < 0) got = 0;

    uint64_t after = (uint64_t)offset + (uint64_t)got;
    int clipped = after < st.size;

    if (clipped)
        tool_result_printf(r,
            "%s: %lu bytes total; showing bytes %lu-%lu. TRUNCATED - %lu bytes "
            "remain, call read_file again with offset=%lu.\n---\n",
            path, (unsigned long)st.size, (unsigned long)offset,
            (unsigned long)(after ? after - 1 : 0),
            (unsigned long)(st.size - after), (unsigned long)after);
    else
        tool_result_printf(r, "%s: %lu bytes total; showing bytes %lu-%lu (to "
                              "end of file).\n---\n",
                           path, (unsigned long)st.size, (unsigned long)offset,
                           (unsigned long)(after ? after - 1 : 0));

    size_t replaced = 0;
    size_t shown = emit_bytes(r, tmp, (size_t)got, &replaced);
    if (replaced)
        tool_result_printf(r, "\n(%lu non-printable byte(s) shown as '.')",
                           (unsigned long)replaced);
    if (shown < (size_t)got)
        tool_result_printf(r, "\n(result buffer full after %lu byte(s); "
                              "call read_file again with offset=%lu)",
                           (unsigned long)shown,
                           (unsigned long)((uint64_t)offset + shown));

    if (tmp) kfree(tmp);
    trace_ret((long)shown, "vfs_read", "%s off=%lu", path, (unsigned long)offset);
    return TOOL_OK;
}

static const tool_t read_file_def = {
    .name        = "read_file",
    .description =
        "Read the contents of a file. Large files are returned in pages: the "
        "result always states the file's total size and, if the whole file did "
        "not fit, the exact offset to pass on the next call. Non-printable "
        "bytes are shown as '.'.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":"
            "\"Absolute path of the file, e.g. \\\"/etc/motd\\\".\"},"
          "\"offset\":{\"type\":\"integer\",\"description\":"
            "\"Byte offset to start reading from. Defaults to 0.\"},"
          "\"max_bytes\":{\"type\":\"integer\",\"description\":"
            "\"Maximum number of bytes to return. Defaults to as much as fits "
            "in one result (about 3700 bytes).\"}},"
        "\"required\":[\"path\"]}",
    .flags  = 0,
    .invoke = t_read_file,
};
REGISTER_VFS_TOOL(read_file_def);

/* ====================================================================== */
/* write_file                                                             */
/* ====================================================================== */

static const char *const write_modes[] = { "overwrite", "append", "create_new" };

static int t_write_file(const tool_call_t *call, tool_result_t *r) {
    json_value_t in, cv;
    char err[VFST_ERR_MAX];
    char path[VFST_PATH_MAX];
    int mode;

    if (parse_input(call, &in, err, sizeof err) != 0)
        return fail_args(r, "vfs_write", err);
    if (arg_path(&in, "path", path, sizeof path, err, sizeof err) != 0)
        return fail_args(r, "vfs_write", err);
    if (arg_enum(&in, "mode", write_modes, 3, 0, &mode, err, sizeof err) != 0)
        return fail_args(r, "vfs_write", err);

    /* ---- content: decoded into a heap buffer bounded by VFST_WRITE_MAX ---- */
    int rc = json_get(&in, "content", &cv);
    if (rc == JSON_ENOENT)
        return fail(r, "vfs_write", path, TOOL_EINVAL,
                    "missing required argument \"content\" (a string; use \"\" "
                    "to create an empty file)");
    if (rc != JSON_OK || cv.type != JSON_STRING)
        return fail(r, "vfs_write", path, TOOL_EINVAL,
                    "argument \"content\" must be a string");

    /* A decoded JSON string is never longer than its raw span, so this cap is
     * both sufficient and bounded by VFST_WRITE_MAX no matter what arrives. */
    size_t cap = cv.len + 1;
    if (cap > VFST_WRITE_MAX + 1) cap = VFST_WRITE_MAX + 1;

    char *data = kmalloc(cap);
    if (!data)
        return fail(r, "vfs_write", path, VFS_ENOSPC,
                    "out of kernel memory staging the write to %s", path);

    size_t dlen = 0;
    rc = json_str(&cv, data, cap, &dlen);
    if (rc == JSON_ENOSPC) {
        kfree(data);
        return fail(r, "vfs_write", path, TOOL_EINVAL,
                    "argument \"content\" is larger than the %d byte write "
                    "limit; write it in several append calls instead",
                    (int)VFST_WRITE_MAX);
    }
    if (rc != JSON_OK) {
        kfree(data);
        return fail(r, "vfs_write", path, TOOL_EINVAL,
                    "argument \"content\" is not a decodable JSON string");
    }

    /* ---- pre-flight: refuse the cases the VFS would fail obscurely ---- */
    vfs_stat_t st;
    int exists = (vfs_stat(path, &st) == VFS_OK);
    if (exists && st.type == VNODE_DIR) {
        kfree(data);
        return fail(r, "vfs_write", path, VFS_EINVAL,
                    "%s is a directory; refusing to write over it", path);
    }
    if (exists && mode == 2) {
        kfree(data);
        return fail(r, "vfs_write", path, VFS_EEXIST,
                    "%s already exists (%lu bytes) and mode is \"create_new\"; "
                    "use mode \"overwrite\" or \"append\" to change it",
                    path, (unsigned long)st.size);
    }
    if (!exists) {
        /* Distinguish "bad parent" from "bad name" before the VFS returns a
         * bare NULL, so the model knows which one to fix. */
        char dir[VFST_PATH_MAX], leaf[VFS_NAME_MAX + 1];
        if (split_parent(path, dir, sizeof dir, leaf, sizeof leaf) != 0) {
            kfree(data);
            return fail(r, "vfs_write", path, VFS_EINVAL,
                        "%s is not a valid file path", path);
        }
        vfs_stat_t pst;
        if (vfs_stat(dir, &pst) != VFS_OK) {
            kfree(data);
            return fail(r, "vfs_write", path, VFS_ENOENT,
                        "parent directory %s does not exist; create it with "
                        "make_dir (parents=true) first", dir);
        }
        if (pst.type != VNODE_DIR) {
            kfree(data);
            return fail(r, "vfs_write", path, VFS_ENOTDIR,
                        "%s is a file, not a directory, so %s cannot be "
                        "created inside it", dir, path);
        }
    }

    int flags = O_CREAT | O_RDWR | (mode == 1 ? O_APPEND : mode == 0 ? O_TRUNC : 0);
    file_t *f = vfs_open(path, flags);
    if (!f) {
        kfree(data);
        return fail(r, "vfs_write", path, VFS_EINVAL, "could not open %s for "
                    "writing", path);
    }

    int64_t n = dlen ? vfs_write(f, data, dlen) : 0;
    vfs_close(f);
    kfree(data);

    if (n < 0)
        return fail(r, "vfs_write", path, (int)n,
                    "the filesystem refused the write to %s (error %d)",
                    path, (int)n);

    vfs_stat_t after;
    uint64_t now = (vfs_stat(path, &after) == VFS_OK) ? after.size : 0;

    if (mode == 1)
        tool_result_printf(r, "appended %lu bytes to %s; the file is now %lu "
                              "bytes.", (unsigned long)n, path, (unsigned long)now);
    else if (exists)
        tool_result_printf(r, "wrote %lu bytes to %s, replacing the previous "
                              "%lu bytes.", (unsigned long)n, path,
                           (unsigned long)st.size);
    else
        tool_result_printf(r, "created %s and wrote %lu bytes.", path,
                           (unsigned long)n);

    if ((uint64_t)n != dlen)
        tool_result_printf(r, " WARNING: only %lu of %lu bytes were stored.",
                           (unsigned long)n, (unsigned long)dlen);

    trace_ok("vfs_write", "%s mode=%s bytes=%lu", path, write_modes[mode],
             (unsigned long)n);
    return TOOL_OK;
}

static const tool_t write_file_def = {
    .name        = "write_file",
    .description =
        "Create a file or change its contents. mode=\"overwrite\" (the default) "
        "replaces the whole file, \"append\" adds to the end, and "
        "\"create_new\" fails if the path already exists - use it when you must "
        "not clobber existing data. The parent directory must already exist. "
        "The result reports how many bytes were stored and the file's new size.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":"
            "\"Absolute path of the file to write.\"},"
          "\"content\":{\"type\":\"string\",\"description\":"
            "\"Bytes to store, at most 65536 after decoding.\"},"
          "\"mode\":{\"type\":\"string\","
            "\"enum\":[\"overwrite\",\"append\",\"create_new\"],"
            "\"description\":\"How to combine content with any existing file. "
            "Defaults to \\\"overwrite\\\".\"}},"
        "\"required\":[\"path\",\"content\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_write_file,
};
REGISTER_VFS_TOOL(write_file_def);

/* ====================================================================== */
/* list_dir                                                               */
/* ====================================================================== */

static int t_list_dir(const tool_call_t *call, tool_result_t *r) {
    json_value_t in;
    char err[VFST_ERR_MAX];
    char path[VFST_PATH_MAX];

    if (parse_input(call, &in, err, sizeof err) != 0)
        return fail_args(r, "vfs_readdir", err);
    if (arg_path(&in, "path", path, sizeof path, err, sizeof err) != 0)
        return fail_args(r, "vfs_readdir", err);

    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK)
        return fail(r, "vfs_readdir", path, VFS_ENOENT,
                    "no such directory: %s", path);
    if (st.type != VNODE_DIR)
        return fail(r, "vfs_readdir", path, VFS_ENOTDIR,
                    "%s is a file, not a directory. Use read_file or stat_path.",
                    path);

    int root = (path[0] == '/' && path[1] == '\0');
    tool_result_printf(r, "%s:", path);

    char name[VFS_NAME_MAX + 1];
    uint32_t i = 0, shown = 0;
    int clipped = 0;

    for (; i < VFST_LIST_MAX; i++) {
        if (vfs_readdir(path, i, name, sizeof name) != VFS_OK) break;

        /* Leave room for the closing note. */
        if (r->len + 160 >= r->cap) { clipped = 1; break; }

        char child[VFST_PATH_MAX];
        vfs_stat_t cst;
        int have = 0;
        int n = snprintf(child, sizeof child, "%s%s%s", root ? "" : path,
                         "/", name);
        if (n > 0 && (size_t)n < sizeof child)
            have = (vfs_stat(child, &cst) == VFS_OK);

        if (!have)
            tool_result_printf(r, "\n  %s  ?", name);
        else if (cst.type == VNODE_DIR) {
            uint32_t kids = dir_entries(child);
            tool_result_printf(r, "\n  %s/  dir  (%lu %s)", name,
                               (unsigned long)kids,
                               kids == 1 ? "entry" : "entries");
        }
        else
            tool_result_printf(r, "\n  %s  file  %lu bytes", name,
                               (unsigned long)cst.size);
        shown++;
    }

    if (shown == 0 && !clipped)
        tool_result_printf(r, " (empty directory)");
    else if (clipped || i >= VFST_LIST_MAX)
        tool_result_printf(r, "\n  ... more entries exist but did not fit in "
                              "one result (%lu shown)", (unsigned long)shown);

    trace_ret((long)shown, "vfs_readdir", "%s", path);
    return TOOL_OK;
}

static const tool_t list_dir_def = {
    .name        = "list_dir",
    .description =
        "List the contents of a directory. Each entry is reported with its type "
        "and, for files, its size in bytes. If the listing does not fit in one "
        "result it says so rather than silently stopping short.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":"
            "\"Absolute path of the directory, e.g. \\\"/\\\" or \\\"/etc\\\".\"}},"
        "\"required\":[\"path\"]}",
    .flags  = 0,
    .invoke = t_list_dir,
};
REGISTER_VFS_TOOL(list_dir_def);

/* ====================================================================== */
/* stat_path                                                              */
/* ====================================================================== */

static int t_stat_path(const tool_call_t *call, tool_result_t *r) {
    json_value_t in;
    char err[VFST_ERR_MAX];
    char path[VFST_PATH_MAX];

    if (parse_input(call, &in, err, sizeof err) != 0)
        return fail_args(r, "vfs_stat", err);
    if (arg_path(&in, "path", path, sizeof path, err, sizeof err) != 0)
        return fail_args(r, "vfs_stat", err);

    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK)
        return fail(r, "vfs_stat", path, VFS_ENOENT,
                    "%s does not exist", path);

    if (st.type == VNODE_DIR)
        tool_result_printf(r, "%s: type=dir entries=%lu mode=0x%lx mtime_ms=%lu",
                           path, (unsigned long)dir_entries(path),
                           (unsigned long)st.mode, (unsigned long)st.mtime_ms);
    else
        tool_result_printf(r, "%s: type=file size=%lu mode=0x%lx mtime_ms=%lu",
                           path, (unsigned long)st.size,
                           (unsigned long)st.mode, (unsigned long)st.mtime_ms);

    trace_ok("vfs_stat", "%s type=%s size=%lu", path, type_name(st.type),
             (unsigned long)st.size);
    return TOOL_OK;
}

static const tool_t stat_path_def = {
    .name        = "stat_path",
    .description =
        "Report metadata for one path: whether it exists, whether it is a file "
        "or a directory, its size (or entry count), permission bits and "
        "modification time. The cheap way to check before writing or deleting.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":"
            "\"Absolute path to inspect.\"}},"
        "\"required\":[\"path\"]}",
    .flags  = 0,
    .invoke = t_stat_path,
};
REGISTER_VFS_TOOL(stat_path_def);

/* ====================================================================== */
/* make_dir                                                               */
/* ====================================================================== */

static int t_make_dir(const tool_call_t *call, tool_result_t *r) {
    json_value_t in;
    char err[VFST_ERR_MAX];
    char path[VFST_PATH_MAX];
    int parents;

    if (parse_input(call, &in, err, sizeof err) != 0)
        return fail_args(r, "vfs_mkdir", err);
    if (arg_path(&in, "path", path, sizeof path, err, sizeof err) != 0)
        return fail_args(r, "vfs_mkdir", err);
    if (arg_bool(&in, "parents", 0, &parents, err, sizeof err) != 0)
        return fail_args(r, "vfs_mkdir", err);

    if (path[1] == '\0')
        return fail(r, "vfs_mkdir", path, VFS_EEXIST,
                    "/ is the filesystem root and already exists");

    vfs_stat_t st;
    if (vfs_stat(path, &st) == VFS_OK) {
        /* Idempotent for directories: re-creating one is not a mistake worth
         * failing a whole turn over. Colliding with a file is. */
        if (st.type == VNODE_DIR) {
            tool_result_printf(r, "%s already exists and is a directory; "
                                  "nothing to do.", path);
            trace_ok("vfs_mkdir", "%s exists=1", path);
            return TOOL_OK;
        }
        return fail(r, "vfs_mkdir", path, VFS_EEXIST,
                    "%s already exists and is a file, so it cannot be made "
                    "into a directory", path);
    }

    uint32_t created = 0;
    if (parents) {
        /* Walk the prefixes, creating each missing one. */
        char partial[VFST_PATH_MAX];
        size_t i = 1;                       /* skip the leading '/' */
        size_t len = strlen(path);
        while (i <= len) {
            if (i == len || path[i] == '/') {
                memcpy(partial, path, i);
                partial[i] = '\0';
                if (vfs_stat(partial, &st) != VFS_OK) {
                    int rc = vfs_mkdir(partial);
                    if (rc != VFS_OK)
                        return fail(r, "vfs_mkdir", partial, rc,
                                    "could not create %s (error %d)", partial, rc);
                    created++;
                } else if (st.type != VNODE_DIR) {
                    return fail(r, "vfs_mkdir", partial, VFS_ENOTDIR,
                                "%s exists and is a file, so %s cannot be "
                                "created under it", partial, path);
                }
            }
            i++;
        }
    } else {
        char dir[VFST_PATH_MAX], leaf[VFS_NAME_MAX + 1];
        if (split_parent(path, dir, sizeof dir, leaf, sizeof leaf) != 0)
            return fail(r, "vfs_mkdir", path, VFS_EINVAL,
                        "%s is not a valid directory path", path);
        if (vfs_stat(dir, &st) != VFS_OK)
            return fail(r, "vfs_mkdir", path, VFS_ENOENT,
                        "parent directory %s does not exist; pass "
                        "parents=true to create the whole chain", dir);
        if (st.type != VNODE_DIR)
            return fail(r, "vfs_mkdir", path, VFS_ENOTDIR,
                        "%s is a file, not a directory", dir);
        int rc = vfs_mkdir(path);
        if (rc != VFS_OK)
            return fail(r, "vfs_mkdir", path, rc,
                        "could not create %s (error %d)", path, rc);
        created = 1;
    }

    tool_result_printf(r, "created directory %s (%lu directory/directories "
                          "made).", path, (unsigned long)created);
    trace_ok("vfs_mkdir", "%s made=%lu", path, (unsigned long)created);
    return TOOL_OK;
}

static const tool_t make_dir_def = {
    .name        = "make_dir",
    .description =
        "Create a directory. Pass parents=true to create every missing parent "
        "as well (like mkdir -p). Creating a directory that already exists is "
        "reported as success; colliding with an existing file is an error.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":"
            "\"Absolute path of the directory to create.\"},"
          "\"parents\":{\"type\":\"boolean\",\"description\":"
            "\"Create missing parent directories too. Defaults to false.\"}},"
        "\"required\":[\"path\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_make_dir,
};
REGISTER_VFS_TOOL(make_dir_def);

/* ====================================================================== */
/* delete_path                                                            */
/* ====================================================================== */

/* The VFS exposes unlink only through vnode_ops_t (there is no vfs_unlink yet),
 * so reach it the sanctioned way: open the parent directory to obtain its
 * vnode and call the filesystem's unlink through the public ops table. */
static int unlink_at(const char *dir, const char *leaf) {
    file_t *d = vfs_open(dir, O_RDONLY);
    if (!d) return VFS_ENOENT;
    int rc = VFS_EINVAL;
    if (d->node && d->node->type == VNODE_DIR && d->node->ops &&
        d->node->ops->unlink)
        rc = d->node->ops->unlink(d->node, leaf);
    vfs_close(d);
    return rc;
}

static int t_delete_path(const tool_call_t *call, tool_result_t *r) {
    json_value_t in;
    char err[VFST_ERR_MAX];
    char path[VFST_PATH_MAX];

    if (parse_input(call, &in, err, sizeof err) != 0)
        return fail_args(r, "vfs_unlink", err);
    if (arg_path(&in, "path", path, sizeof path, err, sizeof err) != 0)
        return fail_args(r, "vfs_unlink", err);

    if (path[1] == '\0')
        return fail(r, "vfs_unlink", path, VFS_EINVAL,
                    "refusing to delete the filesystem root");

    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK)
        return fail(r, "vfs_unlink", path, VFS_ENOENT,
                    "%s does not exist, so nothing was deleted", path);

    if (st.type == VNODE_DIR) {
        uint32_t n = dir_entries(path);
        if (n)
            return fail(r, "vfs_unlink", path, VFS_EINVAL,
                        "%s is not empty (%lu entries); delete its contents "
                        "first - there is no recursive delete", path,
                        (unsigned long)n);
    }

    char dir[VFST_PATH_MAX], leaf[VFS_NAME_MAX + 1];
    if (split_parent(path, dir, sizeof dir, leaf, sizeof leaf) != 0)
        return fail(r, "vfs_unlink", path, VFS_EINVAL,
                    "%s is not a deletable path", path);

    uint64_t     size = st.size;
    vnode_type_t type = st.type;

    int rc = unlink_at(dir, leaf);
    if (rc != VFS_OK)
        return fail(r, "vfs_unlink", path, rc,
                    "the filesystem refused to delete %s (error %d)", path, rc);

    if (type == VNODE_DIR)
        tool_result_printf(r, "deleted empty directory %s. This cannot be "
                              "undone.", path);
    else
        tool_result_printf(r, "deleted file %s (%lu bytes). This cannot be "
                              "undone.", path, (unsigned long)size);

    trace_ok("vfs_unlink", "%s type=%s size=%lu", path, type_name(type),
             (unsigned long)size);
    return TOOL_OK;
}

static const tool_t delete_path_def = {
    .name        = "delete_path",
    .description =
        "Delete a file, or an empty directory. There is no recursive delete and "
        "no undo: a non-empty directory is refused with its entry count so you "
        "can decide deliberately. The filesystem root cannot be deleted.",
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":"
            "\"Absolute path of the file or empty directory to remove.\"}},"
        "\"required\":[\"path\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_delete_path,
};
REGISTER_VFS_TOOL(delete_path_def);

/* ======================================================================
 * DEFERRED
 *   - rename/move and copy: ramfs has no rename op, so both would have to be
 *     read+write+unlink here. That belongs behind a vfs_rename() in the VFS,
 *     not open-coded in a tool.
 *   - recursive delete: destructive and unrecoverable on a machine with no
 *     undo; the model can loop delete_path deliberately instead.
 *   - writing at an explicit offset (patching in place): the three modes cover
 *     what a model actually needs, and an offset write invites off-by-one
 *     corruption of files the model cannot see whole.
 *   - a hex/base64 read encoding for binary files: today non-printable bytes
 *     are rendered as '.' and counted, which is enough to *diagnose* a binary
 *     file, not to round-trip one.
 * ====================================================================== */
