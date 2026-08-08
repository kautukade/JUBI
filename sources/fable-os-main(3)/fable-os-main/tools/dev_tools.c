/* dev_tools.c — the hardware introspection tool family.
 *
 * PURPOSE
 *   Let the model see and steer the actual machine. When someone types "what
 *   hardware am I running on?", the answer must come out of real PCI config
 *   cycles and the real device registry, not out of the model's training data.
 *   Every number in every result here was read from silicon (or QEMU's model of
 *   it) microseconds before it was printed, and the kernel-emitted trace line
 *   next to it proves the read happened.
 *
 * THE TOOLS
 *   device_list        enumerate the device registry (filters: class, state,
 *                      unclaimed_only) — the kernel's view of the machine.
 *   device_info        one device in full: class, state, bound driver, which
 *                      lifecycle hooks that driver actually implements, parent,
 *                      resources, and — for PCI-backed nodes — a live config
 *                      read of its vendor/device/class.
 *   pci_list           walk PCI config space and report every function found,
 *                      with its IDs, class, first programmed BAR, and whether
 *                      any driver has claimed it (filters: bus, vendor, class,
 *                      unclaimed_only).
 *   pci_info           one function, fully decoded: IDs, class/subclass/prog-if,
 *                      command/status, subsystem IDs, interrupt line/pin, all
 *                      BARs (32/64-bit, MMIO/IO, prefetchable), and the
 *                      capability chain.
 *   pci_config_read    raw, bounded config-space reads — the escape hatch for
 *                      anything the decoders above do not cover.
 *   device_suspend     run driver->suspend(dev) and move the device to
 *   device_resume      SUSPENDED / back to ACTIVE. Both are TOOL_MUTATES.
 *
 * WHY THIS SHAPE
 *   A later phase lets the model author drivers at runtime for hardware this
 *   kernel has never heard of. That workflow is exactly:
 *
 *       pci_list unclaimed_only=true      -> "nobody drives 00:01.1"
 *       pci_info addr=00:01.1             -> IDs, class, BARs, capabilities
 *       pci_config_read addr=... off=...  -> anything pci_info did not decode
 *
 *   So an *unclaimed* function is the first-class case, not an afterthought:
 *   both enumerators can filter for it, both report claim status per row, and
 *   both print an "unclaimed: N" summary. "Claimed" is computed honestly — a
 *   PCI function counts as driven if the device node published for it has a
 *   driver, OR if some other registered device with a driver owns a resource at
 *   one of the function's BAR addresses (which is how eth0/e1000 is recognised
 *   as the owner of 00:03.0 even though the PCI node itself is driverless).
 *
 * CONFIG-SPACE WRITES: DELIBERATELY NOT EXPOSED
 *   pci_cfg_write32() exists and this file never calls it. Config space is not
 *   a scratchpad: the BARs are there, so is the command register, so are bridge
 *   apertures and PM control. One mistargeted dword can move a live device's
 *   MMIO window out from under a driver that is mid-DMA, clear memory decoding
 *   on the host bridge, or park a device's window over kernel RAM. There is no
 *   IOMMU and no page protection on MMIO, and the IDT does not help: every one of
 *   those outcomes is a successful write to a mapped address, so the failure mode
 *   is silent corruption, not a fault we could report.
 *
 *   Discovery does not need writes. The one read-only-looking exception, BAR
 *   *sizing*, is really write-0xFFFFFFFF / read / restore: a non-atomic window
 *   during which the device decodes at a garbage address. Not worth it here.
 *
 *   The mutations that *are* exposed go through driver-authored suspend/resume
 *   hooks, which is the correct boundary: the model states intent ("power down
 *   the keyboard"), the driver decides which registers that means. When the
 *   driver VM lands, writes belong there — validated per-device programs with an
 *   offset whitelist and a restore path — not as a raw poke tool.
 *
 * UNTRUSTED INPUT
 *   `call->input` is model output. Every field is range-checked before it can
 *   reach a port: bus 0-255, device 0-31, function 0-7, offset 0-255, width in
 *   {1,2,4}, offset aligned to width, offset+width*count <= 256. Strings are
 *   decoded into fixed buffers with json_get_str (which reports ENOSPC rather
 *   than overrunning). Nothing here allocates. Every rejection becomes an
 *   `error: ...` result plus a symbolic trace line, so a confused model can fix
 *   its arguments on the next turn instead of taking the kernel down.
 */

#include "tool.h"
#include "device.h"
#include "driver.h"
#include "pci.h"
#include "json.h"
#include "trace.h"
#include "kernel.h"

#include <stdarg.h>
#include <stdio.h>       /* vsnprintf, via port/stdio.h when freestanding */

/* Longest device name we will accept from the model. Real names are short
 * ("serial0", "pci00:03.0"); anything longer cannot match, so a 10 KB name is
 * a clean ENOENT rather than a buffer problem. */
#define DEVNAME_MAX     40

/* Keep this much of the result buffer in reserve so a list can always append
 * its "...N more" note and its summary line after it stops emitting rows. */
#define ROW_MARGIN      224

#define LIST_LIMIT_DEF  64
#define LIST_LIMIT_MAX  256
#define CFG_COUNT_MAX   64

/* ====================================================================== */
/* small helpers                                                          */
/* ====================================================================== */

static int name_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (size_t i = 0; i <= DEVNAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 0;                       /* longer than any legal device name */
}

/* Emit the failure result *and* its trace line. `args` is the already-rendered
 * argument text for the trace; `fmt` is the human/model-readable reason. */
static int fail(tool_result_t *r, int err, const char *op, const char *args,
                const char *fmt, ...) {
    char msg[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    r->is_error = 1;
    r->len = 0;                     /* discard any partial output */
    /* ...and un-say that it was truncated. tool_dispatch() staples "[result
     * truncated at N bytes]" onto anything still carrying this flag, so leaving
     * it set after throwing the partial output away appends that claim to a
     * two-line error message, where it is simply false. */
    r->truncated = 0;
    if (r->buf && r->cap) r->buf[0] = '\0';
    tool_result_printf(r, "error: %s", msg);
    trace_err(err, op, "%s", args ? args : "");
    return err;
}

/* Parse the input span as a JSON object. An absent/empty span is treated as
 * `{}` so tools whose arguments are all optional still work. */
static int parse_obj(const tool_call_t *c, json_value_t *obj) {
    const char *p = "{}";
    size_t      n = 2;
    if (c && c->input && c->input_len) { p = c->input; n = c->input_len; }
    if (json_parse(p, n, obj) != JSON_OK) return TOOL_EINVAL;
    if (obj->type != JSON_OBJECT) return TOOL_EINVAL;
    return TOOL_OK;
}

/* Field readers. Return 1 = present and valid, 0 = absent,
 * -1 = present but wrong type, -2 = present but out of range / too long. */

static int f_int(const json_value_t *o, const char *key,
                 int64_t lo, int64_t hi, int64_t *out) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_NUMBER) return -1;
    int64_t x;
    if (json_int(&v, &x) != JSON_OK) return -2;
    if (x < lo || x > hi) return -2;
    *out = x;
    return 1;
}

