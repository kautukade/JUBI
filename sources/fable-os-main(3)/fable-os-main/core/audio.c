/* audio.c — the audio service. See include/audio.h for the architecture and the
 * play contract; this file is the implementation and the reasoning behind the
 * choices the header only states.
 *
 * WHY core/ AND NOT audio/ OR drivers/audio/
 *   drivers/ is wrong by definition: this file must never know a thing about any
 *   piece of audio hardware, and putting it next to e1000.c would invite exactly
 *   the "if the class is 04:01, do this" line that would spoil the experiment.
 *   A directory of its own is what a subsystem gets when it has several
 *   translation units (fs/, gui/, net/, vm/); this is one file with no plausible
 *   second one until a mixer exists. core/ is where the kernel's device-agnostic
 *   services live — core/tool.c is the syscall table, core/kobject.c is the
 *   object base — and a service whose entire purpose is to be device-agnostic
 *   belongs with them.
 *
 * WHAT IS IN THIS FILE, IN ONE LINE EACH
 *   1. synthesis: integer PCM from a frequency and a duration. No FPU, no drift.
 *   2. the buffer: where the samples live, and why it is the VM's arena not ours.
 *   3. the sink: registration, validation, replacement, and the device binding.
 *   4. the play path: fill, then either call a C driver or re-run a VM program.
 *   5. the boot report: one line of truth about whether sound is possible.
 *
 * THE ONE RULE THIS FILE IS POLICED BY
 *   grep it for a register name, a chip name, a vendor id or a PCI class code and
 *   you should find nothing. Everything device-specific is on the other side of
 *   audio_ops_t or inside the model's dvm program.
 */

#include "audio.h"
#include "device.h"
#include "driver.h"
#include "kernel.h"

#include <stdarg.h>
#include <stdio.h>       /* snprintf, via port/stdio.h when freestanding */

/* ====================================================================== */
/* 1. synthesis                                                           */
/* ====================================================================== */

/* sin(pi/2 * i/64) in Q15, i = 0..64. Generated once, checked in: this kernel is
 * built -mno-sse and has no way to compute it at run time, and a table produced
 * by a build-time script would make the build depend on a host toolchain for a
 * constant that will never change. The endpoints are the two values a test can
 * anchor on: 0 and 32767. */
static const int16_t sine_q15[65] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,
};

/* Q15 magnitude of sin(pi/2 * idx/65536) for idx in 0..65536, linearly
 * interpolated between table points. Worst-case error against a real sine is
 * under 0.05% of full scale, which is inaudible and, more usefully, small enough
 * that a test can assert the peak is EXACTLY the requested amplitude. */
static uint32_t sin_q15(uint32_t idx) {
    if (idx >= 65536u) return 32767u;          /* the quarter-period endpoint */
    uint32_t scaled = idx * 64u;               /* 0 .. 4194304                */
    uint32_t ti     = scaled >> 16;            /* table index 0..63           */
    uint32_t fr     = scaled & 0xFFFFu;        /* fraction between ti and ti+1 */
    int32_t  a      = sine_q15[ti];
    int32_t  b      = sine_q15[ti + 1];
    return (uint32_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)fr) >> 16));
}

/* The phase of frame `frame` as a full 32-bit turn, computed from the frame
 * INDEX rather than accumulated frame to frame.
 *
 * An accumulator (phase += hz<<32/rate) is the usual trick and it is wrong here
 * for two reasons: the step has to be truncated, so a one-second tone drifts by
 * however much that truncation was worth, and the drift means a period that
 * divides the sample rate never lands exactly on its peak. Recomputing costs one
 * 64-bit modulo and one divide per frame — 48000 of each per second of audio, on
 * a kernel that is doing nothing else while it plays — and buys exactness that a
 * test can assert on rather than approximate. */
static uint32_t phase_at(uint32_t frame, uint32_t hz, uint32_t rate) {
    uint64_t p = ((uint64_t)frame * (uint64_t)hz) % (uint64_t)rate;
    return (uint32_t)((p << 32) / (uint64_t)rate);
}

/* One sample. `amp` is already clamped to AUDIO_AMP_MAX by the caller, which is
 * what makes every multiply below unable to reach the value that would wrap a
 * sample to the opposite sign. */
static int16_t sample_of(uint32_t phase, uint32_t amp, audio_wave_t wave) {
    uint32_t q = phase >> 30;                  /* quarter of the period, 0..3 */
    uint32_t f = (phase >> 14) & 0xFFFFu;      /* position inside it, 0..65535 */

    switch (wave) {
        case AUDIO_WAVE_SQUARE:
            /* First half-cycle positive, so frame 0 of any square tone is
             * +amplitude — a fixed reference a test can start counting from. */
            return (int16_t)((phase & 0x80000000u) ? -(int32_t)amp : (int32_t)amp);

        case AUDIO_WAVE_TRIANGLE: {
            int32_t u;                         /* Q16: -65536 .. +65536 */
            if      (q == 0) u =  (int32_t)f;             /* 0 -> +1  */
            else if (q == 1) u =  65536 - (int32_t)f;     /* +1 -> 0  */
            else if (q == 2) u = -(int32_t)f;             /* 0 -> -1  */
            else             u = -(65536 - (int32_t)f);   /* -1 -> 0  */
            return (int16_t)(((int64_t)u * (int64_t)amp) / 65536);
        }

        default: {                             /* AUDIO_WAVE_SINE */
            /* Quarters 1 and 3 are the mirror of 0 and 2, so one quarter table
             * covers the whole period. */
            uint32_t u = sin_q15((q & 1u) ? (65536u - f) : f);
            int32_t  v = (int32_t)(((uint64_t)u * (uint64_t)amp) / 32767u);
            return (int16_t)((q >= 2u) ? -v : v);
        }
    }
}

