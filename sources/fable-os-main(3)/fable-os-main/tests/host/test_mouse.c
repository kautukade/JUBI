/* test_mouse.c — the PS/2 mouse driver, on the host.
 *
 * Runs the REAL drivers/input/mouse.c. Nothing here reimplements the driver;
 * what this file supplies is the other end of the wire.
 *
 * WHY THIS CAN BE TESTED AT ALL
 *   Every port access in the driver goes through four function pointers
 *   (mouse_backend_t, see include/mouse.h), so this file installs a SYNTHETIC
 *   i8042 with a synthetic mouse behind it: a shared output buffer that
 *   remembers, per byte, whether it came from the keyboard or the auxiliary
 *   device (that is what status bit 5 means); the controller commands the driver
 *   uses; and an aux device state machine with ACKs, RESEND, the IntelliMouse
 *   magic sequence and a device id — modelled on QEMU's hw/input/ps2.c, which
 *   is the hardware this kernel actually meets.
 *
 *   That fidelity is what makes the interesting failures reachable. On real
 *   hardware you cannot ask for a mouse that refuses the wheel upgrade, a
 *   controller whose input buffer never drains, or a stream with one byte
 *   surgically removed from the middle of a packet. Here each is a struct field.
 *
 * WHAT THIS SUITE IS BUILT TO PROVE
 *
 *   1. The 9-bit deltas are decoded as 9-bit deltas. The sign lives in the
 *      first byte, NOT in the magnitude byte, so a real +200-count flick
 *      arrives as 0xC8 with the sign bit clear. Reading it as int8_t yields -56
 *      — a pointer that jumps left when the hand moves right. Every one of the
 *      256 possible framing bytes is checked against an independent reference,
 *      crossed with eight magnitudes on each axis.
 *
 *   2. Framing survives byte loss. Two mechanisms, tested separately, because
 *      each catches what the other cannot: the always-one bit rejects a byte
 *      that cannot start a packet, and the partial-packet timeout throws away
 *      the wreckage when the deltas themselves have bit 3 set and the first
 *      mechanism is blind. A stream that has lost a byte must converge back to
 *      correct positions, not merely stop crashing.
 *
 *   3. The cursor cannot leave the box, at any of the four edges, and a clamp
 *      on one axis does not disturb the other.
 *
 *   4. The wheel upgrade is attempted and its failure is free. A device that
 *      answers id 0, or that NAKs the magic sequence outright, must still come
 *      up as a working 3-byte mouse rather than as no mouse.
 *
 *   5. Button EDGES are reported once each, and are never lost — not even when
 *      the event queue overflows, which is why a full queue merges rather than
 *      drops.
 *
 *   6. The driver never steals a keyboard byte from the shared output buffer
 *      while streaming, and IRQ12 is left disabled in the controller's
 *      configuration byte. This kernel has no device interrupts; an armed
 *      interrupt line with no handler is a fault waiting for the day they are
 *      switched on.
 *
 * NOT COVERED HERE, proved by booting instead (see the mouse: lines in
 * tests/qemu/cases/boot.case and the FABLEOS_MOUSE_SELFTEST watch loop): that
 * these bytes come from a real 8042, and that QEMU's mouse really does accept
 * the magic sequence.
 */

#include "harness.h"
#include "mouse.h"

#include <string.h>

/* ====================================================================== */
/* a synthetic i8042 + PS/2 mouse                                         */
/* ====================================================================== */

#define FQ_MAX   256
#define FLOG_MAX 64

typedef struct {
    /* the shared output buffer: value + whether it came from the aux device */
    uint8_t  q[FQ_MAX];
    uint8_t  qaux[FQ_MAX];
    int      qhead, qcount;

    /* controller state */
    uint8_t  cfg;              /* configuration byte (0x20 read / 0x60 write) */
    int      aux_port_on;      /* 0xA8 seen                                   */
    int      pending_ctrl;     /* controller command awaiting a data byte     */
    int      ibf_stuck;        /* input buffer never drains: a dead 8042      */
    int      cfg_unreadable;   /* 0x20 yields nothing                         */

    /* aux device state */
    int      present;          /* 0: writes to the mouse vanish               */
    int      reporting;        /* 0xF4 / 0xF5                                 */
    int      type;             /* 0 = plain, 3 = IntelliMouse, 4 = IMEX       */
    int      detect;           /* magic-sequence progress (as QEMU tracks it) */
    int      accept_magic;     /* 0: a mouse that never becomes id 3          */
    int      nak_everything;   /* answers 0xFC to every command               */
    int      nak_magic;        /* answers 0xFC to 0xF3 only                   */
    int      no_bat;           /* reset ACKs but posts no 0xAA                */
    int      resend_once;      /* answer the next command 0xFE, then behave   */
    int      arg_pending;      /* a two-byte command is waiting for its arg   */
    uint8_t  arg_cmd;
    int      sample_rate, resolution;
    int      resets;

    /* what the driver said to the mouse, in order */
    uint8_t  auxlog[FLOG_MAX];
    int      nauxlog;
    /* what the driver said to the controller, in order */
    uint8_t  ctrllog[FLOG_MAX];
    int      nctrllog;
} fake_t;

static fake_t F;

static void fq_push(uint8_t v, int aux) {
    if (F.qcount >= FQ_MAX) return;              /* a real buffer also drops */
    int i = (F.qhead + F.qcount) % FQ_MAX;
    F.q[i] = v; F.qaux[i] = (uint8_t)(aux != 0);
    F.qcount++;
}

static int fq_pop_head_value(void) {           /* what the keyboard driver does */
    if (F.qcount == 0) return -1;
    int v = F.q[F.qhead];
    F.qhead = (F.qhead + 1) % FQ_MAX;
    F.qcount--;
    return v;
}

static void aux_reply(uint8_t v) { fq_push(v, 1); }

/* The aux device. Mirrors QEMU's ps2.c closely enough that the magic sequence
 * behaves identically, including the detail that a reset returns the device to
 * type 0 (so the upgrade has to be renegotiated after every bring-up). */
