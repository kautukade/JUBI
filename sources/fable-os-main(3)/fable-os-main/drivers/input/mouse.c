/* mouse.c — PS/2 auxiliary device (mouse) driver, polled. See include/mouse.h
 * for the contract, the framing/resync argument, and why the hardware stream
 * starts muted.
 *
 * LAYOUT OF THIS FILE
 *   1. the port backend — the only code here that touches an I/O port
 *   2. the 8042 / aux command protocol: bounded waits, ACK, RESEND
 *   3. bring-up: aux port on, interrupts off, reset, defaults, wheel upgrade
 *   4. the decoder — packet framing, resynchronisation, and clamping (pure)
 *   5. the event queue and the consumer API
 *   6. the driver — REGISTER_DRIVER, the device node, and the optional watch
 *      self-test that proves the whole path against a real controller
 *
 * Sections 2-5 compile on the host as-is and are driven by
 * tests/host/test_mouse.c through a synthetic 8042; section 1's default backend
 * and section 6 are kernel-only.
 */

#include "mouse.h"

#ifndef FABLEOS_HOSTTEST
#  include "io.h"
#  include "kernel.h"
#  include "driver.h"
#  include "device.h"
#endif

/* ====================================================================== */
/* 1. the port backend                                                    */
/* ====================================================================== */

#define PS2_DATA        0x60
#define PS2_STATUS      0x64      /* read  */
#define PS2_CMD         0x64      /* write */

/* Status register bits. AUX is the one that makes a shared output buffer
 * workable at all: it says the byte OBF is offering came from the auxiliary
 * device rather than the keyboard. */
#define PS2_STAT_OBF    0x01
#define PS2_STAT_IBF    0x02
#define PS2_STAT_AUX    0x20

/* Controller commands (port 0x64). */
#define PS2_CMD_READ_CFG   0x20
#define PS2_CMD_WRITE_CFG  0x60
#define PS2_CMD_AUX_OFF    0xA7
#define PS2_CMD_AUX_ON     0xA8
#define PS2_CMD_WRITE_AUX  0xD4   /* the next 0x60 write goes to the mouse */

/* Configuration byte bits we care about. */
#define PS2_CFG_AUX_IRQ    0x02   /* IRQ12 on aux data — must stay CLEAR   */
#define PS2_CFG_AUX_CLOCK_OFF 0x20 /* 1 = aux clock disabled                */

/* Auxiliary device commands. */
#define AUX_SET_SCALING_1_1 0xE6
#define AUX_SET_RESOLUTION  0xE8
#define AUX_GET_DEVICE_ID   0xF2
#define AUX_SET_SAMPLE_RATE 0xF3
#define AUX_ENABLE_REPORT   0xF4
#define AUX_DISABLE_REPORT  0xF5
#define AUX_SET_DEFAULTS    0xF6
#define AUX_RESET           0xFF

/* Auxiliary device replies. */
#define AUX_ACK             0xFA
#define AUX_RESEND          0xFE
#define AUX_BAT_OK          0xAA   /* posted by a successful reset */

/* Device ids: 0 = plain 3-byte mouse, 3 = IntelliMouse (wheel), 4 = IMEX. */
#define AUX_ID_PLAIN        0x00
#define AUX_ID_WHEEL        0x03

#ifndef FABLEOS_HOSTTEST
static uint8_t port_status(void *c)             { (void)c; return inb(PS2_STATUS); }
static uint8_t port_read_data(void *c)          { (void)c; return inb(PS2_DATA); }
static void    port_write_data(void *c, uint8_t v) { (void)c; outb(PS2_DATA, v); }
static void    port_write_cmd(void *c, uint8_t v)  { (void)c; outb(PS2_CMD, v); }

static const mouse_backend_t default_backend = {
    port_status, port_read_data, port_write_data, port_write_cmd, (void *)0
};
#else
/* Host builds have no ports. Status reads back 0xFF, which has IBF set
 * permanently, so the first "wait until the controller can take a byte" times
 * out and bring-up reports "no device" — the same answer a machine with no
 * i8042 gives. Tests install a synthetic controller with mouse_set_backend(). */
static uint8_t absent_status(void *c)              { (void)c; return 0xFF; }
static uint8_t absent_read_data(void *c)           { (void)c; return 0xFF; }
static void    absent_write_data(void *c, uint8_t v) { (void)c; (void)v; }
static void    absent_write_cmd(void *c, uint8_t v)  { (void)c; (void)v; }