static int f_bool(const json_value_t *o, const char *key, int *out) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_BOOL) return -1;
    if (json_bool(&v, out) != JSON_OK) return -1;
    return 1;
}

static int f_str(const json_value_t *o, const char *key, char *dst, size_t cap) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_STRING) return -1;
    rc = json_str(&v, dst, cap, NULL);
    if (rc == JSON_ENOSPC) return -2;
    if (rc != JSON_OK) return -1;
    return 1;
}

static int room_low(const tool_result_t *r) {
    return r->truncated || r->len + ROW_MARGIN >= r->cap;
}

/* ====================================================================== */
/* device registry queries                                                */
/* ====================================================================== */

typedef struct {
    const char        *want_name;
    device_res_type_t  want_res;
    uint64_t           want_base;
    int                need_driver;
    device_t          *found;
} devq_t;

static void devq_cb(device_t *d, void *ctx) {
    devq_t *q = (devq_t *)ctx;
    if (q->found || !d) return;
    if (q->need_driver && !d->driver) return;
    if (q->want_name) {
        if (name_eq(d->obj.name, q->want_name)) q->found = d;
        return;
    }
    int n = d->nres;
    if (n > DEV_MAX_RESOURCES) n = DEV_MAX_RESOURCES;
    for (int i = 0; i < n; i++)
        if (d->res[i].type == q->want_res && d->res[i].base == q->want_base) {
            q->found = d;
            return;
        }
}

static device_t *dev_by_name(const char *name) {
    devq_t q;
    memset(&q, 0, sizeof q);
    q.want_name = name;
    device_foreach(devq_cb, &q);
    return q.found;
}

static device_t *dev_by_resource(device_res_type_t t, uint64_t base, int need_driver) {
    if (!base) return NULL;
    devq_t q;
    memset(&q, 0, sizeof q);
    q.want_res    = t;
    q.want_base   = base;
    q.need_driver = need_driver;
    device_foreach(devq_cb, &q);
    return q.found;
}

/* Reuse the kernel's own name tables so a filter can never drift from what the
 * lists print. Returns -1 when the string names no class/state. */
static int class_from_name(const char *s) {
    for (int c = 0; c < DEV_CLASS_MAX; c++)
        if (name_eq(device_class_name((device_class_t)c), s)) return c;
    return -1;
}

static int state_from_name(const char *s) {
    for (int st = DEV_STATE_UNKNOWN; st <= DEV_STATE_FAILED; st++)
        if (name_eq(device_state_name((device_state_t)st), s)) return st;
    return -1;
}

static void put_resources(tool_result_t *r, const device_t *d) {
    int n = d->nres;
    if (n > DEV_MAX_RESOURCES) n = DEV_MAX_RESOURCES;
    for (int i = 0; i < n; i++) {
        const device_resource_t *res = &d->res[i];
        switch (res->type) {
            case RES_MMIO:
                tool_result_printf(r, " mmio=0x%lx", (unsigned long)res->base);
                if (res->size) tool_result_printf(r, "+0x%lx", (unsigned long)res->size);
                break;
            case RES_IOPORT:
                tool_result_printf(r, " io=0x%lx", (unsigned long)res->base);
                if (res->size) tool_result_printf(r, "+%lu", (unsigned long)res->size);
                break;
            case RES_IRQ:
                tool_result_printf(r, " irq=%lu", (unsigned long)res->base);
                break;
            default:
                break;
        }
    }
}

/* ====================================================================== */
/* PCI addressing + decoding                                              */
/* ====================================================================== */

typedef struct { uint8_t bus, dev, fn; } pciaddr_t;