static void aux_write(uint8_t v) {
    if (F.nauxlog < FLOG_MAX) F.auxlog[F.nauxlog++] = v;
    if (!F.present) return;                       /* nothing ever answers */
    if (F.resend_once) { F.resend_once = 0; aux_reply(0xFE); return; }
    if (F.nak_everything) { aux_reply(0xFC); return; }

    if (F.arg_pending) {
        uint8_t cmd = F.arg_cmd;
        F.arg_pending = 0;
        if (cmd == 0xF3) {
            F.sample_rate = v;
            switch (F.detect) {
                case 0: F.detect = (v == 200) ? 1 : 0; break;
                case 1: F.detect = (v == 100) ? 2 : (v == 200 ? 3 : 0); break;
                case 2: if (v == 80 && F.accept_magic) F.type = 3; F.detect = 0; break;
                case 3: if (v == 80 && F.accept_magic) F.type = 4; F.detect = 0; break;
                default: F.detect = 0; break;
            }
        } else if (cmd == 0xE8) {
            F.resolution = v;
        }
        aux_reply(0xFA);
        return;
    }

    switch (v) {
        case 0xFF:                                 /* reset */
            F.qcount = 0; F.qhead = 0;
            F.reporting = 0; F.type = 0; F.detect = 0;
            F.sample_rate = 100; F.resolution = 2;
            F.resets++;
            aux_reply(0xFA);
            if (!F.no_bat) { aux_reply(0xAA); aux_reply((uint8_t)F.type); }
            break;
        case 0xF6:                                 /* set defaults */
            F.reporting = 0; F.sample_rate = 100; F.resolution = 2;
            aux_reply(0xFA);
            break;
        case 0xF5: F.reporting = 0; aux_reply(0xFA); break;
        case 0xF4: F.reporting = 1; aux_reply(0xFA); break;
        case 0xF3:
            if (F.nak_magic) { aux_reply(0xFC); break; }
            F.arg_pending = 1; F.arg_cmd = v; aux_reply(0xFA);
            break;
        case 0xE8: F.arg_pending = 1; F.arg_cmd = v; aux_reply(0xFA); break;
        case 0xF2: aux_reply(0xFA); aux_reply((uint8_t)F.type); break;
        case 0xE6: aux_reply(0xFA); break;
        default:   aux_reply(0xFC); break;
    }
}

/* ---- the four backend entry points ---- */

static uint8_t fake_status(void *ctx) {
    (void)ctx;
    uint8_t s = 0;
    if (F.ibf_stuck) s |= 0x02;
    if (F.qcount > 0) {
        s |= 0x01;
        if (F.qaux[F.qhead]) s |= 0x20;
    }
    return s;
}

static uint8_t fake_read_data(void *ctx) {
    (void)ctx;
    int v = fq_pop_head_value();
    return (uint8_t)(v < 0 ? 0x00 : v);
}

static void fake_write_cmd(void *ctx, uint8_t v) {
    (void)ctx;
    if (F.nctrllog < FLOG_MAX) F.ctrllog[F.nctrllog++] = v;
    switch (v) {
        case 0xA8: F.aux_port_on = 1; F.cfg = (uint8_t)(F.cfg & ~0x20); break;
        case 0xA7: F.aux_port_on = 0; F.cfg = (uint8_t)(F.cfg |  0x20); break;
        case 0x20: if (!F.cfg_unreadable) fq_push(F.cfg, 0); break;
        case 0x60: F.pending_ctrl = 0x60; break;
        case 0xD4: F.pending_ctrl = 0xD4; break;
        default: break;
    }
}

static void fake_write_data(void *ctx, uint8_t v) {
    (void)ctx;
    int p = F.pending_ctrl;
    F.pending_ctrl = 0;
    if (p == 0x60)      F.cfg = v;
    else if (p == 0xD4) aux_write(v);
    /* otherwise it is a keyboard command; this driver never sends one. */
}

static const mouse_backend_t fake_backend = {
    fake_status, fake_read_data, fake_write_data, fake_write_cmd, (void *)0
};

/* A mouse that behaves. cfg starts with BOTH aux bits wrong (IRQ12 enabled,
 * aux clock disabled), which is how a controller left by firmware that thought
 * it was going to use interrupts looks — bring-up has to fix both. */
static void fake_reset(void) {
    memset(&F, 0, sizeof F);
    F.cfg          = 0x22;
    F.present      = 1;
    F.accept_magic = 1;
    F.sample_rate  = 100;
    F.resolution   = 2;
    mouse_set_backend(&fake_backend);
}

/* Queue a packet as the hardware would, honouring the negotiated length. */
static void fake_motion(int dx, int dy, int dz, uint8_t buttons) {
    if (!F.reporting) return;                 /* a muted device sends nothing */
    uint8_t b0 = (uint8_t)(0x08 | (buttons & 0x07));
    if (dx < 0) b0 |= 0x10;
    if (dy < 0) b0 |= 0x20;
    fq_push(b0, 1);
    fq_push((uint8_t)(dx & 0xFF), 1);
    fq_push((uint8_t)(dy & 0xFF), 1);
    if (F.type >= 3) fq_push((uint8_t)(dz & 0xFF), 1);
}

static int auxlog_has_seq(const uint8_t *seq, int n) {
    for (int i = 0; i + n <= F.nauxlog; i++)
        if (memcmp(&F.auxlog[i], seq, (size_t)n) == 0) return 1;
    return 0;
}

static int ctrllog_has(uint8_t v) {
    for (int i = 0; i < F.nctrllog; i++) if (F.ctrllog[i] == v) return 1;
    return 0;
}

/* ====================================================================== */
/* 1. decoding: every framing byte, every sign and overflow combination     */
/* ====================================================================== */

static void test_decode_every_framing_byte(void) {
    /* 0 and 255 are the extremes; 128 and 200 are the two that a naive
     * (int8_t) cast gets wrong when the sign bit disagrees with bit 7. */
    static const uint8_t mags[] = { 0, 1, 5, 8, 127, 128, 200, 255 };
    int accepted = 0, rejected = 0;

    for (int b = 0; b < 256; b++) {
        uint8_t b0 = (uint8_t)b;
        for (unsigned i = 0; i < sizeof mags; i++) {
            for (unsigned j = 0; j < sizeof mags; j++) {
                uint8_t pkt[3] = { b0, mags[i], mags[j] };
                mouse_packet_t p;
                int rc = mouse_decode(pkt, 3, &p);

                if (!(b0 & 0x08)) {          /* the always-one bit is clear */
                    CHECK_EQ(rc, -1);
                    rejected++;
                    continue;
                }
                accepted++;
                CHECK_EQ(rc, 0);
                if (rc != 0) continue;

                int xovf = (b0 & 0x40) != 0, yovf = (b0 & 0x80) != 0;
                int want_dx = (int)mags[i] - ((b0 & 0x10) ? 256 : 0);
                int want_dy = (int)mags[j] - ((b0 & 0x20) ? 256 : 0);

                CHECK_EQ(p.buttons, b0 & MOUSE_BTN_ALL);
                CHECK_EQ(p.dx, xovf ? 0 : want_dx);
                CHECK_EQ(p.dy, yovf ? 0 : want_dy);
                CHECK_EQ((p.flags & MOUSE_F_XOVF) != 0, xovf);
                CHECK_EQ((p.flags & MOUSE_F_YOVF) != 0, yovf);
                CHECK_EQ(p.flags & MOUSE_F_WHEEL, 0);
                CHECK_EQ(p.dz, 0);
            }
        }
    }
    CHECK_EQ(accepted, 128 * 8 * 8);
    CHECK_EQ(rejected, 128 * 8 * 8);
}

/* The single most valuable case in the file, spelled out on its own: a fast
 * flick right is +200 counts, which arrives as magnitude 0xC8 with the sign bit
 * CLEAR. (int8_t)0xC8 is -56, i.e. the pointer would fly left. */
