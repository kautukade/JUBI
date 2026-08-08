/* ac97_bringup.h — the AC'97 bring-up program, as the kernel sees it.
 *
 * PURPOSE
 *   vm/programs/ac97_bringup.dvm is the artifact: a hand-written, known-good
 *   driver for real AC'97 audio hardware, in the driver VM's assembly. This
 *   header is that same text compiled into the kernel, plus the handful of
 *   constants the C side has to agree with it about (which register is which,
 *   which argument slot carries which value, what a descriptor entry looks
 *   like).
 *
 *   It exists to make a MODEL's failures attributable. When a model is asked to
 *   write an AC'97 driver in this ISA (tools/dvm_tools.c) and the machine stays
 *   silent, the question "can the VM drive this hardware at all?" is already
 *   answered: this program does, at boot, on every machine that has the device.
 *
 * RESPONSIBILITIES
 *   - Carry the program text (AC97_BRINGUP_SRC) with no lifetime or heap.
 *   - Pin the argument vector layout: the program hardcodes no address, so
 *     every machine-specific value arrives in a register (AC97_ARG_*).
 *   - Pin the result register layout the program leaves behind (AC97_RES_*).
 *   - Name the AC'97 registers and bits once, so the kernel caller
 *     (vm/programs/ac97_boot.c) and the synthetic codec in the host test
 *     (tests/host/test_dvm_ac97.c) cannot drift apart from the program.
 *
 * KEEPING THE TWO COPIES HONEST
 *   AC97_BRINGUP_SRC below must be byte-for-byte the contents of
 *   ac97_bringup.dvm. tests/host/test_dvm_ac97.c reads that file at runtime and
 *   fails if the two ever differ, so editing one and forgetting the other is a
 *   red test rather than a mystery. Edit the .dvm, then regenerate the block at
 *   the bottom of this header with:
 *
 *       python3 vm/programs/gen_header.py
 *
 * DEPENDENCIES
 *   None. This is a header of constants and one string; it includes nothing so
 *   that a host test and a freestanding kernel TU can both take it.
 *
 * FUTURE EXTENSION POINTS
 *   - Record (PCM in) is the mirror image of this: the same NABM box layout at
 *     offset 0x00 instead of 0x10, and the mixer's record-select/record-gain
 *     registers instead of master/PCM volume.
 *   - A second program with a `service` entry point (stop, change volume,
 *     requeue a buffer) is what turns this from a one-shot bring-up into a
 *     driver the model can keep. dvm.h already notes the entry-point hook.
 */
#ifndef AC97_BRINGUP_H
#define AC97_BRINGUP_H

/* ---- the argument vector the program is run with ---- */
#define AC97_ARG_NAM     0   /* r0: BAR0, the mixer's I/O base          */
#define AC97_ARG_NABM    1   /* r1: BAR1, the bus master's I/O base     */
#define AC97_ARG_BDL     2   /* r2: physical address of the descriptors */
#define AC97_ARG_LVI     3   /* r3: last valid descriptor index         */
#define AC97_ARG_BDF     4   /* r4: bus<<8 | dev<<3 | fn                */
#define AC97_NARGS       5

/* ---- what it leaves behind (dvm_result_t.reg[]) ---- */
#define AC97_RES_GLOB_STA  11
#define AC97_RES_MASTER    12
#define AC97_RES_PCM       13
#define AC97_RES_PICB0     14
#define AC97_RES_PICB1     15

/* ---- NAM: the codec's mixer register file (BAR0), 16-bit registers ---- */
#define AC97_NAM_RESET      0x00
#define AC97_NAM_MASTER     0x02
#define AC97_NAM_PCM_OUT    0x18
#define AC97_NAM_EXT_ID     0x28
#define AC97_NAM_EXT_CTRL   0x2a
#define AC97_NAM_DAC_RATE   0x2c
#define AC97_NAM_SIZE       0x100   /* the spec's 256-byte window */

/* ---- NABM: the bus master (BAR1); the PCM-out channel is the 0x10 box ---- */
#define AC97_PO_BDBAR       0x10
#define AC97_PO_CIV         0x14
#define AC97_PO_LVI         0x15
#define AC97_PO_SR          0x16
#define AC97_PO_PICB        0x18
#define AC97_PO_CR          0x1b
#define AC97_GLOB_CNT       0x2c
#define AC97_GLOB_STA       0x30
#define AC97_NABM_SIZE      0x40    /* the spec's 64-byte window */

