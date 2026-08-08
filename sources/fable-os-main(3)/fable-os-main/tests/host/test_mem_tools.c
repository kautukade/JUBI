/* test_mem_tools.c — the memory / system-state tool family, on the host.
 *
 * Runs the REAL tools/mem_tools.c against the REAL core/tool.c, mm/heap.c,
 * net/json.c and lib/trace.c. Nothing here is a mock, so what the tests observe
 * is what the model will observe.
 *
 * Three things this suite is specifically built to prove:
 *
 *   1. The numbers are measured, not narrated. Allocate a known amount, call
 *      heap_stats, and require the reported figures to move by that amount.
 *
 *   2. The mem_read bound holds against hostile input. Every result buffer is
 *      canary-guarded on both sides, the readable window is aimed at a small
 *      guarded region, and the tool is fed absurd addresses, negative and huge
 *      lengths, wrong types, truncated JSON and fuzz. A dereference outside the
 *      window would smash a canary or segfault the test process — on the real
 *      machine it would reset the VM, so this is the cheap place to find out.
 *
 *   3. The advertised schema is real. tools_build_schema() must contain every
 *      tool here and must parse back cleanly through json.h.
 *
 * Standing in for the linker: Mach-O has no __start_/__stop_ section symbols, so
 * mem_tools.c exports a named pointer per tool under FABLEOS_HOSTTEST and this
 * file assembles the table core/tool.c expects.
 */

#include "harness.h"
#include "tool.h"
#include "trace.h"
#include "json.h"
#include "heap.h"

#include <stdlib.h>

/* ====================================================================== */
/* stand in for the linker-collected tool table                          */
/* ====================================================================== */

extern const tool_t *const fableos_hosttool_heap_stats_tool;
extern const tool_t *const fableos_hosttool_heap_check_tool;
extern const tool_t *const fableos_hosttool_sys_uptime_tool;
extern const tool_t *const fableos_hosttool_mem_map_tool;
extern const tool_t *const fableos_hosttool_mem_read_tool;

/* Deliberately defined WITHOUT the inner const that core/tool.c's extern
 * declaration carries. In the kernel this table is emitted by the linker and is
 * genuinely read-only; here it has to be filled in at startup, and a const
 * definition would land in macOS's read-only __DATA_CONST. Same symbol, same
 * layout, same size — only the writability differs. */
#if defined(__APPLE__)
#  define SYMPFX "_"
/* Force real (non-zerofill) storage so the assembler can take its address in
 * the .set below; a common/BSS symbol cannot be used in that expression. */
#  define TBL_SECTION __attribute__((section("__DATA,__data")))
#else
#  define SYMPFX ""
#  define TBL_SECTION
#endif

const tool_t *__start_tool_table[5] TBL_SECTION = { 0, 0, 0, 0, 0 };
extern const tool_t *const __stop_tool_table[];

/* __stop must be exactly one-past-the-end of __start; only the assembler can
 * express "this symbol equals that symbol plus N bytes". */
__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 40\n");
_Static_assert(sizeof(__start_tool_table) == 40,
               "tool count changed: update the __stop_tool_table byte offset");

static void install_tool_table(void) {
    __start_tool_table[0] = fableos_hosttool_heap_stats_tool;
    __start_tool_table[1] = fableos_hosttool_heap_check_tool;
    __start_tool_table[2] = fableos_hosttool_sys_uptime_tool;
    __start_tool_table[3] = fableos_hosttool_mem_map_tool;
    __start_tool_table[4] = fableos_hosttool_mem_read_tool;
}

/* ---- non-static internals of mem_tools.c under test ---- */
const char *mem_tools_stats_verdict(const heap_stats_t *s);
void        mem_tools_set_window(const void *base, size_t len);

/* ====================================================================== */
/* canary-guarded dispatch helper                                        */
/* ====================================================================== */

#define GUARD      64
#define GUARD_BYTE 0xA5

static char        g_store[GUARD + TOOL_RESULT_MAX + GUARD];
static char       *g_out = g_store + GUARD;
static tool_result_t g_res;

static void guards_reset(void) {
    memset(g_store, GUARD_BYTE, sizeof g_store);
}

static int guards_intact(size_t cap) {
    for (size_t i = 0; i < GUARD; i++)
        if ((unsigned char)g_store[i] != GUARD_BYTE) return 0;
    /* Everything past the declared capacity must also be untouched. */
    for (size_t i = GUARD + cap; i < sizeof g_store; i++)
        if ((unsigned char)g_store[i] != GUARD_BYTE) return 0;
    return 1;
}

/* Dispatch `name` with a raw (deliberately NOT NUL-terminated) input span. */
static const char *call_n(const char *name, const char *input, size_t input_len,
                          size_t cap) {
    guards_reset();
    tool_result_init(&g_res, g_out, cap);

    /* Copy the span somewhere with no terminator after it, so a tool that runs
     * off the end of input_len reads a guard byte rather than a lucky NUL. */
    static char span[8192];
    memset(span, 0x5A, sizeof span);
    if (input && input_len) {
        if (input_len > sizeof span) input_len = sizeof span;
        memcpy(span, input, input_len);
    }

    tool_call_t c;
    c.id        = "toolu_test";
    c.name      = name;
    c.input     = (input && input_len) ? span : input;
    c.input_len = input_len;

    tool_dispatch(&c, &g_res);

    CHECK(guards_intact(cap));
    CHECK(g_res.len < cap);
    CHECK(g_out[g_res.len] == '\0');
    return g_out;
}