static void test_decode_nine_bit_signs(void) {
    mouse_packet_t p;
    uint8_t right200[3] = { 0x08, 200, 0 };
    CHECK_EQ(mouse_decode(right200, 3, &p), 0);
    CHECK_EQ(p.dx, 200);

    uint8_t left200[3] = { 0x08 | 0x10, (uint8_t)(-200 & 0xFF), 0 };  /* 0x38 */
    CHECK_EQ(mouse_decode(left200, 3, &p), 0);
    CHECK_EQ(p.dx, -200);

    uint8_t minus1[3] = { 0x08 | 0x10 | 0x20, 0xFF, 0xFF };
    CHECK_EQ(mouse_decode(minus1, 3, &p), 0);
    CHECK_EQ(p.dx, -1);
    CHECK_EQ(p.dy, -1);

    /* The full-scale negative delta: magnitude 0 with the sign bit set. */
    uint8_t minus256[3] = { 0x08 | 0x10 | 0x20, 0x00, 0x00 };
    CHECK_EQ(mouse_decode(minus256, 3, &p), 0);
    CHECK_EQ(p.dx, -256);
    CHECK_EQ(p.dy, -256);

    /* Sign bit set with a positive-looking magnitude, and vice versa: both are
     * legal encodings and both must come out as 9-bit arithmetic. */
    uint8_t mixed[3] = { 0x08 | 0x20, 0x7F, 0x7F };
    CHECK_EQ(mouse_decode(mixed, 3, &p), 0);
    CHECK_EQ(p.dx, 127);
    CHECK_EQ(p.dy, 127 - 256);
}

static void test_decode_overflow_bits(void) {
    mouse_packet_t p;

    /* X overflow only: X motion is dropped, Y survives untouched. */
    uint8_t xo[3] = { 0x08 | 0x40, 0x40, 0x0A };
    CHECK_EQ(mouse_decode(xo, 3, &p), 0);
    CHECK_EQ(p.dx, 0);
    CHECK_EQ(p.dy, 10);
    CHECK_EQ(p.flags & MOUSE_F_XOVF, MOUSE_F_XOVF);
    CHECK_EQ(p.flags & MOUSE_F_YOVF, 0);

    /* Y overflow only. */
    uint8_t yo[3] = { 0x08 | 0x80, 0x0A, 0x40 };
    CHECK_EQ(mouse_decode(yo, 3, &p), 0);
    CHECK_EQ(p.dx, 10);
    CHECK_EQ(p.dy, 0);

    /* Both, plus every button down: the buttons still count. An overflow is a
     * lost magnitude, not a corrupt packet. */
    uint8_t both[3] = { 0x08 | 0x40 | 0x80 | 0x07, 0xFF, 0xFF };
    CHECK_EQ(mouse_decode(both, 3, &p), 0);
    CHECK_EQ(p.dx, 0);
    CHECK_EQ(p.dy, 0);
    CHECK_EQ(p.buttons, MOUSE_BTN_ALL);
    CHECK_EQ(p.flags & (MOUSE_F_XOVF | MOUSE_F_YOVF), MOUSE_F_XOVF | MOUSE_F_YOVF);
}

static void test_decode_wheel_byte(void) {
    mouse_packet_t p;
    uint8_t up[4]   = { 0x08, 0, 0, 0x01 };
    uint8_t down[4] = { 0x08, 0, 0, 0xFF };
    uint8_t far[4]  = { 0x08, 0, 0, 0x80 };

    CHECK_EQ(mouse_decode(up, 4, &p), 0);
    CHECK_EQ(p.dz, 1);
    CHECK_EQ(p.flags & MOUSE_F_WHEEL, MOUSE_F_WHEEL);

    CHECK_EQ(mouse_decode(down, 4, &p), 0);
    CHECK_EQ(p.dz, -1);

    CHECK_EQ(mouse_decode(far, 4, &p), 0);
    CHECK_EQ(p.dz, -128);

    /* A 4-byte packet read as 3 bytes must not report a wheel it did not see. */
    CHECK_EQ(mouse_decode(up, 3, &p), 0);
    CHECK_EQ(p.dz, 0);
    CHECK_EQ(p.flags & MOUSE_F_WHEEL, 0);
}

static void test_decode_rejects_nonsense(void) {
    mouse_packet_t p;
    uint8_t ok[4] = { 0x08, 0, 0, 0 };
    CHECK_EQ(mouse_decode(ok, 0, &p), -1);
    CHECK_EQ(mouse_decode(ok, 1, &p), -1);
    CHECK_EQ(mouse_decode(ok, 2, &p), -1);
    CHECK_EQ(mouse_decode(ok, 5, &p), -1);
    CHECK_EQ(mouse_decode(ok, -1, &p), -1);
    CHECK_EQ(mouse_decode(NULL, 3, &p), -1);
    CHECK_EQ(mouse_decode(ok, 3, NULL), -1);
}

/* ====================================================================== */
/* 2. the stream: framing and resynchronisation                            */
/* ====================================================================== */

/* Feed a whole packet at one timestamp. */
static int feed_pkt(mouse_state_t *m, uint8_t b0, uint8_t bx, uint8_t by,
                    uint64_t t) {
    int n = 0;
    n += mouse_feed_byte(m, b0, t);
    n += mouse_feed_byte(m, bx, t);
    n += mouse_feed_byte(m, by, t);
    return n;
}

static void test_stream_basic(void) {
    mouse_state_t m;
    mouse_state_init(&m, 200, 100);
    CHECK_EQ(m.x, 100);
    CHECK_EQ(m.y, 50);

    /* Right 10, up 10. Screen Y grows downward, so the cursor's y DECREASES. */
    CHECK_EQ(feed_pkt(&m, 0x08, 10, 10, 1), 1);
    CHECK_EQ(m.x, 110);
    CHECK_EQ(m.y, 40);

    mouse_event_t e;
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.x, 110);
    CHECK_EQ(e.y, 40);
    CHECK_EQ(e.dx, 10);
    CHECK_EQ(e.dy, -10);
    CHECK_EQ(e.buttons, 0);
    CHECK_EQ(e.pressed, 0);
    CHECK_EQ(e.released, 0);
    CHECK_EQ(mouse_state_next_event(&m, &e), 0);

    CHECK_EQ(m.st.packets, 1);
    CHECK_EQ(m.st.bytes, 3);
    CHECK_EQ(m.st.resyncs, 0);
    CHECK_EQ(m.st.stale, 0);
}

