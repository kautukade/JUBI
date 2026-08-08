/* test_audio.c — host tests for the audio service and its tool family.
 *
 * WHAT IS COVERED HERE, AND WHY IT CAN BE
 *   core/audio.c reaches hardware through exactly two things: a native driver's
 *   audio_ops_t, and dvm_run() against a dvm_io_t. Both are injectable, so this
 *   suite supplies them and every layer above becomes host-testable:
 *
 *     synthesis      frequency measured from the samples, amplitude and clipping
 *                    read out of the buffer, sample counts compared to arithmetic
 *                    done independently here. Not "it sounds right" — numbers.
 *     registration   both kinds of sink, replacement, the device-tree binding,
 *                    and every argument a registrar can get wrong.
 *     no sink        every entry point, including that "no driver" beats "bad
 *                    frequency" when the arguments are ALSO nonsense.
 *     the VM path    the real vm/dvm.c is linked and a real assembled play
 *                    program drives a synthetic port device. The suite then reads
 *                    the PCM back out of the address that program handed its
 *                    device, which is the closest thing to "a driver played the
 *                    tone" that can exist without a speaker.
 *     the tools      argument bounds, the trace lines, and that a rejected call
 *                    played nothing.
 *
 * WHAT IS NOT COVERED HERE
 *   That a real sound card does what a real play program tells it to. That is
 *   what booting with an audio device attached is for. This suite proves the
 *   kernel's half: the samples are right, the contract is honoured exactly as
 *   documented, and no argument can make the service lie about what happened.
 *
 * REGISTER_TOOL / REGISTER_DRIVER ON A MACH-O HOST
 *   tools/audio_tools.c is #included rather than linked so tooltable.h's
 *   REGISTER_TOOL redefinition applies to it (and so the tests can reach its
 *   statics). core/audio.c is LINKED, and its REGISTER_DRIVER lands in a
 *   "driver_table" section nobody in this binary walks — the same arrangement
 *   drivers/rtc/rtc.c already has under test_rtc.
 */

#include "harness.h"

#include "audio.h"
#include "device.h"
#include "driver.h"
#include "dvm.h"
#include "json.h"
#include "trace.h"
#include "tool.h"
#include "tooltable.h"
/* CHAT_TOOL_RESULT_CAP: the size chat.c really hands a tool, as opposed to
 * tool.h's TOOL_RESULT_MAX that this file's own buffer uses. The no-sink result
 * is asserted against it because that is the number a model is bounded by. */
#include "chat.h"

#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
/* Apple's <string.h> defines memcpy/memset/... as fortifying macros, which
 * collide with kernel.h's plain prototypes when both land in one TU. */
#undef memcpy
#undef memset
#undef memmove
#undef memcmp
#undef strlen
#endif

#include "../../tools/audio_tools.c"

/* ====================================================================== */
/* formats used throughout                                                */
/* ====================================================================== */

static const audio_format_t FMT_48_STEREO = { 48000, 2, 16 };
static const audio_format_t FMT_48_MONO   = { 48000, 1, 16 };

/* Frames the buffer holds, computed here independently of the service. */
static uint32_t cap_frames(const audio_format_t *f) {
    return AUDIO_PCM_BYTES / (f->channels * 2u);
}

/* ====================================================================== */
/* a synthetic native sink                                                */
/* ====================================================================== */

static struct {
    int      calls;
    uint64_t phys;
    uint32_t bytes, frames, ms;
    int      fail;            /* make play() refuse                        */
    const char *why;          /* ...with this reason                       */
    int      stop_calls;
    int      stop_fail;
} nat;

static int nat_play(void *ctx, uint64_t phys, uint32_t bytes, uint32_t frames,
                    uint32_t ms, char *why, size_t cap) {
    (void)ctx;
    nat.calls++;
    nat.phys = phys; nat.bytes = bytes; nat.frames = frames; nat.ms = ms;
    if (nat.fail) {
        snprintf(why, cap, "%s", nat.why ? nat.why : "no reason");
        return -1;
    }
    return 0;
}

static int nat_stop(void *ctx) {
    (void)ctx;
    nat.stop_calls++;
    return nat.stop_fail ? -1 : 0;
}

static const audio_ops_t nat_ops = { NULL, nat_play, nat_stop };

static void nat_clear(void) { memset(&nat, 0, sizeof nat); }

/* The PCM the sink was handed, read back through the physical address it was
 * given rather than through the service's own accessor — the low 4 GiB is
 * identity-mapped on the real kernel and this mirrors that. */
static const int16_t *given_pcm(void) {
    return (const int16_t *)(uintptr_t)nat.phys;
}

static void register_native(const audio_format_t *f) {
    nat_clear();
    CHECK_EQ(audio_register_native("testsink", NULL, f, &nat_ops), AUDIO_OK);
}

/* ====================================================================== */
/* a synthetic port-mapped audio device, for the VM play path             */
/* ====================================================================== */

/* Deliberately a made-up register layout that resembles no real chip: the point
 * of the service is that it knows nothing about any of them, so the test must
 * not smuggle a real one in. BAR0 is at 0xC000.
 *
 *   +0x10  w32  buffer physical address
 *   +0x14  w32  buffer length in bytes
 *   +0x18  w8   control: writing 1 starts playback
 *   +0x1c  r8   status: bit 0 set while the device is still reading
 */
#define DEVBASE   0xC000u
#define DEV_ADDR  0x10u
#define DEV_LEN   0x14u
#define DEV_CTL   0x18u
#define DEV_STA   0x1Cu

static struct {
    uint32_t addr, len;
    int      started;
    int      busy_reads;      /* status reads left before it reports done  */
    int      status_reads;
    uint64_t delay_us;
    int      denied;          /* accesses outside the modelled registers   */
} dev;

static void dev_reset(int busy_reads) {
    memset(&dev, 0, sizeof dev);
    dev.busy_reads = busy_reads;
}

static void dev_write(void *ctx, uint16_t port, uint8_t width, uint32_t val) {
    (void)ctx; (void)width;
    switch (port - DEVBASE) {
        case DEV_ADDR: dev.addr = val; break;
        case DEV_LEN:  dev.len  = val; break;
        case DEV_CTL:  if (val & 1u) dev.started++; break;
        default:       dev.denied++;   break;
    }
}

static uint32_t dev_read(void *ctx, uint16_t port, uint8_t width) {
    (void)ctx; (void)width;
    if (port - DEVBASE == DEV_STA) {
        dev.status_reads++;
        if (dev.busy_reads > 0) { dev.busy_reads--; return 1; }   /* still busy */
        return 0;                                                /* finished   */
    }
    dev.denied++;
    return 0xFFFFFFFFu;
}

static void dev_delay(void *ctx, uint32_t us) { (void)ctx; dev.delay_us += us; }

static const dvm_io_t dev_io = {
    NULL, dev_write, dev_read, NULL, NULL, NULL, dev_delay
};

/* A play program that satisfies AUDIO_VM_CONTRACT: it takes the buffer from r7
 * and its length from r8, starts the device, polls until the device says it has
 * finished with the memory, and halts. Written the way a model would have to
 * write it, against nothing but the contract. */
static const char PLAY_SRC[] =
    ".equ REG_ADDR 0x10\n"
    ".equ REG_LEN  0x14\n"
    ".equ REG_CTL  0x18\n"
    ".equ REG_STA  0x1c\n"
    "add r1, r0, REG_ADDR\n"
    "out32 r1, r7\n"
    "add r1, r0, REG_LEN\n"
    "out32 r1, r9\n"
    "add r1, r0, REG_CTL\n"
    "out8 r1, 1\n"
    "add r1, r0, REG_STA\n"
    "mov r2, 500\n"
    "poll:\n"
    "  in8 r3, r1\n"
    "  test r3, 1\n"
    "  beq done\n"
    "  delay 1000\n"
    "  sub r2, 1\n"
    "  cmp r2, 0\n"
    "  bne poll\n"
    "  abort \"the device never released the buffer\"\n"
    "done:\n"
    "halt\n";

static dvm_program_t g_test_prog;

/* Assemble `src` into g_test_prog, failing the check (not the process) if the
 * program the suite itself wrote does not build. */
static int assemble(const char *src) {
    dvm_asm_err_t err;
    memset(&err, 0, sizeof err);
    int rc = dvm_assemble(src, strlen(src), &g_test_prog, &err);
    if (rc != 0) {
        th_checks++; th_fails++;
        printf("    FAIL the suite's own program does not assemble: line %d: %s\n",
               err.line, err.msg);
    }
    return rc;
}

/* The sandbox a driver_run caller would have built, plus the DMA grant the audio
 * service requires: the whole arena, starting at its base. */
static void vm_spec_init(audio_vm_spec_t *sp) {
    uint64_t dbase = 0, dsize = 0;
    dvm_dma_region(&dbase, &dsize);
    memset(sp, 0, sizeof *sp);
    dvm_policy_init(&sp->policy);
    dvm_policy_allow_ports(&sp->policy, DEVBASE, DEVBASE + 0x3F);
    CHECK_EQ(dvm_policy_allow_dma(&sp->policy, dbase, dsize), 0);
    sp->program = &g_test_prog;
    sp->io      = &dev_io;
    sp->bar[0]  = DEVBASE;
    sp->bdf     = 0x0028;      /* 00:05.0, as driver_targets would report it */
}

/* ====================================================================== */
/* measurement helpers — this is how "the frequency is right" is asserted  */
/* ====================================================================== */

/* Every gap between sign changes must be exactly `half`, which for a square
 * wave IS the frequency: half = rate / (2 * hz). Returns the number of sign
 * changes seen, or -1 if any gap was wrong. */
static int square_half_period(const int16_t *p, uint32_t frames, uint32_t ch,
                              uint32_t half) {
    int last = -1, n = 0;
    int prev = p[0] > 0;
    for (uint32_t i = 1; i < frames; i++) {
        int cur = p[(size_t)i * ch] > 0;
        if (cur == prev) continue;
        if (last >= 0 && (uint32_t)((int)i - last) != half) return -1;
        last = (int)i;
        n++;
        prev = cur;
    }
    return n;
}

static void extremes(const int16_t *p, uint32_t frames, uint32_t ch,
                     int *lo, int *hi) {
    *lo = 32767; *hi = -32768;
    for (uint32_t i = 0; i < frames; i++) {
        int v = p[(size_t)i * ch];
        if (v < *lo) *lo = v;
        if (v > *hi) *hi = v;
    }
}

/* ====================================================================== */
/* tool plumbing                                                          */
/* ====================================================================== */

static char t_out[TOOL_RESULT_MAX];

/* Dispatch `name` with `input` and return the result text. */
static const char *call_tool(const char *name, const char *input, int *is_err) {
    tool_call_t   c;
    tool_result_t r;
    c.id = "toolu_test"; c.name = name;
    c.input = input; c.input_len = strlen(input);
    tool_result_init(&r, t_out, sizeof t_out);
    tool_dispatch(&c, &r);
    if (is_err) *is_err = r.is_error;
    return t_out;
}

/* ====================================================================== */
/* 1. synthesis                                                           */
/* ====================================================================== */