static int hexdig(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "bb:dd.f" (hex, 1-2 digits for bus and device, 1 for function).
 * Returns 0 on success. Consumes the whole string or fails. */
static int parse_addr(const char *s, pciaddr_t *a) {
    if (!s) return -1;
    int i = 0, v, n;

    for (n = 0, v = 0; n < 2 && hexdig(s[i]) >= 0; n++) v = v * 16 + hexdig(s[i++]);
    if (n == 0 || s[i] != ':' || v > 255) return -1;
    a->bus = (uint8_t)v;
    i++;

    for (n = 0, v = 0; n < 2 && hexdig(s[i]) >= 0; n++) v = v * 16 + hexdig(s[i++]);
    if (n == 0 || s[i] != '.' || v > 31) return -1;
    a->dev = (uint8_t)v;
    i++;

    v = hexdig(s[i]);
    if (v < 0 || v > 7) return -1;
    a->fn = (uint8_t)v;
    i++;

    return s[i] == '\0' ? 0 : -1;
}

/* A device node published by pci.c is named "pci<bb>:<dd>.<f>". */
static int dev_pci_addr(const device_t *d, pciaddr_t *a) {
    const char *n = d ? d->obj.name : NULL;
    if (!n || n[0] != 'p' || n[1] != 'c' || n[2] != 'i') return 0;
    return parse_addr(n + 3, a) == 0;
}

static int pci_present(const pciaddr_t *a) {
    uint16_t vendor = (uint16_t)(pci_cfg_read32(a->bus, a->dev, a->fn, 0x00) & 0xFFFF);
    return vendor != 0xFFFF && vendor != 0x0000;
}

/* Decoded BAR. `next_is_upper` means BAR n+1 is this one's high dword. */
typedef struct {
    uint32_t raw;
    int      present;
    int      is_io;
    int      is64;
    int      prefetch;
    uint64_t base;
} bar_t;

static void decode_bar(const pciaddr_t *a, int idx, bar_t *b) {
    memset(b, 0, sizeof *b);
    b->raw = pci_cfg_read32(a->bus, a->dev, a->fn, (uint8_t)(0x10 + idx * 4));
    if (b->raw == 0 || b->raw == 0xFFFFFFFFu) return;
    if (b->raw & 1) {
        b->is_io   = 1;
        b->base    = b->raw & ~0x3u;
        b->present = b->base != 0;
    } else {
        b->is64     = ((b->raw >> 1) & 3) == 2;
        b->prefetch = (b->raw >> 3) & 1;
        b->base     = b->raw & ~0xFu;
        if (b->is64 && idx < 5) {
            uint32_t hi = pci_cfg_read32(a->bus, a->dev, a->fn,
                                         (uint8_t)(0x10 + (idx + 1) * 4));
            b->base |= ((uint64_t)hi) << 32;
        }
        b->present = b->base != 0;
    }
}

static const char *pci_class_desc(uint8_t cls, uint8_t sub) {
    switch (cls) {
        case 0x00: return "legacy";
        case 0x01:
            switch (sub) {
                case 0x01: return "storage/ide";
                case 0x06: return "storage/sata";
                case 0x08: return "storage/nvme";
                default:   return "storage";
            }
        case 0x02: return sub == 0x00 ? "network/ethernet" : "network";
        case 0x03: return sub == 0x00 ? "display/vga" : "display";
        case 0x04: return "multimedia";
        case 0x05: return "memory-controller";
        case 0x06:
            switch (sub) {
                case 0x00: return "bridge/host";
                case 0x01: return "bridge/isa";
                case 0x04: return "bridge/pci-pci";
                case 0x80: return "bridge/other";
                default:   return "bridge";
            }
        case 0x07: return "communication";
        case 0x08: return "system-peripheral";
        case 0x09: return "input";
        case 0x0A: return "docking";
        case 0x0B: return "processor";
        case 0x0C:
            switch (sub) {
                case 0x03: return "serial-bus/usb";
                case 0x05: return "serial-bus/smbus";
                default:   return "serial-bus";
            }
        case 0x0D: return "wireless";
        case 0x0E: return "intelligent-io";
        case 0x0F: return "satellite";
        case 0x10: return "encryption";
        case 0x11: return "signal-processing";
        case 0x12: return "accelerator";
        case 0x13: return "non-essential-instrumentation";
        case 0xFF: return "unassigned";
        default:   return "unknown-class";
    }
}

static const char *pci_cap_name(uint8_t id) {
    switch (id) {
        case 0x01: return "power-management";
        case 0x02: return "agp";
        case 0x03: return "vpd";
        case 0x04: return "slot-id";
        case 0x05: return "msi";
        case 0x06: return "compact-pci-hotswap";
        case 0x07: return "pci-x";
        case 0x08: return "hypertransport";
        case 0x09: return "vendor-specific";
        case 0x0A: return "debug-port";
        case 0x0D: return "subsystem-id";
        case 0x10: return "pci-express";
        case 0x11: return "msi-x";
        case 0x12: return "sata";
        case 0x13: return "advanced-features";
        default:   return "reserved";
    }
}

/* Who, if anyone, drives this function? `node` is the device published for the
 * PCI function itself; `claim` is any *other* driver-owning device whose
 * resources sit at one of this function's BAR addresses. */
typedef struct {
    device_t *node;
    device_t *claim;
} pci_owner_t;

static void pci_owner(const pciaddr_t *a, pci_owner_t *o) {
    char nm[16];
    snprintf(nm, sizeof nm, "pci%02x:%02x.%x", a->bus, a->dev, a->fn);
    o->node  = dev_by_name(nm);
    o->claim = NULL;
    if (o->node && o->node->driver) return;
    for (int i = 0; i < 6 && !o->claim; i++) {
        bar_t b;
        decode_bar(a, i, &b);
        if (!b.present) { if (b.is64) i++; continue; }
        o->claim = dev_by_resource(b.is_io ? RES_IOPORT : RES_MMIO, b.base, 1);
        if (b.is64) i++;
    }
}

static int owner_is_driven(const pci_owner_t *o) {
    return (o->node && o->node->driver) || (o->claim && o->claim->driver);
}

static void put_owner(tool_result_t *r, const pci_owner_t *o) {
    if (o->node && o->node->driver)
        tool_result_printf(r, " driver=%s(%s)", o->node->driver->name, o->node->obj.name);
    else if (o->claim && o->claim->driver)
        tool_result_printf(r, " driver=%s(%s)", o->claim->driver->name, o->claim->obj.name);
    else
        tool_result_printf(r, " driver=-");
}

/* Resolve a PCI address from either "addr":"bb:dd.f" or bus/device/function.
 * On failure writes the error result itself and returns nonzero. */
static int want_addr(const json_value_t *in, tool_result_t *r, const char *op,
                     pciaddr_t *a) {
    char s[32];
    int  rc = f_str(in, "addr", s, sizeof s);
    if (rc == -1) return fail(r, TOOL_EINVAL, op, "", "\"addr\" must be a string like \"00:03.0\"");
    if (rc == -2) return fail(r, TOOL_EINVAL, op, "", "\"addr\" is too long; expected \"bb:dd.f\" (hex)");
    if (rc == 1) {
        if (parse_addr(s, a) != 0)
            return fail(r, TOOL_EINVAL, op, "", "malformed addr \"%s\"; expected hex \"bb:dd.f\" with bus 00-ff, device 00-1f, function 0-7", s);
        return 0;
    }

    int64_t bus = -1, dev = -1, fn = 0;
    rc = f_int(in, "bus", 0, 255, &bus);
    if (rc < 0) return fail(r, TOOL_EINVAL, op, "", "\"bus\" must be an integer 0-255");
    if (rc == 0) return fail(r, TOOL_EINVAL, op, "", "give either \"addr\" (\"bb:dd.f\") or \"bus\"+\"device\"+\"function\"");
    rc = f_int(in, "device", 0, 31, &dev);
    if (rc < 0) return fail(r, TOOL_EINVAL, op, "", "\"device\" must be an integer 0-31");
    if (rc == 0) return fail(r, TOOL_EINVAL, op, "", "\"device\" is required when addressing by bus/device/function");
    rc = f_int(in, "function", 0, 7, &fn);
    if (rc < 0) return fail(r, TOOL_EINVAL, op, "", "\"function\" must be an integer 0-7");

    a->bus = (uint8_t)bus;
    a->dev = (uint8_t)dev;
    a->fn  = (uint8_t)fn;
    return 0;
}

/* ====================================================================== */
/* device_list                                                            */
/* ====================================================================== */

typedef struct {
    tool_result_t *r;
    int  want_class;      /* -1 = any */
    int  want_state;      /* -1 = any */
    int  unclaimed_only;
    int  limit;
    int  matched;         /* passed the filters                */
    int  shown;           /* actually printed                  */
    int  unclaimed;       /* driverless, among matched         */
    int  stopped;
} dlist_t;

static void dlist_cb(device_t *d, void *ctx) {
    dlist_t *s = (dlist_t *)ctx;
    if (!d) return;
    if (s->want_class >= 0 && (int)d->cls != s->want_class) return;
    if (s->want_state >= 0 && (int)d->state != s->want_state) return;
    if (s->unclaimed_only && d->driver) return;

    s->matched++;
    if (!d->driver) s->unclaimed++;

    if (s->stopped || s->shown >= s->limit || room_low(s->r)) { s->stopped = 1; return; }

    tool_result_printf(s->r, "id=%lu name=%s class=%s state=%s driver=%s",
                       (unsigned long)d->obj.id,
                       d->obj.name ? d->obj.name : "?",
                       device_class_name(d->cls),
                       device_state_name(d->state),
                       d->driver && d->driver->name ? d->driver->name : "-");
    if (d->parent && d->parent->obj.name)
        tool_result_printf(s->r, " parent=%s", d->parent->obj.name);

    pciaddr_t a;
    if (dev_pci_addr(d, &a))
        tool_result_printf(s->r, " pci=%02x:%02x.%x", a.bus, a.dev, a.fn);

    put_resources(s->r, d);
    tool_result_printf(s->r, "\n");
    s->shown++;
}

static int t_device_list(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "device_list", "", "input must be a JSON object");

    dlist_t s;
    memset(&s, 0, sizeof s);
    s.r = r;
    s.want_class = -1;
    s.want_state = -1;
    s.limit = LIST_LIMIT_DEF;

    char buf[32];
    int  rc = f_str(&in, "class", buf, sizeof buf);
    if (rc < 0) return fail(r, TOOL_EINVAL, "device_list", "", "\"class\" must be a short string");
    if (rc == 1) {
        s.want_class = class_from_name(buf);
        if (s.want_class < 0)
            return fail(r, TOOL_EINVAL, "device_list", "",
                        "unknown class \"%s\"; valid: none system bus serial input display storage network audio timer", buf);
    }

    rc = f_str(&in, "state", buf, sizeof buf);
    if (rc < 0) return fail(r, TOOL_EINVAL, "device_list", "", "\"state\" must be a short string");
    if (rc == 1) {
        s.want_state = state_from_name(buf);
        if (s.want_state < 0)
            return fail(r, TOOL_EINVAL, "device_list", "",
                        "unknown state \"%s\"; valid: unknown registered bound active suspended removed failed", buf);
    }

    if (f_bool(&in, "unclaimed_only", &s.unclaimed_only) < 0)
        return fail(r, TOOL_EINVAL, "device_list", "", "\"unclaimed_only\" must be a boolean");

    int64_t lim;
    rc = f_int(&in, "limit", 1, LIST_LIMIT_MAX, &lim);
    if (rc < 0) return fail(r, TOOL_EINVAL, "device_list", "", "\"limit\" must be an integer 1-%d", LIST_LIMIT_MAX);
    if (rc == 1) s.limit = (int)lim;

    device_foreach(dlist_cb, &s);

    if (s.stopped)
        tool_result_printf(r, "...%d more not shown (raise \"limit\" or filter by class/state)\n",
                           s.matched - s.shown);
    tool_result_printf(r, "devices: %d registered, %d matched, %d shown, %d unclaimed\n",
                       device_count(), s.matched, s.shown, s.unclaimed);

    char args[96];
    snprintf(args, sizeof args, "class=%s state=%s unclaimed_only=%d",
             s.want_class < 0 ? "*" : device_class_name((device_class_t)s.want_class),
             s.want_state < 0 ? "*" : device_state_name((device_state_t)s.want_state),
             s.unclaimed_only);
    trace_ret(s.matched, "device_list", "%s", args);
    return TOOL_OK;
}

