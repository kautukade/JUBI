/* device.c — device registry (see include/device.h). */

#include "device.h"
#include "driver.h"
#include "kernel.h"

static device_t *registry;       /* singly linked list of all devices */
static device_t *registry_tail;  /* so registration is O(1)           */
static int       registry_count;

device_t *device_create(const char *name, device_class_t cls) {
    device_t *d = kmalloc(sizeof *d);
    memset(d, 0, sizeof *d);
    kobject_init(&d->obj, KOBJ_DEVICE, name);
    d->cls   = cls;
    d->state = DEV_STATE_REGISTERED;
    return d;
}

int device_add_resource(device_t *d, device_res_type_t type,
                        uint64_t base, uint64_t size) {
    if (d->nres >= DEV_MAX_RESOURCES) return -1;
    d->res[d->nres].type = type;
    d->res[d->nres].base = base;
    d->res[d->nres].size = size;
    d->nres++;
    return 0;
}

void device_register(device_t *d) {
    if (!d) return;

    /* Genuinely idempotent, as device.h promises. The old version cleared
     * d->next and then walked to the tail — which, for an already-registered
     * device, IS d, so it linked d to itself and every later device_foreach()
     * (the device_list/device_info tools, the boot-time device tree) spun
     * forever. On a kernel with no scheduler that is a permanent hang. */
    for (device_t *t = registry; t; t = t->next)
        if (t == d) return;

    /* Append so enumeration order matches discovery order. */
    d->next = NULL;
    if (registry_tail) registry_tail->next = d;
    else               registry = d;
    registry_tail = d;
    registry_count++;
}

void device_bind_driver(device_t *d, struct driver *drv) {
    d->driver = drv;
    if (d->state == DEV_STATE_REGISTERED) d->state = DEV_STATE_BOUND;
}

void device_set_state(device_t *d, device_state_t state) {
    d->state = state;
}

device_t *device_find_by_class(device_class_t cls, int index) {
    for (device_t *d = registry; d; d = d->next)
        if (d->cls == cls && index-- == 0) return d;
    return NULL;
}

void device_foreach(void (*fn)(device_t *, void *), void *ctx) {
    for (device_t *d = registry; d; d = d->next) fn(d, ctx);
}

int device_count(void) { return registry_count; }

const char *device_class_name(device_class_t cls) {
    switch (cls) {
        case DEV_CLASS_SYSTEM:  return "system";
        case DEV_CLASS_BUS:     return "bus";
        case DEV_CLASS_SERIAL:  return "serial";
        case DEV_CLASS_INPUT:   return "input";
        case DEV_CLASS_DISPLAY: return "display";
        case DEV_CLASS_STORAGE: return "storage";
        case DEV_CLASS_NETWORK: return "network";
        case DEV_CLASS_AUDIO:   return "audio";
        case DEV_CLASS_TIMER:   return "timer";
        default:                return "none";
    }
}

const char *device_state_name(device_state_t state) {
    switch (state) {
        case DEV_STATE_REGISTERED: return "registered";
        case DEV_STATE_BOUND:      return "bound";
        case DEV_STATE_ACTIVE:     return "active";
        case DEV_STATE_SUSPENDED:  return "suspended";
        case DEV_STATE_FAILED:     return "failed";
        default:                   return "unknown";
    }
}
