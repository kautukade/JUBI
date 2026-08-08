/* mouse.h — PS/2 auxiliary device (mouse): a pointer for the GUI phase.
 *
 * PURPOSE
 *   Turn the byte stream an i8042 auxiliary port produces into something a user
 *   interface can act on: an absolute cursor position clamped to the screen, the
 *   current button levels, and — the part that actually matters for a GUI —
 *   button *edges*, so a consumer can tell "the left button is down" from "the
 *   left button was just pressed" without keeping its own shadow copy of state.
 *
 * ARCHITECTURE
 *   Four layers, deliberately separated so that everything except the first is
 *   pure logic that runs (and is tested) on the host:
 *
 *     1. the port backend — four function pointers (mouse_backend_t) that are
 *        the only code in the driver touching 0x60/0x64. The kernel installs
 *        real port I/O; tests install a synthetic 8042 + mouse.
 *     2. bring-up — the 8042 command dance and the aux device's ACK protocol,
 *        including the IntelliMouse magic sequence and its fallback.
 *     3. the decoder — mouse_decode() on a raw packet, and mouse_feed_byte()
 *        for the byte-at-a-time stream, which is where packet framing and
 *        resynchronisation live.
 *     4. the consumer API — position, buttons, and an event queue.
 *
 *   Layers 2-4 hold no static state that layer 1 cannot supply, which is why a
 *   host test can bring up a synthetic mouse, negotiate the wheel, feed it a
 *   corrupted stream and assert on the recovery, all in one process.
 *
 * POLLED, NEVER INTERRUPT-DRIVEN
 *   This kernel runs with IF clear and all 16 PIC lines masked; IRQ12 never
 *   fires and there is no bottom half to fire it into. mouse_poll() drains the
 *   controller when a consumer asks, exactly like drivers/input/kbd.c does for
 *   the keyboard. Bring-up therefore explicitly *clears* the aux interrupt bit
 *   in the 8042 configuration byte rather than leaving it as the firmware left
 *   it: an enabled interrupt line whose handler does not exist is a fault
 *   waiting for the day interrupts are switched on.
 *
 * FRAMING AND RESYNCHRONISATION — the reason this file is not 80 lines
 *   A PS/2 mouse packet has no start delimiter and no checksum; it is 3 bytes
 *   (4 with a wheel) whose meaning depends entirely on their position in the
 *   stream. Lose one byte and every subsequent packet is read one byte out of
 *   phase: button bits are read out of a delta, deltas out of button bits, and
 *   the pointer goes berserk in a way that never self-corrects. Two independent
 *   mechanisms recover alignment, because neither is sufficient alone:
 *
 *     a. The always-one bit. Bit 3 of the first byte is defined to be 1. A byte
 *        offered as a packet start with that bit clear cannot be one, so it is
 *        discarded (mouse_stats_t::resyncs) instead of being believed. This
 *        catches misalignment quickly but not always: a delta byte can have bit
 *        3 set, so a bad phase can survive several packets.
 *     b. A partial-packet timeout. A real packet's bytes arrive within ~2 ms of
 *        each other (a PS/2 byte is 11 bits at 10-16.7 kHz), so a packet that
 *        has been half-assembled for MOUSE_PARTIAL_TIMEOUT_MS is not a packet
 *        in progress, it is the wreckage of a lost byte. It is abandoned, which
 *        forces the next byte to be judged as a packet start by (a).
 *
 *   The timeout is wall-clock and not a spin count on purpose: this kernel
 *   polls in a tight loop, so "a few thousand idle polls" is microseconds on
 *   emulation and milliseconds on a 486 — the same mistake include/rtc.h
 *   documents for the UIP wait. The caller supplies the timestamp (millis() in
 *   the kernel, a test-controlled counter on the host) so that timing behaviour
 *   is testable rather than merely asserted.
 *
 * WHY THE STREAM STARTS MUTED
 *   Bring-up enables data reporting, verifies the ACK, and then disables it
 *   again; the stream only starts for real when a consumer first calls
 *   mouse_start() (or mouse_poll(), which starts it implicitly). This is not
 *   timidity, it is the consequence of a shared output buffer: the keyboard and
 *   the mouse deliver bytes through the same 8042 port, distinguished only by
 *   bit 5 of the status register. Muting until someone is listening means a
 *   normal boot is byte-for-byte unaffected by the mouse existing — no bytes
 *   arrive that nobody asked for.
 *
 *   This used also to be the ONLY thing standing between a live pointer and a
 *   corrupted prompt, because drivers/input/kbd.c consumed whatever OBF offered
 *   without checking bit 5: with any window open, mouse motion was decoded as
 *   scancodes and a delta byte of 0x1C is Enter, so the machine submitted
 *   sentences nobody typed while the pointer itself got nothing. kbd.c now tests
 *   the bit and hands the byte to mouse_accept_byte(), so the two drivers can no
 *   longer steal from each other and muting is a bound rather than a defence.
 *
 *   Enabling and immediately disabling is what proves the command path works at
 *   boot, where a failure can be reported, instead of on the GUI's first frame.
 *
 * WHAT IS DELIBERATELY NOT HERE
 *   Acceleration curves, double-click timing, drag thresholds, and cursor
 *   drawing. Those are policy and pixels — they belong to the GUI, which has
 *   the frame clock and the screen. This driver reports what the hardware said,
 *   the position that follows from it, and nothing invented.
 *
 * PUBLIC API
 *   mouse_present / mouse_has_wheel        — what bring-up actually got.
 *   mouse_set_screen / mouse_warp          — the clamp box and the cursor.
 *   mouse_poll / mouse_next_event          — drain hardware, consume events.
 *   mouse_position / mouse_buttons         — level state, cheap to sample.
 *   mouse_start / mouse_stop               — turn the hardware stream on/off.
 *   mouse_stats                            — bytes, packets, resyncs, drops.
 *   mouse_decode / mouse_feed_byte / mouse_check_stale / mouse_state_init
 *                                          — the pure decoder, usable on any
 *                                            mouse_state_t (tests, and a future
 *                                            interrupt path).
 *   mouse_set_backend                      — swap port I/O for a fake device.
 *
 * DEPENDENCIES
 *   stdint/stddef only in the pure layers; kernel.h (millis, kprintf),
 *   driver.h/device.h and io.h in the kernel-only sections of mouse.c.
 *
 * FUTURE EXTENSION POINTS
 *   - IMEX (device id 4, five buttons + a 4-bit wheel field) is one more magic
 *     sequence and one more decode branch; mouse_state_t::plen and the flags
 *     byte already carry what it needs.
 *   - When an IDT services devices, an IRQ12 handler calls mouse_feed_byte()
 *     with millis() and nothing else changes: the decoder is already a
 *     byte-at-a-time state machine, and mouse_poll() becomes optional.
 *   - A second pointing device (USB HID, a touchpad) implements the same
 *     mouse_state_t feed and publishes another device node; the consumer API is
 *     already device-agnostic.
 *   - mouse_set_screen() is the seam for a real framebuffer resolution, and
 *     mouse_warp() is what a GUI uses to centre the pointer on mode set.
 */