static const tool_t device_list_tool = {
    .name = "device_list",
    .description =
        "List devices the kernel has registered, with class, state, bound driver "
        "and hardware resources (MMIO windows, I/O port ranges, IRQ lines). "
        "Filter by class, by state, or with unclaimed_only=true to see only "
        "hardware that has no driver bound. Start here to answer 'what hardware "
        "am I running on?'.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"class\":{\"type\":\"string\",\"description\":\"filter by device class: "
        "none, system, bus, serial, input, display, storage, network, audio, timer\"},"
        "\"state\":{\"type\":\"string\",\"description\":\"filter by state: unknown, "
        "registered, bound, active, suspended, removed, failed\"},"
        "\"unclaimed_only\":{\"type\":\"boolean\",\"description\":\"only devices with no driver bound\"},"
        "\"limit\":{\"type\":\"integer\",\"description\":\"max rows to print, 1-256 (default 64)\"}"
        "},\"required\":[]}",
    .flags  = 0,
    .invoke = t_device_list,
};
REGISTER_TOOL(device_list_tool);

/* ====================================================================== */
/* device_info                                                            */
/* ====================================================================== */

static void put_hooks(tool_result_t *r, const driver_t *drv) {
    int n = 0;
    tool_result_printf(r, " hooks=");
    if (drv->init)    tool_result_printf(r, "%sinit",    n++ ? "," : "");
    if (drv->probe)   tool_result_printf(r, "%sprobe",   n++ ? "," : "");
    if (drv->remove)  tool_result_printf(r, "%sremove",  n++ ? "," : "");
    if (drv->suspend) tool_result_printf(r, "%ssuspend", n++ ? "," : "");
    if (drv->resume)  tool_result_printf(r, "%sresume",  n++ ? "," : "");
    if (!n) tool_result_printf(r, "-");
}