static void test_button_edges(void) {
    mouse_state_t m;
    mouse_state_init(&m, 100, 100);
    mouse_event_t e;

    /* Left down. */
    feed_pkt(&m, 0x08 | MOUSE_BTN_LEFT, 0, 0, 1);
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.pressed, MOUSE_BTN_LEFT);
    CHECK_EQ(e.released, 0);
    CHECK_EQ(e.buttons, MOUSE_BTN_LEFT);
    CHECK_EQ(m.buttons, MOUSE_BTN_LEFT);

    /* Held: motion while down is not another press. */
    feed_pkt(&m, 0x08 | MOUSE_BTN_LEFT, 3, 0, 2);
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.pressed, 0);
    CHECK_EQ(e.released, 0);
    CHECK_EQ(e.buttons, MOUSE_BTN_LEFT);

    /* Right down while left is held: only the new one is an edge. */
    feed_pkt(&m, 0x08 | MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT, 0, 0, 3);
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.pressed, MOUSE_BTN_RIGHT);
    CHECK_EQ(e.released, 0);

    /* Both up at once: two release edges in one event. */
    feed_pkt(&m, 0x08, 0, 0, 4);
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.pressed, 0);
    CHECK_EQ(e.released, MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT);
    CHECK_EQ(e.buttons, 0);

    /* Middle button, and a full click cycle reported exactly once each way. */
    feed_pkt(&m, 0x08 | MOUSE_BTN_MIDDLE, 0, 0, 5);
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.pressed, MOUSE_BTN_MIDDLE);
    feed_pkt(&m, 0x08, 0, 0, 6);
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.released, MOUSE_BTN_MIDDLE);
}

/* Resync mechanism (a): a byte that cannot start a packet is discarded. */
static void test_resync_by_always_one_bit(void) {
    mouse_state_t m;
    mouse_state_init(&m, 500, 500);
    mouse_state_warp(&m, 100, 100);

    feed_pkt(&m, 0x08, 5, 5, 1);
    CHECK_EQ(m.x, 105);
    CHECK_EQ(m.y, 95);
    while (mouse_state_next_event(&m, NULL)) { }

    /* Now lose the MIDDLE byte of a packet, the worst case: the packet is
     * completed with the first byte of the next one, so the damage propagates. */
    mouse_feed_byte(&m, 0x08, 2);        /* header      */
    /* (the 5 that should follow is lost) */
    mouse_feed_byte(&m, 5, 2);           /* dy, read as dx */
    mouse_feed_byte(&m, 0x08, 2);        /* next header, read as dy */
    CHECK_EQ(m.st.packets, 2);           /* a bogus packet was accepted */
    CHECK_EQ(m.x, 110);                  /* dx = 5 */
    CHECK_EQ(m.y, 95 - 8);               /* dy = 8, from the stolen header */
    while (mouse_state_next_event(&m, NULL)) { }

    /* The two remaining bytes of that packet are now offered as headers. Both
     * have the always-one bit clear, so both are refused and alignment returns
     * without any consumer seeing a bogus event. */
    mouse_feed_byte(&m, 5, 2);
    mouse_feed_byte(&m, 5, 2);
    CHECK_EQ(m.st.resyncs, 2);
    CHECK_EQ(m.st.packets, 2);
    CHECK_EQ(m.have, 0);

    /* Back in phase: the next real packet decodes exactly. */
    int32_t x0 = m.x, y0 = m.y;
    CHECK_EQ(feed_pkt(&m, 0x08 | MOUSE_BTN_LEFT, 7, 3, 3), 1);
    CHECK_EQ(m.x, x0 + 7);
    CHECK_EQ(m.y, y0 - 3);
    CHECK_EQ(m.buttons, MOUSE_BTN_LEFT);
    CHECK_EQ(m.st.resyncs, 2);
}

/* Resync mechanism (b): when every byte in the stream happens to have bit 3
 * set, (a) is blind and only the partial-packet timeout can recover. */
static void test_resync_by_partial_timeout(void) {
    mouse_state_t m;
    mouse_state_init(&m, 500, 500);
    mouse_state_warp(&m, 200, 200);

    /* Deltas of 8 and 9 both have bit 3 set, so a misframed group still looks
     * like a legal packet header. */
    CHECK_EQ(feed_pkt(&m, 0x0B, 8, 9, 100), 1);
    CHECK_EQ(m.x, 208);
    CHECK_EQ(m.y, 191);
    CHECK_EQ(m.buttons, MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT);
    while (mouse_state_next_event(&m, NULL)) { }

    /* A packet whose LAST byte is lost leaves two bytes stranded. Nothing about
     * them is invalid; they are simply the wreckage of a packet. */
    mouse_feed_byte(&m, 0x0B, 200);
    mouse_feed_byte(&m, 8, 200);
    CHECK_EQ(m.have, 2);

    /* Not yet stale: a real packet's bytes are ~1 ms apart, so the driver must
     * not abandon a packet that is merely in flight. */
    CHECK_EQ(mouse_check_stale(&m, 200 + MOUSE_PARTIAL_TIMEOUT_MS - 1), 0);
    CHECK_EQ(m.have, 2);
    CHECK_EQ(m.st.stale, 0);

    /* Now it is. */
    CHECK_EQ(mouse_check_stale(&m, 200 + MOUSE_PARTIAL_TIMEOUT_MS), 1);
    CHECK_EQ(m.have, 0);
    CHECK_EQ(m.st.stale, 1);
    CHECK_EQ(m.st.packets, 1);           /* no invented packet */

    /* And the stream is in phase again. */
    CHECK_EQ(feed_pkt(&m, 0x0B, 8, 9, 300), 1);
    CHECK_EQ(m.x, 216);
    CHECK_EQ(m.y, 182);

    /* A stale check with nothing pending is free and reports nothing. */
    CHECK_EQ(mouse_check_stale(&m, 100000), 0);
    CHECK_EQ(m.st.stale, 1);
}

/* The end-to-end property both mechanisms exist for: a stream with bytes
 * removed at intervals must converge back to the position a clean stream would
 * have produced, rather than drifting further away with every packet. */