static void test_format_validation(void) {
    char why[AUDIO_WHY_MAX];

    CHECK_EQ(audio_frame_bytes(&FMT_48_STEREO), 4);
    CHECK_EQ(audio_frame_bytes(&FMT_48_MONO), 2);
    CHECK_EQ(audio_frame_bytes(NULL), 0);
    CHECK(audio_format_valid(&FMT_48_STEREO, why, sizeof why));

    audio_format_t f = FMT_48_STEREO;
    f.bits = 8;
    CHECK(!audio_format_valid(&f, why, sizeof why));
    CHECK_CONTAINS(why, "16-bit");

    f = FMT_48_STEREO; f.channels = 3;
    CHECK(!audio_format_valid(&f, why, sizeof why));
    CHECK_CONTAINS(why, "channels");

    f = FMT_48_STEREO; f.channels = 0;
    CHECK(!audio_format_valid(&f, why, sizeof why));

    f = FMT_48_STEREO; f.rate_hz = 10;
    CHECK(!audio_format_valid(&f, why, sizeof why));
    CHECK_CONTAINS(why, "Hz is outside");

    f = FMT_48_STEREO; f.rate_hz = 4000000;
    CHECK(!audio_format_valid(&f, why, sizeof why));

    CHECK(!audio_format_valid(NULL, why, sizeof why));
    CHECK_CONTAINS(why, "no format");
}

/* The buffer's own address cannot be asserted on here — a host process puts .bss
 * above 4 GiB on arm64 — but the predicate registration applies to it can be, and
 * that is where the arithmetic lives. */
static void test_dma_reachability(void) {
    CHECK_EQ(audio_dma_reachable(0x10000, AUDIO_PCM_BYTES), 1);
    CHECK_EQ(audio_dma_reachable(0xFFFF0000ull, 0x10000ull), 1);  /* exactly 4 GiB */
    CHECK_EQ(audio_dma_reachable(0xFFFF0000ull, 0x10001ull), 0);  /* one past    */
    CHECK_EQ(audio_dma_reachable(0x100000000ull, 16), 0);
    CHECK_EQ(audio_dma_reachable(0x1FFFFFFFFull, 16), 0);
    CHECK_EQ(audio_dma_reachable(0, 16), 0);                      /* null        */
    CHECK_EQ(audio_dma_reachable(0x10000, 0), 0);                 /* empty       */
    CHECK_EQ(audio_dma_reachable(0xFFFFFFFFFFFFFFF0ull, 0x100), 0); /* wraps     */
}

static void test_frames_for_ms(void) {
    /* Arithmetic done independently of the code under test. */
    CHECK_EQ(audio_frames_for_ms(&FMT_48_STEREO, 250), 12000);
    CHECK_EQ(audio_frames_for_ms(&FMT_48_STEREO, 1), 48);
    CHECK_EQ(audio_frames_for_ms(&FMT_48_STEREO, 0), 0);

    const audio_format_t cd = { 44100, 2, 16 };
    CHECK_EQ(audio_frames_for_ms(&cd, 1000), 44100);
    CHECK_EQ(audio_frames_for_ms(&cd, 3), 132);          /* truncated, not 132.3 */

    /* An absurd duration saturates instead of wrapping to something small and
     * plausible, which is what would make a bad request look like a good one. */
    CHECK_EQ(audio_frames_for_ms(&FMT_48_STEREO, 0xFFFFFFFFu), 0xFFFFFFFFu);
    CHECK_EQ(audio_frames_for_ms(NULL, 100), 0);
}

static void test_square_frequency_and_amplitude(void) {
    static int16_t buf[8192];
    /* 1000 Hz at 48 kHz mono: 48 frames per period, 24 per half. 100 ms is
     * 4800 frames and exactly 100 periods. */
    size_t n = audio_synth(buf, 8192, &FMT_48_MONO, 1000, 100, 12000,
                           AUDIO_WAVE_SQUARE);
    CHECK_EQ(n, 4800);

    int changes = square_half_period(buf, (uint32_t)n, 1, 24);
    CHECK(changes > 0);                 /* -1 means a gap was the wrong width */
    /* Sign changes at 24, 72, ... 4776: (4776-24)/24 + 1 = 199. */
    CHECK_EQ(changes, 199);

    /* A square wave is only ever +/-amplitude, and frame 0 is positive. */
    int lo, hi;
    extremes(buf, (uint32_t)n, 1, &lo, &hi);
    CHECK_EQ(hi, 12000);
    CHECK_EQ(lo, -12000);
    CHECK_EQ(buf[0], 12000);
    CHECK_EQ(buf[24], -12000);
    CHECK_EQ(buf[48], 12000);

    int only_two = 1;
    for (size_t i = 0; i < n; i++)
        if (buf[i] != 12000 && buf[i] != -12000) only_two = 0;
    CHECK(only_two);
}

static void test_sine_shape_and_amplitude(void) {
    static int16_t buf[8192];
    /* 1000 Hz at 48 kHz: period 48 frames, so the peaks land exactly on frames
     * 12 and 36 of every period. That exactness is the whole reason phase is
     * recomputed per frame rather than accumulated. */
    size_t n = audio_synth(buf, 8192, &FMT_48_MONO, 1000, 50, 30000,
                           AUDIO_WAVE_SINE);
    CHECK_EQ(n, 2400);

    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[12], 30000);           /* exact peak, no rounding slop */
    CHECK_EQ(buf[24], 0);
    CHECK_EQ(buf[36], -30000);
    CHECK_EQ(buf[48], 0);
    CHECK_EQ(buf[60], 30000);           /* and again a period later    */
    CHECK_EQ(buf[2388], -30000);        /* and in the last period      */

    /* Monotonic through the first quarter, and never outside the amplitude. */
    int rising = 1;
    for (int i = 1; i <= 12; i++) if (buf[i] <= buf[i - 1]) rising = 0;
    CHECK(rising);

    int lo, hi;
    extremes(buf, (uint32_t)n, 1, &lo, &hi);
    CHECK_EQ(hi, 30000);
    CHECK_EQ(lo, -30000);
}

static void test_triangle_shape(void) {
    static int16_t buf[4096];
    size_t n = audio_synth(buf, 4096, &FMT_48_MONO, 1000, 20, 20000,
                           AUDIO_WAVE_TRIANGLE);
    CHECK_EQ(n, 960);

    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[12], 20000);           /* peak at a quarter period */
    CHECK_EQ(buf[24], 0);
    CHECK_EQ(buf[36], -20000);

    /* Linear through the first quarter: rising, with steps that differ by at most
     * one. They are not all identical, and cannot be — the per-frame phase
     * advance here is 2^32/48 turns, which is not a whole number, so the ramp
     * lands one LSB either side of the ideal line. What matters is that it is
     * monotonic and evenly spaced, and that the PEAK is exact (above), because
     * the peak is where a quarter period lands on an integer frame. */
    int step = buf[1] - buf[0];
    int rising = 1, even = 1;
    for (int i = 2; i <= 12; i++) {
        int d = buf[i] - buf[i - 1];
        if (d <= 0) rising = 0;
        if (d != step && d != step + 1 && d != step - 1) even = 0;
    }
    CHECK(rising);
    CHECK(even);

    int lo, hi;
    extremes(buf, (uint32_t)n, 1, &lo, &hi);
    CHECK_EQ(hi, 20000);
    CHECK_EQ(lo, -20000);
}

static void test_amplitude_clipping(void) {
    static int16_t buf[2048];

    /* An amplitude far above full scale is CLAMPED, not wrapped. Wrapping is the
     * failure that matters: a sample that overflows to the opposite sign is a
     * loud click, and it would be produced by exactly the arithmetic a naive
     * implementation uses. */
    for (int w = 0; w < AUDIO_WAVE__COUNT; w++) {
        size_t n = audio_synth(buf, 2048, &FMT_48_MONO, 1000, 20, 0xFFFFFFFFu,
                               (audio_wave_t)w);
        CHECK_EQ(n, 960);
        int lo, hi;
        extremes(buf, (uint32_t)n, 1, &lo, &hi);
        CHECK_EQ(hi, (int)AUDIO_AMP_MAX);
        CHECK_EQ(lo, -(int)AUDIO_AMP_MAX);
        /* -32768 would mean an overflow past full scale in the negative
         * direction, which is the asymmetry that clips audibly. */
        int wrapped = 0;
        for (size_t i = 0; i < n; i++) if (buf[i] == -32768) wrapped = 1;
        CHECK(!wrapped);
    }

    /* And exactly at full scale, nothing changes. */
    size_t n = audio_synth(buf, 2048, &FMT_48_MONO, 1000, 20, AUDIO_AMP_MAX,
                           AUDIO_WAVE_SINE);
    int lo, hi;
    extremes(buf, (uint32_t)n, 1, &lo, &hi);
    CHECK_EQ(hi, 32767);
    CHECK_EQ(lo, -32767);

    /* amplitude 0 is a muted note: exact silence, not an error. */
    n = audio_synth(buf, 2048, &FMT_48_MONO, 1000, 20, 0, AUDIO_WAVE_SINE);
    CHECK_EQ(n, 960);
    extremes(buf, (uint32_t)n, 1, &lo, &hi);
    CHECK_EQ(hi, 0);
    CHECK_EQ(lo, 0);
}

static void test_synth_adversarial(void) {
    static int16_t buf[4096];
    for (size_t i = 0; i < 4096; i++) buf[i] = 0x5A5A;

    /* A rest: exact digital silence, and a success rather than an error. */
    size_t n = audio_synth(buf, 4096, &FMT_48_MONO, 0, 10, 20000, AUDIO_WAVE_SINE);
    CHECK_EQ(n, 480);
    int lo, hi;
    extremes(buf, (uint32_t)n, 1, &lo, &hi);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 0);

    /* Above Nyquist there is no waveform, only aliasing. Refused. */
    CHECK_EQ(audio_synth(buf, 4096, &FMT_48_MONO, 24000, 10, 20000,
                         AUDIO_WAVE_SINE), 0);
    CHECK_EQ(audio_synth(buf, 4096, &FMT_48_MONO, 30000, 10, 20000,
                         AUDIO_WAVE_SINE), 0);
    /* Just below it is fine. */
    CHECK(audio_synth(buf, 4096, &FMT_48_MONO, 23999, 10, 20000,
                      AUDIO_WAVE_SINE) > 0);

    /* 20 GHz. Rejected as a frequency, not wrapped into an audible one. */
    CHECK_EQ(audio_synth(buf, 4096, &FMT_48_MONO, 20000000000u % 0xFFFFFFFFu,
                         10, 20000, AUDIO_WAVE_SINE), 0);
    CHECK_EQ(audio_synth(buf, 4096, &FMT_48_MONO, 0xFFFFFFFFu, 10, 20000,
                         AUDIO_WAVE_SINE), 0);

    CHECK_EQ(audio_synth(buf, 4096, &FMT_48_MONO, 1000, 0, 20000,
                         AUDIO_WAVE_SINE), 0);      /* no duration, no samples */
    CHECK_EQ(audio_synth(NULL, 4096, &FMT_48_MONO, 1000, 10, 20000,
                         AUDIO_WAVE_SINE), 0);
    CHECK_EQ(audio_synth(buf, 0, &FMT_48_MONO, 1000, 10, 20000,
                         AUDIO_WAVE_SINE), 0);
    CHECK_EQ(audio_synth(buf, 4096, NULL, 1000, 10, 20000, AUDIO_WAVE_SINE), 0);
    CHECK_EQ(audio_synth(buf, 4096, &FMT_48_MONO, 1000, 10, 20000,
                         (audio_wave_t)99), 0);

    /* A short destination truncates the SOUND, never the memory after it. */
    static int16_t small[64];
    for (int i = 0; i < 64; i++) small[i] = 0x1234;
    CHECK_EQ(audio_synth(small, 32, &FMT_48_MONO, 1000, 1000, 20000,
                         AUDIO_WAVE_SQUARE), 32);
    CHECK_EQ(small[32], 0x1234);        /* untouched past the bound */
    CHECK_EQ(small[63], 0x1234);

    /* An enormous duration is bounded by the destination, not by luck. */
    CHECK_EQ(audio_synth(small, 32, &FMT_48_MONO, 1000, 0xFFFFFFFFu, 20000,
                         AUDIO_WAVE_SQUARE), 32);
    CHECK_EQ(small[32], 0x1234);
}