uint32_t audio_frame_bytes(const audio_format_t *f) {
    if (!f) return 0;
    if (f->bits != 16) return 0;
    if (f->channels < 1 || f->channels > 2) return 0;
    if (f->rate_hz < AUDIO_RATE_MIN || f->rate_hz > AUDIO_RATE_MAX) return 0;
    return (uint32_t)f->channels * 2u;
}

int audio_format_valid(const audio_format_t *f, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    if (!f) {
        if (why) snprintf(why, cap, "no format was supplied");
        return 0;
    }
    if (f->bits != 16) {
        if (why) snprintf(why, cap,
            "%u-bit samples: this kernel synthesises signed 16-bit PCM and "
            "nothing else", (unsigned)f->bits);
        return 0;
    }
    if (f->channels < 1 || f->channels > 2) {
        if (why) snprintf(why, cap,
            "%u channels: 1 (mono) or 2 (interleaved stereo) only",
            (unsigned)f->channels);
        return 0;
    }
    if (f->rate_hz < AUDIO_RATE_MIN || f->rate_hz > AUDIO_RATE_MAX) {
        if (why) snprintf(why, cap,
            "%lu Hz is outside %lu-%lu Hz; a rate that far out is far more "
            "likely to be a mis-read register than exotic hardware",
            (unsigned long)f->rate_hz, (unsigned long)AUDIO_RATE_MIN,
            (unsigned long)AUDIO_RATE_MAX);
        return 0;
    }
    return 1;
}

uint32_t audio_frames_for_ms(const audio_format_t *f, uint32_t ms) {
    if (!audio_frame_bytes(f)) return 0;
    uint64_t n = ((uint64_t)f->rate_hz * (uint64_t)ms) / 1000ull;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

/* Milliseconds a run of `frames` lasts, rounded UP. Used to size a play
 * program's delay budget, where rounding down would make the budget fractionally
 * shorter than the sound and turn a working driver into an intermittent
 * DVM_TRAP_DELAY_BUDGET. */
static uint32_t ms_for_frames(uint32_t frames, uint32_t rate) {
    if (!rate) return 0;
    uint64_t ms = ((uint64_t)frames * 1000ull + (uint64_t)rate - 1ull) / (uint64_t)rate;
    return ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ms;
}

size_t audio_synth(int16_t *dst, size_t dst_frames, const audio_format_t *fmt,
                   uint32_t hz, uint32_t ms, uint32_t amplitude,
                   audio_wave_t wave) {
    if (!dst || !dst_frames) return 0;
    uint32_t fb = audio_frame_bytes(fmt);
    if (!fb) return 0;
    if ((unsigned)wave >= (unsigned)AUDIO_WAVE__COUNT) return 0;

    /* Above Nyquist there is no waveform to write, only aliasing. Refusing is
     * the honest answer; emitting something and calling it a 30 kHz tone would
     * be the "silence that looks like success" failure wearing a hat. */
    if (hz && (uint64_t)hz * 2ull >= (uint64_t)fmt->rate_hz) return 0;

    uint32_t want = audio_frames_for_ms(fmt, ms);
    size_t   n    = (want < dst_frames) ? (size_t)want : dst_frames;
    if (!n) return 0;

    uint32_t amp = (amplitude > AUDIO_AMP_MAX) ? AUDIO_AMP_MAX : amplitude;
    uint32_t ch  = fmt->channels;

    /* hz == 0 is a rest and amp == 0 is a muted note; both are exact silence,
     * and both are legitimate rather than errors. */
    if (!hz || !amp) {
        memset(dst, 0, n * ch * sizeof(int16_t));
        return n;
    }

    for (size_t i = 0; i < n; i++) {
        int16_t v = sample_of(phase_at((uint32_t)i, hz, fmt->rate_hz), amp, wave);
        for (uint32_t c = 0; c < ch; c++) dst[i * ch + c] = v;
    }
    return n;
}

static const char *const wave_names[AUDIO_WAVE__COUNT] = {
    "sine", "square", "triangle"
};

const char *audio_wave_name(audio_wave_t w) {
    if ((unsigned)w >= (unsigned)AUDIO_WAVE__COUNT) return "?";
    return wave_names[w];
}

int audio_wave_from_name(const char *s, audio_wave_t *out) {
    if (!s || !out) return -1;
    for (int i = 0; i < AUDIO_WAVE__COUNT; i++) {
        const char *n = wave_names[i];
        size_t j = 0;
        while (n[j] && n[j] == s[j]) j++;
        if (!n[j] && !s[j]) { *out = (audio_wave_t)i; return 0; }
    }
    return -1;
}

const char *audio_sink_kind_name(audio_sink_kind_t k) {
    switch (k) {
        case AUDIO_SINK_NATIVE: return "native";
        case AUDIO_SINK_VM:     return "vm-program";
        default:                return "none";
    }
}

/* ====================================================================== */
/* 2. the buffer                                                          */
/* ====================================================================== */

/* THE ONE PIECE OF MEMORY A DEVICE READS DIRECTLY — and this module does not own
 * it. vm/dvm.c does, and publishes it through dvm_dma_region().
 *
 * Not owning it is the whole point. There must be exactly one region on this
 * machine that is page-aligned, inside the identity-mapped low 4 GiB, outside the
 * heap, and outside the VM's memory deny list — because that combination is what
 * a bus master can be pointed at AND a sandboxed program can write. A second such
 * region here would be a second thing to get wrong, and worse: a play program
 * granted dvm's arena could not see a private buffer of ours at all, so a
 * descriptor list it built during bring-up would not be there at play time.
 *
 * The samples go in the FRONT of that arena. Everything from AUDIO_PCM_BYTES to
 * the end belongs to the driver and is never touched here. */
static uint64_t region_base(void) {
    uint64_t base = 0, size = 0;
    dvm_dma_region(&base, &size);
    return size >= (uint64_t)AUDIO_PCM_BYTES ? base : 0;
}

static int16_t *pcm_buffer(void) {
    return (int16_t *)(uintptr_t)region_base();
}

uint64_t audio_buffer_phys(void)  { return region_base(); }
uint32_t audio_buffer_bytes(void) { return AUDIO_PCM_BYTES; }

uint32_t audio_region_bytes(void) {
    uint64_t base = 0, size = 0;
    dvm_dma_region(&base, &size);
    return (uint32_t)size;
}

/* ====================================================================== */
/* 3. the sink                                                            */
/* ====================================================================== */

/* The installed play program. Static, 12 KB, for the same reason the buffer is:
 * registration must not be able to fail for memory, and a heap image would have
 * to be freed by whoever replaces the sink — a lifetime problem with a play path
 * running through it. One sink means one image. */
static dvm_program_t g_prog;

static struct {
    audio_sink_kind_t kind;
    char              name[AUDIO_NAME_MAX];
    char              devname[AUDIO_DEVNAME_MAX];
    device_t         *dev;
    audio_format_t    fmt;

    audio_ops_t       ops;              /* AUDIO_SINK_NATIVE */

    dvm_policy_t      pol;              /* AUDIO_SINK_VM     */
    const dvm_io_t   *io;
    uint64_t          bar[6];
    uint64_t          bdf;

    uint32_t          plays, failures;
    uint32_t          frames_last, ms_last;

    int               vm_ran;
    dvm_status_t      vm_status;
    uint32_t          vm_line;
    uint64_t          vm_steps, vm_io_ops;

    char              last_error[AUDIO_WHY_MAX];
} g;

/* The driver_t the sink is bound to in the device tree. NOT registered with
 * REGISTER_DRIVER: it is not a driver the manager should call init() on, it is a
 * label so `driver=` in driver_report() names whoever is really making the
 * sound. .name points into g.name, which is copied and bounded, so the device
 * tree can never print a dangling or over-long string. drivers_suspend_all()
 * skips it because .suspend is NULL. */
static driver_t g_sink_driver;

/* ---- errors ------------------------------------------------------------- */

/* Record a failure sentence, SANITISED.
 *
 * The reason for the sanitising: part of what lands here is model-controlled. A
 * VM play program's `abort "..."` string is written by the model, and it reaches
 * this buffer through dvm_result_t::msg. Trace lines already escape their
 * arguments (lib/trace.c), and tool results are never echoed to the console, so
 * nothing today would print this at column zero — but the kernel's ground-truth
 * invariant is that a '[' in column zero is always the kernel's, and the cheapest
 * way to keep a future caller from breaking it is for the string to be incapable
 * of carrying the bytes. Control characters become spaces and brackets become
 * parentheses. */
static void set_error(const char *fmt, ...) {
    char raw[AUDIO_WHY_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);

    size_t o = 0;
    for (size_t i = 0; raw[i] && o + 1 < sizeof g.last_error; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '[') c = '(';
        else if (c == ']') c = ')';
        else if (c < 0x20 || c > 0x7E) c = ' ';
        g.last_error[o++] = (char)c;
    }
    g.last_error[o] = '\0';
}

