/* audio.h — the audio service: one place that answers "can this machine make a
 * sound, and who is doing it".
 *
 * PURPOSE
 *   This kernel's whole point is that a model can write a driver for hardware
 *   nobody taught it about (include/dvm.h, tools/dvm_tools.c). Writing one is
 *   half the story. The other half is USING it: after the bring-up program has
 *   run once, something has to be able to say "play a 440 Hz tone" and have that
 *   reach the device, repeatedly, for the rest of the session.
 *
 *   Before this file the only way sound happened was vm/programs/ac97_boot.c: a
 *   C file that runs one hardcoded program at boot and plays one hardcoded tone.
 *   That proves the VM can drive silicon, and proves nothing about a driver being
 *   usable afterwards. This is the missing subsystem — the equivalent of a sound
 *   stack that any driver registers with, whether it is C in the tree or a VM
 *   program the model wrote ninety seconds ago.
 *
 * NO DEVICE KNOWLEDGE LIVES HERE
 *   Not one register name, chip name, class code or vendor id appears in
 *   audio.h, core/audio.c or tools/audio_tools.c, and none may ever be added.
 *   The division of labour is exact:
 *
 *     the kernel  owns the DMA-reachable buffer, turns "440 Hz for 250 ms" into
 *                 signed 16-bit PCM (arithmetic, not device knowledge), decides
 *                 what a sink is allowed to be, and reports honestly.
 *     the driver  owns everything about how one particular chip is made to emit
 *                 the bytes in that buffer. That is the model's job, and if this
 *                 file ever contains a hint about it, the experiment is spoiled.
 *
 *   The consequence to keep in mind while reading: audio.c cannot build a DMA
 *   descriptor list, because the *layout* of one is device knowledge. It can only
 *   hand over a flat buffer and an address. See THE VM PLAY PATH.
 *
 * ARCHITECTURE
 *   Exactly one sink is current, because the question this file exists to answer
 *   ("who is making the sound") has to have one answer. Registering a second one
 *   replaces the first, and the replacement is visible in the device tree:
 *   audio.c binds a driver to the registering device, so `driver=` in
 *   driver_report() shows the truth rather than the wishful version.
 *
 *   Two kinds of sink, one play path:
 *
 *     AUDIO_SINK_NATIVE   a C driver in the tree supplies audio_ops_t.
 *     AUDIO_SINK_VM       a model-authored dvm program is re-invoked per sound.
 *
 *   Everything above the sink (synthesis, bounds, the buffer, the reporting, the
 *   tools) is shared, so a VM-authored driver is not a second-class citizen with
 *   a different feature set — it reaches the same tools a C driver would.
 *
 * THE VM PLAY PATH — the contract a model has to satisfy
 *   A bring-up program initialises the device once and is run by driver_run. A
 *   PLAY program is a *separate, smaller* program that is registered here and
 *   re-entered from its first instruction every time a sound is wanted. Two
 *   programs rather than one entry-point mechanism is deliberate: it needs
 *   nothing from dvm.h that does not already exist, and it keeps "set the codec
 *   up" out of the hot path.
 *
 *   The contract is AUDIO_VM_CONTRACT below, verbatim, and it is the single
 *   source of truth: audio.c preloads exactly those registers, and the tool
 *   descriptions the model reads interpolate the same macro. If it changes, it
 *   changes in one place.
 *
 * WHERE THE SAMPLES LIVE — dvm.c's arena, not a buffer of our own
 *   The PCM goes in the front of the DMA scratch arena vm/dvm.c publishes
 *   through dvm_dma_region(). That is not an economy, it is the only arrangement
 *   that works:
 *
 *     - it is the one region on this machine that is page-aligned, inside the
 *       identity-mapped low 4 GiB (so its address IS its physical address), and
 *       outside both the heap and the VM's memory deny list;
 *     - a play program can therefore both READ it (as the device's source) and
 *       WRITE it (ld/st under a DMA grant), which is what makes descriptor-list
 *       hardware reachable at all — the layout of a descriptor list is device
 *       knowledge, so the kernel must supply the memory and nothing else;
 *     - and it is the SAME memory a bring-up program was granted under
 *       driver_run, so a descriptor list built during bring-up is still there at
 *       play time. A private buffer here would have thrown that away.
 *
 *   The split is fixed and documented so both halves can rely on it: the first
 *   AUDIO_PCM_BYTES of the arena are the kernel's PCM, and everything after that
 *   belongs to the driver. Nothing in this module ever writes the tail, so
 *   whatever a program puts there survives from one sound to the next.
 *
 * HONESTY RULES
 *   - With no sink registered, every play call fails with AUDIO_ENOSINK and a
 *     sentence saying so. Silence that returns success is the one outcome this
 *     module must never produce: on a machine with no shell, "it worked" and
 *     nothing audible is indistinguishable from a broken speaker.
 *   - Arguments are REJECTED, never truncated. A 5-second tone into a 1-second
 *     buffer is an error naming the limit, not a 1-second tone reported as 5.
 *   - Every failure records a sentence retrievable with audio_last_error(), so
 *     the tool layer can hand the model something specific enough to fix.
 *
 * PUBLIC API
 *   registration   audio_register_native / audio_register_vm / audio_release
 *   query          audio_available / audio_status / audio_last_error
 *                  audio_buffer_phys / audio_buffer_bytes / audio_max_ms
 *   synthesis      audio_synth / audio_frames_for_ms   (pure, no sink needed)
 *   playback       audio_tone / audio_tone_ex / audio_play_notes /
 *                  audio_play_pcm / audio_stop
 *   naming         audio_wave_name / audio_wave_from_name / audio_sink_kind_name
 *   tests          audio_reset
 *
 * DEPENDENCIES
 *   dvm.h — for the DMA arena the samples live in and to re-invoke a registered
 *   play program — device.h (publishing the sink into the device tree), and
 *   kernel.h for the console. No heap anywhere: the samples go in dvm.c's static
 *   arena and the installed program image is a static of its own, so registration
 *   cannot fail for memory and a play call allocates nothing. No interrupts:
 *   playback is polled, like everything else in this kernel.
 *
 * FUTURE EXTENSION POINTS
 *   - Formats: 16-bit signed is the only one accepted, because it is the only one
 *     the synthesiser emits. audio_format_t already carries bits/channels/rate,
 *     so 8- and 24-bit are a widening of audio_synth() and a validation change.
 *   - Playback is synchronous by contract. Asynchronous playback needs a "is the
 *     device still reading?" query, which means a second registered program and a
 *     poll hook; the contract note about refilling the buffer is what would have
 *     to change first.
 *   - A mixer (more than one sink, or more than one stream) is a real
 *     possibility, but "who is making the sound" must keep exactly one answer,
 *     so it would be a stream layer above this, not a second current sink.
 *   - Sounds longer than AUDIO_PCM_BYTES need the buffer refilled while the
 *     device is still reading it, which the synchronous contract forbids. The
 *     honest version is a second registered program that reports the device's
 *     current read position, so the kernel can refill the half the device has
 *     passed; that is a real feature, not a constant to raise. Raising
 *     AUDIO_MAX_MS alone would also breach the VM's cumulative-delay ceiling —
 *     see the guard in run_vm().
 *   - AUDIO_PCM_BYTES splits dvm.c's arena by a constant both sides agree on by
 *     reading this header. If a device ever needs more than the 64 KiB left over,
 *     the split wants to become part of the sink's registration rather than a
 *     compile-time number.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stddef.h>

#include "dvm.h"

struct device;      /* forward: see device.h */