static const mouse_backend_t default_backend = {
    absent_status, absent_read_data, absent_write_data, absent_write_cmd, (void *)0
};
#endif

static mouse_backend_t g_be;
static int             g_be_set;

static const mouse_backend_t *be(void) {
    return g_be_set ? &g_be : &default_backend;
}

void mouse_set_backend(const mouse_backend_t *b) {
    if (!b || !b->status || !b->read_data || !b->write_data || !b->write_cmd) {
        g_be_set = 0;
        return;
    }
    g_be    = *b;
    g_be_set = 1;
}

/* ====================================================================== */
/* 2. the 8042 / aux command protocol                                     */
/* ====================================================================== */

/* Monotonic milliseconds where the build has such a thing. Host test builds do
 * not, and return a constant — which disables the wall-clock bound below and
 * leaves the iteration cap in charge, exactly as intended there. The decoder's
 * timeout does not use this: its timestamp is a parameter, so tests control it. */
static uint64_t now_ms(void) {
#ifndef FABLEOS_HOSTTEST
    return millis();
#else
    return 0;
#endif
}

/* Bounded waits. The wall clock is what actually bounds a kernel wait: a mouse
 * takes up to ~500 ms to finish a reset, and counting port reads is not a way
 * to measure time (an inb is ~1 us on an ISA bus and ~5 ns under emulation —
 * the same trap include/rtc.h documents for the UIP wait). The iteration cap
 * exists only so a build with no clock, i.e. the host tests, cannot spin
 * forever; there it is the only bound, so it is small enough to keep a suite
 * that deliberately probes the "no device" path fast. */
#define PS2_WAIT_MS 600u
#ifdef FABLEOS_HOSTTEST
#  define PS2_WAIT_SPINS 4096u
#else
#  define PS2_WAIT_SPINS 4000000u
#endif

static int wait_flag(uint8_t mask, int want) {
    uint64_t start = now_ms();
    for (uint32_t i = 0; i < PS2_WAIT_SPINS; i++) {
        int have = (be()->status(be()->ctx) & mask) != 0;
        if (have == (want != 0)) return 1;
        if ((i & 0xFF) == 0xFF && now_ms() - start > PS2_WAIT_MS) return 0;
    }
    return 0;
}

/* The driver's own decoder state; declared here because the command layer
 * charges discarded keyboard bytes to its stats. */
static mouse_state_t g_state;
static int           g_present;
static int           g_wheel;
static int           g_streaming;
/* Set when enabling the stream failed. Without it, a GUI polling every frame
 * would pay three failed command round-trips per frame forever. Cleared by a
 * bring-up, which is the only thing that could plausibly fix it. */
static int           g_stream_failed;

static int ctrl_cmd(uint8_t cmd) {
    if (!wait_flag(PS2_STAT_IBF, 0)) return -1;
    be()->write_cmd(be()->ctx, cmd);
    return 0;
}

static int ctrl_write(uint8_t v) {
    if (!wait_flag(PS2_STAT_IBF, 0)) return -1;
    be()->write_data(be()->ctx, v);
    return 0;
}

/* A byte from the controller itself (e.g. the configuration byte), which is
 * posted with the AUX bit CLEAR because it did not come from the mouse. */
static int ctrl_read(uint8_t *out) {
    if (!wait_flag(PS2_STAT_OBF, 1)) return -1;
    *out = be()->read_data(be()->ctx);
    return 0;
}

/* A byte from the AUX device. A byte with the AUX bit clear is the keyboard's
 * and cannot be an answer to what we asked; it is dropped (and counted) rather
 * than mistaken for an ACK, which would put every later reply one step out of
 * step. Dropping it costs at most a keystroke typed during boot, which
 * kbd_init() would have drained anyway. Bounded so a controller that only ever
 * offers keyboard bytes cannot hold bring-up here. */
#define AUX_FOREIGN_BUDGET 8

static int aux_read(uint8_t *out) {
    for (int i = 0; i < AUX_FOREIGN_BUDGET; i++) {
        if (!wait_flag(PS2_STAT_OBF, 1)) return -1;
        uint8_t st = be()->status(be()->ctx);
        uint8_t v  = be()->read_data(be()->ctx);
        if (st & PS2_STAT_AUX) { *out = v; return 0; }
        g_state.st.foreign++;
    }
    return -1;
}