static const char *call_tool(const char *name, const char *input) {
    return call_n(name, input, input ? strlen(input) : 0, TOOL_RESULT_MAX);
}

/* Pull the first unsigned decimal that follows `label` out of a report. */
static unsigned long field(const char *text, const char *label) {
    const char *p = strstr(text, label);
    if (!p) return (unsigned long)-1;
    p += strlen(label);
    while (*p && (*p < '0' || *p > '9')) {
        if (*p == '\n') return (unsigned long)-1;   /* label had no number */
        p++;
    }
    if (*p < '0' || *p > '9') return (unsigned long)-1;
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned long)(*p++ - '0');
    return v;
}

/* ====================================================================== */
/* 1. registration and schema                                            */
/* ====================================================================== */

static const char *const kTools[] = {
    "heap_stats", "heap_check", "sys_uptime", "mem_map", "mem_read"
};

static void test_tools_are_registered(void) {
    CHECK_EQ(tool_count(), 5);
    for (size_t i = 0; i < sizeof kTools / sizeof kTools[0]; i++) {
        const tool_t *t = tool_find(kTools[i]);
        CHECK(t != NULL);
        if (!t) continue;
        CHECK_STR(t->name, kTools[i]);
        CHECK(t->description != NULL && strlen(t->description) > 40);
        CHECK(t->input_schema != NULL);
        CHECK(t->invoke != NULL);
    }
    /* heap_check can halt the machine, so it carries the policy flag. */
    CHECK_TOOL_FLAGS("heap_check", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("heap_stats", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("mem_read", TOOL_MUTATES, 0);
}

static void test_schema_is_valid_json(void) {
    static char schema[16384];
    size_t need = tools_build_schema(schema, sizeof schema);
    CHECK(need > 0);
    CHECK(need < sizeof schema);        /* not truncated */

    json_value_t root;
    CHECK_EQ(json_parse(schema, need, &root), JSON_OK);
    CHECK_EQ(root.type, JSON_ARRAY);
    CHECK_EQ((int)json_count(&root), tool_count());

    /* Every entry must carry the three fields the Messages API requires, and
     * each input_schema must itself be a valid JSON object. */
    int seen = 0;
    for (size_t i = 0; i < json_count(&root); i++) {
        json_value_t e, v;
        CHECK_EQ(json_at(&root, i, &e), JSON_OK);
        CHECK_EQ(e.type, JSON_OBJECT);

        char name[TOOL_NAME_MAX];
        CHECK_EQ(json_get_str(&e, "name", name, sizeof name), JSON_OK);
        for (size_t k = 0; k < sizeof kTools / sizeof kTools[0]; k++)
            if (strcmp(name, kTools[k]) == 0) seen++;

        CHECK_EQ(json_get(&e, "description", &v), JSON_OK);
        CHECK_EQ(v.type, JSON_STRING);

        CHECK_EQ(json_get(&e, "input_schema", &v), JSON_OK);
        CHECK_EQ(v.type, JSON_OBJECT);

        json_value_t sub;
        CHECK_EQ(json_parse(v.start, v.len, &sub), JSON_OK);
        CHECK_EQ(sub.type, JSON_OBJECT);
        json_value_t ty;
        CHECK_EQ(json_get(&sub, "type", &ty), JSON_OK);
        CHECK(json_str_eq(&ty, "object"));
        CHECK_EQ(json_get(&sub, "properties", &ty), JSON_OK);
        CHECK_EQ(ty.type, JSON_OBJECT);
    }
    CHECK_EQ(seen, 5);

    /* mem_read must advertise its cap so the model can respect it. */
    CHECK_CONTAINS(schema, "\"maximum\":256");
    CHECK_CONTAINS(schema, "\"required\":[\"address\"]");

    /* A capacity too small for the schema must stay NUL-terminated in bounds
     * and must not write past the end (guard bytes either side). */
    static char tiny[8 + 16];
    memset(tiny, 0xA5, sizeof tiny);
    tools_build_schema(tiny, 8);
    CHECK(strlen(tiny) < 8);
    for (size_t i = 8; i < sizeof tiny; i++) CHECK_EQ((unsigned char)tiny[i], 0xA5);
}

/* ====================================================================== */
/* 2. heap_stats reports measurements, not guesses                       */
/* ====================================================================== */

static void test_heap_stats_matches_allocator(void) {
    heap_stats_t s;
    heap_stats(&s);

    const char *out = call_tool("heap_stats", "{}");
    CHECK_EQ(g_res.is_error, 0);

    CHECK_EQ(field(out, "heap arena:"), (unsigned long)s.arena_size);
    CHECK_EQ(field(out, "\nin use:"),   (unsigned long)s.bytes_in_use);
    CHECK_EQ(field(out, "\nfree:"),     (unsigned long)s.bytes_free);
    CHECK_EQ(field(out, "peak in use:"),(unsigned long)s.peak_in_use);
    CHECK_EQ(field(out, "\nblocks:"),   (unsigned long)s.used_blocks);
    CHECK_EQ(field(out, "\nlifetime:"), (unsigned long)s.total_allocs);
    CHECK_CONTAINS(out, "counters self-consistent");
}

/* The load-bearing test: allocate a known amount and require the reported
 * numbers to move by it. This is the difference between reporting and guessing. */
static void test_heap_stats_tracks_real_allocations(void) {
    const char *out = call_tool("heap_stats", "{}");
    unsigned long in_use0 = field(out, "\nin use:");
    unsigned long live0   = field(out, "\nblocks:");
    unsigned long alloc0  = field(out, "\nlifetime:");

    enum { N = 8, SZ = 4096 };
    void *p[N];
    for (int i = 0; i < N; i++) {
        p[i] = kmalloc(SZ);
        CHECK(p[i] != NULL);
    }

    out = call_tool("heap_stats", "{}");
    unsigned long in_use1 = field(out, "\nin use:");
    unsigned long live1   = field(out, "\nblocks:");
    unsigned long alloc1  = field(out, "\nlifetime:");

    /* Exactly N more live blocks, exactly N more lifetime allocations, and at
     * least N*SZ more bytes in use (the allocator rounds up, never down). */
    CHECK_EQ(live1,  live0 + N);
    CHECK_EQ(alloc1, alloc0 + N);
    CHECK(in_use1 >= in_use0 + (unsigned long)(N * SZ));
    CHECK(in_use1 <  in_use0 + (unsigned long)(N * SZ) + 1024);

    /* peak must have kept up. */
    CHECK(field(out, "peak in use:") >= in_use1);

    for (int i = 0; i < N; i++) kfree(p[i]);

    out = call_tool("heap_stats", "{}");
    /* Coalescing must give every byte back. */
    CHECK_EQ(field(out, "\nin use:"), in_use0);
    CHECK_EQ(field(out, "\nblocks:"), live0);
    CHECK_EQ(field(out, "\nlifetime:"), alloc0 + N);

    /* The three totals must still add up to the arena, in the model's own view. */
    unsigned long arena = field(out, "heap arena:");
    unsigned long used  = field(out, "\nin use:");
    unsigned long freeb = field(out, "\nfree:");
    unsigned long over  = field(out, "\noverhead:");
    CHECK_EQ(used + freeb + over, arena);
}

static void test_heap_stats_ignores_its_input(void) {
    /* Four no-argument tools must be unbreakable by argument garbage. */
    static const char *junk[] = {
        "", "{}", "null", "[1,2,3]", "{\"address\":\"0xdeadbeef\"}",
        "not json at all", "{\"a\":", "\xff\xfe\x00\x01", "{\"length\":-99999}"
    };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        const char *out = call_tool("heap_stats", junk[i]);
        CHECK_EQ(g_res.is_error, 0);
        CHECK_CONTAINS(out, "heap arena:");

        out = call_tool("sys_uptime", junk[i]);
        CHECK_EQ(g_res.is_error, 0);
        CHECK_CONTAINS(out, "uptime:");

        out = call_tool("mem_map", junk[i]);
        CHECK_EQ(g_res.is_error, 0);
        CHECK_CONTAINS(out, "mem_read window:");
    }
}

/* ====================================================================== */
/* 3. the non-panicking consistency verdict                              */
/* ====================================================================== */

static heap_stats_t sane_stats(void) {
    heap_stats_t s;
    s.arena_size   = 16u * 1024u * 1024u;
    s.used_blocks  = 29;
    s.free_blocks  = 2;
    s.overhead     = (s.used_blocks + s.free_blocks) * 32;
    s.bytes_in_use = 2312;
    s.bytes_free   = s.arena_size - s.bytes_in_use - s.overhead;
    s.peak_in_use  = 47104;
    s.total_allocs = 15840;
    s.total_frees  = 15811;      /* 15840 - 15811 == 29 live */
    return s;
}

static void test_stats_verdict(void) {
    heap_stats_t s = sane_stats();
    CHECK(mem_tools_stats_verdict(&s) == NULL);
    CHECK_STR(mem_tools_stats_verdict(NULL), "null-stats");

    /* Each invariant, broken one at a time. */
    s = sane_stats(); s.arena_size = 0;
    CHECK_STR(mem_tools_stats_verdict(&s), "arena-size-zero");

    s = sane_stats(); s.bytes_in_use = s.arena_size + 1;
    CHECK_STR(mem_tools_stats_verdict(&s), "in-use-exceeds-arena");

    s = sane_stats(); s.bytes_free = s.arena_size + 1;
    CHECK_STR(mem_tools_stats_verdict(&s), "free-exceeds-arena");

    s = sane_stats(); s.overhead = s.arena_size + 1;
    CHECK_STR(mem_tools_stats_verdict(&s), "overhead-exceeds-arena");

    s = sane_stats(); s.bytes_free -= 16;
    CHECK_STR(mem_tools_stats_verdict(&s), "size-accounting");

    s = sane_stats(); s.peak_in_use = s.bytes_in_use - 1;
    CHECK_STR(mem_tools_stats_verdict(&s), "peak-below-current");

    s = sane_stats(); s.total_frees = s.total_allocs + 1;
    CHECK_STR(mem_tools_stats_verdict(&s), "more-frees-than-allocs");

    s = sane_stats(); s.total_frees -= 1;   /* now allocs-frees == 30, live 29 */
    CHECK_STR(mem_tools_stats_verdict(&s), "live-blocks-vs-alloc-delta");

    /* free bytes with no free block to hold them */
    s = sane_stats();
    s.free_blocks = 0;
    s.overhead    = s.used_blocks * 32;
    s.bytes_free  = s.arena_size - s.bytes_in_use - s.overhead;
    CHECK_STR(mem_tools_stats_verdict(&s), "free-bytes-without-free-block");

    /* header overhead too small for the number of blocks */
    s = sane_stats();
    s.used_blocks  = 1000;
    s.total_allocs = 15840;
    s.total_frees  = 15840 - 1000;
    s.bytes_free   = s.arena_size - s.bytes_in_use - s.overhead;
    CHECK_STR(mem_tools_stats_verdict(&s), "overhead-too-small-for-block-count");

    /* Real, live stats must always pass. */
    heap_stats_t live;
    heap_stats(&live);
    CHECK(mem_tools_stats_verdict(&live) == NULL);
}

/* ====================================================================== */
/* 4. heap_check                                                         */
/* ====================================================================== */

static void test_heap_check_clean(void) {
    kpanic_hit = 0;
    kcap_reset();

    const char *out = call_tool("heap_check", "{}");
    CHECK_EQ(g_res.is_error, 0);
    CHECK_EQ(kpanic_hit, 0);                 /* a clean heap must never panic */
    CHECK_CONTAINS(out, "heap intact");
    CHECK_CONTAINS(out, "block header magic is valid");

    /* The breadcrumb printed *before* the walk is what makes a panic
     * attributable when there is no shell to ask afterwards. */
    CHECK_CONTAINS(kcap_text(), "heap_check: walking");
    CHECK_CONTAINS(kcap_text(), "panics and halts if corrupt");

    /* The NUMBERS, not just the words around them. The fragment assertions
     * above were the only ones here, and they never looked between the words -
     * so they stayed green while the breadcrumb was written with "%lu", which
     * lib/base.c's kprintf does not understand, and printed the literal text
     * "walking %lu blocks of a %lu KiB arena" on real hardware.
     *
     * READ THIS BEFORE TRUSTING IT: this assertion does NOT catch that bug, and
     * no assertion in this file can. tests/host/kshim.c implements kprintf by
     * delegating to the HOST libc vsnprintf, which is a strict SUPERSET of the
     * kernel's "%s %c %d %u %x %p %%" grammar. "%lu" therefore renders perfectly
     * here and wrongly on the machine, and this check passes either way (I
     * verified that: reverting tools/mem_tools.c to %lu leaves this suite green
     * and puts the literal format string back in a real serial log).
     *
     * So what this buys is narrower than it looks: it pins the two values
     * against the allocator, which would catch them being wrong or transposed.
     * The general defect - that a host console assertion is evidence about the
     * host formatter, not about the kernel's - is only closable by making
     * kshim's kprintf reimplement the kernel's restricted grammar instead of
     * delegating, which is a change to shared test infrastructure that every
     * suite's console assertions depend on. Documented, not attempted. */
    {
        heap_stats_t hs;
        heap_stats(&hs);
        char want[128];
        snprintf(want, sizeof want, "heap_check: walking %u blocks of a %u KiB arena",
                 (unsigned)(hs.used_blocks + hs.free_blocks),
                 (unsigned)(hs.arena_size / 1024));
        CHECK_CONTAINS(kcap_text(), want);
    }
    /* ...and the trace line comes after, from the real result. */
    CHECK_CONTAINS(kcap_text(), "[heap_check blocks=");
    CHECK_CONTAINS(kcap_text(), "-> ok]");

    /* Counts in the prose must match the allocator. */
    heap_stats_t s;
    heap_stats(&s);
    CHECK_EQ(field(out, "Walked"), (unsigned long)(s.used_blocks + s.free_blocks));
}

/* Corruption really does reach panic(). On the host panic() is captured, so we
 * can prove the path exists, then repair the damage immediately. On the real
 * machine this same path halts — which is exactly what the tool's description
 * tells the model and what the pre-flight exists to avoid stumbling into. */
static void test_heap_check_corruption_reaches_panic(void) {
    /* Two allocations, and the SECOND is the one corrupted. The magic word is
     * found by scanning back from the payload rather than by hardcoding
     * mm/heap.c's private header size — but a scan back from the first block in
     * the arena reads below the arena entirely, which ASan reports as a
     * global-buffer-overflow on mm/heap.c's default_arena. `pad` guarantees
     * there is a block in front of p, so every byte the scan touches is inside
     * the arena; zeroing its payload guarantees the first match found going
     * backwards is p's own header and not a coincidence in someone's data. */
    void *pad = kmalloc(64);
    void *p   = kmalloc(64);
    CHECK(pad != NULL);
    CHECK(p != NULL);
    if (!pad || !p) { kfree(pad); kfree(p); return; }
    CHECK((char *)p > (char *)pad);
    memset(pad, 0, 64);

    const uint32_t magic = 0x424C4B21u;      /* "BLK!" */
    uint32_t      *slot  = NULL;
    for (int back = 4; back <= 64; back += 4) {
        uint32_t *w = (uint32_t *)((char *)p - back);
        if (*w == magic) { slot = w; break; }   /* nearest match = p's header */
    }
    CHECK(slot != NULL);
    if (!slot) { kfree(p); kfree(pad); return; }

    uint32_t saved = *slot;
    *slot          = 0xDEADBEEFu;
    kpanic_hit     = 0;
    kpanic_msg     = NULL;

    call_tool("heap_check", "{}");

    CHECK_EQ(kpanic_hit, 1);
    CHECK(kpanic_msg != NULL);

    *slot      = saved;                      /* repair before anything else */
    kpanic_hit = 0;
    kpanic_msg = NULL;
    kfree(p);
    kfree(pad);

    CHECK_EQ(heap_check(), 0);               /* the arena is healthy again */
    CHECK_EQ(kpanic_hit, 0);
}

/* ====================================================================== */
/* 5. sys_uptime and mem_map                                             */
/* ====================================================================== */

static void test_sys_uptime(void) {
    kcap_reset();
    const char *out = call_tool("sys_uptime", "{}");
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "uptime:");
    CHECK_CONTAINS(out, "ms since time_init");
    CHECK_CONTAINS(kcap_text(), "[sys_uptime up=");

    /* Must be a real clock: the reported value has to be monotonic. */
    unsigned long a = field(out, "(");
    out = call_tool("sys_uptime", "{}");
    unsigned long b = field(out, "(");
    CHECK(b >= a);
}

