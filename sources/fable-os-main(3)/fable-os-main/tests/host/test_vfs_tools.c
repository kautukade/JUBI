/* test_vfs_tools.c — the filesystem tool family, hammered on the host.
 *
 * What this proves, in order of how much it matters:
 *
 *   1. REGISTRATION. tool_count()/tool_find()/tools_build_schema() really do
 *      see the six tools, and the schema array the kernel would send to the
 *      API parses back through json_parse().
 *   2. REAL EFFECT. Dispatching write_file puts real bytes in the real ramfs:
 *      the test reads them back through vfs_open/vfs_read, not through the
 *      tool. Claiming vs. doing.
 *   3. GROUND TRUTH. Every dispatch emits exactly one kernel trace line, with
 *      the real path and the real byte count, captured via kcap_text().
 *   4. HOSTILITY. The input span is model output. Every tool is fed 10 KB
 *      paths, negative offsets, wrong types, embedded NULs, "..", truncated
 *      JSON and random bytes, from a buffer whose next page is PROT_NONE so a
 *      one-byte over-read is a SIGSEGV, into a canary-guarded result buffer.
 *
 * The Mach-O shim below is the one concession to the host: ld64 has no
 * __start_/__stop_ section symbols, so they are aliased to the equivalent
 * linker-generated ones. The kernel's ELF path is untouched.
 */

#include "harness.h"

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "vfs.h"
#include "fs.h"
#include "heap.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

/* The boundary symbols, from the one place that owns that shim. The table
 * ENTRIES for this suite are emitted by tools/vfs_tools.c, which is compiled as
 * its own translation unit and so carries its own copy of the Mach-O section
 * spelling — see REGISTER_VFS_TOOL there. */
#include "tooltable.h"

/* ---------------------------------------------------------------------- */
/* plumbing                                                                */
/* ---------------------------------------------------------------------- */

#define RCAP        TOOL_RESULT_MAX
#define GUARD       64
#define GUARD_BYTE  0xA5

/* Mirrors VFST_PATH_MAX in tools/vfs_tools.c: the first length that must be
 * rejected outright rather than silently truncated. */
#define PATH_TOO_LONG_AT 128

static unsigned char rarena[GUARD + RCAP + GUARD];
static char         *rbuf = (char *)rarena + GUARD;

static void guards_arm(void) {
    memset(rarena, GUARD_BYTE, GUARD);
    memset(rarena + GUARD + RCAP, GUARD_BYTE, GUARD);
}

static void guards_check(void) {
    int lo = 1, hi = 1;
    for (int i = 0; i < GUARD; i++) {
        if (rarena[i] != GUARD_BYTE) lo = 0;
        if (rarena[GUARD + RCAP + i] != GUARD_BYTE) hi = 0;
    }
    CHECK(lo);
    CHECK(hi);
}

/* Place `n` bytes so that byte n is the first byte of a PROT_NONE page: any
 * read past the end of the span faults instead of quietly succeeding. */
typedef struct { char *base; char *span; size_t pages; } trap_t;

static trap_t trap_make(const char *src, size_t n) {
    trap_t t;
    size_t pg = (size_t)getpagesize();
    size_t need = ((n + pg - 1) / pg) * pg;
    if (need == 0) need = pg;
    t.pages = need + pg;
    t.base  = mmap(NULL, t.pages, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (t.base == MAP_FAILED) { perror("mmap"); exit(2); }
    mprotect(t.base + need, pg, PROT_NONE);
    t.span = t.base + need - n;
    if (n) memcpy(t.span, src, n);
    return t;
}

static void trap_free(trap_t *t) { munmap(t->base, t->pages); }

/* Dispatch one tool call with a trapped input span and a guarded result. */
static int dispatch(const char *name, const char *input, size_t inlen,
                    tool_result_t *out) {
    trap_t t = trap_make(input, inlen);
    tool_call_t c = { .id = "toolu_test", .name = name,
                      .input = t.span, .input_len = inlen };
    guards_arm();
    tool_result_init(out, rbuf, RCAP);
    int rc = tool_dispatch(&c, out);
    guards_check();
    /* The result must always be a NUL-terminated string inside the buffer. */
    CHECK(out->len < RCAP);
    CHECK(rbuf[out->len] == '\0');
    CHECK(strlen(rbuf) == out->len);
    trap_free(&t);
    return rc;
}

static int call_json(const char *name, const char *input, tool_result_t *out) {
    return dispatch(name, input, strlen(input), out);
}

/* Read a whole file straight from the VFS — deliberately NOT via a tool. */
static long vfs_slurp(const char *path, char *dst, size_t cap) {
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) return -1;
    int64_t n = vfs_read(f, dst, cap - 1);
    vfs_close(f);
    if (n < 0) return -1;
    dst[n] = '\0';
    return (long)n;
}

