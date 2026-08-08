/* test_dvm_ac97.c — host tests for the reference AC'97 driver program.
 *
 * WHAT THIS FILE IS FOR
 *   vm/programs/ac97_bringup.dvm is a driver written in the driver VM's
 *   assembly, and it is the evidence that the VM can drive real hardware at
 *   all. Evidence rots. This suite is what stops it: a synthetic AC'97
 *   controller — mixer register file, bus-master registers, a descriptor list
 *   walker and a DMA engine that consumes samples as time passes — with the
 *   real program run against it through the real VM.
 *
 *   tests/host/test_dvm.c proves the VM executes each opcode correctly. This
 *   proves one particular program is a correct AC'97 driver: that it performs
 *   the bring-up in the order the datasheet requires, that it stays inside the
 *   two register windows the device owns, that every one of its twelve failure
 *   exits fires on the hardware fault it names, and that when it succeeds the
 *   codec is unmuted, running at 48 kHz, has played every buffer in the list,
 *   and has been left with its bus master stopped.
 *
 * WHAT IT CANNOT PROVE, and what does
 *   That the ports actually reach a codec. Only hardware does that, so it is
 *   proved by booting: tests/qemu/cases/ac97.case asserts on the same trace
 *   this suite asserts on, and vm/programs/capture_audio.sh records the audio
 *   QEMU's AC97 produced and measures it.
 *
 * THE LOAD-BEARING ASSERTIONS
 *   1. `escaped` stays zero: the synthetic device counts every access outside
 *      the NAM and NABM windows it models. A driver that pokes a neighbouring
 *      device's BAR would be caught here even though the VM allowed it.
 *   2. The write sequence is golden: exactly which register got exactly which
 *      value, in order. That is the driver's whole observable effect.
 *   3. The .dvm file on disk and the copy compiled into the kernel are byte
 *      identical, so the artifact and the thing that runs cannot diverge.
 */

#include "harness.h"
#include "dvm.h"
#include "trace.h"
#include "../../vm/programs/ac97_bringup.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* the synthetic AC'97 controller                                         */
/* ====================================================================== */

/* Deliberately NOT the addresses QEMU hands out (0xc000/0xc400): the program
 * takes both bases as arguments and must not have learned either of them. The
 * device can also be relocated at runtime — see test_bases_come_from_args. */
#define NAM_BASE   0x1200u
#define NABM_BASE  0x1400u
#define BDL_ADDR   0x00200000u        /* "physical" address of the list      */
#define TONE_ADDR  0x00210000u        /* "physical" address of the samples   */
#define BDF        ((0u << 8) | (6u << 3) | 0u)   /* 00:06.0                 */
#define BDL_N      4u                 /* descriptors the caller queues       */
#define BD_SAMPLES 12000u             /* 16-bit samples per descriptor       */
#define LVI        (BDL_N - 1u)

/* PCI config space of the modelled function: only the class dword matters. */
#define CFG_CLASS  0x04010001u        /* class 04 subclass 01 prog-if 00 rev 1 */

/* ---- fault injection: one switch per failure exit in the program ---- */
static struct {
    int wrong_class;      /* something else is at that PCI function      */
    int never_ready;      /* GLOB_STA never reports the primary codec    */
    int dead_mixer;       /* mixer reads back as zeroes                  */
    int stuck_muted;      /* master mute bit refuses to clear            */
    int pcm_stuck_muted;  /* PCM out mute bit refuses to clear           */
    int rate_rejected;    /* DAC rate register ignores writes            */
    int rr_stuck;         /* PO_CR reset bit never self-clears           */
    int bdbar_readonly;   /* BDBAR ignores writes                        */
    int lvi_readonly;     /* LVI ignores writes                          */
    int halt_at_start;    /* DCH still set after the run bit goes in     */
    int picb_frozen;      /* engine "runs" but the position never moves  */
    int never_drains;     /* engine loops forever, DCH is never set      */
    int random_reads;     /* every register reads back as bus noise      */
} fault;

/* ---- device state ---- */
static uint16_t nam_base  = NAM_BASE;    /* where this device is decoded... */
static uint16_t nabm_base = NABM_BASE;   /* ...so the program can be moved  */
static uint16_t nam[AC97_NAM_SIZE / 2];
static uint32_t glob_cnt;
static int      pcr_reads;            /* reads before the codec answers */
static uint32_t po_bdbar;
static uint8_t  po_cr, po_civ, po_piv, po_lvi;
static uint16_t po_sr, po_picb;
static uint32_t bdl[AC97_BDL_MAX * 2];   /* the list, in "guest memory" */
static uint64_t samples_played;
static int      buffers_done;
static int      bd_fetches;
static uint32_t last_bd_addr;

/* ---- observation ---- */
typedef enum { A_WR, A_RD, A_PCI, A_DLY } akind_t;
typedef struct { akind_t k; uint32_t off; uint8_t w; uint32_t val; int nabm; } acc_t;

#define ALOG_MAX 512
static acc_t   alog[ALOG_MAX];
static int     nalog;
static long    escaped;               /* MUST be 0 for the whole suite */
static long    accesses;
static uint64_t delay_total;

static void log_acc(akind_t k, int nabm, uint32_t off, uint8_t w, uint32_t val) {
    accesses++;
    if (nalog < ALOG_MAX) {
        alog[nalog].k = k; alog[nalog].nabm = nabm; alog[nalog].off = off;
        alog[nalog].w = w; alog[nalog].val = val;
        nalog++;
    }
}