#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stddef.h>

/* ---- button bits (as they appear in the packet's first byte) ---- */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04
#define MOUSE_BTN_ALL    0x07

/* ---- event/packet flags ---- */
#define MOUSE_F_XOVF   0x01   /* X delta overflowed: motion on X was lost   */
#define MOUSE_F_YOVF   0x02   /* Y delta overflowed: motion on Y was lost   */
#define MOUSE_F_WHEEL  0x04   /* this packet carried a wheel byte           */
#define MOUSE_F_CLAMP  0x08   /* the position hit an edge of the screen box */

/* A half-assembled packet older than this is treated as the debris of a lost
 * byte rather than a packet in progress. Two orders of magnitude above the
 * ~2 ms a genuine 3-byte packet takes on the wire, far below human perception. */
#define MOUSE_PARTIAL_TIMEOUT_MS 50u

/* Events buffered between consumer calls. A GUI polling once per frame at 60 Hz
 * sees at most ~3 packets per frame from a 200 Hz mouse, so this is generous;
 * when it does fill, the oldest events are merged rather than dropped (see
 * mouse_state_t::merges) so no button edge is ever lost. */
#define MOUSE_QUEUE_LEN 32

/* Default clamp box: the 720x400 pixel geometry of the VGA text console, so the
 * pointer is inside the screen before anything calls mouse_set_screen(). */
#define MOUSE_DEFAULT_W 720
#define MOUSE_DEFAULT_H 400

/* ---- one decoded packet, before it is applied to a position ---- */
typedef struct mouse_packet {
    int16_t dx;        /* 9-bit signed X delta, right-positive              */
    int16_t dy;        /* 9-bit signed Y delta, UP-positive (hardware sense)*/
    int8_t  dz;        /* wheel: negative = toward the user, 0 without one  */
    uint8_t buttons;   /* MOUSE_BTN_* levels at the time of the packet      */
    uint8_t flags;     /* MOUSE_F_XOVF | MOUSE_F_YOVF | MOUSE_F_WHEEL       */
} mouse_packet_t;