static void test_mem_map_describes_the_window(void) {
    static unsigned char region[512];
    mem_tools_set_window(region, sizeof region);

    kcap_reset();
    const char *out = call_tool("mem_map", "{}");
    CHECK_EQ(g_res.is_error, 0);

    char lo[40];
    snprintf(lo, sizeof lo, "0x%llx", (unsigned long long)(uintptr_t)region);
    CHECK_CONTAINS(out, lo);
    CHECK_CONTAINS(out, "mem_read window:");
    CHECK_CONTAINS(out, "max 256 bytes per call");
    CHECK_CONTAINS(out, "heap arena:");
    CHECK_CONTAINS(kcap_text(), "[mem_map window=");
}

/* ====================================================================== */
/* 6. mem_read: the bound                                                */
/* ====================================================================== */

/* A guarded region the window is aimed at. Anything the tool reads must come
 * from `body`; the fences must never be touched or reported. */
#define BODY_LEN 512
static struct {
    unsigned char front[256];
    unsigned char body[BODY_LEN];
    unsigned char back[256];
} rgn;

static void region_init(void) {
    memset(rgn.front, 0xF1, sizeof rgn.front);
    memset(rgn.back,  0xB2, sizeof rgn.back);
    for (int i = 0; i < BODY_LEN; i++) rgn.body[i] = (unsigned char)i;
    memcpy(rgn.body + 32, "fable-os", sizeof "fable-os" - 1);
    mem_tools_set_window(rgn.body, BODY_LEN);
}

