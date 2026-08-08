/* test_capability.c — the capability store, its dispatcher and its budgets.
 *
 * WHAT THIS SUITE IS FOR
 *   core/capability.c is the part of this machine that KEEPS things, so almost
 *   every question about it is a question about a boundary: a record that is
 *   half-written, a program whose registers say something the ABI does not
 *   allow, a composition that calls itself, a replacement that would break a
 *   caller, a name that came off a disk instead of out of this kernel. None of
 *   those are reachable by calling the happy path with good arguments, so the
 *   suite spends most of its checks on the other kind.
 *
 * WHY IT CAN BE A HOST TEST AT ALL
 *   Capabilities get NO device access by design (include/capability.h, THE
 *   SANDBOX), so the whole dispatcher runs against a dvm_io_t with every hardware
 *   hook NULL. The only thing it needs from the kernel is a syscall backend, and
 *   that is injectable — FAKE_SYS below is a synthetic one that answers
 *   con.write / fs.* / time.ms over the real VFS and, crucially, COUNTS
 *   BOUNDARY ESCAPES: if the VM ever hands it a path outside the data root, or
 *   an unterminated one, `g_escapes` goes up and the assertion at the end of
 *   every relevant test fails. The confinement is enforced in the portable half
 *   of vm/dvm.c, so proving it here proves it on the machine.
 *
 * WHAT A "REBOOT" IS IN THIS FILE
 *   capability_init() rebuilds the registry from the store and nothing else, so
 *   reboot_and_reload() — clear the registry, re-read the directory — is exactly
 *   what the machine does at boot, minus the hardware. It is not a substitute for
 *   the real two-boot proof (that is in the transcript, against a FAT32 disk),
 *   but it is what lets the corrupt-record and fall-back-to-backup paths be
 *   tested at all: they need a store that has been damaged between boots, which
 *   is not a thing a running kernel does to itself.
 */

/* kernel.h FIRST, before harness.h drags in the host <string.h>. macOS fortifies
 * memcpy/memset into function-like macros, and kernel.h then DECLARES those
 * names — which expands the macro over a prototype and produces forty lines of
 * unrelated errors. Including it first is the whole fix. */
#include "kernel.h"

#include "harness.h"
#include "capability.h"
#include "dvm.h"
#include "vfs.h"
#include "fs.h"
#include "tool.h"
#include "tooltable.h"
#include "trace.h"
#include "json.h"
#include "heap.h"

#include <stdlib.h>

/* REGISTER_TOOL collects pointers in a linker section that a Mach-O host link
 * has no __start_/__stop_ symbols for, and the descriptors are static to their
 * translation unit — so the tool source is compiled INTO this one, after
 * tooltable.h has redefined the macro. tests/host/tooltable.h explains both
 * halves; the Makefile carries the matching prerequisite line. */
#include "../../tools/capability_tools.c"

/* ====================================================================== */
/* the synthetic kernel a capability program is run against                */
/* ====================================================================== */

/* Every escape is a defect in the layer under test, so they are counted rather
 * than asserted one at a time: one global, checked everywhere. */
static int      g_escapes;
static char     g_said[4096];
static size_t   g_said_len;
static uint64_t g_now_ms = 1000;
static int      g_sys_calls;

static void said_reset(void) { g_said[0] = '\0'; g_said_len = 0; }

/* The ONE rule the backend polices on the VM's behalf, so that a violation is
 * attributed to the VM and not to this file's own leniency: a path a capability
 * program named must be absolute, printable, NUL-terminated inside its buffer,
 * and inside the data root. */
static void check_path(const char *p) {
    const char *root = capability_data_root();
    size_t      rl   = strlen(root);
    if (!p || p[0] != '/') { g_escapes++; return; }
    if (strlen(p) >= 128)  { g_escapes++; return; }
    for (size_t i = 0; p[i]; i++)
        if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] >= 0x7F) {
            g_escapes++;
            return;
        }
    if (strncmp(p, root, rl) != 0 || (p[rl] != '/' && p[rl] != '\0'))
        g_escapes++;
}

static int64_t fk_con_write(void *ctx, const char *text, size_t len,
                            char *err, size_t errcap) {
    (void)ctx; (void)err; (void)errcap;
    g_sys_calls++;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c >= 0x7F) { g_escapes++; break; }
    }
    for (size_t i = 0; i < len && g_said_len + 1 < sizeof g_said; i++)
        g_said[g_said_len++] = text[i];
    g_said[g_said_len] = '\0';
    /* And out through trace.c, exactly as the real backend does, so the column
     * zero invariant is exercised on this path too. */
    trace_ok("dvm.say", "%s", g_said);
    return (int64_t)len;
}

static int64_t fk_fs_read(void *ctx, const char *path, void *dst, size_t cap,
                          char *err, size_t errcap) {
    (void)ctx;
    g_sys_calls++;
    check_path(path);
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) { snprintf(err, errcap, "no such file"); return -1; }
    int64_t n = vfs_read(f, dst, (uint64_t)cap);
    vfs_close(f);
    return n < 0 ? -1 : n;
}

static int64_t fk_fs_write(void *ctx, const char *path, const void *src,
                           size_t len, char *err, size_t errcap) {
    (void)ctx;
    g_sys_calls++;
    check_path(path);
    file_t *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    if (!f) { snprintf(err, errcap, "could not create it"); return -1; }
    int64_t n = vfs_write(f, src, (uint64_t)len);
    vfs_close(f);
    return n < 0 ? -1 : n;
}

static int64_t fk_fs_size(void *ctx, const char *path, char *err, size_t errcap) {
    (void)ctx;
    g_sys_calls++;
    check_path(path);
    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK) { snprintf(err, errcap, "not there"); return -1; }
    return (int64_t)st.size;
}

static int64_t fk_time_ms(void *ctx, char *err, size_t errcap) {
    (void)ctx; (void)err; (void)errcap;
    g_sys_calls++;
    return (int64_t)(g_now_ms += 7);
}

static const dvm_sys_t FAKE_SYS = {
    .ctx        = 0,
    .con_write  = fk_con_write,
    .fs_read    = fk_fs_read,
    .fs_write   = fk_fs_write,
    .fs_size    = fk_fs_size,
    .audio_tone = 0,
    .time_ms    = fk_time_ms,
};

/* ====================================================================== */
/* fixture                                                                */
/* ====================================================================== */

static void unlink_in(const char *dir, const char *leaf) {
    file_t *d = vfs_open(dir, O_RDONLY);
    if (!d) return;
    if (d->node && d->node->type == VNODE_DIR && d->node->ops &&
        d->node->ops->unlink)
        d->node->ops->unlink(d->node, leaf);
    vfs_close(d);
}

/* Empty the store on disk AND in RAM, so each test starts from a machine that
 * has never been taught anything. */
static void fresh_machine(void) {
    capability_set_sys(&FAKE_SYS);
    const char *store = capability_store_root();
    char name[128];
    /* readdir indices shift under unlink, so sweep from index 0 until it is
     * empty rather than walking once. */
    for (int guard = 0; guard < 256; guard++) {
        if (vfs_readdir(store, 0, name, sizeof name) != VFS_OK) break;
        unlink_in(store, name);
    }
    capability_init();
    g_escapes = 0;
    g_sys_calls = 0;
    said_reset();
    kcap_reset();
}

/* What the machine does at boot: forget the registry, read the store back. */
static void reboot_and_reload(void) {
    capability_init();
}

static char g_msg[512];

/* Save a program capability, asserting nothing — callers check. */
static int save_program(const char *name, const char *doc, const char *params,
                        const char *src) {
    static char in[CAP_SRC_MAX * 2 + 1024];
    static char esc[CAP_SRC_MAX * 2 + 8];
    json_escape(esc, sizeof esc, src);
    snprintf(in, sizeof in,
             "{\"name\":\"%s\",\"doc\":\"%s\",\"params\":[%s],\"program\":\"%s\"}",
             name, doc, params, esc);

    static cap_t spec;
    static char  srcbuf[CAP_SRC_MAX + 1];
    size_t       srclen = 0;
    int rc = capability_parse_spec(in, strlen(in), &spec, srcbuf, sizeof srcbuf,
                                   &srclen, g_msg, sizeof g_msg);
    if (rc != CAP_OK) return rc;
    return capability_save(&spec, srcbuf, srclen, g_msg, sizeof g_msg);
}

static int save_raw(const char *json) {
    static cap_t spec;
    static char  srcbuf[CAP_SRC_MAX + 1];
    size_t       srclen = 0;
    int rc = capability_parse_spec(json, strlen(json), &spec, srcbuf,
                                   sizeof srcbuf, &srclen, g_msg, sizeof g_msg);
    if (rc != CAP_OK) return rc;
    return capability_save(&spec, srcbuf, srclen, g_msg, sizeof g_msg);
}

static int call1(const char *name, uint64_t a0, cap_result_t *res) {
    uint64_t a[1] = { a0 };
    return capability_invoke(name, a, 1, res);
}

/* ---- the reference programs ---------------------------------------------- */

/* r0 = a0 * 2, plus a text result, which is the whole ABI in six lines. */
static const char *P_DOUBLE =
    "mul   r9, r0, 2            ; the answer\n"
    "mstr  r1, 100, \"doubled to \"\n"
    "mitoa r1, r1, r9, 10\n"
    "sub   r2, r1, 100          ; r2 = LENGTH, not the end offset\n"
    "mov   r1, 100              ; r1 = where the text starts\n"
    "mov   r0, r9\n"
    "halt\n";

