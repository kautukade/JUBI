/* driver.h — self-registering driver framework + driver manager.
 *
 * PURPOSE
 *   A driver is the code that brings a class of hardware to life. Drivers
 *   register themselves at link time (no central list to edit) and the driver
 *   manager runs their lifecycle in a well-defined order.
 *
 * LIFECYCLE
 *   init()    one-time subsystem bring-up (scan the bus, set up state). Run in
 *             ascending level order so serial/PCI come up before the devices
 *             that depend on them. This is the original, always-present hook.
 *   probe()   bind to a specific device_t and make it active. Optional.
 *   remove()  release a device (hot-unplug / shutdown). Optional.
 *   suspend() / resume()  power management. Optional.
 *
 * Any of probe/remove/suspend/resume may be NULL; the manager skips them.
 *
 * TO ADD A DRIVER
 *   Drop a .c under drivers/<subsystem>/, define a driver_t, REGISTER_DRIVER().
 *   In init() (or probe()) call device_create()/device_register() to publish
 *   the hardware into the device model. That's the whole contract.
 *
 * DEPENDENCIES
 *   device.h (the object a driver operates on). No subsystem reaches into
 *   another's internals — everything goes through device_t and these hooks.
 *
 * FUTURE EXTENSION POINTS
 *   A `match` predicate + the `cls` field let the manager auto-probe drivers
 *   against newly discovered devices (dynamic/hot-plug buses). Interrupt
 *   dispatch will hang off driver->isr once an IDT exists.
 */
#ifndef DRIVER_H
#define DRIVER_H

#include "device.h"

enum {
    DRV_LEVEL_EARLY  = 0,   /* serial console */
    DRV_LEVEL_BUS    = 1,   /* PCI */
    DRV_LEVEL_DEVICE = 2,   /* framebuffer, NIC, keyboard, audio */
    DRV_LEVEL_LATE   = 3,
};

typedef struct driver {
    const char    *name;
    int            level;
    device_class_t cls;              /* class this driver handles (or NONE) */

    int (*init)(void);               /* subsystem bring-up; 0 on success    */
    int (*probe)(device_t *dev);     /* bind to a device;   0 on success    */
    int (*remove)(device_t *dev);
    int (*suspend)(device_t *dev);
    int (*resume)(device_t *dev);
} driver_t;

/* Place a pointer to the driver in the "driver_table" section. KEEP() in the
 * linker script stops it being garbage-collected. */
#define REGISTER_DRIVER(var)                                                   \
    static const driver_t *const __drvptr_##var                                \
        __attribute__((used, section("driver_table"))) = &(var)

/* Driver manager. */
void drivers_init(void);                 /* run every init() in level order  */
void driver_report(void);                /* print the device tree            */
void drivers_suspend_all(void);          /* suspend all bound, active devices */
void drivers_resume_all(void);           /* resume all suspended devices      */
driver_t *driver_find_for_class(device_class_t cls);

#endif /* DRIVER_H */
