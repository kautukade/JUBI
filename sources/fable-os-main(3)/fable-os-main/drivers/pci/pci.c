/* pci.c — PCI bus driver: config-space access, device enumeration, lookup. */

#include "pci.h"
#include "driver.h"
#include "device.h"
#include "kernel.h"
#include "io.h"
#include <stdio.h>

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

/* Map a PCI base-class code to a generic device class. */
static device_class_t pci_class_to_dev(uint8_t pci_class) {
    switch (pci_class) {
        case 0x01: return DEV_CLASS_STORAGE;
        case 0x02: return DEV_CLASS_NETWORK;
        case 0x03: return DEV_CLASS_DISPLAY;
        case 0x04: return DEV_CLASS_AUDIO;
        case 0x06: return DEV_CLASS_BUS;      /* bridge */
        default:   return DEV_CLASS_SYSTEM;
    }
}

/* CONFIG_ADDRESS gives the device field 5 bits (15:11) and the function field 3
 * (10:8). Masking here rather than trusting each caller is the point: a dev >= 32
 * would spill into the bus field and an fn >= 8 into the device field, so a read
 * reported as "00:20.0" would quietly return bus 1 device 0 — and
 * pci_cfg_write32() would WRITE it. Model-supplied numbers reach this primitive
 * (tools/dev_tools.c, vm/dvm.c), and they each clamp already; the address
 * builder is the one place where it cannot be forgotten. */
static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (1u << 31) | ((uint32_t)bus << 16) |
           ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(fn & 0x07) << 8) |
           (off & 0xFC);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(CONFIG_ADDR, cfg_addr(bus, dev, fn, off));
    return inl(CONFIG_DATA);
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    outl(CONFIG_ADDR, cfg_addr(bus, dev, fn, off));
    outl(CONFIG_DATA, val);
}

uint32_t pci_read32(const pci_dev_t *d, uint8_t off) {
    return pci_cfg_read32(d->bus, d->dev, d->func, off);
}
void pci_write32(const pci_dev_t *d, uint8_t off, uint32_t val) {
    pci_cfg_write32(d->bus, d->dev, d->func, off, val);
}

/* Read one function's identity. Returns 0 when nothing answers.
 * Named for what it does: driver.h's `probe` is the driver-lifecycle hook that
 * binds a driver to a device_t, which is a different thing entirely. */
static int read_function(uint8_t bus, uint8_t dev, uint8_t fn, pci_dev_t *out) {
    uint32_t id = pci_cfg_read32(bus, dev, fn, 0x00);
    uint16_t vendor = id & 0xFFFF;
    if (vendor == 0xFFFF) return 0;
    uint32_t cls = pci_cfg_read32(bus, dev, fn, 0x08);
    out->bus = bus; out->dev = dev; out->func = fn;
    out->vendor = vendor; out->device = id >> 16;
    out->class = (cls >> 24) & 0xFF;
    out->subclass = (cls >> 16) & 0xFF;
    return 1;
}

/* Walk every PCI function that answers, in bus/device/function order, calling
 * `visit` for each. Stops and returns the first nonzero `visit` result.
 *
 * One copy on purpose: this loop, including the multifunction shortcut below,
 * used to be written out three times (pci_find_vendor, pci_find_class,
 * pci_init), which left nowhere to fix the walk itself. */