/* ---- bits ---- */
#define AC97_GC_COLD        0x02        /* GLOB_CNT: release cold reset   */
#define AC97_GS_PCR         0x100       /* GLOB_STA: primary codec ready  */
#define AC97_CR_RPBM        0x01        /* PO_CR: run bus master          */
#define AC97_CR_RR          0x02        /* PO_CR: reset the channel       */
#define AC97_SR_DCH         0x01        /* PO_SR: DMA controller halted   */
#define AC97_MUTE           0x8000      /* any mixer volume register      */
#define AC97_VRA            0x0001      /* extended audio: variable rate  */
#define AC97_RATE_48K       0xbb80      /* 48000 Hz                       */
#define AC97_PCM_0DB        0x0808      /* PCM out volume, 0 dB, unmuted  */

/* ---- buffer descriptor list ----
 * 32 entries max, 8 bytes each, 8-byte aligned. Word 0 is the buffer's
 * physical address; word 1 is the length in 16-BIT SAMPLES (not frames and not
 * bytes: a stereo frame is two samples) in bits 15:0, plus these flags. */
/* (The other flag, IOC in bit 31, raises an interrupt when a buffer completes.
 * It is not defined here because it can never be used: every PIC line in this
 * kernel is masked and IF stays 0, so the program polls PICB and DCH instead.)
 *
 * BUP, per the 82801AA datasheet, table "BD Control and Length": 0 = when this
 * buffer is complete and no next buffer is ready, keep transmitting the last
 * valid sample; 1 = if this is the last valid buffer, transmit zeros after it.
 * So it belongs on the final descriptor of a stream, which is what the caller
 * does. NOTE: QEMU's hw/audio/ac97.c has these two cases the wrong way round
 * (BD_BUP -> BUP_LAST -> repeat s->last_samp), so on QEMU a finished stream
 * holds the last sample as DC until the bus master is stopped. The program
 * stops it, which is correct on both and makes the difference unobservable. */
#define AC97_BD_BUP         0x40000000u
#define AC97_BD_LEN_MASK    0x0000ffffu
#define AC97_BDL_MAX        32