/* ====================================================================== */
/* error codes                                                            */
/* ====================================================================== */

/* Negative errnos from the space vfs.h defines and tool.h reuses, so a trace
 * line renders them symbolically (ENOENT, not -2). AUDIO_ENOSINK is ENOENT's
 * value on purpose: "the thing you asked for is not here" is exactly right, and
 * a new number would print as a bare integer in the one place the operator is
 * meant to be able to trust. */
#define AUDIO_OK         0
#define AUDIO_ENOSINK   -2     /* no audio output is registered            */
#define AUDIO_EINVAL   -22     /* bad argument (and it says which)         */
#define AUDIO_ENOSPC   -28     /* does not fit in the buffer               */
#define AUDIO_EIO       -5     /* the sink was asked and it failed         */

/* ====================================================================== */
/* limits — all enforced, all testable                                    */
/* ====================================================================== */

#define AUDIO_NAME_MAX        24    /* sink name, including NUL            */
#define AUDIO_DEVNAME_MAX     41    /* device-tree name, including NUL      */
#define AUDIO_WHY_MAX        192    /* one failure sentence                */

/* How much of vm/dvm.c's DMA arena the kernel fills with samples. 192 KiB is
 * 1024 ms of 48 kHz stereo 16-bit, the worst case of every format this module
 * accepts; anything slower or mono needs less. The arena is DVM_DMA_SIZE
 * (256 KiB), so the remaining 64 KiB is the driver's — enough for any descriptor
 * list, ring or command buffer real audio hardware asks for, which is the split
 * dvm.h sized the arena for. */
