/* ac97_boot.c — TEST FIXTURE: run the reference AC'97 driver program on real
 *               hardware. NOT PART OF ANY DEFAULT BUILD.
 *
 * WHY THIS FILE LIVES UNDER tests/ AND BEHIND A FLAG
 *   fable-os must be genuinely ignorant of the sound card an operator attaches.
 *   That is the whole experiment: hand the model an unclaimed audio device and
 *   ask it to write the driver. A kernel that ships a hand-written AC'97 driver
 *   cannot run that experiment and cannot honestly claim the ignorance — the
 *   device would already be claimed and the codec already initialised before
 *   anyone typed a sentence.
 *
 *   So this file is a fixture, in two independent ways:
 *     1. it is not in vm/ or drivers/ any more, it is under tests/qemu/, so no
 *        source list in the kernel build even mentions the path; and
 *     2. Makefile adds it to KERNEL_SRCS only when EXTRA_CFLAGS contains
 *        -DFABLEOS_AC97_REFERENCE, and the #error below refuses to compile it
 *        without that define, so it cannot arrive in an image by accident.
 *
 *   The default kernel therefore contains no audio driver at all: an attached
 *   AC'97 (or HDA, or anything else of PCI class 04) is enumerated, published as
 *   an unclaimed device with its BARs, and left completely untouched.
 *
 *   tests/qemu/cases/ac97.case is the only thing that builds this, via its
 *   `build-cflags:` line; tests/qemu/cases/audio-unclaimed.case asserts the
 *   opposite for the default build. Nothing else should ever define the flag.
 *
 *       make EXTRA_CFLAGS=-DFABLEOS_AC97_REFERENCE
 *       make run EXTRA_CFLAGS=-DFABLEOS_AC97_REFERENCE \
 *                QEMU_EXTRA="-device AC97,audiodev=a0 -audiodev coreaudio,id=a0"
 *
 *   (Both on ONE command line: EXTRA_CFLAGS is recorded in .build-flags, so
 *   passing it to only one of the two invocations silently boots the other
 *   kernel.)
 *
 * PURPOSE
 *   The driver VM (include/dvm.h) is a machine whose opcodes are hardware
 *   primitives, built so a model can write a driver that cannot crash the
 *   kernel. Everything about it was verified against a synthetic device on the
 *   host. This file is the part that could not be: it takes the hand-written
 *   reference program in vm/programs/ac97_bringup.dvm, points it at an AC'97
 *   audio controller that no kernel driver has claimed, and lets it play sound.
 *
 *   That matters for one reason. A MODEL can write and run a driver in this ISA
 *   (tools/dvm_tools.c: driver_assemble, driver_run). When one of those fails,
 *   the failure has to be attributable — a bad program, not a VM that was never
 *   able to drive hardware in the first place. This fixture removes the second
 *   possibility: in the fixture build, on a machine that has the device, the ISA
 *   demonstrably drives real silicon end to end.
 *
 * WHAT IS AND IS NOT THE DRIVER
 *   The driver is the .dvm program. It does the whole bring-up: the AC-link
 *   cold reset, the codec-ready handshake, the mixer reset, unmuting master and
 *   PCM, the sample rate, resetting the DMA channel, loading the descriptor
 *   list, starting the bus master, and confirming the position counter really
 *   moves. Not one of those port accesses is issued by C.
 *
 *   This file is the caller, and it does exactly the four things the ISA cannot
 *   express, all of them deliberately outside the sandbox:
 *     1. find the device and read its BARs (the VM may read config space, but
 *        it must be told which function, and the policy has to exist first);
 *     2. set the PCI bus-master bit — `pcicfg` is a read-only opcode;
 *     3. produce the PCM samples and the buffer descriptor list, because the VM
 *        has no writable memory and RAM is on its deny list;
 *     4. build the policy: exactly the two BARs this device owns, no MMIO at
 *        all, and one PCI function. The program cannot widen it.
 *
 * WHY IT RUNS AT BOOT AND NOT FROM A PROMPT
 *   Natural language is this kernel's only interface, and reaching the model
 *   needs an API key. A bring-up that only happens when someone types a
 *   sentence cannot be asserted on by tests/qemu, so the proof would rot. In the
 *   fixture build it therefore runs as an ordinary DRV_LEVEL_LATE driver init:
 *   present hardware gets driven, absent hardware gets one line saying so, and
 *   nothing about either path can fail the boot. That is exactly why the flag
 *   exists rather than an `#if 0` — a boot-time driver is assertable, and a
 *   boot-time driver is also the thing that must not ship.
 *
 * THE SOUND
 *   Half a second of stereo 16-bit PCM at 48 kHz: 250 ms of A4 (440 Hz) then
 *   250 ms of E5 (660 Hz), as square waves, split across four descriptors so
 *   the DMA engine has to walk the list rather than play one buffer. Two
 *   distinct tones make the QEMU-captured WAV objectively identifiable as this
 *   kernel's output rather than as noise. The buffer is static because the
 *   codec keeps DMA-ing out of it after dvm_run() has returned: freeing it
 *   would hand live DMA memory back to the heap.
 *
 * DEPENDENCIES
 *   dvm.h (the VM), pci.h (find the device, read BARs, enable bus mastering),
 *   device.h/driver.h (claim the device so the tree shows who owns it),
 *   kernel.h (kmalloc for the 12 KB program image, console). No interrupts: the
 *   program polls, like everything else in this kernel.
 *
 * FUTURE EXTENSION POINTS
 *   - The model runs its own programs through tools/dvm_tools.c, so this is the
 *     worked example rather than the only caller. An ac97_play_tone() split out
 *     of bring_up() is the natural shape for a "play something" tool.
 *   - Stopping playback, changing volume and requeueing buffers are all more
 *     .dvm programs against the same policy; none of them needs new C.
 *   - Nothing here is AC'97-specific except the program text and the two BAR
 *     sizes. A second device is a second .dvm file plus a match rule.
 *   - The real successor to this file is not more C: it is the model writing
 *     this same bring-up itself through driver_assemble/driver_run. When that
 *     works, this fixture stays as the control — the thing that says the VM was
 *     capable on this machine, so a model's failure is the model's.
 */