/* Tail-call factorial in accumulator-passing style. This is the shape the tool
 * description advertises, executed verbatim.
 *   fact(n, acc): if n == 0 -> acc, else fact(n-1, acc*n) */
static const char *P_FACT =
    ".equ TAILCALL 0x7a11ca11\n"
    ".equ NAMEAT   1024\n"
    "cmp   r0, 0\n"
    "beq   done\n"
    "mul   r10, r1, r0          ; acc * n\n"
    "sub   r11, r0, 1           ; n - 1\n"
    "mstr  r4, NAMEAT, \"fact\"  ; the callee's name, built at run time\n"
    "sub   r5, r4, NAMEAT       ; r5 = its LENGTH\n"
    "mov   r4, NAMEAT\n"
    "mov   r6, r11              ; arg 0 = n-1\n"
    "mov   r7, r10              ; arg 1 = acc*n\n"
    "mov   r3, TAILCALL\n"
    "halt\n"
    "done:\n"
    "mov   r0, r1               ; the accumulator is the answer\n"
    "mov   r2, 0                ; no text\n"
    "mov   r3, 0                ; NOT a tail call\n"
    "halt\n";

/* A tail-call loop with no exit: the invocation budget is what stops it. */
static const char *P_FOREVER =
    ".equ TAILCALL 0x7a11ca11\n"
    "mstr  r4, 512, \"forever\"\n"
    "sub   r5, r4, 512\n"
    "mov   r4, 512\n"
    "mov   r3, TAILCALL\n"
    "halt\n";

/* Writes a file under the data root and reads it back; r0 = bytes read. */
static const char *P_NOTE =
    "mstr  r10, 0,    \"/data/note.txt\"\n"
    "sub   r10, r10, 0\n"
    "mstr  r11, 200,  \"kept\"\n"
    "sub   r11, r11, 200\n"
    "push  0\n"
    "push  r10\n"
    "push  200\n"
    "push  r11\n"
    "sys   r12, fs.write\n"
    "bne   bad\n"
    "push  0\n"
    "push  r10\n"
    "push  400\n"
    "push  64\n"
    "sys   r0, fs.read\n"
    "bne   bad\n"
    "mov   r1, 400\n"
    "mov   r2, r0\n"
    "halt\n"
    "bad: abort \"the filesystem refused\"\n";

/* Tries to overwrite the capability store. MUST be refused by the VM. */
static const char *P_ATTACK =
    "mstr  r10, 0, \"/cap/double.cap\"\n"
    "sub   r10, r10, 0\n"
    "mstr  r11, 200, \"x\"\n"
    "sub   r11, r11, 200\n"
    "push  0\n"
    "push  r10\n"
    "push  200\n"
    "push  r11\n"
    "sys   r12, fs.write\n"
    "bne   refused\n"
    "mov   r0, 1                ; it worked, which is the bug\n"
    "halt\n"
    "refused:\n"
    "mov   r0, 0\n"
    "halt\n";

/* ====================================================================== */
/* registration and the schema                                            */
/* ====================================================================== */