static int t_device_info(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "device_info", "", "input must be a JSON object");

    char name[DEVNAME_MAX + 1];
    int  rc = f_str(&in, "name", name, sizeof name);
    if (rc == 0)  return fail(r, TOOL_EINVAL, "device_info", "", "\"name\" is required (see device_list)");
    if (rc == -1) return fail(r, TOOL_EINVAL, "device_info", "", "\"name\" must be a string");
    if (rc == -2) return fail(r, TOOL_ENOENT, "device_info", "<oversized>",
                              "no such device: name longer than %d characters", DEVNAME_MAX);

    device_t *d = dev_by_name(name);
    if (!d) return fail(r, TOOL_ENOENT, "device_info", name,
                        "no such device \"%s\"; use device_list to see registered names", name);

    tool_result_printf(r, "name=%s id=%lu class=%s state=%s capabilities=0x%lx\n",
                       d->obj.name ? d->obj.name : "?", (unsigned long)d->obj.id,
                       device_class_name(d->cls), device_state_name(d->state),
                       (unsigned long)d->capabilities);

    if (d->driver) {
        tool_result_printf(r, "driver=%s level=%d handles=%s",
                           d->driver->name ? d->driver->name : "?",
                           d->driver->level, device_class_name(d->driver->cls));
        put_hooks(r, d->driver);
        tool_result_printf(r, " suspendable=%s resumable=%s\n",
                           d->driver->suspend ? "yes" : "no",
                           d->driver->resume ? "yes" : "no");
    } else {
        tool_result_printf(r, "driver=- (unclaimed: no driver has bound to this device)\n");
    }

    tool_result_printf(r, "parent=%s\n",
                       d->parent && d->parent->obj.name ? d->parent->obj.name : "-");

    int nres = d->nres;
    if (nres > DEV_MAX_RESOURCES) nres = DEV_MAX_RESOURCES;
    tool_result_printf(r, "resources=%d\n", nres);
    for (int i = 0; i < nres; i++) {
        const device_resource_t *res = &d->res[i];
        const char *t = res->type == RES_MMIO   ? "mmio"
                      : res->type == RES_IOPORT ? "ioport"
                      : res->type == RES_IRQ    ? "irq" : "none";
        tool_result_printf(r, "  [%d] %s base=0x%lx size=0x%lx\n", i, t,
                           (unsigned long)res->base, (unsigned long)res->size);
    }

    /* If this node came from the PCI scan, read the hardware right now so the
     * answer is live silicon rather than what the registry cached at boot. */
    pciaddr_t a;
    if (dev_pci_addr(d, &a)) {
        uint32_t id  = pci_cfg_read32(a.bus, a.dev, a.fn, 0x00);
        uint32_t cls = pci_cfg_read32(a.bus, a.dev, a.fn, 0x08);
        tool_result_printf(r,
            "pci=%02x:%02x.%x vendor=0x%04x device=0x%04x class=%02x:%02x prog_if=0x%02x rev=0x%02x (%s)\n"
            "hint: pci_info addr=%02x:%02x.%x for BARs and capabilities\n",
            a.bus, a.dev, a.fn, (unsigned)(id & 0xFFFF), (unsigned)(id >> 16),
            (unsigned)((cls >> 24) & 0xFF), (unsigned)((cls >> 16) & 0xFF),
            (unsigned)((cls >> 8) & 0xFF), (unsigned)(cls & 0xFF),
            pci_class_desc((uint8_t)(cls >> 24), (uint8_t)(cls >> 16)),
            a.bus, a.dev, a.fn);
    }

    trace_ok("device_info", "%s class=%s state=%s driver=%s",
             d->obj.name ? d->obj.name : "?", device_class_name(d->cls),
             device_state_name(d->state),
             d->driver && d->driver->name ? d->driver->name : "-");
    return TOOL_OK;
}

static const tool_t device_info_tool = {
    .name = "device_info",
    .description =
        "Describe one registered device in full: class, state, the bound driver "
        "and which lifecycle hooks that driver actually implements (so you can "
        "tell in advance whether device_suspend will work), bus parent, and every "
        "MMIO/port/IRQ resource. For a PCI-backed device it also performs a live "
        "config-space read of the vendor/device/class IDs.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"device name as printed by device_list, e.g. \\\"eth0\\\" or \\\"pci00:03.0\\\"\"}"
        "},\"required\":[\"name\"]}",
    .flags  = 0,
    .invoke = t_device_info,
};
REGISTER_TOOL(device_info_tool);

/* ====================================================================== */
/* pci_list                                                               */
/* ====================================================================== */

static int t_pci_list(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "pci_list", "", "input must be a JSON object");

    int64_t bus_filter = -1, vendor_filter = -1, class_filter = -1, lim = LIST_LIMIT_DEF;
    int     unclaimed_only = 0, rc;

    rc = f_int(&in, "bus", 0, 255, &bus_filter);
    if (rc < 0) return fail(r, TOOL_EINVAL, "pci_list", "", "\"bus\" must be an integer 0-255");
    if (rc == 0) bus_filter = -1;

    rc = f_int(&in, "vendor", 0, 0xFFFF, &vendor_filter);
    if (rc < 0) return fail(r, TOOL_EINVAL, "pci_list", "", "\"vendor\" must be an integer 0-65535 (e.g. 32902 for 0x8086)");
    if (rc == 0) vendor_filter = -1;

    rc = f_int(&in, "class", 0, 0xFF, &class_filter);
    if (rc < 0) return fail(r, TOOL_EINVAL, "pci_list", "", "\"class\" must be an integer 0-255 (PCI base class, e.g. 2 for network)");
    if (rc == 0) class_filter = -1;

    if (f_bool(&in, "unclaimed_only", &unclaimed_only) < 0)
        return fail(r, TOOL_EINVAL, "pci_list", "", "\"unclaimed_only\" must be a boolean");

    rc = f_int(&in, "limit", 1, LIST_LIMIT_MAX, &lim);
    if (rc < 0) return fail(r, TOOL_EINVAL, "pci_list", "", "\"limit\" must be an integer 1-%d", LIST_LIMIT_MAX);

    int lo = bus_filter < 0 ? 0 : (int)bus_filter;
    int hi = bus_filter < 0 ? 255 : (int)bus_filter;

    int found = 0, shown = 0, unclaimed = 0, stopped = 0;

    for (int bus = lo; bus <= hi; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                pciaddr_t a = { (uint8_t)bus, (uint8_t)dev, (uint8_t)fn };
                uint32_t id = pci_cfg_read32(a.bus, a.dev, a.fn, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF || vendor == 0x0000) {
                    if (fn == 0) break;      /* function 0 absent => no device */
                    continue;
                }

                uint32_t clsreg = pci_cfg_read32(a.bus, a.dev, a.fn, 0x08);
                uint8_t  base   = (uint8_t)(clsreg >> 24);
                uint8_t  sub    = (uint8_t)(clsreg >> 16);

                if (vendor_filter >= 0 && vendor != (uint16_t)vendor_filter) continue;
                if (class_filter  >= 0 && base   != (uint8_t)class_filter)   continue;

                pci_owner_t own;
                pci_owner(&a, &own);
                int driven = owner_is_driven(&own);
                if (unclaimed_only && driven) continue;

                found++;
                if (!driven) unclaimed++;

                if (stopped || shown >= (int)lim || room_low(r)) { stopped = 1; continue; }

                uint32_t hdrreg = pci_cfg_read32(a.bus, a.dev, a.fn, 0x0C);

                /* Show the first BAR that is actually programmed. Not always
                 * BAR0 — a PIIX IDE function, for instance, puts its only
                 * window in BAR4, and a model needs to see that it has one. */
                bar_t b0;
                int   b0idx = -1;
                for (int i = 0; i < 6; i++) {
                    decode_bar(&a, i, &b0);
                    if (b0.present) { b0idx = i; break; }
                    if (b0.is64) i++;
                }

                tool_result_printf(r,
                    "%02x:%02x.%x %04x:%04x class=%02x:%02x prog_if=%02x rev=%02x hdr=0x%02x",
                    a.bus, a.dev, a.fn, (unsigned)vendor, (unsigned)(id >> 16),
                    (unsigned)base, (unsigned)sub,
                    (unsigned)((clsreg >> 8) & 0xFF), (unsigned)(clsreg & 0xFF),
                    (unsigned)((hdrreg >> 16) & 0xFF));
                if (b0idx >= 0)
                    tool_result_printf(r, " bar%d=%s:0x%lx", b0idx,
                                       b0.is_io ? "io" : "mem", (unsigned long)b0.base);
                put_owner(r, &own);
                tool_result_printf(r, " %s\n", pci_class_desc(base, sub));
                shown++;
            }

    if (stopped)
        tool_result_printf(r, "...%d more not shown (raise \"limit\" or filter by bus/class/vendor)\n",
                           found - shown);
    tool_result_printf(r, "pci: %d function(s) matched, %d shown, %d unclaimed"
                          " (no driver bound - candidates for a new driver)\n",
                       found, shown, unclaimed);

    char args[96];
    snprintf(args, sizeof args, "bus=%d-%d vendor=%d class=%d unclaimed_only=%d",
             lo, hi, (int)vendor_filter, (int)class_filter, unclaimed_only);
    trace_ret(found, "pci_list", "%s", args);
    return TOOL_OK;
}