/* Belt and braces with the Makefile's source-list gate. If this file is ever
 * added to a build without the flag, the build stops here rather than quietly
 * putting an audio driver into a kernel that is supposed to have none. */
#ifndef FABLEOS_AC97_REFERENCE
#error "tests/qemu/fixtures/ac97_boot.c is a TEST FIXTURE: it is only built with EXTRA_CFLAGS=-DFABLEOS_AC97_REFERENCE (see tests/qemu/cases/ac97.case). A default kernel must contain no audio driver."
#endif

#include "kernel.h"
#include "driver.h"
#include "device.h"
#include "pci.h"
#include "dvm.h"
/* The program itself stays in vm/programs/ beside the .dvm it is generated
 * from, because tests/host/test_dvm_ac97.c runs it against a synthetic codec
 * and includes it by the same relative path. A .dvm program and its generated
 * C-string header are data for the VM: nothing links them into a kernel unless
 * a caller like this one is compiled, and no default build compiles one. */
#include "../../../vm/programs/ac97_bringup.h"

/* ---- the tone ---------------------------------------------------------- */

#define TONE_RATE_HZ   48000u          /* what the program asks the codec for */
#define TONE_MS        500u
#define TONE_FRAMES    (TONE_RATE_HZ * TONE_MS / 1000u)   /* stereo frames    */
#define TONE_AMPLITUDE 8000            /* ~24% of full scale: loud, not harsh */
#define NOTE_A4_HZ     440u
#define NOTE_E5_HZ     660u
#define BDL_ENTRIES    4u
#define FRAMES_PER_BD  (TONE_FRAMES / BDL_ENTRIES)

/* Both of these hold for the constants above and would break silently if they
 * were changed: the descriptor length field is 16 bits, and an uneven split
 * would drop the tail of the tone with no diagnostic anywhere. A wrong-length
 * descriptor presents as "the audio sounds odd" on a machine with no debugger,
 * so it is caught at build time instead. */