static void test_tools_are_registered(void) {
    CHECK(tool_find("capability_save")   != 0);
    CHECK(tool_find("capability_call")   != 0);
    CHECK(tool_find("capability_list")   != 0);
    CHECK(tool_find("capability_revert") != 0);
    CHECK_TOOL_FLAGS("capability_save",   TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("capability_call",   TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("capability_revert", TOOL_MUTATES, TOOL_MUTATES);
    /* Listing changes nothing, and a read-only mode has to be able to tell. */
    CHECK_TOOL_FLAGS("capability_list",   TOOL_MUTATES, 0);
}

static void test_schemas_are_valid_json(void) {
    for (int i = 0; i < tool_count(); i++) {
        const tool_t *t = tool_at(i);
        json_value_t  v;
        CHECK(json_parse(t->input_schema, strlen(t->input_schema), &v) == JSON_OK);
        CHECK(v.type == JSON_OBJECT);
        CHECK(t->description && t->description[0]);
    }
}

/* THE SCHEMA MUST NOT CHANGE SIZE WHEN A CAPABILITY IS SAVED.
 *
 * This is the invariant that keeps include/chat.h's CHAT_REGISTRY_BYTES a
 * compile-time constant while the description it measures reads off a disk. If it
 * ever breaks, `make test-qemu` goes red on a machine where nothing is wrong, and
 * the reason would be invisible: the tool schema would simply be a different
 * length after the operator saved something last night. */
static void test_the_panel_is_a_fixed_width(void) {
    fresh_machine();
    size_t empty = tools_build_schema(0, 0);
    CHECK_EQ(strlen(capability_panel()), CAP_PANEL_CHARS);
    CHECK(capability_panel_check(g_msg, sizeof g_msg));

    CHECK_EQ(save_program("alpha", "doubles a number", "\"n\"", P_DOUBLE), CAP_OK);
    CHECK_EQ(strlen(capability_panel()), CAP_PANEL_CHARS);
    CHECK_EQ(tools_build_schema(0, 0), empty);

    /* And with the panel full to overflowing. */
    for (int i = 0; i < CAP_MAX - 1; i++) {
        char nm[CAP_NAME_MAX];
        snprintf(nm, sizeof nm, "filler%d", i);
        CHECK_EQ(save_program(nm, "a capability with a deliberately long doc line "
                                  "so the panel has to clip the list", "\"n\"",
                              P_DOUBLE), CAP_OK);
    }
    CHECK_EQ(capability_count(), CAP_MAX);
    CHECK_EQ(strlen(capability_panel()), CAP_PANEL_CHARS);
    CHECK(capability_panel_check(g_msg, sizeof g_msg));
    CHECK_EQ(tools_build_schema(0, 0), empty);
    /* Clipped, but never mid-name: the escape hatch is named. */
    CHECK_CONTAINS(capability_panel(), "more (capability_list)");

    /* The 17th is refused, and says why, and nothing is damaged. */
    CHECK_EQ(save_program("onetoomany", "does not fit", "", P_DOUBLE), CAP_ENOSPC);
    CHECK_CONTAINS(g_msg, "Delete one first");
    CHECK_EQ(capability_count(), CAP_MAX);
    fresh_machine();
}

static void test_the_panel_holds_no_byte_that_escaping_would_lengthen(void) {
    fresh_machine();
    /* A doc is refused if it is not printable, so the only way a hostile byte
     * could reach the panel is a bug in the renderer. Prove the sanitiser runs
     * by checking every byte of a full panel. */
    CHECK_EQ(save_program("quoted", "a doc with no quotes in it at all", "", P_DOUBLE),
             CAP_OK);
    const char *p = capability_panel();
    for (size_t i = 0; i < CAP_PANEL_CHARS; i++) {
        unsigned char c = (unsigned char)p[i];
        CHECK(c >= 0x20 && c < 0x7F && c != '"' && c != '\\');
    }
    /* A doc containing a quote is refused before it can get near the panel. */
    CHECK_EQ(save_raw("{\"name\":\"q2\",\"doc\":\"say \\\"hi\\\"\",\"params\":[],"
                      "\"program\":\"halt\"}"), CAP_OK);
    CHECK_EQ(strlen(capability_panel()), CAP_PANEL_CHARS);
    for (size_t i = 0; i < CAP_PANEL_CHARS; i++)
        CHECK(p[i] != '"' && p[i] != '\\');
}

/* ====================================================================== */
/* save, load, call                                                       */
/* ====================================================================== */

static void test_save_then_call(void) {
    fresh_machine();
    CHECK_EQ(save_program("double", "doubles n and describes it", "\"n\"",
                          P_DOUBLE), CAP_OK);
    CHECK_CONTAINS(g_msg, "version 1");

    const cap_t *c = capability_find("double");
    CHECK(c != 0);
    CHECK_EQ(c->version, 1);
    CHECK_EQ(c->nparam, 1);
    CHECK_EQ(c->kind, CAP_KIND_PROGRAM);
    CHECK(c->ninsn > 0);
    CHECK_STR(c->param[0], "n");

    cap_result_t res;
    CHECK_EQ(call1("double", 21, &res), CAP_OK);
    CHECK_EQ(res.value, 42);
    CHECK_STR(res.text, "doubled to 42");
    CHECK_EQ(res.invocations, 1);
    CHECK_EQ(res.depth_used, 0);
    CHECK(res.steps > 0);
    CHECK_EQ(g_escapes, 0);

    /* The kernel said so, in column zero, from the real return value. */
    CHECK_CONTAINS(kcap_text(), "[cap.call double v1 depth=0 -> 42]");
}

static void test_a_capability_survives_a_reboot(void) {
    fresh_machine();
    CHECK_EQ(save_program("triple", "returns n*3", "\"n\"",
                          "mul r0, r0, 3\nmov r2, 0\nhalt\n"), CAP_OK);

    /* Everything in RAM goes away; only the bytes in the store remain. */
    reboot_and_reload();

    const cap_t *c = capability_find("triple");
    CHECK(c != 0);
    CHECK_EQ(c ? c->version : 0, 1);
    CHECK_EQ(capability_rejected(), 0);
    CHECK_STR(c ? c->doc : "", "returns n*3");

    cap_result_t res;
    CHECK_EQ(call1("triple", 14, &res), CAP_OK);
    CHECK_EQ(res.value, 42);
    CHECK_EQ(g_escapes, 0);
}

static void test_calling_something_that_is_not_there(void) {
    fresh_machine();
    CHECK_EQ(save_program("here", "exists", "", "mov r0, 1\nhalt\n"), CAP_OK);

    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("nothere", none, 0, &res), CAP_ENOENT);
    CHECK_CONTAINS(res.msg, "no capability called \"nothere\"");
    /* And it is told what DOES exist, so the next turn can be right. */
    CHECK_CONTAINS(res.msg, "here");

    /* An empty machine says that rather than listing nothing. */
    fresh_machine();
    CHECK_EQ(capability_invoke("anything", none, 0, &res), CAP_ENOENT);
    CHECK_CONTAINS(res.msg, "none are saved");
}

static void test_arity_is_the_signature(void) {
    fresh_machine();
    CHECK_EQ(save_program("needs2", "wants two", "\"a\",\"b\"",
                          "add r0, r0, r1\nmov r2, 0\nhalt\n"), CAP_OK);
    cap_result_t res;
    uint64_t     a[3] = { 1, 2, 3 };

    CHECK_EQ(capability_invoke("needs2", a, 1, &res), CAP_EINVAL);
    CHECK_CONTAINS(res.msg, "takes 2 argument(s) and was called with 1");
    CHECK_EQ(capability_invoke("needs2", a, 3, &res), CAP_EINVAL);
    CHECK_EQ(capability_invoke("needs2", a, 2, &res), CAP_OK);
    CHECK_EQ(res.value, 3);

    /* Above the ABI's ceiling it is refused before anything is looked up. */
    uint64_t many[CAP_MAX_PARAMS + 2];
    for (int i = 0; i < CAP_MAX_PARAMS + 2; i++) many[i] = 1;
    CHECK_EQ(capability_invoke("needs2", many, CAP_MAX_PARAMS + 2, &res),
             CAP_EINVAL);
    CHECK_CONTAINS(res.msg, "at most");
}

/* ====================================================================== */
/* bad descriptions are refused, and nothing is written                    */
/* ====================================================================== */

static void test_bad_signatures_are_refused(void) {
    fresh_machine();

    /* A name that is not a name. */
    CHECK_EQ(save_program("Double", "capital letter", "", "halt\n"), CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "lowercase letter");
    CHECK_EQ(save_program("2fast", "leading digit", "", "halt\n"), CAP_EINVAL);
    CHECK_EQ(save_program("with-dash", "punctuation", "", "halt\n"), CAP_EINVAL);
    CHECK_EQ(save_program("", "empty", "", "halt\n"), CAP_EINVAL);
    CHECK_EQ(save_program("waaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaay_too_long",
                          "over the limit", "", "halt\n"), CAP_EINVAL);

    /* No doc: the doc is part of the contract, not a comment. */
    CHECK_EQ(save_raw("{\"name\":\"nodoc\",\"program\":\"halt\"}"), CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "doc");
    /* A doc with a newline in it would break the one-line panel. */
    CHECK_EQ(save_raw("{\"name\":\"twoline\",\"doc\":\"a\\nb\",\"program\":\"halt\"}"),
             CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "one line");

    /* Neither body, or both. */
    CHECK_EQ(save_raw("{\"name\":\"empty\",\"doc\":\"nothing\"}"), CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "either");
    CHECK_EQ(save_raw("{\"name\":\"both\",\"doc\":\"two bodies\","
                      "\"program\":\"halt\",\"steps\":[{\"call\":\"x\"}]}"),
             CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "not both");

    /* Too many parameters, and duplicate names. */
    CHECK_EQ(save_program("toomany", "seven params",
                          "\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\"", "halt\n"),
             CAP_EINVAL);
    CHECK_EQ(save_program("dup", "two the same", "\"a\",\"a\"", "halt\n"),
             CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "both called");

    /* A program that does not assemble is refused HERE. That is the whole
     * point: the error is the assembler's, with a line number, and it costs one
     * turn instead of one per call for ever. */
    CHECK_EQ(save_program("broken", "will not assemble", "",
                          "mov r0, 1\noutq 0x60, 1\nhalt\n"), CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "does not assemble");
    CHECK_CONTAINS(g_msg, "Line 2");
    CHECK_EQ(save_program("noop", "empty body", "", ""), CAP_EINVAL);

    /* Source with a byte the record could not hold. */
    CHECK_EQ(save_raw("{\"name\":\"binary\",\"doc\":\"has a NUL\","
                      "\"program\":\"halt\\u0000halt\"}"), CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "printable ASCII");

    /* NOTHING was saved, and the store on disk is still empty. */
    CHECK_EQ(capability_count(), 0);
    reboot_and_reload();
    CHECK_EQ(capability_count(), 0);
    CHECK_EQ(capability_rejected(), 0);
}

static void test_malformed_tool_input_cannot_hurt_anything(void) {
    fresh_machine();
    static const char *junk[] = {
        "", "{", "}", "[]", "null", "\"a string\"", "{\"name\":5}",
        "{\"name\":\"a\",\"doc\":5,\"program\":\"halt\"}",
        "{\"name\":\"a\",\"doc\":\"d\",\"params\":\"notanarray\",\"program\":\"halt\"}",
        "{\"name\":\"a\",\"doc\":\"d\",\"params\":[1,2],\"program\":\"halt\"}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":{}}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"x\",\"args\":\"no\"}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"a\",\"when\":[]}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"a\",\"args\":[\"$9\"]}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"a\",\"args\":[\"#1\"]}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"a\",\"args\":[\"99999999999999999999999\"]}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"a\",\"args\":[\"$\"]}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"a\",\"args\":[\"#x\"]}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"a\",\"when\":[\"0\",\"=\",\"0\"]}]}",
        "{\"name\":\"a\",\"doc\":\"d\",\"steps\":[{\"call\":\"a\",\"when\":[\"0\",\"==\"]}]}",
    };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        int rc = save_raw(junk[i]);
        CHECK(rc == CAP_EINVAL);
        CHECK(g_msg[0] != '\0');           /* always a sentence, never silence */
        CHECK_EQ(kpanic_hit, 0);
    }
    CHECK_EQ(capability_count(), 0);

    /* And through the real tool, which is the path model output actually takes. */
    char          buf[TOOL_RESULT_MAX];
    tool_result_t r;
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        tool_call_t call = { "id", "capability_save", junk[i], strlen(junk[i]) };
        tool_result_init(&r, buf, sizeof buf);
        CHECK_EQ(tool_dispatch(&call, &r), TOOL_OK);
        CHECK_EQ(r.is_error, 1);
        CHECK(r.len > 0);
    }
    CHECK_EQ(kpanic_hit, 0);
}

/* ====================================================================== */
/* name collisions and replacement                                        */
/* ====================================================================== */

