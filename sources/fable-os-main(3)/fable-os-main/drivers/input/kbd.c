/* kbd.c — PS/2 keyboard driver (i8042, polled, scancode set 1).
 *
 * We don't use interrupts: the terminal is a simple read-a-line-then-act loop,
 * so busy-polling the controller's status/data ports is enough and keeps the
 * kernel free of an IDT. Make codes (key down) are < 0x80; break codes (key up)
 * have bit 7 set. We track Shift for the upper-case / symbol layer and ignore
 * everything else (Ctrl, Alt, function keys, the keypad).
 *
 * This driver's only job is scancode -> ASCII. Line assembly (echo, backspace,
 * line endings, overflow) used to live here; it now lives in drivers/input/
 * input.c behind input_source_t, shared with the serial and scripted sources.
 * The keyboard just registers itself as one more source. */

#include "kbd.h"
#include "input.h"
#include "mouse.h"
#include "driver.h"
#include "device.h"
#include "kernel.h"
#include "io.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_OBF    0x01            /* output buffer full -> a byte is ready */

/* Status bit 5: the byte in the output buffer came from the AUX device (the
 * mouse), not the keyboard. There is ONE one-byte output buffer for both, and
 * this bit is the only thing that tells them apart.
 *
 * Testing it is not optional. A mouse packet's first byte is always
 * 0x08|buttons|signs, i.e. 0x08..0x3F, which is squarely inside the scancode
 * table below (0x08 = '7', 0x18 = 'o'), and a movement delta of 0x1C decodes as
 * Enter — so a keyboard driver that swallows aux bytes types garbage at the
 * natural-language prompt and then SUBMITS it to the model. The mouse driver
 * already yields on a non-AUX byte (drivers/input/mouse.c mouse_poll), so
 * without this the theft is one-directional and the mouse never works either. */
#define PS2_AUX    0x20

/* An unclaimed ISA port reads back all-ones, so a status byte of 0xFF means
 * "there is no i8042 here" — not "a byte is waiting". This distinction is
 * load-bearing: with status stuck at 0xFF, OBF looks permanently set and the
 * data port yields 0xFF forever, which is a break code for make 0x7F and so is
 * skipped. An unbounded `while (status & OBF) read()` therefore never
 * terminates on a board without a PS/2 controller — and on this kernel (no
 * interrupts, no scheduler, no watchdog) that is a permanent hang inside
 * drivers_init(), before the prompt exists and before any later driver runs. */
#define PS2_ABSENT 0xFF

/* Most bytes consumed in one call. The i8042 has a one-byte output buffer, so a
 * handful covers any real burst; the bound exists so that no single source can
 * monopolise input_poll()'s round robin from *inside* a driver, which is what
 * would otherwise defeat INPUT_DRAIN_BUDGET one layer up and starve the serial
 * line — the only other way into this machine. */
#define PS2_MAX_BYTES 32

#define SC_EXTENDED 0xE0           /* prefix byte for the grey keys */
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_KP_ENTER 0x1C           /* E0-prefixed: keypad Enter */
#define SC_KP_SLASH 0x35           /* E0-prefixed: keypad '/'   */

/* US QWERTY, scancode set 1, indexed by make code (0x00..0x39). 0 == no ASCII. */
static const char map_lower[0x40] = {
    0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',   /* 00-07 */
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',  /* 08-0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',    /* 10-17 */
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',    /* 18-1F (1D = Ctrl) */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',    /* 20-27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',    /* 28-2F (2A = LShift) */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',    /* 30-37 (36 = RShift) */
    0,    ' ',  0,    0,    0,    0,    0,    0,       /* 38-3F (38 = Alt) */
};

static const char map_upper[0x40] = {
    0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',
    '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
    0,    ' ',  0,    0,    0,    0,    0,    0,
};

static int shift_down;
static int extended;     /* the byte just read was the 0xE0 prefix */
static int kbd_absent;   /* latched: no controller, stop touching the ports */

/* Drain the controller until it yields a printable/edit character, or run out
 * of pending bytes, or hit PS2_MAX_BYTES. Never blocks — this is the
 * input_source_t contract. */