static const tool_t pci_list_tool = {
    .name = "pci_list",
    .description =
        "Enumerate the PCI bus by reading real configuration space right now. "
        "Reports for each function: address bb:dd.f, vendor:device ID, "
        "class:subclass, prog-if, revision, header type, its first programmed "
        "BAR, and which driver "
        "(if any) owns it. unclaimed_only=true lists hardware no driver has "
        "claimed - the starting point for writing a new driver.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"bus\":{\"type\":\"integer\",\"description\":\"scan only this bus, 0-255 (default: all buses)\"},"
        "\"vendor\":{\"type\":\"integer\",\"description\":\"only this PCI vendor ID, 0-65535\"},"
        "\"class\":{\"type\":\"integer\",\"description\":\"only this PCI base class, 0-255 (1=storage, 2=network, 3=display, 6=bridge, 12=serial bus)\"},"
        "\"unclaimed_only\":{\"type\":\"boolean\",\"description\":\"only functions with no driver bound\"},"
        "\"limit\":{\"type\":\"integer\",\"description\":\"max rows to print, 1-256 (default 64)\"}"
        "},\"required\":[]}",
    .flags  = 0,
    .invoke = t_pci_list,
};
REGISTER_TOOL(pci_list_tool);

/* ====================================================================== */
/* pci_info                                                               */
/* ====================================================================== */

static int t_pci_info(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "pci_info", "", "input must be a JSON object");

    pciaddr_t a;
    if (want_addr(&in, r, "pci_info", &a) != 0) return TOOL_EINVAL;

    char args[32];
    snprintf(args, sizeof args, "%02x:%02x.%x", a.bus, a.dev, a.fn);

    if (!pci_present(&a))
        return fail(r, TOOL_ENOENT, "pci_info", args,
                    "no device at %s (vendor ID reads 0xffff); use pci_list to see what exists", args);

    uint32_t id     = pci_cfg_read32(a.bus, a.dev, a.fn, 0x00);
    uint32_t cmdst  = pci_cfg_read32(a.bus, a.dev, a.fn, 0x04);
    uint32_t clsreg = pci_cfg_read32(a.bus, a.dev, a.fn, 0x08);
    uint32_t hdrreg = pci_cfg_read32(a.bus, a.dev, a.fn, 0x0C);
    uint32_t irqreg = pci_cfg_read32(a.bus, a.dev, a.fn, 0x3C);

    uint16_t vendor = (uint16_t)(id & 0xFFFF), device = (uint16_t)(id >> 16);
    uint16_t cmd    = (uint16_t)(cmdst & 0xFFFF), status = (uint16_t)(cmdst >> 16);
    uint8_t  base   = (uint8_t)(clsreg >> 24), sub = (uint8_t)(clsreg >> 16);
    uint8_t  hdr    = (uint8_t)((hdrreg >> 16) & 0xFF);

    tool_result_printf(r, "addr=%02x:%02x.%x vendor=0x%04x device=0x%04x\n",
                       a.bus, a.dev, a.fn, (unsigned)vendor, (unsigned)device);
    tool_result_printf(r, "class=%02x subclass=%02x prog_if=%02x revision=%02x %s\n",
                       (unsigned)base, (unsigned)sub,
                       (unsigned)((clsreg >> 8) & 0xFF), (unsigned)(clsreg & 0xFF),
                       pci_class_desc(base, sub));
    tool_result_printf(r, "header_type=0x%02x multifunction=%s\n",
                       (unsigned)(hdr & 0x7F), (hdr & 0x80) ? "yes" : "no");
    tool_result_printf(r, "command=0x%04x [%s%s%s%s] status=0x%04x\n",
                       (unsigned)cmd,
                       (cmd & 1) ? "io " : "", (cmd & 2) ? "mem " : "",
                       (cmd & 4) ? "busmaster " : "",
                       (cmd & 0x400) ? "intx-disabled" : "intx-enabled",
                       (unsigned)status);

    if ((hdr & 0x7F) == 0x00) {
        uint32_t ss = pci_cfg_read32(a.bus, a.dev, a.fn, 0x2C);
        tool_result_printf(r, "subsystem=%04x:%04x\n",
                           (unsigned)(ss & 0xFFFF), (unsigned)(ss >> 16));
    }
    tool_result_printf(r, "irq_line=%u irq_pin=%u\n",
                       (unsigned)(irqreg & 0xFF), (unsigned)((irqreg >> 8) & 0xFF));

    /* Type 0 headers have BAR0-5; type 1 (PCI-PCI bridge) only BAR0-1. */
    int nbars = ((hdr & 0x7F) == 0x01) ? 2 : 6;
    for (int i = 0; i < nbars; i++) {
        bar_t b;
        decode_bar(&a, i, &b);
        if (!b.present) {
            tool_result_printf(r, "bar%d unused raw=0x%08lx\n", i, (unsigned long)b.raw);
        } else if (b.is_io) {
            tool_result_printf(r, "bar%d io base=0x%lx raw=0x%08lx\n",
                               i, (unsigned long)b.base, (unsigned long)b.raw);
        } else {
            tool_result_printf(r, "bar%d mem%d base=0x%lx prefetch=%s raw=0x%08lx\n",
                               i, b.is64 ? 64 : 32, (unsigned long)b.base,
                               b.prefetch ? "yes" : "no", (unsigned long)b.raw);
        }
        if (b.is64 && i + 1 < nbars) {
            tool_result_printf(r, "bar%d (upper half of bar%d)\n", i + 1, i);
            i++;
        }
    }
    tool_result_printf(r, "note: BAR sizes are not probed - sizing requires writing "
                          "config space, which this kernel does not permit yet\n");

    /* Capability chain: only valid when the status register advertises it. */
    if (status & 0x10) {
        uint8_t off = (uint8_t)(pci_cfg_read32(a.bus, a.dev, a.fn, 0x34) & 0xFF);
        int     guard = 0;
        tool_result_printf(r, "capabilities:");
        int any = 0;
        while (off >= 0x40 && off <= 0xFC && guard++ < 48 && !room_low(r)) {
            uint32_t capreg = pci_cfg_read32(a.bus, a.dev, a.fn, (uint8_t)(off & 0xFC));
            uint8_t  capid  = (uint8_t)(capreg & 0xFF);
            uint8_t  next   = (uint8_t)((capreg >> 8) & 0xFF);
            tool_result_printf(r, " 0x%02x/%s@0x%02x", (unsigned)capid,
                               pci_cap_name(capid), (unsigned)off);
            any = 1;
            if (next == off) break;              /* malformed chain: no loops */
            off = next;
        }
        if (!any) tool_result_printf(r, " (empty)");
        tool_result_printf(r, "\n");
    } else {
        tool_result_printf(r, "capabilities: none advertised\n");
    }

    pci_owner_t own;
    pci_owner(&a, &own);
    tool_result_printf(r, "device_node=%s",
                       own.node && own.node->obj.name ? own.node->obj.name : "-");
    if (own.node)
        tool_result_printf(r, " state=%s", device_state_name(own.node->state));
    put_owner(r, &own);
    if (!owner_is_driven(&own))
        tool_result_printf(r, " UNCLAIMED");
    tool_result_printf(r, "\n");

    trace_ok("pci_info", "%s %04x:%04x class=%02x:%02x driven=%d",
             args, (unsigned)vendor, (unsigned)device,
             (unsigned)base, (unsigned)sub, owner_is_driven(&own));
    return TOOL_OK;
}