static void test_a_second_save_is_a_version_not_a_collision(void) {
    fresh_machine();
    CHECK_EQ(save_program("grow", "version one", "\"n\"",
                          "mov r0, 1\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(capability_find("grow")->version, 1);
    CHECK_EQ(capability_find("grow")->has_backup, 0);

    CHECK_EQ(save_program("grow", "version two", "\"n\"",
                          "mov r0, 2\nmov r2, 0\nhalt\n"), CAP_OK);
    const cap_t *c = capability_find("grow");
    CHECK_EQ(c->version, 2);
    CHECK_EQ(c->has_backup, 1);
    CHECK_STR(c->doc, "version two");
    /* One name, one slot: replacing does not leak a registry entry. */
    CHECK_EQ(capability_count(), 1);

    cap_result_t res;
    CHECK_EQ(call1("grow", 0, &res), CAP_OK);
    CHECK_EQ(res.value, 2);          /* the NEW code ran, not the cached old one */

    /* And the version that ran is the version on disk. */
    reboot_and_reload();
    CHECK_EQ(capability_find("grow")->version, 2);
    CHECK_EQ(call1("grow", 0, &res), CAP_OK);
    CHECK_EQ(res.value, 2);
}

static void test_replacement_cannot_break_a_caller(void) {
    fresh_machine();
    CHECK_EQ(save_program("leaf", "takes one", "\"n\"",
                          "mov r0, r0\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(save_raw("{\"name\":\"user\",\"doc\":\"calls leaf once\","
                      "\"params\":[\"n\"],"
                      "\"steps\":[{\"call\":\"leaf\",\"args\":[\"$0\"]}]}"),
             CAP_OK);

    /* Re-shaping leaf would leave user calling it with the wrong count. */
    int rc = save_program("leaf", "now takes two", "\"a\",\"b\"",
                          "add r0, r0, r1\nmov r2, 0\nhalt\n");
    CHECK_EQ(rc, CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "called by \"user\" step 1");
    CHECK_CONTAINS(g_msg, "Nothing was changed");

    /* And nothing WAS: leaf is still v1 with one parameter, and user still runs. */
    CHECK_EQ(capability_find("leaf")->version, 1);
    CHECK_EQ(capability_find("leaf")->nparam, 1);
    cap_result_t res;
    CHECK_EQ(call1("user", 9, &res), CAP_OK);
    CHECK_EQ(res.value, 9);

    /* Same arity is fine — that is a normal improvement. */
    CHECK_EQ(save_program("leaf", "same shape, better", "\"n\"",
                          "add r0, r0, 100\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(call1("user", 9, &res), CAP_OK);
    CHECK_EQ(res.value, 109);

    /* Deleting it is refused for the same reason, naming the same caller. */
    CHECK_EQ(capability_delete("leaf", g_msg, sizeof g_msg), CAP_EEXIST);
    CHECK_CONTAINS(g_msg, "called by \"user\" step 1");
    CHECK(capability_find("leaf") != 0);

    /* Remove the caller first and it goes. */
    CHECK_EQ(capability_delete("user", g_msg, sizeof g_msg), CAP_OK);
    CHECK_EQ(capability_delete("leaf", g_msg, sizeof g_msg), CAP_OK);
    CHECK_EQ(capability_count(), 0);
    reboot_and_reload();
    CHECK_EQ(capability_count(), 0);
}

static void test_rollback_restores_the_working_version(void) {
    fresh_machine();
    CHECK_EQ(save_program("fix", "v1 works", "",
                          "mov r0, 111\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(capability_revert("fix", g_msg, sizeof g_msg), CAP_ENOENT);
    CHECK_CONTAINS(g_msg, "one level of undo");

    CHECK_EQ(save_program("fix", "v2 is wrong", "",
                          "mov r0, 222\nmov r2, 0\nhalt\n"), CAP_OK);
    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("fix", none, 0, &res), CAP_OK);
    CHECK_EQ(res.value, 222);

    /* Roll back: the CODE of v1 comes back, as version 3. No number reused. */
    CHECK_EQ(capability_revert("fix", g_msg, sizeof g_msg), CAP_OK);
    CHECK_CONTAINS(g_msg, "version 1 is now version 3");
    const cap_t *c = capability_find("fix");
    CHECK_EQ(c->version, 3);
    CHECK_STR(c->doc, "v1 works");
    CHECK_EQ(capability_invoke("fix", none, 0, &res), CAP_OK);
    CHECK_EQ(res.value, 111);

    /* Doing it again is a REDO, because the thing rolled away from became the
     * new backup. That is the property that makes one level of undo safe to use
     * without thinking about it. */
    CHECK_EQ(capability_revert("fix", g_msg, sizeof g_msg), CAP_OK);
    CHECK_EQ(capability_find("fix")->version, 4);
    CHECK_EQ(capability_invoke("fix", none, 0, &res), CAP_OK);
    CHECK_EQ(res.value, 222);

    /* Survives the reboot as version 4. */
    reboot_and_reload();
    CHECK_EQ(capability_find("fix")->version, 4);
    CHECK_EQ(capability_revert("nosuch", g_msg, sizeof g_msg), CAP_ENOENT);
}

/* ====================================================================== */
/* corruption                                                             */
/* ====================================================================== */

/* Read the whole record file, mutate one byte, write it back. */
static int poke_record(const char *name, const char *suffix, size_t off,
                       char with) {
    char path[192], buf[CAP_FILE_MAX];
    snprintf(path, sizeof path, "%s/%s%s", capability_store_root(), name, suffix);
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) return -1;
    int64_t n = vfs_read(f, buf, sizeof buf);
    vfs_close(f);
    if (n <= 0 || off >= (size_t)n) return -1;
    buf[off] = with;
    f = vfs_open(path, O_WRONLY | O_TRUNC);
    if (!f) return -1;
    vfs_write(f, buf, (uint64_t)n);
    vfs_close(f);
    return 0;
}

static size_t record_size(const char *name, const char *suffix) {
    char path[192];
    vfs_stat_t st;
    snprintf(path, sizeof path, "%s/%s%s", capability_store_root(), name, suffix);
    if (vfs_stat(path, &st) != VFS_OK) return 0;
    return (size_t)st.size;
}

static void test_a_corrupted_record_is_rejected(void) {
    fresh_machine();
    CHECK_EQ(save_program("keep", "the only version", "",
                          "mov r0, 7\nmov r2, 0\nhalt\n"), CAP_OK);
    size_t n = record_size("keep", ".cap");
    CHECK(n > 20);

    /* Damage one byte of the program source, deep inside the JSON. The checksum
     * is what notices; nothing else in the file has changed shape. */
    CHECK_EQ(poke_record("keep", ".cap", n - 6, 'Z'), 0);
    kcap_reset();
    reboot_and_reload();
    CHECK_EQ(capability_count(), 0);          /* NOT registered */
    CHECK_EQ(capability_rejected(), 1);
    CHECK_CONTAINS(kcap_text(), "REJECTED");
    CHECK_CONTAINS(kcap_text(), "checksum mismatch");
    /* And it cannot be called, rather than being called and misbehaving. */
    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("keep", none, 0, &res), CAP_ENOENT);

    /* Every byte of the hash prefix matters. */
    for (size_t i = 0; i < 17; i++) {
        fresh_machine();
        CHECK_EQ(save_program("keep", "the only version", "",
                              "mov r0, 7\nmov r2, 0\nhalt\n"), CAP_OK);
        CHECK_EQ(poke_record("keep", ".cap", i, i == 16 ? ' ' : 'q'), 0);
        reboot_and_reload();
        CHECK_EQ(capability_count(), 0);
        CHECK_EQ(capability_rejected(), 1);
    }

    /* A record whose name no longer matches its filename is corruption too:
     * the only thing that ever writes these files derives one from the other. */
    fresh_machine();
    CHECK_EQ(save_program("named", "consistent", "",
                          "mov r0, 1\nmov r2, 0\nhalt\n"), CAP_OK);
    {
        char src[192], dst[192], buf[CAP_FILE_MAX];
        snprintf(src, sizeof src, "%s/named.cap", capability_store_root());
        snprintf(dst, sizeof dst, "%s/other.cap", capability_store_root());
        file_t *f = vfs_open(src, O_RDONLY);
        int64_t got = f ? vfs_read(f, buf, sizeof buf) : -1;
        if (f) vfs_close(f);
        CHECK(got > 0);
        f = vfs_open(dst, O_CREAT | O_WRONLY | O_TRUNC);
        if (f) { vfs_write(f, buf, (uint64_t)got); vfs_close(f); }
    }
    kcap_reset();
    reboot_and_reload();
    /* "named" loads; the copy calling itself "other.cap" does not. */
    CHECK(capability_find("named") != 0);
    CHECK_EQ(capability_find("other"), 0);
    CHECK_EQ(capability_rejected(), 1);
    CHECK_CONTAINS(kcap_text(), "the two disagree");
}

static void test_a_damaged_current_version_falls_back_to_its_backup(void) {
    fresh_machine();
    CHECK_EQ(save_program("resilient", "the good one", "",
                          "mov r0, 55\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(save_program("resilient", "the new one", "",
                          "mov r0, 66\nmov r2, 0\nhalt\n"), CAP_OK);

    /* Simulate a save interrupted by power loss: the backup is intact (it is
     * written and flushed first) and the current file is garbage. */
    size_t n = record_size("resilient", ".cap");
    CHECK_EQ(poke_record("resilient", ".cap", n / 2, '!'), 0);

    kcap_reset();
    reboot_and_reload();
    CHECK_CONTAINS(kcap_text(), "recovered from its backup");
    const cap_t *c = capability_find("resilient");
    CHECK(c != 0);
    CHECK_EQ(c ? c->version : 0, 1);
    CHECK_STR(c ? c->doc : "", "the good one");

    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("resilient", none, 0, &res), CAP_OK);
    CHECK_EQ(res.value, 55);          /* the WORKING version, running */
    CHECK_EQ(capability_rejected(), 1);
}

static void test_truncated_and_empty_records(void) {
    fresh_machine();
    const char *store = capability_store_root();
    char        path[192];

    static const char *bad[] = {
        "",                                  /* empty file            */
        "\n",                                /* no hash               */
        "0123456789abcdef",                  /* hash, no newline      */
        "0123456789abcdef\n",                /* hash, no record       */
        "0123456789abcdef\n{}",              /* hash wrong, JSON thin */
        "not hex at all!!\n{\"cap\":1}",     /* prefix is not hex     */
        "0123456789abcdefX{\"cap\":1}",      /* separator is wrong    */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(path, sizeof path, "%s/bad%d.cap", store, (int)i);
        file_t *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
        CHECK(f != 0);
        if (f) {
            if (bad[i][0]) vfs_write(f, bad[i], strlen(bad[i]));
            vfs_close(f);
        }
    }
    reboot_and_reload();
    CHECK_EQ(capability_count(), 0);
    CHECK_EQ(capability_rejected(), (int)(sizeof bad / sizeof bad[0]));
    CHECK_EQ(kpanic_hit, 0);

    /* A well-formed hash over well-formed JSON that is not a capability is
     * rejected by the field checks, not by the hash. */
    fresh_machine();
}

/* ====================================================================== */
/* composition                                                            */
/* ====================================================================== */

static void test_a_capability_calls_another(void) {
    fresh_machine();
    CHECK_EQ(save_program("addten", "returns n+10", "\"n\"",
                          "add r0, r0, 10\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(save_program("shout", "returns n*100 with text", "\"n\"",
                          "mul   r9, r0, 100\n"
                          "mstr  r1, 300, \"final \"\n"
                          "mitoa r1, r1, r9, 10\n"
                          "sub   r2, r1, 300\n"
                          "mov   r1, 300\n"
                          "mov   r0, r9\n"
                          "halt\n"), CAP_OK);

    /* pipeline(n) = shout(addten(n)) — a composition wiring one result into the
     * next call's argument, which is the thing composition is for. */
    CHECK_EQ(save_raw("{\"name\":\"pipeline\",\"doc\":\"adds ten then shouts\","
                      "\"params\":[\"n\"],\"steps\":["
                      "{\"call\":\"addten\",\"args\":[\"$0\"]},"
                      "{\"call\":\"shout\",\"args\":[\"#1\"]}]}"), CAP_OK);

    cap_result_t res;
    CHECK_EQ(call1("pipeline", 5, &res), CAP_OK);
    CHECK_EQ(res.value, 1500);              /* (5+10)*100 */
    CHECK_EQ(res.invocations, 3);           /* pipeline, addten, shout */
    CHECK_EQ(res.depth_used, 1);
    CHECK_STR(res.text, "final 1500");      /* the LAST step's text */
    CHECK_CONTAINS(res.chain, "pipeline -> addten -> shout");
    CHECK_EQ(g_escapes, 0);

    /* The kernel traced every hop, from real return values. */
    CHECK_CONTAINS(kcap_text(), "[cap.call addten v1 depth=1 -> 15]");
    CHECK_CONTAINS(kcap_text(), "[cap.call shout v1 depth=1 -> 1500]");

    /* A composition that calls one that calls one: two frames deep. */
    CHECK_EQ(save_raw("{\"name\":\"outer\",\"doc\":\"wraps pipeline\","
                      "\"params\":[\"n\"],\"steps\":["
                      "{\"call\":\"pipeline\",\"args\":[\"$0\"]}]}"), CAP_OK);
    CHECK_EQ(call1("outer", 5, &res), CAP_OK);
    CHECK_EQ(res.value, 1500);
    CHECK_EQ(res.depth_used, 2);
    CHECK_EQ(res.invocations, 4);
}

static void test_a_guard_is_what_makes_a_cycle_terminate(void) {
    fresh_machine();
    CHECK_EQ(save_program("dec", "returns n-1, or 0", "\"n\"",
                          "cmp r0, 0\nbeq zero\nsub r0, r0, 1\nmov r2, 0\nhalt\n"
                          "zero: mov r0, 0\nmov r2, 0\nhalt\n"), CAP_OK);
    /* countdown(n): if n > 0, dec it; if n > 1, recurse on the decremented
     * value. BOTH guards test $0, not #1 — a guard that reads an earlier step's
     * result is only legal when that step certainly ran, and the version of
     * this test that guarded step 2 on "#1" was refused for exactly that
     * reason when n was 0. See test_a_step_cannot_use_a_result_that_was_skipped. */
    CHECK_EQ(save_raw("{\"name\":\"countdown\",\"doc\":\"recurses down to zero\","
                      "\"params\":[\"n\"],\"steps\":["
                      "{\"call\":\"dec\",\"args\":[\"$0\"],"
                      "\"when\":[\"$0\",\">\",\"0\"]},"
                      "{\"call\":\"countdown\",\"args\":[\"#1\"],"
                      "\"when\":[\"$0\",\">\",\"1\"]}]}"), CAP_OK);

    cap_result_t res;
    CHECK_EQ(call1("countdown", 3, &res), CAP_OK);
    CHECK_EQ(res.value, 0);
    CHECK_EQ(res.depth_used, 3);            /* three nested frames, then done */
    CHECK_CONTAINS(res.chain, "countdown -> dec -> countdown");

    /* Every guard false is a legal, quiet zero, not a failure. */
    CHECK_EQ(call1("countdown", 0, &res), CAP_OK);
    CHECK_EQ(res.value, 0);
    CHECK_EQ(res.invocations, 1);
    CHECK_EQ(res.text_len, 0);

    /* Deep enough to hit the depth cap, and the message carries the path. */
    CHECK_EQ(call1("countdown", 40, &res), CAP_EDEPTH);
    CHECK_CONTAINS(res.msg, "more than 8 deep");
    /* The advice survives, because the chain is a field and not part of the
     * sentence. It used to be, and at depth 8 it filled the buffer and pushed
     * the advice out. */
    CHECK_CONTAINS(res.msg, "\"when\" guard");
    CHECK_CONTAINS(res.chain, "countdown -> dec -> countdown");
    CHECK(res.depth_used <= CAP_MAX_DEPTH);
    CHECK_EQ(kpanic_hit, 0);
}

static void test_the_depth_limit_refuses_runaway_recursion(void) {
    fresh_machine();
    /* No guard at all: this composition can only ever recurse. */
    CHECK_EQ(save_raw("{\"name\":\"runaway\",\"doc\":\"calls itself for ever\","
                      "\"params\":[],\"steps\":[{\"call\":\"runaway\"}]}"),
             CAP_OK);
    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("runaway", none, 0, &res), CAP_EDEPTH);
    CHECK_CONTAINS(res.msg, "more than 8 deep");
    CHECK_EQ(res.depth_used, CAP_MAX_DEPTH);
    /* Bounded work, not "eventually": one frame per invocation and no more. */
    CHECK_EQ(res.invocations, CAP_MAX_DEPTH + 1);
    CHECK_EQ(kpanic_hit, 0);

    /* Two capabilities calling each other is the same cycle wearing a hat, and
     * it is caught by the same counter. A save cannot name a capability that
     * does not exist yet, so the pair is built by replacing the first. */
    fresh_machine();
    CHECK_EQ(save_program("ping", "placeholder", "",
                          "mov r0, 0\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(save_raw("{\"name\":\"pong\",\"doc\":\"calls ping\",\"params\":[],"
                      "\"steps\":[{\"call\":\"ping\"}]}"), CAP_OK);
    CHECK_EQ(save_raw("{\"name\":\"ping\",\"doc\":\"calls pong\",\"params\":[],"
                      "\"steps\":[{\"call\":\"pong\"}]}"), CAP_OK);
    CHECK_EQ(capability_invoke("ping", none, 0, &res), CAP_EDEPTH);
    CHECK_CONTAINS(res.chain, "ping -> pong -> ping");
    CHECK(res.invocations <= CAP_MAX_INVOCATIONS);
    CHECK_EQ(kpanic_hit, 0);
}

static void test_a_step_cannot_use_a_result_that_was_skipped(void) {
    fresh_machine();
    CHECK_EQ(save_program("one", "returns 1", "", "mov r0, 1\nmov r2, 0\nhalt\n"),
             CAP_OK);
    CHECK_EQ(save_program("id", "returns n", "\"n\"", "mov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(save_raw("{\"name\":\"skipper\",\"doc\":\"uses a skipped step\","
                      "\"params\":[\"n\"],\"steps\":["
                      "{\"call\":\"one\",\"when\":[\"$0\",\"==\",\"7\"]},"
                      "{\"call\":\"id\",\"args\":[\"#1\"]}]}"), CAP_OK);

    cap_result_t res;
    CHECK_EQ(call1("skipper", 7, &res), CAP_OK);      /* step 1 ran */
    CHECK_EQ(res.value, 1);

    /* Step 1 skipped: #1 is not zero, it is a mistake, and it says so. A silent
     * zero here would be an argument the author never wrote. */
    CHECK_EQ(call1("skipper", 8, &res), CAP_EINVAL);
    CHECK_CONTAINS(res.msg, "step 1 was skipped");
}

static void test_a_composition_can_only_call_what_exists(void) {
    fresh_machine();
    CHECK_EQ(save_raw("{\"name\":\"hopeful\",\"doc\":\"calls a typo\","
                      "\"params\":[],\"steps\":[{\"call\":\"doesnotexist\"}]}"),
             CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "does not have");
    CHECK_EQ(capability_count(), 0);

    /* Wrong arity is caught at SAVE time against the real callee. */
    CHECK_EQ(save_program("takes1", "one arg", "\"n\"", "mov r2, 0\nhalt\n"),
             CAP_OK);
    CHECK_EQ(save_raw("{\"name\":\"wrongcount\",\"doc\":\"two args for one\","
                      "\"params\":[],\"steps\":["
                      "{\"call\":\"takes1\",\"args\":[\"1\",\"2\"]}]}"),
             CAP_EINVAL);
    CHECK_CONTAINS(g_msg, "it takes 1");
}

/* A STORED COMPOSITION IS RE-CHECKED AGAINST THE REGISTRY IT LANDS IN.
 *
 * load_one() re-validates a stored PROGRAM by re-assembling it, so "it is in the
 * registry" implies "it went through the assembler on this boot". That guarantee
 * did not extend to a composition: steps_parse() never resolves the callee, and
 * both checks that matter — the callee exists, and the step's argument count
 * equals its arity — lived only in capability_save(). A record whose step called
 * something with the wrong number of arguments therefore registered cleanly,
 * appeared in capability_list and in the fixed-width discovery panel, and returned
 * CAP_EINVAL on every single call.
 *
 * NO TAMPERING BELOW, deliberately. The sequence is ordinary saves plus one
 * flipped byte, i.e. exactly the bit-rot case the checksum and the .bak fallback
 * exist for — so this is inside the declared threat model, not outside it. */
static void test_a_stored_composition_is_rechecked_against_this_registry(void) {
    fresh_machine();

    /* 1. a callee that takes nothing, and something else to point at later. */
    CHECK_EQ(save_program("good", "good(): 7", "",
                          "mov r0, 7\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(save_program("other", "other(): 9", "",
                          "mov r0, 9\nmov r2, 0\nhalt\n"), CAP_OK);

    /* 2. caller v1 calls good with no arguments — correct today. */
    CHECK_EQ(save_raw("{\"name\":\"caller\",\"doc\":\"caller(): calls good\","
                      "\"params\":[],\"steps\":[{\"call\":\"good\"}]}"), CAP_OK);

    /* 3. caller v2 calls other instead. v1 becomes caller.bak. */
    CHECK_EQ(save_raw("{\"name\":\"caller\",\"doc\":\"caller(): calls other\","
                      "\"params\":[],\"steps\":[{\"call\":\"other\"}]}"), CAP_OK);

    /* 4. good gains a parameter. PERMITTED, and that is the interesting part:
     * arity_change_breaks_callers() only sees the RAM table, where the live
     * caller no longer calls good. Nothing consults the .bak records. */
    CHECK_EQ(save_program("good", "good(n): n", "\"n\"",
                          "mov r0, r0\nmov r2, 0\nhalt\n"), CAP_OK);

    /* 5. one byte of caller.cap rots. */
    size_t n = record_size("caller", ".cap");
    CHECK(n > 20);
    CHECK_EQ(poke_record("caller", ".cap", n - 6, 'Z'), 0);

    /* 6. the next boot falls back to caller.bak — which calls good with no
     * arguments, while good now takes one. */
    kcap_reset();
    reboot_and_reload();

    /* It must NOT be registered: a capability that fails every call is worse than
     * one that is absent, because the discovery panel advertises it. */
    CHECK(capability_find("caller") == NULL);
    CHECK_CONTAINS(kcap_text(), "REJECTED");
    CHECK_CONTAINS(kcap_text(), "every call would fail");

    /* And the rest of the registry is untouched: one poisoned record, not a
     * store. Removing a slot from the middle of the array must not lose a
     * neighbour, which is why the loop below does not advance on a removal. */
    CHECK(capability_find("good") != NULL);
    CHECK(capability_find("other") != NULL);
    CHECK_EQ(capability_count(), 2);

    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("caller", none, 0, &res), CAP_ENOENT);
    CHECK_EQ(capability_invoke("other", none, 0, &res), CAP_OK);
    CHECK_EQ(res.value, 9);
}

/* The same pass must not reject a composition that is FINE, or it is not a check
 * but a ban. Records load in readdir order, so a caller is routinely read before
 * its callee — which is exactly why this is a second pass and not a line in
 * steps_parse(). */
static void test_a_valid_composition_survives_the_recheck(void) {
    fresh_machine();
    CHECK_EQ(save_program("zzlast", "zzlast(n): n+1", "\"n\"",
                          "add r0, r0, 1\nmov r2, 0\nhalt\n"), CAP_OK);
    /* "aafirst" sorts before "zzlast", so on any ordering where the directory is
     * walked alphabetically the caller is seen first. */
    CHECK_EQ(save_raw("{\"name\":\"aafirst\",\"doc\":\"aafirst(n): zzlast(n)\","
                      "\"params\":[\"n\"],\"steps\":["
                      "{\"call\":\"zzlast\",\"args\":[\"$0\"]}]}"), CAP_OK);

    kcap_reset();
    reboot_and_reload();
    CHECK_EQ(capability_count(), 2);
    CHECK(capability_find("aafirst") != NULL);
    CHECK_EQ(capability_rejected(), 0);

    cap_result_t res;
    CHECK_EQ(call1("aafirst", 41, &res), CAP_OK);
    CHECK_EQ(res.value, 42);
}

/* ====================================================================== */
/* tail calls                                                             */
/* ====================================================================== */

static void test_a_tail_call_is_a_loop_not_a_frame(void) {
    fresh_machine();
    CHECK_EQ(save_program("fact", "fact(n,acc): n! * acc, by tail call",
                          "\"n\",\"acc\"", P_FACT), CAP_OK);

    uint64_t     a[2] = { 5, 1 };
    cap_result_t res;
    CHECK_EQ(capability_invoke("fact", a, 2, &res), CAP_OK);
    CHECK_EQ(res.value, 120);
    /* Six invocations (n=5,4,3,2,1,0) and NOT ONE composition frame: a tail call
     * costs no kernel stack, which is the whole reason it is allowed to be this
     * deep at all. */
    CHECK_EQ(res.invocations, 6);
    CHECK_EQ(res.depth_used, 0);
    CHECK_CONTAINS(res.chain, "fact -> fact -> fact");
    CHECK_CONTAINS(kcap_text(), "[cap.tail fact v1 -> fact");
    CHECK_EQ(g_escapes, 0);

    a[0] = 10; a[1] = 1;
    CHECK_EQ(capability_invoke("fact", a, 2, &res), CAP_OK);
    CHECK_EQ(res.value, 3628800);
    CHECK_EQ(res.invocations, 11);
    CHECK_EQ(res.depth_used, 0);

    a[0] = 0; a[1] = 1;
    CHECK_EQ(capability_invoke("fact", a, 2, &res), CAP_OK);
    CHECK_EQ(res.value, 1);
    CHECK_EQ(res.invocations, 1);
}

static void test_an_endless_tail_chain_hits_the_invocation_budget(void) {
    fresh_machine();
    CHECK_EQ(save_program("forever", "tail-calls itself with no exit", "",
                          P_FOREVER), CAP_OK);

    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("forever", none, 0, &res), CAP_EBUDGET);
    CHECK_EQ(res.invocations, CAP_MAX_INVOCATIONS);
    CHECK_EQ(res.depth_used, 0);
    CHECK_CONTAINS(res.msg, "64 capability invocations");
    CHECK_CONTAINS(res.msg, "calls itself");
    CHECK_CONTAINS(res.msg, "\"forever\" was next");
    /* The chain is clipped rather than unbounded, and says so exactly once. */
    CHECK_CONTAINS(res.chain, " ...");
    CHECK(strlen(res.chain) < CAP_CHAIN_MAX);
    CHECK_EQ(capability_find("forever")->calls, CAP_MAX_INVOCATIONS);
    CHECK_EQ(kpanic_hit, 0);
    CHECK_EQ(capability_find("forever")->failures, 1);
}

static void test_a_malformed_tail_call_is_this_capability_s_failure(void) {
    fresh_machine();
    /* r3 says "tail call" but r5 (the name length) is zero. */
    CHECK_EQ(save_program("badtail1", "asks for a nameless tail call", "",
                          "mov r3, 0x7a11ca11\nmov r4, 0\nmov r5, 0\nhalt\n"),
             CAP_OK);
    /* A name span that runs off the end of the scratch arena. */
    CHECK_EQ(save_program("badtail2", "names a span outside the arena", "",
                          "mov r3, 0x7a11ca11\nmov r4, 65530\nmov r5, 20\nhalt\n"),
             CAP_OK);
    /* A syntactically impossible capability name. */
    CHECK_EQ(save_program("badtail3", "tail-calls rubbish", "",
                          "mstr r4, 64, \"NOT A NAME\"\n"
                          "sub  r5, r4, 64\nmov r4, 64\n"
                          "mov  r3, 0x7a11ca11\nhalt\n"), CAP_OK);
    /* A well-formed name for something that is not there. */
    CHECK_EQ(save_program("badtail4", "tail-calls a stranger", "",
                          "mstr r4, 64, \"stranger\"\n"
                          "sub  r5, r4, 64\nmov r4, 64\n"
                          "mov  r3, 0x7a11ca11\nhalt\n"), CAP_OK);

    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("badtail1", none, 0, &res), CAP_EINVAL);
    CHECK_CONTAINS(res.msg, "badtail1");
    CHECK_CONTAINS(res.msg, "r5 is the length");
    CHECK_EQ(capability_invoke("badtail2", none, 0, &res), CAP_EINVAL);
    CHECK_CONTAINS(res.msg, "outside the 65536-byte arena");
    CHECK_EQ(capability_invoke("badtail3", none, 0, &res), CAP_EINVAL);
    CHECK_CONTAINS(res.msg, "not usable");
    CHECK_EQ(capability_invoke("badtail4", none, 0, &res), CAP_ENOENT);
    CHECK_CONTAINS(res.msg, "tail-called \"stranger\"");
    CHECK_EQ(kpanic_hit, 0);

    /* A stale value in r3 that is not the magic is NOT a tail call. */
    CHECK_EQ(save_program("notail", "leaves junk in r3", "",
                          "mov r0, 5\nmov r2, 0\nmov r3, 0x7a11ca10\nhalt\n"),
             CAP_OK);
    CHECK_EQ(capability_invoke("notail", none, 0, &res), CAP_OK);
    CHECK_EQ(res.value, 5);
    CHECK_EQ(res.invocations, 1);
}

static void test_a_tail_call_can_target_a_composition(void) {
    fresh_machine();
    CHECK_EQ(save_program("addone", "n+1", "\"n\"",
                          "add r0, r0, 1\nmov r2, 0\nhalt\n"), CAP_OK);
    CHECK_EQ(save_raw("{\"name\":\"twice\",\"doc\":\"adds one twice\","
                      "\"params\":[\"n\"],\"steps\":["
                      "{\"call\":\"addone\",\"args\":[\"$0\"]},"
                      "{\"call\":\"addone\",\"args\":[\"#1\"]}]}"), CAP_OK);
    CHECK_EQ(save_program("jump", "tail-calls the composition", "\"n\"",
                          "mstr r4, 32, \"twice\"\n"
                          "sub  r5, r4, 32\nmov r4, 32\n"
                          "mov  r6, r0\nmov r3, 0x7a11ca11\nhalt\n"), CAP_OK);

    cap_result_t res;
    CHECK_EQ(call1("jump", 40, &res), CAP_OK);
    CHECK_EQ(res.value, 42);
    CHECK_EQ(res.depth_used, 1);
    CHECK_EQ(res.invocations, 4);           /* jump, twice, addone, addone */
}

/* ====================================================================== */
/* the sandbox                                                            */
/* ====================================================================== */

static void test_a_capability_gets_no_device_access(void) {
    fresh_machine();
    CHECK_EQ(save_program("portpeek", "tries to read a port", "",
                          "in8 r0, 0x60\nhalt\n"), CAP_OK);
    CHECK_EQ(save_program("mmiopeek", "tries to read memory", "",
                          "ld32 r0, [0xfebc0000]\nhalt\n"), CAP_OK);
    CHECK_EQ(save_program("cfgpeek", "tries PCI config space", "",
                          "pcicfg r0, 0, 0\nhalt\n"), CAP_OK);

    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("portpeek", none, 0, &res), CAP_ETRAP);
    CHECK_CONTAINS(res.msg, "PORT_DENIED");
    CHECK_EQ(capability_invoke("mmiopeek", none, 0, &res), CAP_ETRAP);
    CHECK_CONTAINS(res.msg, "MMIO_DENIED");
    CHECK_EQ(capability_invoke("cfgpeek", none, 0, &res), CAP_ETRAP);
    CHECK_CONTAINS(res.msg, "PCI_DENIED");
    /* Each failure names the capability and the SOURCE LINE, which is what a
     * model needs to fix it. */
    CHECK_CONTAINS(res.msg, "line 1");
    CHECK_EQ(g_escapes, 0);
}