_Static_assert(FRAMES_PER_BD * 2u <= AC97_BD_LEN_MASK,
               "the buffer-descriptor length field counts 16-bit samples in 16 bits");
_Static_assert(TONE_FRAMES % BDL_ENTRIES == 0,
               "the tone must divide evenly across the descriptor list");

/* Both of these are read by the codec's DMA engine long after this file has
 * returned, so neither may ever live on the heap or the stack. 8-byte alignment
 * is architectural for the descriptor list; the samples only need 2. */
static int16_t  tone[TONE_FRAMES * 2] __attribute__((aligned(64)));
static uint32_t bdl[BDL_ENTRIES * 2]  __attribute__((aligned(8)));

/* A square wave, because it is trivially recognisable in a captured WAV and
 * needs no floating point (this kernel is built -mno-sse). `phase` counts
 * half-cycles elapsed, so its low bit is the sign of the current sample. */
static void build_tone(void) {
    for (uint32_t i = 0; i < TONE_FRAMES; i++) {
        uint32_t hz    = (i < TONE_FRAMES / 2) ? NOTE_A4_HZ : NOTE_E5_HZ;
        uint32_t phase = (i * 2u * hz) / TONE_RATE_HZ;
        int16_t  v     = (phase & 1u) ? (int16_t)TONE_AMPLITUDE
                                      : (int16_t)-TONE_AMPLITUDE;
        tone[i * 2 + 0] = v;            /* left  */
        tone[i * 2 + 1] = v;            /* right */
    }
}

/* One descriptor per quarter of the tone. The length field counts 16-bit
 * SAMPLES, so a stereo frame costs two. BUP goes on the last entry, which the
 * datasheet defines as "transmit zeros after this buffer" (see ac97_bringup.h
 * for the exact wording and for QEMU's inversion of it). */
static void build_bdl(void) {
    for (uint32_t e = 0; e < BDL_ENTRIES; e++) {
        uint32_t ctl = (FRAMES_PER_BD * 2u) & AC97_BD_LEN_MASK;
        if (e == BDL_ENTRIES - 1) ctl |= AC97_BD_BUP;
        bdl[e * 2 + 0] = (uint32_t)(uintptr_t)&tone[e * FRAMES_PER_BD * 2];
        bdl[e * 2 + 1] = ctl;
    }
}

/* ---- claiming the device ------------------------------------------------ */

static const driver_t ac97_vm_driver;

/* The PCI scan already published this controller as an unclaimed audio device
 * (`pci00:04.0 audio registered driver=-`). Find that node by its I/O base and
 * record who ended up owning it, so the device tree tells the truth either way:
 * active when the VM program brought it up, failed when it did not. */
static void claim(uint16_t nam, device_state_t state) {
    for (int i = 0;; i++) {
        device_t *d = device_find_by_class(DEV_CLASS_AUDIO, i);
        if (!d) return;
        for (int r = 0; r < d->nres; r++) {
            if (d->res[r].type != RES_IOPORT || d->res[r].base != nam) continue;
            device_bind_driver(d, (driver_t *)&ac97_vm_driver);
            device_set_state(d, state);
            return;
        }
    }
}

/* ---- the run ------------------------------------------------------------ */

/* Assemble and execute the reference program against this controller. Returns
 * DVM_OK when the codec is playing. Every other outcome is reported and
 * survivable: this is a self-test, not a boot dependency. */