static const tool_t pci_info_tool = {
    .name = "pci_info",
    .description =
        "Fully decode one PCI function from live config space: vendor/device IDs, "
        "class/subclass/prog-if/revision, command and status registers, subsystem "
        "IDs, interrupt line and pin, every base address register (BAR) with its "
        "type (32- or 64-bit MMIO, or I/O ports) and prefetchability, the "
        "capability chain (MSI, MSI-X, power management, PCIe...), and whether a "
        "driver owns it. This is what you read before writing a driver for an "
        "unclaimed device. BAR sizes are not reported - probing them requires "
        "config-space writes, which are not permitted.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"addr\":{\"type\":\"string\",\"description\":\"hex PCI address \\\"bb:dd.f\\\" exactly as pci_list prints it, e.g. \\\"00:03.0\\\"\"},"
        "\"bus\":{\"type\":\"integer\",\"description\":\"bus 0-255 (alternative to addr)\"},"
        "\"device\":{\"type\":\"integer\",\"description\":\"device 0-31 (alternative to addr)\"},"
        "\"function\":{\"type\":\"integer\",\"description\":\"function 0-7 (default 0)\"}"
        "},\"required\":[]}",
    .flags  = 0,
    .invoke = t_pci_info,
};
REGISTER_TOOL(pci_info_tool);

/* ====================================================================== */
/* pci_config_read                                                        */
/* ====================================================================== */

static int t_pci_config_read(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "pci_config_read", "", "input must be a JSON object");

    pciaddr_t a;
    if (want_addr(&in, r, "pci_config_read", &a) != 0) return TOOL_EINVAL;

    char args[64];
    snprintf(args, sizeof args, "%02x:%02x.%x", a.bus, a.dev, a.fn);

    int64_t off = 0, width = 4, count = 1;
    int     rc  = f_int(&in, "offset", 0, 255, &off);
    if (rc == 0)  return fail(r, TOOL_EINVAL, "pci_config_read", args,
                              "\"offset\" is required (byte offset 0-255 into config space)");
    if (rc == -1) return fail(r, TOOL_EINVAL, "pci_config_read", args, "\"offset\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "pci_config_read", args,
                              "\"offset\" out of range: PCI config space is 256 bytes, so 0-255");

    rc = f_int(&in, "width", 1, 4, &width);
    if (rc < 0) return fail(r, TOOL_EINVAL, "pci_config_read", args,
                            "\"width\" must be 1, 2 or 4 bytes");
    if (width != 1 && width != 2 && width != 4)
        return fail(r, TOOL_EINVAL, "pci_config_read", args,
                    "\"width\" must be 1, 2 or 4 bytes, not %d", (int)width);

    rc = f_int(&in, "count", 1, CFG_COUNT_MAX, &count);
    if (rc < 0) return fail(r, TOOL_EINVAL, "pci_config_read", args,
                            "\"count\" must be an integer 1-%d", CFG_COUNT_MAX);

    if (off % width != 0)
        return fail(r, TOOL_EINVAL, "pci_config_read", args,
                    "offset 0x%02x is not %d-byte aligned", (unsigned)off, (int)width);
    if (off + width * count > 256)
        return fail(r, TOOL_EINVAL, "pci_config_read", args,
                    "read of %d x %d bytes at offset 0x%02x runs past the end of "
                    "config space (256 bytes)", (int)count, (int)width, (unsigned)off);

    if (!pci_present(&a))
        return fail(r, TOOL_ENOENT, "pci_config_read", args,
                    "no device at %s (vendor ID reads 0xffff); every read there would "
                    "return all-ones", args);

    tool_result_printf(r, "%s offset=0x%02x width=%d count=%d\n",
                       args, (unsigned)off, (int)width, (int)count);

    uint32_t first = 0;
    int      percol = width == 4 ? 4 : (width == 2 ? 8 : 16);
    for (int i = 0; i < (int)count; i++) {
        uint32_t byteoff = (uint32_t)off + (uint32_t)(i * width);
        uint32_t dword   = pci_cfg_read32(a.bus, a.dev, a.fn, (uint8_t)(byteoff & 0xFC));
        uint32_t shift   = (byteoff & 3u) * 8u;
        uint32_t val     = width == 4 ? dword
                         : width == 2 ? ((dword >> shift) & 0xFFFFu)
                                      : ((dword >> shift) & 0xFFu);
        if (i == 0) first = val;
        if (i % percol == 0) tool_result_printf(r, "0x%02x:", (unsigned)byteoff);
        tool_result_printf(r, width == 4 ? " 0x%08lx" : (width == 2 ? " 0x%04lx" : " 0x%02lx"),
                           (unsigned long)val);
        if (i % percol == percol - 1 || i == (int)count - 1) tool_result_printf(r, "\n");
        if (room_low(r) && i + 1 < (int)count) {
            tool_result_printf(r, "...truncated after %d value(s)\n", i + 1);
            break;
        }
    }

    /* The kernel's vsnprintf has no '*' (dynamic width) conversion, so pick a
     * literal format per access width rather than passing the width in. */
    if (count == 1)
        trace_ok("pci_config_read",
                 width == 4 ? "%s off=0x%02x w=%d val=0x%08lx"
                            : (width == 2 ? "%s off=0x%02x w=%d val=0x%04lx"
                                          : "%s off=0x%02x w=%d val=0x%02lx"),
                 args, (unsigned)off, (int)width, (unsigned long)first);
    else
        trace_ok("pci_config_read",
                 width == 4 ? "%s off=0x%02x w=%d n=%d first=0x%08lx"
                            : (width == 2 ? "%s off=0x%02x w=%d n=%d first=0x%04lx"
                                          : "%s off=0x%02x w=%d n=%d first=0x%02lx"),
                 args, (unsigned)off, (int)width, (int)count, (unsigned long)first);
    return TOOL_OK;
}