/* Power-on defaults, straight out of the AC'97 spec: everything muted. */
static void mixer_reset(void) {
    memset(nam, 0, sizeof nam);
    nam[AC97_NAM_MASTER   / 2] = 0x8000;
    nam[AC97_NAM_PCM_OUT  / 2] = 0x8808;
    nam[AC97_NAM_EXT_ID   / 2] = 0x0809;   /* VRA supported */
    nam[AC97_NAM_EXT_CTRL / 2] = 0x0009;   /* VRA already enabled */
    nam[AC97_NAM_DAC_RATE / 2] = AC97_RATE_48K;
    nam[0x7c / 2] = 0x8384;                /* vendor id, for realism */
}

static void bm_reset(void) {
    po_bdbar = 0; po_civ = 0; po_piv = 0; po_lvi = 0;
    po_picb = 0; po_sr = AC97_SR_DCH; po_cr = 0;
}

static void dev_reset(void) {
    nam_base = NAM_BASE;
    nabm_base = NABM_BASE;
    memset(&fault, 0, sizeof fault);
    memset(alog, 0, sizeof alog);
    nalog = 0;
    delay_total = 0;
    samples_played = 0;
    buffers_done = 0;
    bd_fetches = 0;
    last_bd_addr = 0;
    pcr_reads = 2;                  /* the codec answers on the third read */
    glob_cnt = 0;
    mixer_reset();
    bm_reset();

    /* What vm/programs/ac97_boot.c builds in RAM before it runs the program. */
    memset(bdl, 0, sizeof bdl);
    for (uint32_t e = 0; e < BDL_N; e++) {
        bdl[e * 2 + 0] = TONE_ADDR + e * BD_SAMPLES * 2u;
        bdl[e * 2 + 1] = BD_SAMPLES | (e == BDL_N - 1 ? AC97_BD_BUP : 0u);
    }
}

/* The bus master reads the descriptor over PCI. Modelled, because a driver
 * that writes the wrong BDBAR must not be able to play anything. */
static void fetch_bd(void) {
    if (po_bdbar != BDL_ADDR) { po_sr |= AC97_SR_DCH; return; }
    bd_fetches++;
    last_bd_addr = bdl[po_civ * 2 + 0];
    po_picb = (uint16_t)(bdl[po_civ * 2 + 1] & AC97_BD_LEN_MASK);
}

/* Time passes only inside DELAY, which makes playback deterministic: 48 kHz
 * stereo is 96 16-bit samples per millisecond. */
static void engine_advance(uint32_t us) {
    if (!(po_cr & AC97_CR_RPBM) || (po_sr & AC97_SR_DCH)) return;
    if (fault.picb_frozen) return;
    uint64_t samples = (uint64_t)us * 96u / 1000u;
    while (samples && !(po_sr & AC97_SR_DCH)) {
        uint32_t take = samples < po_picb ? (uint32_t)samples : po_picb;
        po_picb = (uint16_t)(po_picb - take);
        samples -= take;
        samples_played += take;
        if (po_picb) break;                     /* ran out of time, not data */
        buffers_done++;
        if (po_civ == po_lvi && !fault.never_drains) {
            po_sr |= AC97_SR_DCH;               /* the list is finished */
            break;
        }
        po_civ = fault.never_drains && po_civ == po_lvi ? 0 : po_piv;
        po_piv = (uint8_t)((po_civ + 1u) % AC97_BDL_MAX);
        fetch_bd();
    }
}

static uint32_t prng(void);      /* bus noise, for the flaky-codec runs */

/* ---- the dvm_io_t backend ---- */

static void io_pw(void *ctx, uint16_t port, uint8_t w, uint32_t val) {
    (void)ctx;
    if (port >= nam_base && port + w <= nam_base + AC97_NAM_SIZE) {
        uint32_t off = port - nam_base;
        log_acc(A_WR, 0, off, w, val);
        switch (off) {
        case AC97_NAM_RESET:   mixer_reset(); break;
        case AC97_NAM_MASTER:
            nam[off / 2] = (uint16_t)((val & 0xbf3fu) |
                                      (fault.stuck_muted ? AC97_MUTE : 0u));
            break;
        case AC97_NAM_PCM_OUT:
            nam[off / 2] = (uint16_t)((val & 0x9f1fu) |
                                      (fault.pcm_stuck_muted ? AC97_MUTE : 0u));
            break;
        case AC97_NAM_DAC_RATE:
            /* Writable only with VRA enabled — that is the whole point of the
             * extended-audio handshake the program performs first. */
            if (fault.rate_rejected) { nam[off / 2] = 0xac44; break; }  /* clamps to 44.1k */
            if (nam[AC97_NAM_EXT_CTRL / 2] & AC97_VRA) nam[off / 2] = (uint16_t)val;
            break;
        default: nam[off / 2] = (uint16_t)val; break;
        }
        return;
    }
    if (port >= nabm_base && port + w <= nabm_base + AC97_NABM_SIZE) {
        uint32_t off = port - nabm_base;
        log_acc(A_WR, 1, off, w, val);
        switch (off) {
        case AC97_GLOB_CNT: glob_cnt = val; break;
        case AC97_PO_BDBAR: if (!fault.bdbar_readonly) po_bdbar = val & ~3u; break;
        case AC97_PO_LVI:   if (!fault.lvi_readonly) po_lvi = (uint8_t)(val & 0x1f); break;
        case AC97_PO_SR:    po_sr &= (uint16_t)~(val & 0x1cu); break;  /* W1C */
        case AC97_PO_CR:
            if (val & AC97_CR_RR) {
                if (fault.rr_stuck) { po_cr |= AC97_CR_RR; break; }
                bm_reset();
            } else if (val & AC97_CR_RPBM) {
                po_cr = (uint8_t)val;
                po_civ = po_piv;
                po_piv = (uint8_t)((po_piv + 1u) % AC97_BDL_MAX);
                fetch_bd();
                if (fault.halt_at_start) po_sr |= AC97_SR_DCH;
                else                     po_sr &= (uint16_t)~AC97_SR_DCH;
            } else {
                po_cr = (uint8_t)val;
                po_sr |= AC97_SR_DCH;
            }
            break;
        default: break;
        }
        return;
    }
    escaped++;
}