static void test_stereo_interleave(void) {
    static int16_t buf[512];
    size_t n = audio_synth(buf, 128, &FMT_48_STEREO, 1000, 100, 10000,
                           AUDIO_WAVE_SINE);
    CHECK_EQ(n, 128);                   /* clipped to dst_frames */
    int same = 1;
    for (size_t i = 0; i < n; i++)
        if (buf[i * 2] != buf[i * 2 + 1]) same = 0;
    CHECK(same);                        /* both channels carry the same tone */
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[24], 10000);           /* frame 12, left channel  */
    CHECK_EQ(buf[25], 10000);           /* frame 12, right channel */
}

static void test_wave_names(void) {
    audio_wave_t w = AUDIO_WAVE_SQUARE;
    CHECK_STR(audio_wave_name(AUDIO_WAVE_SINE), "sine");
    CHECK_STR(audio_wave_name(AUDIO_WAVE_SQUARE), "square");
    CHECK_STR(audio_wave_name(AUDIO_WAVE_TRIANGLE), "triangle");
    CHECK_STR(audio_wave_name((audio_wave_t)77), "?");

    CHECK_EQ(audio_wave_from_name("sine", &w), 0);
    CHECK_EQ(w, AUDIO_WAVE_SINE);
    CHECK_EQ(audio_wave_from_name("triangle", &w), 0);
    CHECK_EQ(w, AUDIO_WAVE_TRIANGLE);
    CHECK_EQ(audio_wave_from_name("Sine", &w), -1);       /* exact match only */
    CHECK_EQ(audio_wave_from_name("sin", &w), -1);
    CHECK_EQ(audio_wave_from_name("sinewave", &w), -1);
    CHECK_EQ(audio_wave_from_name("", &w), -1);
    CHECK_EQ(audio_wave_from_name(NULL, &w), -1);
    CHECK_EQ(w, AUDIO_WAVE_TRIANGLE);                    /* untouched on failure */

    CHECK_STR(audio_sink_kind_name(AUDIO_SINK_NONE), "none");
    CHECK_STR(audio_sink_kind_name(AUDIO_SINK_NATIVE), "native");
    CHECK_STR(audio_sink_kind_name(AUDIO_SINK_VM), "vm-program");
}

/* ====================================================================== */
/* 2. no sink — every failure path                                        */
/* ====================================================================== */

static void test_no_sink_fails_everything(void) {
    audio_reset();

    CHECK_EQ(audio_available(), 0);
    CHECK_EQ(audio_max_ms(), 0);

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.present, 0);
    CHECK_EQ(s.kind, AUDIO_SINK_NONE);
    CHECK_STR(s.name, "");
    CHECK_STR(s.device, "");
    CHECK_EQ(s.plays, 0);
    CHECK_EQ(s.fmt.rate_hz, 0);      /* no sink, no sample rate to report */
    CHECK_EQ(s.fmt.channels, 0);
    /* The buffer exists whether or not anything can play out of it, and saying
     * so is what lets a driver author know where to point the device. */
    CHECK_EQ(s.buf_bytes, AUDIO_PCM_BYTES);
    CHECK(s.buf_phys != 0);

    CHECK_EQ(audio_tone(440, 250), AUDIO_ENOSINK);
    CHECK_CONTAINS(audio_last_error(), "no audio output driver is registered");

    CHECK_EQ(audio_tone_ex(440, 250, 8000, AUDIO_WAVE_SQUARE), AUDIO_ENOSINK);

    audio_note_t nt = { 440, 100, 8000, AUDIO_WAVE_SINE };
    CHECK_EQ(audio_play_notes(&nt, 1), AUDIO_ENOSINK);

    static int16_t pcm[64];
    CHECK_EQ(audio_play_pcm(pcm, 32), AUDIO_ENOSINK);
    CHECK_EQ(audio_stop(), AUDIO_ENOSINK);

    /* Nothing counted as a play or a failure: there was no sink to fail. */
    audio_status(&s);
    CHECK_EQ(s.plays, 0);
    CHECK_EQ(s.failures, 0);

    /* THE ORDERING THAT MATTERS. With arguments that are ALSO nonsense, the
     * answer must still be "there is no audio driver" — otherwise a silent
     * machine sends the model off fixing a frequency that was never the
     * problem. */
    CHECK_EQ(audio_tone(0, 0), AUDIO_ENOSINK);
    CHECK_CONTAINS(audio_last_error(), "no audio output driver is registered");
    CHECK_EQ(audio_tone_ex(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                           (audio_wave_t)200), AUDIO_ENOSINK);
    CHECK_CONTAINS(audio_last_error(), "no audio output driver is registered");
    CHECK_EQ(audio_play_notes(NULL, 0), AUDIO_ENOSINK);

    /* The one sentence, in one place. */
    CHECK_CONTAINS(audio_no_sink_reason(), "no audio output driver is registered");
}

/* ====================================================================== */
/* 3. registration                                                        */
/* ====================================================================== */

static void test_register_native(void) {
    audio_reset();
    nat_clear();

    CHECK_EQ(audio_register_native("snd0", NULL, &FMT_48_STEREO, &nat_ops),
             AUDIO_OK);
    CHECK_EQ(audio_available(), 1);
    CHECK_STR(audio_last_error(), "");

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.present, 1);
    CHECK_EQ(s.kind, AUDIO_SINK_NATIVE);
    CHECK_STR(s.name, "snd0");
    CHECK_STR(s.device, "");
    CHECK_EQ(s.fmt.rate_hz, 48000);
    CHECK_EQ(s.fmt.channels, 2);
    CHECK_EQ(s.vm_insns, 0);

    /* 196608 bytes / 4 = 49152 frames = 1024 ms at 48 kHz, capped at
     * AUDIO_MAX_MS. */
    CHECK_EQ(cap_frames(&FMT_48_STEREO), 49152);
    CHECK_EQ(audio_max_ms(), AUDIO_MAX_MS);

    /* A slow mono sink still gets the same cap, because the cap is a policy
     * ceiling and not a buffer accident. */
    const audio_format_t slow = { 8000, 1, 16 };
    CHECK_EQ(audio_register_native("snd1", NULL, &slow, &nat_ops), AUDIO_OK);
    CHECK_EQ(audio_max_ms(), AUDIO_MAX_MS);
}

static void test_register_rejects(void) {
    audio_reset();

    /* Bad names. */
    CHECK_EQ(audio_register_native(NULL, NULL, &FMT_48_STEREO, &nat_ops),
             AUDIO_EINVAL);
    CHECK_EQ(audio_register_native("", NULL, &FMT_48_STEREO, &nat_ops),
             AUDIO_EINVAL);
    CHECK_EQ(audio_register_native("has space", NULL, &FMT_48_STEREO, &nat_ops),
             AUDIO_EINVAL);
    CHECK_EQ(audio_register_native("bad\nname", NULL, &FMT_48_STEREO, &nat_ops),
             AUDIO_EINVAL);
    /* Brackets are what a forged kernel trace line is made of, so a name cannot
     * contain one even though nothing prints it at column zero today. */
    CHECK_EQ(audio_register_native("has[bracket]", NULL, &FMT_48_STEREO,
                                   &nat_ops), AUDIO_EINVAL);
    CHECK_EQ(audio_register_native("has]bracket", NULL, &FMT_48_STEREO,
                                   &nat_ops), AUDIO_EINVAL);
    CHECK_EQ(audio_register_native("thisnameisfarlongerthanthelimitallows",
                                   NULL, &FMT_48_STEREO, &nat_ops), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "printable characters");
    CHECK_EQ(audio_available(), 0);

    /* Bad formats. */
    audio_format_t f = FMT_48_STEREO;
    f.bits = 24;
    CHECK_EQ(audio_register_native("snd", NULL, &f, &nat_ops), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "cannot fill");
    CHECK_EQ(audio_register_native("snd", NULL, NULL, &nat_ops), AUDIO_EINVAL);

    /* No play hook: a sink that could never be asked for a sound. */
    audio_ops_t bad = { NULL, NULL, NULL };
    CHECK_EQ(audio_register_native("snd", NULL, &FMT_48_STEREO, &bad),
             AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "no play() hook");
    CHECK_EQ(audio_register_native("snd", NULL, &FMT_48_STEREO, NULL),
             AUDIO_EINVAL);
    CHECK_EQ(audio_available(), 0);
}

static void test_refused_registration_keeps_the_old_sink(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    /* A registration that is refused must not half-replace the working one:
     * losing audio because a second driver was misconfigured would be a
     * regression the operator cannot see until the next sound. */
    audio_format_t f = FMT_48_STEREO;
    f.channels = 7;
    CHECK_EQ(audio_register_native("other", NULL, &f, &nat_ops), AUDIO_EINVAL);

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.present, 1);
    CHECK_STR(s.name, "testsink");
    CHECK_EQ(s.fmt.channels, 2);
    CHECK_EQ(audio_tone(440, 10), AUDIO_OK);      /* still works */
}