static dvm_status_t bring_up(const pci_dev_t *d, uint16_t nam, uint16_t nabm) {
    dvm_program_t *prog = (dvm_program_t *)kmalloc(sizeof *prog);
    if (!prog) {
        kputs("ac97: no memory for the program image\n");
        return DVM_TRAP_POLICY;
    }

    dvm_asm_err_t aerr;
    if (dvm_assemble(AC97_BRINGUP_SRC, strlen(AC97_BRINGUP_SRC), prog, &aerr) != 0) {
        kprintf("ac97: the reference program does not assemble: line %d: %s\n",
                aerr.line, aerr.msg);
        kfree(prog);
        return DVM_TRAP_BADOP;
    }

    /* The security boundary. Two port windows the size the AC'97 spec says
     * those BARs are, one PCI function, and no MMIO whatsoever — the trace
     * prints all three before the first instruction runs.
     *
     * Every return is checked. dvm_policy_allow_*() leaves the policy UNCHANGED
     * when it refuses (dvm.h is explicit that a refusal never silently opens
     * something smaller), so an unchecked failure here would surface later as
     * "out8: port 0x... is not in this program's allowed ranges" — a message
     * that blames the program for a caller's bug.
     *
     * WHAT THE POLICY DOES NOT COVER: the VM's allowlist governs what the CPU
     * does, and this device is a bus master (see pci_enable_bus_master below),
     * so the codec's own DMA is outside it. Granting the whole 64-byte NABM
     * window therefore also grants the PCM-IN channel box at +0x00, whose BDBAR
     * could point anywhere in the low 4 GiB. That is safe only because the
     * program run here is the hand-written reference; tools/dvm_tools.c refuses
     * to point a model-authored program at a device that is already
     * bus-mastering, for exactly this reason. */
    dvm_policy_t pol;
    dvm_policy_init(&pol);
    if (dvm_policy_allow_ports(&pol, nam,  (uint16_t)(nam  + AC97_NAM_SIZE  - 1)) != 0 ||
        dvm_policy_allow_ports(&pol, nabm, (uint16_t)(nabm + AC97_NABM_SIZE - 1)) != 0 ||
        dvm_policy_allow_pci(&pol, d->bus, d->dev, d->func) != 0) {
        kputs("ac97: could not build the sandbox policy for this device\n");
        kfree(prog);
        return DVM_TRAP_POLICY;
    }
    /* The program polls the whole tone out and then stops the bus master, so
     * its delay budget has to cover the audio itself: 40 ms for the codec, 20
     * for the DMA reset, 100 watching the position counter, and up to 1.5 s
     * draining a buffer that is only TONE_MS long. The VM's own ceiling is 2 s.
     * Budget, not sleep: the run costs about TONE_MS in practice. */
    pol.max_delay_us = 1800000;

    const uint64_t args[AC97_NARGS] = {
        [AC97_ARG_NAM]  = nam,
        [AC97_ARG_NABM] = nabm,
        [AC97_ARG_BDL]  = (uint64_t)(uintptr_t)bdl,
        [AC97_ARG_LVI]  = BDL_ENTRIES - 1,
        [AC97_ARG_BDF]  = (uint64_t)(((uint32_t)d->bus << 8) |
                                     ((uint32_t)d->dev << 3) | d->func),
    };

    dvm_result_t res;
    dvm_status_t st = dvm_run(prog, &pol, dvm_io_hardware(), args, AC97_NARGS, &res);

    if (st == DVM_OK) {
        /* Past tense on purpose: the program does not merely start playback,
         * it polls the descriptor list out to the end and stops the bus
         * master, so by the time this prints the tone has been and gone. */
        kprintf("ac97: played %u ms of PCM through the driver VM "
                "(%u instructions, %u device accesses, %u us of waiting)\n",
                (unsigned)TONE_MS, (unsigned)res.steps, (unsigned)res.io_ops,
                (unsigned)res.delay_us);
        kprintf("ac97: codec says glob_sta=0x%x master=0x%x pcm=0x%x, "
                "picb %u -> %u samples\n",
                (unsigned)res.reg[AC97_RES_GLOB_STA],
                (unsigned)res.reg[AC97_RES_MASTER],
                (unsigned)res.reg[AC97_RES_PCM],
                (unsigned)res.reg[AC97_RES_PICB0],
                (unsigned)res.reg[AC97_RES_PICB1]);
    } else {
        kprintf("ac97: bring-up stopped: %s at .dvm line %u after %u accesses\n",
                dvm_status_name(st), (unsigned)res.line, (unsigned)res.io_ops);
        kprintf("ac97: %s\n", res.msg);
    }

    kfree(prog);          /* the VM keeps nothing: the run is over */
    return st;
}