static uint32_t io_pr(void *ctx, uint16_t port, uint8_t w) {
    (void)ctx;
    uint32_t v;
    if (port >= nam_base && port + w <= nam_base + AC97_NAM_SIZE) {
        uint32_t off = port - nam_base;
        v = fault.random_reads ? prng() : fault.dead_mixer ? 0u : nam[off / 2];
        log_acc(A_RD, 0, off, w, v);
        return v;
    }
    if (port >= nabm_base && port + w <= nabm_base + AC97_NABM_SIZE) {
        uint32_t off = port - nabm_base;
        if (fault.random_reads) { v = prng(); log_acc(A_RD, 1, off, w, v); return v; }
        switch (off) {
        case AC97_GLOB_CNT: v = glob_cnt; break;
        case AC97_GLOB_STA:
            if (fault.never_ready)  v = 0;
            else if (pcr_reads > 0) { pcr_reads--; v = 0; }
            else                    v = AC97_GS_PCR;
            break;
        case AC97_PO_BDBAR: v = po_bdbar; break;
        case AC97_PO_CIV:   v = po_civ;   break;
        case AC97_PO_LVI:   v = po_lvi;   break;
        case AC97_PO_SR:    v = po_sr;    break;
        case AC97_PO_PICB:  v = po_picb;  break;
        case AC97_PO_CR:    v = po_cr;    break;
        default:            v = 0;        break;
        }
        log_acc(A_RD, 1, off, w, v);
        return v;
    }
    escaped++;
    return 0xFFFFFFFFu;
}

static uint32_t io_pci(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    (void)ctx;
    if (bus != 0 || dev != 6 || fn != 0) { escaped++; return 0xFFFFFFFFu; }
    uint32_t v;
    switch (off) {
    case 0x00: v = 0x24158086u; break;
    case 0x08: v = fault.wrong_class ? 0x02000001u : CFG_CLASS; break;
    default:   v = 0; break;
    }
    log_acc(A_PCI, 0, off, 4, v);
    return v;
}

static void io_delay(void *ctx, uint32_t us) {
    (void)ctx;
    delay_total += us;
    log_acc(A_DLY, 0, 0, 0, us);
    engine_advance(us);
}

/* No MMIO hooks at all: this device has none, and a program that tried an
 * ld/st would be refused by the policy long before reaching a NULL hook. */
static const dvm_io_t CODEC = {
    .ctx = NULL,
    .port_write = io_pw, .port_read = io_pr,
    .mmio_write = NULL,  .mmio_read = NULL,
    .pci_read32 = io_pci, .delay_us = io_delay,
};

/* ====================================================================== */
/* running the real program                                               */
/* ====================================================================== */

static dvm_program_t P;
static dvm_policy_t  POL;
static dvm_result_t  R;
static dvm_asm_err_t E;

static void policy_for_device(void) {
    dvm_policy_init(&POL);
    dvm_policy_allow_ports(&POL, NAM_BASE,  NAM_BASE  + AC97_NAM_SIZE  - 1);
    dvm_policy_allow_ports(&POL, NABM_BASE, NABM_BASE + AC97_NABM_SIZE - 1);
    dvm_policy_allow_pci(&POL, 0, 6, 0);
    POL.max_delay_us = 1800000;          /* what vm/programs/ac97_boot.c uses */
}

static int assemble_reference(void) {
    if (dvm_assemble(AC97_BRINGUP_SRC, strlen(AC97_BRINGUP_SRC), &P, &E) == 0)
        return 1;
    printf("    FAIL the reference program does not assemble: line %d: %s\n",
           E.line, E.msg);
    th_fails++;
    return 0;
}

static dvm_status_t run_reference(void) {
    const uint64_t args[AC97_NARGS] = {
        [AC97_ARG_NAM]  = NAM_BASE,
        [AC97_ARG_NABM] = NABM_BASE,
        [AC97_ARG_BDL]  = BDL_ADDR,
        [AC97_ARG_LVI]  = LVI,
        [AC97_ARG_BDF]  = BDF,
    };
    return dvm_run(&P, &POL, &CODEC, args, AC97_NARGS, &R);
}

/* Assemble, reset the device, run. The common preamble of every test below. */
static dvm_status_t fresh_run(void) {
    if (!assemble_reference()) return DVM_TRAP_BADOP;
    dev_reset();
    policy_for_device();
    kcap_reset();
    return run_reference();
}