static void test_resync_converges_after_byte_loss(void) {
    mouse_state_t clean, lossy;
    mouse_state_init(&clean, 4000, 4000);
    mouse_state_init(&lossy, 4000, 4000);
    mouse_state_warp(&clean, 2000, 2000);
    mouse_state_warp(&lossy, 2000, 2000);

    /* 120 identical small movements. Byte 7 of every 20 is dropped from the
     * lossy stream; the timeout is applied between bursts, as a poll loop
     * finding an empty buffer would. */
    int seq = 0;
    uint64_t t = 1000;
    for (int i = 0; i < 120; i++) {
        uint8_t pkt[3] = { 0x08, 4, 3 };
        for (int b = 0; b < 3; b++) {
            mouse_feed_byte(&clean, pkt[b], t);
            if ((seq++ % 20) != 7) mouse_feed_byte(&lossy, pkt[b], t);
        }
        t += 5;
        mouse_check_stale(&lossy, t);
        mouse_check_stale(&clean, t);
    }

    CHECK(lossy.st.resyncs + lossy.st.stale > 0);   /* it really did resync */
    CHECK_EQ(clean.st.packets, 120);

    /* It kept decoding: 18 bytes were removed, and the cost is the packets they
     * belonged to, not the rest of the session. */
    CHECK(lossy.st.packets >= 100);
    CHECK(lossy.st.packets <= 120);

    /* It is still tracking the same motion — right and up — and the position
     * error is bounded by what the lost packets were worth. The hard bound is
     * one full 9-bit delta per packet that never completed; the measured error
     * is far smaller (~4 counts per lost packet, i.e. exactly the motion those
     * packets carried), and this asserts it cannot exceed the bound. */
    int lost = (int)(clean.st.packets - lossy.st.packets);
    int32_t ex = clean.x - lossy.x, ey = clean.y - lossy.y;
    if (ex < 0) ex = -ex;
    if (ey < 0) ey = -ey;
    CHECK(lossy.x > 2000);
    CHECK(lossy.y < 2000);
    /* The error is signed either way: a misframed packet can read a header byte
     * as a delta and move slightly TOO far as easily as too little. What matters
     * is that the magnitude is bounded by the lost packets. */
    CHECK(ex <= 256 * (lost + 1));
    CHECK(ey <= 256 * (lost + 1));

    /* THE convergence property: once the corruption stops, tracking is exact
     * again. A decoder that had settled into a wrong phase would keep applying
     * deltas out of position for ever, and this is what catches it. */
    CHECK_EQ(lossy.have, 0);
    uint32_t resyncs_before = lossy.st.resyncs + lossy.st.stale;
    int32_t x0 = lossy.x, y0 = lossy.y;
    for (int i = 0; i < 20; i++) {
        feed_pkt(&lossy, 0x08, 4, 3, t);
        t += 5;
    }
    CHECK_EQ(lossy.x - x0, 20 * 4);
    CHECK_EQ(y0 - lossy.y, 20 * 3);
    CHECK_EQ(lossy.st.resyncs + lossy.st.stale, resyncs_before);
}

static void test_wheel_length_change_drops_partial(void) {
    mouse_state_t m;
    mouse_state_init(&m, 100, 100);
    CHECK_EQ(m.plen, 3);

    mouse_feed_byte(&m, 0x08, 1);
    mouse_feed_byte(&m, 5, 1);
    CHECK_EQ(m.have, 2);

    /* Renegotiating the packet length mid-stream invalidates what we hold: the
     * bytes were framed for a 3-byte packet. */
    mouse_state_set_wheel(&m, 1);
    CHECK_EQ(m.plen, 4);
    CHECK_EQ(m.have, 0);

    /* Now four bytes make a packet, and only four. */
    CHECK_EQ(mouse_feed_byte(&m, 0x08, 2), 0);
    CHECK_EQ(mouse_feed_byte(&m, 2, 2), 0);
    CHECK_EQ(mouse_feed_byte(&m, 2, 2), 0);
    CHECK_EQ(mouse_feed_byte(&m, 0xFF, 2), 1);
    mouse_event_t e;
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.dz, -1);
    CHECK_EQ(e.flags & MOUSE_F_WHEEL, MOUSE_F_WHEEL);

    /* Setting the same length again is a no-op and must NOT discard a partial. */
    mouse_feed_byte(&m, 0x08, 3);
    mouse_state_set_wheel(&m, 1);
    CHECK_EQ(m.have, 1);
}

/* ====================================================================== */
/* 3. clamping                                                             */
/* ====================================================================== */

static void test_clamp_all_four_edges(void) {
    mouse_state_t m;
    mouse_state_init(&m, 100, 50);
    mouse_event_t e;

    /* LEFT: a big negative X delta. Y must not move. */
    mouse_state_warp(&m, 10, 25);
    feed_pkt(&m, 0x08 | 0x10, (uint8_t)(-100 & 0xFF), 0, 1);
    CHECK_EQ(m.x, 0);
    CHECK_EQ(m.y, 25);
    CHECK_EQ(mouse_state_next_event(&m, &e), 1);
    CHECK_EQ(e.dx, -10);                       /* only the applied motion */
    CHECK_EQ(e.dy, 0);
    CHECK_EQ(e.flags & MOUSE_F_CLAMP, MOUSE_F_CLAMP);
    /* Pushing further left does not move it and does not wrap. */
    feed_pkt(&m, 0x08 | 0x10, (uint8_t)(-100 & 0xFF), 0, 2);
    CHECK_EQ(m.x, 0);

    /* RIGHT: the last legal column is w-1. */
    mouse_state_warp(&m, 90, 25);
    feed_pkt(&m, 0x08, 200, 0, 3);
    CHECK_EQ(m.x, 99);
    CHECK_EQ(m.y, 25);
    feed_pkt(&m, 0x08, 200, 0, 4);
    CHECK_EQ(m.x, 99);

    /* TOP: hardware Y is up-positive, so a POSITIVE dy walks toward y = 0. */
    mouse_state_warp(&m, 50, 10);
    feed_pkt(&m, 0x08, 0, 200, 5);
    CHECK_EQ(m.y, 0);
    CHECK_EQ(m.x, 50);
    feed_pkt(&m, 0x08, 0, 200, 6);
    CHECK_EQ(m.y, 0);

    /* BOTTOM: h-1. */
    mouse_state_warp(&m, 50, 40);
    feed_pkt(&m, 0x08 | 0x20, 0, (uint8_t)(-200 & 0xFF), 7);
    CHECK_EQ(m.y, 49);
    CHECK_EQ(m.x, 50);
    feed_pkt(&m, 0x08 | 0x20, 0, (uint8_t)(-200 & 0xFF), 8);
    CHECK_EQ(m.y, 49);

    /* A corner: both axes clamp in the same packet. */
    mouse_state_warp(&m, 5, 45);
    feed_pkt(&m, 0x08 | 0x10 | 0x20, (uint8_t)(-50 & 0xFF), (uint8_t)(-50 & 0xFF), 9);
    CHECK_EQ(m.x, 0);
    CHECK_EQ(m.y, 49);

    CHECK(m.st.clamped >= 5);
    /* Buttons keep working while pinned to an edge — a drag onto the edge of the
     * screen is the most ordinary thing a GUI does. */
    feed_pkt(&m, 0x08 | MOUSE_BTN_LEFT | 0x10, (uint8_t)(-9 & 0xFF), 0, 10);
    while (mouse_state_next_event(&m, &e)) { }
    CHECK_EQ(m.buttons, MOUSE_BTN_LEFT);
    CHECK_EQ(m.x, 0);
}