static void clear_error(void) { g.last_error[0] = '\0'; }

const char *audio_last_error(void) { return g.last_error; }

/* ---- validation --------------------------------------------------------- */

/* A sink name is printed in the device tree, in the boot report and in every
 * trace line, so it must be one bounded, space-free, printable token. Rejected
 * rather than mangled: a name quietly rewritten is a name that does not match
 * what the registrar thinks it registered.
 *
 * Brackets are refused along with the control characters. Nothing today could
 * put this string at column zero, and lib/trace.c escapes its arguments anyway —
 * but the kernel's ground-truth invariant is that a '[' in column zero is the
 * kernel's, and a name that cannot contain one cannot participate in breaking
 * it, whatever a future caller prints. */
static int name_ok(const char *name) {
    if (!name || !name[0]) return 0;
    size_t i = 0;
    for (; name[i]; i++) {
        if (i >= AUDIO_NAME_MAX - 1) return 0;
        unsigned char c = (unsigned char)name[i];
        if (c < 0x21 || c > 0x7E) return 0;
        if (c == '[' || c == ']') return 0;
    }
    return 1;
}

/* A device fetches this buffer over the bus with a 32-bit address on most
 * hardware, and there is no IOMMU here to fix it up. Above 4 GiB the transfer
 * would land somewhere else entirely — which is not a quieter sound, it is a
 * write into unrelated memory. */
int audio_dma_reachable(uint64_t base, uint64_t size) {
    if (!base || !size) return 0;
    if (base + size < base) return 0;                  /* wraps */
    return base + size <= 0x100000000ull;
}

/* The same question, asked of the real buffer.
 *
 * HOST DIVERGENCE, and it is worth stating because a green host test must not be
 * read as evidence about this: on the kernel the low 4 GiB is identity-mapped, so
 * the buffer's virtual address IS the address a device is handed and the check
 * above is the real thing. In a host process there is no DMA, no identity map,
 * and the loader puts .bss where it likes — above 4 GiB on arm64 macOS — so the
 * address carries no meaning and testing it would only assert a fact about the
 * host loader. The PREDICATE is still host-tested, directly, with synthetic
 * addresses (audio_dma_reachable); that the real buffer satisfies it is a
 * property of the kernel image and is proved by booting.
 *
 * The same shape as vm/dvm.c's kernel_top(), for the same reason. */