/* ====================================================================== */
/* 1. the artifact and the compiled copy are the same bytes               */
/* ====================================================================== */

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf || n < 0 || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    buf[n] = '\0';
    *len = (size_t)n;
    fclose(f);
    return buf;
}

static void test_source_matches_artifact(void) {
    /* `make test-host` runs from ; the second path is for running the
     * binary straight out of tests/build/. */
    static const char *paths[] = {
        "vm/programs/ac97_bringup.dvm",
        "../../vm/programs/ac97_bringup.dvm",
    };
    size_t len = 0;
    char  *disk = NULL;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0] && !disk; i++)
        disk = slurp(paths[i], &len);
    if (!disk) {
        printf("    FAIL cannot open vm/programs/ac97_bringup.dvm "
               "(run the suite from , as `make test-host` does)\n");
        th_fails++;
        th_checks++;
        return;
    }
    const char *baked = AC97_BRINGUP_SRC;
    CHECK_EQ(len, strlen(baked));
    if (len == strlen(baked)) {
        size_t at = 0;
        while (at < len && disk[at] == baked[at]) at++;
        if (at != len)
            printf("    the two copies diverge at byte %lu: file has '%c', "
                   "header has '%c' — run python3 vm/programs/gen_header.py\n",
                   (unsigned long)at, disk[at], baked[at]);
        CHECK_EQ(at, len);
    } else {
        printf("    ac97_bringup.h is stale — run python3 vm/programs/gen_header.py\n");
    }
    free(disk);
}

/* ====================================================================== */
/* 2. it assembles, and it fits the machine with room to spare            */
/* ====================================================================== */

static void test_assembles_within_limits(void) {
    memset(&E, 0, sizeof E);
    CHECK_EQ(dvm_assemble(AC97_BRINGUP_SRC, strlen(AC97_BRINGUP_SRC), &P, &E), 0);
    CHECK_EQ(E.line, 0);
    CHECK_STR(E.msg, "");

    CHECK(P.ninsn > 80);                       /* a real bring-up, not a stub */
    CHECK(P.ninsn <= DVM_MAX_INSNS);
    CHECK(P.nlabel <= DVM_MAX_LABELS);
    CHECK(P.nstr <= DVM_MAX_STRINGS);
    CHECK(P.strused <= DVM_STRPOOL);
    CHECK_EQ(dvm_program_validate(&P, &E), DVM_OK);

    printf("    program: %u instructions (max %d), %u labels (max %d), "
           "%u strings using %u/%d pool bytes, %u source lines\n",
           (unsigned)P.ninsn, DVM_MAX_INSNS, (unsigned)P.nlabel, DVM_MAX_LABELS,
           (unsigned)P.nstr, (unsigned)P.strused, DVM_STRPOOL,
           (unsigned)P.src_lines);

    /* A NOT-NUL-terminated span, which is how it arrives from a JSON tool
     * call. Same program, same instruction count. */
    size_t n = strlen(AC97_BRINGUP_SRC);
    char *span = (char *)malloc(n);
    memcpy(span, AC97_BRINGUP_SRC, n);
    dvm_program_t *q = (dvm_program_t *)malloc(sizeof *q);
    CHECK_EQ(dvm_assemble(span, n, q, &E), 0);
    CHECK_EQ(q->ninsn, P.ninsn);
    CHECK_EQ(memcmp(q->insn, P.insn, sizeof q->insn), 0);
    free(q);
    free(span);
}

/* ====================================================================== */
/* 3. the bring-up, against a codec that behaves                          */
/* ====================================================================== */

/* The driver's entire observable effect on the hardware, in order. */
typedef struct { int nabm; uint32_t off; uint8_t w; uint32_t val; } wexp_t;

static const wexp_t GOLDEN_WRITES[] = {
    { 1, AC97_GLOB_CNT,     4, 0x00000000 },  /* assert AC-link cold reset   */
    { 1, AC97_GLOB_CNT,     4, AC97_GC_COLD },/* release it                  */
    { 0, AC97_NAM_RESET,    2, 0x0000 },      /* mixer to power-on defaults  */
    { 0, AC97_NAM_MASTER,   2, AC97_MUTE },   /* probe: mute must stick      */
    { 0, AC97_NAM_MASTER,   2, 0x0000 },      /* unmute, full volume         */
    { 0, AC97_NAM_PCM_OUT,  2, AC97_PCM_0DB },/* unmute the PCM path at 0 dB */
    { 0, AC97_NAM_EXT_CTRL, 2, 0x0009 },      /* VRA on (it already was)     */
    { 0, AC97_NAM_DAC_RATE, 2, AC97_RATE_48K },
    { 1, AC97_PO_CR,        1, AC97_CR_RR },  /* reset the PCM-out channel   */
    { 1, AC97_PO_BDBAR,     4, BDL_ADDR },
    { 1, AC97_PO_LVI,       1, LVI },
    { 1, AC97_PO_CR,        1, AC97_CR_RPBM },/* run                         */
    { 1, AC97_PO_CR,        1, 0x00 },        /* drained: stop the master    */
};