/* The program text. Byte-for-byte ac97_bringup.dvm — see the note above. */
#define AC97_BRINGUP_SRC \
/* --- BEGIN GENERATED (do not edit by hand) --- */ \
    "; ===========================================================================\n" \
    "; ac97_bringup.dvm — bring up an Intel AC'97 codec, play the caller's buffer,\n" \
    ";                    and stop the bus master cleanly afterwards. Written in the\n" \
    ";                    driver VM's assembly (see include/dvm.h).\n" \
    ";\n" \
    "; This is the reference program for the driver VM: hand-written, known-good,\n" \
    "; and executed on real (emulated) hardware at boot by vm/programs/ac97_boot.c.\n" \
    "; It exists so that when a MODEL is asked to write a driver in this ISA and it\n" \
    "; does not work, the failure is attributable — the VM's ability to drive real\n" \
    "; hardware is already established by this file.\n" \
    ";\n" \
    "; SOURCE OF THE REGISTER SEMANTICS\n" \
    ";   Intel AC'97 Component Specification 2.3 (the codec/mixer side, \"NAM\")\n" \
    ";   Intel 82801AA (ICH) datasheet, the AC'97 audio controller section (the bus\n" \
    ";   master side, \"NABM\"), which is what QEMU's -device AC97 implements.\n" \
    ";\n" \
    ";   NAM  (BAR0) is the codec's mixer register file, 16-bit registers:\n" \
    ";       0x00 reset          write anything -> mixer back to power-on defaults\n" \
    ";       0x02 master volume  bit15 = mute, 13:8 = left att, 5:0 = right att\n" \
    ";       0x18 PCM out volume bit15 = mute, 0x0808 = 0 dB\n" \
    ";       0x28 extended audio ID           bit0 = VRA (variable rate) supported\n" \
    ";       0x2a extended audio ctrl/status  bit0 = VRA enable\n" \
    ";       0x2c PCM front DAC rate          Hz, writable only when VRA is enabled\n" \
    ";   NABM (BAR1) is the DMA engine. The PCM-out channel lives at 0x10:\n" \
    ";       0x10 PO_BDBAR  32-bit physical address of the buffer descriptor list\n" \
    ";       0x14 PO_CIV    current index in that list        (read only)\n" \
    ";       0x15 PO_LVI    last valid index\n" \
    ";       0x16 PO_SR     status; bit0 DCH = DMA controller halted\n" \
    ";       0x18 PO_PICB   samples left in the current buffer (read only)\n" \
    ";       0x1b PO_CR     control; bit0 RPBM = run, bit1 RR = reset the channel\n" \
    ";   and the link-wide registers at:\n" \
    ";       0x2c GLOB_CNT  bit1 = AC-link cold reset# (0 asserts, 1 releases)\n" \
    ";       0x30 GLOB_STA  bit8 = primary codec ready\n" \
    ";\n" \
    "; ARGUMENTS — the caller preloads these, so nothing here is hardcoded to one\n" \
    "; machine. The BARs come from PCI config space, read by the caller.\n" \
    ";   r0  NAM  base I/O port  (BAR0)\n" \
    ";   r1  NABM base I/O port  (BAR1)\n" \
    ";   r2  physical address of the buffer descriptor list, 8-byte aligned\n" \
    ";   r3  last valid index in that list (LVI)\n" \
    ";   r4  the controller's PCI bdf, as bus<<8 | dev<<3 | fn\n" \
    ";\n" \
    "; RESULTS — left in registers for the caller to check and print\n" \
    ";   r11 GLOB_STA once the codec reported ready\n" \
    ";   r12 master volume as it read back after unmuting\n" \
    ";   r13 PCM out volume as it read back\n" \
    ";   r14 PO_PICB at the first observation\n" \
    ";   r15 PO_PICB at the second observation; r15 != r14 proves DMA is moving\n" \
    ";\n" \
    "; WHAT THIS PROGRAM DOES NOT DO, because the ISA cannot express it:\n" \
    ";   - build the buffer descriptor list or the PCM samples. There is no writable\n" \
    ";     memory in this machine and RAM is on the VM's deny list, so the caller\n" \
    ";     supplies both and passes the list's address in r2.\n" \
    ";   - enable PCI bus mastering. pcicfg is a READ; without the caller setting\n" \
    ";     the bus-master bit first, every DMA fetch below would return garbage.\n" \
    "; ===========================================================================\n" \
    "\n" \
    "; ---- NAM (mixer) ----------------------------------------------------------\n" \
    ".equ NAM_RESET      0x00\n" \
    ".equ NAM_MASTER     0x02\n" \
    ".equ NAM_PCM_OUT    0x18\n" \
    ".equ NAM_EXT_ID     0x28\n" \
    ".equ NAM_EXT_CTRL   0x2a\n" \
    ".equ NAM_DAC_RATE   0x2c\n" \
    "\n" \
    "; ---- NABM (bus master) ----------------------------------------------------\n" \
    ".equ PO_BDBAR       0x10\n" \
    ".equ PO_LVI         0x15\n" \
    ".equ PO_SR          0x16\n" \
    ".equ PO_PICB        0x18\n" \
    ".equ PO_CR          0x1b\n" \
    ".equ GLOB_CNT       0x2c\n" \
    ".equ GLOB_STA       0x30\n" \
    "\n" \
    "; ---- bits -----------------------------------------------------------------\n" \
    ".equ GC_COLD        0x02        ; GLOB_CNT: release the AC-link cold reset\n" \
    ".equ GS_PCR         0x100       ; GLOB_STA: primary codec ready\n" \
    ".equ CR_RPBM        0x01        ; PO_CR:    run bus master\n" \
    ".equ CR_RR          0x02        ; PO_CR:    reset this channel's registers\n" \
    ".equ SR_DCH         0x01        ; PO_SR:    DMA controller halted\n" \
    ".equ MUTE           0x8000      ; every mixer volume register, bit 15\n" \
    ".equ VRA            0x0001      ; extended audio: variable rate audio\n" \
    ".equ RATE_48K       0xbb80      ; 48000, the AC'97 reference rate\n" \
    ".equ PCM_0DB        0x0808      ; PCM out volume, 0 dB, unmuted\n" \
    ".equ CLASS_AUDIO    0x0401      ; PCI class 04 subclass 01 = AC'97-era audio\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 0. Confirm what is at that PCI function before touching any of its ports.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    pcicfg r7, r4, 0x08         ; class / subclass / prog-if / revision\n" \
    "    shr    r7, 16\n" \
    "    cmp    r7, CLASS_AUDIO\n" \
    "    bne    bad_device\n" \
    "    print  \"pci class\", r7\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 1. Cold-reset the AC-link: drop GLOB_CNT bit1, wait, raise it again.\n" \
    ";    (Linux's snd_intel8x0 does exactly this before touching the codec.)\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add   r5, r1, GLOB_CNT\n" \
    "    out32 r5, 0\n" \
    "    delay 2000\n" \
    "    out32 r5, GC_COLD\n" \
    "    delay 20000\n" \
    "    in32  r7, r5\n" \
    "    print \"glob_cnt\", r7\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 2. Wait for the primary codec to report ready. This is the handshake that\n" \
    ";    says a codec is wired to the link at all; everything below assumes it.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add  r5, r1, GLOB_STA\n" \
    "    mov  r6, 40                 ; 40 ms of patience, in 1 ms steps\n" \
    "pcr_wait:\n" \
    "    in32 r11, r5\n" \
    "    test r11, GS_PCR\n" \
    "    bne  pcr_ok\n" \
    "    delay 1000\n" \
    "    sub  r6, 1\n" \
    "    cmp  r6, 0\n" \
    "    bne  pcr_wait\n" \
    "    abort \"primary codec never reported ready\"\n" \
    "pcr_ok:\n" \
    "    print \"glob_sta\", r11\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 3. Reset the mixer to its power-on defaults. Master and PCM come back MUTED\n" \
    ";    (0x8000 / 0x8808 per the spec), which is why step 4 and 5 exist.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add   r5, r0, NAM_RESET\n" \
    "    out16 r5, 0\n" \
    "    delay 1000\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 4. Prove a codec is really answering: the mute bit must stick when set, and\n" \
    ";    must clear when cleared. A register file that is not there reads back as\n" \
    ";    all-ones or all-zeroes and fails one of the two.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add   r5, r0, NAM_MASTER\n" \
    "    out16 r5, MUTE\n" \
    "    in16  r7, r5\n" \
    "    test  r7, MUTE\n" \
    "    beq   no_codec\n" \
    "    out16 r5, 0                 ; unmute, 0 dB attenuation, both channels\n" \
    "    in16  r12, r5\n" \
    "    test  r12, MUTE\n" \
    "    bne   still_muted\n" \
    "    print \"master vol\", r12\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 5. Unmute the PCM mixer path at 0 dB. Master alone is not enough: the PCM\n" \
    ";    stream is attenuated separately and also comes out of reset muted.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add   r5, r0, NAM_PCM_OUT\n" \
    "    out16 r5, PCM_0DB\n" \
    "    in16  r13, r5\n" \
    "    test  r13, MUTE\n" \
    "    bne   pcm_muted\n" \
    "    print \"pcm vol\", r13\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 6. Sample rate. Only a VRA-capable codec can be asked for one; a codec\n" \
    ";    without VRA is fixed at 48 kHz, which is the rate the caller generated.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add  r5, r0, NAM_EXT_ID\n" \
    "    in16 r7, r5\n" \
    "    test r7, VRA\n" \
    "    beq  rate_fixed\n" \
    "    add   r5, r0, NAM_EXT_CTRL\n" \
    "    in16  r8, r5\n" \
    "    or    r8, VRA\n" \
    "    out16 r5, r8\n" \
    "    add   r5, r0, NAM_DAC_RATE\n" \
    "    out16 r5, RATE_48K\n" \
    "    in16  r8, r5\n" \
    "    cmp   r8, RATE_48K\n" \
    "    bne   rate_bad\n" \
    "    print \"dac rate\", r8\n" \
    "    jmp   rate_done\n" \
    "rate_fixed:\n" \
    "    print \"no vra: fixed 48k\"\n" \
    "rate_done:\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 7. Reset the PCM-out DMA channel. RR clears BDBAR/LVI/CIV/PICB and is\n" \
    ";    self-clearing, so it doubles as a liveness check on the bus master.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add  r5, r1, PO_CR\n" \
    "    out8 r5, CR_RR\n" \
    "    mov  r6, 20\n" \
    "rr_wait:\n" \
    "    in8  r7, r5\n" \
    "    test r7, CR_RR\n" \
    "    beq  rr_done\n" \
    "    delay 1000\n" \
    "    sub  r6, 1\n" \
    "    cmp  r6, 0\n" \
    "    bne  rr_wait\n" \
    "    abort \"PCM-out reset bit never cleared\"\n" \
    "rr_done:\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 8. Hand it the descriptor list. Both registers are read back: this is the\n" \
    ";    cheapest proof that the BAR really is the bus master's register file.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add   r5, r1, PO_BDBAR\n" \
    "    out32 r5, r2\n" \
    "    in32  r7, r5\n" \
    "    cmp   r7, r2\n" \
    "    bne   bdbar_bad\n" \
    "    print \"bdbar\", r7\n" \
    "\n" \
    "    add  r5, r1, PO_LVI\n" \
    "    out8 r5, r3\n" \
    "    in8  r7, r5\n" \
    "    cmp  r7, r3\n" \
    "    bne  lvi_bad\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 9. Run. From here the codec is pulling our samples over the PCI bus.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add  r5, r1, PO_CR\n" \
    "    out8 r5, CR_RPBM\n" \
    "    delay 5000\n" \
    "    add  r5, r1, PO_SR\n" \
    "    in16 r7, r5\n" \
    "    test r7, SR_DCH\n" \
    "    bne  dma_halted\n" \
    "    print \"po_sr\", r7\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 10. Prove it. PICB counts down through the current buffer as the codec\n" \
    ";     consumes it, so two different readings mean audio is really moving —\n" \
    ";     not merely that the registers accepted what we wrote.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add  r5, r1, PO_PICB\n" \
    "    in16 r14, r5\n" \
    "    mov  r6, 20                 ; up to 100 ms; one QEMU audio tick is ~10 ms\n" \
    "picb_wait:\n" \
    "    delay 5000\n" \
    "    in16 r15, r5\n" \
    "    cmp  r15, r14\n" \
    "    bne  picb_moved\n" \
    "    sub  r6, 1\n" \
    "    cmp  r6, 0\n" \
    "    bne  picb_wait\n" \
    "    abort \"dma is running but picb never moved\"\n" \
    "picb_moved:\n" \
    "    print \"picb was\", r14\n" \
    "    print \"picb now\", r15\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; 11. Let the list drain, then stop the bus master. A machine with no\n" \
    ";     interrupts has to poll for the end, and leaving the engine running past\n" \
    ";     the last buffer is what makes hardware click or drone: the controller\n" \
    ";     keeps feeding the codec whatever the underrun policy says.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "    add  r5, r1, PO_SR\n" \
    "    mov  r6, 60                 ; up to 1.5 s for a buffer of well under 1 s\n" \
    "drain_wait:\n" \
    "    in16 r7, r5\n" \
    "    test r7, SR_DCH\n" \
    "    bne  drained\n" \
    "    delay 25000\n" \
    "    sub  r6, 1\n" \
    "    cmp  r6, 0\n" \
    "    bne  drain_wait\n" \
    "    abort \"playback never finished: no DCH\"\n" \
    "drained:\n" \
    "    print \"drained po_sr\", r7\n" \
    "    add  r5, r1, PO_CR\n" \
    "    out8 r5, 0                  ; clear RPBM: the bus master is idle again\n" \
    "    in8  r7, r5\n" \
    "    print \"stopped po_cr\", r7\n" \
    "    halt\n" \
    "\n" \
    "; ---------------------------------------------------------------------------\n" \
    "; Failure exits. Every one of these names the step that failed, because the\n" \
    "; whole point of running a driver in a VM is that a broken bring-up leaves a\n" \
    "; readable transcript instead of a dead machine.\n" \
    "; ---------------------------------------------------------------------------\n" \
    "bad_device:\n" \
    "    abort \"not a PCI audio-class function\"\n" \
    "no_codec:\n" \
    "    abort \"master mute bit did not stick\"\n" \
    "still_muted:\n" \
    "    abort \"master volume stayed muted\"\n" \
    "pcm_muted:\n" \
    "    abort \"pcm out volume stayed muted\"\n" \
    "rate_bad:\n" \
    "    abort \"codec refused the 48 kHz rate\"\n" \
    "bdbar_bad:\n" \
    "    abort \"bdbar did not read back\"\n" \
    "lvi_bad:\n" \
    "    abort \"lvi did not read back\"\n" \
    "dma_halted:\n" \
    "    abort \"dma halted immediately after start\"\n"
/* --- END GENERATED --- */

#endif /* AC97_BRINGUP_H */
