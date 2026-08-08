/* serial.h — COM1 (16550 UART): the kernel console, and a way in.
 *
 * PURPOSE
 *   Carry the kernel's own output somewhere a human or a script can read it, and
 *   carry typed bytes back. Once the display is a graphics framebuffer the VGA
 *   text buffer is no longer visible, so kprintf lands here; with `qemu ...
 *   -serial stdio` a test can pipe a natural-language question straight into the
 *   kernel, which is impossible with the PS/2 keyboard.
 *
 * RESPONSIBILITIES
 *   - Bring up the UART (115200 8N1) and publish a device node.
 *   - Transmit one byte at a time, translating '\n' to CRLF.
 *   - Hand back received bytes, non-blocking.
 *
 * PUBLIC API
 *   serial_putc / serial_write — transmit.
 *   serial_getc_nb            — receive, or -1.
 *   serial_emit               — the weak-symbol override that routes kprintf
 *                               here; declared so the two signatures are checked
 *                               against each other instead of only agreeing by
 *                               convention (lib/base.c defines the weak no-op).
 *
 * DEPENDENCIES
 *   io.h (inb/outb), device.h/driver.h (publication). Nothing else — the console
 *   must work before the heap, the clock, or the IDT.
 *
 * POLLED, AND BOUNDED
 *   Receive is polled: this kernel keeps all 16 PIC lines masked, so no IRQ 4
 *   handler exists. Transmit waits for the holding register with a bound, not
 *   forever; an unbounded wait in the kernel's only output path is a hang that
 *   cannot report itself. The receive FIFO is left disabled on purpose (see
 *   serial_init) so the far end hands us one byte at a time and nothing is lost
 *   while the kernel is busy inside DNS or a TLS handshake. Line editing on top
 *   of these bytes lives in input.c; see include/input.h.
 *
 * FUTURE EXTENSION POINTS
 *   A second port (COM2) is the same code with a different base; a loopback
 *   presence probe would let bring-up report an absent UART rather than write
 *   into the void.
 */
#ifndef SERIAL_H
#define SERIAL_H

void serial_putc(char c);
void serial_write(const char *s);

/* Next received byte (0..255), or -1 if none is waiting. Never blocks. */
int serial_getc_nb(void);

/* Overrides the weak definition in lib/base.c so kprintf reaches the line. */
void serial_emit(char c);

#endif /* SERIAL_H */
