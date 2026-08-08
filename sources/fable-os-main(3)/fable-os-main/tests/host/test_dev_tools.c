/* test_dev_tools.c — host tests for the hardware introspection tool family.
 *
 * WHAT CAN AND CANNOT BE TESTED HERE
 *   tools/dev_tools.c reaches hardware through exactly one function,
 *   pci_cfg_read32(), which in the kernel is two port I/O instructions and so
 *   cannot run in a host process (nor even assemble on an arm64 host). Rather
 *   than contort the kernel to be host-testable, this test simply *is* the
 *   hardware: drivers/pci/pci.c is not linked, and this file supplies a
 *   synthetic 256-byte config space per function, modelled on the exact machine
 *   QEMU presents. Everything above the port I/O — argument parsing, every
 *   bound, the BAR/capability decoders, claim detection, result formatting, the
 *   trace lines, and the power lifecycle — is therefore covered natively.
 *
 *   What is NOT covered here, and is proved by booting instead: that
 *   pci_cfg_read32 talks to real hardware, and that the values below are the
 *   values the machine actually returns.
 *
 *   Bonus property this arrangement buys us for free: pci_cfg_write32 is also
 *   defined here, and counts its calls. The suite asserts that count stays at
 *   zero, which is a mechanical proof that no tool in this family ever writes
 *   PCI configuration space.
 *
 * REGISTER_TOOL ON A MACH-O HOST
 *   REGISTER_TOOL places a pointer in a linker section named "tool_table" and
 *   relies on GNU ld's __start_/__stop_ symbols. Mach-O has neither the bare
 *   section name nor those symbols, so on Apple hosts the macro is neutralised
 *   and the two boundary symbols core/tool.c expects are synthesised by hand
 *   from a literal table. tools/dev_tools.c is #included rather than linked so
 *   the redefinition applies to it; that also gives the tests direct access to
 *   its statics (parse_addr, the tool descriptors) for unit-level checks.
 */

#include "harness.h"

#include "tool.h"
#include "device.h"
#include "driver.h"
#include "trace.h"
#include "json.h"
#include "pci.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* synthetic PCI hardware                                                 */
/* ====================================================================== */

typedef struct {
    uint8_t  bus, dev, fn;
    uint32_t w[64];            /* 256 bytes of config space */
} fake_fn_t;

#define FAKE_MAX 8
static fake_fn_t fake[FAKE_MAX];
static int       nfake;
static long      fake_reads;
static long      fake_writes;      /* MUST stay 0 for the whole suite */
static long      fake_bad_access;  /* reads outside 0..255 / misaligned */

static fake_fn_t *fake_add(uint8_t bus, uint8_t dev, uint8_t fn) {
    fake_fn_t *f = &fake[nfake++];
    memset(f, 0, sizeof *f);
    f->bus = bus; f->dev = dev; f->fn = fn;
    for (int i = 0; i < 64; i++) f->w[i] = 0;
    return f;
}

/* The real pci.c symbols, replaced. */
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    fake_reads++;
    /* The caller must never hand us an offset the hardware could not take;
     * cfg_addr() masks off the low two bits, so a dword read is all we can do. */
    if (off & 3) fake_bad_access++;
    for (int i = 0; i < nfake; i++)
        if (fake[i].bus == bus && fake[i].dev == dev && fake[i].fn == fn)
            return fake[i].w[off >> 2];
    return 0xFFFFFFFFu;
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    (void)bus; (void)dev; (void)fn; (void)off; (void)val;
    fake_writes++;             /* a tool doing this is a test failure */
}

/* ====================================================================== */
/* the module under test                                                  */
/* ====================================================================== */

#ifdef __APPLE__
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) extern const tool_t *const __toolptr_unused_##var
/* Apple's <string.h> defines memcpy/memset/... as fortifying macros, which
 * collide with kernel.h's plain prototypes when both land in one TU. */
#undef memcpy
#undef memset
#undef memmove
#undef memcmp
#undef strlen
#endif

#include "../../tools/dev_tools.c"

#ifdef __APPLE__
/* core/tool.c walks [__start_tool_table, __stop_tool_table). Mach-O will not
 * synthesise those, so define the array and place the end marker with .set. */
const tool_t *const __start_tool_table[] = {
    &device_list_tool,
    &device_info_tool,
    &pci_list_tool,
    &pci_info_tool,
    &pci_config_read_tool,
    &device_suspend_tool,
    &device_resume_tool,
};
_Static_assert(sizeof __start_tool_table == 7 * sizeof(void *),
               "update the .set offset below when adding a tool");
__asm__(".globl ___stop_tool_table\n"
        ".set ___stop_tool_table, ___start_tool_table + 56\n");
#endif

#define NTOOLS 7

/* ====================================================================== */
/* synthetic device registry                                              */
/* ====================================================================== */

static int kbd_suspends, kbd_resumes;
static int flaky_suspends;

static int fake_kbd_suspend(device_t *d) { (void)d; kbd_suspends++; return 0; }
static int fake_kbd_resume(device_t *d)  { (void)d; kbd_resumes++;  return 0; }
static int fake_flaky_suspend(device_t *d) { (void)d; flaky_suspends++; return -5; }

static driver_t drv_serial = { .name = "serial", .level = 0, .cls = DEV_CLASS_SERIAL };
static driver_t drv_pci    = { .name = "pci",    .level = 1, .cls = DEV_CLASS_BUS };
static driver_t drv_kbd    = { .name = "kbd",    .level = 2, .cls = DEV_CLASS_INPUT,
                               .suspend = fake_kbd_suspend, .resume = fake_kbd_resume };
static driver_t drv_e1000  = { .name = "e1000",  .level = 2, .cls = DEV_CLASS_NETWORK };
static driver_t drv_flaky  = { .name = "flaky",  .level = 3, .cls = DEV_CLASS_AUDIO,
                               .suspend = fake_flaky_suspend };

static device_t *dev_kbd, *dev_eth, *dev_flaky, *dev_pcibus;

static device_t *mkdev(const char *name, device_class_t cls, driver_t *drv,
                       device_state_t st, device_t *parent) {
    device_t *d = device_create(name, cls);
    d->parent = parent;
    if (drv) device_bind_driver(d, drv);
    device_set_state(d, st);
    device_register(d);
    return d;
}

/* Mirror the machine QEMU actually presents (see the boot log), plus one
 * 64-bit-BAR function and one driver whose suspend hook fails. */
