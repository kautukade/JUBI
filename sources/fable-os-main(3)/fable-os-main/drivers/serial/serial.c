/* serial.c — COM1 16550 UART: the kernel debug console, and an input line.
 *
 * Once we switch the display into a graphics framebuffer the VGA text buffer is
 * no longer visible, so kprintf output is routed here (base.c calls the weak
 * serial_emit, which this driver overrides). QEMU forwards COM1 to a file/stdio
 * with -serial, giving us a verifiable boot log.
 *
 * The line also runs the other way. Receive is polled (no IDT exists, so no IRQ
 * 4 handler): serial_getc_nb() reads the Line Status Register's Data Ready bit
 * and pulls a byte if one is there. drivers/input/serial_input.c wraps that as
 * an input_source_t, which is how a headless test pipes a question in. */

#include "serial.h"
#include "driver.h"
#include "device.h"
#include "io.h"

#define COM1 0x3F8

/* Register offsets from COM1. */
#define UART_RBR 0   /* receive buffer (read)                */
#define UART_IER 1   /* interrupt enable                     */
#define UART_FCR 2   /* FIFO control (write)                 */
#define UART_LCR 3   /* line control                         */
#define UART_MCR 4   /* modem control                        */
#define UART_LSR 5   /* line status                          */

#define LSR_DR   0x01   /* data ready: a received byte is waiting */
#define LSR_THRE 0x20   /* transmit holding register empty        */

static const driver_t serial_driver;   /* forward decl for device binding */

/* Bounded wait for the transmit holding register to empty.
 *
 * Every kprintf in the kernel funnels through serial_emit -> serial_putc, so an
 * unbounded wait here hangs the machine INSIDE a print, with no diagnostic
 * possible because the diagnostic channel is the thing that is stuck. An absent
 * UART happens to be safe (an unclaimed port reads 0xFF, so THRE looks set), but
 * a UART that is present and wedged — a flow-controlled far end holding off, an
 * emulator or hardware fault — is not.
 *
 * TX_SPINS is enormous next to a character time at 115200 baud (87 us); in
 * normal operation the first read already succeeds and this costs one inb. After
 * TX_GIVE_UP_RUN *consecutive* expiries the port is declared dead and output is
 * dropped cheaply, so a wedged UART costs a bounded amount of time in total
 * rather than the full bound per character for the rest of the boot. A single
 * success clears the run, so a slow far end is waited for, not written off. */
#define TX_SPINS       1000000u
#define TX_GIVE_UP_RUN 8

static unsigned tx_timeouts;   /* consecutive THRE waits that expired */
static int      tx_dead;       /* latched once the run is long enough  */

static int tx_ready(void) {
    if (tx_dead) return 0;
    for (unsigned i = 0; i < TX_SPINS; i++) {
        if (inb(COM1 + UART_LSR) & LSR_THRE) { tx_timeouts = 0; return 1; }
    }
    if (++tx_timeouts >= TX_GIVE_UP_RUN) tx_dead = 1;
    return 0;
}

void serial_putc(char c) {
    if (c == '\n' && tx_ready()) outb(COM1, '\r');   /* CRLF for terminals */
    if (tx_ready()) outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

/* Bytes rescued from the receiver during bring-up, before FCR was touched — see
 * serial_init(). Handed out first so the received stream stays in order. */
#define RX_STASH_MAX 16
static uint8_t rx_stash[RX_STASH_MAX];
static uint8_t rx_stash_len, rx_stash_pos;

int serial_getc_nb(void) {
    if (rx_stash_pos < rx_stash_len) return rx_stash[rx_stash_pos++];
    if (!(inb(COM1 + UART_LSR) & LSR_DR)) return -1;
    return (int)inb(COM1 + UART_RBR);
}

/* Override base.c's weak no-op so kprintf reaches the serial line. */
void serial_emit(char c) {
    serial_putc(c);
}

static int serial_init(void) {
    outb(COM1 + UART_IER, 0x00);   /* no interrupts: we poll both directions */
    outb(COM1 + UART_LCR, 0x80);   /* DLAB on */
    outb(COM1 + 0,        0x01);   /* divisor 1 => 115200 baud (lo) */
    outb(COM1 + 1,        0x00);   /* (hi) */
    outb(COM1 + UART_LCR, 0x03);   /* 8 bits, no parity, 1 stop; DLAB off */
    /* Rescue anything already sitting in the receiver before we touch FCR: a
     * test pipes its question into `-serial stdio` before the guest has run an
     * instruction, and any FIFO-enable transition resets the receiver and throws
     * those bytes away. serial_getc_nb() replays the stash ahead of the port, so
     * the stream stays in order. Costs nothing when there is nothing to rescue. */
    while ((inb(COM1 + UART_LSR) & LSR_DR) && rx_stash_len < RX_STASH_MAX)
        rx_stash[rx_stash_len++] = inb(COM1 + UART_RBR);

    /* FIFOs off, deliberately.
     *
     * A 16-byte receive FIFO only helps a driver that can fall behind by up to
     * 16 characters and then catch up — i.e. an interrupt-driven one. We poll,
     * and boot spends seconds inside DNS and the TLS handshake without polling
     * at all, so 16 bytes of slack changes nothing. What it does change is
     * correctness: with the FIFO disabled the far end is only allowed to hand us
     * a byte once we have consumed the previous one, which makes a piped or
     * pasted line lossless no matter how long the kernel is busy. Measured
     * boot-to-prompt is identical either way, so this is free. */
    outb(COM1 + UART_FCR, 0x00);
    outb(COM1 + UART_MCR, 0x0B);   /* DTR/RTS/OUT2 */
    serial_write("serial: COM1 up @ 115200\n");

    device_t *d = device_create("serial0", DEV_CLASS_SERIAL);
    device_add_resource(d, RES_IOPORT, COM1, 8);
    device_add_resource(d, RES_IRQ, 4, 1);
    device_register(d);
    device_bind_driver(d, (driver_t *)&serial_driver);
    device_set_state(d, DEV_STATE_ACTIVE);
    return 0;
}

static const driver_t serial_driver = {
    .name = "serial",
    .level = DRV_LEVEL_EARLY,
    .cls = DEV_CLASS_SERIAL,
    .init = serial_init,
};
REGISTER_DRIVER(serial_driver);