static void test_screen_resize_keeps_cursor_inside(void) {
    mouse_state_t m;
    mouse_state_init(&m, 1024, 768);
    mouse_state_warp(&m, 1000, 700);

    mouse_state_set_screen(&m, 320, 200);
    CHECK_EQ(m.x, 319);
    CHECK_EQ(m.y, 199);

    /* A degenerate box is clamped to 1x1 rather than rejected, so there is
     * always exactly one valid cursor position. */
    mouse_state_set_screen(&m, 0, -5);
    CHECK_EQ(m.w, 1);
    CHECK_EQ(m.h, 1);
    CHECK_EQ(m.x, 0);
    CHECK_EQ(m.y, 0);

    mouse_state_init(&m, -3, 0);
    CHECK_EQ(m.w, 1);
    CHECK_EQ(m.h, 1);
    CHECK_EQ(m.x, 0);
    CHECK_EQ(m.y, 0);

    /* Warping outside the box lands on the edge, never outside it. */
    mouse_state_set_screen(&m, 640, 480);
    mouse_state_warp(&m, 99999, -99999);
    CHECK_EQ(m.x, 639);
    CHECK_EQ(m.y, 0);

    /* NULL is a no-op everywhere in the pure layer. */
    mouse_state_init(NULL, 10, 10);
    mouse_state_set_screen(NULL, 10, 10);
    mouse_state_warp(NULL, 1, 1);
    mouse_state_set_wheel(NULL, 1);
    CHECK_EQ(mouse_feed_byte(NULL, 0x08, 0), 0);
    CHECK_EQ(mouse_check_stale(NULL, 0), 0);
    CHECK_EQ(mouse_state_next_event(NULL, NULL), 0);
}

/* ====================================================================== */
/* 4. the event queue under pressure                                       */
/* ====================================================================== */

static void test_queue_overflow_merges_and_keeps_edges(void) {
    mouse_state_t m;
    mouse_state_init(&m, 4000, 4000);
    mouse_state_warp(&m, 2000, 2000);

    /* Overrun the queue by 3x without consuming anything. */
    for (int i = 0; i < MOUSE_QUEUE_LEN * 3; i++)
        feed_pkt(&m, 0x08, 1, 0, (uint64_t)i);

    /* A button press arrives when the queue is long since full: it must not be
     * the event that gets thrown away. */
    feed_pkt(&m, 0x08 | MOUSE_BTN_RIGHT, 1, 0, 999);
    feed_pkt(&m, 0x08, 1, 0, 1000);

    CHECK(m.st.merges > 0);
    CHECK_EQ(m.qcount, MOUSE_QUEUE_LEN);
    CHECK_EQ(m.x, 2000 + MOUSE_QUEUE_LEN * 3 + 2);

    uint8_t saw_press = 0, saw_release = 0;
    int32_t last_x = -1;
    int events = 0;
    mouse_event_t e;
    while (mouse_state_next_event(&m, &e)) {
        events++;
        saw_press   |= e.pressed;
        saw_release |= e.released;
        last_x = e.x;
    }
    CHECK_EQ(events, MOUSE_QUEUE_LEN);
    CHECK_EQ(saw_press, MOUSE_BTN_RIGHT);      /* the press survived */
    CHECK_EQ(saw_release, MOUSE_BTN_RIGHT);    /* and so did the release */
    CHECK_EQ(last_x, m.x);                     /* the latest position survived */
}

/* ====================================================================== */
/* 5. bring-up against a synthetic controller                              */
/* ====================================================================== */

static void test_bringup_sequence(void) {
    fake_reset();
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_present(), 1);

    /* The aux port was enabled... */
    CHECK(ctrllog_has(0xA8));
    /* ...and the configuration byte was rewritten with IRQ12 OFF and the aux
     * clock ON. This kernel masks every PIC line, so an armed IRQ12 could not
     * fire today — but it is the kind of latent state that turns switching
     * interrupts on into a triple fault. */
    CHECK_EQ(F.cfg & 0x02, 0);
    CHECK_EQ(F.cfg & 0x20, 0);
    CHECK(ctrllog_has(0x20));
    CHECK(ctrllog_has(0x60));

    /* The device was reset and given its defaults... */
    CHECK_EQ(F.resets, 1);
    CHECK_EQ(F.auxlog[0], 0xFF);
    { uint8_t seq[] = { 0xF6 }; CHECK(auxlog_has_seq(seq, 1)); }
    /* ...the magic sequence was attempted... */
    { uint8_t seq[] = { 0xF3, 200, 0xF3, 100, 0xF3, 80, 0xF2 };
      CHECK(auxlog_has_seq(seq, (int)sizeof seq)); }
    /* ...reporting was verified and then switched back off... */
    { uint8_t seq[] = { 0xF4, 0xF5 }; CHECK(auxlog_has_seq(seq, 2)); }
    CHECK_EQ(F.reporting, 0);
    /* ...and the sample rate was left somewhere sane, not at the 80 the magic
     * sequence leaves behind. */
    CHECK_EQ(F.sample_rate, 100);

    /* Nothing is queued: bring-up cleaned up after itself, so the keyboard
     * driver will not find mouse bytes in the buffer. */
    CHECK_EQ(F.qcount, 0);
}

static void test_wheel_upgrade_accepted(void) {
    fake_reset();
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_has_wheel(), 1);
    CHECK_EQ(F.type, 3);
    CHECK_EQ(mouse_state()->plen, 4);

    /* And the driver really does read 4-byte packets from it. */
    CHECK_EQ(mouse_poll(), 0);                  /* starts the stream */
    CHECK_EQ(F.reporting, 1);
    fake_motion(4, 0, -1, MOUSE_BTN_LEFT);
    CHECK_EQ(mouse_poll(), 1);

    mouse_event_t e;
    CHECK_EQ(mouse_next_event(&e), 1);
    CHECK_EQ(e.dx, 4);
    CHECK_EQ(e.dz, -1);
    CHECK_EQ(e.pressed, MOUSE_BTN_LEFT);
    CHECK_EQ(e.flags & MOUSE_F_WHEEL, MOUSE_F_WHEEL);
}

static void test_wheel_upgrade_declined(void) {
    /* A plain mouse: it ACKs every command in the magic sequence but never
     * changes its device id. The upgrade must cost nothing. */
    fake_reset();
    F.accept_magic = 0;
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_present(), 1);
    CHECK_EQ(mouse_has_wheel(), 0);
    CHECK_EQ(mouse_state()->plen, 3);

    CHECK_EQ(mouse_poll(), 0);
    fake_motion(-3, 2, 0, 0);                   /* 3 bytes, no wheel byte */
    CHECK_EQ(mouse_poll(), 1);
    mouse_event_t e;
    CHECK_EQ(mouse_next_event(&e), 1);
    CHECK_EQ(e.dx, -3);
    CHECK_EQ(e.dy, -2);                          /* up on the wire = up screen */
    CHECK_EQ(e.dz, 0);
    CHECK_EQ(e.flags & MOUSE_F_WHEEL, 0);
}

static void test_wheel_upgrade_refused_midway(void) {
    /* A device that NAKs 0xF3 outright — the harshest fallback path, since the
     * driver is left with a device it has half-configured. It must still come
     * up as a 3-byte mouse. */
    fake_reset();
    F.nak_magic = 1;
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_present(), 1);
    CHECK_EQ(mouse_has_wheel(), 0);
    CHECK_EQ(mouse_state()->plen, 3);

    CHECK_EQ(mouse_poll(), 0);
    fake_motion(7, 0, 0, 0);
    CHECK_EQ(mouse_poll(), 1);
    int x, y;
    mouse_position(&x, &y);
    CHECK(x > 0);
}