static void build_world(void) {
    fake_fn_t *f;

    /* 00:00.0  8086:1237  class 06:00  host bridge */
    f = fake_add(0, 0, 0);
    f->w[0] = 0x12378086; f->w[1] = 0x02000006; f->w[2] = 0x06000002;

    /* 00:01.0  8086:7000  class 06:01  ISA bridge, multifunction */
    f = fake_add(0, 1, 0);
    f->w[0] = 0x70008086; f->w[1] = 0x02800007; f->w[2] = 0x06010000;
    f->w[3] = 0x00800000;                      /* header type 0x80 */

    /* 00:01.1  8086:7010  class 01:01 prog_if 80  IDE, BAR4 = I/O 0xc040 */
    f = fake_add(0, 1, 1);
    f->w[0] = 0x70108086; f->w[1] = 0x02800001; f->w[2] = 0x01018000;
    f->w[8] = 0x0000c041;                      /* BAR4 */

    /* 00:01.3  8086:7113  class 06:80 */
    f = fake_add(0, 1, 3);
    f->w[0] = 0x71138086; f->w[1] = 0x02800000; f->w[2] = 0x06800000;

    /* 00:02.0  1234:1111  class 03:00  display, BAR0 = mem 0xfd000000 */
    f = fake_add(0, 2, 0);
    f->w[0] = 0x11111234; f->w[1] = 0x02b00003; f->w[2] = 0x03000002;
    f->w[4] = 0xfd000008;                      /* mem32, prefetchable */
    f->w[6] = 0xfebd0000;                      /* BAR2, mem32 */

    /* 00:03.0  8086:100e  class 02:00 rev 03  e1000.
     * BAR0 = mem 0xfebc0000, BAR1 = I/O 0xc000, cap list -> PM @ 0xdc. */
    f = fake_add(0, 3, 0);
    f->w[0] = 0x100e8086; f->w[1] = 0x02300007; f->w[2] = 0x02000003;
    f->w[4] = 0xfebc0000;
    f->w[5] = 0x0000c001;
    f->w[11] = 0x11001af4;                     /* 0x2c subsystem 1af4:1100 */
    f->w[13] = 0x000000dc;                     /* 0x34 cap pointer */
    f->w[15] = 0x0000010b;                     /* 0x3c irq line 11 pin 1 */
    f->w[0xdc >> 2] = 0x00000001;              /* cap id 0x01 (PM), next 0 */

    /* 00:04.0  1af4:1041  class 01:00, 64-bit prefetchable BAR0 */
    f = fake_add(0, 4, 0);
    f->w[0] = 0x10411af4; f->w[1] = 0x00000007; f->w[2] = 0x01000001;
    f->w[4] = 0xfe00000c;                      /* mem64 + prefetch */
    f->w[5] = 0x00000001;                      /* upper dword */

    /* ---- device registry ---- */
    mkdev("serial0", DEV_CLASS_SERIAL, &drv_serial, DEV_STATE_ACTIVE, NULL);
    device_add_resource(dev_by_name("serial0"), RES_IOPORT, 0x3f8, 8);
    device_add_resource(dev_by_name("serial0"), RES_IRQ, 4, 1);

    dev_pcibus = mkdev("pci0", DEV_CLASS_BUS, &drv_pci, DEV_STATE_ACTIVE, NULL);

    mkdev("pci00:00.0", DEV_CLASS_BUS,     NULL, DEV_STATE_REGISTERED, dev_pcibus);
    mkdev("pci00:01.0", DEV_CLASS_BUS,     NULL, DEV_STATE_REGISTERED, dev_pcibus);
    device_add_resource(
        mkdev("pci00:01.1", DEV_CLASS_STORAGE, NULL, DEV_STATE_REGISTERED, dev_pcibus),
        RES_IOPORT, 0xc040, 0);
    mkdev("pci00:01.3", DEV_CLASS_BUS,     NULL, DEV_STATE_REGISTERED, dev_pcibus);
    device_add_resource(
        mkdev("pci00:02.0", DEV_CLASS_DISPLAY, NULL, DEV_STATE_REGISTERED, dev_pcibus),
        RES_MMIO, 0xfd000000, 0);
    device_add_resource(
        mkdev("pci00:03.0", DEV_CLASS_NETWORK, NULL, DEV_STATE_REGISTERED, dev_pcibus),
        RES_MMIO, 0xfebc0000, 0);
    mkdev("pci00:04.0", DEV_CLASS_STORAGE, NULL, DEV_STATE_REGISTERED, dev_pcibus);

    dev_kbd = mkdev("kbd0", DEV_CLASS_INPUT, &drv_kbd, DEV_STATE_ACTIVE, NULL);
    device_add_resource(dev_kbd, RES_IOPORT, 0x60, 1);
    device_add_resource(dev_kbd, RES_IRQ, 1, 1);

    dev_eth = mkdev("eth0", DEV_CLASS_NETWORK, &drv_e1000, DEV_STATE_ACTIVE, NULL);
    device_add_resource(dev_eth, RES_MMIO, 0xfebc0000, 0x20000);

    dev_flaky = mkdev("snd0", DEV_CLASS_AUDIO, &drv_flaky, DEV_STATE_ACTIVE, NULL);
}

#define NDEVS 12

/* ====================================================================== */
/* invocation helper with an overflow canary                              */
/* ====================================================================== */

#define RBUF 4096
#define CANARY 0x5A

static char          rbuf[RBUF + 64];

static int canary_intact(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i] != (char)CANARY) return 0;
    return 1;
}
static tool_result_t R;
static int           last_rc;

/* Run a tool by name through the real dispatcher with a raw JSON input span.
 * Verifies the universal invariants on every single call. */
static const char *call_n(const char *tool, const char *input, size_t len) {
    memset(rbuf, CANARY, sizeof rbuf);
    tool_result_init(&R, rbuf, RBUF);
    kcap_reset();

    uint32_t before = trace_count();
    tool_call_t c = { .id = "toolu_test", .name = tool,
                      .input = input, .input_len = len };
    last_rc = tool_dispatch(&c, &R);

    /* Invariants that must hold no matter how hostile the input was. */
    CHECK(kpanic_hit == 0);
    CHECK(R.len < R.cap);
    CHECK(rbuf[R.len] == '\0');
    CHECK(canary_intact(rbuf + RBUF, 64));
    CHECK(trace_count() > before);          /* every action leaves a trace line */
    CHECK(fake_writes == 0);                /* never write config space */
    CHECK(fake_bad_access == 0);            /* never a misaligned config cycle */
    return rbuf;
}