static void test_a_capability_can_keep_a_file_but_not_rewrite_the_store(void) {
    fresh_machine();
    CHECK_EQ(save_program("note", "writes a note and reads it back", "", P_NOTE),
             CAP_OK);
    CHECK_EQ(save_program("attack", "tries to overwrite the store", "", P_ATTACK),
             CAP_OK);

    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("note", none, 0, &res), CAP_OK);
    CHECK_EQ(res.value, 4);
    CHECK_STR(res.text, "kept");
    CHECK_EQ(g_escapes, 0);

    /* THE ATTACK. fs.write into the capability store does not even come back as
     * a failure the program could branch on: vm/dvm.c TRAPS a path outside the
     * granted root, so the program is stopped rather than told "no". Note the
     * program has a `bne refused` branch that therefore never runs — which is
     * the strongest form of this result, and it is why the assertion is a trap
     * and not a zero. g_escapes proves the path never reached the synthetic
     * kernel at all, so the confinement lives in the portable half of the VM and
     * holds on the real machine too. */
    CHECK_EQ(capability_invoke("attack", none, 0, &res), CAP_ETRAP);
    CHECK_CONTAINS(res.msg, "SYS_DENIED");
    CHECK_CONTAINS(res.msg, "outside");
    CHECK_EQ(g_escapes, 0);
    /* And the store is untouched: "attack" is still callable and still v1. */
    reboot_and_reload();
    CHECK_EQ(capability_find("attack")->version, 1);
    CHECK_EQ(capability_rejected(), 0);
}