#define AUDIO_PCM_BYTES  (192u * 1024u)
_Static_assert(AUDIO_PCM_BYTES < DVM_DMA_SIZE,
               "the PCM span must leave the driver room in the DMA arena");

/* Longest single sound. Bounded by dvm.c's 2 s ceiling on cumulative DELAY: a
 * play program has to be able to poll the whole buffer out inside its delay
 * budget, and that budget is duration + AUDIO_PLAY_MARGIN_US. */
#define AUDIO_MAX_MS          1000u
#define AUDIO_PLAY_MARGIN_US  250000u

#define AUDIO_AMP_MAX        32767u   /* full scale for signed 16-bit       */
#define AUDIO_AMP_DEFAULT     8000u   /* ~24% of full scale: loud, not harsh */

/* Audible range. Nothing outside it is a sound, and a request outside it is far
 * more likely to be a confused model than a deliberate ultrasonic test. The
 * sink's own Nyquist limit (rate/2) is applied on top of this, per sink. */
#define AUDIO_HZ_MIN            20u
#define AUDIO_HZ_MAX         20000u

/* Sample rates a sink may declare. The low end is a telephone codec, the high
 * end is studio; outside that a driver has almost certainly mis-read a register
 * rather than found exotic hardware. */
#define AUDIO_RATE_MIN        4000u
#define AUDIO_RATE_MAX      192000u

#define AUDIO_SEQ_MAX           32    /* notes in one audio_play_notes call */

/* ====================================================================== */
/* format                                                                 */
/* ====================================================================== */

/* Signed little-endian PCM, channel-interleaved. `bits` is 16 and only 16
 * today: it is carried rather than assumed so widening the synthesiser later
 * does not change this struct or any caller. */
typedef struct audio_format {
    uint32_t rate_hz;      /* frames per second                            */
    uint8_t  channels;     /* 1 (mono) or 2 (interleaved L,R)              */
    uint8_t  bits;         /* 16                                           */
} audio_format_t;

/* Bytes one frame occupies (channels * bits/8). 0 for a format this module
 * would refuse, so it doubles as a validity test. */
uint32_t audio_frame_bytes(const audio_format_t *f);

/* Returns 1 if this module can synthesise into `f`, else 0 with a sentence in
 * `why` naming the field that is wrong. `why` may be NULL. */
int audio_format_valid(const audio_format_t *f, char *why, size_t cap);

/* ====================================================================== */
/* waveforms — generic arithmetic, no floating point                      */
/* ====================================================================== */