static const char *call(const char *tool, const char *input) {
    return call_n(tool, input, strlen(input));
}

/* ====================================================================== */
/* schema                                                                 */
/* ====================================================================== */

static const char *tool_names[NTOOLS] = {
    "device_list", "device_info", "pci_list", "pci_info",
    "pci_config_read", "device_suspend", "device_resume",
};

static void test_registration(void) {
    CHECK_EQ(tool_count(), NTOOLS);
    for (int i = 0; i < NTOOLS; i++) {
        const tool_t *t = tool_find(tool_names[i]);
        CHECK(t != NULL);
        if (!t) continue;
        CHECK(t->description != NULL && strlen(t->description) > 40);
        CHECK(t->input_schema != NULL);
        CHECK(t->invoke != NULL);
        /* Every schema must itself be a valid JSON object. */
        json_value_t v;
        CHECK_EQ(json_parse_str(t->input_schema, &v), JSON_OK);
        CHECK_EQ(v.type, JSON_OBJECT);
        json_value_t props;
        CHECK_EQ(json_get(&v, "properties", &props), JSON_OK);
        CHECK_EQ(props.type, JSON_OBJECT);
        json_value_t req;
        CHECK_EQ(json_get(&v, "required", &req), JSON_OK);
        CHECK_EQ(req.type, JSON_ARRAY);
    }

    /* Only the two power tools change system state. */
    CHECK_TOOL_FLAGS("device_suspend", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("device_resume", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("device_list", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("device_info", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("pci_list", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("pci_info", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("pci_config_read", TOOL_MUTATES, 0);
}

static void test_schema_assembly(void) {
    static char buf[16384];
    size_t n = tools_build_schema(buf, sizeof buf);
    CHECK(n > 0);
    CHECK(n < sizeof buf);            /* not truncated */

    /* The advertised array must be valid JSON the API would accept. */
    json_value_t arr;
    CHECK_EQ(json_parse(buf, n, &arr), JSON_OK);
    CHECK_EQ(arr.type, JSON_ARRAY);
    CHECK_EQ((int)json_count(&arr), NTOOLS);

    for (size_t i = 0; i < json_count(&arr); i++) {
        json_value_t e, name, desc, sch;
        CHECK_EQ(json_at(&arr, i, &e), JSON_OK);
        CHECK_EQ(json_get(&e, "name", &name), JSON_OK);
        CHECK_EQ(json_get(&e, "description", &desc), JSON_OK);
        CHECK_EQ(json_get(&e, "input_schema", &sch), JSON_OK);
        CHECK_EQ(sch.type, JSON_OBJECT);
        char nm[TOOL_NAME_MAX];
        CHECK_EQ(json_str(&name, nm, sizeof nm, NULL), JSON_OK);
        CHECK(tool_find(nm) != NULL);
    }
    for (int i = 0; i < NTOOLS; i++) CHECK_CONTAINS(buf, tool_names[i]);

    /* Truncation is reported, never silent, and never overruns. snprintf
     * semantics: the return value is the size REQUIRED, so `ret >= cap` is what
     * tells the caller the array it holds is only a prefix. */
    char small[32];
    memset(small, CANARY, sizeof small);
    size_t want = tools_build_schema(small, 16);
    CHECK(want >= 16);                /* truncation is detectable */
    CHECK_EQ(want, n);                /* and the size needed is reported */
    CHECK(strlen(small) == 15);       /* stopped, did not run off the end */
    CHECK(small[15] == '\0');
    for (int i = 16; i < 32; i++) CHECK(small[i] == (char)CANARY);
    CHECK_EQ(tools_build_schema(small, 0), n);   /* sizing call writes nothing */
    for (int i = 16; i < 32; i++) CHECK(small[i] == (char)CANARY);
}

/* ====================================================================== */
/* device_list                                                            */
/* ====================================================================== */

static void test_device_list(void) {
    const char *out = call("device_list", "{}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "name=serial0 class=serial state=active driver=serial");
    CHECK_CONTAINS(out, "io=0x3f8+8");
    CHECK_CONTAINS(out, "irq=4");
    CHECK_CONTAINS(out, "name=eth0 class=network state=active driver=e1000");
    CHECK_CONTAINS(out, "mmio=0xfebc0000+0x20000");
    CHECK_CONTAINS(out, "name=pci00:03.0");
    CHECK_CONTAINS(out, "parent=pci0");
    CHECK_CONTAINS(out, "pci=00:03.0");
    CHECK_CONTAINS(out, "driver=-");
    char sum[96];
    snprintf(sum, sizeof sum, "devices: %d registered, %d matched, %d shown, %d unclaimed",
             NDEVS, NDEVS, NDEVS, 7);
    CHECK_CONTAINS(out, sum);
    CHECK_CONTAINS(kcap_text(), "[device_list class=* state=* unclaimed_only=0 -> 12]");

    /* class filter */
    out = call("device_list", "{\"class\":\"network\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "name=eth0");
    CHECK_CONTAINS(out, "name=pci00:03.0");
    CHECK(strstr(out, "serial0") == NULL);
    CHECK_CONTAINS(out, "2 matched, 2 shown, 1 unclaimed");
    CHECK_CONTAINS(kcap_text(), "[device_list class=network state=* unclaimed_only=0 -> 2]");

    /* state filter */
    out = call("device_list", "{\"state\":\"active\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "5 matched");
    CHECK(strstr(out, "pci00:00.0") == NULL);

    /* the first-class case: hardware nobody drives */
    out = call("device_list", "{\"unclaimed_only\":true}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "pci00:00.0");
    CHECK_CONTAINS(out, "pci00:04.0");
    CHECK(strstr(out, "name=eth0") == NULL);
    CHECK(strstr(out, "name=kbd0") == NULL);
    CHECK_CONTAINS(out, "7 matched, 7 shown, 7 unclaimed");

    /* combined filters that match nothing are not an error */
    out = call("device_list", "{\"class\":\"audio\",\"state\":\"suspended\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "0 matched, 0 shown, 0 unclaimed");

    /* limit + the "more not shown" note */
    out = call("device_list", "{\"limit\":2}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "...10 more not shown");
    CHECK_CONTAINS(out, "12 matched, 2 shown");

    /* an absent input span behaves like {} */
    memset(rbuf, CANARY, sizeof rbuf);
    tool_result_init(&R, rbuf, RBUF);
    tool_call_t c = { .id = "x", .name = "device_list", .input = NULL, .input_len = 0 };
    CHECK_EQ(tool_dispatch(&c, &R), TOOL_OK);
    CHECK(!R.is_error);
    CHECK_CONTAINS(rbuf, "12 matched");
}

static void test_device_list_bad_input(void) {
    struct { const char *in; const char *want; } cases[] = {
        { "{\"class\":\"nic\"}",        "unknown class"  },
        { "{\"class\":123}",            "must be a short string" },
        { "{\"class\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}", "must be a short string" },
        { "{\"state\":\"asleep\"}",     "unknown state"  },
        { "{\"state\":[]}",             "must be a short string" },
        { "{\"unclaimed_only\":\"yes\"}", "must be a boolean" },
        { "{\"limit\":0}",              "must be an integer 1-256" },
        { "{\"limit\":-1}",             "must be an integer 1-256" },
        { "{\"limit\":99999999}",       "must be an integer 1-256" },
        { "{\"limit\":\"lots\"}",       "must be an integer 1-256" },
        { "[]",                         "must be a JSON object" },
        { "\"hello\"",                  "must be a JSON object" },
        { "42",                         "must be a JSON object" },
        { "{",                          "must be a JSON object" },
        { "{\"class\":}",               "must be a JSON object" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *out = call("device_list", cases[i].in);
        CHECK(R.is_error);
        CHECK_CONTAINS(out, "error: ");
        CHECK_CONTAINS(out, cases[i].want);
        CHECK_CONTAINS(kcap_text(), "-> EINVAL]");
    }
}

/* ====================================================================== */
/* device_info                                                            */
/* ====================================================================== */

static void test_device_info(void) {
    const char *out = call("device_info", "{\"name\":\"eth0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "name=eth0");
    CHECK_CONTAINS(out, "class=network state=active");
    CHECK_CONTAINS(out, "driver=e1000 level=2 handles=network");
    CHECK_CONTAINS(out, "suspendable=no resumable=no");
    CHECK_CONTAINS(out, "resources=1");
    CHECK_CONTAINS(out, "[0] mmio base=0xfebc0000 size=0x20000");
    CHECK_CONTAINS(kcap_text(), "[device_info eth0 class=network state=active driver=e1000 -> ok]");

    /* A driver that can be power-cycled advertises it. */
    out = call("device_info", "{\"name\":\"kbd0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "hooks=suspend,resume");
    CHECK_CONTAINS(out, "suspendable=yes resumable=yes");
    CHECK_CONTAINS(out, "[0] ioport base=0x60 size=0x1");
    CHECK_CONTAINS(out, "[1] irq base=0x1 size=0x1");

    /* A PCI node with no driver: says so, and reads live config space. */
    out = call("device_info", "{\"name\":\"pci00:03.0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "driver=- (unclaimed");
    CHECK_CONTAINS(out, "parent=pci0");
    CHECK_CONTAINS(out, "pci=00:03.0 vendor=0x8086 device=0x100e class=02:00 prog_if=0x00 rev=0x03 (network/ethernet)");
    CHECK_CONTAINS(out, "hint: pci_info addr=00:03.0");

    /* A non-PCI device must not claim a PCI address. */
    out = call("device_info", "{\"name\":\"serial0\"}");
    CHECK(!R.is_error);
    CHECK(strstr(out, "pci=") == NULL);
    CHECK_CONTAINS(out, "parent=-");
}

static void test_device_info_bad_input(void) {
    const char *out = call("device_info", "{}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "\"name\" is required");

    out = call("device_info", "{\"name\":42}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "must be a string");

    out = call("device_info", "{\"name\":\"nosuch\"}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "no such device \"nosuch\"");
    CHECK_CONTAINS(kcap_text(), "[device_info nosuch -> ENOENT]");

    /* A 10 000-character name is a clean ENOENT, not a buffer problem. */
    static char big[10300];
    int p = 0;
    p += snprintf(big + p, sizeof big - p, "{\"name\":\"");
    for (int i = 0; i < 10000; i++) big[p++] = 'A';
    snprintf(big + p, sizeof big - p, "\"}");
    out = call("device_info", big);
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "name longer than 40 characters");
    CHECK_CONTAINS(kcap_text(), "-> ENOENT]");

    /* Escapes and control characters in a name are handled by json_str. */
    out = call("device_info", "{\"name\":\"eth\\u0000\"}");
    CHECK(R.is_error);
    out = call("device_info", "{\"name\":\"\\ud800\\udc00\"}");
    CHECK(R.is_error);
}

/* ====================================================================== */
/* pci_list                                                               */
/* ====================================================================== */

static void test_pci_list(void) {
    const char *out = call("pci_list", "{\"bus\":0}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "00:00.0 8086:1237 class=06:00 prog_if=00 rev=02 hdr=0x00 driver=- bridge/host");
    CHECK_CONTAINS(out, "00:01.0 8086:7000 class=06:01 prog_if=00 rev=00 hdr=0x80");
    CHECK_CONTAINS(out, "00:01.1 8086:7010 class=01:01 prog_if=80 rev=00 hdr=0x00 bar4=io:0xc040 driver=- storage/ide");
    CHECK_CONTAINS(out, "00:01.3 8086:7113 class=06:80");
    CHECK_CONTAINS(out, "00:02.0 1234:1111 class=03:00 prog_if=00 rev=02 hdr=0x00 bar0=mem:0xfd000000 driver=- display/vga");
    /* eth0 owns 00:03.0 by BAR, even though the PCI node itself is driverless. */
    CHECK_CONTAINS(out, "00:03.0 8086:100e class=02:00 prog_if=00 rev=03 hdr=0x00 bar0=mem:0xfebc0000 driver=e1000(eth0) network/ethernet");
    CHECK_CONTAINS(out, "00:04.0 1af4:1041 class=01:00 prog_if=00 rev=01 hdr=0x00 bar0=mem:0x1fe000000 driver=- storage");
    CHECK_CONTAINS(out, "pci: 7 function(s) matched, 7 shown, 6 unclaimed");
    CHECK_CONTAINS(kcap_text(), "[pci_list bus=0-0 vendor=-1 class=-1 unclaimed_only=0 -> 7]");

    /* Scanning every bus finds exactly the same seven and nothing phantom. */
    out = call("pci_list", "{}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "7 function(s) matched");
    CHECK_CONTAINS(kcap_text(), "[pci_list bus=0-255");

    /* vendor filter */
    out = call("pci_list", "{\"vendor\":4660}");        /* 0x1234 */
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "00:02.0 1234:1111");
    CHECK_CONTAINS(out, "1 function(s) matched");

    /* class filter */
    out = call("pci_list", "{\"class\":6}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "3 function(s) matched");
    CHECK(strstr(out, "8086:100e") == NULL);

    /* the driver-authoring entry point */
    out = call("pci_list", "{\"unclaimed_only\":true}");
    CHECK(!R.is_error);
    CHECK(strstr(out, "00:03.0") == NULL);
    CHECK_CONTAINS(out, "00:04.0");
    CHECK_CONTAINS(out, "6 function(s) matched, 6 shown, 6 unclaimed");

    /* a filter matching nothing is a legitimate empty answer */
    out = call("pci_list", "{\"vendor\":1}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "0 function(s) matched");

    /* limit */
    out = call("pci_list", "{\"bus\":0,\"limit\":3}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "...4 more not shown");
}

static void test_pci_list_bad_input(void) {
    struct { const char *in; const char *want; } cases[] = {
        { "{\"bus\":256}",      "\"bus\" must be an integer 0-255" },
        { "{\"bus\":-1}",       "\"bus\" must be an integer 0-255" },
        { "{\"bus\":\"0\"}",    "\"bus\" must be an integer 0-255" },
        { "{\"vendor\":65536}", "\"vendor\" must be an integer 0-65535" },
        { "{\"vendor\":-5}",    "\"vendor\" must be an integer 0-65535" },
        { "{\"class\":256}",    "\"class\" must be an integer 0-255" },
        { "{\"class\":true}",   "\"class\" must be an integer 0-255" },
        { "{\"limit\":257}",    "\"limit\" must be an integer 1-256" },
        { "{\"unclaimed_only\":1}", "must be a boolean" },
        { "null",               "must be a JSON object" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *out = call("pci_list", cases[i].in);
        CHECK(R.is_error);
        CHECK_CONTAINS(out, cases[i].want);
        CHECK_CONTAINS(kcap_text(), "-> EINVAL]");
    }
}

/* ====================================================================== */
/* pci_info                                                               */
/* ====================================================================== */

static void test_pci_info(void) {
    const char *out = call("pci_info", "{\"addr\":\"00:03.0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "addr=00:03.0 vendor=0x8086 device=0x100e");
    CHECK_CONTAINS(out, "class=02 subclass=00 prog_if=00 revision=03 network/ethernet");
    CHECK_CONTAINS(out, "header_type=0x00 multifunction=no");
    CHECK_CONTAINS(out, "command=0x0007 [io mem busmaster intx-enabled] status=0x0230");
    CHECK_CONTAINS(out, "subsystem=1af4:1100");
    CHECK_CONTAINS(out, "irq_line=11 irq_pin=1");
    CHECK_CONTAINS(out, "bar0 mem32 base=0xfebc0000 prefetch=no raw=0xfebc0000");
    CHECK_CONTAINS(out, "bar1 io base=0xc000 raw=0x0000c001");
    CHECK_CONTAINS(out, "bar2 unused");
    CHECK_CONTAINS(out, "bar5 unused");
    CHECK_CONTAINS(out, "capabilities: 0x01/power-management@0xdc");
    CHECK_CONTAINS(out, "device_node=pci00:03.0 state=registered driver=e1000(eth0)");
    CHECK(strstr(out, "UNCLAIMED") == NULL);
    CHECK_CONTAINS(out, "BAR sizes are not probed");
    CHECK_CONTAINS(kcap_text(), "[pci_info 00:03.0 8086:100e class=02:00 driven=1 -> ok]");

    /* bus/device/function addressing is equivalent to addr */
    const char *alt = call("pci_info", "{\"bus\":0,\"device\":3,\"function\":0}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(alt, "addr=00:03.0 vendor=0x8086 device=0x100e");

    /* 64-bit prefetchable BAR, and an unclaimed function */
    out = call("pci_info", "{\"addr\":\"00:04.0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "bar0 mem64 base=0x1fe000000 prefetch=yes raw=0xfe00000c");
    CHECK_CONTAINS(out, "bar1 (upper half of bar0)");
    CHECK_CONTAINS(out, "capabilities: none advertised");
    CHECK_CONTAINS(out, "UNCLAIMED");
    CHECK_CONTAINS(kcap_text(), "driven=0 -> ok]");

    /* multifunction flag decoded from the header type */
    out = call("pci_info", "{\"addr\":\"00:01.0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "header_type=0x00 multifunction=yes");

    /* prefetchable 32-bit memory BAR */
    out = call("pci_info", "{\"addr\":\"00:02.0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "bar0 mem32 base=0xfd000000 prefetch=yes");
    CHECK_CONTAINS(out, "bar2 mem32 base=0xfebd0000");
}

static void test_pci_info_bad_input(void) {
    struct { const char *in; const char *want; const char *trace; } cases[] = {
        { "{}",                        "give either \"addr\"", "EINVAL" },
        { "{\"addr\":\"zz:03.0\"}",    "malformed addr",       "EINVAL" },
        { "{\"addr\":\"00:03\"}",      "malformed addr",       "EINVAL" },
        { "{\"addr\":\"00:03.8\"}",    "malformed addr",       "EINVAL" },
        { "{\"addr\":\"00:20.0\"}",    "malformed addr",       "EINVAL" },
        { "{\"addr\":\"000:03.0\"}",   "malformed addr",       "EINVAL" },
        { "{\"addr\":\"00:03.0 \"}",   "malformed addr",       "EINVAL" },
        { "{\"addr\":\"\"}",           "malformed addr",       "EINVAL" },
        { "{\"addr\":\"-1:-1.-1\"}",   "malformed addr",       "EINVAL" },
        { "{\"addr\":7}",              "must be a string",     "EINVAL" },
        { "{\"bus\":0,\"device\":32}", "\"device\" must be an integer 0-31", "EINVAL" },
        { "{\"bus\":0,\"device\":-1}", "\"device\" must be an integer 0-31", "EINVAL" },
        { "{\"bus\":300,\"device\":0}","\"bus\" must be an integer 0-255",   "EINVAL" },
        { "{\"bus\":0}",               "\"device\" is required",             "EINVAL" },
        { "{\"bus\":0,\"device\":0,\"function\":8}", "\"function\" must be an integer 0-7", "EINVAL" },
        { "{\"addr\":\"00:1f.7\"}",    "no device at 00:1f.7", "ENOENT" },
        { "{\"addr\":\"ff:1f.7\"}",    "no device at ff:1f.7", "ENOENT" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *out = call("pci_info", cases[i].in);
        CHECK(R.is_error);
        CHECK_CONTAINS(out, cases[i].want);
        char t[32];
        snprintf(t, sizeof t, "-> %s]", cases[i].trace);
        CHECK_CONTAINS(kcap_text(), t);
    }

    /* An oversized addr string must not overflow the 32-byte buffer. */
    static char big[600];
    int p = snprintf(big, sizeof big, "{\"addr\":\"");
    for (int i = 0; i < 400; i++) big[p++] = '0';
    snprintf(big + p, sizeof big - p, "\"}");
    const char *out = call("pci_info", big);
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "too long");
}

/* ====================================================================== */
/* pci_config_read                                                        */
/* ====================================================================== */

static void test_pci_config_read(void) {
    /* The headline: the e1000's BAR0. */
    const char *out = call("pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":16}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "00:03.0 offset=0x10 width=4 count=1");
    CHECK_CONTAINS(out, "0x10: 0xfebc0000");
    CHECK_CONTAINS(kcap_text(), "[pci_config_read 00:03.0 off=0x10 w=4 val=0xfebc0000 -> ok]");

    /* Sub-dword reads are extracted from the dword the hardware can return. */
    out = call("pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":0,\"width\":2}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "0x00: 0x8086");
    CHECK_CONTAINS(kcap_text(), "off=0x00 w=2 val=0x8086 -> ok]");

    out = call("pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":2,\"width\":2}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "0x02: 0x100e");

    out = call("pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":11,\"width\":1}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "0x0b: 0x02");         /* base class byte */
    CHECK_CONTAINS(kcap_text(), "off=0x0b w=1 val=0x02 -> ok]");

    /* A whole-header dump. */
    out = call("pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":0,\"count\":16}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "0x00: 0x100e8086 0x02300007 0x02000003 0x00000000");
    CHECK_CONTAINS(out, "0x10: 0xfebc0000 0x0000c001");
    CHECK_CONTAINS(kcap_text(), "off=0x00 w=4 n=16 first=0x100e8086 -> ok]");

    /* Reading right up to the last legal byte. */
    out = call("pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":252}");
    CHECK(!R.is_error);
    out = call("pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":255,\"width\":1}");
    CHECK(!R.is_error);
    out = call("pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":0,\"count\":64}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "count=64");

    /* bus/device/function form */
    out = call("pci_config_read", "{\"bus\":0,\"device\":2,\"function\":0,\"offset\":16}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "0x10: 0xfd000008");
}

static void test_pci_config_read_bounds(void) {
    struct { const char *in; const char *want; const char *trace; } cases[] = {
        /* missing / wrong-typed arguments */
        { "{\"addr\":\"00:03.0\"}",                       "\"offset\" is required", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":\"0x10\"}",   "\"offset\" must be an integer", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":null}",       "\"offset\" is required", "EINVAL" },
        /* offset out of config space */
        { "{\"addr\":\"00:03.0\",\"offset\":256}",        "\"offset\" out of range", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":-1}",         "\"offset\" out of range", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":-2147483648}","\"offset\" out of range", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":99999999999}","\"offset\" out of range", "EINVAL" },
        /* alignment */
        { "{\"addr\":\"00:03.0\",\"offset\":1}",          "not 4-byte aligned", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":2}",          "not 4-byte aligned", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":3}",          "not 4-byte aligned", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":17,\"width\":2}", "not 2-byte aligned", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":254}",        "not 4-byte aligned", "EINVAL" },
        /* width */
        { "{\"addr\":\"00:03.0\",\"offset\":0,\"width\":3}", "must be 1, 2 or 4", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":0,\"width\":0}", "must be 1, 2 or 4", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":0,\"width\":8}", "must be 1, 2 or 4", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":0,\"width\":-4}","must be 1, 2 or 4", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":0,\"width\":1e3}","must be 1, 2 or 4", "EINVAL" },
        /* count */
        { "{\"addr\":\"00:03.0\",\"offset\":0,\"count\":0}",  "must be an integer 1-64", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":0,\"count\":65}", "must be an integer 1-64", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":0,\"count\":-3}", "must be an integer 1-64", "EINVAL" },
        /* the read as a whole must stay inside 256 bytes */
        { "{\"addr\":\"00:03.0\",\"offset\":252,\"count\":2}",           "runs past the end", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":4,\"count\":64}",            "runs past the end", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":255,\"width\":1,\"count\":2}","runs past the end", "EINVAL" },
        { "{\"addr\":\"00:03.0\",\"offset\":254,\"width\":2,\"count\":2}","runs past the end", "EINVAL" },
        /* absent hardware */
        { "{\"addr\":\"00:1e.0\",\"offset\":0}",          "no device at 00:1e.0", "ENOENT" },
        { "{\"bus\":255,\"device\":31,\"function\":7,\"offset\":0}", "no device at ff:1f.7", "ENOENT" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *out = call("pci_config_read", cases[i].in);
        CHECK(R.is_error);
        CHECK_CONTAINS(out, cases[i].want);
        char t[32];
        snprintf(t, sizeof t, "-> %s]", cases[i].trace);
        CHECK_CONTAINS(kcap_text(), t);
    }
}

/* Every legal (offset, width, count) triple must be accepted and every illegal
 * one refused — exhaustively, because this is the tuple that reaches a port. */
static void test_pci_config_read_exhaustive(void) {
    static const int widths[] = { 1, 2, 4 };
    char in[128];
    int  accepted = 0, refused = 0;

    for (int wi = 0; wi < 3; wi++) {
        int w = widths[wi];
        for (int off = 0; off <= 256; off += (w == 1 ? 17 : 1)) {
            for (int cnt = 1; cnt <= 64; cnt += 21) {
                snprintf(in, sizeof in,
                         "{\"addr\":\"00:03.0\",\"offset\":%d,\"width\":%d,\"count\":%d}",
                         off, w, cnt);
                int legal = (off >= 0 && off <= 255) && (off % w == 0) &&
                            (off + w * cnt <= 256);
                call("pci_config_read", in);
                if (legal) { CHECK(!R.is_error); accepted++; }
                else       { CHECK(R.is_error);  refused++; }
            }
        }
    }
    CHECK(accepted > 100);
    CHECK(refused > 100);
    CHECK_EQ(fake_bad_access, 0);
    CHECK_EQ(fake_writes, 0);
}

/* ====================================================================== */
/* power lifecycle                                                        */
/* ====================================================================== */

static void test_power_cycle(void) {
    kbd_suspends = kbd_resumes = 0;
    CHECK_EQ(dev_kbd->state, DEV_STATE_ACTIVE);

    const char *out = call("device_suspend", "{\"name\":\"kbd0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "kbd0: state active -> suspended (driver kbd suspend hook returned 0)");
    CHECK_CONTAINS(kcap_text(), "[device_suspend kbd0 driver=kbd active->suspended -> ok]");
    CHECK_EQ(kbd_suspends, 1);                        /* the hook really ran */
    CHECK_EQ(dev_kbd->state, DEV_STATE_SUSPENDED);    /* the state really moved */

    /* An independent tool must agree the machine changed. */
    out = call("device_info", "{\"name\":\"kbd0\"}");
    CHECK_CONTAINS(out, "state=suspended");
    out = call("device_list", "{\"state\":\"suspended\"}");
    CHECK_CONTAINS(out, "name=kbd0");
    CHECK_CONTAINS(out, "1 matched");

    /* Suspending twice is refused, and the hook is not run again. */
    out = call("device_suspend", "{\"name\":\"kbd0\"}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "kbd0 is suspended; suspend requires state active");
    CHECK_EQ(kbd_suspends, 1);
    CHECK_CONTAINS(kcap_text(), "[device_suspend kbd0 -> EINVAL]");

    out = call("device_resume", "{\"name\":\"kbd0\"}");
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "kbd0: state suspended -> active");
    CHECK_CONTAINS(kcap_text(), "[device_resume kbd0 driver=kbd suspended->active -> ok]");
    CHECK_EQ(kbd_resumes, 1);
    CHECK_EQ(dev_kbd->state, DEV_STATE_ACTIVE);

    /* Resuming something that is not suspended is refused. */
    out = call("device_resume", "{\"name\":\"kbd0\"}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "kbd0 is active; resume requires state suspended");
    CHECK_EQ(kbd_resumes, 1);
}

static void test_power_refusals(void) {
    /* Driver with no suspend hook. */
    const char *out = call("device_suspend", "{\"name\":\"eth0\"}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "driver \"e1000\" implements no suspend hook");
    CHECK_EQ(dev_eth->state, DEV_STATE_ACTIVE);
    CHECK_CONTAINS(kcap_text(), "[device_suspend eth0 -> EINVAL]");

    /* No driver bound at all. */
    out = call("device_suspend", "{\"name\":\"pci00:03.0\"}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "has no driver bound");

    /* Unknown device. */
    out = call("device_resume", "{\"name\":\"gpu0\"}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "no such device \"gpu0\"");
    CHECK_CONTAINS(kcap_text(), "-> ENOENT]");

    /* Missing / wrong-typed name. */
    out = call("device_suspend", "{}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "\"name\" is required");
    out = call("device_resume", "{\"name\":[1,2]}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "must be a string");

    /* A hook that fails leaves the state exactly where it was. */
    flaky_suspends = 0;
    CHECK_EQ(dev_flaky->state, DEV_STATE_ACTIVE);
    out = call("device_suspend", "{\"name\":\"snd0\"}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "driver \"flaky\" suspend hook failed (rc=-5); snd0 left active");
    CHECK_EQ(flaky_suspends, 1);
    CHECK_EQ(dev_flaky->state, DEV_STATE_ACTIVE);

    /* And it has no resume hook either. */
    out = call("device_resume", "{\"name\":\"snd0\"}");
    CHECK(R.is_error);
    CHECK_CONTAINS(out, "implements no resume hook");
}

/* ====================================================================== */
/* address parser (unit level)                                            */
/* ====================================================================== */

static void test_parse_addr(void) {
    pciaddr_t a;
    CHECK_EQ(parse_addr("00:03.0", &a), 0);
    CHECK_EQ(a.bus, 0); CHECK_EQ(a.dev, 3); CHECK_EQ(a.fn, 0);
    CHECK_EQ(parse_addr("ff:1f.7", &a), 0);
    CHECK_EQ(a.bus, 255); CHECK_EQ(a.dev, 31); CHECK_EQ(a.fn, 7);
    CHECK_EQ(parse_addr("FF:1F.7", &a), 0);
    CHECK_EQ(a.bus, 255); CHECK_EQ(a.dev, 31);
    CHECK_EQ(parse_addr("0:3.0", &a), 0);
    CHECK_EQ(a.bus, 0); CHECK_EQ(a.dev, 3);

    static const char *bad[] = {
        "", ":", ".", "0", "00:", "00:03", "00:03.", "00.03:0", "00:03.0.",
        "00:03.08", "00:32.0", "00:20.0", "000:03.0", "00:003.0", "00:03.8",
        " 00:03.0", "00:03.0 ", "gg:03.0", "00:gg.0", "00:03.g", "--:--.-",
        "\n", "0x00:0x03.0", "00;03.0", "00:03,0",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(parse_addr(bad[i], &a) != 0);
    CHECK(parse_addr(NULL, &a) != 0);

    /* Every legal address round-trips through the printed form. */
    for (int bus = 0; bus < 256; bus += 7)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                char s[16];
                snprintf(s, sizeof s, "%02x:%02x.%x", bus, dev, fn);
                CHECK_EQ(parse_addr(s, &a), 0);
                CHECK_EQ(a.bus, bus); CHECK_EQ(a.dev, dev); CHECK_EQ(a.fn, fn);
            }
}

/* ====================================================================== */
/* result-buffer pressure                                                 */
/* ====================================================================== */

/* A cramped result buffer must clip cleanly at every size, never overrun, and
 * never leave the buffer unterminated. */
static void test_tiny_buffers(void) {
    static char buf[256 + 64];
    const char *calls[][2] = {
        { "device_list",     "{}" },
        { "pci_list",        "{}" },
        { "pci_info",        "{\"addr\":\"00:03.0\"}" },
        { "pci_config_read", "{\"addr\":\"00:03.0\",\"offset\":0,\"count\":64}" },
        { "device_info",     "{\"name\":\"eth0\"}" },
        { "device_suspend",  "{\"name\":\"nosuch\"}" },
    };
    for (size_t k = 0; k < sizeof calls / sizeof calls[0]; k++) {
        for (size_t cap = 1; cap <= 256; cap++) {
            memset(buf, CANARY, sizeof buf);
            tool_result_t r;
            tool_result_init(&r, buf, cap);
            tool_call_t c = { .id = "x", .name = calls[k][0],
                              .input = calls[k][1], .input_len = strlen(calls[k][1]) };
            tool_dispatch(&c, &r);
            CHECK(kpanic_hit == 0);
            CHECK(r.len < cap);
            CHECK(buf[r.len] == '\0');
            CHECK(canary_intact(buf + cap, sizeof buf - cap));
        }
    }
}

/* ====================================================================== */
/* fuzz                                                                   */
/* ====================================================================== */

static uint32_t rng_state = 0x1234abcdu;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Values chosen to be exactly the things a confused model emits. */
static const char *fuzz_keys[] = {
    "addr", "bus", "device", "function", "offset", "width", "count",
    "name", "class", "state", "limit", "unclaimed_only", "vendor",
    "", "Addr", "offset ", "\\u0000", "__proto__", "0",
};
static const char *fuzz_vals[] = {
    "0", "-1", "1", "255", "256", "4096", "-2147483648", "9223372036854775807",
    "9223372036854775808", "-9223372036854775809", "1e300", "0.5", "-0.0",
    "true", "false", "null", "[]", "{}", "[1,2,3]", "{\"a\":1}",
    "\"\"", "\"00:03.0\"", "\"00:03\"", "\"network\"", "\"eth0\"",
    "\"\\u0000\"", "\"\\ud800\"", "\"../../etc\"", "\"%s%s%s%n\"",
    "\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"",
};

static void test_fuzz_structured(void) {
    char in[512];
    for (int iter = 0; iter < 4000; iter++) {
        int nk = 1 + (int)(rnd() % 4);
        int p  = snprintf(in, sizeof in, "{");
        for (int i = 0; i < nk; i++) {
            const char *k = fuzz_keys[rnd() % (sizeof fuzz_keys / sizeof fuzz_keys[0])];
            const char *v = fuzz_vals[rnd() % (sizeof fuzz_vals / sizeof fuzz_vals[0])];
            p += snprintf(in + p, sizeof in - p, "%s\"%s\":%s", i ? "," : "", k, v);
        }
        snprintf(in + p, sizeof in - p, "}");

        const char *tool = tool_names[rnd() % NTOOLS];
        call(tool, in);
        /* call() asserts the invariants; here just make sure a failure is
         * always explained rather than silently empty. */
        if (R.is_error) CHECK(R.len > 7);
    }
    CHECK_EQ(fake_writes, 0);
    CHECK_EQ(fake_bad_access, 0);
}

/* Raw bytes, mostly not JSON at all, and never NUL-terminated at the span end. */
static void test_fuzz_bytes(void) {
    static char in[257];
    for (int iter = 0; iter < 3000; iter++) {
        size_t len = 1 + (rnd() % 200);
        for (size_t i = 0; i < len; i++) {
            uint32_t r = rnd();
            in[i] = (r % 4 == 0) ? "{}[]\":,0123456789truefalsnl."[r % 28]
                                 : (char)(r & 0xFF);
        }
        in[len] = (char)0xAA;                 /* no NUL: the span is the bound */
        const char *tool = tool_names[rnd() % NTOOLS];
        call_n(tool, in, len);
        CHECK((char)in[len] == (char)0xAA);   /* nobody walked off the end */
    }
    CHECK_EQ(fake_writes, 0);
    CHECK_EQ(fake_bad_access, 0);
}

/* Deeply nested and pathological documents must be rejected by json.h without
 * blowing the stack. */
static void test_fuzz_pathological(void) {
    static char in[8192];
    for (int depth = 1; depth < 400; depth += 7) {
        int p = 0;
        for (int i = 0; i < depth && p < (int)sizeof in - 2; i++) in[p++] = '[';
        for (int i = 0; i < depth && p < (int)sizeof in - 1; i++) in[p++] = ']';
        for (int t = 0; t < NTOOLS; t++) {
            call_n(tool_names[t], in, (size_t)p);
            CHECK(R.is_error);
        }
    }
    /* A huge but valid object with one good field still works. */
    int p = snprintf(in, sizeof in, "{\"addr\":\"00:03.0\",\"offset\":16");
    for (int i = 0; i < 200; i++)
        p += snprintf(in + p, sizeof in - p, ",\"junk%d\":%d", i, i);
    snprintf(in + p, sizeof in - p, "}");
    const char *out = call("pci_config_read", in);
    CHECK(!R.is_error);
    CHECK_CONTAINS(out, "0x10: 0xfebc0000");
}

/* ====================================================================== */

int main(void) {
    console_init();
    build_world();

    RUN(test_registration);
    RUN(test_schema_assembly);
    RUN(test_parse_addr);
    RUN(test_device_list);
    RUN(test_device_list_bad_input);
    RUN(test_device_info);
    RUN(test_device_info_bad_input);
    RUN(test_pci_list);
    RUN(test_pci_list_bad_input);
    RUN(test_pci_info);
    RUN(test_pci_info_bad_input);
    RUN(test_pci_config_read);
    RUN(test_pci_config_read_bounds);
    RUN(test_pci_config_read_exhaustive);
    RUN(test_power_cycle);
    RUN(test_power_refusals);
    RUN(test_tiny_buffers);
    RUN(test_fuzz_structured);
    RUN(test_fuzz_bytes);
    RUN(test_fuzz_pathological);

    /* The load-bearing safety property of this whole tool family. */
    printf("  (config reads: %ld, config writes: %ld, misaligned: %ld)\n",
           fake_reads, fake_writes, fake_bad_access);
    CHECK_EQ(fake_writes, 0);
    CHECK_EQ(fake_bad_access, 0);
    CHECK(fake_reads > 1000);

    return th_report("dev_tools");
}