static int dma_reachable(char *why, size_t cap) {
    uint64_t base = audio_buffer_phys();
    if (!base) {
        snprintf(why, cap,
                 "the driver VM published no DMA arena big enough for %lu bytes "
                 "of PCM, so there is nowhere for samples to live",
                 (unsigned long)AUDIO_PCM_BYTES);
        return 0;
    }
#ifndef FABLEOS_HOSTTEST
    if (audio_dma_reachable(base, audio_region_bytes())) return 1;
    snprintf(why, cap,
             "the DMA arena is at 0x%lx, which a 32-bit bus master cannot reach; "
             "no device can be given this address", (unsigned long)base);
    return 0;
#else
    return 1;
#endif
}

/* ---- the device-tree binding ------------------------------------------- */

static void unbind_device(void) {
    if (g.dev) {
        device_bind_driver(g.dev, NULL);
        /* Back to "known, nobody drives it" — which is the truth once the sink
         * that drove it is gone, and is what makes the device targetable by
         * another driver program again. */
        device_set_state(g.dev, DEV_STATE_REGISTERED);
    }
    g.dev        = NULL;
    g.devname[0] = '\0';
}

static void bind_device(device_t *d) {
    g.dev = d;
    if (!d) { g.devname[0] = '\0'; return; }
    snprintf(g.devname, sizeof g.devname, "%s", d->obj.name ? d->obj.name : "?");
    g_sink_driver.name  = g.name;
    g_sink_driver.level = DRV_LEVEL_LATE;
    g_sink_driver.cls   = DEV_CLASS_AUDIO;
    device_bind_driver(d, &g_sink_driver);
    device_set_state(d, DEV_STATE_ACTIVE);
}

/* ---- registration ------------------------------------------------------ */

/* Everything both kinds of sink must satisfy. Returns AUDIO_OK having changed
 * NOTHING, so a caller can finish its own kind-specific checks before any state
 * is replaced. */
static int check_common(const char *name, const audio_format_t *fmt) {
    char why[AUDIO_WHY_MAX];

    if (!name_ok(name)) {
        set_error("a sink name must be 1-%d printable characters with no spaces",
                  AUDIO_NAME_MAX - 1);
        return AUDIO_EINVAL;
    }
    if (!audio_format_valid(fmt, why, sizeof why)) {
        set_error("sink \"%s\" declared a format this kernel cannot fill: %s",
                  name, why);
        return AUDIO_EINVAL;
    }
    if (!dma_reachable(why, sizeof why)) {
        set_error("%s", why);
        return AUDIO_EINVAL;
    }
    return AUDIO_OK;
}

/* Install, after every check has passed. */
static void install(audio_sink_kind_t kind, const char *name, device_t *dev,
                    const audio_format_t *fmt) {
    unbind_device();
    memset(&g.ops, 0, sizeof g.ops);
    memset(&g.pol, 0, sizeof g.pol);
    memset(g.bar, 0, sizeof g.bar);
    g.io      = NULL;
    g.bdf     = 0;
    g.vm_ran  = 0;
    g.vm_status = DVM_OK;
    g.vm_line = 0;
    g.vm_steps = g.vm_io_ops = 0;
    g.plays = g.failures = 0;
    g.frames_last = g.ms_last = 0;

    g.kind = kind;
    g.fmt  = *fmt;
    snprintf(g.name, sizeof g.name, "%s", name);
    bind_device(dev);
    clear_error();
}

int audio_register_native(const char *name, struct device *dev,
                          const audio_format_t *fmt, const audio_ops_t *ops) {
    int rc = check_common(name, fmt);
    if (rc != AUDIO_OK) return rc;

    if (!ops || !ops->play) {
        set_error("sink \"%s\" supplied no play() hook, so it could never be "
                  "asked for a sound", name);
        return AUDIO_EINVAL;
    }

    install(AUDIO_SINK_NATIVE, name, (device_t *)dev, fmt);
    g.ops = *ops;
    return AUDIO_OK;
}

int audio_register_vm(const char *name, struct device *dev,
                      const audio_format_t *fmt, const audio_vm_spec_t *spec) {
    int rc = check_common(name, fmt);
    if (rc != AUDIO_OK) return rc;

    if (!spec || !spec->program) {
        set_error("sink \"%s\" supplied no play program", name);
        return AUDIO_EINVAL;
    }
    if (spec->program->ninsn == 0) {
        set_error("the play program for \"%s\" is empty: there is nothing to run "
                  "when a sound is wanted", name);
        return AUDIO_EINVAL;
    }

    /* Validate the image and the sandbox NOW. The alternative is discovering a
     * corrupt program or an impossible policy at the first sound — by which time
     * the model that could have fixed it has moved on, and the operator is
     * looking at silence rather than at a message. */
    dvm_asm_err_t aerr;
    memset(&aerr, 0, sizeof aerr);
    dvm_status_t vst = dvm_program_validate(spec->program, &aerr);
    if (vst != DVM_OK) {
        set_error("the play program for \"%s\" is not a valid image: %s at line %d",
                  name, dvm_status_name(vst), aerr.line);
        return AUDIO_EINVAL;
    }

    char pmsg[DVM_MSG_MAX];
    if (dvm_policy_check(&spec->policy, pmsg, sizeof pmsg) != DVM_OK) {
        set_error("the sandbox offered for \"%s\" is not permitted: %s", name, pmsg);
        return AUDIO_EINVAL;
    }

    const dvm_io_t *io = spec->io ? spec->io : dvm_io_hardware();
    if (!io) {
        set_error("this build has no hardware I/O backend, so a play program "
                  "could never reach a device");
        return AUDIO_EINVAL;
    }

    /* A program that can reach nothing cannot play anything. Catching it here
     * turns a puzzling PORT_DENIED on the first note into a sentence at install
     * time. */
    if (spec->policy.nport == 0 && spec->policy.nmmio == 0) {
        set_error("the sandbox offered for \"%s\" opens no ports and no memory "
                  "windows, so the program has no way to reach the device", name);
        return AUDIO_EINVAL;
    }

    /* THE DMA GRANT IS NOT OPTIONAL, for three separate reasons that all point
     * the same way: it is what lets the program write a descriptor list into the
     * space after the PCM, it is what dvm.c treats as the caller having accepted
     * that this device will bus-master (there is no IOMMU), and it is what arms
     * the guard bands dvm_dma_check() reads after every run. A sink installed
     * without one would play through a hole in all three. */
    if (spec->policy.dma_size == 0) {
        set_error("the sandbox offered for \"%s\" has no DMA grant. The samples "
                  "live in the VM's DMA arena, so the grant is what lets the "
                  "program write alongside them and what lets the kernel notice "
                  "a device that writes outside the buffer", name);
        return AUDIO_EINVAL;
    }
    if (spec->policy.dma_base != audio_buffer_phys() ||
        spec->policy.dma_size < (uint64_t)AUDIO_PCM_BYTES) {
        set_error("the DMA grant offered for \"%s\" is 0x%lx+%lu, but the PCM "
                  "occupies 0x%lx+%lu: the grant must start at the arena base and "
                  "cover at least the samples, or r7 would not point at them",
                  name, (unsigned long)spec->policy.dma_base,
                  (unsigned long)spec->policy.dma_size,
                  (unsigned long)audio_buffer_phys(),
                  (unsigned long)AUDIO_PCM_BYTES);
        return AUDIO_EINVAL;
    }

    install(AUDIO_SINK_VM, name, (device_t *)dev, fmt);
    memcpy(&g_prog, spec->program, sizeof g_prog);   /* the service owns it now */
    g.pol = spec->policy;
    g.io  = io;
    for (int i = 0; i < 6; i++) g.bar[i] = spec->bar[i];
    g.bdf = spec->bdf;
    return AUDIO_OK;
}