/* ---- what a consumer gets ---- */
typedef struct mouse_event {
    int32_t x, y;      /* absolute position after this event, inside the box */
    int16_t dx, dy;    /* motion APPLIED, screen sense (dy down-positive)    */
    int8_t  dz;        /* wheel notches                                     */
    uint8_t buttons;   /* levels after this event                           */
    uint8_t pressed;   /* buttons that went down at this event (edges)      */
    uint8_t released;  /* buttons that came up at this event (edges)        */
    uint8_t flags;     /* MOUSE_F_*                                        */
} mouse_event_t;

/* ---- health counters. Everything here is a symptom worth printing.
 * Cumulative for the life of the driver: they deliberately survive
 * mouse_bringup(), so a suspend/resume cannot hide a mouse that resyncs
 * constantly by zeroing the evidence. ---- */
typedef struct mouse_stats {
    uint32_t bytes;       /* bytes fed to the decoder                        */
    uint32_t packets;     /* complete, valid packets decoded                 */
    uint32_t resyncs;     /* bytes discarded because they cannot start a pkt  */
    uint32_t stale;       /* partial packets abandoned on the timeout         */
    uint32_t overflows;   /* packets with an overflow bit set                 */
    uint32_t clamped;     /* packets whose motion was clipped by the box      */
    uint32_t merges;      /* events merged because the queue was full         */
    uint32_t foreign;     /* non-aux bytes seen while draining (keyboard)     */
} mouse_stats_t;

/* ---- decoder + consumer state. One instance is the driver's; tests make
 * their own. Zero-initialised is invalid: call mouse_state_init(). ---- */
typedef struct mouse_state {
    int32_t  w, h;                 /* clamp box, in pixels                  */
    int32_t  x, y;                 /* absolute position, always inside it    */

    uint8_t  pkt[4];               /* bytes collected so far                */
    uint8_t  have;                 /* how many of them                      */
    uint8_t  plen;                 /* packet length: 3, or 4 with a wheel    */
    uint8_t  buttons;              /* last known button levels              */
    uint64_t last_byte_ms;         /* when pkt[have-1] arrived              */

    mouse_event_t q[MOUSE_QUEUE_LEN];
    uint8_t  qhead, qcount;

    mouse_stats_t st;
} mouse_state_t;

/* ---- the pure layers (host-testable, no hardware) ---- */

/* Bind a decoder to a clamp box and put the cursor in its centre. w/h below 1
 * are clamped to 1, so the box is never empty and the position never invalid.
 * plen is set to 3 (no wheel); mouse_state_set_wheel() opts into 4. */
void mouse_state_init(mouse_state_t *m, int w, int h);

/* Resize the clamp box, keeping the cursor inside it. Same clamping rules. */
void mouse_state_set_screen(mouse_state_t *m, int w, int h);

/* Move the cursor, clamped. Generates no event: this is the consumer moving the
 * pointer itself (a GUI centring it after a mode set), not the hardware. */
void mouse_state_warp(mouse_state_t *m, int x, int y);

/* Select 3- or 4-byte packets. Called by bring-up once the device id is known;
 * discards any partial packet, since its length just changed. */
void mouse_state_set_wheel(mouse_state_t *m, int has_wheel);

/* Decode a raw packet. len must be 3 or 4. Returns 0 on success, -1 if len is
 * wrong or pkt[0] fails the always-one check (bit 3).
 *
 * dx/dy are sign-extended from 9 bits: the magnitude byte is NOT a two's
 * complement int8 — its sign lives in bit 4 (X) / bit 5 (Y) of pkt[0], and a
 * delta of +200 arrives as magnitude 0xC8 with the sign bit CLEAR. Reading the
 * byte as int8_t would turn that into -56.
 *
 * When an overflow bit is set the corresponding delta is reported as 0 and the
 * flag is passed on. An overflowed axis means "at least 256 counts, in an
 * amount the hardware could not tell us"; the truncated value that arrives with
 * it is not a small motion, so applying it moves the pointer by a plausible
 * distance in a direction that may be wrong. Dropping the axis loses motion
 * visibly and recovers on the next packet, which is the better failure. */
int mouse_decode(const uint8_t *pkt, int len, mouse_packet_t *out);