static void test_a_nested_capability_still_has_a_voice(void) {
    fresh_machine();
    CHECK_EQ(save_program("speak", "writes one line to the console", "",
                          "mstr r1, 0, \"from inside a chain\"\n"
                          "sub  r2, r1, 0\nmov r1, 0\n"
                          "push 0\npush r2\nsys r0, con.write\n"
                          "halt\n"), CAP_OK);
    CHECK_EQ(save_raw("{\"name\":\"wrapper\",\"doc\":\"calls speak\","
                      "\"params\":[],\"steps\":[{\"call\":\"speak\"}]}"), CAP_OK);

    said_reset();
    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("wrapper", none, 0, &res), CAP_OK);
    /* Nested runs do not repeat the VM's grant block — it is identical every
     * time — but con.write goes through the backend, so a capability inside a
     * chain can still be heard. */
    CHECK_STR(g_said, "from inside a chain");
    CHECK_EQ(g_escapes, 0);
}

static void test_a_runaway_program_is_stopped_by_the_step_budget(void) {
    fresh_machine();
    CHECK_EQ(save_program("spin", "loops for ever", "",
                          "top: add r1, r1, 1\njmp top\n"), CAP_OK);
    cap_result_t res;
    uint64_t     none[1] = { 0 };
    CHECK_EQ(capability_invoke("spin", none, 0, &res), CAP_ETRAP);
    CHECK_CONTAINS(res.msg, "STEP_LIMIT");
    CHECK(res.steps <= CAP_RUN_STEPS);
    CHECK_EQ(kpanic_hit, 0);
}