static void test_replacement_and_device_binding(void) {
    audio_reset();
    nat_clear();

    device_t *d1 = device_create("snda", DEV_CLASS_AUDIO);
    device_t *d2 = device_create("sndb", DEV_CLASS_AUDIO);
    CHECK(d1 && d2);
    if (!d1 || !d2) return;
    device_add_resource(d1, RES_IOPORT, DEVBASE, 0x40);
    device_register(d1);
    device_register(d2);

    CHECK_EQ(audio_register_native("first", d1, &FMT_48_STEREO, &nat_ops),
             AUDIO_OK);
    /* The device tree tells the truth: `driver=` names the sink. */
    CHECK(d1->driver != NULL);
    if (d1->driver) {
        CHECK_STR(d1->driver->name, "first");
        CHECK_EQ(d1->driver->cls, DEV_CLASS_AUDIO);
    }
    CHECK_EQ(d1->state, DEV_STATE_ACTIVE);

    audio_status_t s;
    audio_status(&s);
    CHECK_STR(s.device, "snda");

    /* Replacing the sink releases the old device, so it is honestly unclaimed
     * again rather than showing a driver that is no longer driving it. */
    CHECK_EQ(audio_register_native("second", d2, &FMT_48_MONO, &nat_ops),
             AUDIO_OK);
    CHECK(d1->driver == NULL);
    CHECK_EQ(d1->state, DEV_STATE_REGISTERED);
    CHECK(d2->driver != NULL);
    if (d2->driver) CHECK_STR(d2->driver->name, "second");
    CHECK_EQ(d2->state, DEV_STATE_ACTIVE);
    audio_status(&s);
    CHECK_STR(s.name, "second");
    CHECK_STR(s.device, "sndb");
    CHECK_EQ(s.fmt.channels, 1);
    /* Counters belong to the sink, not to the machine. */
    CHECK_EQ(s.plays, 0);

    audio_release();
    CHECK_EQ(audio_available(), 0);
    audio_status(&s);
    CHECK_EQ(s.fmt.rate_hz, 0);      /* the format went with the sink */
    CHECK(d2->driver == NULL);
    CHECK_EQ(d2->state, DEV_STATE_REGISTERED);
    audio_release();                     /* idempotent */
    CHECK_EQ(audio_available(), 0);
}

/* ====================================================================== */
/* 4. the native play path                                                */
/* ====================================================================== */

static void test_native_play(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    CHECK_EQ(audio_tone_ex(1000, 100, 12000, AUDIO_WAVE_SQUARE), AUDIO_OK);
    CHECK_EQ(nat.calls, 1);
    CHECK_EQ(nat.frames, 4800);
    CHECK_EQ(nat.bytes, 4800 * 4);       /* stereo, 16-bit */
    CHECK_EQ(nat.ms, 100);
    CHECK_EQ(nat.phys, audio_buffer_phys());

    /* The samples the sink was pointed at really are the tone: read them back
     * through the address it was handed, and measure. */
    const int16_t *pcm = given_pcm();
    CHECK_EQ(square_half_period(pcm, nat.frames, 2, 24), 199);
    int lo, hi;
    extremes(pcm, nat.frames, 2, &lo, &hi);
    CHECK_EQ(hi, 12000);
    CHECK_EQ(lo, -12000);

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.plays, 1);
    CHECK_EQ(s.failures, 0);
    CHECK_EQ(s.frames_last, 4800);
    CHECK_EQ(s.ms_last, 100);
    CHECK_STR(s.last_error, "");

    /* The default tone is a sine at the default amplitude. */
    CHECK_EQ(audio_tone(440, 50), AUDIO_OK);
    CHECK_EQ(nat.calls, 2);
    extremes(given_pcm(), nat.frames, 2, &lo, &hi);
    CHECK_EQ(hi, (int)AUDIO_AMP_DEFAULT);
}

static void test_native_play_failure(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    nat.fail = 1;
    nat.why  = "status register still 0x00 after 40 ms";
    CHECK_EQ(audio_tone(440, 50), AUDIO_EIO);
    CHECK_CONTAINS(audio_last_error(), "status register still 0x00");
    CHECK_CONTAINS(audio_last_error(), "testsink");

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.plays, 0);
    CHECK_EQ(s.failures, 1);
    CHECK_CONTAINS(s.last_error, "could not play");

    /* A driver that refuses without a reason still gets a legible message. */
    nat.why = NULL;
    CHECK_EQ(audio_tone(440, 50), AUDIO_EIO);
    CHECK_CONTAINS(audio_last_error(), "no reason");

    nat.fail = 0;
    CHECK_EQ(audio_tone(440, 50), AUDIO_OK);
    audio_status(&s);
    CHECK_EQ(s.plays, 1);
    CHECK_EQ(s.failures, 2);
    CHECK_STR(s.last_error, "");         /* cleared by the success */
}

static void test_play_argument_bounds(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    /* Durations. */
    CHECK_EQ(audio_tone(440, 0), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "0 ms");
    CHECK_EQ(audio_tone(440, AUDIO_MAX_MS + 1), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "longer than one sound may be");
    CHECK_EQ(audio_tone(440, 0xFFFFFFFFu), AUDIO_EINVAL);
    /* A negative duration through the C API arrives as an enormous unsigned
     * one; it must be refused by the bound, not wrapped into a short tone. */
    CHECK_EQ(audio_tone(440, (uint32_t)-250), AUDIO_EINVAL);
    CHECK_EQ(audio_tone(440, AUDIO_MAX_MS), AUDIO_OK);

    /* Frequencies. The sink's own Nyquist limit appears in the message. */
    CHECK_EQ(audio_tone(19, 50), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "outside what this sink can play");
    CHECK_EQ(audio_tone(20000000, 50), AUDIO_EINVAL);
    CHECK_EQ(audio_tone(0xFFFFFFFFu, 50), AUDIO_EINVAL);
    CHECK_EQ(audio_tone(24000, 50), AUDIO_EINVAL);        /* exactly Nyquist */
    CHECK_CONTAINS(audio_last_error(), "24000");
    CHECK_EQ(audio_tone(20, 50), AUDIO_OK);
    CHECK_EQ(audio_tone(19999, 50), AUDIO_OK);

    /* Amplitude and waveform. */
    CHECK_EQ(audio_tone_ex(440, 50, AUDIO_AMP_MAX + 1, AUDIO_WAVE_SINE),
             AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "above full scale");
    CHECK_EQ(audio_tone_ex(440, 50, 8000, (audio_wave_t)42), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "not one this kernel synthesises");
    /* 256 narrows to 0 (= SINE) in audio_note_t's uint8_t field, so it must be
     * refused before the narrowing rather than quietly becoming a sine. */
    CHECK_EQ(audio_tone_ex(440, 50, 8000, (audio_wave_t)256), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "not one this kernel synthesises");

    /* On a slow sink the frequency ceiling really is lower, and the message
     * says which rate made it so. */
    const audio_format_t slow = { 8000, 1, 16 };
    CHECK_EQ(audio_register_native("slow", NULL, &slow, &nat_ops), AUDIO_OK);
    CHECK_EQ(audio_tone(5000, 50), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "8000");
    CHECK_EQ(audio_tone(3000, 50), AUDIO_OK);
}

static void test_sequence(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    audio_note_t notes[3] = {
        { 440, 50, 8000, AUDIO_WAVE_SQUARE },
        { 0,   50, 8000, AUDIO_WAVE_SQUARE },   /* a rest */
        { 660, 50, 8000, AUDIO_WAVE_SQUARE },
    };
    CHECK_EQ(audio_play_notes(notes, 3), AUDIO_OK);
    /* All three fit in one buffer, so it is one gapless play. */
    CHECK_EQ(nat.calls, 1);
    CHECK_EQ(nat.frames, 3 * 2400);

    /* The rest really is silence, and the notes really are not. */
    const int16_t *pcm = given_pcm();
    int lo, hi;
    extremes(pcm + 2400 * 2, 2400, 2, &lo, &hi);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 0);
    extremes(pcm, 2400, 2, &lo, &hi);
    CHECK_EQ(hi, 8000);

    /* amplitude 0 in a note literal means "the default", so a plain
     * {hz, ms} note is audible rather than accidentally muted. */
    audio_note_t plain = { 440, 20, 0, AUDIO_WAVE_SINE };
    nat_clear();
    CHECK_EQ(audio_play_notes(&plain, 1), AUDIO_OK);
    extremes(given_pcm(), nat.frames, 2, &lo, &hi);
    CHECK_EQ(hi, (int)AUDIO_AMP_DEFAULT);

    CHECK_EQ(audio_play_notes(NULL, 1), AUDIO_EINVAL);
    CHECK_EQ(audio_play_notes(notes, 0), AUDIO_EINVAL);
    CHECK_EQ(audio_play_notes(notes, -5), AUDIO_EINVAL);
    CHECK_EQ(audio_play_notes(notes, AUDIO_SEQ_MAX + 1), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "more than one sequence may hold");
}

static void test_sequence_validated_before_it_sounds(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    audio_note_t notes[4] = {
        { 440, 50, 8000, AUDIO_WAVE_SINE },
        { 494, 50, 8000, AUDIO_WAVE_SINE },
        { 523, 50, 8000, AUDIO_WAVE_SINE },
        { 440, 9999, 8000, AUDIO_WAVE_SINE },   /* the bad one, last */
    };
    CHECK_EQ(audio_play_notes(notes, 4), AUDIO_EINVAL);
    /* NOTHING was played: three notes followed by an error would leave the
     * caller unable to tell what was heard from what was refused. */
    CHECK_EQ(nat.calls, 0);
    CHECK_CONTAINS(audio_last_error(), "note 4 of 4");
}

static void test_sequence_spans_the_buffer(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    /* 49152 frames fit, which is 1024 ms. Six 200 ms notes are 1200 ms, so this
     * must become two plays rather than one truncated one. */
    audio_note_t notes[6];
    for (int i = 0; i < 6; i++) {
        notes[i].hz = 400 + 50 * (uint32_t)i;
        notes[i].ms = 200;
        notes[i].amplitude = 8000;
        notes[i].wave = AUDIO_WAVE_SINE;
    }
    CHECK_EQ(audio_play_notes(notes, 6), AUDIO_OK);
    CHECK_EQ(nat.calls, 2);
    /* First flush: as many whole notes as fit (5 * 9600 = 48000 <= 49152).
     * Second: the remainder. Nothing is dropped. */
    CHECK_EQ(nat.frames, 9600);
    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.plays, 2);
}

static void test_play_pcm(void) {
    audio_reset();
    register_native(&FMT_48_MONO);

    static int16_t src[1000];
    for (int i = 0; i < 1000; i++) src[i] = (int16_t)(i * 7);

    CHECK_EQ(audio_play_pcm(src, 1000), AUDIO_OK);
    CHECK_EQ(nat.frames, 1000);
    CHECK_EQ(nat.bytes, 2000);
    /* Copied, not referenced: the sink was given the kernel's buffer, and the
     * caller's array is nowhere near a device. */
    CHECK(nat.phys != (uint64_t)(uintptr_t)src);
    CHECK_EQ(memcmp(given_pcm(), src, sizeof src), 0);

    CHECK_EQ(audio_play_pcm(NULL, 100), AUDIO_EINVAL);
    CHECK_EQ(audio_play_pcm(src, 0), AUDIO_EINVAL);
    CHECK_EQ(audio_play_pcm(src, cap_frames(&FMT_48_MONO) + 1), AUDIO_ENOSPC);
    CHECK_CONTAINS(audio_last_error(), "more than the buffer holds");
    /* The buffer holds 98304 mono frames, which is 2048 ms — longer than one
     * sound may be, so the duration bound catches it even though it fits. */
    CHECK_EQ(audio_play_pcm(src, cap_frames(&FMT_48_MONO)), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "at most");
}

static void test_stop(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);
    CHECK_EQ(audio_stop(), AUDIO_OK);
    CHECK_EQ(nat.stop_calls, 1);

    nat.stop_fail = 1;
    CHECK_EQ(audio_stop(), AUDIO_EIO);

    /* A sink with no stop hook says so instead of pretending. */
    audio_ops_t nostop = { NULL, nat_play, NULL };
    audio_reset();
    CHECK_EQ(audio_register_native("nostop", NULL, &FMT_48_STEREO, &nostop),
             AUDIO_OK);
    CHECK_EQ(audio_stop(), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "no stop path");
}