static int region_fences_ok(void) {
    for (size_t i = 0; i < sizeof rgn.front; i++)
        if (rgn.front[i] != 0xF1) return 0;
    for (size_t i = 0; i < sizeof rgn.back; i++)
        if (rgn.back[i] != 0xB2) return 0;
    return 1;
}

/* Build {"address":"0x...","length":N} for the body offset `off`. */
static const char *body_call(size_t off, long len, int with_len) {
    static char in[128];
    unsigned long long a = (unsigned long long)(uintptr_t)(rgn.body + off);
    if (with_len)
        snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":%ld}", a, len);
    else
        snprintf(in, sizeof in, "{\"address\":\"0x%llx\"}", a);
    return call_tool("mem_read", in);
}

static void test_mem_read_happy_path(void) {
    region_init();
    kcap_reset();

    const char *out = body_call(0, 16, 1);
    CHECK_EQ(g_res.is_error, 0);
    CHECK(region_fences_ok());

    /* Address gutter, computed independently by the host libc. */
    char gutter[40];
    snprintf(gutter, sizeof gutter, "0x%016llx",
             (unsigned long long)(uintptr_t)rgn.body);
    CHECK_CONTAINS(out, gutter);

    /* Exact bytes and the ASCII column. */
    CHECK_CONTAINS(out, "00 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f");
    CHECK_CONTAINS(out, "|................|");
    CHECK_CONTAINS(out, "16 bytes at 0x");
    CHECK_CONTAINS(kcap_text(), "[mem_read addr=0x");
    CHECK_CONTAINS(kcap_text(), "len=16 -> 16]");

    /* Printable bytes must survive into the ASCII column. */
    out = body_call(32, 16, 1);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "|fable-os");
    CHECK_CONTAINS(out, "66 61 62 6c 65 2d 6f 73");   /* "fable-os" */

    /* Default length is 64 and is exercised when "length" is omitted. */
    out = body_call(0, 0, 0);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "64 bytes at 0x");

    /* Exactly the last byte of the window is readable. */
    out = body_call(BODY_LEN - 1, 1, 1);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "1 bytes at 0x");

    /* A full-size read at the very end of the window. */
    out = body_call(BODY_LEN - 256, 256, 1);
    CHECK_EQ(g_res.is_error, 0);
    CHECK(region_fences_ok());

    /* Decimal addresses work too. */
    char in[128];
    snprintf(in, sizeof in, "{\"address\":\"%llu\",\"length\":4}",
             (unsigned long long)(uintptr_t)rgn.body);
    out = call_tool("mem_read", in);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "00 01 02 03");

    /* ...and so does a bare JSON integer. */
    snprintf(in, sizeof in, "{\"address\":%llu,\"length\":4}",
             (unsigned long long)(uintptr_t)rgn.body);
    out = call_tool("mem_read", in);
    CHECK_EQ(g_res.is_error, 0);
    CHECK_CONTAINS(out, "00 01 02 03");

    CHECK(region_fences_ok());
}