/* A chain of programs that each burn their whole per-run budget must be bounded
 * by the TREE budget, or 64 legal runs add up to something nobody asked for. */
static void test_the_tree_step_budget_bounds_a_whole_chain(void) {
    fresh_machine();
    /* Each invocation burns ~30000 steps and then tail-calls itself. */
    CHECK_EQ(save_program("burn", "burns steps then tail-calls itself", "",
                          ".equ TAILCALL 0x7a11ca11\n"
                          "mov  r1, 10000\n"
                          "top: sub r1, r1, 1\ncmp r1, 0\nbne top\n"
                          "mstr r4, 900, \"burn\"\n"
                          "sub  r5, r4, 900\nmov r4, 900\n"
                          "mov  r3, TAILCALL\nhalt\n"), CAP_OK);
    cap_result_t res;
    uint64_t     none[1] = { 0 };
    int rc = capability_invoke("burn", none, 0, &res);
    /* Whichever cap binds first, it binds: the point is that it STOPS, with a
     * legible reason and a bounded amount of work behind it. */
    CHECK(rc == CAP_EBUDGET);
    CHECK(res.steps <= CAP_TREE_STEPS);
    CHECK(res.invocations <= CAP_MAX_INVOCATIONS);
    CHECK_EQ(kpanic_hit, 0);
}

/* ====================================================================== */
/* the store itself                                                       */
/* ====================================================================== */

static void test_the_store_and_the_data_root_are_never_the_same_place(void) {
    const char *store = capability_store_root();
    const char *data  = capability_data_root();
    CHECK(store[0] == '/');
    CHECK(data[0]  == '/');
    CHECK(strcmp(store, data) != 0);
    /* Neither may be a prefix of the other at a component boundary, or the VM's
     * fs_root check would let a program into the store. */
    size_t sl = strlen(store), dl = strlen(data);
    CHECK(!(strncmp(store, data, dl) == 0 && (store[dl] == '/' || store[dl] == 0)));
    CHECK(!(strncmp(data, store, sl) == 0 && (data[sl]  == '/' || data[sl]  == 0)));
    /* Both exist after init: a program's first fs.write must not fail because
     * nothing made its directory. */
    vfs_stat_t st;
    CHECK_EQ(vfs_stat(store, &st), VFS_OK);
    CHECK(st.type == VNODE_DIR);
    CHECK_EQ(vfs_stat(data, &st), VFS_OK);
}

static void test_files_in_the_store_that_are_not_records_are_ignored(void) {
    fresh_machine();
    CHECK_EQ(save_program("real", "a real one", "",
                          "mov r0, 1\nmov r2, 0\nhalt\n"), CAP_OK);
    char path[192];
    const char *junk[] = { "readme.txt", "notes", ".cap", "a.capx" };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", capability_store_root(), junk[i]);
        file_t *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
        if (f) { vfs_write(f, "hello", 5); vfs_close(f); }
    }
    reboot_and_reload();
    CHECK_EQ(capability_count(), 1);
    CHECK_EQ(capability_rejected(), 0);      /* ignored, not "rejected" */
    CHECK(capability_find("real") != 0);
    fresh_machine();
}

/* ====================================================================== */
/* the tools, end to end                                                  */
/* ====================================================================== */

static char           g_buf[TOOL_RESULT_MAX];
static tool_result_t  g_res;

static int call_tool(const char *name, const char *input) {
    tool_call_t call = { "toolu_1", name, input, strlen(input) };
    tool_result_init(&g_res, g_buf, sizeof g_buf);
    return tool_dispatch(&call, &g_res);
}