void audio_release(void) {
    unbind_device();
    g.kind = AUDIO_SINK_NONE;
    g.name[0] = '\0';
    memset(&g.ops, 0, sizeof g.ops);
    /* The format goes too. It belonged to the sink, and leaving it behind would
     * let audio_status() report a sample rate nothing is running at. */
    memset(&g.fmt, 0, sizeof g.fmt);
    g.io = NULL;
}

void audio_reset(void) {
    audio_release();
    memset(&g, 0, sizeof g);
}

/* ====================================================================== */
/* query                                                                  */
/* ====================================================================== */

int audio_available(void) { return g.kind != AUDIO_SINK_NONE; }

uint32_t audio_max_ms(void) {
    uint32_t fb = audio_frame_bytes(&g.fmt);
    if (!audio_available() || !fb) return 0;
    uint64_t cap_frames = AUDIO_PCM_BYTES / fb;
    uint64_t ms         = cap_frames * 1000ull / (uint64_t)g.fmt.rate_hz;
    return ms > AUDIO_MAX_MS ? AUDIO_MAX_MS : (uint32_t)ms;
}

void audio_status(audio_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->present   = audio_available();
    out->kind      = g.kind;
    out->fmt       = g.fmt;
    out->buf_phys  = audio_buffer_phys();
    out->buf_bytes = audio_buffer_bytes();
    out->region_bytes = audio_region_bytes();
    out->max_ms    = audio_max_ms();
    out->plays     = g.plays;
    out->failures  = g.failures;
    out->frames_last = g.frames_last;
    out->ms_last     = g.ms_last;
    snprintf(out->name,   sizeof out->name,   "%s", g.name);
    snprintf(out->device, sizeof out->device, "%s", g.devname);
    snprintf(out->last_error, sizeof out->last_error, "%s", g.last_error);
    if (g.kind == AUDIO_SINK_VM) {
        out->vm_insns  = g_prog.ninsn;
        out->vm_ran    = g.vm_ran;
        out->vm_status = g.vm_status;
        out->vm_line   = g.vm_line;
        out->vm_steps  = g.vm_steps;
        out->vm_io_ops = g.vm_io_ops;
    }
}

/* ====================================================================== */
/* 4. the play path                                                       */
/* ====================================================================== */

/* Re-enter the installed play program. See AUDIO_VM_CONTRACT in audio.h — the
 * register assignment below IS that contract, and the enum it indexes with is
 * the shared definition, so the words and the code cannot drift. */