/* This kernel is built -mno-sse: there is no FPU available, so every waveform
 * below is integer-only. SINE is a 65-point quarter-wave Q15 table with linear
 * interpolation between points (error under 0.05% of full scale), not a
 * polynomial approximation. */
typedef enum {
    AUDIO_WAVE_SINE = 0,       /* the default: a tone, not a buzz          */
    AUDIO_WAVE_SQUARE,         /* trivially recognisable in a captured WAV */
    AUDIO_WAVE_TRIANGLE,
    AUDIO_WAVE__COUNT
} audio_wave_t;

const char *audio_wave_name(audio_wave_t w);          /* never NULL */

/* Parse "sine"/"square"/"triangle" (exact, lower case). 0 on success, -1 if the
 * name is not one of them; *out is untouched on failure. */
int audio_wave_from_name(const char *s, audio_wave_t *out);

/* ====================================================================== */
/* synthesis — pure functions, usable with no sink registered              */
/* ====================================================================== */

/* Frames in `ms` milliseconds at this format's rate: rate * ms / 1000,
 * TRUNCATED (a 3 ms tone at 44100 Hz is 132 frames, not 132.3). Saturates at
 * 0xFFFFFFFF rather than wrapping, so a caller that forgot to bound `ms` gets an
 * absurd number it will reject instead of a small plausible one. Returns 0 for
 * an invalid format. */
uint32_t audio_frames_for_ms(const audio_format_t *f, uint32_t ms);

/* Write `ms` milliseconds of `wave` at `hz` into `dst`, in `fmt`, and return the
 * number of FRAMES written. `dst` must have room for dst_frames * channels
 * int16_t. Never writes more than dst_frames frames, so the return value is
 * min(audio_frames_for_ms(), dst_frames) and a short buffer truncates the sound
 * rather than the memory after it.
 *
 *   hz == 0        a rest: exact digital silence, and NOT an error, because a
 *                  melody needs gaps.
 *   hz >= rate/2   above Nyquist. Refused (returns 0): what a device would
 *                  actually emit there is aliasing noise, and pretending
 *                  otherwise would be the "silence that looks like success"
 *                  failure in a different costume.
 *   amplitude      clamped to AUDIO_AMP_MAX. A sine or triangle reaches exactly
 *                  +/-amplitude at its peak; nothing ever wraps to the opposite
 *                  sign, which is what clipping sounds like when it goes wrong.
 *
 * Phase is computed per frame from the frame index rather than accumulated, so a
 * one-second tone has no cumulative drift and a period that divides the sample
 * rate lands exactly on its peaks. That is what makes frequency and amplitude
 * numerically checkable in a test instead of approximately checkable. */
size_t audio_synth(int16_t *dst, size_t dst_frames, const audio_format_t *fmt,
                   uint32_t hz, uint32_t ms, uint32_t amplitude,
                   audio_wave_t wave);

/* ====================================================================== */
/* sinks                                                                  */
/* ====================================================================== */

typedef enum {
    AUDIO_SINK_NONE = 0,
    AUDIO_SINK_NATIVE,     /* a C driver in the tree      */
    AUDIO_SINK_VM          /* a model-authored dvm program */
} audio_sink_kind_t;

const char *audio_sink_kind_name(audio_sink_kind_t k);   /* never NULL */

/* A native driver's play path. `play` must not return until the device has
 * finished reading [phys, phys+bytes) — the same rule the VM contract states,
 * for the same reason: the kernel refills that memory on the next call.
 *
 * Return 0 on success, or negative on failure after writing a specific reason
 * into `why` (cap bytes, always NUL-terminated). "the device did not start" is
 * useless; "status register still 0x00 after 40 ms" is what a caller can act
 * on. `stop` may be NULL. */
typedef struct audio_ops {
    void *ctx;
    int (*play)(void *ctx, uint64_t phys, uint32_t bytes, uint32_t frames,
                uint32_t ms, char *why, size_t cap);
    int (*stop)(void *ctx);
} audio_ops_t;