/* Send one byte to the aux device and require its acknowledgement.
 *
 * Two things can arrive instead of the ACK, and they are not the same thing:
 *
 *   RESEND (0xFE) is "say that again", not "no" — the device did not receive
 *   the byte cleanly. Repeat the command.
 *
 *   A packet byte. A command issued while the device is streaming (mouse_stop()
 *   is exactly that) races the packet already on the wire, and those bytes are
 *   indistinguishable from a reply: the value 0xFA is also a perfectly ordinary
 *   -6 delta. So up to one packet's worth of unrecognised bytes is skipped while
 *   looking for the reply, which is what makes stopping a moving mouse succeed
 *   instead of reporting a failure the device did not actually have. The bytes
 *   are lost either way — a command boundary is a discontinuity in the stream,
 *   which is why every caller resets the decoder afterwards.
 *
 * 0xFC ("error") is a refusal and is reported as one. */
#define AUX_SEND_TRIES  3
#define AUX_STRAY_BUDGET 4          /* one 4-byte packet in flight */

static int aux_send(uint8_t cmd) {
    for (int t = 0; t < AUX_SEND_TRIES; t++) {
        if (ctrl_cmd(PS2_CMD_WRITE_AUX) != 0) return -1;
        if (ctrl_write(cmd) != 0) return -1;

        int resend = 0;
        for (int s = 0; s <= AUX_STRAY_BUDGET; s++) {
            uint8_t r;
            if (aux_read(&r) != 0) return -1;
            if (r == AUX_ACK)    return 0;
            if (r == AUX_RESEND) { resend = 1; break; }
            if (r == 0xFC)       return -1;      /* the device said no */
            /* anything else: a byte of a packet that was already in flight */
        }
        if (!resend) return -1;
    }
    return -1;
}

/* A two-byte command (0xF3 rate, 0xE8 resolution): both halves are ACKed. */
static int aux_send2(uint8_t cmd, uint8_t arg) {
    if (aux_send(cmd) != 0) return -1;
    return aux_send(arg);
}

/* Discard whatever is sitting in the output buffer. Bounded: a controller stuck
 * with OBF permanently set must not turn this into a hang. */
#define AUX_FLUSH_BUDGET 32

static void aux_flush(void) {
    for (int i = 0; i < AUX_FLUSH_BUDGET; i++) {
        uint8_t st = be()->status(be()->ctx);
        if (!(st & PS2_STAT_OBF)) return;
        if (!(st & PS2_STAT_AUX)) g_state.st.foreign++;
        (void)be()->read_data(be()->ctx);
    }
}

/* ====================================================================== */
/* 3. bring-up                                                            */
/* ====================================================================== */

/* Forward declarations from section 4/5 (pure logic used by bring-up). */
static void reset_stream(mouse_state_t *m);

/* Ask the device what it is. Returns the id byte, or -1 if it did not answer. */
static int aux_device_id(void) {
    if (aux_send(AUX_GET_DEVICE_ID) != 0) return -1;
    uint8_t id;
    if (aux_read(&id) != 0) return -1;
    return (int)id;
}

/* The IntelliMouse upgrade: set the sample rate to 200, then 100, then 80 and
 * ask for the device id. A device that understands the sequence answers 3 and
 * from then on sends 4-byte packets with a wheel byte; a plain mouse answers 0
 * and keeps sending 3, having merely had its sample rate set three times, which
 * is harmless. That is why this is attempted rather than negotiated: the
 * fallback is the same device in a valid state, so failure costs nothing.
 *
 * Returns 1 if 4-byte packets are now in force, 0 if not. Never fails the
 * bring-up: a mouse that NAKs one of these is still a mouse. */
static int try_wheel_upgrade(void) {
    if (aux_send2(AUX_SET_SAMPLE_RATE, 200) != 0) return 0;
    if (aux_send2(AUX_SET_SAMPLE_RATE, 100) != 0) return 0;
    if (aux_send2(AUX_SET_SAMPLE_RATE, 80)  != 0) return 0;

    int id = aux_device_id();
    return id == AUX_ID_WHEEL;
}