int kbd_getchar_nb(void) {
    if (kbd_absent) return -1;

    for (int i = 0; i < PS2_MAX_BYTES; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (st == PS2_ABSENT) { kbd_absent = 1; return -1; }
        if (!(st & PS2_OBF)) break;

        uint8_t sc = inb(PS2_DATA);

        /* Not ours. Hand it to the mouse rather than dropping it: the mouse
         * decoder is a 3/4-byte state machine, so a discarded byte costs it
         * alignment and a resync, while forwarding keeps the pointer working
         * even though the keyboard won the race for the buffer. */
        if (st & PS2_AUX) { (void)mouse_accept_byte(sc); continue; }

        if (sc == SC_EXTENDED) { extended = 1; continue; }

        int was_ext = extended;
        extended = 0;

        if (was_ext) {
            /* The i8042 brackets the grey navigation keys with "fake shift"
             * codes: E0 AA (a fake LShift *release*) before Insert/Delete/an
             * arrow while Shift is held, and E0 2A after. Decoding those as a
             * genuine shift edge silently drops the Shift the user IS holding —
             * so the next letters come out lower case — or invents one they are
             * not. Tracking the prefix is the only way to tell them apart.
             *
             * Of the extended keys, only keypad Enter and keypad '/' have an
             * ASCII meaning worth having, and they are deliberately aliased
             * onto their main-block twins. Everything else (arrows, Home,
             * Delete, right Ctrl/Alt, the GUI keys) is dropped rather than
             * accidentally decoded as whatever shares its low byte. */
            if (sc != SC_KP_ENTER && sc != SC_KP_SLASH) continue;
        } else if (sc & 0x80) {                /* break (key release) */
            uint8_t make = sc & 0x7F;
            if (make == SC_LSHIFT || make == SC_RSHIFT) shift_down = 0;
            continue;
        } else if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
            shift_down = 1;
            continue;
        }

        if (sc >= 0x40) continue;              /* outside our table */

        char c = shift_down ? map_upper[sc] : map_lower[sc];
        if (c) return (int)(unsigned char)c;
    }
    return -1;
}

/* ---- input_source_t adapter ---- */

static int kbd_src_getc(input_source_t *self) {
    (void)self;
    int c = kbd_getchar_nb();
    return (c < 0) ? INPUT_NONE : c;
}

static input_source_t kbd_src = {
    .name    = "kbd",
    .getc_nb = kbd_src_getc,
    .priv    = NULL,
    .flags   = INPUT_ECHO,
};

static void kbd_drain(void) {
    for (int i = 0; i < PS2_MAX_BYTES; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (st == PS2_ABSENT) { kbd_absent = 1; return; }
        if (!(st & PS2_OBF)) return;
        (void)inb(PS2_DATA);
    }
}

static const driver_t kbd_driver;

static int kbd_init(void) {
    /* Probe before doing anything else. Without this the driver announces "PS/2
     * keyboard ready" on every machine, including the modern boards and QEMU
     * configurations that have no i8042 at all. */
    uint8_t st = inb(PS2_STATUS);
    if (st == PS2_ABSENT) {
        kbd_absent = 1;
        kprintf("kbd: no i8042 controller (status 0x%x); no PS/2 keyboard\n", st);
        return -1;
    }

    kbd_drain();                       /* clear any stale byte */
    input_register_source(&kbd_src);
    kprintf("kbd: PS/2 keyboard ready\n");

    device_t *d = device_create("kbd0", DEV_CLASS_INPUT);
    device_add_resource(d, RES_IOPORT, PS2_DATA, 1);
    device_add_resource(d, RES_IRQ, 1, 1);
    device_register(d);
    device_bind_driver(d, (driver_t *)&kbd_driver);
    device_set_state(d, DEV_STATE_ACTIVE);
    return 0;
}

/* Power management: nothing to persist; on resume just clear stale input so a
 * key held across the transition doesn't leak in. Demonstrates the lifecycle. */
static int kbd_suspend(device_t *d) { (void)d; shift_down = 0; extended = 0; return 0; }
static int kbd_resume(device_t *d)  { (void)d; kbd_drain(); return 0; }

static const driver_t kbd_driver = {
    .name = "kbd",
    .level = DRV_LEVEL_DEVICE,
    .cls = DEV_CLASS_INPUT,
    .init = kbd_init,
    .suspend = kbd_suspend,
    .resume = kbd_resume,
};
REGISTER_DRIVER(kbd_driver);