/* What a VM sink is: a program, the sandbox it runs under, and the register
 * preload that identifies its device. Every field is COPIED by
 * audio_register_vm(), including the whole program image, so the caller may free
 * its own copy the moment the call returns and a stale pointer cannot become a
 * play path into freed memory. */
typedef struct audio_vm_spec {
    const dvm_program_t *program;   /* the PLAY program (see AUDIO_VM_CONTRACT) */
    dvm_policy_t         policy;    /* the sandbox, exactly as driver_run built it */
    const dvm_io_t      *io;        /* backend; NULL means dvm_io_hardware()   */
    uint64_t             bar[6];    /* r0..r5, as driver_targets reports them  */
    uint64_t             bdf;       /* r6                                      */
} audio_vm_spec_t;

/* THE REGISTER PRELOAD. audio.c writes exactly these; a doc that disagrees with
 * this enum is the doc's bug.
 *
 * r0..r8 are dvm.h's own DVM_ARG_* convention, spelled with its constants rather
 * than with numbers so the two cannot drift: a play program is entered with the
 * same register meanings a bring-up program got from driver_run, which is most of
 * what a model has to learn here already learned. Only r9..r11 are new, and each
 * is a number the program would otherwise have to derive. */
enum {
    AUDIO_REG_BAR0    = DVM_ARG_BAR0,      /* r0..r5: this device's BAR bases  */
    AUDIO_REG_BDF     = DVM_ARG_BDF,       /* r6:  bus/device/function         */
    AUDIO_REG_DMA     = DVM_ARG_DMA_BASE,  /* r7:  the DMA arena, and the PCM  */
    AUDIO_REG_DMA_SZ  = DVM_ARG_DMA_SIZE,  /* r8:  bytes granted at r7         */
    AUDIO_REG_PCM     = DVM_NARGS,         /* r9:  bytes of PCM at r7, THIS sound */
    AUDIO_REG_FRAMES  = DVM_NARGS + 1,     /* r10: frames in it                */
    AUDIO_REG_MS      = DVM_NARGS + 2,     /* r11: how long it lasts, in ms    */
    /* r12: the driver's own scratch. A FIXED address — r7 + AUDIO_PCM_BYTES —
     * and that is the whole reason it is a register of its own rather than
     * something the program computes. r9 varies with the length of the sound, so
     * a descriptor list built at r7+r9 would move every time the duration
     * changed, and the program would hand the device a stale address on the
     * second note. This one never moves. */
    AUDIO_REG_SCRATCH = DVM_NARGS + 3,
    AUDIO_NARGS       = DVM_NARGS + 4
};
_Static_assert(AUDIO_NARGS <= DVM_NREGS,
               "the play contract must fit in the VM's register file");

/* The contract, in the words the model reads. Interpolated into the audio tool
 * descriptions and printed by audio_status when nothing is registered, so the
 * text that teaches it and the code that implements it cannot drift.
 *
 * Whatever tool ends up installing a play program (that lives with the driver
 * VM's tools, not here) must interpolate THIS macro too. */