int mouse_bringup(void) {
    g_present       = 0;
    g_wheel         = 0;
    g_streaming     = 0;
    g_stream_failed = 0;

    /* Keep the clamp box and cursor across a re-initialisation; a resume must
     * not teleport the pointer. First call: install the defaults. */
    if (g_state.w <= 0 || g_state.h <= 0)
        mouse_state_init(&g_state, MOUSE_DEFAULT_W, MOUSE_DEFAULT_H);
    else
        reset_stream(&g_state);

    aux_flush();

    /* Enable the auxiliary port. This is also what clears the aux clock
     * disable bit, so it must come before the configuration byte is read back
     * or we would write the stale value straight back. */
    if (ctrl_cmd(PS2_CMD_AUX_ON) != 0) return -1;

    /* IRQ12 off, aux clock on. Best-effort: if the controller will not hand
     * over its configuration byte we carry on rather than abandon a device that
     * 0xA8 has already enabled — every PIC line is masked anyway, so a stale
     * interrupt-enable bit cannot reach the CPU today. It is still cleared when
     * possible, because "an interrupt nobody handles is armed" is exactly the
     * state that turns switching interrupts on later into a triple fault. */
    uint8_t cfg;
    if (ctrl_cmd(PS2_CMD_READ_CFG) == 0 && ctrl_read(&cfg) == 0) {
        uint8_t want = (uint8_t)((cfg & ~(PS2_CFG_AUX_IRQ | PS2_CFG_AUX_CLOCK_OFF)));
        if (want != cfg) {
            if (ctrl_cmd(PS2_CMD_WRITE_CFG) == 0) (void)ctrl_write(want);
        }
    }

    /* Reset. Puts a device of unknown provenance into its defined state:
     * defaults restored, reporting off, and — importantly for the next step —
     * back to 3-byte packets, so the wheel negotiation starts from scratch. */
    if (aux_send(AUX_RESET) != 0) return -1;

    /* A reset posts 0xAA (self-test passed) then the device id. Both are read
     * so the next reply we wait for is not one of them, but neither is required:
     * a device that answered the reset is a device, and the id is asked for
     * explicitly during the upgrade anyway. The id byte is only consumed when
     * 0xAA actually arrived — otherwise the byte just read may have BEEN it. */
    uint8_t bat = 0;
    if (aux_read(&bat) == 0 && bat == AUX_BAT_OK) {
        uint8_t id0;
        (void)aux_read(&id0);
    }

    if (aux_send(AUX_SET_DEFAULTS) != 0) return -1;

    g_wheel = try_wheel_upgrade();
    mouse_state_set_wheel(&g_state, g_wheel);

    /* 100 samples/second: the wheel handshake left the rate at 80, and a plain
     * mouse that ignored the handshake is wherever 0xF6 left it. Resolution and
     * scaling are deliberately left at the defaults 0xF6 just set — this driver
     * reports counts, and choosing a sensitivity is the GUI's business. */
    (void)aux_send2(AUX_SET_SAMPLE_RATE, 100);

    /* Prove data reporting works HERE, where a failure can be reported at boot,
     * then mute it again: nothing consumes mouse bytes yet, and an unconsumed
     * stream is fed to the keyboard driver instead (see mouse.h, "WHY THE
     * STREAM STARTS MUTED"). mouse_start() turns it on for real. */
    if (aux_send(AUX_ENABLE_REPORT) != 0) return -1;
    if (aux_send(AUX_DISABLE_REPORT) != 0) return -1;
    aux_flush();
    reset_stream(&g_state);

    g_present = 1;
    return 0;
}

/* ====================================================================== */
/* 4. the decoder: framing, resynchronisation, clamping (pure)            */
/* ====================================================================== */

/* Bit 3 of the first byte is defined to be 1; it is the only structural check a
 * PS/2 packet offers. */
#define PKT_ALWAYS_ONE 0x08
#define PKT_X_SIGN     0x10
#define PKT_Y_SIGN     0x20
#define PKT_X_OVF      0x40
#define PKT_Y_OVF      0x80

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int16_t sat16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int8_t sat8(int32_t v) {
    if (v >  127) return  127;
    if (v < -128) return -128;
    return (int8_t)v;
}

/* Drop a partial packet and everything queued, keeping geometry and position.
 * Used whenever the byte stream is known to have a discontinuity. */
static void reset_stream(mouse_state_t *m) {
    m->have         = 0;
    m->last_byte_ms = 0;
    m->qhead        = 0;
    m->qcount       = 0;
}

void mouse_state_init(mouse_state_t *m, int w, int h) {
    if (!m) return;
    mouse_stats_t zero = { 0, 0, 0, 0, 0, 0, 0, 0 };
    m->st      = zero;
    m->plen    = 3;
    m->buttons = 0;
    m->w = m->h = 1;
    m->x = m->y = 0;
    reset_stream(m);
    mouse_state_set_screen(m, w, h);
    m->x = m->w / 2;
    m->y = m->h / 2;
}

