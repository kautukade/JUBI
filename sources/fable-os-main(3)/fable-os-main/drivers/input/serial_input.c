/* serial_input.c — COM1 as an input source.
 *
 * The UART receive path lives in drivers/serial/serial.c (it owns the port);
 * this file is the thin adapter that publishes it to the input layer. Keeping
 * them apart means the serial console stays a pure console and the input layer
 * stays hardware-agnostic.
 *
 * Why this matters: the PS/2 keyboard cannot be fed by a headless test, so
 * without a serial input path there is no way to drive a natural-language turn
 * from a script. With it, `printf 'question\n' | qemu ... -serial stdio` is a
 * complete end-to-end test harness.
 *
 * Registered at DRV_LEVEL_DEVICE rather than inside serial_init(), so the input
 * subsystem is only ever wired up once the whole console is known good.
 */

#include "input.h"
#include "serial.h"
#include "driver.h"
#include "device.h"
#include "kernel.h"

static int serial_src_getc(input_source_t *self) {
    (void)self;
    int c = serial_getc_nb();
    return (c < 0) ? INPUT_NONE : c;
}

/* Echo is on for the same reason it is on for the keyboard: the terminal on the
 * far end may be in raw mode (and a pipe certainly is), so the kernel echoing is
 * what makes typed input visible — and, in a test, what proves it arrived. */
static input_source_t serial_src = {
    .name    = "serial",
    .getc_nb = serial_src_getc,
    .priv    = NULL,
    .flags   = INPUT_ECHO,
};

static int serial_input_init(void) {
    /* Deliberately no FIFO drain here. Boot spends seconds in DNS and the TLS
     * self-test, and a test script pipes its question in immediately — those
     * bytes are already sitting in the UART FIFO by the time we get here, and
     * throwing them away would break the very case this exists for. Anything
     * genuinely spurious just lands in a line the user can backspace over. */
    if (input_register_source(&serial_src) != 0) {
        kprintf("serial-in: failed to register input source\n");
        return -1;
    }
    kprintf("serial-in: COM1 registered as input source\n");
    return 0;
}

static const driver_t serial_input_driver = {
    .name  = "serial-in",
    .level = DRV_LEVEL_DEVICE,
    .cls   = DEV_CLASS_NONE,
    .init  = serial_input_init,
};
REGISTER_DRIVER(serial_input_driver);