static void test_the_tools_do_the_whole_job(void) {
    fresh_machine();

    CHECK_EQ(call_tool("capability_save",
        "{\"name\":\"celsius\",\"doc\":\"celsius(f): (f-32)*5/9, whole degrees\","
        "\"params\":[\"f\"],\"program\":\""
        "sub r9, r0, 32\\nmul r9, r9, 5\\ndiv r0, r9, 9\\n"
        "mstr r1, 0, \\\"celsius \\\"\\nmitoa r1, r1, r0, 10\\n"
        "sub r2, r1, 0\\nmov r1, 0\\nhalt\\n\"}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(g_buf, "version 1");
    CHECK_CONTAINS(g_buf, "signature: celsius(f)");
    CHECK_CONTAINS(g_buf, "do not write it again");
    CHECK_CONTAINS(kcap_text(), "[cap.save celsius program -> 1]");

    /* The saved capability is now visible in the tool the model would use. */
    CHECK_CONTAINS(tool_find("capability_call")->description, "celsius(f) v1");
    CHECK_CONTAINS(tool_find("capability_call")->description,
                   "celsius(f): (f-32)*5/9");

    CHECK_EQ(call_tool("capability_call",
                       "{\"name\":\"celsius\",\"args\":[212]}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(g_buf, "celsius returned 100");
    CHECK_CONTAINS(g_buf, "text: celsius 100");

    CHECK_EQ(call_tool("capability_list", "{}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(g_buf, "1 saved");
    CHECK_CONTAINS(g_buf, "celsius(f) v1");
    CHECK_CONTAINS(g_buf, capability_data_root());

    CHECK_EQ(call_tool("capability_list", "{\"name\":\"celsius\"}"), TOOL_OK);
    CHECK_CONTAINS(g_buf, "[program]");
    CHECK_CONTAINS(g_buf, "one save from being lost");
    CHECK_CONTAINS(g_buf, "calls since boot: 1");

    /* Wrong argument count, through the tool, is a legible error. */
    CHECK_EQ(call_tool("capability_call", "{\"name\":\"celsius\"}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(g_buf, "takes 1 argument(s) and was called with 0");

    CHECK_EQ(call_tool("capability_call",
                       "{\"name\":\"celsius\",\"args\":[\"hot\"]}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(g_buf, "not a whole number");

    CHECK_EQ(call_tool("capability_call", "{\"name\":\"nope\"}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(g_buf, "no capability called");

    /* Replace, then roll back, then delete — all through the tools. */
    CHECK_EQ(call_tool("capability_save",
        "{\"name\":\"celsius\",\"doc\":\"broken second attempt\","
        "\"params\":[\"f\"],\"program\":\"mov r0, 0\\nmov r2, 0\\nhalt\\n\"}"),
        TOOL_OK);
    CHECK_CONTAINS(g_buf, "version 2 replaced version 1");
    CHECK_CONTAINS(g_buf, "capability_revert brings it back");

    CHECK_EQ(call_tool("capability_revert", "{\"name\":\"celsius\"}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(g_buf, "rolled back");
    CHECK_EQ(call_tool("capability_call",
                       "{\"name\":\"celsius\",\"args\":[212]}"), TOOL_OK);
    CHECK_CONTAINS(g_buf, "returned 100");

    CHECK_EQ(call_tool("capability_revert",
                       "{\"name\":\"celsius\",\"mode\":\"delete\"}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(g_buf, "deleted");
    CHECK_EQ(capability_count(), 0);
    CHECK_EQ(call_tool("capability_call",
                       "{\"name\":\"celsius\",\"args\":[212]}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 1);

    CHECK_EQ(call_tool("capability_revert",
                       "{\"name\":\"celsius\",\"mode\":\"sideways\"}"), TOOL_OK);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(g_buf, "rollback");

    /* An empty machine's list says so rather than printing a bare zero. */
    CHECK_EQ(call_tool("capability_list", "{}"), TOOL_OK);
    CHECK_CONTAINS(g_buf, "capability_save is how this machine learns");
}

static void test_every_tool_call_leaves_exactly_one_trace_line(void) {
    fresh_machine();
    struct { const char *tool, *in; } cases[] = {
        { "capability_list",   "{}" },
        { "capability_list",   "{\"name\":\"nope\"}" },
        { "capability_save",   "{\"name\":\"t\",\"doc\":\"one\",\"program\":\"mov r0,1\\nhalt\"}" },
        { "capability_call",   "{\"name\":\"t\"}" },
        { "capability_revert", "{\"name\":\"t\"}" },
        { "capability_revert", "{\"name\":\"t\",\"mode\":\"delete\"}" },
        { "capability_save",   "{}" },
        { "capability_call",   "{}" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        kcap_reset();
        /* A capability run emits the VM's own lines too, so count only the
         * lines this file is responsible for: the tool's own. */
        uint64_t before = trace_emissions();
        CHECK_EQ(call_tool(cases[i].tool, cases[i].in), TOOL_OK);
        CHECK(trace_emissions() > before);
        /* Whatever was printed, no line may start anywhere but with '[' — the
         * column-zero invariant, on this file's paths. */
        const char *t = kcap_text();
        for (size_t k = 0; t[k]; k++)
            if (t[k] == '\n' && t[k + 1]) CHECK(t[k + 1] == '[');
        CHECK(t[0] == '\0' || t[0] == '[');
    }
}

/* Model output reaches these tools as bytes. Hammer them with generated
 * nonsense and require only two things: no panic, and a result every time. */
static uint32_t g_seed = 0x5eed1234u;
static uint32_t rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return g_seed;
}

static void test_fuzz_the_tools(void) {
    fresh_machine();
    CHECK_EQ(save_program("target", "a real capability to aim at", "\"n\"",
                          "mov r2, 0\nhalt\n"), CAP_OK);

    static const char *keys[] = { "name", "doc", "params", "program", "steps",
                                  "args", "mode", "call", "when", "", "x" };
    static const char *vals[] = { "\"target\"", "\"\"", "0", "-1", "[]", "{}",
                                  "null", "true", "[[[[]]]]", "\"$0\"", "\"#1\"",
                                  "1e9", "\"\\u0000\"", "99999999999999999999",
                                  ("\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""),
                                  "[{\"call\":\"target\",\"args\":[\"$0\"]}]" };
    static const char *tools[] = { "capability_save", "capability_call",
                                   "capability_list", "capability_revert" };
    char in[512];

    for (int iter = 0; iter < 6000; iter++) {
        int  n = (int)(rnd() % 4);
        int  p = 0;
        p += snprintf(in + p, sizeof in - (size_t)p, "{");
        for (int k = 0; k <= n && p > 0 && (size_t)p < sizeof in - 40; k++)
            p += snprintf(in + p, sizeof in - (size_t)p, "%s\"%s\":%s",
                          k ? "," : "",
                          keys[rnd() % (sizeof keys / sizeof keys[0])],
                          vals[rnd() % (sizeof vals / sizeof vals[0])]);
        if ((size_t)p < sizeof in - 2) p += snprintf(in + p, sizeof in - (size_t)p, "}");
        /* Every fourth iteration, truncate it mid-way: a real transport does. */
        if ((rnd() & 3) == 0 && p > 4) in[p / 2] = '\0';

        CHECK_EQ(call_tool(tools[rnd() % 4], in), TOOL_OK);
        CHECK(g_res.len > 0);
        CHECK_EQ(kpanic_hit, 0);
        CHECK_EQ(kfmt_violations, 0);
        /* The panel stays exactly the right width no matter what happened. */
        CHECK_EQ(strlen(capability_panel()), CAP_PANEL_CHARS);
    }
    /* Whatever the fuzz did, the store is still readable and self-consistent. */
    reboot_and_reload();
    for (int i = 0; i < capability_count(); i++) {
        const cap_t *c = capability_at(i);
        CHECK(capability_name_ok(c->name, 0, 0));
        CHECK(c->version >= 1);
        CHECK(c->nparam <= CAP_MAX_PARAMS);
        CHECK(c->kind == CAP_KIND_PROGRAM || c->kind == CAP_KIND_COMPOSE);
    }
    CHECK_EQ(g_escapes, 0);
}

/* ====================================================================== */

int main(void) {
    console_init();
    vfs_init();
    ramfs_register();
    vfs_mount("/", "ramfs");
    capability_set_sys(&FAKE_SYS);
    capability_bringup();

    RUN(test_tools_are_registered);
    RUN(test_schemas_are_valid_json);
    RUN(test_the_panel_is_a_fixed_width);
    RUN(test_the_panel_holds_no_byte_that_escaping_would_lengthen);
    RUN(test_save_then_call);
    RUN(test_a_capability_survives_a_reboot);
    RUN(test_calling_something_that_is_not_there);
    RUN(test_arity_is_the_signature);
    RUN(test_bad_signatures_are_refused);
    RUN(test_malformed_tool_input_cannot_hurt_anything);
    RUN(test_a_second_save_is_a_version_not_a_collision);
    RUN(test_replacement_cannot_break_a_caller);
    RUN(test_rollback_restores_the_working_version);
    RUN(test_a_corrupted_record_is_rejected);
    RUN(test_a_damaged_current_version_falls_back_to_its_backup);
    RUN(test_truncated_and_empty_records);
    RUN(test_a_capability_calls_another);
    RUN(test_a_guard_is_what_makes_a_cycle_terminate);
    RUN(test_the_depth_limit_refuses_runaway_recursion);
    RUN(test_a_step_cannot_use_a_result_that_was_skipped);
    RUN(test_a_composition_can_only_call_what_exists);
    RUN(test_a_stored_composition_is_rechecked_against_this_registry);
    RUN(test_a_valid_composition_survives_the_recheck);
    RUN(test_a_tail_call_is_a_loop_not_a_frame);
    RUN(test_an_endless_tail_chain_hits_the_invocation_budget);
    RUN(test_a_malformed_tail_call_is_this_capability_s_failure);
    RUN(test_a_tail_call_can_target_a_composition);
    RUN(test_a_capability_gets_no_device_access);
    RUN(test_a_capability_can_keep_a_file_but_not_rewrite_the_store);
    RUN(test_a_nested_capability_still_has_a_voice);
    RUN(test_a_runaway_program_is_stopped_by_the_step_budget);
    RUN(test_the_tree_step_budget_bounds_a_whole_chain);
    RUN(test_the_store_and_the_data_root_are_never_the_same_place);
    RUN(test_files_in_the_store_that_are_not_records_are_ignored);
    RUN(test_the_tools_do_the_whole_job);
    RUN(test_every_tool_call_leaves_exactly_one_trace_line);
    RUN(test_fuzz_the_tools);

    return th_report("capability");
}