static void test_bringup_survives_resend(void) {
    /* 0xFE is "say that again", not "no". */
    fake_reset();
    F.resend_once = 1;
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_present(), 1);
    CHECK_EQ(F.resets, 1);
}

static void test_bringup_no_bat_byte(void) {
    /* A device that ACKs the reset but posts no 0xAA self-test byte is still a
     * device; the reset reply is not where we learn the id. */
    fake_reset();
    F.no_bat = 1;
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_present(), 1);
}

static void test_bringup_no_device(void) {
    /* An 8042 with no mouse behind it: writes vanish, nothing answers. */
    fake_reset();
    F.present = 0;
    CHECK_EQ(mouse_bringup(), -1);
    CHECK_EQ(mouse_present(), 0);
    CHECK_EQ(mouse_has_wheel(), 0);

    /* Every consumer entry point must be safe and quiet on such a machine. */
    CHECK_EQ(mouse_poll(), 0);
    mouse_event_t e;
    CHECK_EQ(mouse_next_event(&e), 0);
    CHECK_EQ(mouse_start(), -1);
    CHECK_EQ(mouse_stop(), -1);
    int x = -1, y = -1;
    mouse_position(&x, &y);
    CHECK(x >= 0);
    CHECK(y >= 0);
}

static void test_bringup_dead_controller(void) {
    /* The input buffer never drains: there is no way to send a command. */
    fake_reset();
    F.ibf_stuck = 1;
    CHECK_EQ(mouse_bringup(), -1);
    CHECK_EQ(mouse_present(), 0);
    CHECK_EQ(mouse_poll(), 0);
}

static void test_bringup_nak_everything(void) {
    fake_reset();
    F.nak_everything = 1;
    CHECK_EQ(mouse_bringup(), -1);
    CHECK_EQ(mouse_present(), 0);
}

static void test_bringup_without_config_byte(void) {
    /* A controller that will not hand over its configuration byte. 0xA8 has
     * already enabled the port, so the mouse must still come up — refusing a
     * working device over an unreadable diagnostic register would be worse. */
    fake_reset();
    F.cfg_unreadable = 1;
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_present(), 1);
    CHECK_EQ(mouse_has_wheel(), 1);
}

static void test_default_backend_is_absent_device(void) {
    /* No backend installed at all: the host has no ports, and the driver must
     * conclude "no mouse" rather than read garbage or hang. */
    mouse_set_backend(NULL);
    CHECK_EQ(mouse_bringup(), -1);
    CHECK_EQ(mouse_present(), 0);

    /* A partially filled backend is refused as a whole, not used with holes. */
    mouse_backend_t half = { fake_status, NULL, fake_write_data, fake_write_cmd, NULL };
    mouse_set_backend(&half);
    CHECK_EQ(mouse_bringup(), -1);
}

/* ====================================================================== */
/* 6. the driver's polling path                                            */
/* ====================================================================== */

static void test_muted_until_a_consumer_polls(void) {
    fake_reset();
    CHECK_EQ(mouse_bringup(), 0);

    /* Nothing is listening yet, so the device is not reporting: motion produces
     * no bytes at all. This is what keeps mouse packets out of the keyboard
     * driver's path on a normal boot. */
    CHECK_EQ(F.reporting, 0);
    fake_motion(10, 10, 0, 0);
    CHECK_EQ(F.qcount, 0);

    /* The first poll turns the stream on. */
    CHECK_EQ(mouse_poll(), 0);
    CHECK_EQ(F.reporting, 1);
    fake_motion(10, 10, 0, 0);
    CHECK_EQ(mouse_poll(), 1);

    /* And it can be muted again explicitly. */
    CHECK_EQ(mouse_stop(), 0);
    CHECK_EQ(F.reporting, 0);
    fake_motion(10, 10, 0, 0);
    CHECK_EQ(F.qcount, 0);
}

static void test_stop_while_the_mouse_is_moving(void) {
    /* Stopping a device that is mid-burst is the one command that always races
     * the data stream: the ACK arrives behind bytes that look exactly like it.
     * The device must end up genuinely silent, and the driver must say so. */
    fake_reset();
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_poll(), 0);
    CHECK_EQ(F.reporting, 1);

    /* A packet is already in the buffer when the disable goes out. */
    fake_motion(0xFA - 256, -6, 0, 0);          /* delta bytes that ARE 0xFA */
    CHECK(F.qcount >= 3);

    CHECK_EQ(mouse_stop(), 0);
    CHECK_EQ(F.reporting, 0);
    CHECK_EQ(F.qcount, 0);                      /* orphaned bytes cleared */
    CHECK_EQ(mouse_state()->have, 0);

    /* And it is really stopped: nothing appears on its own any more. */
    fake_motion(5, 5, 0, 0);
    CHECK_EQ(F.qcount, 0);

    /* Polling is a statement that somebody wants data, so it re-arms the stream
     * (see mouse_stop() in mouse.h): the way to keep a stopped mouse quiet is to
     * stop polling it, and the way to keep the hardware quiet for a suspend is
     * that nothing polls while the machine is down. */
    CHECK_EQ(mouse_poll(), 0);
    CHECK_EQ(F.reporting, 1);
    fake_motion(5, 5, 0, 0);
    CHECK_EQ(mouse_poll(), 1);
}

static void test_poll_leaves_keyboard_bytes_alone(void) {
    fake_reset();
    CHECK_EQ(mouse_bringup(), 0);
    CHECK_EQ(mouse_poll(), 0);                  /* stream on, buffer empty */

    /* A keystroke lands first, then a mouse packet behind it. 0x1C is Enter in
     * scancode set 1: if the mouse driver ate it, the machine would lose a
     * submitted sentence. There is one output buffer and no way to see past its
     * head, so the correct behaviour is to stop and leave it. */
    fq_push(0x1C, 0);
    fake_motion(5, 0, 0, 0);
    int before = F.qcount;

    CHECK_EQ(mouse_poll(), 0);
    CHECK_EQ(F.qcount, before);                 /* nothing was consumed */
    CHECK_EQ(F.q[F.qhead], 0x1C);               /* the keystroke is still there */

    /* The keyboard driver takes its byte; the mouse packet is still intact
     * behind it and the next poll decodes it. */
    CHECK_EQ(fq_pop_head_value(), 0x1C);
    CHECK_EQ(mouse_poll(), 1);
    mouse_event_t e;
    CHECK_EQ(mouse_next_event(&e), 1);
    CHECK_EQ(e.dx, 5);
}