/* ====================================================================== */
/* 5. the VM play path — a model-authored driver, end to end              */
/* ====================================================================== */

static void test_vm_register_rejects(void) {
    audio_reset();
    if (assemble(PLAY_SRC) != 0) return;

    audio_vm_spec_t sp;

    /* No program at all. */
    vm_spec_init(&sp);
    sp.program = NULL;
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "no play program");
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, NULL), AUDIO_EINVAL);

    /* An empty program: nothing would run when a sound was wanted. */
    static dvm_program_t empty;
    memset(&empty, 0, sizeof empty);
    vm_spec_init(&sp);
    sp.program = &empty;
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "is empty");

    /* A corrupt image is caught by the VM's own validator at REGISTRATION,
     * while the model that wrote it can still be told. */
    static dvm_program_t bent;
    memcpy(&bent, &g_test_prog, sizeof bent);
    bent.insn[0].op = 250;                /* not an opcode */
    vm_spec_init(&sp);
    sp.program = &bent;
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "not a valid image");

    /* A sandbox that reaches nothing. */
    vm_spec_init(&sp);
    dvm_policy_init(&sp.policy);          /* deny-all, no windows opened */
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "no way to reach the device");

    /* A sandbox the VM itself refuses (COM1 is on its deny list). */
    vm_spec_init(&sp);
    dvm_policy_init(&sp.policy);
    dvm_policy_allow_ports(&sp.policy, 0x3F8, 0x3FF);
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "not permitted");

    /* No DMA grant. The samples live in the VM's arena, so a sink without a
     * grant could not let its program write alongside them and the kernel could
     * not arm the guard bands that notice a runaway device. */
    vm_spec_init(&sp);
    dvm_policy_init(&sp.policy);
    dvm_policy_allow_ports(&sp.policy, DEVBASE, DEVBASE + 0x3F);
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "no DMA grant");

    /* A grant that does not start at the arena base, so r7 would not point at
     * the samples. */
    uint64_t dbase = 0, dsize = 0;
    dvm_dma_region(&dbase, &dsize);
    vm_spec_init(&sp);
    dvm_policy_init(&sp.policy);
    dvm_policy_allow_ports(&sp.policy, DEVBASE, DEVBASE + 0x3F);
    CHECK_EQ(dvm_policy_allow_dma(&sp.policy, dbase + 4096, dsize - 4096), 0);
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "must start at the arena base");

    /* A grant that starts right but is too small to hold the PCM. */
    vm_spec_init(&sp);
    dvm_policy_init(&sp.policy);
    dvm_policy_allow_ports(&sp.policy, DEVBASE, DEVBASE + 0x3F);
    CHECK_EQ(dvm_policy_allow_dma(&sp.policy, dbase, 4096), 0);
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "cover at least the samples");

    /* No I/O backend: on a host build dvm_io_hardware() is NULL, so a spec that
     * does not bring its own has no way to reach anything. */
    vm_spec_init(&sp);
    sp.io = NULL;
    CHECK_EQ(audio_register_vm("vm", NULL, &FMT_48_STEREO, &sp), AUDIO_EINVAL);
    CHECK_CONTAINS(audio_last_error(), "no hardware I/O backend");

    CHECK_EQ(audio_available(), 0);
}

static void test_vm_plays_a_tone(void) {
    audio_reset();
    if (assemble(PLAY_SRC) != 0) return;

    device_t *d = device_create("pci00:05.0", DEV_CLASS_AUDIO);
    CHECK(d != NULL);
    if (!d) return;
    device_add_resource(d, RES_IOPORT, DEVBASE, 0x40);
    device_register(d);

    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("modelsnd", d, &FMT_48_STEREO, &sp), AUDIO_OK);

    /* The device tree names the model's driver as the owner. */
    CHECK(d->driver != NULL);
    if (d->driver) CHECK_STR(d->driver->name, "modelsnd");
    CHECK_EQ(d->state, DEV_STATE_ACTIVE);

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.kind, AUDIO_SINK_VM);
    CHECK_EQ(s.vm_insns, g_test_prog.ninsn);
    CHECK_EQ(s.vm_ran, 0);
    CHECK_STR(s.device, "pci00:05.0");

    /* The spec may be discarded the moment registration returns: the service
     * copied the image. Scribble on the caller's copy to prove it. */
    memset(&g_test_prog, 0xEE, sizeof g_test_prog);

    dev_reset(3);                          /* busy for three status reads */
    CHECK_EQ(audio_tone_ex(1000, 100, 12000, AUDIO_WAVE_SQUARE), AUDIO_OK);

    /* THE CONTRACT, CHECKED AGAINST WHAT THE PROGRAM ACTUALLY DID.
     * r7 became the device's buffer address and r8 its byte count. */
    CHECK_EQ(dev.addr, (uint32_t)audio_buffer_phys());
    CHECK_EQ(dev.len, 4800 * 4);
    CHECK_EQ(dev.started, 1);
    CHECK_EQ(dev.denied, 0);               /* it touched only real registers */
    CHECK_EQ(dev.status_reads, 4);         /* three busy, then done */
    CHECK_EQ(dev.delay_us, 3000);

    /* ...and the memory it pointed the device at holds the tone. This is the
     * whole point of the subsystem: a driver the kernel knows nothing about was
     * handed real PCM and told where it was.
     *
     * The address is followed through audio_buffer_phys() rather than through
     * dev.addr, because dev.addr is what a 32-bit `out32` carried. On the kernel
     * those are the same number — the buffer is in the identity-mapped low 4 GiB,
     * which is the invariant audio_dma_reachable() enforces at registration — but
     * in a host process .bss sits above 4 GiB and the low half is not a pointer.
     * The equality checked above is the part of the contract that matters; this
     * only needs to reach the same bytes the device would have. */
    const int16_t *pcm = (const int16_t *)(uintptr_t)audio_buffer_phys();
    CHECK_EQ(square_half_period(pcm, 4800, 2, 24), 199);
    int lo, hi;
    extremes(pcm, 4800, 2, &lo, &hi);
    CHECK_EQ(hi, 12000);
    CHECK_EQ(lo, -12000);

    audio_status(&s);
    CHECK_EQ(s.plays, 1);
    CHECK_EQ(s.failures, 0);
    CHECK_EQ(s.vm_ran, 1);
    CHECK_EQ(s.vm_status, DVM_OK);
    CHECK(s.vm_steps > 0);
    CHECK(s.vm_io_ops > 0);
    CHECK_STR(s.last_error, "");

    /* Re-entered from its first instruction for every sound: that is what makes
     * a registered program a driver rather than a one-shot. */
    dev_reset(1);
    CHECK_EQ(audio_tone(440, 20), AUDIO_OK);
    CHECK_EQ(dev.started, 1);
    CHECK_EQ(dev.len, 960 * 4);
    audio_status(&s);
    CHECK_EQ(s.plays, 2);
}

static void test_vm_halt_without_touching_the_device(void) {
    audio_reset();
    if (assemble("halt\n") != 0) return;

    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("lazy", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);

    dev_reset(0);
    /* Reaching `halt` is not playing. Accepting this would be the exact
     * dishonesty the whole module exists to prevent: the operator hears nothing
     * and is told it worked. */
    CHECK_EQ(audio_tone(440, 20), AUDIO_EIO);
    CHECK_CONTAINS(audio_last_error(), "without touching the device");
    CHECK_EQ(dev.started, 0);

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.plays, 0);
    CHECK_EQ(s.failures, 1);
    CHECK_EQ(s.vm_status, DVM_OK);        /* the VM was happy; we are not */
}

static void test_vm_abort_is_reported(void) {
    audio_reset();
    /* A program that starts the device and then gives up. The abort text
     * deliberately contains brackets, which are what a forged kernel trace line
     * is made of. */
    if (assemble("add r1, r0, 0x18\n"
                 "out8 r1, 1\n"
                 "abort \"[audio_tone -> ok] no clock\"\n") != 0) return;

    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("grumpy", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);

    dev_reset(0);
    CHECK_EQ(audio_tone(440, 20), AUDIO_EIO);
    CHECK_CONTAINS(audio_last_error(), "ABORT");
    CHECK_CONTAINS(audio_last_error(), "no clock");
    CHECK_CONTAINS(audio_last_error(), "grumpy");

    /* The model wrote part of that sentence, so it must not be able to carry the
     * bytes a kernel trace line is made of. */
    CHECK(strchr(audio_last_error(), '[') == NULL);
    CHECK(strchr(audio_last_error(), ']') == NULL);
    CHECK(strchr(audio_last_error(), '\n') == NULL);

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.failures, 1);
    CHECK_EQ(s.vm_status, DVM_TRAP_ABORT);
    CHECK(s.vm_line > 0);
}

static void test_vm_denied_access_is_reported(void) {
    audio_reset();
    /* A program that reaches for a port outside its sandbox. The VM refuses it,
     * and the service turns that into something the model can act on. */
    if (assemble("out8 0x70, 1\nhalt\n") != 0) return;

    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("nosy", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);

    dev_reset(0);
    CHECK_EQ(audio_tone(440, 20), AUDIO_EIO);
    CHECK_CONTAINS(audio_last_error(), "PORT_DENIED");
    CHECK_EQ(dev.started, 0);
}