static int run_vm(uint32_t bytes, uint32_t frames, uint32_t ms) {
    /* The program has to poll a real device for the whole length of the sound,
     * so its delay budget must cover the audio plus room for bring-up polling.
     * Raise it on a COPY: the installed policy is what the registrar agreed to
     * and must not creep upward across calls. Never lowered — a registrar that
     * asked for more keeps it. dvm_policy_check() below has the last word, so a
     * duration that would exceed the VM's own 2 s ceiling is refused with the
     * VM's message rather than silently clamped into a truncated sound. */
    dvm_policy_t pol = g.pol;
    uint64_t need = (uint64_t)ms * 1000ull + (uint64_t)AUDIO_PLAY_MARGIN_US;
    if (pol.max_delay_us < need) pol.max_delay_us = need;

    /* AND THE ACCESS BUDGET, for the same reason and on the same copy.
     *
     * The policy a sink is registered with came from driver_run, where it was
     * sized for BRING-UP: driver_run's DRV_MAX_IO is 1024 device accesses, which
     * is generous for "reset the chip and read some registers back" and is not
     * enough for the thing this contract actually asks for. The contract tells a
     * play program to "poll until the device has finished reading that memory",
     * and polling a device for the length of a sound costs one access per poll: an
     * 800 ms sound at one read per millisecond is 800 accesses before anything
     * else the program does, and a 1000 ms sound is over the whole budget. So the
     * documented instruction and the enforced budget disagreed, and the program
     * trapped with IO_LIMIT through no fault of its author.
     *
     * That is not hypothetical. It is what a live model hit: it found the card,
     * brought it up, installed a correct-looking play program, and its first tone
     * died on IO_LIMIT inside the poll loop.
     *
     * One access per millisecond of sound, doubled for a program that also reads a
     * status register per poll, on top of whatever the registrar already had. It is
     * proportional to the work asked for rather than a bigger constant, and it
     * stays far below the VM's own ceiling. It does not widen what the program may
     * TOUCH — the port and MMIO windows are unchanged — only how many times it may
     * touch the same allowed registers, and how long that can take is separately
     * bounded by max_delay_us above. */
    uint64_t polls = (uint64_t)ms * 2ull + 256ull;
    if (pol.max_io < polls) pol.max_io = polls;

    /* A GUARD, not a live path, and worth saying which. The policy was already
     * accepted by dvm_policy_check() at registration, and the only field raised
     * here is bounded by AUDIO_MAX_MS + AUDIO_PLAY_MARGIN_US = 1.25 s, which is
     * inside the VM's 2 s cumulative-delay ceiling — so with today's constants
     * this cannot fail. It is here because raising AUDIO_MAX_MS is a one-line
     * change that would otherwise turn into "long sounds mysteriously stop
     * working", and because a refusal must never become a silently truncated
     * sound. There is no host test for it for exactly the reason above. */
    char pmsg[DVM_MSG_MAX];
    if (dvm_policy_check(&pol, pmsg, sizeof pmsg) != DVM_OK) {
        set_error("%lu ms of audio needs a %lu ms delay budget, which the VM will "
                  "not grant: %s", (unsigned long)ms,
                  (unsigned long)(need / 1000ull), pmsg);
        return AUDIO_EIO;
    }

    uint64_t args[AUDIO_NARGS];
    memset(args, 0, sizeof args);
    for (int i = 0; i < 6; i++) args[AUDIO_REG_BAR0 + i] = g.bar[i];
    args[AUDIO_REG_BDF]    = g.bdf;
    args[AUDIO_REG_DMA]    = pol.dma_base;      /* == the arena base, checked  */
    args[AUDIO_REG_DMA_SZ] = pol.dma_size;      /*    at registration          */
    args[AUDIO_REG_PCM]    = bytes;
    args[AUDIO_REG_FRAMES] = frames;
    args[AUDIO_REG_MS]     = ms;
    /* Fixed for the lifetime of the sink, whatever this sound's length is. */
    args[AUDIO_REG_SCRATCH] = pol.dma_base + (uint64_t)AUDIO_PCM_BYTES;

    dvm_result_t res;
    dvm_status_t st = dvm_run(&g_prog, &pol, g.io, args, AUDIO_NARGS, &res);

    g.vm_ran    = 1;
    g.vm_status = st;
    g.vm_line   = res.line;
    g.vm_steps  = res.steps;
    g.vm_io_ops = res.io_ops;

    if (st != DVM_OK) {
        set_error("the play program for \"%s\" stopped: %s at line %lu%s%s",
                  g.name, dvm_status_name(st), (unsigned long)res.line,
                  res.msg[0] ? " - " : "", res.msg);
        return AUDIO_EIO;
    }

    /* Reaching `halt` is not the same as playing. The one-instruction program
     * `halt` reaches halt having touched nothing, and reporting that as a sound
     * is precisely the dishonesty this whole module exists to prevent: the
     * operator would hear nothing and be told it worked. Requiring at least one
     * device access is a weak test, but it is a true one, and it is the strongest
     * statement the kernel can make without knowing what the device is. */
    if (res.io_ops == 0) {
        set_error("the play program for \"%s\" reached halt without touching the "
                  "device once, so nothing was played", g.name);
        return AUDIO_EIO;
    }

    /* Did the DEVICE write outside the arena? This is the only mechanism on the
     * machine that notices a bad DMA address at all (dvm.h is explicit that it
     * only catches near misses), and a device scribbling past its buffer matters
     * far more than the tone did — so it is a failure even though a sound may
     * well have come out. Reported once, in the words dvm.c chose. */
    char dmsg[DVM_MSG_MAX];
    if (dvm_dma_check(dmsg, sizeof dmsg)) {
        set_error("the device driven by \"%s\" wrote outside its buffer: %s. "
                  "Playback is stopped; check the address the program handed it",
                  g.name, dmsg);
        return AUDIO_EIO;
    }
    return AUDIO_OK;
}

/* Hand the first `frames` frames of the buffer to whoever is registered. */
static int play_buffer(uint32_t frames) {
    uint32_t fb = audio_frame_bytes(&g.fmt);
    if (!frames || !fb) {
        set_error("there are no samples to play");
        return AUDIO_EINVAL;
    }
    uint32_t bytes = frames * fb;
    uint32_t ms    = ms_for_frames(frames, g.fmt.rate_hz);
    uint64_t phys  = audio_buffer_phys();

    int rc;
    if (g.kind == AUDIO_SINK_VM) {
        rc = run_vm(bytes, frames, ms);
    } else {
        char why[AUDIO_WHY_MAX];
        why[0] = '\0';
        int drv = g.ops.play(g.ops.ctx, phys, bytes, frames, ms, why, sizeof why);
        if (drv != 0) {
            set_error("sink \"%s\" could not play: %s", g.name,
                      why[0] ? why : "the driver gave no reason");
            rc = AUDIO_EIO;
        } else {
            rc = AUDIO_OK;
        }
    }

    if (rc == AUDIO_OK) {
        g.plays++;
        g.frames_last = frames;
        g.ms_last     = ms;
        clear_error();
    } else {
        g.failures++;
    }
    return rc;
}

/* The one sentence for "this machine cannot make a sound", in one place, so the
 * service and the tool layer cannot describe the same condition differently. */
const char *audio_no_sink_reason(void) {
    return "no audio output driver is registered, so this machine cannot make a "
           "sound yet. A driver must claim an audio device and register itself as "
           "the audio sink first";
}