static void test_bringup_succeeds(void) {
    dvm_status_t st = fresh_run();
    CHECK_EQ(st, DVM_OK);
    CHECK_STR(R.msg, "");
    if (st != DVM_OK) {
        printf("    stopped at .dvm line %u: %s\n", (unsigned)R.line, R.msg);
        return;
    }

    /* ---- the codec was left in a working state ---- */
    CHECK_EQ(R.reg[AC97_RES_GLOB_STA] & AC97_GS_PCR, AC97_GS_PCR);
    CHECK_EQ(R.reg[AC97_RES_MASTER] & AC97_MUTE, 0);
    CHECK_EQ(R.reg[AC97_RES_PCM] & AC97_MUTE, 0);
    CHECK_EQ(R.reg[AC97_RES_PCM], AC97_PCM_0DB);
    CHECK_EQ(nam[AC97_NAM_DAC_RATE / 2], AC97_RATE_48K);
    CHECK_EQ(nam[AC97_NAM_MASTER / 2] & AC97_MUTE, 0);
    CHECK_EQ(nam[AC97_NAM_PCM_OUT / 2] & AC97_MUTE, 0);

    /* ---- it really played, and it really stopped ---- */
    CHECK(R.reg[AC97_RES_PICB0] > R.reg[AC97_RES_PICB1]);   /* counted down */
    /* PICB is first read 5 ms after the run bit, by which time the engine
     * has consumed 5000 us * 96 samples/ms = 480 samples of the first buffer. */
    CHECK_EQ(R.reg[AC97_RES_PICB0], BD_SAMPLES - 480);
    CHECK_EQ(samples_played, (uint64_t)BD_SAMPLES * BDL_N);
    CHECK_EQ(buffers_done, (int)BDL_N);
    CHECK_EQ(bd_fetches, (int)BDL_N);
    CHECK_EQ(last_bd_addr, TONE_ADDR + (BDL_N - 1) * BD_SAMPLES * 2u);
    CHECK_EQ(po_sr & AC97_SR_DCH, AC97_SR_DCH);
    CHECK_EQ(po_cr & AC97_CR_RPBM, 0);         /* bus master left idle */
    CHECK_EQ(po_bdbar, BDL_ADDR);
    CHECK_EQ(po_lvi, LVI);

    /* ---- the exact write sequence ---- */
    int nw = 0;
    for (int i = 0; i < nalog; i++) {
        if (alog[i].k != A_WR) continue;
        if (nw < (int)(sizeof GOLDEN_WRITES / sizeof GOLDEN_WRITES[0])) {
            const wexp_t *e = &GOLDEN_WRITES[nw];
            int ok = alog[i].nabm == e->nabm && alog[i].off == e->off &&
                     alog[i].w == e->w && alog[i].val == e->val;
            th_checks++;
            if (!ok) {
                th_fails++;
                printf("    FAIL write %d: got %s+0x%02x w%u = 0x%x, "
                       "want %s+0x%02x w%u = 0x%x\n", nw,
                       alog[i].nabm ? "nabm" : "nam", alog[i].off, alog[i].w,
                       alog[i].val, e->nabm ? "nabm" : "nam", e->off, e->w, e->val);
            }
        }
        nw++;
    }
    CHECK_EQ(nw, (int)(sizeof GOLDEN_WRITES / sizeof GOLDEN_WRITES[0]));

    /* ---- it stayed inside the register windows the spec defines ---- */
    for (int i = 0; i < nalog; i++) {
        if (alog[i].k != A_WR && alog[i].k != A_RD) continue;
        CHECK(alog[i].off < (alog[i].nabm ? AC97_NABM_SIZE : AC97_NAM_SIZE));
    }

    /* ---- the run stayed cheap ---- */
    CHECK(R.steps < 400);
    CHECK(R.io_ops < 120);
    CHECK_EQ(R.n_mmio_rd + R.n_mmio_wr, 0);    /* it touches no memory at all */
    CHECK_EQ(R.n_pci_rd, 1);
    CHECK(R.delay_us <= POL.max_delay_us);
    CHECK_EQ(R.trace_dropped, 0);

    /* ---- the kernel's own account of it ---- */
    const char *log = kcap_text();
    CHECK_CONTAINS(log, "[dvm.grant ports 0x1200-0x12ff,0x1400-0x143f -> ok]");
    CHECK_CONTAINS(log, "[dvm.grant mmio none -> ok]");
    CHECK_CONTAINS(log, "[dvm.grant pci 00:06.0 -> ok]");
    CHECK_CONTAINS(log, "[dvm.print pci class = 0x401 -> ok]");
    CHECK_CONTAINS(log, "[dvm.print glob_sta = 0x100 -> ok]");
    CHECK_CONTAINS(log, "[dvm.print master vol = 0x0 -> ok]");
    CHECK_CONTAINS(log, "[dvm.print pcm vol = 0x808 -> ok]");
    CHECK_CONTAINS(log, "[dvm.print dac rate = 0xbb80 -> ok]");
    CHECK_CONTAINS(log, "[dvm.print bdbar = 0x200000 -> ok]");
    CHECK_CONTAINS(log, "[dvm.print stopped po_cr = 0x0 -> ok]");
    CHECK_CONTAINS(log, "[dvm.halt");
    printf("    ran: %u steps, %u accesses (%u reads, %u writes, %u pci), "
           "%u us of delay, %u trace lines\n",
           (unsigned)R.steps, (unsigned)R.io_ops, (unsigned)R.n_port_rd,
           (unsigned)R.n_port_wr, (unsigned)R.n_pci_rd, (unsigned)R.delay_us,
           (unsigned)R.trace_lines);
}

/* Nothing in the program is hardcoded to one machine. Move the device to a
 * different pair of BARs, hand the program the new bases, and the whole
 * bring-up must work identically — that is what makes it a driver rather than
 * a recording of one particular boot. */