static int pci_for_each(int (*visit)(const pci_dev_t *d, void *ctx), void *ctx) {
    for (uint16_t bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            for (uint8_t fn = 0; fn < 8; fn++) {
                pci_dev_t d;
                if (!read_function((uint8_t)bus, dev, fn, &d)) {
                    /* Nothing at function 0 means the slot is empty, so its
                     * other seven function numbers cannot hold anything. */
                    if (fn == 0) break;
                    continue;
                }
                int rc = visit(&d, ctx);
                if (rc) return rc;
            }
    return 0;
}

typedef struct { uint16_t vendor, device; pci_dev_t *out; } match_id_t;

static int visit_match_id(const pci_dev_t *d, void *ctx) {
    match_id_t *q = ctx;
    if (d->vendor != q->vendor || d->device != q->device) return 0;
    *q->out = *d;
    return 1;
}

int pci_find_vendor(uint16_t vendor, uint16_t device, pci_dev_t *out) {
    match_id_t q = { vendor, device, out };
    return pci_for_each(visit_match_id, &q);
}

typedef struct { uint8_t base_class, subclass; pci_dev_t *out; } match_cls_t;

static int visit_match_cls(const pci_dev_t *d, void *ctx) {
    match_cls_t *q = ctx;
    if (d->class != q->base_class || d->subclass != q->subclass) return 0;
    *q->out = *d;
    return 1;
}

int pci_find_class(uint8_t base_class, uint8_t subclass, pci_dev_t *out) {
    match_cls_t q = { base_class, subclass, out };
    return pci_for_each(visit_match_cls, &q);
}

void pci_enable_bus_master(const pci_dev_t *d) {
    uint32_t cmd = pci_read32(d, 0x04);
    cmd |= (1 << 1) | (1 << 2);   /* memory space + bus master */
    pci_write32(d, 0x04, cmd);
}

uint32_t pci_bar(const pci_dev_t *d, int bar) {
    /* Valid type-0 BAR indices are 0..5. bar == 6 would read the cardbus CIS
     * pointer, bar == 9 the capability pointer, and a negative index reads
     * backwards out of the BAR window entirely. */
    if (bar < 0 || bar > 5) return 0;

    uint32_t v = pci_read32(d, (uint8_t)(0x10 + bar * 4));

    /* Report a base address ONLY for a nonzero 32-bit memory BAR, because that
     * is the only thing a caller can safely map. Masking the type bits off and
     * handing back whatever was there is how an I/O port number gets treated as
     * an MMIO address, and how an unassigned BAR becomes a mapping at physical
     * 0 — which on this kernel does not fault, since the low 4 GiB is mapped
     * writable. Callers that want the raw dword use pci_read32(d, 0x10 + 4n). */
    if (v & 1)             return 0;   /* I/O space, not memory              */
    if ((v & 0x6) == 0x4)  return 0;   /* 64-bit BAR: the pair is not decoded */
    return v & ~0xFu;                  /* 0 when the BAR is unassigned        */
}

/* Publish an enumerated PCI function into the device model. */
static void pci_publish(const pci_dev_t *d, device_t *bus) {
    /* device_create() stores the name by pointer, so it has to outlive the
     * device; devices are never destroyed, so this allocation is deliberately
     * never freed. 16 bytes holds "pciBB:DD.F" (10 chars) with room to spare. */
    char *name = kmalloc(16);
    snprintf(name, 16, "pci%02x:%02x.%x", d->bus, d->dev, d->func);

    device_t *dev = device_create(name, pci_class_to_dev(d->class));
    dev->parent = bus;

    /* Advertise BAR0 as a resource so downstream drivers/tools can see it. */
    uint32_t bar0 = pci_read32(d, 0x10);
    if (bar0 & 1)
        device_add_resource(dev, RES_IOPORT, bar0 & ~0x3u, 0);
    else if (bar0 & ~0xFu)
        device_add_resource(dev, RES_MMIO, bar0 & ~0xFu, 0);

    device_register(dev);
}

static const driver_t pci_driver;

typedef struct { device_t *bus; int n; } enumerate_t;

static int visit_publish(const pci_dev_t *d, void *ctx) {
    enumerate_t *e = ctx;
    kprintf("pci: %02x:%02x.%x  %04x:%04x  class %02x:%02x\n",
            d->bus, d->dev, d->func, d->vendor, d->device, d->class, d->subclass);
    pci_publish(d, e->bus);
    e->n++;
    return 0;                     /* 0 = keep walking */
}

static int pci_init(void) {
    device_t *bus = device_create("pci0", DEV_CLASS_BUS);
    device_register(bus);
    device_bind_driver(bus, (driver_t *)&pci_driver);
    device_set_state(bus, DEV_STATE_ACTIVE);

    enumerate_t e = { bus, 0 };
    pci_for_each(visit_publish, &e);
    kprintf("pci: %d device(s)\n", e.n);
    return 0;
}

static const driver_t pci_driver = {
    .name = "pci",
    .level = DRV_LEVEL_BUS,
    .cls = DEV_CLASS_BUS,
    .init = pci_init,
};
REGISTER_DRIVER(pci_driver);
