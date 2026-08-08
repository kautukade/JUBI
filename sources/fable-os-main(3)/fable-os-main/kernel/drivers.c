/* drivers.c — driver manager: runs driver lifecycle and dispatches power
 * transitions across the device registry. See include/driver.h. */

#include "driver.h"
#include "device.h"
#include "kernel.h"
#include <stdio.h>   /* snprintf: base.c's kprintf lacks %-Ns left-align */

/* Provided by the linker (see linker.ld / GNU ld __start_/__stop_ symbols). */
extern const driver_t *const __start_driver_table[];
extern const driver_t *const __stop_driver_table[];

static const driver_t *const *drv_base;
static int drv_count;

static void collect(void) {
    drv_base  = __start_driver_table;
    drv_count = (int)(__stop_driver_table - __start_driver_table);
}

void drivers_init(void) {
    collect();
    for (int level = DRV_LEVEL_EARLY; level <= DRV_LEVEL_LATE; level++) {
        for (int i = 0; i < drv_count; i++) {
            const driver_t *d = drv_base[i];
            if (d->level != level) continue;
            int rc = d->init ? d->init() : 0;
            if (rc != 0)
                kprintf("driver %s: init failed (%d)\n", d->name, rc);
        }
    }
}

driver_t *driver_find_for_class(device_class_t cls) {
    collect();
    for (int i = 0; i < drv_count; i++)
        if (drv_base[i]->cls == cls) return (driver_t *)drv_base[i];
    return NULL;
}

static void report_one(device_t *d, void *ctx) {
    (void)ctx;
    char line[96];
    snprintf(line, sizeof line, "  [%2u] %-10s %-8s %-11s driver=%-6s",
             (unsigned)d->obj.id, d->obj.name,
             device_class_name(d->cls), device_state_name(d->state),
             d->driver ? d->driver->name : "-");
    kputs(line);
    for (int i = 0; i < d->nres; i++) {
        const device_resource_t *r = &d->res[i];
        switch (r->type) {
            case RES_MMIO:   kprintf(" mmio=%p", (void *)(uintptr_t)r->base); break;
            case RES_IOPORT: kprintf(" io=%x", (unsigned)r->base); break;
            case RES_IRQ:    kprintf(" irq=%u", (unsigned)r->base); break;
            default: break;
        }
    }
    kputc('\n');
}

void driver_report(void) {
    kprintf("devices: %d registered\n", device_count());
    device_foreach(report_one, NULL);
}

void drivers_suspend_all(void) {
    collect();
    for (int i = 0; i < drv_count; i++) {
        const driver_t *drv = drv_base[i];
        if (!drv->suspend) continue;
        for (int idx = 0;; idx++) {
            device_t *d = device_find_by_class(drv->cls, idx);
            if (!d) break;
            if (d->driver == drv && d->state == DEV_STATE_ACTIVE) {
                if (drv->suspend(d) == 0) device_set_state(d, DEV_STATE_SUSPENDED);
            }
        }
    }
}

void drivers_resume_all(void) {
    collect();
    for (int i = 0; i < drv_count; i++) {
        const driver_t *drv = drv_base[i];
        if (!drv->resume) continue;
        for (int idx = 0;; idx++) {
            device_t *d = device_find_by_class(drv->cls, idx);
            if (!d) break;
            if (d->driver == drv && d->state == DEV_STATE_SUSPENDED) {
                if (drv->resume(d) == 0) device_set_state(d, DEV_STATE_ACTIVE);
            }
        }
    }
}