/* ---- driver entry ------------------------------------------------------- */

/* Never returns non-zero. A missing or uncooperative sound card is not a boot
 * failure, and "driver ac97vm: init failed" in the log of every machine without
 * an AC'97 would be a lie about severity. */
static int ac97_init(void) {
    pci_dev_t d;

    /* THE ONLY WAY TO REACH THIS CODE is a build that asked for it by name.
     * There used to be a -DFABLEOS_AC97_UNCLAIMED escape hatch here that turned
     * the bring-up OFF, which had the polarity backwards: the shipped kernel
     * drove the card and an opt-in flag made it behave. The gate is now the
     * other way round and lives in the Makefile, so the shipped kernel does not
     * contain this function at all. */

    /* Class 04:01 is the AC'97-era "multimedia audio" code (HD Audio is 04:03),
     * which is what QEMU's -device AC97 and every ICH southbridge report. The
     * program re-reads the class itself through pcicfg before it touches a
     * port, so a lookalike device is refused by the driver, not by this scan. */
    if (!pci_find_class(0x04, 0x01, &d)) {
        kputs("ac97: no AC'97-class audio controller found; "
              "skipping the driver-VM audio bring-up\n");
        return 0;
    }

    uint32_t raw0 = pci_read32(&d, 0x10), raw1 = pci_read32(&d, 0x14);
    kprintf("ac97: %02x:%02x.%x  %04x:%04x  bar0=0x%x bar1=0x%x\n",
            (unsigned)d.bus, (unsigned)d.dev, (unsigned)d.func,
            (unsigned)d.vendor, (unsigned)d.device,
            (unsigned)raw0, (unsigned)raw1);

    /* ICH-generation AC'97 puts both register files in I/O space. ICH4 and
     * later add MMIO aliases in BAR2/BAR3; this program drives the I/O pair, so
     * say so plainly rather than poking whatever is in the BAR. */
    if (!(raw0 & 1) || !(raw1 & 1)) {
        kputs("ac97: BAR0/BAR1 are not I/O BARs - this looks like an MMIO-only "
              "controller, which the reference program does not drive\n");
        return 0;
    }
    uint32_t base0 = raw0 & ~3u, base1 = raw1 & ~3u;
    if (!base0 || !base1 ||
        base0 + AC97_NAM_SIZE  - 1 > 0xFFFFu ||
        base1 + AC97_NABM_SIZE - 1 > 0xFFFFu) {
        kputs("ac97: the BARs are unassigned or out of I/O space\n");
        return 0;
    }
    uint16_t nam = (uint16_t)base0, nabm = (uint16_t)base1;

    /* The descriptor list and every buffer in it are fetched by the device over
     * PCI, so they have to be reachable by a 32-bit bus master, and the device
     * has to be allowed to master at all. Neither is expressible in the ISA. */
    uint64_t bdl_pa  = (uint64_t)(uintptr_t)bdl;
    uint64_t tone_pa = (uint64_t)(uintptr_t)tone;
    if (bdl_pa + sizeof bdl > 0x100000000ull ||
        tone_pa + sizeof tone > 0x100000000ull) {
        kputs("ac97: the DMA buffers are above 4 GiB\n");
        return 0;
    }

    build_tone();
    build_bdl();
    pci_enable_bus_master(&d);

    dvm_status_t st = bring_up(&d, nam, nabm);
    claim(nam, st == DVM_OK ? DEV_STATE_ACTIVE : DEV_STATE_FAILED);
    return 0;
}

static const driver_t ac97_vm_driver = {
    .name  = "ac97vm",
    .level = DRV_LEVEL_LATE,      /* after PCI and after every real driver */
    .cls   = DEV_CLASS_AUDIO,
    .init  = ac97_init,
};
REGISTER_DRIVER(ac97_vm_driver);