static const char *const TOOL_NAMES[] = {
    "read_file", "write_file", "list_dir", "stat_path", "make_dir",
    "delete_path"
};
#define NTOOLS ((int)(sizeof TOOL_NAMES / sizeof TOOL_NAMES[0]))

/* ====================================================================== */
/* 1. registration and schema                                             */
/* ====================================================================== */

static void test_tools_are_registered(void) {
    CHECK(tool_count() >= NTOOLS);
    for (int i = 0; i < NTOOLS; i++) {
        const tool_t *t = tool_find(TOOL_NAMES[i]);
        CHECK(t != NULL);
        if (!t) continue;
        CHECK_STR(t->name, TOOL_NAMES[i]);
        CHECK(t->description != NULL && strlen(t->description) > 20);
        CHECK(t->input_schema != NULL);
        CHECK(t->invoke != NULL);
        CHECK(strlen(t->name) < TOOL_NAME_MAX);
    }
    /* Unknown names must not resolve to something. */
    CHECK(tool_find("read_file_") == NULL);
    CHECK(tool_find("") == NULL);
    CHECK(tool_find(NULL) == NULL);
}

static void test_mutating_tools_are_flagged(void) {
    CHECK_TOOL_FLAGS("write_file", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("make_dir", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("delete_path", TOOL_MUTATES, TOOL_MUTATES);
    /* Read-only tools must not claim to mutate. */
    CHECK_TOOL_FLAGS("read_file", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("list_dir", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("stat_path", TOOL_MUTATES, 0);
}

/* The advertised contract is what the model sees; if it is not valid JSON the
 * whole request is malformed, so parse it back with the kernel's own reader. */
static void test_schema_is_valid_json(void) {
    static char buf[64 * 1024];
    size_t n = tools_build_schema(buf, sizeof buf);
    CHECK(n > 0);
    CHECK(n < sizeof buf);              /* no truncation */

    json_value_t root;
    CHECK_EQ(json_parse(buf, n, &root), JSON_OK);
    CHECK_EQ(root.type, JSON_ARRAY);
    CHECK_EQ((int)json_count(&root), tool_count());

    int seen = 0;
    for (size_t i = 0; i < json_count(&root); i++) {
        json_value_t el, v;
        CHECK_EQ(json_at(&root, i, &el), JSON_OK);
        CHECK_EQ(el.type, JSON_OBJECT);

        char name[TOOL_NAME_MAX];
        CHECK_EQ(json_get_str(&el, "name", name, sizeof name), JSON_OK);
        CHECK_EQ(json_get(&el, "description", &v), JSON_OK);
        CHECK_EQ(v.type, JSON_STRING);

        /* input_schema must itself be an object with type/properties. */
        CHECK_EQ(json_get(&el, "input_schema", &v), JSON_OK);
        CHECK_EQ(v.type, JSON_OBJECT);

        json_value_t props;
        for (int k = 0; k < NTOOLS; k++) {
            if (strcmp(name, TOOL_NAMES[k]) != 0) continue;
            seen++;
            CHECK(json_str_eq(&v, "") == 0);
            json_value_t ty;
            CHECK_EQ(json_get(&v, "type", &ty), JSON_OK);
            CHECK(json_str_eq(&ty, "object"));
            CHECK_EQ(json_get(&v, "properties", &props), JSON_OK);
            CHECK_EQ(props.type, JSON_OBJECT);
            json_value_t p;
            CHECK_EQ(json_get(&props, "path", &p), JSON_OK);
            json_value_t req;
            CHECK_EQ(json_get(&v, "required", &req), JSON_OK);
            CHECK_EQ(req.type, JSON_ARRAY);
        }
    }
    CHECK_EQ(seen, NTOOLS);
}

/* Truncating the schema must not run off the end of the destination, and the
 * caller must be able to TELL that it truncated. tools_build_schema() has
 * snprintf semantics: the return value is the size required, independent of the
 * capacity offered, so `ret >= cap` is the truncation test — which is exactly
 * how the request builder decides whether the tools array it is about to send
 * is whole. */
static void test_schema_truncation_is_bounded(void) {
    const size_t need = tools_build_schema(NULL, 0);   /* the sizing idiom */
    CHECK(need > 2);

    for (size_t cap = 1; cap <= 512; cap += 7) {
        char small[600];
        memset(small, GUARD_BYTE, sizeof small);
        size_t ret = tools_build_schema(small, cap);

        CHECK(ret == need);                           /* bytes NEEDED, always */
        CHECK(strlen(small) < cap);                   /* NUL-terminated inside */
        CHECK(strlen(small) == (need < cap ? need : cap - 1));
        CHECK((ret >= cap) == (strlen(small) < need));/* truncation detectable */
        for (size_t i = cap; i < sizeof small; i++)   /* nothing beyond cap */
            if (small[i] != (char)GUARD_BYTE) { CHECK(0); break; }
    }

    /* The whole array does fit somewhere, and then the idiom says "no truncation". */
    static char whole[64 * 1024];
    CHECK(tools_build_schema(whole, sizeof whole) < sizeof whole);
    CHECK(strlen(whole) == need);
}

/* ====================================================================== */
/* 2. real filesystem effect                                              */
/* ====================================================================== */

static void test_write_lands_real_bytes(void) {
    tool_result_t r;
    trace_reset();
    kcap_reset();

    call_json("write_file",
              "{\"path\":\"/tmp/proof.txt\",\"content\":\"hello kernel\\n\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "created /tmp/proof.txt");
    CHECK_CONTAINS(rbuf, "13 bytes");

    /* Independent verification straight through the VFS. */
    char back[128];
    long n = vfs_slurp("/tmp/proof.txt", back, sizeof back);
    CHECK_EQ(n, 13);
    CHECK_STR(back, "hello kernel\n");

    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/tmp/proof.txt", &st), VFS_OK);
    CHECK_EQ(st.type, VNODE_FILE);
    CHECK_EQ((long)st.size, 13);

    /* Exactly one kernel-emitted line, and it says what really happened. */
    CHECK_EQ(trace_count(), 1);
    CHECK_CONTAINS(kcap_text(),
                   "[vfs_write /tmp/proof.txt mode=overwrite bytes=13 -> ok]");
}

static void test_overwrite_and_append(void) {
    tool_result_t r;
    char back[256];

    call_json("write_file",
              "{\"path\":\"/tmp/proof.txt\",\"content\":\"abc\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "replacing the previous 13 bytes");
    CHECK_EQ(vfs_slurp("/tmp/proof.txt", back, sizeof back), 3);
    CHECK_STR(back, "abc");

    kcap_reset();
    call_json("write_file",
              "{\"path\":\"/tmp/proof.txt\",\"content\":\"def\",\"mode\":\"append\"}",
              &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(vfs_slurp("/tmp/proof.txt", back, sizeof back), 6);
    CHECK_STR(back, "abcdef");
    CHECK_CONTAINS(kcap_text(), "mode=append bytes=3 -> ok]");

    /* create_new must refuse to clobber. */
    call_json("write_file",
              "{\"path\":\"/tmp/proof.txt\",\"content\":\"zzz\","
              "\"mode\":\"create_new\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "already exists");
    CHECK_EQ(vfs_slurp("/tmp/proof.txt", back, sizeof back), 6);   /* untouched */

    /* Empty content truncates to zero, and is not an error. */
    call_json("write_file", "{\"path\":\"/tmp/proof.txt\",\"content\":\"\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(vfs_slurp("/tmp/proof.txt", back, sizeof back), 0);
}

/* JSON escapes, including an embedded NUL, must survive into the file. */
static void test_write_preserves_escaped_bytes(void) {
    tool_result_t r;
    call_json("write_file",
              "{\"path\":\"/tmp/esc.bin\",\"content\":\"a\\u0000b\\tc\\n\"}", &r);
    CHECK_EQ(r.is_error, 0);

    file_t *f = vfs_open("/tmp/esc.bin", O_RDONLY);
    CHECK(f != NULL);
    unsigned char raw[16];
    int64_t n = vfs_read(f, raw, sizeof raw);
    vfs_close(f);
    CHECK_EQ(n, 6);
    CHECK_EQ(raw[0], 'a');
    CHECK_EQ(raw[1], 0);
    CHECK_EQ(raw[2], 'b');
    CHECK_EQ(raw[3], '\t');
    CHECK_EQ(raw[4], 'c');
    CHECK_EQ(raw[5], '\n');

    /* Reading it back through the tool must not let the NUL cut the result. */
    call_json("read_file", "{\"path\":\"/tmp/esc.bin\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "a.b");
    CHECK_CONTAINS(rbuf, "non-printable");
}

/* ====================================================================== */
/* 3. read / list / stat / mkdir / delete behaviour                       */
/* ====================================================================== */

static void test_read_roundtrip(void) {
    tool_result_t r;
    trace_reset();
    kcap_reset();
    call_json("read_file", "{\"path\":\"/etc/motd\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "/etc/motd: 12 bytes total");
    CHECK_CONTAINS(rbuf, "to end of file");
    CHECK_CONTAINS(rbuf, "boot banner");
    CHECK_EQ(trace_count(), 1);
    CHECK_CONTAINS(kcap_text(), "[vfs_read /etc/motd off=0 -> 12]");
}

static void test_read_offset_and_max_bytes(void) {
    tool_result_t r;
    call_json("read_file",
              "{\"path\":\"/etc/motd\",\"offset\":5,\"max_bytes\":3}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "showing bytes 5-7");
    CHECK_CONTAINS(rbuf, "TRUNCATED");
    CHECK_CONTAINS(rbuf, "offset=8");

    /* Reading past the end is legal and explicit, not an error. */
    call_json("read_file", "{\"path\":\"/etc/motd\",\"offset\":9999}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "at or past");
}

/* A file bigger than one result must page cleanly and reassemble exactly. */
static void test_read_paging_reassembles(void) {
    tool_result_t r;
    const size_t N = 10000;
    char *big = malloc(N + 1);
    for (size_t i = 0; i < N; i++) big[i] = (char)('a' + (i % 26));
    big[N] = '\0';

    char *req = malloc(N + 128);
    int rl = snprintf(req, N + 128,
                      "{\"path\":\"/tmp/big.txt\",\"content\":\"%s\"}", big);
    CHECK(rl > 0);
    dispatch("write_file", req, (size_t)rl, &r);
    CHECK_EQ(r.is_error, 0);

    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/tmp/big.txt", &st), VFS_OK);
    CHECK_EQ((long)st.size, (long)N);

    /* Page through it the way the model is told to. */
    char *acc = malloc(N + 4096);
    size_t got = 0;
    unsigned long off = 0;
    int pages = 0;
    for (;;) {
        char q[128];
        snprintf(q, sizeof q,
                 "{\"path\":\"/tmp/big.txt\",\"offset\":%lu}", off);
        call_json("read_file", q, &r);
        CHECK_EQ(r.is_error, 0);
        pages++;
        const char *sep = strstr(rbuf, "\n---\n");
        CHECK(sep != NULL);
        if (!sep) break;
        const char *body = sep + 5;
        size_t blen = strlen(body);
        CHECK(got + blen <= N);
        memcpy(acc + got, body, blen);
        got += blen;
        off += blen;
        if (!strstr(rbuf, "TRUNCATED")) break;
        CHECK(pages < 20);
        if (pages >= 20) break;
    }
    CHECK(pages > 1);                     /* it really did have to page */
    CHECK_EQ((long)got, (long)N);
    CHECK_EQ(memcmp(acc, big, N), 0);

    call_json("delete_path", "{\"path\":\"/tmp/big.txt\"}", &r);
    CHECK_EQ(r.is_error, 0);

    free(big); free(req); free(acc);
}

static void test_list_dir(void) {
    tool_result_t r;
    trace_reset();
    kcap_reset();
    call_json("list_dir", "{\"path\":\"/\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "etc/  dir");
    CHECK_CONTAINS(rbuf, "tmp/  dir");
    CHECK_EQ(trace_count(), 1);
    CHECK_CONTAINS(kcap_text(), "[vfs_readdir / -> ");

    call_json("list_dir", "{\"path\":\"/etc\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "motd  file  12 bytes");

    /* A file is not a directory, and the message says which tool to use. */
    call_json("list_dir", "{\"path\":\"/etc/motd\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "not a directory");

    call_json("list_dir", "{\"path\":\"/nope\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "no such directory");
}

static void test_list_dir_empty(void) {
    tool_result_t r;
    call_json("make_dir", "{\"path\":\"/tmp/emptydir\"}", &r);
    CHECK_EQ(r.is_error, 0);
    call_json("list_dir", "{\"path\":\"/tmp/emptydir\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "empty directory");
    call_json("delete_path", "{\"path\":\"/tmp/emptydir\"}", &r);
    CHECK_EQ(r.is_error, 0);
}

/* Many entries: the listing must stop cleanly, inside the buffer, and say so. */
static void test_list_dir_truncates_loudly(void) {
    tool_result_t r;
    call_json("make_dir", "{\"path\":\"/tmp/many\"}", &r);
    for (int i = 0; i < 200; i++) {
        char q[160];
        snprintf(q, sizeof q,
                 "{\"path\":\"/tmp/many/entry_number_%03d\",\"content\":\"x\"}", i);
        call_json("write_file", q, &r);
        CHECK_EQ(r.is_error, 0);
    }
    call_json("list_dir", "{\"path\":\"/tmp/many\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "did not fit in one result");
    CHECK(r.len < RCAP);

    for (int i = 0; i < 200; i++) {
        char q[160];
        snprintf(q, sizeof q,
                 "{\"path\":\"/tmp/many/entry_number_%03d\"}", i);
        call_json("delete_path", q, &r);
        CHECK_EQ(r.is_error, 0);
    }
    call_json("delete_path", "{\"path\":\"/tmp/many\"}", &r);
    CHECK_EQ(r.is_error, 0);
}

static void test_stat(void) {
    tool_result_t r;
    trace_reset();
    kcap_reset();
    call_json("stat_path", "{\"path\":\"/etc/motd\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "type=file size=12");
    CHECK_EQ(trace_count(), 1);
    CHECK_CONTAINS(kcap_text(), "[vfs_stat /etc/motd type=file size=12 -> ok]");

    call_json("stat_path", "{\"path\":\"/etc\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "type=dir entries=1");

    kcap_reset();
    call_json("stat_path", "{\"path\":\"/does/not/exist\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "does not exist");
    CHECK_CONTAINS(kcap_text(), "[vfs_stat /does/not/exist -> ENOENT]");
}

static void test_make_dir(void) {
    tool_result_t r;

    /* No parents flag and a missing parent: refuse, and say how to fix it. */
    call_json("make_dir", "{\"path\":\"/a/b/c\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "parents=true");
    CHECK(vfs_stat("/a", &(vfs_stat_t){0}) != VFS_OK);

    kcap_reset();
    call_json("make_dir", "{\"path\":\"/a/b/c\",\"parents\":true}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(kcap_text(), "[vfs_mkdir /a/b/c made=3 -> ok]");
    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/a/b/c", &st), VFS_OK);
    CHECK_EQ(st.type, VNODE_DIR);

    /* Idempotent. */
    call_json("make_dir", "{\"path\":\"/a/b/c\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(rbuf, "already exists and is a directory");

    /* Colliding with a file is an error. */
    call_json("make_dir", "{\"path\":\"/etc/motd\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "already exists and is a file");

    /* Root. */
    call_json("make_dir", "{\"path\":\"/\"}", &r);
    CHECK_EQ(r.is_error, 1);

    call_json("delete_path", "{\"path\":\"/a/b/c\"}", &r);
    CHECK_EQ(r.is_error, 0);
    call_json("delete_path", "{\"path\":\"/a/b\"}", &r);
    call_json("delete_path", "{\"path\":\"/a\"}", &r);
    CHECK(vfs_stat("/a", &st) != VFS_OK);
}

static void test_delete(void) {
    tool_result_t r;

    call_json("write_file",
              "{\"path\":\"/tmp/doomed\",\"content\":\"bye\"}", &r);
    CHECK_EQ(r.is_error, 0);

    trace_reset();
    kcap_reset();
    call_json("delete_path", "{\"path\":\"/tmp/doomed\"}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK_EQ(trace_count(), 1);
    CHECK_CONTAINS(kcap_text(), "[vfs_unlink /tmp/doomed type=file size=3 -> ok]");
    vfs_stat_t st;
    CHECK(vfs_stat("/tmp/doomed", &st) != VFS_OK);       /* really gone */

    /* Deleting it again is a legible ENOENT, not a fault. */
    call_json("delete_path", "{\"path\":\"/tmp/doomed\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "does not exist");

    /* Non-empty directories are refused with a count. */
    call_json("delete_path", "{\"path\":\"/etc\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "not empty");
    CHECK_EQ(vfs_stat("/etc", &st), VFS_OK);

    /* The root is refused outright. */
    kcap_reset();
    call_json("delete_path", "{\"path\":\"/\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "refusing to delete the filesystem root");
    CHECK_CONTAINS(kcap_text(), "-> EINVAL]");
    CHECK_EQ(vfs_stat("/", &st), VFS_OK);
}

/* ====================================================================== */
/* 4. hostile input                                                       */
/* ====================================================================== */

static void test_path_validation(void) {
    tool_result_t r;
    struct { const char *json; const char *want; } cases[] = {
        { "{}",                              "missing required argument"     },
        { "{\"path\":123}",                  "must be a string"              },
        { "{\"path\":null}",                 "must be a string"              },
        { "{\"path\":[]}",                   "must be a string"              },
        { "{\"path\":{}}",                   "must be a string"              },
        { "{\"path\":\"\"}",                 "must not be empty"             },
        { "{\"path\":\"etc/motd\"}",         "must be an absolute path"      },
        { "{\"path\":\"../etc/motd\"}",      "must be an absolute path"      },
        { "{\"path\":\"/etc/../../root\"}",  "must not contain"              },
        { "{\"path\":\"/..\"}",              "must not contain"              },
        { "{\"path\":\"/a/../b\"}",          "must not contain"              },
        { "{\"path\":\"/a\\u0000b\"}",       "embedded NUL"                  },
        { "{\"path\":\"/a\\u0007b\"}",       "control character"             },
        { "{\"path\":\"/a\\nb\"}",           "control character"             },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (int t = 0; t < NTOOLS; t++) {
            kcap_reset();
            call_json(TOOL_NAMES[t], cases[i].json, &r);
            CHECK_EQ(r.is_error, 1);
            CHECK_CONTAINS(rbuf, cases[i].want);
            CHECK_CONTAINS(kcap_text(), "-> EINVAL]");
        }
    }
}

static void test_long_paths(void) {
    tool_result_t r;
    static char q[16384];
    for (size_t plen = 100; plen <= 12000; plen = plen < 200 ? plen + 1 : plen * 2) {
        size_t o = 0;
        o += (size_t)snprintf(q + o, sizeof q - o, "{\"path\":\"");
        for (size_t i = 0; i < plen && o < sizeof q - 32; i++)
            q[o++] = (i % 40 == 0) ? '/' : 'x';
        o += (size_t)snprintf(q + o, sizeof q - o, "\",\"content\":\"c\"}");
        q[o] = '\0';

        for (int t = 0; t < NTOOLS; t++) {
            dispatch(TOOL_NAMES[t], q, o, &r);
            if (plen < PATH_TOO_LONG_AT) continue;  /* short ones fail for other reasons */
            CHECK_EQ(r.is_error, 1);
            CHECK_CONTAINS(rbuf, "too long");
        }
    }
    /* A single component longer than VFS_NAME_MAX is rejected, not truncated. */
    size_t o = (size_t)snprintf(q, sizeof q, "{\"path\":\"/");
    for (int i = 0; i < 100; i++) q[o++] = 'y';
    o += (size_t)snprintf(q + o, sizeof q - o, "\",\"content\":\"c\"}");
    dispatch("write_file", q, o, &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "path component longer than");
}

static void test_numeric_validation(void) {
    tool_result_t r;
    const char *bad[] = {
        "{\"path\":\"/etc/motd\",\"offset\":-1}",
        "{\"path\":\"/etc/motd\",\"offset\":\"0\"}",
        "{\"path\":\"/etc/motd\",\"offset\":1e400}",
        "{\"path\":\"/etc/motd\",\"offset\":99999999999999999999999}",
        "{\"path\":\"/etc/motd\",\"offset\":true}",
        "{\"path\":\"/etc/motd\",\"max_bytes\":-5}",
        "{\"path\":\"/etc/motd\",\"max_bytes\":[1]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        call_json("read_file", bad[i], &r);
        CHECK_EQ(r.is_error, 1);
        CHECK(strstr(rbuf, "offset") || strstr(rbuf, "max_bytes"));
    }
    /* Enormous-but-legal values clamp instead of failing. */
    call_json("read_file",
              "{\"path\":\"/etc/motd\",\"offset\":0,\"max_bytes\":1000000}", &r);
    CHECK_EQ(r.is_error, 0);
    CHECK(r.len < RCAP);
}

static void test_enum_and_bool_validation(void) {
    tool_result_t r;
    call_json("write_file",
              "{\"path\":\"/tmp/x\",\"content\":\"a\",\"mode\":\"clobber\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "must be one of");
    CHECK_CONTAINS(rbuf, "create_new");

    call_json("write_file",
              "{\"path\":\"/tmp/x\",\"content\":\"a\",\"mode\":7}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "must be one of");

    call_json("make_dir", "{\"path\":\"/tmp/y\",\"parents\":\"yes\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "true or false");

    call_json("write_file", "{\"path\":\"/tmp/x\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "missing required argument \"content\"");

    call_json("write_file", "{\"path\":\"/tmp/x\",\"content\":42}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "\"content\" must be a string");
}

static void test_write_size_limit(void) {
    tool_result_t r;
    size_t N = 70000;
    char *q = malloc(N + 128);
    size_t o = (size_t)snprintf(q, N + 128, "{\"path\":\"/tmp/huge\",\"content\":\"");
    for (size_t i = 0; i < N; i++) q[o++] = 'Z';
    o += (size_t)snprintf(q + o, 64, "\"}");
    dispatch("write_file", q, o, &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "write limit");
    vfs_stat_t st;
    CHECK(vfs_stat("/tmp/huge", &st) != VFS_OK);   /* nothing was created */
    free(q);
}

/* Truncated / malformed JSON, from a span whose next page is unmapped. */
static void test_malformed_json_no_overread(void) {
    tool_result_t r;
    const char *full =
        "{\"path\":\"/tmp/frag.txt\",\"content\":\"hello\",\"mode\":\"append\"}";
    size_t n = strlen(full);
    for (size_t cut = 0; cut <= n; cut++) {
        for (int t = 0; t < NTOOLS; t++) {
            dispatch(TOOL_NAMES[t], full, cut, &r);
            CHECK(r.len < RCAP);
            CHECK_EQ(kpanic_hit, 0);
        }
    }
    const char *junk[] = {
        "", "{", "}", "[]", "null", "\"str\"", "3", "{\"path\"", "{\"path\":",
        "{\"path\":\"", "{\"path\":\"/a\"", "{,}", "{\"a\":}", "[[[[[[[[[[",
        "{\"path\":\"\\ud800\"}", "{\"path\":\"\\u\"}", "{\"path\":\"\\\"}",
        "{\"path\" \"/etc\"}", "\xff\xfe\x00\x01", "{\"path\":\"/etc\",}",
    };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++)
        for (int t = 0; t < NTOOLS; t++) {
            dispatch(TOOL_NAMES[t], junk[i], strlen(junk[i]), &r);
            CHECK(r.len < RCAP);
            CHECK_EQ(kpanic_hit, 0);
        }
}

/* Deep nesting must hit json.h's depth limit, not the C stack. */
static void test_deep_nesting(void) {
    tool_result_t r;
    static char q[8192];
    size_t o = 0;
    for (int i = 0; i < 2000; i++) q[o++] = '[';
    for (int i = 0; i < 2000; i++) q[o++] = ']';
    for (int t = 0; t < NTOOLS; t++) {
        dispatch(TOOL_NAMES[t], q, o, &r);
        CHECK_EQ(r.is_error, 1);
        CHECK_EQ(kpanic_hit, 0);
    }
}

/* Pseudo-random bytes and random mutations of a valid call. Nothing may panic,
 * over-read, or leave the result buffer unterminated. */
static uint32_t rng_state = 0x1234567u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_fuzz(void) {
    tool_result_t r;
    char buf[512];
    const char *seed =
        "{\"path\":\"/tmp/fz.txt\",\"content\":\"data\",\"offset\":3,"
        "\"max_bytes\":16,\"mode\":\"append\",\"parents\":true}";
    size_t slen = strlen(seed);

    for (int iter = 0; iter < 4000; iter++) {
        size_t n;
        if (iter % 2 == 0) {
            n = rng() % sizeof buf;
            for (size_t i = 0; i < n; i++) buf[i] = (char)(rng() & 0xff);
        } else {
            n = slen;
            memcpy(buf, seed, n);
            int muts = 1 + (int)(rng() % 4);
            for (int m = 0; m < muts; m++) {
                size_t at = rng() % n;
                switch (rng() % 3) {
                    case 0: buf[at] = (char)(rng() & 0xff); break;
                    case 1: buf[at] = '\0'; break;
                    default: n = at; break;              /* truncate */
                }
                if (n == 0) break;
            }
        }
        const char *name = TOOL_NAMES[rng() % NTOOLS];
        dispatch(name, buf, n, &r);
        CHECK_EQ(kpanic_hit, 0);
        if (kpanic_hit) return;
    }
    /* The filesystem must still be sane after all that. */
    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/etc/motd", &st), VFS_OK);
    CHECK_EQ(heap_check(), 0);
}

/* An unknown tool name is dispatch's job, but assert the recovery path anyway:
 * the model must be told what does exist. */
static void test_unknown_tool_lists_ours(void) {
    tool_result_t r;
    CHECK_EQ(call_json("no_such_tool", "{}", &r), TOOL_ENOENT);
    CHECK_EQ(r.is_error, 1);
    for (int i = 0; i < NTOOLS; i++) CHECK_CONTAINS(rbuf, TOOL_NAMES[i]);
}

/* Every successful or failed call must produce exactly one kernel line. */
static void test_exactly_one_trace_line_per_call(void) {
    tool_result_t r;
    const char *calls[][2] = {
        { "stat_path",   "{\"path\":\"/etc\"}"                              },
        { "stat_path",   "{\"path\":\"/nope\"}"                             },
        { "list_dir",    "{\"path\":\"/\"}"                                 },
        { "read_file",   "{\"path\":\"/etc/motd\"}"                         },
        { "read_file",   "{\"bogus\":1}"                                    },
        { "make_dir",    "{\"path\":\"/tmp/t1\"}"                           },
        { "write_file",  "{\"path\":\"/tmp/t1/f\",\"content\":\"q\"}"       },
        { "delete_path", "{\"path\":\"/tmp/t1/f\"}"                         },
        { "delete_path", "{\"path\":\"/tmp/t1\"}"                           },
    };
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        trace_reset();
        kcap_reset();
        call_json(calls[i][0], calls[i][1], &r);
        CHECK_EQ(trace_count(), 1);
        CHECK_CONTAINS(kcap_text(), "[");
        CHECK_CONTAINS(kcap_text(), " -> ");
    }
}

/* Path canonicalisation: these all name the same file. */
static void test_path_canonicalisation(void) {
    tool_result_t r;
    const char *forms[] = {
        "/etc/motd", "//etc//motd", "/etc/./motd", "/./etc/motd",
        "/etc/motd/", "///etc/motd//"
    };
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        char q[128];
        snprintf(q, sizeof q, "{\"path\":\"%s\"}", forms[i]);
        kcap_reset();
        call_json("stat_path", q, &r);
        CHECK_EQ(r.is_error, 0);
        CHECK_CONTAINS(rbuf, "/etc/motd: type=file");
        /* The trace shows the canonical path, not what the model typed. */
        CHECK_CONTAINS(kcap_text(), "[vfs_stat /etc/motd type=file");
    }
}

static void test_write_missing_parent(void) {
    tool_result_t r;
    call_json("write_file",
              "{\"path\":\"/no/such/dir/f.txt\",\"content\":\"x\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "parent directory /no/such/dir does not exist");

    /* Writing through a file as if it were a directory. */
    call_json("write_file",
              "{\"path\":\"/etc/motd/child\",\"content\":\"x\"}", &r);
    CHECK_EQ(r.is_error, 1);

    /* Writing over a directory. */
    call_json("write_file", "{\"path\":\"/etc\",\"content\":\"x\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "is a directory");

    /* Reading a directory. */
    call_json("read_file", "{\"path\":\"/etc\"}", &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(rbuf, "Use list_dir instead");
}

/* ====================================================================== */

/* Mirrors the kernel's boot filesystem (fs_init + the /etc,/tmp,/etc/motd
 * self-test in kernel/main.c) so the tests run against the same shape. */
static void boot_fs(void) {
    vfs_init();
    ramfs_register();
    if (vfs_mount("/", "ramfs") != VFS_OK) { printf("mount failed\n"); exit(2); }
    vfs_mkdir("/etc");
    vfs_mkdir("/tmp");
    file_t *f = vfs_open("/etc/motd", O_CREAT | O_RDWR);
    if (!f) { printf("motd failed\n"); exit(2); }
    vfs_write(f, "boot banner\n", 12);     /* 12 printable bytes */
    vfs_close(f);
}

int main(void) {
    console_init();
    boot_fs();

    RUN(test_tools_are_registered);
    RUN(test_mutating_tools_are_flagged);
    RUN(test_schema_is_valid_json);
    RUN(test_schema_truncation_is_bounded);

    RUN(test_write_lands_real_bytes);
    RUN(test_overwrite_and_append);
    RUN(test_write_preserves_escaped_bytes);

    RUN(test_read_roundtrip);
    RUN(test_read_offset_and_max_bytes);
    RUN(test_read_paging_reassembles);
    RUN(test_list_dir);
    RUN(test_list_dir_empty);
    RUN(test_list_dir_truncates_loudly);
    RUN(test_stat);
    RUN(test_make_dir);
    RUN(test_delete);
    RUN(test_path_canonicalisation);
    RUN(test_write_missing_parent);

    RUN(test_path_validation);
    RUN(test_long_paths);
    RUN(test_numeric_validation);
    RUN(test_enum_and_bool_validation);
    RUN(test_write_size_limit);
    RUN(test_malformed_json_no_overread);
    RUN(test_deep_nesting);
    RUN(test_unknown_tool_lists_ours);
    RUN(test_exactly_one_trace_line_per_call);
    RUN(test_fuzz);

    return th_report("vfs_tools");
}