/* The one place "is a sound possible at all" is answered. Every entry point
 * calls this FIRST, before looking at a single argument, so a machine with no
 * audio driver never reports a frequency problem it does not have. */
static int need_sink(void) {
    if (audio_available()) return AUDIO_OK;
    set_error("%s", audio_no_sink_reason());
    return AUDIO_ENOSINK;
}

/* Frames the buffer holds in the current format. */
static uint32_t capacity_frames(void) {
    uint32_t fb = audio_frame_bytes(&g.fmt);
    return fb ? (AUDIO_PCM_BYTES / fb) : 0;
}

/* Validate one note against the current sink. 0 on success. */
static int check_note(const audio_note_t *nt, int index, int of) {
    uint32_t nyquist = g.fmt.rate_hz / 2u;
    uint32_t hi      = (AUDIO_HZ_MAX < nyquist) ? AUDIO_HZ_MAX : nyquist - 1u;
    const char *where = "";
    char wbuf[48];

    if (of > 1) {
        snprintf(wbuf, sizeof wbuf, " (note %d of %d)", index + 1, of);
        where = wbuf;
    }

    if (nt->hz != 0 && (nt->hz < AUDIO_HZ_MIN || nt->hz > hi)) {
        set_error("%lu Hz%s is outside what this sink can play: %lu-%lu Hz "
                  "(0 means a rest). The sink runs at %lu Hz, so nothing at or "
                  "above %lu Hz exists for it",
                  (unsigned long)nt->hz, where, (unsigned long)AUDIO_HZ_MIN,
                  (unsigned long)hi, (unsigned long)g.fmt.rate_hz,
                  (unsigned long)nyquist);
        return AUDIO_EINVAL;
    }
    if (nt->ms == 0) {
        set_error("a duration of 0 ms%s would produce no samples", where);
        return AUDIO_EINVAL;
    }
    if (nt->ms > AUDIO_MAX_MS) {
        set_error("%lu ms%s is longer than one sound may be: the limit is %lu ms, "
                  "which is what the kernel's PCM buffer holds. Ask for several "
                  "notes instead of one long one",
                  (unsigned long)nt->ms, where, (unsigned long)audio_max_ms());
        return AUDIO_EINVAL;
    }
    uint32_t frames = audio_frames_for_ms(&g.fmt, nt->ms);
    if (!frames) {
        set_error("%lu ms%s at %lu Hz rounds down to no samples at all",
                  (unsigned long)nt->ms, where, (unsigned long)g.fmt.rate_hz);
        return AUDIO_EINVAL;
    }
    if (frames > capacity_frames()) {
        set_error("%lu ms%s needs %lu frames and the buffer holds %lu",
                  (unsigned long)nt->ms, where, (unsigned long)frames,
                  (unsigned long)capacity_frames());
        return AUDIO_ENOSPC;
    }
    if (nt->amplitude > AUDIO_AMP_MAX) {
        set_error("amplitude %lu%s is above full scale for 16-bit samples (%lu)",
                  (unsigned long)nt->amplitude, where, (unsigned long)AUDIO_AMP_MAX);
        return AUDIO_EINVAL;
    }
    if ((unsigned)nt->wave >= (unsigned)AUDIO_WAVE__COUNT) {
        set_error("waveform %u%s is not one this kernel synthesises",
                  (unsigned)nt->wave, where);
        return AUDIO_EINVAL;
    }
    return AUDIO_OK;
}

int audio_tone_ex(uint32_t hz, uint32_t ms, uint32_t amplitude,
                  audio_wave_t wave) {
    int rc = need_sink();
    if (rc != AUDIO_OK) return rc;

    /* audio_note_t::wave is a uint8_t, so the cast below would turn 256 into 0 —
     * silently substituting a sine for a waveform that does not exist. Checked
     * BEFORE it is narrowed, so an out-of-range request is an error rather than a
     * different sound than the one asked for. */
    if ((unsigned)wave >= (unsigned)AUDIO_WAVE__COUNT) {
        set_error("waveform %lu is not one this kernel synthesises",
                  (unsigned long)wave);
        return AUDIO_EINVAL;
    }

    audio_note_t nt;
    nt.hz        = hz;
    nt.ms        = ms;
    nt.amplitude = amplitude;
    nt.wave      = (uint8_t)wave;
    return audio_play_notes(&nt, 1);
}

int audio_tone(uint32_t hz, uint32_t ms) {
    return audio_tone_ex(hz, ms, AUDIO_AMP_DEFAULT, AUDIO_WAVE_SINE);
}

int audio_play_notes(const audio_note_t *notes, int n) {
    int rc = need_sink();
    if (rc != AUDIO_OK) return rc;

    if (!notes || n <= 0) {
        set_error("no notes were supplied");
        return AUDIO_EINVAL;
    }
    if (n > AUDIO_SEQ_MAX) {
        set_error("%d notes is more than one sequence may hold (%d)",
                  n, AUDIO_SEQ_MAX);
        return AUDIO_EINVAL;
    }

    /* EVERY note is checked before the first one sounds. The alternative —
     * validate as you go — plays eight notes and then reports an error, leaving
     * the caller unable to tell what was heard from what was refused. */
    for (int i = 0; i < n; i++) {
        rc = check_note(&notes[i], i, n);
        if (rc != AUDIO_OK) return rc;
    }

    uint32_t cap = capacity_frames();
    uint32_t ch  = g.fmt.channels;
    uint32_t used = 0;

    for (int i = 0; i < n; i++) {
        uint32_t want = audio_frames_for_ms(&g.fmt, notes[i].ms);

        /* Pack consecutive notes into one buffer-full so a melody is gapless
         * where it can be, and flush when the next note will not fit. */
        if (used && used + want > cap) {
            rc = play_buffer(used);
            if (rc != AUDIO_OK) return rc;
            used = 0;
        }

        uint32_t amp = notes[i].amplitude ? notes[i].amplitude : AUDIO_AMP_DEFAULT;
        size_t got = audio_synth(&pcm_buffer()[(size_t)used * ch],
                                 cap - used, &g.fmt,
                                 notes[i].hz, notes[i].ms, amp,
                                 (audio_wave_t)notes[i].wave);
        if (got == 0) {
            /* check_note() has already ruled out every argument that can do
             * this, so reaching here means the synthesiser and the validator
             * disagree — a source bug, reported as one rather than as silence. */
            set_error("the synthesiser produced no samples for note %d "
                      "(%lu Hz, %lu ms): this is a kernel bug, not a bad request",
                      i + 1, (unsigned long)notes[i].hz,
                      (unsigned long)notes[i].ms);
            g.failures++;
            return AUDIO_EINVAL;
        }
        used += (uint32_t)got;
    }

    if (used) {
        rc = play_buffer(used);
        if (rc != AUDIO_OK) return rc;
    }
    return AUDIO_OK;
}