void mouse_state_set_screen(mouse_state_t *m, int w, int h) {
    if (!m) return;
    /* A box with no pixels in it has no valid cursor position, so the smallest
     * legal box is 1x1 rather than an error the caller has to handle. */
    m->w = (w < 1) ? 1 : (int32_t)w;
    m->h = (h < 1) ? 1 : (int32_t)h;
    m->x = clamp32(m->x, 0, m->w - 1);
    m->y = clamp32(m->y, 0, m->h - 1);
}

void mouse_state_warp(mouse_state_t *m, int x, int y) {
    if (!m) return;
    m->x = clamp32((int32_t)x, 0, m->w - 1);
    m->y = clamp32((int32_t)y, 0, m->h - 1);
}

void mouse_state_set_wheel(mouse_state_t *m, int has_wheel) {
    if (!m) return;
    uint8_t plen = has_wheel ? 4 : 3;
    if (plen != m->plen) {
        m->plen = plen;
        /* Bytes already collected were framed for the old length. */
        m->have = 0;
    }
}

int mouse_decode(const uint8_t *pkt, int len, mouse_packet_t *out) {
    if (!pkt || !out || (len != 3 && len != 4)) return -1;

    uint8_t b0 = pkt[0];
    if (!(b0 & PKT_ALWAYS_ONE)) return -1;

    out->buttons = (uint8_t)(b0 & MOUSE_BTN_ALL);
    out->flags   = 0;
    if (b0 & PKT_X_OVF) out->flags |= MOUSE_F_XOVF;
    if (b0 & PKT_Y_OVF) out->flags |= MOUSE_F_YOVF;

    /* 9-bit two's complement: magnitude byte plus a sign bit in byte 0. Reading
     * the magnitude as int8_t is the classic bug — a real +200 count motion
     * arrives as 0xC8 with the sign bit clear and would come out as -56. */
    int16_t dx = (int16_t)pkt[1];
    int16_t dy = (int16_t)pkt[2];
    if (b0 & PKT_X_SIGN) dx = (int16_t)(dx - 256);
    if (b0 & PKT_Y_SIGN) dy = (int16_t)(dy - 256);

    out->dx = (out->flags & MOUSE_F_XOVF) ? 0 : dx;
    out->dy = (out->flags & MOUSE_F_YOVF) ? 0 : dy;

    out->dz = 0;
    if (len == 4) {
        out->flags |= MOUSE_F_WHEEL;
        /* IntelliMouse (id 3) sends a full signed byte here. IMEX (id 4) uses
         * only the low nibble plus two extra buttons; see mouse.h. */
        out->dz = (int8_t)pkt[3];
    }
    return 0;
}

/* ====================================================================== */
/* 5. the event queue and the consumer API                                */
/* ====================================================================== */

/* Append an event. A full queue means the consumer is slower than the hardware;
 * the newest event is then MERGED into the one already at the tail rather than
 * either being dropped. Dropping the oldest would lose a button press, and
 * dropping the newest would lose the current position — merging loses neither:
 * motion adds up, edges are the union, and the position is the latest. */
static void queue_event(mouse_state_t *m, const mouse_event_t *e) {
    if (m->qcount < MOUSE_QUEUE_LEN) {
        m->q[(m->qhead + m->qcount) % MOUSE_QUEUE_LEN] = *e;
        m->qcount++;
        return;
    }
    mouse_event_t *tail = &m->q[(m->qhead + MOUSE_QUEUE_LEN - 1) % MOUSE_QUEUE_LEN];
    tail->x         = e->x;
    tail->y         = e->y;
    tail->dx        = sat16((int32_t)tail->dx + e->dx);
    tail->dy        = sat16((int32_t)tail->dy + e->dy);
    tail->dz        = sat8((int32_t)tail->dz + e->dz);
    tail->buttons   = e->buttons;
    tail->pressed  |= e->pressed;
    tail->released |= e->released;
    tail->flags    |= e->flags;
    m->st.merges++;
}

int mouse_state_next_event(mouse_state_t *m, mouse_event_t *out) {
    if (!m || m->qcount == 0) return 0;
    if (out) *out = m->q[m->qhead];
    m->qhead = (uint8_t)((m->qhead + 1) % MOUSE_QUEUE_LEN);
    m->qcount--;
    return 1;
}

/* Apply a decoded packet: move the cursor inside the box, work out the button
 * edges, and queue what a consumer sees. */