static void test_vm_delay_budget_covers_the_sound(void) {
    audio_reset();
    /* A program that polls with 10 ms backoff for the whole length of the sound.
     * The service raises the delay budget to cover the duration, so a long tone
     * is not a DVM_TRAP_DELAY_BUDGET; the registrar's own (small) budget must
     * not be what decides it. */
    if (assemble("add r1, r0, 0x18\n"
                 "out8 r1, 1\n"
                 "add r1, r0, 0x1c\n"
                 "mov r2, 200\n"
                 "poll:\n"
                 "  in8 r3, r1\n"
                 "  test r3, 1\n"
                 "  beq done\n"
                 "  delay 10000\n"
                 "  sub r2, 1\n"
                 "  cmp r2, 0\n"
                 "  bne poll\n"
                 "  abort \"timed out\"\n"
                 "done:\n"
                 "halt\n") != 0) return;

    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    sp.policy.max_delay_us = 20000;        /* 20 ms: nowhere near enough */
    CHECK_EQ(audio_register_vm("slowpoll", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);

    dev_reset(50);                         /* busy for 500 ms of polling */
    CHECK_EQ(audio_tone(440, 500), AUDIO_OK);
    CHECK_EQ(dev.delay_us, 500000);

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.plays, 1);

    /* And the raise is a raise only: a registrar that asked for MORE keeps it,
     * because a program that needs a long bring-up poll on a slow codec knows
     * something the service does not. */
    vm_spec_init(&sp);
    sp.policy.max_delay_us = 1500000;
    CHECK_EQ(audio_register_vm("patient", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);
    dev_reset(120);                        /* 1.2 s of polling, over the raise */
    CHECK_EQ(audio_tone(440, 100), AUDIO_OK);
    CHECK_EQ(dev.delay_us, 1200000);
}

/* THE CAPABILITY THE ARENA SPLIT EXISTS FOR.
 *
 * Real audio hardware is usually told where to work by being handed a descriptor
 * list in memory, not a bare address — and the LAYOUT of that list is device
 * knowledge, so the kernel cannot build it. This proves the alternative works: a
 * play program builds its own structure in the space after the PCM, hands the
 * device the address of THAT, and finds it still there on the next sound. */
static void test_vm_builds_its_own_descriptor(void) {
    audio_reset();

    /* r12 is the driver's own space: a fixed address after the samples. Build a
     * two-word "descriptor" there (buffer address, byte count) and give the
     * device the descriptor's address instead of the buffer's. */
    if (assemble("mov r4, r12\n"             /* r4 = the driver's scratch */
                 "st32 [r4+0], r7\n"         /* descriptor[0] = pcm base  */
                 "st32 [r4+4], r9\n"         /* descriptor[1] = pcm bytes */
                 "ld32 r5, [r4+4]\n"         /* read one back             */
                 "cmp r5, r9\n"              /* (r9 fits in 32 bits; r7's  */
                 "bne bad\n"                 /*  low half need not equal it */
                 "add r1, r0, 0x10\n"
                 "out32 r1, r4\n"            /* hand over the DESCRIPTOR  */
                 "add r1, r0, 0x14\n"
                 "out32 r1, r9\n"
                 "add r1, r0, 0x18\n"
                 "out8 r1, 1\n"
                 "halt\n"
                 "bad:\n"
                 "abort \"the descriptor did not read back\"\n") != 0) return;

    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("bdlsnd", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);

    dev_reset(0);
    CHECK_EQ(audio_tone_ex(1000, 100, 12000, AUDIO_WAVE_SQUARE), AUDIO_OK);

    uint64_t dbase = 0, dsize = 0;
    dvm_dma_region(&dbase, &dsize);

    /* The device was pointed at the descriptor, which sits after the samples. */
    CHECK_EQ(dev.addr, (uint32_t)(dbase + AUDIO_PCM_BYTES));
    CHECK_EQ(dev.started, 1);

    /* And the descriptor really holds the buffer the kernel filled. */
    const uint32_t *desc = (const uint32_t *)(uintptr_t)(dbase + AUDIO_PCM_BYTES);
    CHECK_EQ(desc[0], (uint32_t)dbase);
    CHECK_EQ(desc[1], 4800u * 4u);

    /* The kernel refills the PCM for the next sound but never touches the tail,
     * so a structure built there survives. Poison a byte the kernel must not
     * write, play again, and check it is still poisoned. */
    volatile uint8_t *tail =
        (volatile uint8_t *)(uintptr_t)(dbase + AUDIO_PCM_BYTES + 64);
    *tail = 0xA7;
    dev_reset(0);
    CHECK_EQ(audio_tone_ex(500, 50, 9000, AUDIO_WAVE_SQUARE), AUDIO_OK);
    CHECK_EQ(*tail, 0xA7);
    CHECK_EQ(desc[1], 2400u * 4u);          /* rewritten by the program    */

    /* No DMA accesses were charged to the device-access budget, and the guard
     * bands around the arena are intact — the program stayed inside its grant. */
    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.plays, 2);
    CHECK_EQ(s.failures, 0);
    CHECK_STR(s.last_error, "");
}

static void test_vm_replaced_by_native(void) {
    audio_reset();
    if (assemble(PLAY_SRC) != 0) return;

    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("vmsink", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);
    dev_reset(1);
    CHECK_EQ(audio_tone(440, 20), AUDIO_OK);

    /* A native driver taking over must really take over: the VM program must not
     * run again, and the status must stop claiming a play program. */
    nat_clear();
    CHECK_EQ(audio_register_native("cdriver", NULL, &FMT_48_STEREO, &nat_ops),
             AUDIO_OK);
    dev_reset(1);
    CHECK_EQ(audio_tone(440, 20), AUDIO_OK);
    CHECK_EQ(nat.calls, 1);
    CHECK_EQ(dev.started, 0);

    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.kind, AUDIO_SINK_NATIVE);
    CHECK_EQ(s.vm_insns, 0);
    CHECK_EQ(s.vm_ran, 0);
}

/* ====================================================================== */
/* 6. the tools                                                           */
/* ====================================================================== */