static void test_bases_come_from_args(void) {
    CHECK(assemble_reference());
    dev_reset();
    nam_base  = 0x8000;
    nabm_base = 0x8400;

    dvm_policy_init(&POL);
    dvm_policy_allow_ports(&POL, nam_base,  nam_base  + AC97_NAM_SIZE  - 1);
    dvm_policy_allow_ports(&POL, nabm_base, nabm_base + AC97_NABM_SIZE - 1);
    dvm_policy_allow_pci(&POL, 0, 6, 0);
    POL.max_delay_us = 1800000;

    const uint64_t args[AC97_NARGS] = {
        [AC97_ARG_NAM] = nam_base, [AC97_ARG_NABM] = nabm_base,
        [AC97_ARG_BDL] = BDL_ADDR, [AC97_ARG_LVI] = LVI, [AC97_ARG_BDF] = BDF,
    };
    kcap_reset();
    CHECK_EQ(dvm_run(&P, &POL, &CODEC, args, AC97_NARGS, &R), DVM_OK);
    CHECK_EQ(samples_played, (uint64_t)BD_SAMPLES * BDL_N);
    CHECK_EQ(nam[AC97_NAM_MASTER / 2] & AC97_MUTE, 0);
    CHECK_CONTAINS(kcap_text(), "port=0x8002");    /* master volume, relocated */
    CHECK_CONTAINS(kcap_text(), "port=0x841b");    /* PO_CR, relocated         */
    CHECK_EQ(escaped, 0);

    /* Point it at a window where nothing answers (reads come back as all-ones,
     * the classic signature of an empty I/O range). The driver must refuse
     * rather than start a DMA engine that is not there. */
    dev_reset();
    nam_base  = 0x9000;                     /* the device moved... */
    nabm_base = 0x9400;
    dvm_policy_init(&POL);                  /* ...but the caller did not */
    dvm_policy_allow_ports(&POL, 0x8000, 0x80ff);
    dvm_policy_allow_ports(&POL, 0x8400, 0x843f);
    dvm_policy_allow_pci(&POL, 0, 6, 0);
    POL.max_delay_us = 1800000;
    long before = escaped;
    CHECK_EQ(dvm_run(&P, &POL, &CODEC, args, AC97_NARGS, &R), DVM_TRAP_ABORT);
    /* An unclaimed I/O range floats to all-ones, so the mute bit "sticks" and
     * then refuses to clear: the driver stops at the second half of the probe,
     * which is exactly why the probe has two halves. */
    CHECK_CONTAINS(R.msg, "master volume stayed muted");
    CHECK_EQ(samples_played, 0);
    CHECK_EQ(escaped - before, 9);          /* 9 accesses hit empty I/O space */
    CHECK_EQ(nam[AC97_NAM_MASTER / 2], 0x8000);   /* the real mixer is untouched */
    escaped = before;                       /* accounted for; not a containment bug */
}

/* ====================================================================== */
/* 4. every failure exit fires on the fault it names                      */
/* ====================================================================== */

static void expect_abort(const char *what, const char *needle) {
    dvm_status_t st = run_reference();
    th_checks++;
    if (st != DVM_TRAP_ABORT || !strstr(R.msg, needle)) {
        th_fails++;
        printf("    FAIL %s: expected ABORT containing \"%s\", got %s \"%s\" "
               "at .dvm line %u\n", what, needle, dvm_status_name(st), R.msg,
               (unsigned)R.line);
        return;
    }
    /* A refused bring-up must still leave the machine explicable. */
    CHECK(R.line > 0);
    CHECK(R.steps > 0);
    CHECK_CONTAINS(kcap_text(), "[dvm.stop ABORT");
}

#define FAULT_CASE(field, what, needle)                                        \
    do {                                                                       \
        dev_reset(); policy_for_device(); kcap_reset();                        \
        fault.field = 1;                                                       \
        expect_abort(what, needle);                                            \
    } while (0)

static void test_every_failure_exit(void) {
    CHECK(assemble_reference());

    FAULT_CASE(wrong_class,     "a non-audio function",   "not a PCI audio-class function");
    FAULT_CASE(never_ready,     "codec never ready",      "primary codec never reported ready");
    FAULT_CASE(dead_mixer,      "no mixer behind the BAR","master mute bit did not stick");
    FAULT_CASE(stuck_muted,     "master mute stuck",      "master volume stayed muted");
    FAULT_CASE(pcm_stuck_muted, "pcm mute stuck",         "pcm out volume stayed muted");
    FAULT_CASE(rate_rejected,   "rate write ignored",     "codec refused the 48 kHz rate");
    FAULT_CASE(rr_stuck,        "channel reset stuck",    "PCM-out reset bit never cleared");
    FAULT_CASE(bdbar_readonly,  "bdbar ignores writes",   "bdbar did not read back");
    FAULT_CASE(lvi_readonly,    "lvi ignores writes",     "lvi did not read back");
    FAULT_CASE(halt_at_start,   "dma halted at once",     "dma halted immediately after start");
    FAULT_CASE(picb_frozen,     "position never moves",   "picb never moved");
    FAULT_CASE(never_drains,    "playback never ends",    "playback never finished");

    /* The last one is the interesting bound: a stream that never ends must be
     * stopped by the delay budget's worth of patience, not run forever. */
    CHECK(R.delay_us <= POL.max_delay_us);
    CHECK(R.steps < 1000);
}

