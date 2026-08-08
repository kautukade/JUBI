/* kbd.h — PS/2 keyboard (i8042, polled), scancode -> ASCII.
 *
 * PURPOSE
 *   Turn scancodes from the legacy keyboard controller into ASCII characters.
 *   That is the driver's whole job: line assembly (echo, backspace, overflow)
 *   belongs to drivers/input/input.c, shared with the serial and scripted
 *   sources, so the same editing behaviour applies however a byte arrived.
 *
 * RESPONSIBILITIES
 *   - Probe for an i8042 at bring-up and refuse to register if there is none.
 *   - Decode scancode set 1, tracking Shift and the 0xE0 extended prefix.
 *   - Register itself as an input_source_t and publish a device node.
 *
 * PUBLIC API
 *   kbd_getchar_nb — the one entry point; everything else goes through
 *   input_poll(), which is what the kernel actually calls.
 *
 * DEPENDENCIES
 *   io.h (inb), input.h (input_source_t), device.h/driver.h (publication).
 *   No interrupts: all 16 PIC lines are masked, so the controller is polled.
 *
 * ABSENT HARDWARE
 *   Every access is bounded and an all-ones status latches the controller as
 *   absent. A driver that polls a missing port forever hangs a kernel that has
 *   no scheduler to switch away to and no watchdog to notice.
 *
 * FUTURE EXTENSION POINTS
 *   Ctrl/Alt modifiers, other keymaps, and the LED/typematic command path all
 *   fit behind this same function. IRQ1 delivery becomes possible if the PIC is
 *   ever unmasked; the device node already records the line.
 */
#ifndef KBD_H
#define KBD_H

/* Next key as ASCII, or -1 if none is waiting. Never blocks, and reads at most
 * a bounded number of scancodes per call. Enter is '\n', Backspace is '\b'.
 * Keys with no ASCII mapping are skipped. */
int kbd_getchar_nb(void);

#endif /* KBD_H */