#define AUDIO_VM_CONTRACT                                                       \
    "PLAY CONTRACT. The kernel re-runs your play program from its first "        \
    "instruction every time a sound is wanted. r0-r8 mean what they mean in "    \
    "driver_run, and there are four more:\n"                                     \
    "  r0-r5  this device's BAR bases, r6 its bdf\n"                             \
    "  r7     the DMA buffer's physical address\n"                               \
    "  r8     bytes granted there\n"                                             \
    "  r9     bytes of PCM the kernel has ALREADY WRITTEN at r7, this sound\n"    \
    "  r10    frames in it\n"                                                    \
    "  r11    how long it lasts, in milliseconds\n"                              \
    "  r12    your own scratch memory, a FIXED address that never moves\n"        \
    "The samples are signed 16-bit little-endian, interleaved, at the rate and "  \
    "channel count you declared when you registered. r7 to r7+r9 is the audio "   \
    "and the kernel rewrites it for every sound; r12 to r7+r8 is YOURS and the "  \
    "kernel never writes it, so a descriptor list you build there with "          \
    "st8/st16/st32 survives from one sound to the next. Build it at r12, not at "  \
    "r7+r9 - r9 changes with the length of the sound.\n"                          \
    "Your program must make the device play r9 bytes from r7, poll until the "    \
    "device has finished reading that memory, and reach `halt`. DONE MEANS "      \
    "FINISHED: the kernel overwrites the PCM for the next sound, so halting "     \
    "while the device is still reading corrupts the next one. On any failure, "   \
    "`abort \"reason\"` - the reason goes back to whoever asked for the sound. "  \
    "This runs once per sound, so everything in it must be safe to do again; "    \
    "leave one-time bring-up in the program you ran with driver_run.\n"           \
    "HOW TO WAIT, because the budgets are real and a spin loop hits them. Every "  \
    "device read costs one access against the run's access budget, and `delay` "   \
    "spends the delay budget. For a play run the kernel raises both to fit this "  \
    "sound: the delay budget covers r11 ms plus a quarter-second of margin, and "  \
    "the access budget covers about two reads per millisecond of r11. So put a "   \
    "`delay` INSIDE the poll loop and read the status register ONCE per pass - "   \
    "roughly `delay 1000` (1 ms) per read, giving about r11 iterations. A tight "  \
    "loop with no delay, or a retry cap in the tens of thousands, will trap with "  \
    "IO_LIMIT or DELAY_LIMIT before the sound has finished. Cap the loop at a few "\
    "times r11, and if it runs out, `abort` with what the status register said - " \
    "that is a far more useful answer than halting early."

/* Register a native driver as the audio output. `name` identifies it in reports
 * and in the device tree (copied, bounded, must be non-empty and printable).
 * `dev` is the device node to bind so `driver=` tells the truth; NULL is allowed
 * for a sink with no device_t, and is reported as such rather than hidden.
 *
 * Returns AUDIO_OK, or AUDIO_EINVAL with a sentence in audio_last_error(). A
 * refused registration leaves any previous sink UNTOUCHED — it does not
 * half-replace it. */
int audio_register_native(const char *name, struct device *dev,
                          const audio_format_t *fmt, const audio_ops_t *ops);

/* The same, for a model-authored play program. In addition to the checks above,
 * the program is re-validated with dvm_program_validate() and the policy with
 * dvm_policy_check(), so a corrupt image or a sandbox the VM would refuse is
 * rejected at REGISTRATION rather than at the first sound — the model is still
 * in a position to fix it then. */
int audio_register_vm(const char *name, struct device *dev,
                      const audio_format_t *fmt, const audio_vm_spec_t *spec);

/* Drop the current sink and unbind it from the device tree. Idempotent. */
void audio_release(void);

/* ====================================================================== */
/* query                                                                  */
/* ====================================================================== */

int audio_available(void);        /* 1 if something can make a sound        */

/* Where the PCM lives: the base of vm/dvm.c's DMA arena, which on this kernel is
 * also its physical address. 0 only if the VM published no arena. */
uint64_t audio_buffer_phys(void);

/* Bytes of that arena the kernel fills with samples (AUDIO_PCM_BYTES), and the
 * size of the whole arena. The difference is the driver's own space. */
uint32_t audio_buffer_bytes(void);
uint32_t audio_region_bytes(void);

/* 1 if [base, base+size) is entirely inside the low 4 GiB, which is what a
 * 32-bit bus master can address on a machine with no IOMMU. Pure predicate, no
 * state: registration applies it to the PCM buffer, and it is exposed because a
 * C driver deciding whether it can hand an address to its device needs exactly
 * this question answered, and a second copy of the arithmetic is a second place
 * to get an off-by-one wrong. */