int audio_play_pcm(const int16_t *frames, uint32_t nframes) {
    int rc = need_sink();
    if (rc != AUDIO_OK) return rc;

    if (!frames || !nframes) {
        set_error("no PCM was supplied");
        return AUDIO_EINVAL;
    }
    uint32_t cap = capacity_frames();
    if (nframes > cap) {
        set_error("%lu frames is more than the buffer holds (%lu)",
                  (unsigned long)nframes, (unsigned long)cap);
        return AUDIO_ENOSPC;
    }
    uint32_t ms = ms_for_frames(nframes, g.fmt.rate_hz);
    if (ms > AUDIO_MAX_MS) {
        set_error("%lu frames is %lu ms of audio and one sound may be at most "
                  "%lu ms", (unsigned long)nframes, (unsigned long)ms,
                  (unsigned long)AUDIO_MAX_MS);
        return AUDIO_EINVAL;
    }

    /* Copied, never referenced: the caller's memory must not become a DMA
     * target. The buffer is the only region this kernel promises a device may
     * read, and a caller's stack array is emphatically not it. */
    memcpy(pcm_buffer(), frames,
           (size_t)nframes * g.fmt.channels * sizeof(int16_t));
    return play_buffer(nframes);
}

int audio_stop(void) {
    int rc = need_sink();
    if (rc != AUDIO_OK) return rc;

    if (g.kind == AUDIO_SINK_NATIVE && g.ops.stop) {
        if (g.ops.stop(g.ops.ctx) != 0) {
            set_error("sink \"%s\" could not be stopped", g.name);
            return AUDIO_EIO;
        }
        clear_error();
        return AUDIO_OK;
    }
    set_error("sink \"%s\" has no stop path: it plays each sound synchronously, "
              "so by the time anything can ask, it has already stopped", g.name);
    return AUDIO_EINVAL;
}

/* ====================================================================== */
/* 5. the boot report                                                     */
/* ====================================================================== */

/* One line at boot saying whether this machine can make a sound.
 *
 * It runs LATE, after every in-tree driver has had its chance to register, so
 * "nothing is registered" is a fact about the finished boot rather than about
 * ordering. On a machine with no audio driver the answer is "no", printed
 * plainly — which is the point: the operator should never have to infer from
 * silence whether the subsystem is missing or the speaker is. */
static int audio_service_init(void) {
    if (!audio_available()) {
        kputs("audio: no output driver is registered - nothing can play a sound "
              "yet\n");
        kprintf("audio: %u KB of PCM space is waiting at 0x%x (of a %u KB DMA "
                "arena) for a driver to claim an audio device and register as "
                "the sink\n",
                (unsigned)(AUDIO_PCM_BYTES / 1024u),
                (unsigned)audio_buffer_phys(),
                (unsigned)(audio_region_bytes() / 1024u));
        return 0;
    }
    /* g.name is bounded, printable and space-free by name_ok(), so printing it
     * cannot disturb the console or forge a trace line. */
    kprintf("audio: sink \"%s\" (%s) is registered on %s: %u Hz, %u channel(s), "
            "%u-bit, up to %u ms per sound\n",
            g.name, audio_sink_kind_name(g.kind),
            g.devname[0] ? g.devname : "no device node",
            (unsigned)g.fmt.rate_hz, (unsigned)g.fmt.channels,
            (unsigned)g.fmt.bits, (unsigned)audio_max_ms());
    return 0;
}

/* cls is NONE on purpose: this is the SERVICE, not a driver for audio hardware,
 * and claiming DEV_CLASS_AUDIO here would make driver_find_for_class() start
 * answering with it. The sink's own label (g_sink_driver) is the one that carries
 * DEV_CLASS_AUDIO.
 *
 * Excluded from host builds, as drivers/rtc/rtc.c already is and for the same
 * reason: REGISTER_DRIVER puts a pointer in a section named "driver_table", and
 * Mach-O requires "SEGMENT,SECTION" so the bare name will not compile with the
 * host toolchain. Nothing is lost — the only thing behind the macro is the boot
 * line, whose whole claim is about a finished boot, and `make test-qemu` is what
 * proves that. Everything the host suite tests is above this point. */
#ifndef FABLEOS_HOSTTEST
static const driver_t audio_service_driver = {
    .name  = "audio",
    .level = DRV_LEVEL_LATE,
    .cls   = DEV_CLASS_NONE,
    .init  = audio_service_init,
};
REGISTER_DRIVER(audio_service_driver);
#else
/* Referenced so the host build does not warn it away, and so a change that broke
 * the report would still fail to compile under `make test-host`. */
int (*const audio_service_init_ref)(void) = audio_service_init;
#endif