/* A fault must never leave the driver believing it succeeded, and must never
 * leave the DMA engine running with a descriptor list the caller may reuse. */
static void test_failure_leaves_no_playback(void) {
    dev_reset(); policy_for_device(); kcap_reset();
    fault.bdbar_readonly = 1;
    CHECK_EQ(run_reference(), DVM_TRAP_ABORT);
    CHECK_EQ(po_cr & AC97_CR_RPBM, 0);
    CHECK_EQ(samples_played, 0);
    CHECK_EQ(bd_fetches, 0);
}

/* ====================================================================== */
/* 5. containment: the policy is the boundary, not the program's goodwill */
/* ====================================================================== */

static void test_wrong_policy_refuses_before_touching_anything(void) {
    CHECK(assemble_reference());
    dev_reset();

    /* A caller that grants the NEIGHBOURING device's BAR (a plausible bug in
     * whatever derives the policy) must not let this program drive ours. */
    dvm_policy_init(&POL);
    dvm_policy_allow_ports(&POL, 0x1100, 0x11ff);
    dvm_policy_allow_pci(&POL, 0, 6, 0);
    long before = escaped;
    CHECK_EQ(run_reference(), DVM_TRAP_PORT_DENIED);
    CHECK_CONTAINS(R.msg, "is not in this program's allowed ranges");
    CHECK_EQ(escaped - before, 0);
    CHECK_EQ(samples_played, 0);

    /* No PCI grant at all: it cannot even identify the device. */
    dev_reset();
    dvm_policy_init(&POL);
    dvm_policy_allow_ports(&POL, NAM_BASE, NAM_BASE + AC97_NAM_SIZE - 1);
    dvm_policy_allow_ports(&POL, NABM_BASE, NABM_BASE + AC97_NABM_SIZE - 1);
    CHECK_EQ(run_reference(), DVM_TRAP_PCI_DENIED);
    CHECK_EQ(nalog, 0);

    /* Deny-all, the default a caller gets before it opens anything. */
    dev_reset();
    dvm_policy_init(&POL);
    CHECK_EQ(run_reference(), DVM_TRAP_PCI_DENIED);
    CHECK_EQ(nalog, 0);

    /* A policy that overlaps the compiled-in deny list is refused outright,
     * before a single instruction runs — even though the program itself is
     * fine and would never have touched those ports. */
    dev_reset();
    policy_for_device();
    CHECK_EQ(dvm_policy_allow_ports(&POL, 0x3f8, 0x3ff), 0);
    CHECK_EQ(run_reference(), DVM_TRAP_POLICY);
    CHECK_EQ(R.steps, 0);
    CHECK_EQ(nalog, 0);
}

/* The budgets are the other half of containment: with a mean device that never
 * finishes anything, the run still ends, and it ends explicably. */
static void test_budgets_bound_a_hostile_device(void) {
    CHECK(assemble_reference());

    dev_reset(); policy_for_device();
    fault.never_ready = 1;
    POL.max_delay_us = 5000;                 /* less than the codec wait */
    CHECK_EQ(run_reference(), DVM_TRAP_DELAY_BUDGET);
    CHECK(R.delay_us <= 5000);

    dev_reset(); policy_for_device();
    fault.never_ready = 1;
    POL.max_io = 4;                          /* fewer than the poll loop needs */
    CHECK_EQ(run_reference(), DVM_TRAP_IO_BUDGET);
    CHECK_EQ(R.io_ops, 4);

    dev_reset(); policy_for_device();
    fault.never_ready = 1;
    POL.max_steps = 20;
    CHECK_EQ(run_reference(), DVM_TRAP_STEPS);
    CHECK_EQ(R.steps, 20);

    dev_reset(); policy_for_device();
    POL.max_prints = 2;
    CHECK_EQ(run_reference(), DVM_TRAP_PRINT_BUDGET);
    CHECK_EQ(R.prints, 2);
}

/* ====================================================================== */
/* 6. the program survives being handed nonsense                          */
/* ====================================================================== */

static void test_absurd_arguments(void) {
    CHECK(assemble_reference());

    /* Bases at the very top of I/O space: the VM must refuse the access
     * rather than wrap the port number round. */
    dev_reset();
    dvm_policy_init(&POL);
    dvm_policy_allow_ports(&POL, 0xff00, 0xffff);
    dvm_policy_allow_pci(&POL, 0, 6, 0);
    const uint64_t hi[AC97_NARGS] = {
        [AC97_ARG_NAM] = 0xfffful, [AC97_ARG_NABM] = 0xfffful,
        [AC97_ARG_BDL] = BDL_ADDR, [AC97_ARG_LVI] = LVI, [AC97_ARG_BDF] = BDF,
    };
    dvm_status_t st = dvm_run(&P, &POL, &CODEC, hi, AC97_NARGS, &R);
    CHECK(st == DVM_TRAP_RANGE || st == DVM_TRAP_PORT_DENIED);
    CHECK_EQ(escaped, 0);

    /* Every argument at its 64-bit maximum, which is what a confused caller
     * (or a model-supplied argument vector) would look like. */
    dev_reset();
    policy_for_device();
    uint64_t junk[AC97_NARGS];
    for (int i = 0; i < AC97_NARGS; i++) junk[i] = ~0ull;
    st = dvm_run(&P, &POL, &CODEC, junk, AC97_NARGS, &R);
    CHECK(st != DVM_OK);
    CHECK(R.msg[0] != '\0');
    CHECK_EQ(escaped, 0);

    /* No arguments at all: every register starts at zero, so the program
     * addresses port 0 and is refused. It must not run off into the device. */
    dev_reset();
    policy_for_device();
    st = dvm_run(&P, &POL, &CODEC, NULL, 0, &R);
    CHECK(st != DVM_OK);
    CHECK_EQ(nalog, 0);
    CHECK_EQ(escaped, 0);
}

