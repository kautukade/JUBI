/* device.h — generic device model.
 *
 * PURPOSE
 *   A uniform description of every piece of hardware the kernel knows about,
 *   independent of how it was discovered (hardcoded, PCI scan, and later USB).
 *
 * RESPONSIBILITIES
 *   - Describe a device: id/name/class/capabilities/resources/state.
 *   - Bind a device to the driver that manages it.
 *   - Maintain a global registry that can be enumerated and searched.
 *
 * PUBLIC API
 *   device_create / device_add_resource / device_register — build & publish.
 *   device_bind_driver                                    — attach a driver.
 *   device_find_by_class / device_foreach / device_count  — enumerate.
 *   device_set_state / device_class_name / device_state_name.
 *
 * DEPENDENCIES
 *   kobject.h (identity/lifetime), heap (device_create allocates).
 *
 * FUTURE EXTENSION POINTS
 *   A device carries a parent pointer, so PCI/USB topologies form a tree. New
 *   classes and capability bits extend the enums without breaking callers. When
 *   hot-plug arrives, device_unregister + driver->remove + a DEV_STATE_REMOVED
 *   complete the story; none of the three exists yet.
 */
#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>
#include "kobject.h"

struct driver;   /* forward: see driver.h */

typedef enum {
    DEV_CLASS_NONE = 0,
    DEV_CLASS_SYSTEM,     /* host bridge, misc controllers   */
    DEV_CLASS_BUS,        /* PCI/USB bus controller          */
    DEV_CLASS_SERIAL,     /* UART                            */
    DEV_CLASS_INPUT,      /* keyboard, mouse                 */
    DEV_CLASS_DISPLAY,    /* VGA/framebuffer                 */
    DEV_CLASS_STORAGE,    /* disk / block device             */
    DEV_CLASS_NETWORK,    /* NIC                             */
    DEV_CLASS_AUDIO,      /* sound card                      */
    DEV_CLASS_TIMER,      /* PIT/HPET                        */
    DEV_CLASS_MAX,
} device_class_t;

typedef enum {
    DEV_STATE_UNKNOWN = 0,
    DEV_STATE_REGISTERED,   /* known, no driver bound yet    */
    DEV_STATE_BOUND,        /* a driver has claimed it       */
    DEV_STATE_ACTIVE,       /* up and running                */
    DEV_STATE_SUSPENDED,    /* powered down, resumable       */
    DEV_STATE_FAILED,       /* bring-up failed               */
} device_state_t;

typedef enum {
    RES_NONE = 0,
    RES_MMIO,      /* memory-mapped I/O window */
    RES_IOPORT,    /* x86 I/O port range       */
    RES_IRQ,       /* interrupt line           */
} device_res_type_t;

typedef struct device_resource {
    device_res_type_t type;
    uint64_t base;   /* address / port / irq number */
    uint64_t size;   /* bytes / ports (1 for IRQ)   */
} device_resource_t;

#define DEV_MAX_RESOURCES 4

typedef struct device {
    kobject_t          obj;         /* MUST be first (see kobject.h) */
    device_class_t     cls;
    uint32_t           capabilities;/* class-defined bitmask         */
    device_state_t     state;
    struct driver     *driver;      /* bound driver, or NULL         */
    struct device     *parent;      /* bus/topology parent, or NULL  */
    device_resource_t  res[DEV_MAX_RESOURCES];
    int                nres;
    struct device     *next;        /* registry chain                */
} device_t;

/* Allocate and initialise a device (state = REGISTERED). Not yet published.
 *
 * `name` is stored by pointer, NOT copied, so it must outlive the device — a
 * string literal, or a heap buffer the caller never frees (drivers/pci/pci.c
 * formats one per function and keeps it forever). Devices are never destroyed
 * today; a future device_unregister() has to deal with this. */
device_t *device_create(const char *name, device_class_t cls);

/* Attach a resource. Returns 0 on success, -1 if the slot table is full — a
 * dropped resource means the device comes up missing an IRQ or a BAR, so the
 * return is worth checking (it can only fail because the caller asked for more
 * than DEV_MAX_RESOURCES, i.e. it is a source bug, not runtime input). */
int  device_add_resource(device_t *d, device_res_type_t type,
                         uint64_t base, uint64_t size);

/* Publish into the global registry. Idempotent: registering the same device
 * twice is a no-op, not a corrupt list. */
void device_register(device_t *d);

void device_bind_driver(device_t *d, struct driver *drv);
void device_set_state(device_t *d, device_state_t state);

/* Enumeration. index is 0-based within the class; returns NULL past the end. */
device_t *device_find_by_class(device_class_t cls, int index);
void      device_foreach(void (*fn)(device_t *, void *), void *ctx);
int       device_count(void);

const char *device_class_name(device_class_t cls);
const char *device_state_name(device_state_t state);

#endif /* DEVICE_H */