static void test_poll_tracks_a_sequence_of_moves(void) {
    fake_reset();
    CHECK_EQ(mouse_bringup(), 0);
    mouse_set_screen(320, 200);
    mouse_warp(160, 100);
    CHECK_EQ(mouse_poll(), 0);

    for (int i = 0; i < 10; i++) fake_motion(3, -2, 0, 0);
    CHECK_EQ(mouse_poll(), 10);

    int x, y;
    mouse_position(&x, &y);
    CHECK_EQ(x, 160 + 30);
    CHECK_EQ(y, 100 + 20);

    /* Drain, then push far more motion than the queue holds in one go: the
     * position must still be right, which is the property merging protects. */
    while (mouse_next_event(NULL)) { }
    for (int i = 0; i < MOUSE_QUEUE_LEN + 20; i++) fake_motion(1, 0, 0, 0);
    int drained = 0;
    while (mouse_poll() > 0) drained++;
    CHECK(drained > 0);
    mouse_position(&x, &y);
    CHECK_EQ(x, 160 + 30 + MOUSE_QUEUE_LEN + 20);

    const mouse_stats_t *s = mouse_stats();
    CHECK(s->packets >= 10u + MOUSE_QUEUE_LEN + 20u);
    CHECK_EQ(s->resyncs, 0);
    CHECK_EQ(s->stale, 0);
}

static void test_poll_recovers_from_corruption(void) {
    /* The same corruption story as test_resync_by_always_one_bit, but driven
     * through the whole driver: bytes come off a controller, and the recovery is
     * visible in the driver's own statistics. */
    fake_reset();
    CHECK_EQ(mouse_bringup(), 0);
    mouse_set_screen(500, 500);
    mouse_warp(250, 250);
    CHECK_EQ(mouse_poll(), 0);
    while (mouse_next_event(NULL)) { }

    /* Byte-for-byte: a packet with its middle byte missing, then two good ones. */
    fq_push(0x08, 1); fq_push(4, 1);            /* header, dy-read-as-dx */
    fake_motion(4, 4, 0, 0);
    fake_motion(4, 4, 0, 0);
    (void)mouse_poll();

    const mouse_stats_t *s = mouse_stats();
    CHECK(s->resyncs > 0);                      /* it noticed */

    /* Once in phase, further motion is tracked exactly. */
    int x0, y0, x1, y1;
    mouse_position(&x0, &y0);
    while (mouse_next_event(NULL)) { }
    for (int i = 0; i < 5; i++) fake_motion(2, 0, 0, 0);
    CHECK_EQ(mouse_poll(), 5);
    mouse_position(&x1, &y1);
    CHECK_EQ(x1, x0 + 10);
    CHECK_EQ(y1, y0);
    CHECK_EQ(mouse_stats()->resyncs, s->resyncs);   /* no further loss */
}

static void test_foreign_bytes_are_counted(void) {
    /* A keyboard byte sitting in the buffer when bring-up starts is discarded
     * (a command reply cannot be told from it otherwise) and counted, so the
     * cost is visible rather than silent. */
    fake_reset();
    fq_push(0x2A, 0);                            /* left shift make code */
    CHECK_EQ(mouse_bringup(), 0);
    CHECK(mouse_stats()->foreign >= 1u);
}

static void test_bringup_preserves_cursor_and_counters(void) {
    /* A fresh decoder starts with every counter at zero and the cursor in the
     * middle of the box. (The driver's own instance cannot be checked for this
     * here — by now it has run every test above — which is exactly why the
     * decoder is a struct a test can own outright.) */
    mouse_state_t fresh;
    mouse_state_init(&fresh, 800, 600);
    CHECK_EQ(fresh.st.bytes, 0);
    CHECK_EQ(fresh.st.packets, 0);
    CHECK_EQ(fresh.st.resyncs, 0);
    CHECK_EQ(fresh.st.stale, 0);
    CHECK_EQ(fresh.st.overflows, 0);
    CHECK_EQ(fresh.st.clamped, 0);
    CHECK_EQ(fresh.st.merges, 0);
    CHECK_EQ(fresh.st.foreign, 0);
    CHECK_EQ(fresh.qcount, 0);
    CHECK_EQ(fresh.have, 0);
    CHECK_EQ(fresh.plen, 3);
    CHECK_EQ(fresh.x, 400);
    CHECK_EQ(fresh.y, 300);

    /* Re-initialising the hardware (a resume) must not teleport the pointer or
     * forget the clamp box, and the health counters are cumulative for the life
     * of the driver: a resume that reset them would hide a mouse that resyncs
     * constantly behind a suspend. */
    fake_reset();
    CHECK_EQ(mouse_bringup(), 0);
    mouse_set_screen(640, 480);
    mouse_warp(123, 45);
    CHECK_EQ(mouse_poll(), 0);
    fake_motion(1, 1, 0, 0);
    CHECK_EQ(mouse_poll(), 1);

    uint32_t packets_before = mouse_stats()->packets;
    CHECK(packets_before > 0);

    CHECK_EQ(mouse_bringup(), 0);
    int x, y;
    mouse_position(&x, &y);
    CHECK_EQ(x, 124);
    CHECK_EQ(y, 44);
    CHECK_EQ(mouse_state()->w, 640);
    CHECK_EQ(mouse_state()->h, 480);
    CHECK_EQ(mouse_stats()->packets, packets_before);
    /* ...but the partial packet and the event queue do NOT survive: the byte
     * stream has a discontinuity across a re-initialisation. */
    CHECK_EQ(mouse_state()->have, 0);
    CHECK_EQ(mouse_state()->qcount, 0);
}

int main(void) {
    printf("mouse\n");
    RUN(test_decode_every_framing_byte);
    RUN(test_decode_nine_bit_signs);
    RUN(test_decode_overflow_bits);
    RUN(test_decode_wheel_byte);
    RUN(test_decode_rejects_nonsense);

    RUN(test_stream_basic);
    RUN(test_button_edges);
    RUN(test_resync_by_always_one_bit);
    RUN(test_resync_by_partial_timeout);
    RUN(test_resync_converges_after_byte_loss);
    RUN(test_wheel_length_change_drops_partial);

    RUN(test_clamp_all_four_edges);
    RUN(test_screen_resize_keeps_cursor_inside);
    RUN(test_queue_overflow_merges_and_keeps_edges);

    RUN(test_bringup_sequence);
    RUN(test_wheel_upgrade_accepted);
    RUN(test_wheel_upgrade_declined);
    RUN(test_wheel_upgrade_refused_midway);
    RUN(test_bringup_survives_resend);
    RUN(test_bringup_no_bat_byte);
    RUN(test_bringup_no_device);
    RUN(test_bringup_dead_controller);
    RUN(test_bringup_nak_everything);
    RUN(test_bringup_without_config_byte);
    RUN(test_default_backend_is_absent_device);

    RUN(test_muted_until_a_consumer_polls);
    RUN(test_stop_while_the_mouse_is_moving);
    RUN(test_poll_leaves_keyboard_bytes_alone);
    RUN(test_poll_tracks_a_sequence_of_moves);
    RUN(test_poll_recovers_from_corruption);
    RUN(test_foreign_bytes_are_counted);
    RUN(test_bringup_preserves_cursor_and_counters);
    return th_report("mouse");
}