int audio_dma_reachable(uint64_t base, uint64_t size);

/* Longest single sound under the CURRENT sink: the smaller of AUDIO_MAX_MS and
 * what the buffer holds at the sink's rate. 0 when nothing is registered. */
uint32_t audio_max_ms(void);

/* The last failure, as a sentence. Never NULL; "" when nothing has failed since
 * the last successful call. */
const char *audio_last_error(void);

/* The sentence every play path fails with when nothing is registered. Exposed so
 * the tool layer can answer "this machine has no audio" BEFORE it parses
 * arguments — otherwise a silent machine gets told its frequency is wrong — while
 * the wording still lives in exactly one place. Never NULL. */
const char *audio_no_sink_reason(void);

typedef struct audio_status {
    int               present;
    audio_sink_kind_t kind;
    char              name[AUDIO_NAME_MAX];
    char              device[AUDIO_DEVNAME_MAX];   /* "" if no device node   */
    audio_format_t    fmt;
    uint64_t          buf_phys;
    uint32_t          buf_bytes;      /* PCM span the kernel fills            */
    uint32_t          region_bytes;   /* the whole DMA arena                  */
    uint32_t          max_ms;

    uint32_t          plays;         /* sounds that reached the device       */
    uint32_t          failures;
    uint32_t          frames_last;   /* frames in the last sound             */
    uint32_t          ms_last;

    /* VM sinks only, zeroed otherwise: what the last run of the play program
     * did. This is the ground truth behind "it played" — the model's own claim
     * and these numbers are separate things. */
    uint32_t          vm_insns;      /* instructions in the installed program */
    dvm_status_t      vm_status;
    uint32_t          vm_line;
    uint64_t          vm_steps;
    uint64_t          vm_io_ops;
    int               vm_ran;        /* 1 once the program has been run       */

    char              last_error[AUDIO_WHY_MAX];
} audio_status_t;

void audio_status(audio_status_t *out);

/* ====================================================================== */
/* playback                                                               */
/* ====================================================================== */

/* One entry in a sequence. hz == 0 is a rest. amplitude 0 means "use
 * AUDIO_AMP_DEFAULT", which is what makes a note literal `{440, 200, 0, 0}`
 * mean something sensible. */
typedef struct audio_note {
    uint32_t hz;
    uint32_t ms;
    uint32_t amplitude;
    uint8_t  wave;          /* audio_wave_t */
} audio_note_t;

/* Synthesise and play. Each returns AUDIO_OK, or one of the codes above with a
 * sentence in audio_last_error(). None of them can succeed without a sink:
 * AUDIO_ENOSINK is checked first, before any argument, so "no driver" is never
 * reported as "bad frequency". */
int audio_tone(uint32_t hz, uint32_t ms);
int audio_tone_ex(uint32_t hz, uint32_t ms, uint32_t amplitude, audio_wave_t wave);

/* A sequence, gapless within each buffer-full. Notes are ALL validated before
 * the first one sounds, so a bad note nine into a melody is an error instead of
 * eight notes followed by an error. A sequence longer than the buffer is played
 * in consecutive buffer-fulls; the seam between them is audible on hardware,
 * which is a property of having one buffer and no interrupts, not a defect. */
int audio_play_notes(const audio_note_t *notes, int n);

/* Play caller-supplied PCM, already in the sink's format, interleaved. Copied
 * into the service's buffer (the caller's memory is never handed to a device). */
int audio_play_pcm(const int16_t *frames, uint32_t nframes);

/* Ask the sink to stop. AUDIO_EINVAL if this sink has no way to (a VM sink
 * plays synchronously and has already stopped by the time anything could ask). */
int audio_stop(void);

/* ---- test hook ---- */
/* Release the sink and zero every counter and the last error. Exists for host
 * tests, which need a known state between cases; nothing in the kernel calls it. */
void audio_reset(void);

#endif /* AUDIO_H */