static void expect_refused(const char *input, const char *because) {
    const char *out = call_tool("mem_read", input);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(out, "mem_read refused");
    CHECK_CONTAINS(out, because);
    /* The refusal must always restate the accepted shape and the live window. */
    CHECK_CONTAINS(out, "readable window");
    CHECK(region_fences_ok());
}

static void test_mem_read_rejects_out_of_window(void) {
    region_init();

    char in[160];
    unsigned long long lo = (unsigned long long)(uintptr_t)rgn.body;
    unsigned long long hi = lo + BODY_LEN;

    /* One byte below the window. */
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":1}", lo - 1);
    expect_refused(in, "outside the readable window");

    /* The first byte past the window. */
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":1}", hi);
    expect_refused(in, "outside the readable window");

    /* Starts inside, ends outside — the off-by-one that matters. */
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":2}", hi - 1);
    expect_refused(in, "outside the readable window");

    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":256}", hi - 255);
    expect_refused(in, "outside the readable window");

    /* Classic bad addresses: NULL, low memory, the VGA framebuffer, an e1000
     * BAR, the top of the canonical range, and a non-canonical address. */
    static const char *const nasty[] = {
        "0x0", "0x1", "0xb8000", "0xa0000", "0xfebc0000", "0xfee00000",
        "0xffffffff", "0x100000000", "0x7fffffffffff", "0x800000000000",
        "0xffffffffffffffff", "0xdeadbeef", "0xcafebabe0000",
    };
    for (size_t i = 0; i < sizeof nasty / sizeof nasty[0]; i++) {
        snprintf(in, sizeof in, "{\"address\":\"%s\",\"length\":64}", nasty[i]);
        expect_refused(in, "outside the readable window");
    }

    /* An address just inside the region but with a length that would run into
     * the back fence must still be refused, and the fence must be pristine. */
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":200}", hi - 100);
    expect_refused(in, "outside the readable window");
    CHECK(region_fences_ok());
}