static void apply_packet(mouse_state_t *m, const mouse_packet_t *p) {
    m->st.packets++;
    if (p->flags & (MOUSE_F_XOVF | MOUSE_F_YOVF)) m->st.overflows++;

    /* Screen coordinates grow downward; the hardware's Y grows upward. */
    int32_t want_x = m->x + p->dx;
    int32_t want_y = m->y - p->dy;
    int32_t nx = clamp32(want_x, 0, m->w - 1);
    int32_t ny = clamp32(want_y, 0, m->h - 1);

    mouse_event_t e;
    e.flags = p->flags;
    if (nx != want_x || ny != want_y) {
        e.flags |= MOUSE_F_CLAMP;
        m->st.clamped++;
    }
    e.dx = sat16(nx - m->x);
    e.dy = sat16(ny - m->y);
    e.dz = p->dz;
    m->x = nx;
    m->y = ny;
    e.x  = nx;
    e.y  = ny;

    e.pressed  = (uint8_t)(p->buttons & (uint8_t)~m->buttons);
    e.released = (uint8_t)((uint8_t)~p->buttons & m->buttons & MOUSE_BTN_ALL);
    e.buttons  = p->buttons;
    m->buttons = p->buttons;

    queue_event(m, &e);
}

int mouse_feed_byte(mouse_state_t *m, uint8_t b, uint64_t now) {
    if (!m) return 0;
    if (m->plen != 3 && m->plen != 4) m->plen = 3;   /* never trust a bad plen */

    m->st.bytes++;

    /* Framing. A first byte with the always-one bit clear cannot start a
     * packet, so the stream is out of phase: throw the byte away and try the
     * next one. This is the fast half of resynchronisation; the other half is
     * mouse_check_stale(). */
    if (m->have == 0 && !(b & PKT_ALWAYS_ONE)) {
        m->st.resyncs++;
        return 0;
    }

    m->pkt[m->have++]  = b;
    m->last_byte_ms    = now;
    if (m->have < m->plen) return 0;

    int len = m->have;
    m->have = 0;

    mouse_packet_t p;
    if (mouse_decode(m->pkt, len, &p) != 0) {
        /* Unreachable while the only rejection is the byte-0 check above, which
         * has already passed. Counted rather than asserted so that a future
         * decoder with more validation degrades into a resync instead of
         * silently accepting a packet it does not believe. */
        m->st.resyncs++;
        return 0;
    }
    apply_packet(m, &p);
    return 1;
}

int mouse_check_stale(mouse_state_t *m, uint64_t now) {
    if (!m || m->have == 0) return 0;
    if (now - m->last_byte_ms < MOUSE_PARTIAL_TIMEOUT_MS) return 0;
    m->have = 0;
    m->st.stale++;
    return 1;
}

/* ---- the driver's own instance ---- */

/* Bytes taken from the controller in one poll. A bound, not a target: a
 * controller that always has a byte ready must not own the CPU. 200 samples/s
 * of 4-byte packets is 800 bytes/s, so this drains many milliseconds of motion
 * per call. */
#define MOUSE_DRAIN_BUDGET 64

#ifdef FABLEOS_MOUSE_SELFTEST
/* Deliberate byte loss, for proving resynchronisation against real hardware.
 * Compiled out of every normal build. See section 6. */
static uint32_t g_drop_every;
static uint32_t g_byte_seq;
static uint32_t g_dropped_bytes;
#endif

int mouse_present(void)   { return g_present; }
int mouse_has_wheel(void) { return g_wheel; }

void mouse_set_screen(int w, int h) { mouse_state_set_screen(&g_state, w, h); }
void mouse_warp(int x, int y)       { mouse_state_warp(&g_state, x, y); }

void mouse_position(int *x, int *y) {
    if (x) *x = (int)g_state.x;
    if (y) *y = (int)g_state.y;
}

uint8_t mouse_buttons(void) { return g_state.buttons; }

const mouse_stats_t *mouse_stats(void) { return &g_state.st; }
const mouse_state_t *mouse_state(void) { return &g_state; }

int mouse_start(void) {
    if (!g_present) return -1;
    if (aux_send(AUX_ENABLE_REPORT) != 0) return -1;
    reset_stream(&g_state);      /* the stream restarts on a packet boundary */
    g_streaming = 1;
    return 0;
}

int mouse_stop(void) {
    if (!g_present) return -1;
    g_streaming = 0;
    int rc = aux_send(AUX_DISABLE_REPORT);
    if (rc != 0) {
        /* The reply was lost in a burst of motion longer than one packet. The
         * device has stopped talking by now (it did process the command), so a
         * second attempt on a quiet wire is the honest way to find out whether
         * it is really off rather than assuming either answer. */
        aux_flush();
        rc = aux_send(AUX_DISABLE_REPORT);
    }
    aux_flush();                 /* bytes already in flight are now orphans */
    reset_stream(&g_state);
    return rc;
}

