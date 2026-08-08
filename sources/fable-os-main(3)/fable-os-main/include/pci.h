/* pci.h — PCI configuration space: access, enumeration, lookup.
 *
 * PURPOSE
 *   The bus every other piece of hardware on this machine is discovered
 *   through. It provides the two config-space primitives (read/write a dword at
 *   a bus:device.function) and, on top of them, enumeration into the device
 *   model and lookup by identity or class.
 *
 * RESPONSIBILITIES
 *   - Build a legal CONFIG_ADDRESS and read/write CONFIG_DATA.
 *   - Enumerate the bus once at bring-up, printing each function and publishing
 *     a device_t for it under the pci0 bus node.
 *   - Answer "where is the device with these IDs / this class".
 *   - Decode BAR 0..5 and enable bus mastering.
 *
 * PUBLIC API
 *   pci_cfg_read32 / pci_cfg_write32  — by bus/dev/fn, the raw primitives.
 *   pci_read32 / pci_write32          — the same, given a pci_dev_t.
 *   pci_find_vendor / pci_find_class  — first match, or 0.
 *   pci_bar                           — mappable base of a memory BAR, or 0.
 *   pci_enable_bus_master             — let a device DMA.
 *
 * DEPENDENCIES
 *   io.h (inl/outl), device.h/driver.h (publication), kernel.h (kprintf, heap).
 *
 * UNTRUSTED INPUT
 *   Model-supplied bus/device/function numbers reach pci_cfg_read32 and
 *   pci_cfg_write32 (tools/dev_tools.c, vm/dvm.c). The device and function
 *   fields are masked to their architectural widths inside the address builder,
 *   so an out-of-range number cannot alias onto a different device; the callers
 *   clamp as well. `off` is masked to a dword boundary. There is no such
 *   protection for `bus`, which is 8 bits wide and therefore always in range.
 *
 * FUTURE EXTENSION POINTS
 *   64-bit BARs need the upper dword from BAR n+1 (pci_bar reports 0 for them
 *   today rather than truncating). PCIe extended config space needs MMCONFIG
 *   rather than the 0xCF8/0xCFC window. Once drivers implement driver.h's
 *   probe() hook, enumeration can bind drivers to the nodes published here
 *   instead of each driver rescanning config space for itself.
 */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor, device;
    /* PCI base class and subclass. Spelled `class` for historical reasons; the
     * rest of the tree uses `cls` (see device.h) and this is the one exception. */
    uint8_t  class, subclass;
} pci_dev_t;

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val);

uint32_t pci_read32(const pci_dev_t *d, uint8_t off);
void     pci_write32(const pci_dev_t *d, uint8_t off, uint32_t val);

/* Find the first matching device. Returns 1 and fills *out, or 0. Both rescan
 * config space; they do not consult the device nodes pci_init() published. */
int  pci_find_vendor(uint16_t vendor, uint16_t device, pci_dev_t *out);
int  pci_find_class(uint8_t base_class, uint8_t subclass, pci_dev_t *out);

void pci_enable_bus_master(const pci_dev_t *d);

/* Mappable base address of BAR `bar`, or 0.
 *
 * Returns nonzero ONLY for an assigned 32-bit MEMORY BAR with a valid index
 * (0..5). An I/O BAR, a 64-bit BAR (whose upper half lives in BAR n+1 and is not
 * decoded here), an unassigned BAR and an out-of-range index all read as 0, so
 * a caller that checks the return cannot end up mapping an I/O port number or
 * dereferencing physical 0 — which on this kernel does not fault, because the
 * low 4 GiB is mapped writable. Callers that want the raw dword, type bits and
 * all, read it with pci_read32(d, 0x10 + 4 * bar). */
uint32_t pci_bar(const pci_dev_t *d, int bar);

#endif /* PCI_H */