static void test_mem_read_rejects_bad_arguments(void) {
    region_init();
    char in[160];
    unsigned long long lo = (unsigned long long)(uintptr_t)rgn.body;

    call_tool("mem_read", NULL);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(g_out, "no arguments given");

    expect_refused("", "no arguments given");
    expect_refused("   ", "not valid JSON");
    expect_refused("{", "not valid JSON");
    expect_refused("{\"address\":", "not valid JSON");
    expect_refused("{\"address\":\"0x100000\"", "not valid JSON");
    expect_refused("{\"address\":\"0x100000\",}", "not valid JSON");
    expect_refused("[1,2,3]", "not a JSON object");
    expect_refused("\"a string\"", "not a JSON object");
    expect_refused("null", "not a JSON object");
    expect_refused("1234", "not a JSON object");
    expect_refused("{}", "missing required field");
    expect_refused("{\"length\":16}", "missing required field");
    expect_refused("{\"addr\":\"0x1000\"}", "missing required field");

    expect_refused("{\"address\":true}", "must be a string or an integer");
    expect_refused("{\"address\":null}", "must be a string or an integer");
    expect_refused("{\"address\":{}}", "must be a string or an integer");
    expect_refused("{\"address\":[]}", "must be a string or an integer");
    expect_refused("{\"address\":-1}", "is negative");
    expect_refused("{\"address\":-9223372036854775807}", "is negative");

    expect_refused("{\"address\":\"\"}", "not a valid number");
    expect_refused("{\"address\":\"0x\"}", "not a valid number");
    expect_refused("{\"address\":\"zzz\"}", "not a valid number");
    expect_refused("{\"address\":\"0x100000 bytes\"}", "not a valid number");
    expect_refused("{\"address\":\"0x100000;rm -rf /\"}", "not a valid number");
    expect_refused("{\"address\":\"-0x100000\"}", "not a valid number");
    expect_refused("{\"address\":\"0xg00d\"}", "not a valid number");
    /* 17 hex digits: overflows uint64 and must be rejected, never wrapped. */
    expect_refused("{\"address\":\"0x1ffffffffffffffff\"}", "not a valid number");
    expect_refused("{\"address\":\"99999999999999999999999999\"}",
                   "not a valid number");
    /* Longer than the address buffer. */
    {
        char big[600];
        size_t n = 0;
        n += (size_t)snprintf(big + n, sizeof big - n, "{\"address\":\"0x");
        for (int i = 0; i < 400; i++) big[n++] = '1';
        snprintf(big + n, sizeof big - n, "\"}");
        expect_refused(big, "too long");
    }

    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":0}", lo);
    expect_refused(in, "at least 1");
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":-1}", lo);
    expect_refused(in, "at least 1");
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":-2147483648}", lo);
    expect_refused(in, "at least 1");
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":257}", lo);
    expect_refused(in, "256-byte per-call cap");
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":4294967296}", lo);
    expect_refused(in, "256-byte per-call cap");
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":\"64\"}", lo);
    expect_refused(in, "must be an integer");
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":true}", lo);
    expect_refused(in, "must be an integer");
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":[64]}", lo);
    expect_refused(in, "must be an integer");
    /* Numbers json.h refuses to represent (exponent form, overflow). */
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":1e3}", lo);
    expect_refused(in, "not a representable integer");
    snprintf(in, sizeof in,
             "{\"address\":\"0x%llx\",\"length\":999999999999999999999}", lo);
    expect_refused(in, "not a representable integer");

    CHECK(region_fences_ok());
}