int mouse_poll(void) {
    if (!g_present || g_stream_failed) return 0;
    if (!g_streaming && mouse_start() != 0) { g_stream_failed = 1; return 0; }

    int packets = 0;
    for (int i = 0; i < MOUSE_DRAIN_BUDGET; i++) {
        uint8_t st = be()->status(be()->ctx);
        if (!(st & PS2_STAT_OBF)) {
            /* Nothing pending: this is the moment a half-assembled packet can
             * be recognised as the debris of a lost byte. */
            mouse_check_stale(&g_state, now_ms());
            break;
        }
        /* The head of the buffer is a keyboard byte. It is not ours, and there
         * is no way to see past it — one output buffer, first come first
         * served. Leave it for drivers/input/kbd.c and stop; the mouse bytes
         * behind it are still there next time. */
        if (!(st & PS2_STAT_AUX)) break;

        uint8_t v = be()->read_data(be()->ctx);
#ifdef FABLEOS_MOUSE_SELFTEST
        if (g_drop_every && (++g_byte_seq % g_drop_every) == 0) {
            g_dropped_bytes++;
            continue;                /* pretend this byte never arrived */
        }
#endif
        packets += mouse_feed_byte(&g_state, v, now_ms());
    }
    return packets;
}

int mouse_next_event(mouse_event_t *out) {
    if (g_state.qcount == 0) (void)mouse_poll();
    return mouse_state_next_event(&g_state, out);
}

int mouse_accept_byte(uint8_t b) {
    /* No mouse: the byte cannot be decoded into anything, and it has already
     * been removed from the controller. Dropping it is the only option, and it
     * is the right one — a phantom aux byte on a board with no aux device is
     * noise, not input. */
    if (!g_present) return 0;
    return mouse_feed_byte(&g_state, b, now_ms());
}

/* ====================================================================== */
/* 6. the driver                                                          */
/* ====================================================================== */
#ifndef FABLEOS_HOSTTEST

static const driver_t mouse_driver;

/* ---- optional watch self-test ------------------------------------------
 * The one thing a host test cannot prove is that these bytes come from a real
 * controller. This does, and it is compiled out of every normal build:
 *
 *   make EXTRA_CFLAGS='-DFABLEOS_MOUSE_SELFTEST'
 *   make EXTRA_CFLAGS='-DFABLEOS_MOUSE_SELFTEST -DFABLEOS_MOUSE_DROP_EVERY=7'
 *   make EXTRA_CFLAGS='-DFABLEOS_MOUSE_SELFTEST -DFABLEOS_MOUSE_WATCH_MS=20000'
 *
 * It streams the mouse for a bounded window and prints every event it decodes,
 * so driving QEMU's monitor (mouse_move / mouse_button) shows the positions and
 * button edges the driver derived from real hardware bytes. DROP_EVERY throws
 * away one byte in N as they come off the port, which is the only honest way to
 * demonstrate that resynchronisation recovers a genuinely corrupted stream —
 * the byte is lost after the hardware sent it and before the decoder sees it,
 * exactly like a missed poll would lose it.
 *
 * Mirrors drivers/acpi/power_selftest.c and arch/x86_64/fault_inject.c: a
 * diagnostic that has to run against hardware lives with the driver, behind a
 * flag, rather than in a test directory that cannot reach it. */
#ifdef FABLEOS_MOUSE_SELFTEST

#ifndef FABLEOS_MOUSE_WATCH_MS
#  define FABLEOS_MOUSE_WATCH_MS 12000u
#endif
#ifndef FABLEOS_MOUSE_DROP_EVERY
#  define FABLEOS_MOUSE_DROP_EVERY 0u
#endif
/* Serial is 115200 baud; a fast mouse can outrun it. Print the first N events
 * in full and count the rest, so the window is never spent on I/O. */
#define MOUSE_WATCH_MAX_PRINT 120

static void btn_str(uint8_t b, char out[4]) {
    out[0] = (b & MOUSE_BTN_LEFT)   ? 'L' : '-';
    out[1] = (b & MOUSE_BTN_MIDDLE) ? 'M' : '-';
    out[2] = (b & MOUSE_BTN_RIGHT)  ? 'R' : '-';
    out[3] = '\0';
}