static void test_tool_registration(void) {
    CHECK(tool_find("audio_status") != NULL);
    CHECK(tool_find("audio_tone") != NULL);
    CHECK(tool_find("audio_melody") != NULL);
    CHECK_TOOL_FLAGS("audio_status", ~0u, 0);
    CHECK_TOOL_FLAGS("audio_tone", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("audio_melody", TOOL_MUTATES, TOOL_MUTATES);

    /* The description NAMES the contract; audio_status prints it (asserted in
     * test_tool_status_reports_no_sink). It is deliberately not interpolated
     * here: the registry is sent on every request and has a hard byte budget, so
     * a kilobyte of driver-authoring instructions does not belong in it. */
    const tool_t *t = tool_find("audio_status");
    if (t) {
        CHECK_CONTAINS(t->description, "PLAY CONTRACT");
        CHECK(strstr(t->description, "DONE MEANS FINISHED") == NULL);
        /* ...and it names the tool that consumes the contract, because a model
         * that reads the contract and cannot find the install step is exactly the
         * live failure this family already had once. */
        CHECK_CONTAINS(t->description, "driver_install");
    }

    /* No description in this family may be so long that it eats the shared
     * registry budget. 700 bytes is generous for a verb and stops the contract
     * creeping back in. */
    static const char *const mine[] = { "audio_status", "audio_tone",
                                        "audio_melody" };
    for (size_t i = 0; i < 3; i++) {
        const tool_t *x = tool_find(mine[i]);
        if (!x) continue;
        CHECK(strlen(x->description) < 700);
    }

    /* THIS FAMILY'S SHARE OF A SHARED BUDGET. Only these three tools are linked
     * into this suite, so tools_build_schema() here is exactly what this file
     * costs the registry that chat.h's CHAT_TOOLS_BYTES bounds. Printed so the
     * number is visible, and bounded so it cannot quietly grow into the headroom
     * every other tool family also needs. */
    static char schema[16384];
    size_t sn = tools_build_schema(schema, sizeof schema);
    CHECK_EQ(tool_count(), 3);
    printf("    (audio family: %lu bytes of schema for %d tools)\n",
           (unsigned long)sn, tool_count());
    /* ~940 bytes per tool, against a registry-wide average of ~815. Room to
     * spare on any one of them, but not room for the play contract. */
    CHECK(sn > 0 && sn < 2900);

    /* Every schema must be valid JSON, or tool_find() would be returning a
     * descriptor the API will reject. */
    for (int i = 0; i < tool_count(); i++) {
        const tool_t *x = tool_at(i);
        json_value_t v;
        if (!x) continue;
        CHECK_EQ(json_parse_str(x->input_schema, &v), JSON_OK);
    }
}

static void test_tool_status_reports_no_sink(void) {
    audio_reset();
    kcap_reset();

    int err = -1;
    const char *out = call_tool("audio_status", "{}", &err);
    /* Asking what the machine can do and being told "nothing" is a correct
     * answer to the question asked, so it is not an error result. */
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(out, "NO OUTPUT DRIVER IS REGISTERED");
    CHECK_CONTAINS(out, "volume problem");

    /* THE HONEST FAILURE MUST TEACH A FIX THAT EXISTS, and this assertion has now
     * been wrong in BOTH directions, which is why it is worth the paragraph.
     *
     * It first required the whole PLAY CONTRACT here. A live model run showed that
     * was the wrong lesson twice over: nothing in the kernel called
     * audio_register_vm(), so "register a play program as the sink" was an
     * instruction for a button that was not there. The model obeyed it, hunted for
     * the missing tool for most of a turn, and never made a sound. The assertion
     * was then changed to require the text to SAY no install tool exists.
     *
     * driver_install exists now (tools/dvm_tools.c), so that sentence became the
     * false one — and a test demanding it would have kept it false. What is
     * actually being pinned, and all that was ever being pinned, is that this text
     * names a path a model can walk TODAY, in order, with no step missing. So it
     * checks the three tools of that path and the one fact the live model got
     * wrong (that the kernel, not the program, writes the samples), and it
     * explicitly forbids the two obsolete claims coming back. */
    CHECK_CONTAINS(out, "driver_targets");
    CHECK_CONTAINS(out, "driver_run");
    CHECK_CONTAINS(out, "driver_install");
    CHECK_CONTAINS(out, "PLAY CONTRACT");
    CHECK_CONTAINS(out, "The kernel fills the buffer");
    CHECK(strstr(out, "NO TOOL IN THIS KERNEL CAN REGISTER A SINK") == NULL);
    CHECK(strstr(out, "NOTHING FILLS THAT BUFFER FOR YOU") == NULL);

    /* IT MUST FIT THE SIZE THE MODEL ACTUALLY RECEIVES. t_out is
     * TOOL_RESULT_MAX (4096), but chat.c hands a tool CHAT_TOOL_RESULT_CAP
     * (1024) — which is why this suite was green while the real answer arrived
     * cut off mid-word. Assert against the number that governs in the machine,
     * or this whole test is measuring a buffer no model ever sees. */
    CHECK(strlen(out) < CHAT_TOOL_RESULT_CAP);
    /* ...and it must not end mid-sentence, which is what truncation looks like. */
    CHECK_EQ(out[strlen(out) - 1], '\n');

    /* The contract is still REACHABLE, just on request and paged, because 1400
     * bytes cannot be delivered in a 1024-byte result. Page 1 announces the
     * count and where the rest is; page 2 carries the register list. */
    kcap_reset();
    const char *c1 = call_tool("audio_status", "{\"contract\":true}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(c1, "play contract, part 1 of ");
    CHECK_CONTAINS(c1, "part=2 for the rest");
    CHECK_CONTAINS(c1, "PLAY CONTRACT");
    CHECK(strlen(c1) < CHAT_TOOL_RESULT_CAP);

    /* "part" alone implies the contract: making a model repeat contract=true to
     * turn a page would spend a round on a formality. */
    const char *c2 = call_tool("audio_status", "{\"part\":2}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(c2, "play contract, part 2 of ");
    CHECK(strlen(c2) < CHAT_TOOL_RESULT_CAP);

    /* Every byte of the contract has to appear on SOME page, or paging has
     * quietly become a different kind of truncation. Walk them all and compare
     * the reassembly against the macro itself. */
    static char joined[4096];
    size_t jn = 0;
    for (int p = 1; p <= 8; p++) {
        char req[32];
        snprintf(req, sizeof req, "{\"part\":%d}", p);
        const char *pg = call_tool("audio_status", req, &err);
        CHECK_EQ(err, 0);
        if (strstr(pg, "does not exist")) break;
        /* Skip past the "play contract, part N of M...:\n" head. Anchored on the
         * marker, not on the first ':', because the no-sink line above it has
         * colons of its own. */
        const char *body = strstr(pg, "play contract, part ");
        if (!body) continue;
        body = strchr(body, '\n');
        if (!body) continue;
        body++;
        size_t bl = strlen(body);
        /* Remove exactly ONE trailing newline: the one put_contract() appends.
         * Stripping greedily would also eat the contract's own line break where a
         * page happens to end on one, and the reassembly would not compare equal
         * for a reason that has nothing to do with paging. */
        if (bl && body[bl - 1] == '\n') bl--;
        if (jn + bl < sizeof joined) { memcpy(joined + jn, body, bl); jn += bl; }
    }
    joined[jn] = '\0';
    CHECK_EQ(strcmp(joined, AUDIO_VM_CONTRACT), 0);

    /* An absurd page says so rather than printing nothing at all. */
    const char *c9 = call_tool("audio_status", "{\"part\":64}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(c9, "does not exist");
    /* And a page number outside the schema's range is a refusal, not a clamp. */
    int e2 = -1;
    call_tool("audio_status", "{\"part\":0}", &e2);
    CHECK_EQ(e2, 1);

    /* The kernel emitted its own line about it. */
    CHECK_CONTAINS(kcap_text(), "[audio_status sink=-");
}

static void test_tool_status_reports_a_sink(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);
    CHECK_EQ(audio_tone(440, 50), AUDIO_OK);
    kcap_reset();

    int err = -1;
    const char *out = call_tool("audio_status", "{}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(out, "sink=\"testsink\"");
    CHECK_CONTAINS(out, "kind=native");
    CHECK_CONTAINS(out, "48000 Hz, 2 channel(s), 16-bit");
    CHECK_CONTAINS(out, "longest single sound 1000 ms");
    CHECK_CONTAINS(out, "1 ok, 0 failed");
    CHECK_CONTAINS(out, "2400 frames (50 ms)");
    /* Not printed unless asked, so a status call is cheap in tokens. */
    CHECK(strstr(out, "PLAY CONTRACT") == NULL);
    CHECK_CONTAINS(kcap_text(), "[audio_status sink=testsink kind=native -> 1]");

    out = call_tool("audio_status", "{\"contract\":true}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(out, "PLAY CONTRACT");

    out = call_tool("audio_status", "{\"contract\":\"yes\"}", &err);
    CHECK_EQ(err, 1);
    CHECK_CONTAINS(out, "must be a boolean");
}

static void test_tool_tone_no_sink(void) {
    audio_reset();
    kcap_reset();

    int err = -1;
    const char *out = call_tool("audio_tone", "{\"hz\":440}", &err);
    CHECK_EQ(err, 1);
    CHECK_CONTAINS(out, "no audio output driver is registered");
    CHECK_CONTAINS(out, "audio_status");
    CHECK_CONTAINS(kcap_text(), "[audio_tone no-sink -> ENOENT]");

    /* And with nonsense arguments too, the answer is still "no driver". */
    out = call_tool("audio_tone", "{\"hz\":0,\"ms\":-9}", &err);
    CHECK_EQ(err, 1);
    CHECK_CONTAINS(out, "no audio output driver is registered");

    out = call_tool("audio_melody", "{\"notes\":[440]}", &err);
    CHECK_EQ(err, 1);
    CHECK_CONTAINS(out, "no audio output driver is registered");
}

static void test_tool_tone_plays(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);
    kcap_reset();

    int err = -1;
    const char *out = call_tool("audio_tone",
                                "{\"hz\":440,\"ms\":100,\"wave\":\"square\","
                                "\"amplitude\":9000}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(out, "played 440 Hz square for 100 ms");
    CHECK_CONTAINS(out, "4800 frames");
    CHECK_CONTAINS(out, "testsink");
    CHECK_EQ(nat.calls, 1);
    CHECK_EQ(nat.frames, 4800);
    int lo, hi;
    extremes(given_pcm(), nat.frames, 2, &lo, &hi);
    CHECK_EQ(hi, 9000);
    CHECK_CONTAINS(kcap_text(),
                   "[audio_tone hz=440 ms=100 wave=square amp=9000 sink=testsink "
                   "-> 4800]");

    /* Defaults: sine, 250 ms, amplitude 8000. */
    nat_clear();
    out = call_tool("audio_tone", "{\"hz\":880}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(out, "played 880 Hz sine for 250 ms");
    CHECK_EQ(nat.frames, 12000);
    extremes(given_pcm(), nat.frames, 2, &lo, &hi);
    CHECK_EQ(hi, (int)AUDIO_AMP_DEFAULT);
}

static void test_tool_tone_adversarial(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    static const char *const bad[] = {
        "{}",                                    /* hz is required          */
        "{\"hz\":0}",                            /* 0 Hz is not a tone      */
        "{\"hz\":19}",
        "{\"hz\":20001}",
        "{\"hz\":20000000000}",                  /* 20 GHz                  */
        "{\"hz\":99999999999999999999999}",      /* past int64              */
        "{\"hz\":\"440\"}",                      /* a string                */
        "{\"hz\":true}",
        "{\"hz\":[440]}",
        "{\"hz\":440,\"ms\":0}",
        "{\"hz\":440,\"ms\":-250}",              /* negative duration       */
        "{\"hz\":440,\"ms\":1001}",
        "{\"hz\":440,\"ms\":4294967296}",        /* enormous duration       */
        "{\"hz\":440,\"amplitude\":32768}",
        "{\"hz\":440,\"amplitude\":-1}",
        "{\"hz\":440,\"wave\":\"bagpipes\"}",
        "{\"hz\":440,\"wave\":42}",
        "{\"hz\":440,\"wave\":\"averylongwaveformname\"}",
        "[]",                                    /* not an object           */
        "{\"hz\":440",                           /* truncated JSON          */
        "not json at all",
    };

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int err = -1;
        const char *out = call_tool("audio_tone", bad[i], &err);
        if (err != 1) {
            th_checks++; th_fails++;
            printf("    FAIL audio_tone accepted %s\n", bad[i]);
        } else {
            CHECK_CONTAINS(out, "error:");
        }
    }
    /* Not one of them made a sound, and not one of them faulted. */
    CHECK_EQ(nat.calls, 0);
    audio_status_t s;
    audio_status(&s);
    CHECK_EQ(s.plays, 0);

    /* A fractional frequency is TRUNCATED, not refused: json_int() in the shared,
     * hardened reader truncates toward zero by design (net/json.c), and this file
     * must not grow a second number parser to disagree with it. Pinned here so the
     * behaviour is a decision rather than an accident. */
    int e2 = -1;
    const char *o2 = call_tool("audio_tone", "{\"hz\":440.9,\"ms\":10}", &e2);
    CHECK_EQ(e2, 0);
    CHECK_CONTAINS(o2, "played 440 Hz");
    nat_clear();

    /* An embedded NUL in a string argument must not truncate the parse into
     * something that looks valid. */
    char withnul[] = "{\"hz\":440,\"wave\":\"si\0ne\"}";
    tool_call_t   c = { "id", "audio_tone", withnul, sizeof withnul - 1 };
    tool_result_t r;
    tool_result_init(&r, t_out, sizeof t_out);
    tool_dispatch(&c, &r);
    CHECK_EQ(r.is_error, 1);
    CHECK_EQ(nat.calls, 0);
}

static void test_tool_melody(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);
    kcap_reset();

    int err = -1;
    /* The terse form: a list of frequencies plus one default duration. */
    const char *out = call_tool("audio_melody",
                                "{\"notes\":[440,494,523,0,659],\"ms\":40}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(out, "played 5 note(s)");
    CHECK_CONTAINS(out, "200 ms");
    CHECK_EQ(nat.calls, 1);
    CHECK_EQ(nat.frames, 5 * 1920);
    CHECK_CONTAINS(kcap_text(), "[audio_melody notes=5 ms=200 sink=testsink -> 5]");

    /* The explicit form, mixed with the terse one, with per-note overrides. */
    nat_clear();
    out = call_tool("audio_melody",
                    "{\"notes\":[{\"hz\":440,\"ms\":30,\"wave\":\"square\","
                    "\"amplitude\":20000},220,{\"hz\":0,\"ms\":30}],"
                    "\"ms\":30}", &err);
    CHECK_EQ(err, 0);
    CHECK_EQ(nat.calls, 1);
    CHECK_EQ(nat.frames, 3 * 1440);
    int lo, hi;
    extremes(given_pcm(), 1440, 2, &lo, &hi);
    CHECK_EQ(hi, 20000);                 /* the per-note amplitude won */
    extremes(given_pcm() + 1440 * 2, 1440, 2, &lo, &hi);
    /* 220 Hz does not divide 48000, so no frame lands exactly on a peak and the
     * maximum is one LSB short. Asserting equality here would be asserting a
     * coincidence about 220 rather than anything about the default amplitude. */
    CHECK(hi > (int)AUDIO_AMP_DEFAULT - 4 && hi <= (int)AUDIO_AMP_DEFAULT);
    extremes(given_pcm() + 2880 * 2, 1440, 2, &lo, &hi);
    CHECK_EQ(hi, 0);                     /* the rest really is silent    */

    static const char *const bad[] = {
        "{}",                                     /* notes required        */
        "{\"notes\":[]}",                         /* nothing to play       */
        "{\"notes\":440}",                        /* not an array          */
        "{\"notes\":\"440 494\"}",
        "{\"notes\":{\"hz\":440}}",
        "{\"notes\":[440,\"494\"]}",              /* a string in the list  */
        "{\"notes\":[440,{\"ms\":50}]}",          /* a note with no hz     */
        "{\"notes\":[440,{\"hz\":30000}]}",
        "{\"notes\":[440,{\"hz\":440,\"ms\":0}]}",
        "{\"notes\":[440,{\"hz\":440,\"ms\":-1}]}",
        "{\"notes\":[440],\"ms\":0}",
        "{\"notes\":[440],\"ms\":99999}",
        "{\"notes\":[440],\"amplitude\":40000}",
        "{\"notes\":[440],\"wave\":\"ocarina\"}",
        "{\"notes\":[440,[440]]}",
        "{\"notes\":[440,null]}",
        /* 33 notes: one past the limit. */
        ("{\"notes\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,"
         "23,24,25,26,27,28,29,30,31,32,33]}"),
    };
    nat_clear();
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int e = -1;
        const char *o = call_tool("audio_melody", bad[i], &e);
        if (e != 1) {
            th_checks++; th_fails++;
            printf("    FAIL audio_melody accepted %s\n", bad[i]);
        } else {
            CHECK_CONTAINS(o, "error:");
        }
    }
    CHECK_EQ(nat.calls, 0);              /* none of them played anything */

    /* Exactly at the limit is fine. */
    out = call_tool("audio_melody",
                    "{\"notes\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,"
                    "19,20,21,22,23,24,25,26,27,28,29,30,31,32],\"ms\":2}", &err);
    /* Those "frequencies" are all below 20 Hz, so the SINK refuses them — with
     * its own message, which is the layering working. */
    CHECK_EQ(err, 1);
    CHECK_CONTAINS(out, "outside what this sink can play");

    out = call_tool("audio_melody",
                    "{\"notes\":[440,441,442,443,444,445,446,447,448,449,450,"
                    "451,452,453,454,455,456,457,458,459,460,461,462,463,464,"
                    "465,466,467,468,469,470,471],\"ms\":2}", &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(out, "played 32 note(s)");
}

static void test_tool_melody_spans_buffers(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);

    int err = -1;
    const char *out = call_tool("audio_melody",
                                "{\"notes\":[440,494,523,587,659,698],"
                                "\"ms\":200}", &err);
    CHECK_EQ(err, 0);
    CHECK_EQ(nat.calls, 2);
    /* The seam is audible, so the answer says so rather than letting the model
     * conclude the driver is broken. */
    CHECK_CONTAINS(out, "did not fit in one buffer");
    CHECK_CONTAINS(out, "2 pieces");
}

static void test_tool_reports_a_sink_failure(void) {
    audio_reset();
    register_native(&FMT_48_STEREO);
    nat.fail = 1;
    nat.why  = "the DMA engine never left position 0";
    kcap_reset();

    int err = -1;
    const char *out = call_tool("audio_tone", "{\"hz\":440,\"ms\":50}", &err);
    CHECK_EQ(err, 1);
    CHECK_CONTAINS(out, "the DMA engine never left position 0");
    CHECK_CONTAINS(kcap_text(), "[audio_tone hz=440 ms=50 wave=sine sink=testsink");
    CHECK_CONTAINS(kcap_text(), "-> EINVAL]");
}

/* ---------------------------------------------------------------------
 * THE GOAL STATE, AND THE HOLE THAT WAS IN IT
 *
 * Once the model has installed its own audio driver, the machine's audio sink IS
 * a driver program. `sys audio.tone` from inside ANOTHER driver program then
 * meant running two programs at once, and the two share one scratch arena and
 * one pair of DMA guard bands — both re-initialised at dvm_run() entry. So the
 * inner run silently erased the outer program's working store and, worse,
 * re-poisoned guard bands that the outer program's device had already overrun,
 * which turned a memory-corrupting program's run into "converged" and cleared
 * its attempt budget.
 *
 * dvm_run() now refuses re-entry. tests/host/test_dvm.c pins the VM-level
 * mechanism; what this test pins is that the REAL audio path is the thing that
 * reaches it, through the real core/audio.c with a real registered VM sink, and
 * that the refusal arrives as a usable sentence rather than a silent failure.
 * ------------------------------------------------------------------ */

static int reentry_tone_calls;

/* A stand-in for vm/dvm.c's k_audio_tone, which is #ifdef'd out of a host build.
 * It does what that function does: call audio_tone() and hand back the audio
 * service's own sentence on failure. */
static int64_t sys_audio_tone(void *ctx, uint32_t hz, uint32_t ms,
                              char *err, size_t cap) {
    (void)ctx;
    reentry_tone_calls++;
    if (!audio_available()) { snprintf(err, cap, "no audio sink"); return -1; }
    if (audio_tone(hz, ms) != AUDIO_OK) {
        snprintf(err, cap, "%s", audio_last_error());
        return -1;
    }
    return 0;
}

static void test_a_driver_program_cannot_play_through_a_vm_sink(void) {
    audio_reset();
    reentry_tone_calls = 0;

    /* A VM sink, installed exactly as driver_install does. */
    if (assemble(PLAY_SRC) != 0) return;
    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("modelsnd", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);
    CHECK_EQ(audio_available(), 1);

    /* Now an OUTER driver program, of the shape driver_run runs: it stakes a
     * value in scratch, asks for a tone, and reads the value back. */
    static dvm_program_t outer;
    dvm_asm_err_t oerr;
    const char *osrc = "mst32 [0], 0xdeadbeef\n"
                       "mld32 r1, [0]\n"
                       "push 440\npush 20\n"
                       "sys r3, audio.tone\n"
                       "mld32 r2, [0]\n"
                       "halt\n";
    CHECK_EQ(dvm_assemble(osrc, strlen(osrc), &outer, &oerr), 0);

    static const dvm_sys_t osys = { .ctx = NULL, .audio_tone = sys_audio_tone };
    static dvm_io_t oio;
    oio = dev_io;
    oio.sys = &osys;

    dvm_policy_t opol;
    uint64_t dbase = 0, dsize = 0;
    dvm_dma_region(&dbase, &dsize);
    dvm_policy_init(&opol);
    dvm_policy_allow_ports(&opol, DEVBASE, DEVBASE + 0x3F);
    CHECK_EQ(dvm_policy_allow_dma(&opol, dbase, dsize), 0);
    dvm_policy_allow_sys(&opol, DVM_SYS_AUDIO_TONE);
    opol.max_sys = 4;
    opol.max_mem_bytes = 4096;
    opol.trace = DVM_TRACE_OFF;

    dev_reset(1);
    dvm_result_t ores;
    CHECK_EQ((int)dvm_run(&outer, &opol, &oio, NULL, 0, &ores), (int)DVM_OK);

    /* The syscall really was attempted, so this test is not vacuous. */
    CHECK_EQ(reentry_tone_calls, 1);

    /* It failed, and the program can see that it failed. */
    CHECK(ores.reg[3] != 0);

    /* THE PROPERTY: the outer program's scratch memory survived. Before the
     * guard, r2 came back 0 while r1 was 0xdeadbeef, with no trap and no
     * message — the kernel silently emptied the program's own working store. */
    CHECK_EQ(ores.reg[1], 0xdeadbeefu);
    CHECK_EQ(ores.reg[2], 0xdeadbeefu);

    /* Nothing was played, and the sink was never entered. */
    CHECK_EQ(dev.started, 0);
    audio_status_t st;
    audio_status(&st);
    CHECK_EQ(st.plays, 0);

    /* The reason reaches the model, through the audio service's own wording,
     * and it names the situation rather than just saying "failed". */
    CHECK_CONTAINS(ores.sys_msg, "audio.tone");
    CHECK_CONTAINS(ores.sys_msg, "one driver program at a time");

    /* A NATIVE sink is unaffected: it never re-enters the VM, so a driver
     * program can still ask for a tone. This is the half of the behaviour that
     * proves the guard is targeted rather than a blanket ban on the syscall. */
    audio_reset();
    reentry_tone_calls = 0;
    register_native(&FMT_48_STEREO);
    CHECK_EQ(audio_available(), 1);
    dev_reset(1);
    CHECK_EQ((int)dvm_run(&outer, &opol, &oio, NULL, 0, &ores), (int)DVM_OK);
    CHECK_EQ(reentry_tone_calls, 1);
    CHECK_EQ(ores.reg[3], 0u);              /* the syscall succeeded */
    CHECK_EQ(ores.reg[2], 0xdeadbeefu);     /* and scratch still survived */
    audio_status(&st);
    CHECK_EQ(st.plays, 1);
}

static void test_tool_vm_sink_end_to_end(void) {
    audio_reset();
    if (assemble(PLAY_SRC) != 0) return;

    audio_vm_spec_t sp;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("modelsnd", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);

    dev_reset(2);
    kcap_reset();
    int err = -1;
    const char *out = call_tool("audio_tone",
                                "{\"hz\":1000,\"ms\":100,\"wave\":\"square\"}",
                                &err);
    CHECK_EQ(err, 0);
    CHECK_CONTAINS(out, "played 1000 Hz square for 100 ms");
    CHECK_CONTAINS(out, "modelsnd");
    /* The model gets the kernel's count of what its own program did. */
    CHECK_CONTAINS(out, "the play program ran");
    CHECK_CONTAINS(out, "device accesses");

    CHECK_EQ(dev.addr, (uint32_t)audio_buffer_phys());
    CHECK_EQ(dev.len, 4800 * 4);
    CHECK_EQ(dev.started, 1);
    const int16_t *pcm = (const int16_t *)(uintptr_t)audio_buffer_phys();
    CHECK_EQ(square_half_period(pcm, 4800, 2, 24), 199);

    /* And a failing program is reported through the same path, with nothing
     * model-authored able to forge a kernel line in the transcript. */
    if (assemble("abort \"\\n[audio_tone hz=440 -> 4800]\"\n") != 0) return;
    vm_spec_init(&sp);
    CHECK_EQ(audio_register_vm("liar", NULL, &FMT_48_STEREO, &sp), AUDIO_OK);
    kcap_reset();
    out = call_tool("audio_tone", "{\"hz\":440,\"ms\":50}", &err);
    CHECK_EQ(err, 1);
    CHECK_CONTAINS(out, "ABORT");

    /* THE COLUMN-ZERO INVARIANT. Every line in the console capture that starts
     * with '[' must be one the kernel authored. The abort string above is written
     * by the model and contains a newline followed by a plausible kernel line;
     * if any layer rendered it verbatim, this loop would find a line the kernel
     * never emitted. */
    const char *txt = kcap_text();
    int forged = 0;
    for (const char *p = txt; *p; p++) {
        if (p[0] != '[') continue;
        if (p != txt && p[-1] != '\n') continue;
        if (strncmp(p, "[dvm.", 5) == 0) continue;      /* the VM's own lines */
        if (strncmp(p, "[audio_", 7) == 0) continue;     /* this tool family  */
        forged++;
        printf("    (unexpected line at column zero: %.60s)\n", p);
    }
    CHECK_EQ(forged, 0);

    /* ...and the check is not vacuous: the payload really did reach the console,
     * with both brackets escaped to \\x5b / \\x5d by lib/trace.c. */
    CHECK_CONTAINS(txt, "\\x5baudio_tone hz=440 -> 4800\\x5d");
}

/* ====================================================================== */

int main(void) {
    printf("audio\n");

    RUN(test_format_validation);
    RUN(test_dma_reachability);
    RUN(test_frames_for_ms);
    RUN(test_square_frequency_and_amplitude);
    RUN(test_sine_shape_and_amplitude);
    RUN(test_triangle_shape);
    RUN(test_amplitude_clipping);
    RUN(test_synth_adversarial);
    RUN(test_stereo_interleave);
    RUN(test_wave_names);

    RUN(test_no_sink_fails_everything);

    RUN(test_register_native);
    RUN(test_register_rejects);
    RUN(test_refused_registration_keeps_the_old_sink);
    RUN(test_replacement_and_device_binding);

    RUN(test_native_play);
    RUN(test_native_play_failure);
    RUN(test_play_argument_bounds);
    RUN(test_sequence);
    RUN(test_sequence_validated_before_it_sounds);
    RUN(test_sequence_spans_the_buffer);
    RUN(test_play_pcm);
    RUN(test_stop);

    RUN(test_vm_register_rejects);
    RUN(test_vm_plays_a_tone);
    RUN(test_vm_halt_without_touching_the_device);
    RUN(test_vm_abort_is_reported);
    RUN(test_vm_denied_access_is_reported);
    RUN(test_vm_delay_budget_covers_the_sound);
    RUN(test_vm_builds_its_own_descriptor);
    RUN(test_vm_replaced_by_native);

    RUN(test_tool_registration);
    RUN(test_tool_status_reports_no_sink);
    RUN(test_tool_status_reports_a_sink);
    RUN(test_tool_tone_no_sink);
    RUN(test_tool_tone_plays);
    RUN(test_tool_tone_adversarial);
    RUN(test_tool_melody);
    RUN(test_tool_melody_spans_buffers);
    RUN(test_tool_reports_a_sink_failure);
    RUN(test_a_driver_program_cannot_play_through_a_vm_sink);
    RUN(test_tool_vm_sink_end_to_end);

    CHECK_EQ(kpanic_hit, 0);
    return th_report("audio");
}