/* An empty window (the host default before mem_tools_set_window) must refuse
 * everything — the fail-closed default matters, because a kernel where the
 * linker symbols went missing must read nothing rather than read anything. */
static void test_mem_read_empty_window_refuses_everything(void) {
    mem_tools_set_window(NULL, 0);
    expect_refused("{\"address\":\"0x100000\",\"length\":1}",
                   "outside the readable window");
    expect_refused("{\"address\":0}", "outside the readable window");
    region_init();
}

static void test_mem_read_truncates_legibly(void) {
    region_init();
    char in[128];
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":256}",
             (unsigned long long)(uintptr_t)rgn.body);

    /* A capacity that fits only a few rows: the tool must stop early and say so,
     * rather than handing back a prefix the model would treat as complete. */
    for (size_t cap = 200; cap <= 700; cap += 50) {
        const char *out = call_n("mem_read", in, strlen(in), cap);
        CHECK(strlen(out) < cap);
        CHECK(region_fences_ok());
        CHECK(strstr(out, "TRUNCATED") != NULL || strstr(out, "truncated") != NULL);
        if (strstr(out, "[TRUNCATED:")) {
            /* It must say how much it showed and where to resume. */
            CHECK_CONTAINS(out, "of the 256 bytes requested");
            CHECK_CONTAINS(out, "Call mem_read again with address 0x");
            unsigned long shown = field(out, "[TRUNCATED: showed");
            CHECK(shown < 256);
            CHECK(shown % 16 == 0);
        }
    }

    /* Pathologically small buffers must not write out of bounds. */
    for (size_t cap = 1; cap <= 64; cap++) {
        const char *out = call_n("mem_read", in, strlen(in), cap);
        CHECK(strlen(out) < cap);
        CHECK(region_fences_ok());
    }

    /* Same for the fixed-size reports. */
    for (size_t cap = 1; cap <= 80; cap++) {
        call_n("heap_stats", "{}", 2, cap);
        call_n("mem_map",    "{}", 2, cap);
        call_n("sys_uptime", "{}", 2, cap);
        CHECK(g_res.len < cap);
    }
    /* At a cap that truncates but leaves room, the notice must be present. */
    const char *out = call_n("heap_stats", "{}", 2, 64);
    CHECK_CONTAINS(out, "truncated");
}

/* ====================================================================== */
/* 7. fuzz                                                               */
/* ====================================================================== */

static uint64_t rng_state = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

/* Random JSON-shaped and non-JSON input. Nothing may panic, smash a canary,
 * touch a fence, or leave the result unterminated. */