/* Feed one byte from the aux device. now_ms is a monotonic millisecond clock,
 * used only for the partial-packet timeout. Returns 1 if the byte completed a
 * packet (an event has been queued and the position updated), 0 otherwise —
 * including when the byte was discarded to regain alignment. */
int mouse_feed_byte(mouse_state_t *m, uint8_t b, uint64_t now_ms);

/* Abandon a partial packet that has not advanced for MOUSE_PARTIAL_TIMEOUT_MS.
 * Call it whenever a drain finds no data. Returns 1 if it dropped something. */
int mouse_check_stale(mouse_state_t *m, uint64_t now_ms);

/* Pop the oldest queued event. Returns 1 and fills *out, or 0 if empty. */
int mouse_state_next_event(mouse_state_t *m, mouse_event_t *out);

/* ---- the consumer API (the driver's own state) ---- */

int  mouse_present(void);      /* bring-up found and configured a device */
int  mouse_has_wheel(void);    /* the IntelliMouse upgrade was accepted  */

void mouse_set_screen(int w, int h);
void mouse_warp(int x, int y);
void mouse_position(int *x, int *y);
uint8_t mouse_buttons(void);

/* Drain everything the controller has for us and queue the events. Returns the
 * number of packets decoded (0 is the normal answer). Starts the hardware
 * stream on first use — see "WHY THE STREAM STARTS MUTED" above. */
int mouse_poll(void);

/* Next event, polling the hardware when the queue is empty. Returns 1/0.
 * This is the whole interface a GUI event loop needs. */
int mouse_next_event(mouse_event_t *out);

/* Hand over a byte that some OTHER driver read off the shared i8042 output
 * buffer with the AUX status bit set.
 *
 * The keyboard and the mouse sit behind ONE one-byte output buffer and are told
 * apart only by status bit 5. Whoever polls first gets the byte, and there is no
 * way to put it back. drivers/input/kbd.c therefore checks that bit and calls
 * this instead of decoding an aux byte as a scancode — which is not a cosmetic
 * concern: mouse packet byte 1 is always 0x08|buttons|signs, and a delta of 0x1C
 * is Enter, so the machine would SUBMIT sentences nobody typed.
 *
 * Returns 1 if the byte completed a packet. Silently discards the byte when no
 * mouse was found at bring-up (there is nothing else sensible to do with it).
 * Feeding bytes this way keeps the decoder in phase; it does not start or stop
 * the hardware stream, and the 32-event queue merges on overflow, so an
 * unconsumed stream costs nothing but the newest position. */
int mouse_accept_byte(uint8_t b);

/* Enable/disable data reporting on the device (0xF4/0xF5). Returns 0 on
 * success, -1 if the device did not acknowledge. Stopping also discards any
 * partial packet and any queued event, because the stream is about to have a
 * hole in it.
 *
 * mouse_poll() re-arms a stopped stream, since polling is a request for data.
 * The way to keep a stopped mouse quiet is therefore to stop polling it, which
 * is what a suspended machine does by definition. */
int mouse_start(void);
int mouse_stop(void);

const mouse_stats_t *mouse_stats(void);

/* Read-only view of the driver's decoder state (tests and diagnostics). */
const mouse_state_t *mouse_state(void);

/* ---- injectable port backend ----
 * The four accesses the i8042 needs. In the kernel these are inb/outb on
 * 0x60/0x64; a host test installs a synthetic controller. Passing NULL restores
 * the default backend (real ports in the kernel, an absent device on the host,
 * where every status read returns 0xFF so bring-up fails cleanly). */
typedef struct mouse_backend {
    uint8_t (*status)(void *ctx);              /* read  0x64 */
    uint8_t (*read_data)(void *ctx);           /* read  0x60 */
    void    (*write_data)(void *ctx, uint8_t); /* write 0x60 */
    void    (*write_cmd)(void *ctx, uint8_t);  /* write 0x64 */
    void    *ctx;
} mouse_backend_t;

void mouse_set_backend(const mouse_backend_t *b);

/* Run the full bring-up sequence against whatever backend is installed:
 * enable the aux port, clear the aux interrupt bit, reset the device, set
 * defaults, attempt the wheel upgrade, verify data reporting, mute it again.
 * Returns 0 if a usable device was configured, -1 if none answered.
 *
 * Exposed (rather than being buried in the driver's init) so a host test can
 * drive it against a synthetic mouse and assert on both the commands sent and
 * the negotiated result. Safe to call again to re-initialise the hardware. */
int mouse_bringup(void);

#endif /* MOUSE_H */