/* ====================================================================== */
/* 7. a codec that answers with garbage                                   */
/* ====================================================================== */

/* Hardware that is broken, half-initialised, or simply not the device the
 * caller thought it was does not return neat values — it returns whatever is
 * floating on the bus. Thousands of runs against a codec whose every read is
 * random must still always terminate, always inside the budgets, always with
 * a legible reason, and never with an access outside the two windows. This is
 * the property that makes a model-authored driver safe to just try. */
static uint32_t prng_state;
static uint32_t prng(void) {
    prng_state = prng_state * 1664525u + 1013904223u;
    return prng_state >> 8;
}

/* Every register this driver is ever allowed to write. Anything else in the
 * log means the program wandered, whatever the VM thought of it. */
static int write_target_is_known(int nabm, uint32_t off) {
    if (!nabm)
        return off == AC97_NAM_RESET || off == AC97_NAM_MASTER ||
               off == AC97_NAM_PCM_OUT || off == AC97_NAM_EXT_CTRL ||
               off == AC97_NAM_DAC_RATE;
    return off == AC97_GLOB_CNT || off == AC97_PO_BDBAR ||
           off == AC97_PO_LVI   || off == AC97_PO_CR;
}

static void test_flaky_codec(void) {
    CHECK(assemble_reference());
    int statuses[DVM_STATUS__COUNT];
    memset(statuses, 0, sizeof statuses);
    struct { char text[DVM_MSG_MAX]; int n; } msgs[16];
    int nmsg = 0;

    for (int iter = 0; iter < 2000; iter++) {
        dev_reset();
        policy_for_device();
        prng_state = (uint32_t)iter * 2654435761u + 12345u;
        fault.random_reads = 1;
        long before = escaped;

        dvm_status_t st = run_reference();
        if ((unsigned)st < DVM_STATUS__COUNT) statuses[st]++;
        int seen = 0;
        for (int i = 0; i < nmsg && !seen; i++)
            if (strcmp(msgs[i].text, R.msg) == 0) { msgs[i].n++; seen = 1; }
        if (!seen && nmsg < 16) {
            snprintf(msgs[nmsg].text, sizeof msgs[nmsg].text, "%s", R.msg);
            msgs[nmsg++].n = 1;
        }

        /* It ended, and it ended inside every bound it was given. */
        CHECK(R.steps <= POL.max_steps);
        CHECK(R.io_ops <= POL.max_io);
        CHECK(R.delay_us <= POL.max_delay_us);
        CHECK(R.prints <= POL.max_prints);
        /* It said why, in bytes that are safe to hand back to a model. */
        if (st != DVM_OK) CHECK(R.msg[0] != '\0');
        for (int i = 0; R.msg[i]; i++) CHECK((unsigned char)R.msg[i] >= 0x20);
        /* It never left the device's own registers. */
        CHECK_EQ(escaped - before, 0);
        for (int i = 0; i < nalog; i++)
            if (alog[i].k == A_WR) CHECK(write_target_is_known(alog[i].nabm, alog[i].off));
    }

    printf("    2000 runs against a codec returning noise:");
    for (int i = 0; i < DVM_STATUS__COUNT; i++)
        if (statuses[i]) printf(" %s=%d", dvm_status_name(i), statuses[i]);
    printf("\n");
    for (int i = 0; i < nmsg; i++)
        printf("      %4d x \"%s\"\n", msgs[i].n, msgs[i].text);
    /* Noise takes several different exits. If it only ever took one, this test
     * would be asserting far less than it looks like it is. */
    CHECK(nmsg >= 4);
}

/* ====================================================================== */
/* 8. determinism — a golden program has to produce a golden run          */
/* ====================================================================== */

static void test_repeatable(void) {
    uint64_t steps = 0, io = 0;
    for (int i = 0; i < 32; i++) {
        CHECK_EQ(fresh_run(), DVM_OK);
        if (i == 0) { steps = R.steps; io = R.io_ops; }
        CHECK_EQ(R.steps, steps);
        CHECK_EQ(R.io_ops, io);
        CHECK_EQ(samples_played, (uint64_t)BD_SAMPLES * BDL_N);
    }
}

int main(void) {
    console_init();
    RUN(test_source_matches_artifact);
    RUN(test_assembles_within_limits);
    RUN(test_bringup_succeeds);
    RUN(test_bases_come_from_args);
    RUN(test_every_failure_exit);
    RUN(test_failure_leaves_no_playback);
    RUN(test_wrong_policy_refuses_before_touching_anything);
    RUN(test_budgets_bound_a_hostile_device);
    RUN(test_absurd_arguments);
    RUN(test_flaky_codec);
    RUN(test_repeatable);

    printf("  (device accesses: %ld, outside the modelled windows: %ld)\n",
           accesses, escaped);
    CHECK_EQ(escaped, 0);
    CHECK_EQ(kpanic_hit, 0);
    CHECK(accesses > 500);
    return th_report("dvm_ac97");
}