static void test_fuzz_inputs(void) {
    region_init();
    kpanic_hit = 0;

    static const char *frag[] = {
        "{", "}", "[", "]", "\"address\"", ":", ",", "\"length\"",
        "0x100000", "-1", "1e309", "null", "true", "\"\"", "\\u0000",
        "0xffffffffffffffff", "999999999999999999999999", "\"0x\"",
        "\n", " ", "\t", "\xc3\x28", "\x7f", "%s", "%n", "0",
    };
    char buf[512];

    for (int iter = 0; iter < 4000; iter++) {
        size_t n     = 0;
        int    parts = (int)(rnd() % 12);
        for (int i = 0; i < parts && n < sizeof buf - 8; i++) {
            const char *f = frag[rnd() % (sizeof frag / sizeof frag[0])];
            size_t      l = strlen(f);
            if (n + l >= sizeof buf) break;
            memcpy(buf + n, f, l);
            n += l;
        }
        /* Sometimes append raw bytes with no terminator at all. */
        if (rnd() & 1) {
            int extra = (int)(rnd() % 16);
            for (int i = 0; i < extra && n < sizeof buf; i++)
                buf[n++] = (char)(rnd() & 0xFF);
        }

        const char *name = kTools[rnd() % 5];
        size_t      cap  = 32 + (rnd() % (TOOL_RESULT_MAX - 32));
        call_n(name, buf, n, cap);

        CHECK_EQ(kpanic_hit, 0);
        CHECK(region_fences_ok());
        if (kpanic_hit) { kpanic_hit = 0; break; }
    }

    /* The heap must be untouched by 4000 rounds of garbage. */
    CHECK_EQ(heap_check(), 0);
    CHECK_EQ(kpanic_hit, 0);
}

/* Sweep every valid (offset, length) pair against the window and confirm the
 * accept/refuse decision is exactly the mathematical one. */
static void test_bounds_sweep(void) {
    region_init();
    unsigned long long lo = (unsigned long long)(uintptr_t)rgn.body;
    char in[160];
    int accepted = 0, refused = 0;

    for (long off = -8; off <= BODY_LEN + 8; off += 1) {
        for (long len = 0; len <= 300; len += 37) {
            snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":%ld}",
                     (unsigned long long)((long long)lo + off), len);
            call_tool("mem_read", in);

            int should_pass = (len >= 1 && len <= 256 &&
                               off >= 0 && off < BODY_LEN &&
                               len <= BODY_LEN - off);
            CHECK_EQ(g_res.is_error, should_pass ? 0 : 1);
            if (should_pass) accepted++; else refused++;
            CHECK(region_fences_ok());
        }
    }
    CHECK(accepted > 100);
    CHECK(refused  > 100);
}

/* ====================================================================== */
/* 8. dispatch-level behaviour                                           */
/* ====================================================================== */

static void test_every_call_emits_a_trace_line(void) {
    region_init();
    char in[128];
    snprintf(in, sizeof in, "{\"address\":\"0x%llx\",\"length\":16}",
             (unsigned long long)(uintptr_t)rgn.body);

    struct { const char *name; const char *input; const char *needle; } cases[] = {
        { "heap_stats", "{}", "[heap_stats in_use=" },
        { "heap_check", "{}", "[heap_check blocks=" },
        { "sys_uptime", "{}", "[sys_uptime up="     },
        { "mem_map",    "{}", "[mem_map window="    },
        { "mem_read",   in,   "[mem_read addr="     },
        { "mem_read",   "{}", "[mem_read missing required field \"address\" "
                              "-> EINVAL]" },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t before = trace_count();
        kcap_reset();
        call_tool(cases[i].name, cases[i].input);
        CHECK_EQ(trace_count(), before + 1);      /* exactly one kernel action */
        CHECK_CONTAINS(kcap_text(), cases[i].needle);
    }
}

static void test_unknown_tool_is_recoverable(void) {
    guards_reset();
    tool_result_init(&g_res, g_out, TOOL_RESULT_MAX);
    tool_call_t c = { "id", "mem_reed", "{}", 2 };
    CHECK_EQ(tool_dispatch(&c, &g_res), TOOL_ENOENT);
    CHECK_EQ(g_res.is_error, 1);
    CHECK_CONTAINS(g_out, "no such tool");
    CHECK_CONTAINS(g_out, "mem_read");            /* the real name is offered */
    CHECK(guards_intact(TOOL_RESULT_MAX));
}

int main(void) {
    console_init();
    install_tool_table();

    RUN(test_tools_are_registered);
    RUN(test_schema_is_valid_json);
    RUN(test_heap_stats_matches_allocator);
    RUN(test_heap_stats_tracks_real_allocations);
    RUN(test_heap_stats_ignores_its_input);
    RUN(test_stats_verdict);
    RUN(test_heap_check_clean);
    RUN(test_heap_check_corruption_reaches_panic);
    RUN(test_sys_uptime);
    RUN(test_mem_map_describes_the_window);
    RUN(test_mem_read_happy_path);
    RUN(test_mem_read_rejects_out_of_window);
    RUN(test_mem_read_rejects_bad_arguments);
    RUN(test_mem_read_empty_window_refuses_everything);
    RUN(test_mem_read_truncates_legibly);
    RUN(test_fuzz_inputs);
    RUN(test_bounds_sweep);
    RUN(test_every_call_emits_a_trace_line);
    RUN(test_unknown_tool_is_recoverable);

    return th_report("mem_tools");
}