static void mouse_watch(void) {
    g_drop_every = FABLEOS_MOUSE_DROP_EVERY;

    kprintf("mouse: watch %u ms, drop 1 byte in %u (0 = none) - move the "
            "pointer and click\n",
            (unsigned)FABLEOS_MOUSE_WATCH_MS, (unsigned)g_drop_every);

    if (mouse_start() != 0) {
        kprintf("mouse: watch: could not enable data reporting\n");
        return;
    }

    uint64_t deadline = millis() + FABLEOS_MOUSE_WATCH_MS;
    uint32_t shown = 0, events = 0;

    while (millis() < deadline) {
        mouse_event_t e;
        while (mouse_state_next_event(&g_state, &e)) {
            events++;
            if (shown < MOUSE_WATCH_MAX_PRINT) {
                shown++;
                char now[4], dn[4], up[4];
                btn_str(e.buttons, now);
                btn_str(e.pressed, dn);
                btn_str(e.released, up);
                kprintf("mouse: ev %u x=%d y=%d dx=%d dy=%d dz=%d btn=%s "
                        "down=%s up=%s flags=%x\n",
                        (unsigned)events, (int)e.x, (int)e.y, (int)e.dx,
                        (int)e.dy, (int)e.dz, now, dn, up, (unsigned)e.flags);
            }
        }
        (void)mouse_poll();
    }

    const mouse_stats_t *s = mouse_stats();
    kprintf("mouse: watch done: %u events (%u shown), %u bytes, %u packets, "
            "%u resync, %u stale, %u overflow, %u clamped, %u merged, "
            "%u foreign, %u bytes dropped on purpose\n",
            (unsigned)events, (unsigned)shown, (unsigned)s->bytes,
            (unsigned)s->packets, (unsigned)s->resyncs, (unsigned)s->stale,
            (unsigned)s->overflows, (unsigned)s->clamped, (unsigned)s->merges,
            (unsigned)s->foreign, (unsigned)g_dropped_bytes);

    int x, y;
    mouse_position(&x, &y);
    kprintf("mouse: watch final position %d,%d buttons=%x\n",
            x, y, (unsigned)mouse_buttons());

    g_drop_every = 0;
    (void)mouse_stop();
}
#endif /* FABLEOS_MOUSE_SELFTEST */

static int mouse_init(void) {
    int rc = mouse_bringup();

    device_t *d = device_create("mouse0", DEV_CLASS_INPUT);
    device_add_resource(d, RES_IOPORT, PS2_DATA, 1);
    device_add_resource(d, RES_IRQ, 12, 1);   /* recorded, deliberately unused */
    device_register(d);
    device_bind_driver(d, (driver_t *)&mouse_driver);

    if (rc != 0) {
        /* No mouse is a normal configuration, not a broken driver: a serial
         * console has none, and neither does a machine whose firmware never
         * enabled the aux port. Say so once, mark the node FAILED so the device
         * tree tells the truth, and return success so boot does not report a
         * driver failure for hardware that simply is not there. */
        device_set_state(d, DEV_STATE_FAILED);
        kprintf("mouse: no PS/2 auxiliary device answered - this machine has "
                "no pointer\n");
        return 0;
    }

    device_set_state(d, DEV_STATE_ACTIVE);
    kprintf("mouse: PS/2 mouse ready (%d-byte packets%s), %dx%d box, muted "
            "until a consumer polls\n",
            (int)g_state.plen, g_wheel ? ", wheel" : "",
            (int)g_state.w, (int)g_state.h);

#ifdef FABLEOS_MOUSE_SELFTEST
    mouse_watch();
#endif
    return 0;
}

/* Power management. Suspend silences the device so nothing arrives while the
 * machine is down; resume re-runs the whole bring-up rather than just switching
 * reporting back on, because a controller that has been through a power
 * transition may have lost the aux enable, the configuration byte and the wheel
 * mode — and re-negotiating is what keeps plen and the hardware in agreement.
 * The cursor position deliberately survives. */
static int mouse_suspend(device_t *d) {
    (void)d;
    if (!g_present) return 0;
    (void)mouse_stop();
    return 0;
}

static int mouse_resume(device_t *d) {
    (void)d;
    if (mouse_bringup() != 0) {
        device_set_state(d, DEV_STATE_FAILED);
        kprintf("mouse: resume: the auxiliary device did not come back\n");
        return -1;
    }
    return 0;
}

static const driver_t mouse_driver = {
    .name    = "mouse",
    .level   = DRV_LEVEL_DEVICE,
    .cls     = DEV_CLASS_INPUT,
    .init    = mouse_init,
    .suspend = mouse_suspend,
    .resume  = mouse_resume,
};
REGISTER_DRIVER(mouse_driver);

#endif /* !FABLEOS_HOSTTEST */