static const tool_t pci_config_read_tool = {
    .name = "pci_config_read",
    .description =
        "Read raw bytes from a PCI function's 256-byte configuration space. Use "
        "this for registers pci_info does not decode - vendor-specific capability "
        "blocks, expansion ROM base, bridge windows. Reads only: this kernel "
        "exposes no config-space write, because a stray write can move a live "
        "device's BAR or disable its decoding with no way to recover. Offsets must "
        "be aligned to the access width and the whole read must stay inside 256 bytes.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"addr\":{\"type\":\"string\",\"description\":\"hex PCI address \\\"bb:dd.f\\\", e.g. \\\"00:03.0\\\"\"},"
        "\"bus\":{\"type\":\"integer\",\"description\":\"bus 0-255 (alternative to addr)\"},"
        "\"device\":{\"type\":\"integer\",\"description\":\"device 0-31 (alternative to addr)\"},"
        "\"function\":{\"type\":\"integer\",\"description\":\"function 0-7 (default 0)\"},"
        "\"offset\":{\"type\":\"integer\",\"description\":\"byte offset 0-255, must be a multiple of width\"},"
        "\"width\":{\"type\":\"integer\",\"description\":\"bytes per value: 1, 2 or 4 (default 4)\"},"
        "\"count\":{\"type\":\"integer\",\"description\":\"how many consecutive values to read, 1-64 (default 1)\"}"
        "},\"required\":[\"offset\"]}",
    .flags  = 0,
    .invoke = t_pci_config_read,
};
REGISTER_TOOL(pci_config_read_tool);

/* ====================================================================== */
/* device_suspend / device_resume                                         */
/* ====================================================================== */

/* One implementation; `up` selects resume. Mirrors what drivers.c does for the
 * whole registry, but for a single named device and with every refusal turned
 * into a sentence the model can act on. */
static int power_transition(const tool_call_t *c, tool_result_t *r, int up) {
    const char *op   = up ? "device_resume" : "device_suspend";
    const char *verb = up ? "resume" : "suspend";

    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, op, "", "input must be a JSON object");

    char name[DEVNAME_MAX + 1];
    int  rc = f_str(&in, "name", name, sizeof name);
    if (rc == 0)  return fail(r, TOOL_EINVAL, op, "", "\"name\" is required (see device_list)");
    if (rc == -1) return fail(r, TOOL_EINVAL, op, "", "\"name\" must be a string");
    if (rc == -2) return fail(r, TOOL_ENOENT, op, "<oversized>",
                              "no such device: name longer than %d characters", DEVNAME_MAX);

    device_t *d = dev_by_name(name);
    if (!d) return fail(r, TOOL_ENOENT, op, name,
                        "no such device \"%s\"; use device_list to see registered names", name);

    driver_t *drv = d->driver;
    if (!drv)
        return fail(r, TOOL_EINVAL, op, name,
                    "%s has no driver bound, so there is nothing to %s it", name, verb);

    int (*hook)(device_t *) = up ? drv->resume : drv->suspend;
    if (!hook)
        return fail(r, TOOL_EINVAL, op, name,
                    "driver \"%s\" implements no %s hook; %s cannot be powered %s",
                    drv->name ? drv->name : "?", verb, name, up ? "up" : "down");

    device_state_t want_from = up ? DEV_STATE_SUSPENDED : DEV_STATE_ACTIVE;
    if (d->state != want_from)
        return fail(r, TOOL_EINVAL, op, name,
                    "%s is %s; %s requires state %s", name,
                    device_state_name(d->state), verb, device_state_name(want_from));

    const char *before = device_state_name(d->state);
    int hrc = hook(d);
    if (hrc != 0)
        return fail(r, TOOL_EINVAL, op, name,
                    "driver \"%s\" %s hook failed (rc=%d); %s left %s",
                    drv->name ? drv->name : "?", verb, hrc, name, before);

    device_set_state(d, up ? DEV_STATE_ACTIVE : DEV_STATE_SUSPENDED);

    tool_result_printf(r, "%s: state %s -> %s (driver %s %s hook returned 0)\n",
                       name, before, device_state_name(d->state),
                       drv->name ? drv->name : "?", verb);
    trace_ok(op, "%s driver=%s %s->%s", name, drv->name ? drv->name : "?",
             before, device_state_name(d->state));
    return TOOL_OK;
}

static int t_device_suspend(const tool_call_t *c, tool_result_t *r) {
    return power_transition(c, r, 0);
}
static int t_device_resume(const tool_call_t *c, tool_result_t *r) {
    return power_transition(c, r, 1);
}

static const tool_t device_suspend_tool = {
    .name = "device_suspend",
    .description =
        "Power down one device by running its driver's suspend hook, then mark it "
        "SUSPENDED. This really stops the hardware - do not use it on a device the "
        "current conversation depends on. Only works on a device that is ACTIVE and "
        "whose driver implements suspend; device_info reports suspendable=yes/no.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"device name as printed by device_list, e.g. \\\"kbd0\\\"\"}"
        "},\"required\":[\"name\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_device_suspend,
};
REGISTER_TOOL(device_suspend_tool);

static const tool_t device_resume_tool = {
    .name = "device_resume",
    .description =
        "Bring a SUSPENDED device back up by running its driver's resume hook, then "
        "mark it ACTIVE. Fails legibly if the device is not suspended or its driver "
        "has no resume hook.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"name\":{\"type\":\"string\",\"description\":\"device name as printed by device_list, e.g. \\\"kbd0\\\"\"}"
        "},\"required\":[\"name\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_device_resume,
};
REGISTER_TOOL(device_resume_tool);
