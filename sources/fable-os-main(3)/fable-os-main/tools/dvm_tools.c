/* dvm_tools.c — the model writes a device driver, and this file runs it.
 *
 * PURPOSE
 *   Everything before this file built the pieces: pci_list finds hardware
 *   nobody drives (tools/dev_tools.c), the driver VM executes a bring-up
 *   sequence that cannot crash the kernel (include/dvm.h), and a hand-written
 *   AC'97 program proved the VM really does drive silicon (vm/programs/).
 *   This file is the loop that joins them:
 *
 *       pci_list unclaimed_only=true   "nobody drives 00:05.0"
 *       driver_targets                 "here is the exact sandbox 00:05.0 gets"
 *       driver_assemble                "does my program even parse?"      (free)
 *       driver_run                     "run it"        -> it failed, here is why
 *       driver_run                     "run the fixed one"  -> the device is up
 *
 *   The model writes the driver. The kernel decides what the driver may touch,
 *   runs it, and reports what actually happened — every value in that report
 *   was read back out of the device microseconds earlier.
 *
 * THE TOOLS
 *   driver_targets   which devices a program may be pointed at, and for each
 *                    one the port/MMIO/PCI windows it would be granted, or the
 *                    reason it is not targetable at all.
 *   driver_assemble  syntax-check a program and get a numbered listing back.
 *                    Touches no hardware, costs no attempt: this is where a
 *                    model should burn its typos.
 *   driver_run       assemble + execute against ONE NAMED DEVICE. TOOL_MUTATES:
 *                    it writes to real device registers.
 *   driver_install   keep a second, shorter program as the machine's AUDIO SINK,
 *                    re-run by core/audio.c for every sound. This is the step
 *                    that turns "the card is up" into "the machine can play".
 *   driver_trace     page through the access log of the last run, for when the
 *                    tail that fitted in driver_run's answer was not enough.
 *
 * THE SANDBOX IS DERIVED, NEVER SUPPLIED
 *   The model names a DEVICE ("pci00:05.0"), never an address. Everything the
 *   program is allowed to touch is computed here, from that function's own
 *   configuration space:
 *
 *     - each programmed BAR becomes one window: ports get [base, base+size),
 *       memory BARs get the same as an MMIO window with read+write;
 *     - `size` is not readable from a BAR (sizing needs config-space writes,
 *       which this kernel refuses), so it is bounded three ways and the
 *       smallest wins: the BAR's own natural alignment, a hard cap
 *       (256 bytes of I/O, which is the PCI maximum, or 1 MiB of MMIO), and
 *       the distance to the next BAR base found anywhere on the bus, so a
 *       window can never swallow a neighbouring device's registers;
 *     - the local APIC / IOAPIC / HPET block at 0xFEC00000-0xFEEFFFFF is
 *       seeded into that neighbour list as a barrier, and a BAR that lands
 *       inside it is refused outright;
 *     - exactly one PCI function is opened for `pcicfg`, and config-space
 *       WRITES do not exist in the ISA at all;
 *     - the VM's own DMA scratch buffer is granted, at an address this file gets
 *       from dvm.c and never computes;
 *     - the whole thing is then handed to dvm_policy_check(), so the VM's own
 *       compiled-in deny list (PICs, PIT, PS/2, CMOS, COM1, 0xCF8, all memory
 *       below the end of the kernel image) gets the last word. A device whose
 *       BAR overlaps any of it is reported as untargetable rather than
 *       silently trimmed.
 *
 * WHAT IS REFUSED BEFORE A SINGLE INSTRUCTION RUNS
 *   - a device that is not PCI-backed: there is no BAR to derive a window from,
 *     and the legacy ports those devices use are on the VM's deny list anyway;
 *   - a device with a driver bound: two drivers on one device is a race, and
 *     the resident driver would be the one to fall over;
 *   - a display controller: that is the operator's console (see target_blocked).
 *
 * DMA, AND THE PART THAT IS NOT CONTAINED
 *   A program that can only poke registers can reset a codec. It cannot play a
 *   sound, because a DMA engine has to be handed a physical address and the VM
 *   used to have no memory to name. Two things fixed that, and both of them are
 *   real privilege, so they are described here plainly rather than buried:
 *
 *     1. A DMA SCRATCH BUFFER. include/dvm.h owns one 256 KiB page-aligned static
 *        arena. build_sandbox() grants it and passes its physical address and size
 *        in r7/r8. This file never computes that address: it asks dvm.c, and
 *        dvm_policy_allow_dma() refuses anything that is not a subrange of dvm.c's
 *        own array, so a bug here cannot turn into a window on the heap.
 *
 *     2. BUS MASTER ENABLE, set by this file. `pcicfg` is a read-only opcode and
 *        stays that way — one bad config dword moves a live device's BAR — so
 *        t_driver_run() sets PCI command bit 2 (plus bit 0 and/or bit 1 if the
 *        sandbox granted I/O or memory windows, without which the program cannot
 *        reach its own BARs) on the ONE named function, reads the register back to
 *        see which bits actually stuck, and clears exactly the bits it added if the
 *        program did not reach halt. A program that halted keeps them, because it
 *        may have started a transfer the operator is meant to hear.
 *
 *   WHAT THAT DOES NOT BUY. The VM bounds what the PROGRAM touches. Nothing here
 *   bounds what the HARDWARE does once the program has written an address into a
 *   device's DMA register. There is no IOMMU on this machine. A device given a
 *   wrong physical address will DMA over kernel memory and no instruction check
 *   can see it happen.
 *
 *   The mitigations are worth listing exactly, because the difference between
 *   "mitigated" and "prevented" is the whole point:
 *     - only the one named target is made a bus master, and only by C;
 *     - the program is told exactly one valid buffer address and one size;
 *     - the arena is its own .bss object with 64 KiB of poisoned guard band
 *       either side, and dvm_dma_check() reports an overrun of up to that much
 *       as a fact with a byte offset, to the model and to the trace;
 *     - the values a program wrote into device registers are recorded twice, in
 *       two records with OPPOSITE blind spots, so the address it handed the
 *       hardware is usually — but NOT always — recoverable. Stated precisely,
 *       because this bullet is part of the argument for accepting the escalation
 *       and it used to claim "every value ... is in the access log", which is
 *       false:
 *         * g_log, which driver_run's tail and driver_trace read, is a RING of
 *           DRV_LOG_MAX (256) events against a budget of DRV_MAX_IO (1024). It
 *           keeps the LAST 256, so it loses the START of a long run;
 *         * the serial console trace in vm/dvm.c is bounded by max_trace (128 for
 *           trace=io, 256 for trace=all) and keeps the FIRST N, so it loses the
 *           END.
 *       A program that polls 200 times, then writes a wild descriptor address,
 *       then polls 300 more times (501 events — an ordinary bring-up shape, and
 *       inside budget) has that write in NEITHER record. driver_trace is honest at
 *       the point of use ("%lu events happened; the last %lu are kept"), and this
 *       comment now is too. Closing the hole needs the log to keep both ends
 *       rather than one, which is a bigger change than a constant.
 *
 *   A determined program can still compute a completely different 32-bit number
 *   and write it into a descriptor-base register, and that DMA will happen. This
 *   is the same trust any real kernel extends to a driver it loads. The honest
 *   description of this path is "a trusted driver, written this turn, whose
 *   register accesses are sandboxed and whose DMA is not".
 *
 * THE RETRY LOOP
 *   A first attempt is usually wrong, and the interesting question is what the
 *   model gets to see afterwards. It gets: the trap name, the message the VM
 *   wrote, the source LINE and its own text back, that instruction
 *   disassembled, the final register file, and the tail of an access log this
 *   file recorded by wrapping the I/O backend — every port, address, width and
 *   value, in order, as they really happened. That is enough to revise a
 *   program without guessing, and it is why the loop converges instead of
 *   flailing.
 *
 *   It is bounded. DRV_ATTEMPT_BUDGET consecutive failed runs against one
 *   target and the tool stops running programs there and says so; a run that
 *   reaches `halt` clears the count. Without that a confused model owns the
 *   machine for as many rounds as chat.c will give it, poking a device it does
 *   not understand. dvm_tools_reset_attempts() clears the table, and nothing in
 *   the kernel calls it yet — see FUTURE EXTENSION POINTS.
 *
 * UNTRUSTED INPUT
 *   `source` is a program written by a language model and arrives as a raw,
 *   non-NUL-terminated JSON span that may contain embedded NULs, 16 KB of
 *   nothing, or no newlines at all. It is decoded with json.h into a fixed
 *   buffer, passed to dvm_assemble() with an explicit length, and every
 *   structural bound belongs to the assembler. Numeric arguments are range
 *   checked here before they can become a budget. Nothing in this file
 *   allocates except the 12 KB program image, which is kmalloc'd and freed on
 *   every path.
 *
 * DEPENDENCIES
 *   dvm.h (assemble/policy/run), pci.h (config space — reads only), device.h
 *   (the registry that gives a device its NAME), json.h, trace.h, tool.h,
 *   kernel.h for kmalloc and the console.
 *
 * FUTURE EXTENSION POINTS
 *   - chat.c should call dvm_tools_reset_attempts() at the start of each
 *     operator turn, so a new sentence gets a fresh budget while a runaway
 *     model inside one turn still hits the wall. That is a one-line change in a
 *     file this one does not own.
 *   - A program that halts cleanly is a driver. Keeping the assembled image and
 *     re-entering it on demand (dvm.h's entry-point hook, which does not exist
 *     yet) is how a model-authored driver becomes resident rather than one-shot.
 *   - The third thing DMA wants and still does not have is a way to BOUND where a
 *     device may write. VT-d would give it: a domain whose only mapping is the
 *     arena turns a wrong descriptor address into a fault instead of corruption,
 *     and would let this file stop describing the DMA path as trust.
 *   - dvm_dma_check() runs once, after the run. A device that keeps mastering
 *     afterwards can dirty the guard band later and be blamed on the next run. A
 *     "stop everything this device was doing" path (clear the master bit and the
 *     device's own run bit) would need device knowledge the kernel refuses to
 *     have; the honest alternative is what happens now, which is to say so.
 *   - BAR sizes are inferred. A `pci_size_bar` operation that writes all-ones
 *     and restores, under an explicit lock, would make the windows exact.
 */

#include "tool.h"
#include "dvm.h"
#include "audio.h"
#include "device.h"
#include "driver.h"
#include "pci.h"
#include "json.h"
#include "trace.h"
#include "vfs.h"
#include "kernel.h"

#include <stdarg.h>
#include <stdio.h>       /* vsnprintf, via port/stdio.h when freestanding */

/* ---- bounds, all of them enforced ---- */
#define DRV_NAME_MAX        40      /* longest device name we will look up   */
#define DRV_SRC_MAX         DVM_SRC_MAX      /* the assembler's own ceiling  */
#define DRV_ATTEMPT_BUDGET  5       /* consecutive failed runs per target    */
#define DRV_TARGETS_TRACKED 8       /* targets whose attempts we remember    */
#define DRV_LOG_MAX         256     /* device events kept from the last run  */
#define DRV_BASES_MAX       128     /* BAR bases collected from the whole bus */
/* RESULT BUDGET ARITHMETIC, and it has to be arithmetic rather than a guess.
 *
 * The number that matters is NOT the 4096 a host test passes; it is
 * CHAT_TOOL_RESULT_CAP, which is 1024 (net/chat.c keeps one shared result buffer
 * that size). Every driver_targets answer a model has ever read was capped at
 * 1024 bytes, and one usable row is 260-300 of them.
 *
 * DRV_ROW_MARGIN used to be 200 and was checked BEFORE a row was emitted. The
 * footer is about 240 bytes, so at len ~= 780 the check passed, the ~300-byte row
 * was written, the result hard-truncated mid-token, and the "...more not shown"
 * line and the "targets: N known, M can be driven, K shown" summary were never
 * written AT ALL. The model got a listing that stopped mid-word with no count and
 * no hint that anything was missing.
 *
 * So the footer is RESERVED, and a row is only started if the whole of it plus
 * the whole footer still fit. DRV_ROW_MAX is a worst case measured against the
 * widest row this file can produce (a target line, a sandbox line carrying four
 * BAR windows plus a DMA grant, a preloaded-register line, a pci-cmd line and an
 * attempts line). Being generous here costs at most one row and buys the property
 * that a row is never cut in half. */
#define DRV_ROW_MAX         384     /* worst case for one whole target row    */
#define DRV_FOOTER_RESERVE  300     /* the summary lines, which always print  */
#define DRV_ROW_MARGIN      200     /* legacy guard for the other listings    */

/* Window sizing. An I/O BAR is at most 256 bytes by the PCI spec; 1 MiB is a
 * generous ceiling for a memory BAR that the neighbour clamp normally beats. */
#define DRV_IO_CAP          0x100ull
#define DRV_MMIO_CAP        0x100000ull
#define DRV_MMIO_CAP_TIGHT  0x1000ull        /* used if the bus scan overflowed */

/* Budgets handed to the VM. The model may raise the delay budget (a real
 * bring-up waits) and the step budget (a poll loop counts), each up to a
 * ceiling; nothing else is negotiable. */
#define DRV_MAX_IO          1024u
#define DRV_DELAY_DEF_US    250000u
#define DRV_DELAY_MAX_MS    2000     /* == dvm.c's CEIL_DELAY_US             */
#define DRV_STEPS_DEF       100000u
#define DRV_STEPS_MAX       1000000u
#define DRV_PRINTS          32u
/* Enough to write every byte of the scratch buffer as 16-bit samples, which is
 * the widest thing a program is likely to want to do with it. dvm.c's ceiling is
 * one access per byte. */
#define DRV_MAX_DMA_OPS     (DVM_DMA_SIZE / 2)
#define DRV_TRACE_IO_LINES  128u     /* console lines at trace=io            */
#define DRV_TRACE_ALL_LINES 256u     /* ...and at trace=all                  */

/* Scratch memory and the kernel calls. A text-shaped program does a handful of
 * passes over the arena, so 64 of them is slack rather than a limit; 64
 * syscalls is more filesystem traffic than any one program should need and far
 * less than a loop would make. */
#define DRV_MAX_MEM_BYTES   (64ull * DVM_MEM_SIZE)
#define DRV_MAX_SYS         64u

/* THE SUBTREE A PROGRAM MAY NAME A FILE IN.
 *
 * fs.write is the widest hole in the syscall set: a program that can write
 * files can leave something a later turn will read and believe. Confining it to
 * one directory is what keeps "the model wrote a program that wrote a file"
 * from being able to touch anything the model put somewhere else with
 * write_file, and it gives the operator one place to look.
 *
 * "/" would have been the lazy choice and is exactly what dvm.c refuses to
 * assume: an empty fs_root with an fs syscall granted is a policy error there,
 * so this constant has to be typed by somebody. */
#define DRV_FS_ROOT         "/vm"

/* Bytes of the scratch arena echoed back after a run. This is a program's
 * OUTPUT channel for anything wider than sixteen registers, and 192 bytes is
 * enough to show a built request or a parsed field without eating the access
 * log's room in a 1 KiB result. */
#define DRV_SCRATCH_SHOW    192

/* The name a program with no device runs under, in the attempt table and in
 * every message. It cannot collide with a device: pci.c names its nodes
 * "pci<bb>:<dd>.<f>" and resolve_target only ever matches a registered one. */
#define DRV_SOFTWARE_NAME   "(no device)"

/* The platform block: IOAPIC, HPET, local APIC. No BAR belongs here, and a
 * program that reached it would be reprogramming interrupt routing. */
#define DRV_PLATFORM_LO     0xFEC00000ull
#define DRV_PLATFORM_HI     0xFEF00000ull

/* PCI base class 0x03 = display controller. On a machine whose only human
 * interface is a console painted into video memory, this is not "a device" -
 * it is the operator's eyes. See target_blocked(). */
#define PCI_CLASS_DISPLAY   0x03u

/* The PCI command register, and the ONLY config-space write anywhere in the
 * driver-VM path. The ISA has no config write on purpose (one bad dword moves a
 * live MMIO window), so the three bits a DMA-capable bring-up needs are set here,
 * by C, for the one named target, and undone if the program fails.
 *
 * The upper half of dword 0x04 is the STATUS register and its error bits are
 * write-1-to-clear, so every write below puts ZERO there: writing back the value
 * that was read would silently clear a Master Abort or Received Target Abort that
 * is evidence about this very device. */
#define PCI_CMD_REG         0x04
#define PCI_CMD_IO          0x0001u   /* I/O space decode:    reach an I/O BAR   */
#define PCI_CMD_MEM         0x0002u   /* memory space decode: reach a memory BAR */
#define PCI_CMD_MASTER      0x0004u   /* bus master: the device may DMA          */

/* ====================================================================== */
/* small helpers                                                          */
/* ====================================================================== */

/* These mirror tools/dev_tools.c. They are duplicated rather than shared
 * because that file's copies are static to it, and a tool family is meant to
 * be a self-contained .c that needs no edit anywhere else (see tool.h). */

static int name_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (size_t i = 0; i <= DRV_NAME_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 0;                       /* longer than any legal device name */
}

/* Emit the failure result *and* its trace line, discarding partial output so
 * the model never reads half an answer followed by an error. */
static int fail(tool_result_t *r, int err, const char *op, const char *args,
                const char *fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    r->is_error = 1;
    r->len = 0;
    /* Discarding the partial output also discards the truncation: tool_dispatch()
     * appends "[result truncated at N bytes]" to anything still flagged, which
     * on a short error message is a false claim. */
    r->truncated = 0;
    if (r->buf && r->cap) r->buf[0] = '\0';
    tool_result_printf(r, "error: %s", msg);
    trace_err(err, op, "%s", args ? args : "");
    return err;
}

static int parse_obj(const tool_call_t *c, json_value_t *obj) {
    const char *p = "{}";
    size_t      n = 2;
    if (c && c->input && c->input_len) { p = c->input; n = c->input_len; }
    if (json_parse(p, n, obj) != JSON_OK) return TOOL_EINVAL;
    if (obj->type != JSON_OBJECT) return TOOL_EINVAL;
    return TOOL_OK;
}

/* 1 = present and valid, 0 = absent, -1 = wrong type, -2 = out of range. */
static int f_int(const json_value_t *o, const char *key,
                 int64_t lo, int64_t hi, int64_t *out) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_NUMBER) return -1;
    int64_t x;
    if (json_int(&v, &x) != JSON_OK) return -2;
    if (x < lo || x > hi) return -2;
    *out = x;
    return 1;
}

static int f_bool(const json_value_t *o, const char *key, int *out) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_BOOL) return -1;
    if (json_bool(&v, out) != JSON_OK) return -1;
    return 1;
}

static int f_str(const json_value_t *o, const char *key, char *dst, size_t cap,
                 size_t *out_len) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_STRING) return -1;
    rc = json_str(&v, dst, cap, out_len);
    if (rc == JSON_ENOSPC) return -2;
    if (rc != JSON_OK) return -1;
    return 1;
}

static int room_low(const tool_result_t *r) {
    return r->truncated || r->len + DRV_ROW_MARGIN >= r->cap;
}

/* Append to a fixed buffer, returning the new length and never running past
 * cap. snprintf's "length it would have written" return is a trap when it is
 * used as an offset, so it is clamped here once instead of at every call. */
static int catf(char *buf, size_t cap, int used, const char *fmt, ...) {
    if (!cap || used < 0 || (size_t)used + 1 >= cap) return used;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, cap - (size_t)used, fmt, ap);
    va_end(ap);
    if (n < 0) return used;
    used += n;
    if ((size_t)used >= cap) used = (int)cap - 1;
    return used;
}

static int hexdig(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ====================================================================== */
/* the target: a NAME, resolved to one PCI function                       */
/* ====================================================================== */

typedef struct {
    char      name[DRV_NAME_MAX + 1];   /* the registry name, e.g. pci00:05.0 */
    uint8_t   bus, dev, fn;
    device_t *node;
    uint16_t  vendor, device;
    uint8_t   cls, sub;
    uint16_t  command;
} target_t;

typedef struct {
    const char        *want;        /* by name...                            */
    device_res_type_t  want_res;    /* ...or by the resource it owns         */
    uint64_t           want_base;
    device_t          *found;
} devq_t;

static void devq_cb(device_t *d, void *ctx) {
    devq_t *q = (devq_t *)ctx;
    if (q->found || !d) return;
    if (q->want) {
        if (name_eq(d->obj.name, q->want)) q->found = d;
        return;
    }
    if (!d->driver) return;                 /* only a DRIVEN owner counts */
    int n = d->nres;
    if (n > DEV_MAX_RESOURCES) n = DEV_MAX_RESOURCES;
    for (int i = 0; i < n; i++)
        if (d->res[i].type == q->want_res && d->res[i].base == q->want_base) {
            q->found = d;
            return;
        }
}

static device_t *dev_by_name(const char *name) {
    devq_t q;
    memset(&q, 0, sizeof q);
    q.want = name;
    device_foreach(devq_cb, &q);
    return q.found;
}

/* The device with a bound driver that owns a resource at `base`, if any. This
 * is how a PCI function is recognised as driven when the driver bound itself to
 * a different node: pci.c publishes "pci00:03.0" driverless while the e1000
 * driver owns "eth0" at the same MMIO address. Targeting that function would
 * poke a live NIC's registers out from under the driver mid-conversation. */
static device_t *dev_by_resource(device_res_type_t t, uint64_t base) {
    if (!base) return NULL;
    devq_t q;
    memset(&q, 0, sizeof q);
    q.want_res  = t;
    q.want_base = base;
    device_foreach(devq_cb, &q);
    return q.found;
}

/* Parse "bb:dd.f" exactly as pci_list prints it. Consumes the whole string. */
static int parse_addr(const char *s, uint8_t *bus, uint8_t *dev, uint8_t *fn) {
    if (!s) return -1;
    int i = 0, v, n;

    for (n = 0, v = 0; n < 2 && hexdig(s[i]) >= 0; n++) v = v * 16 + hexdig(s[i++]);
    if (n == 0 || s[i] != ':' || v > 255) return -1;
    *bus = (uint8_t)v;
    i++;

    for (n = 0, v = 0; n < 2 && hexdig(s[i]) >= 0; n++) v = v * 16 + hexdig(s[i++]);
    if (n == 0 || s[i] != '.' || v > 31) return -1;
    *dev = (uint8_t)v;
    i++;

    v = hexdig(s[i]);
    if (v < 0 || v > 7) return -1;
    *fn = (uint8_t)v;
    i++;
    return s[i] == '\0' ? 0 : -1;
}

/* A device node published by pci.c is named "pci<bb>:<dd>.<f>". */
static int node_pci_addr(const device_t *d, uint8_t *bus, uint8_t *dev, uint8_t *fn) {
    const char *n = d ? d->obj.name : NULL;
    if (!n || n[0] != 'p' || n[1] != 'c' || n[2] != 'i') return 0;
    return parse_addr(n + 3, bus, dev, fn) == 0;
}

static void target_read_ids(target_t *t) {
    uint32_t id  = pci_cfg_read32(t->bus, t->dev, t->fn, 0x00);
    uint32_t cs  = pci_cfg_read32(t->bus, t->dev, t->fn, 0x08);
    uint32_t cmd = pci_cfg_read32(t->bus, t->dev, t->fn, 0x04);
    t->vendor  = (uint16_t)(id & 0xFFFF);
    t->device  = (uint16_t)(id >> 16);
    t->cls     = (uint8_t)(cs >> 24);
    t->sub     = (uint8_t)(cs >> 16);
    t->command = (uint16_t)(cmd & 0xFFFF);
}

/* Resolve a model-supplied name. Accepts the registry name ("pci00:05.0") or
 * the bare address pci_list prints ("00:05.0"), but BOTH must resolve to a
 * device the kernel has actually registered — the model cannot invent one.
 * Returns 0, or -1 with `why` filled in. */
static int resolve_target(const char *name, target_t *t, char *why, size_t cap) {
    device_t *d = dev_by_name(name);

    if (!d) {
        uint8_t bus, dev, fn;
        char    alt[16];
        if (parse_addr(name, &bus, &dev, &fn) == 0) {
            snprintf(alt, sizeof alt, "pci%02x:%02x.%x", bus, dev, fn);
            d = dev_by_name(alt);
        }
    }
    if (!d) {
        snprintf(why, cap,
                 "no device named \"%s\"; driver_targets lists the ones a program "
                 "can be run against", name);
        return -1;
    }

    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", d->obj.name ? d->obj.name : "?");
    t->node = d;

    if (!node_pci_addr(d, &t->bus, &t->dev, &t->fn)) {
        snprintf(why, cap,
                 "\"%s\" is not a PCI device, so it has no BARs to derive a sandbox "
                 "from; only PCI functions can be driven by a program", t->name);
        return -1;
    }
    target_read_ids(t);
    if (t->vendor == 0xFFFF || t->vendor == 0x0000) {
        snprintf(why, cap,
                 "%s reads back vendor 0x%04x - nothing is answering at that address",
                 t->name, (unsigned)t->vendor);
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* BAR decoding and window derivation                                     */
/* ====================================================================== */

typedef struct {
    uint32_t raw;
    int      present;
    int      is_io;
    int      is64;
    uint64_t base;
} bar_t;

static void decode_bar(uint8_t bus, uint8_t dev, uint8_t fn, int idx, bar_t *b) {
    memset(b, 0, sizeof *b);
    b->raw = pci_cfg_read32(bus, dev, fn, (uint8_t)(0x10 + idx * 4));
    if (b->raw == 0 || b->raw == 0xFFFFFFFFu) return;
    if (b->raw & 1) {
        b->is_io   = 1;
        b->base    = b->raw & ~0x3u;
        b->present = b->base != 0;
    } else {
        b->is64 = ((b->raw >> 1) & 3) == 2;
        b->base = b->raw & ~0xFu;
        if (b->is64 && idx < 5) {
            uint32_t hi = pci_cfg_read32(bus, dev, fn, (uint8_t)(0x10 + (idx + 1) * 4));
            b->base |= ((uint64_t)hi) << 32;
        }
        b->present = b->base != 0;
    }
}

/* Every BAR base on the machine, so one device's derived window can be stopped
 * at the next device's registers. Rebuilt per tool call: nothing here writes
 * config space, so this is a read-only photograph of the bus. */
static uint64_t g_io_base[DRV_BASES_MAX];
static int      g_n_io;
static uint64_t g_mem_base[DRV_BASES_MAX];
static int      g_n_mem;
static int      g_bases_overflow;
static int      g_bases_valid;

static void push_base(uint64_t *tab, int *n, uint64_t v) {
    for (int i = 0; i < *n; i++) if (tab[i] == v) return;
    if (*n >= DRV_BASES_MAX) { g_bases_overflow = 1; return; }
    tab[(*n)++] = v;
}

/* One scan per tool call: it is thousands of config reads, and a tool call is
 * the only thing that can change what is on the bus (nothing here writes config
 * space, and there is no hot-plug). */
static void bases_invalidate(void) { g_bases_valid = 0; }

static void scan_bar_bases(void) {
    if (g_bases_valid) return;
    g_bases_valid = 1;
    g_n_io = g_n_mem = 0;
    g_bases_overflow = 0;

    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint16_t vendor =
                    (uint16_t)(pci_cfg_read32((uint8_t)bus, (uint8_t)dev,
                                              (uint8_t)fn, 0x00) & 0xFFFF);
                if (vendor == 0xFFFF || vendor == 0x0000) {
                    if (fn == 0) break;          /* function 0 absent => no device */
                    continue;
                }
                for (int i = 0; i < 6; i++) {
                    bar_t b;
                    decode_bar((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, i, &b);
                    if (b.present)
                        push_base(b.is_io ? g_io_base : g_mem_base,
                                  b.is_io ? &g_n_io   : &g_n_mem, b.base);
                    if (b.is64) i++;
                }
            }
}

static uint64_t next_base_above(uint64_t base, int is_io) {
    const uint64_t *tab = is_io ? g_io_base : g_mem_base;
    int             n   = is_io ? g_n_io    : g_n_mem;
    uint64_t        best = 0;

    for (int i = 0; i < n; i++)
        if (tab[i] > base && (!best || tab[i] < best)) best = tab[i];

    /* The platform block behaves like the next device along. With windows
     * bounded by their own natural alignment this clamp can never actually
     * bind (an aligned block cannot cross a boundary aligned more coarsely
     * than itself), and a BAR *inside* the block is refused outright in
     * build_sandbox. It is here so that raising the cap, or ever sizing a
     * window some other way, cannot quietly hand out the IOAPIC. */
    if (!is_io && DRV_PLATFORM_LO > base && (!best || DRV_PLATFORM_LO < best))
        best = DRV_PLATFORM_LO;
    return best;
}

/* How much of a BAR to open. Smallest of: its natural alignment (a BAR is
 * always aligned to its own size, so this is an upper bound on that size), the
 * hard cap for its space, and the distance to the next base on the bus. */
static uint64_t window_size(uint64_t base, int is_io) {
    if (!base) return 0;
    uint64_t size = base & (~base + 1);              /* lowest set bit */
    uint64_t cap  = is_io ? DRV_IO_CAP
                          : (g_bases_overflow ? DRV_MMIO_CAP_TIGHT : DRV_MMIO_CAP);
    if (size > cap) size = cap;
    uint64_t next = next_base_above(base, is_io);
    if (next && next - base < size) size = next - base;
    return size;
}

/* ====================================================================== */
/* the sandbox                                                            */
/* ====================================================================== */

typedef struct {
    dvm_policy_t pol;
    uint64_t     args[DVM_NARGS]; /* see DVM_ARG_*: BARs, bdf, dma base+size */
    int          nargs;
    char         text[288];       /* "ports 0xd000-0xd0ff; mmio none; ..."   */
    int          nwin;
} sandbox_t;

/* Why a target cannot be driven, or 0 if it can. `why` is written either way:
 * on success it holds nothing, on failure a sentence for the model. */
static int target_blocked(const target_t *t, char *why, size_t cap) {
    /* The display adapter is the operator's console, and it is the ONE device
     * on this machine that no address-based rule can protect.
     *
     * vm/dvm.c's compiled-in deny list keeps a program off the VGA text buffer
     * at 0xB8000. But a display adapter's BARs are a SECOND decode path to the
     * same character/attribute memory (on the QEMU stdvga this kernel boots,
     * BAR0 is the VRAM linear aperture at 0xfd000000 and BAR2 offset 0x400 is
     * the legacy CRTC/sequencer register block). Deriving a window from those
     * BARs would hand out read-write access to the console at an address the
     * deny list cannot recognise as the console, and no owner check catches it
     * either: the kernel writes 0xB8000 directly, so the framebuffer has no
     * device_t and dev_by_resource() finds nothing to object to.
     *
     * A program with that window can repaint the transcript - including
     * painting a '[' in column zero and forging a line of kernel ground truth,
     * which is the one property lib/trace.c exists to guarantee. Refusing the
     * whole device class is the only rule that is true at every address it
     * might appear at, and it is what makes this tool's promise ("a program can
     * only reach the device you named - not ... the console") honest.
     *
     * Cost, stated plainly: an unclaimed GPU cannot be driven from here. That
     * is the right trade while the console is the only human interface. A
     * kernel that owns its framebuffer through a real driver, and can redraw
     * the transcript after a program has scribbled on it, could revisit this. */
    if (t->cls == PCI_CLASS_DISPLAY) {
        snprintf(why, cap,
                 "%s is a display controller (class %02x) and no display device is "
                 "targetable: the display is the operator's console, and its BARs "
                 "reach the same character memory the deny list protects at 0xb8000, "
                 "so a program there could erase the record of what it did",
                 t->name, (unsigned)t->cls);
        return 1;
    }

    if (t->node && t->node->driver) {
        snprintf(why, cap,
                 "driver \"%s\" is already bound to %s; running a program against a "
                 "claimed device would race the driver that owns it",
                 t->node->driver->name ? t->node->driver->name : "?", t->name);
        return 1;
    }

    /* ...and the same question asked of the hardware rather than the node. */
    for (int i = 0; i < 6; i++) {
        bar_t b;
        decode_bar(t->bus, t->dev, t->fn, i, &b);
        if (!b.present) { if (b.is64) i++; continue; }
        device_t *owner = dev_by_resource(b.is_io ? RES_IOPORT : RES_MMIO, b.base);
        if (owner) {
            snprintf(why, cap,
                     "%s BAR%d (0x%lx) is already driven by \"%s\" as device \"%s\"; "
                     "a program there would race a live driver",
                     t->name, i, (unsigned long)b.base,
                     owner->driver && owner->driver->name ? owner->driver->name : "?",
                     owner->obj.name ? owner->obj.name : "?");
            return 1;
        }
        if (b.is64) i++;
    }

    /* A device that is ALREADY bus-mastering used to be refused here, on the
     * grounds that the VM cannot police DMA. It is now targetable, because a
     * driver that cannot DMA cannot play a sound, and because refusing it did not
     * actually make the machine safe — it only made the capability unreachable.
     * What changed is that the kernel now sets the bit deliberately, for one
     * device, and takes it back if the program fails (see cmd_enable).
     *
     * The one thing that IS different about a device found already mastering: we
     * did not set that bit, so we must not clear it either. Something else — the
     * firmware, or an earlier successful program — put it there, and clearing it
     * could stop a transfer already in flight. cmd_enable records only the bits it
     * added, which is exactly this distinction. */
    return 0;
}

/* ====================================================================== */
/* bus mastering: the kernel's one config-space write                      */
/* ====================================================================== */

/* What the run needs enabled, given the windows it was granted:
 *
 *   PCI_CMD_MASTER (bit 2) — REQUIRED for DMA, and the reason this function
 *     exists. With it clear, the host bridge does not forward the device's
 *     master transactions: the descriptor fetch never happens, the device reports
 *     no error, and the symptom is silence with a position counter stuck at zero.
 *     There is no way for the program to set it, because `pcicfg` reads only.
 *   PCI_CMD_IO (bit 0) — needed only if the sandbox granted port windows. With
 *     it clear the function does not decode I/O cycles at all, so every in/out
 *     the program makes reads 0xff and writes into the void.
 *   PCI_CMD_MEM (bit 1) — the same for memory BARs.
 *
 * Deliberately NOT touched: bit 10 (Interrupt Disable). IF is 0 and every PIC
 * line is masked on this machine, so an asserted INTx is delivered nowhere and
 * disabling it would buy nothing while widening the set of bits that have to be
 * restored correctly. Status bits are not touched either — see PCI_CMD_REG.
 *
 * Returns the bits this call ADDED and that read back as set. `before` gets the
 * command register as it was found. A bit that was already set is not in the
 * return value, so cmd_restore() cannot clear something it did not enable. */
static uint16_t cmd_enable(const target_t *t, uint16_t want, uint16_t *before) {
    uint32_t dw  = pci_cfg_read32(t->bus, t->dev, t->fn, PCI_CMD_REG);
    uint16_t cmd = (uint16_t)(dw & 0xFFFFu);
    if (before) *before = cmd;

    uint16_t add = (uint16_t)(want & ~cmd);
    if (!add) return 0;

    pci_cfg_write32(t->bus, t->dev, t->fn, PCI_CMD_REG, (uint32_t)(cmd | add));

    /* Read back rather than assume. A bit that does not stick means the function
     * does not implement that capability (a device with no BARs of that kind, or
     * no master capability at all), and reporting "we enabled it" would send the
     * model looking for a bug in its program instead of in its assumptions. */
    uint16_t got = (uint16_t)(pci_cfg_read32(t->bus, t->dev, t->fn, PCI_CMD_REG) & 0xFFFFu);
    return (uint16_t)(got & add);
}

/* Undo exactly `added`, and nothing else. Called only when the program failed:
 * a program that reached `halt` may have left the device happily DMAing a tone
 * out of the buffer, and clearing bus mastering underneath it would cut the sound
 * off in the middle — which is the one outcome the operator would read as "it
 * did not work". */
static void cmd_restore(const target_t *t, uint16_t added) {
    if (!added) return;
    uint32_t dw  = pci_cfg_read32(t->bus, t->dev, t->fn, PCI_CMD_REG);
    uint16_t cmd = (uint16_t)(dw & 0xFFFFu);
    pci_cfg_write32(t->bus, t->dev, t->fn, PCI_CMD_REG,
                    (uint32_t)(cmd & (uint16_t)~added));
}

static void put_cmd_bits(char *buf, size_t cap, uint16_t bits) {
    int n = 0;
    buf[0] = '\0';
    if (bits & PCI_CMD_IO)     n = catf(buf, cap, n, "%sio(bit0)", n ? "+" : "");
    if (bits & PCI_CMD_MEM)    n = catf(buf, cap, n, "%smem(bit1)", n ? "+" : "");
    if (bits & PCI_CMD_MASTER) n = catf(buf, cap, n, "%sbus-master(bit2)", n ? "+" : "");
    if (!n) catf(buf, cap, 0, "none");
}

/* THE SYSCALLS A PROGRAM GETS, one line of justification each. Every one of
 * them is a hole; the point of listing them here rather than looping over the
 * enum is that adding a syscall to dvm.h must not silently grant it.
 *
 *   con.write   the program's own voice. It goes out through trace.c, so it
 *               cannot forge a kernel line, and it is charged to max_prints.
 *   fs.read     a program that can keep what it computed, and read it back on
 *   fs.write    a later turn, is the difference between a machine that learns
 *   fs.size     and one that starts over. Confined to DRV_FS_ROOT.
 *   audio.tone  the machine can already make a sound; a program that can ask
 *               for one can report progress without the operator reading a log.
 *               CAVEAT, and it is not a small one: if the machine's audio sink
 *               is itself a driver program (the normal outcome of this tool),
 *               servicing this syscall would mean running a second program
 *               while this one is mid-flight, and dvm_run() refuses that with
 *               DVM_TRAP_REENTRY. So audio.tone works from a program only while
 *               the sink is native C. That refusal is the correct answer rather
 *               than a limitation to route around: the two programs would share
 *               one scratch arena and one pair of DMA guard bands, and the inner
 *               one silently erased both for the outer one.
 *   time.ms     a timeout that is a real timeout rather than a loop count.
 *
 * NOT granted, because it does not exist: anything that reaches the network.
 * See include/dvm.h's FUTURE EXTENSION POINTS. */
static void grant_syscalls(dvm_policy_t *pol, char *why, size_t cap) {
    dvm_policy_allow_sys(pol, DVM_SYS_CON_WRITE);
    dvm_policy_allow_sys(pol, DVM_SYS_FS_READ);
    dvm_policy_allow_sys(pol, DVM_SYS_FS_WRITE);
    dvm_policy_allow_sys(pol, DVM_SYS_FS_SIZE);
    dvm_policy_allow_sys(pol, DVM_SYS_AUDIO_TONE);
    dvm_policy_allow_sys(pol, DVM_SYS_TIME_MS);
    if (dvm_policy_set_fs_root(pol, DRV_FS_ROOT) != 0) {
        /* Unreachable with a literal constant, and checked anyway: an fs
         * syscall with no root is a policy error one layer down, and finding
         * out here beats finding out in dvm_run(). */
        snprintf(why, cap, "the VM refused \"%s\" as a filesystem root, which is a "
                           "kernel bug", DRV_FS_ROOT);
    }
    pol->max_sys       = DRV_MAX_SYS;
    pol->max_mem_bytes = DRV_MAX_MEM_BYTES;
}

/* The one directory a program may write in has to exist before it can. mkdir is
 * idempotent enough for this: an existing directory returns VFS_EEXIST and the
 * program's own fs.write is where a real failure gets reported, with the path
 * the program actually named. */
static void ensure_fs_root(void) {
    static int done;
    if (done) return;
    done = 1;
    vfs_mkdir(DRV_FS_ROOT);
}

/* The budgets and grants that do not depend on a device: scratch memory, the
 * syscalls, and the counters every run shares. Split out so that a program with
 * no target gets exactly the same non-device machine as one with a target. */
static void base_sandbox(sandbox_t *sb, uint64_t max_steps, uint64_t delay_us,
                         dvm_trace_t trace, char *why, size_t cap) {
    /* Cleared HERE, not by the callers: `why` arrives holding whatever the last
     * function to fail wrote into it, and the two lines below read it as "did
     * grant_syscalls object". */
    if (cap) why[0] = '\0';
    memset(sb, 0, sizeof *sb);
    dvm_policy_init(&sb->pol);
    sb->pol.max_steps           = max_steps;
    sb->pol.max_io              = DRV_MAX_IO;
    sb->pol.max_dma_ops         = DRV_MAX_DMA_OPS;
    sb->pol.max_delay_us        = delay_us;
    sb->pol.max_prints          = DRV_PRINTS;
    sb->pol.trace               = trace;
    sb->pol.max_trace           = (trace == DVM_TRACE_ALL) ? DRV_TRACE_ALL_LINES
                                                           : DRV_TRACE_IO_LINES;
    grant_syscalls(&sb->pol, why, cap);
}

/* A program with no device at all. No ports, no MMIO, no PCI function and no
 * DMA grant — which is not a weakened driver sandbox but the absence of one,
 * and is why this is the shape to run anything that is not a driver in. It
 * still has the scratch arena, the strings and the syscalls, because those are
 * properties of the machine rather than grants. */
static int build_software_sandbox(sandbox_t *sb, uint64_t max_steps,
                                  uint64_t delay_us, dvm_trace_t trace,
                                  char *why, size_t cap) {
    base_sandbox(sb, max_steps, delay_us, trace, why, cap);
    if (why[0]) return -1;
    sb->nargs = 0;
    snprintf(sb->text, sizeof sb->text,
             "no device; %u bytes of scratch memory; syscalls under %s",
             (unsigned)DVM_MEM_SIZE, DRV_FS_ROOT);

    char pmsg[DVM_MSG_MAX];
    if (dvm_policy_check(&sb->pol, pmsg, sizeof pmsg) != DVM_OK) {
        snprintf(why, cap, "the VM refused a sandbox with no device in it, which is "
                           "a kernel bug: %s", pmsg);
        return -1;
    }
    return 0;
}

/* Build the policy from the target's BARs. Returns 0, or -1 with `why` set. */
static int build_sandbox(const target_t *t, sandbox_t *sb,
                         uint64_t max_steps, uint64_t delay_us, dvm_trace_t trace,
                         char *why, size_t cap) {
    base_sandbox(sb, max_steps, delay_us, trace, why, cap);
    if (why[0]) return -1;

    scan_bar_bases();

    char ports[96], mmio[96];
    int  np = 0, nm = 0;
    ports[0] = mmio[0] = '\0';

    for (int i = 0; i < 6; i++) {
        bar_t b;
        decode_bar(t->bus, t->dev, t->fn, i, &b);
        if (!b.present) { if (b.is64) i++; continue; }

        if (!b.is_io && b.base >= DRV_PLATFORM_LO && b.base < DRV_PLATFORM_HI) {
            snprintf(why, cap,
                     "%s BAR%d sits at 0x%lx, inside the interrupt-controller block "
                     "(0x%lx-0x%lx). Nothing may be granted there",
                     t->name, i, (unsigned long)b.base,
                     (unsigned long)DRV_PLATFORM_LO, (unsigned long)DRV_PLATFORM_HI - 1);
            return -1;
        }

        /* x86 I/O space is 16 bits wide however many bits the BAR register
         * holds. A base above that decodes nowhere, and truncating it to fit
         * would open a window somewhere else entirely. */
        if (b.is_io && b.base > 0xFFFFull) {
            snprintf(why, cap,
                     "%s BAR%d claims I/O at 0x%lx, which is outside the 64 KiB "
                     "x86 I/O space; this device is not addressable",
                     t->name, i, (unsigned long)b.base);
            return -1;
        }

        uint64_t size = window_size(b.base, b.is_io);
        if (!size) { if (b.is64) i++; continue; }

        if (b.is_io) {
            if (b.base + size - 1 > 0xFFFFull) size = 0x10000ull - b.base;
            if (dvm_policy_allow_ports(&sb->pol, (uint16_t)b.base,
                                       (uint16_t)(b.base + size - 1)) != 0) {
                snprintf(why, cap, "%s has more I/O windows than the sandbox can hold",
                         t->name);
                return -1;
            }
            np = catf(ports, sizeof ports, np, "%s0x%lx-0x%lx",
                      np ? "," : "", (unsigned long)b.base,
                      (unsigned long)(b.base + size - 1));
        } else {
            if (dvm_policy_allow_mmio(&sb->pol, b.base, size, DVM_MMIO_RW) != 0) {
                snprintf(why, cap, "%s has more memory windows than the sandbox can hold",
                         t->name);
                return -1;
            }
            nm = catf(mmio, sizeof mmio, nm, "%s0x%lx+0x%lx",
                      nm ? "," : "", (unsigned long)b.base, (unsigned long)size);
        }
        /* i is 0..5 here — the loop bound is 6 and the 64-bit skip below has not
         * run yet — so this test cannot currently bind. Kept deliberately: it is
         * the bound on a write into args[] from an index the BAR walk advances in
         * two places, and a guard on an array write in the code that derives the
         * sandbox is worth more than the line it costs. */
        if (i < 6) sb->args[DVM_ARG_BAR0 + i] = b.base;
        sb->nwin++;
        if (b.is64) i++;
    }

    if (!sb->nwin) {
        snprintf(why, cap,
                 "%s has no programmed BAR: there are no registers to reach, so "
                 "there is nothing a driver program could do with it", t->name);
        return -1;
    }

    dvm_policy_allow_pci(&sb->pol, t->bus, t->dev, t->fn);
    sb->args[DVM_ARG_BDF] =
        (uint64_t)(((uint32_t)t->bus << 8) | ((uint32_t)t->dev << 3) | t->fn);

    /* The DMA scratch buffer. Every program gets the whole arena: exactly one
     * program runs at a time on this machine, so there is nothing to divide it
     * between, and a fixed address and size are one less thing for a model to get
     * wrong across a retry. The address comes from dvm.c — this file never
     * computes one, which is why dvm_policy_allow_dma() cannot be talked into a
     * window on the heap. */
    uint64_t dma_base = 0, dma_size = 0;
    dvm_dma_region(&dma_base, &dma_size);
    if (dvm_policy_allow_dma(&sb->pol, dma_base, dma_size) != 0) {
        snprintf(why, cap,
                 "the VM refused to grant its own dma scratch buffer (0x%lx+0x%lx), "
                 "which is a kernel bug, not a problem with your program",
                 (unsigned long)dma_base, (unsigned long)dma_size);
        return -1;
    }
    sb->args[DVM_ARG_DMA_BASE] = dma_base;
    sb->args[DVM_ARG_DMA_SIZE] = dma_size;
    sb->nargs = DVM_NARGS;

    /* The scratch arena and the syscalls are deliberately NOT named here. They
     * are identical for every target — they are properties of the machine, not
     * of this device — and driver_targets prints one of these lines per device
     * into a 1 KiB result. Repeating 34 bytes of unchanging text per row cost
     * two whole devices off the end of the list the first time it was tried.
     * They are stated once, in each tool's footer. */
    snprintf(sb->text, sizeof sb->text,
             "ports %s; mmio %s; pci %02x:%02x.%x; dma 0x%lx+0x%lx",
             np ? ports : "none", nm ? mmio : "none", t->bus, t->dev, t->fn,
             (unsigned long)dma_base, (unsigned long)dma_size);

    /* The VM's deny list gets the last word: a BAR overlapping COM1 or the PIT
     * makes the whole device untargetable, and it says so in its own words. */
    char pmsg[DVM_MSG_MAX];
    if (dvm_policy_check(&sb->pol, pmsg, sizeof pmsg) != DVM_OK) {
        snprintf(why, cap, "the sandbox %s would need is not permitted: %s",
                 t->name, pmsg);
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* the recording I/O backend                                              */
/* ====================================================================== */

/* Everything the program did to the device, in order, as it happened. dvm.c
 * prints its own trace to the console for the operator; this log exists so the
 * MODEL can read back what its program actually caused, which is the input to
 * the next attempt. */

typedef enum { A_PW, A_PR, A_MW, A_MR, A_PCI, A_DLY } akind_t;

typedef struct {
    uint8_t  kind;
    uint8_t  width;
    uint64_t addr;      /* port, MMIO address, or (bdf << 8) | offset */
    uint32_t val;
} acc_t;

static acc_t    g_log[DRV_LOG_MAX];      /* ring: the LAST DRV_LOG_MAX events */
static uint32_t g_log_n;                 /* entries held (<= DRV_LOG_MAX)     */
static uint64_t g_log_total;             /* events that happened              */

static const dvm_io_t *g_inner;
static dvm_io_t        g_recorder;

static void log_acc(akind_t k, uint64_t addr, uint8_t width, uint32_t val) {
    uint32_t slot = (uint32_t)(g_log_total % DRV_LOG_MAX);
    g_log[slot].kind  = (uint8_t)k;
    g_log[slot].width = width;
    g_log[slot].addr  = addr;
    g_log[slot].val   = val;
    g_log_total++;
    if (g_log_n < DRV_LOG_MAX) g_log_n++;
}

/* Global index (1-based, as the model sees it) of held entry `i`. */
static uint64_t log_index(uint32_t i) { return g_log_total - g_log_n + i + 1; }

static const acc_t *log_at(uint32_t i) {
    if (i >= g_log_n) return NULL;
    uint64_t first = g_log_total - g_log_n;
    return &g_log[(uint32_t)((first + i) % DRV_LOG_MAX)];
}

/* Writes are logged before they are issued and reads after, so the log always
 * contains the access that was in flight if a device ever wedges the machine. */
static void rec_port_write(void *ctx, uint16_t port, uint8_t width, uint32_t val) {
    (void)ctx;
    log_acc(A_PW, port, width, val);
    g_inner->port_write(g_inner->ctx, port, width, val);
}
static uint32_t rec_port_read(void *ctx, uint16_t port, uint8_t width) {
    (void)ctx;
    uint32_t v = g_inner->port_read(g_inner->ctx, port, width);
    log_acc(A_PR, port, width, v);
    return v;
}
static void rec_mmio_write(void *ctx, uint64_t addr, uint8_t width, uint32_t val) {
    (void)ctx;
    log_acc(A_MW, addr, width, val);
    g_inner->mmio_write(g_inner->ctx, addr, width, val);
}
static uint32_t rec_mmio_read(void *ctx, uint64_t addr, uint8_t width) {
    (void)ctx;
    uint32_t v = g_inner->mmio_read(g_inner->ctx, addr, width);
    log_acc(A_MR, addr, width, v);
    return v;
}
static uint32_t rec_pci_read32(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn,
                               uint8_t off) {
    (void)ctx;
    uint32_t v = g_inner->pci_read32(g_inner->ctx, bus, dev, fn, off);
    log_acc(A_PCI, ((uint64_t)(((uint32_t)bus << 8) | ((uint32_t)dev << 3) | fn) << 8)
                   | off, 4, v);
    return v;
}
static void rec_delay_us(void *ctx, uint32_t us) {
    (void)ctx;
    log_acc(A_DLY, 0, 0, us);
    g_inner->delay_us(g_inner->ctx, us);
}

/* Wrap `io`, hook for hook. A hook the real backend does not have must stay
 * NULL: dvm.c turns a missing hook into DVM_TRAP_NOIO, and a recorder that
 * filled it in would make a program believe an access happened. */
static const dvm_io_t *recorder_bind(const dvm_io_t *io) {
    g_log_n = 0;
    g_log_total = 0;
    memset(&g_recorder, 0, sizeof g_recorder);
    if (!io) return NULL;
    g_inner = io;
    if (io->port_write) g_recorder.port_write = rec_port_write;
    if (io->port_read)  g_recorder.port_read  = rec_port_read;
    if (io->mmio_write) g_recorder.mmio_write = rec_mmio_write;
    if (io->mmio_read)  g_recorder.mmio_read  = rec_mmio_read;
    if (io->pci_read32) g_recorder.pci_read32 = rec_pci_read32;
    if (io->delay_us)   g_recorder.delay_us   = rec_delay_us;
    /* Carried through unwrapped. A syscall is not a device access and does not
     * belong in the access log; dropping it here instead would silently take
     * the kernel away from every program driver_run has ever executed. */
    g_recorder.sys = io->sys;
    return &g_recorder;
}

/* The backends a run executes against. dvm_io_hardware() and dvm_sys_kernel()
 * are NULL on a host build, where there are no in/out instructions and no
 * kernel; tests substitute synthetic ones through the two setters below. */
static const dvm_io_t  *g_io_override;
static const dvm_sys_t *g_sys_override;

void dvm_tools_set_io(const dvm_io_t *io)   { g_io_override = io; }
void dvm_tools_set_sys(const dvm_sys_t *s)  { g_sys_override = s; }

static const dvm_sys_t *kernel_sys(void) {
    return g_sys_override ? g_sys_override : dvm_sys_kernel();
}

/* A device backend with the current kernel behind it. The override path has to
 * splice rather than replace: a test that supplies a synthetic DEVICE still
 * wants the syscall backend it separately supplied, and dvm_io_hardware()'s own
 * struct is const. */
static dvm_io_t g_bound_io;

static const dvm_io_t *hardware_io(void) {
    const dvm_io_t *base = g_io_override ? g_io_override : dvm_io_hardware();
    if (!base) return NULL;
    if (!g_sys_override) return base;
    g_bound_io = *base;
    g_bound_io.sys = g_sys_override;
    return &g_bound_io;
}

/* No device at all: every hook NULL, so every device opcode traps NO_BACKEND
 * rather than reaching something. The syscalls are all a software program has,
 * and that is the point. */
static dvm_io_t g_soft_io;

static const dvm_io_t *software_io(void) {
    memset(&g_soft_io, 0, sizeof g_soft_io);
    g_soft_io.sys = kernel_sys();
    return &g_soft_io;
}

/* ====================================================================== */
/* the attempt budget                                                     */
/* ====================================================================== */

static struct {
    char     name[DRV_NAME_MAX + 1];
    unsigned fails;
} g_attempt[DRV_TARGETS_TRACKED];

/* Forget every target's failure count. Not reachable by the model: it is the
 * kernel's to call (see FUTURE EXTENSION POINTS) and the tests'. */
void dvm_tools_reset_attempts(void) {
    memset(g_attempt, 0, sizeof g_attempt);
}

static unsigned attempts_used(const char *name) {
    for (int i = 0; i < DRV_TARGETS_TRACKED; i++)
        if (g_attempt[i].name[0] && name_eq(g_attempt[i].name, name))
            return g_attempt[i].fails;
    return 0;
}

static void attempts_record(const char *name, int ok) {
    int slot = -1, empty = -1, weakest = 0;

    for (int i = 0; i < DRV_TARGETS_TRACKED; i++) {
        if (g_attempt[i].name[0] && name_eq(g_attempt[i].name, name)) { slot = i; break; }
        if (empty < 0 && !g_attempt[i].name[0]) empty = i;
        if (g_attempt[i].fails < g_attempt[weakest].fails) weakest = i;
    }

    if (slot < 0) {
        /* The table is 8 entries and a ninth target has to evict somebody. It
         * must NOT be whichever target happens to sit in slot 0: attempts_used()
         * reports 0 for a name the table has forgotten, so evicting a target
         * that is close to its budget would hand its budget back, and naming
         * eight other devices once each would be a reset the model can perform
         * on itself. Evict the LEAST-failed entry instead, so the targets a
         * runaway conversation is actually burning are the ones that survive. */
        slot = (empty >= 0) ? empty : weakest;
        /* And start the new occupant from zero: inheriting the evicted entry's
         * count would refuse a device on its first failure (or, the other way
         * round, silently reattribute a still-broken target's history). */
        g_attempt[slot].fails = 0;
        snprintf(g_attempt[slot].name, sizeof g_attempt[slot].name, "%s", name);
    }

    g_attempt[slot].fails = ok ? 0 : g_attempt[slot].fails + 1;
}

/* ====================================================================== */
/* the last run, kept for driver_trace                                    */
/* ====================================================================== */

static struct {
    int          valid;
    char         target[DRV_NAME_MAX + 1];
    char         sandbox[sizeof ((sandbox_t *)0)->text];  /* never shorter */
    char         insn[80];              /* the stopping instruction, disassembled */
    char         srcline[112];          /* the model's own text for that line     */
    char         dma_msg[DVM_MSG_MAX];  /* "" unless a device overran the buffer  */
    uint32_t     ninsn;
    unsigned     attempt;
    uint16_t     cmd_before, cmd_added, cmd_cleared;
    dvm_result_t res;
} g_last;

/* The decoded program text. Static because 16 KB does not belong on a 64 KiB
 * kernel stack shared with lwIP and mbedTLS. */
static char   g_src[DRV_SRC_MAX + 1];
static size_t g_srclen;

/* Copy source line `line` (1-based) into dst, printable and bounded. This is
 * how the model gets its own text back next to the trap. */
static void source_line(uint32_t line, char *dst, size_t cap) {
    dst[0] = '\0';
    if (!line || !cap) return;

    size_t i = 0;
    uint32_t cur = 1;
    while (i < g_srclen && cur < line) {
        if (g_src[i] == '\n') cur++;
        i++;
    }
    if (cur != line) return;

    while (i < g_srclen && (g_src[i] == ' ' || g_src[i] == '\t')) i++;

    size_t o = 0;
    while (i < g_srclen && g_src[i] != '\n' && o + 1 < cap) {
        unsigned char c = (unsigned char)g_src[i++];
        dst[o++] = (c < 0x20 || c > 0x7E) ? ' ' : (char)c;
    }
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
}

/* ====================================================================== */
/* result formatting                                                      */
/* ====================================================================== */

static unsigned wbits(uint8_t w) { return w == 1 ? 8u : w == 2 ? 16u : 32u; }

static void put_acc(tool_result_t *r, uint64_t idx, const acc_t *a) {
    switch ((akind_t)a->kind) {
        case A_PW:
            tool_result_printf(r, "  %lu out%u 0x%04lx <= 0x%lx\n",
                               (unsigned long)idx, wbits(a->width),
                               (unsigned long)a->addr, (unsigned long)a->val);
            break;
        case A_PR:
            tool_result_printf(r, "  %lu in%u 0x%04lx => 0x%lx\n",
                               (unsigned long)idx, wbits(a->width),
                               (unsigned long)a->addr, (unsigned long)a->val);
            break;
        case A_MW:
            tool_result_printf(r, "  %lu st%u 0x%lx <= 0x%lx\n",
                               (unsigned long)idx, wbits(a->width),
                               (unsigned long)a->addr, (unsigned long)a->val);
            break;
        case A_MR:
            tool_result_printf(r, "  %lu ld%u 0x%lx => 0x%lx\n",
                               (unsigned long)idx, wbits(a->width),
                               (unsigned long)a->addr, (unsigned long)a->val);
            break;
        case A_PCI:
            tool_result_printf(r, "  %lu pcicfg %02lx:%02lx.%lx off 0x%02lx => 0x%08lx\n",
                               (unsigned long)idx,
                               (unsigned long)((a->addr >> 16) & 0xFF),
                               (unsigned long)((a->addr >> 11) & 0x1F),
                               (unsigned long)((a->addr >> 8) & 0x7),
                               (unsigned long)(a->addr & 0xFF),
                               (unsigned long)a->val);
            break;
        default:
            tool_result_printf(r, "  %lu delay %lu us\n",
                               (unsigned long)idx, (unsigned long)a->val);
            break;
    }
}

static void put_registers(tool_result_t *r, const dvm_result_t *res) {
    int any = 0;
    tool_result_printf(r, "regs:");
    for (int i = 0; i < DVM_NREGS; i++) {
        if (!res->reg[i]) continue;
        tool_result_printf(r, " r%d=0x%lx", i, (unsigned long)res->reg[i]);
        any = 1;
    }
    tool_result_printf(r, any ? " (others 0)\n" : " all zero\n");
}

static void put_counters(tool_result_t *r, const dvm_result_t *res) {
    tool_result_printf(r,
        "ran %lu steps, %lu device accesses (port rd %lu wr %lu, mmio rd %lu wr %lu, "
        "pci %lu), %lu dma buffer accesses (rd %lu wr %lu), %lu us delay, %lu prints, "
        "%lu scratch bytes touched, %lu syscalls\n",
        (unsigned long)res->steps, (unsigned long)res->io_ops,
        (unsigned long)res->n_port_rd, (unsigned long)res->n_port_wr,
        (unsigned long)res->n_mmio_rd, (unsigned long)res->n_mmio_wr,
        (unsigned long)res->n_pci_rd,
        (unsigned long)res->n_dma_rd + res->n_dma_wr,
        (unsigned long)res->n_dma_rd, (unsigned long)res->n_dma_wr,
        (unsigned long)res->delay_us, (unsigned long)res->prints,
        (unsigned long)res->mem_bytes, (unsigned long)res->n_sys);
    if (res->sys_msg[0])
        tool_result_printf(r, "last syscall failure: %s\n", res->sys_msg);
}

/* The start of the scratch arena, as text. A program's answer is often wider
 * than sixteen registers — a built request, a field it picked out — and this is
 * where it comes back. The arena still holds what the run left; the NEXT
 * dvm_run() is what zeroes it.
 *
 * Every byte here is model-authored, so every byte that is not printable ASCII
 * becomes '.', including the newlines: this text goes into a tool result that
 * is quoted back to the model and may be shown to the operator, and a '[' in
 * column zero is the one thing nothing model-controlled may ever produce. The
 * fixed prefix in front of it means column zero is never the program's. */
static void put_scratch(tool_result_t *r, const dvm_result_t *res) {
    if (!res->mem_hwm) return;
    unsigned n = res->mem_hwm < DRV_SCRATCH_SHOW ? res->mem_hwm : DRV_SCRATCH_SHOW;
    unsigned char raw[DRV_SCRATCH_SHOW];
    char          txt[DRV_SCRATCH_SHOW + 1];
    size_t got = dvm_mem_peek(0, raw, n);
    size_t i;
    for (i = 0; i < got; i++)
        txt[i] = (raw[i] >= 0x20 && raw[i] < 0x7F) ? (char)raw[i] : '.';
    txt[i] = '\0';
    tool_result_printf(r, "scratch[0..%lu) of %lu written: %s\n",
                       (unsigned long)got, (unsigned long)res->mem_hwm, txt);
}

/* What the KERNEL did to config space around this run. The model needs this for
 * two reasons: "bus mastering is on" is a precondition its program cannot check
 * for itself (pcicfg can read the bit but not set it), and if a run failed it
 * needs to know the bit is gone again before the next attempt. */
static void put_bme(tool_result_t *r, uint64_t dma_base,
                    uint16_t before, uint16_t added, uint16_t cleared) {
    char bits[48];
    put_cmd_bits(bits, sizeof bits, added);

    /* One line if nothing unusual happened. Every extra line here is a line of
     * access log the model does not get to read, so this stays terse. */
    tool_result_printf(r, "pci cmd 0x%04x -> 0x%04x: kernel set %s%s\n",
                       (unsigned)before, (unsigned)(before | added), bits,
                       (before & PCI_CMD_MASTER)
                           ? " (bus mastering was already enabled, so the kernel did not "
                             "set it and will not clear it)" : "");
    if (cleared) {
        char cb[48];
        put_cmd_bits(cb, sizeof cb, cleared);
        tool_result_printf(r, "  the run failed, so %s was cleared again\n", cb);
    } else if ((before | added) & PCI_CMD_MASTER) {
        tool_result_printf(r, "  this device can DMA now: r%d=0x%lx is the only address "
                              "it is safe to give it (no IOMMU)\n",
                           DVM_ARG_DMA_BASE, (unsigned long)dma_base);
    }
}

/* The tail of the log: the last thing that happened is what explains a trap. */
static void put_log_tail(tool_result_t *r) {
    if (!g_log_n) {
        tool_result_printf(r, "no device accesses were made\n");
        return;
    }
    uint32_t shown = 0;
    uint32_t first = 0;
    /* Count backwards from the end for as many rows as the budget allows. */
    for (uint32_t i = g_log_n; i > 0; i--) {
        if (r->len + DRV_ROW_MARGIN + (shown + 1) * 40 >= r->cap) break;
        first = i - 1;
        shown++;
    }
    if (!shown) return;

    if (shown < g_log_n)
        tool_result_printf(r, "last %lu of %lu device events "
                              "(driver_trace has the rest):\n",
                           (unsigned long)shown, (unsigned long)g_log_total);
    else
        tool_result_printf(r, "%lu device events:\n", (unsigned long)g_log_total);

    for (uint32_t i = first; i < g_log_n; i++) {
        const acc_t *a = log_at(i);
        if (!a) break;
        put_acc(r, log_index(i), a);
    }
}

/* ====================================================================== */
/* driver_targets                                                         */
/* ====================================================================== */

typedef struct {
    tool_result_t *r;
    int  limit;
    int  offset;              /* skip this many matching rows before printing */
    int  include_unusable;
    int  want_usable;         /* this pass prints usable rows / unusable rows */
    int  matched, shown, usable, stopped;
    int  skipped;             /* rows the offset stepped over                */
    int  eligible;            /* rows that would have printed given room     */
    int  out_of_room;         /* stopped because of BYTES, not because of limit */
} tlist_t;

static void target_row(device_t *d, void *ctx) {
    tlist_t *s = (tlist_t *)ctx;
    if (!d) return;

    target_t t;
    memset(&t, 0, sizeof t);
    snprintf(t.name, sizeof t.name, "%s", d->obj.name ? d->obj.name : "?");
    t.node = d;
    if (!node_pci_addr(d, &t.bus, &t.dev, &t.fn)) return;   /* not a PCI function */
    target_read_ids(&t);
    if (t.vendor == 0xFFFF || t.vendor == 0x0000) return;

    char why[DVM_MSG_MAX + 96];
    why[0] = '\0';

    sandbox_t sb;
    int ok = !target_blocked(&t, why, sizeof why) &&
             build_sandbox(&t, &sb, DRV_STEPS_DEF, DRV_DELAY_DEF_US,
                           DVM_TRACE_IO, why, sizeof why) == 0;

    /* Counting happens on the usable pass only, so a two-pass listing still
     * reports each function once. */
    if (s->want_usable) {
        s->matched++;
        if (ok) s->usable++;
    }
    if (ok != s->want_usable) return;
    if (!ok && !s->include_unusable) return;

    /* This row is one the caller asked for. Count it before deciding whether
     * there is room, so the footer can say exactly how many were left out. */
    s->eligible++;
    if (s->skipped < s->offset) { s->skipped++; return; }
    if (s->shown >= s->limit) { s->stopped = 1; return; }

    /* Room for the WHOLE row AND the whole footer, checked before a byte of the
     * row is written. Anything less and the result truncates mid-token and loses
     * the summary — see DRV_ROW_MAX. */
    if (s->r->truncated ||
        s->r->len + DRV_ROW_MAX + DRV_FOOTER_RESERVE >= s->r->cap) {
        s->stopped = 1;
        s->out_of_room = 1;
        return;
    }

    tool_result_printf(s->r, "target=%s %02x:%02x.%x %04x:%04x class=%02x:%02x\n",
                       t.name, t.bus, t.dev, t.fn,
                       (unsigned)t.vendor, (unsigned)t.device,
                       (unsigned)t.cls, (unsigned)t.sub);
    if (ok) {
        tool_result_printf(s->r, "  usable: yes  sandbox: %s\n", sb.text);
        tool_result_printf(s->r, "  preloaded:");
        for (int i = 0; i < 6; i++)
            if (sb.args[DVM_ARG_BAR0 + i])
                tool_result_printf(s->r, " r%d=0x%lx", i,
                                   (unsigned long)sb.args[DVM_ARG_BAR0 + i]);
        /* The addresses are already on the sandbox line above; what this line adds
         * is WHICH REGISTER each arrives in, which is the part a program needs. */
        tool_result_printf(s->r, " r%d=bdf 0x%lx r%d=dma 0x%lx r%d=0x%lx\n",
                           DVM_ARG_BDF, (unsigned long)sb.args[DVM_ARG_BDF],
                           DVM_ARG_DMA_BASE, (unsigned long)sb.args[DVM_ARG_DMA_BASE],
                           DVM_ARG_DMA_SIZE, (unsigned long)sb.args[DVM_ARG_DMA_SIZE]);
        {
            char bits[48];
            uint16_t want = PCI_CMD_MASTER |
                            (sb.pol.nport ? PCI_CMD_IO  : 0) |
                            (sb.pol.nmmio ? PCI_CMD_MEM : 0);
            put_cmd_bits(bits, sizeof bits, (uint16_t)(want & ~t.command));
            tool_result_printf(s->r, "  driver_run sets pci cmd %s (cmd 0x%04x%s)\n",
                               bits, (unsigned)t.command,
                               (t.command & PCI_CMD_MASTER) ? ", master already on" : "");
        }
        unsigned used = attempts_used(t.name);
        if (used)
            tool_result_printf(s->r, "  attempts used: %u of %d\n",
                               used, DRV_ATTEMPT_BUDGET);
    } else {
        tool_result_printf(s->r, "  usable: no - %s\n", why);
    }
    s->shown++;
}

static int t_driver_targets(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "driver_targets", "", "input must be a JSON object");

    tlist_t s;
    memset(&s, 0, sizeof s);
    s.r = r;
    s.limit = 16;

    int64_t lim;
    int rc = f_int(&in, "limit", 1, 64, &lim);
    if (rc < 0) return fail(r, TOOL_EINVAL, "driver_targets", "",
                            "\"limit\" must be an integer 1-64");
    if (rc == 1) s.limit = (int)lim;

    if (f_bool(&in, "include_unusable", &s.include_unusable) < 0)
        return fail(r, TOOL_EINVAL, "driver_targets", "",
                    "\"include_unusable\" must be a boolean");

    /* PAGING, because "raise limit" was advice that could not work. One row is up
     * to 384 bytes and a model's whole result is CHAT_TOOL_RESULT_CAP = 1024, so
     * two or three rows is the most any single answer can carry, whatever `limit`
     * says. Without an offset the functions past the first few were simply
     * unreachable — on a machine with several unclaimed devices the sound card
     * could be one of them. */
    int64_t off;
    rc = f_int(&in, "offset", 0, 4096, &off);
    if (rc < 0) return fail(r, TOOL_EINVAL, "driver_targets", "",
                            "\"offset\" must be an integer 0-4096");
    if (rc == 1) s.offset = (int)off;

    /* Usable targets first: the answer is clipped to the model's result budget,
     * and the row it can act on must not be the one that falls off the end. */
    bases_invalidate();
    s.want_usable = 1;
    device_foreach(target_row, &s);
    if (s.include_unusable) {
        s.want_usable = 0;
        device_foreach(target_row, &s);
    }

    /* THE FOOTER ALWAYS PRINTS. DRV_FOOTER_RESERVE is held back for exactly this,
     * because a listing that stops with no count and no reason is worse than a
     * short one: the model cannot tell the difference between "that is all there
     * is" and "there were ten more". Say how many, and say what to do — and the
     * advice has to be true. When the stop was caused by the BYTE budget, raising
     * "limit" changes nothing at all; the only thing that works is another call
     * with an "offset". */
    int not_shown = s.eligible - s.skipped - s.shown;
    if (not_shown < 0) not_shown = 0;
    if (not_shown > 0) {
        if (s.out_of_room)
            tool_result_printf(r,
                "...%d more matched but this result is full (%d bytes is the whole "
                "budget). Call again with \"offset\": %d - raising \"limit\" will "
                "not help\n",
                not_shown, (int)r->cap, s.offset + s.shown);
        else
            tool_result_printf(r, "...%d more not shown (raise \"limit\")\n",
                               not_shown);
    }
    tool_result_printf(r,
        "targets: %d PCI function(s) known, %d can be driven by a program, %d shown",
        s.matched, s.usable, s.shown);
    if (s.offset) tool_result_printf(r, " after skipping %d", s.skipped);
    tool_result_printf(r, "\n");
    /* Once, not per row: every program gets these, target or no target. */
    tool_result_printf(r,
        "every program also gets %u bytes of scratch memory and the syscalls under "
        "%s; driver_run with no \"target\" runs one with no device at all\n",
        (unsigned)DVM_MEM_SIZE, DRV_FS_ROOT);
    if (!s.usable)
        tool_result_printf(r,
            "none are targetable right now; add include_unusable=true to see why "
            "each one was rejected\n");

    trace_ret(s.usable, "driver_targets", "known=%d usable=%d", s.matched, s.usable);
    return TOOL_OK;
}

static const tool_t driver_targets_tool = {
    .name = "driver_targets",
    .description =
        "List the devices a driver program may be run against. For each one: the "
        "sandbox it would get (ports and MMIO derived from that device's own "
        "BARs), the PCI function opened for pcicfg, the DMA buffer, the registers "
        "preloaded before the first instruction, and the PCI command bits the "
        "kernel will set. include_unusable=true also lists what cannot be driven "
        "and why: claimed by a driver, no programmed BAR, a display controller "
        "(the operator's console), or a BAR the VM never allows. A result holds two "
        "or three rows; if it says more matched, call again with \"offset\".",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"include_unusable\":{\"type\":\"boolean\",\"description\":\"also list devices that cannot be targeted, with the reason\"},"
        "\"limit\":{\"type\":\"integer\",\"description\":\"max rows, 1-64 (default 16)\"},"
        "\"offset\":{\"type\":\"integer\",\"description\":\"skip this many rows: pages a listing too big for one result\"}"
        "},\"required\":[]}",
    .flags  = 0,
    .invoke = t_driver_targets,
};
REGISTER_TOOL(driver_targets_tool);

/* ====================================================================== */
/* reading the "source" argument                                          */
/* ====================================================================== */

/* Decode `source` into g_src. Returns 0, or a TOOL_* code after writing the
 * error result itself. */
static int want_source(const json_value_t *in, tool_result_t *r, const char *op,
                       const char *args) {
    g_src[0] = '\0';
    g_srclen = 0;

    size_t n  = 0;
    int    rc = f_str(in, "source", g_src, sizeof g_src, &n);
    if (rc == 0)
        return fail(r, TOOL_EINVAL, op, args,
                    "\"source\" is required: the program text, one instruction per line");
    if (rc == -1)
        return fail(r, TOOL_EINVAL, op, args, "\"source\" must be a string");
    if (rc == -2)
        return fail(r, TOOL_EINVAL, op, args,
                    "\"source\" is longer than %d bytes, which is the whole program "
                    "budget; a bring-up sequence should be well under it", DRV_SRC_MAX);

    g_srclen = n;
    if (n == 0)
        return fail(r, TOOL_EINVAL, op, args, "\"source\" is empty: there is no program to run");
    return 0;
}

/* Report an assembler failure the way the model can act on: line number, the
 * assembler's own words, and the line it wrote. */
static void put_asm_error(tool_result_t *r, const dvm_asm_err_t *e) {
    if (e->line > 0) {
        char text[112];
        source_line((uint32_t)e->line, text, sizeof text);
        tool_result_printf(r, "assembly failed at line %d: %s\n", e->line, e->msg);
        if (text[0]) tool_result_printf(r, "  line %d: %s\n", e->line, text);
    } else {
        tool_result_printf(r, "assembly failed: %s\n", e->msg);
    }
}

/* ====================================================================== */
/* driver_assemble                                                        */
/* ====================================================================== */

static int t_driver_assemble(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "driver_assemble", "", "input must be a JSON object");

    int rc = want_source(&in, r, "driver_assemble", "");
    if (rc != 0) return rc;

    int listing = 0;
    if (f_bool(&in, "listing", &listing) < 0)
        return fail(r, TOOL_EINVAL, "driver_assemble", "", "\"listing\" must be a boolean");

    dvm_program_t *prog = (dvm_program_t *)kmalloc(sizeof *prog);
    if (!prog)
        return fail(r, TOOL_ENOSPC, "driver_assemble", "",
                    "no memory for a program image right now");

    dvm_asm_err_t err;
    memset(&err, 0, sizeof err);
    if (dvm_assemble(g_src, g_srclen, prog, &err) != 0) {
        r->is_error = 1;
        put_asm_error(r, &err);
        tool_result_printf(r, "nothing was run; fix the line and try again\n");
        trace_err(TOOL_EINVAL, "driver_assemble", "line=%d", err.line);
        kfree(prog);
        return TOOL_EINVAL;
    }

    tool_result_printf(r, "ok: %lu instructions, %lu labels, %lu strings, "
                          "%lu source lines\n",
                       (unsigned long)prog->ninsn, (unsigned long)prog->nlabel,
                       (unsigned long)prog->nstr, (unsigned long)prog->src_lines);
    if (listing) {
        for (uint32_t pc = 0; pc < prog->ninsn && !room_low(r); pc++) {
            char text[80];
            if (!dvm_disasm(prog, pc, text, sizeof text)) break;
            tool_result_printf(r, "%3lu ln%-4lu %s\n", (unsigned long)pc,
                               (unsigned long)prog->insn[pc].line, text);
        }
        if (room_low(r)) tool_result_printf(r, "...listing clipped\n");
    }
    tool_result_printf(r, "this only parsed the program; driver_run executes it\n");

    trace_ret((long)prog->ninsn, "driver_assemble", "insns=%lu",
              (unsigned long)prog->ninsn);
    kfree(prog);
    return TOOL_OK;
}

static const tool_t driver_assemble_tool = {
    .name = "driver_assemble",
    .description =
        "Check that a program assembles. Touches no hardware and spends none of "
        "driver_run's attempts, so use it for every syntax question. Returns the "
        "instruction, label and string counts, or the first error with its line "
        "number and that line's text; listing=true adds a numbered disassembly, "
        "which confirms the assembler read an addressing form the way you meant.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"source\":{\"type\":\"string\",\"description\":\"the program text (see driver_run for the instruction set)\"},"
        "\"listing\":{\"type\":\"boolean\",\"description\":\"also print a numbered disassembly\"}"
        "},\"required\":[\"source\"]}",
    .flags  = 0,
    .invoke = t_driver_assemble,
};
REGISTER_TOOL(driver_assemble_tool);

/* ====================================================================== */
/* driver_run                                                             */
/* ====================================================================== */

static dvm_trace_t want_trace(const json_value_t *in, int *bad) {
    char t[16];
    *bad = 0;
    int rc = f_str(in, "trace", t, sizeof t, NULL);
    if (rc == 0) return DVM_TRACE_IO;
    if (rc < 0)  { *bad = 1; return DVM_TRACE_IO; }
    if (name_eq(t, "off")) return DVM_TRACE_OFF;
    if (name_eq(t, "io"))  return DVM_TRACE_IO;
    if (name_eq(t, "all")) return DVM_TRACE_ALL;
    *bad = 1;
    return DVM_TRACE_IO;
}

static int t_driver_run(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "driver_run", "", "input must be a JSON object");

    /* ---- the target names a device, and only a device ----
     *
     * OPTIONAL, and its absence is a mode rather than a default. With a name,
     * this is the driver tool it has always been. Without one there is no
     * device sandbox at all — no ports, no MMIO, no PCI function, no DMA — and
     * what is left is the scratch arena, the string instructions and the
     * syscalls. That is the shape for everything the model wants to build that
     * is not a driver, and it costs eleven bytes of schema rather than a
     * fiftieth tool's worth. */
    char name[DRV_NAME_MAX + 1];
    int  rc = f_str(&in, "target", name, sizeof name, NULL);
    if (rc == -1) return fail(r, TOOL_EINVAL, "driver_run", "", "\"target\" must be a string");
    if (rc == -2) return fail(r, TOOL_ENOENT, "driver_run", "<oversized>",
                              "no such device: a name longer than %d characters cannot "
                              "match anything registered", DRV_NAME_MAX);
    int software = (rc == 0);

    bases_invalidate();

    char why[DVM_MSG_MAX + 96];
    why[0] = '\0';
    target_t t;
    memset(&t, 0, sizeof t);
    if (software) {
        snprintf(t.name, sizeof t.name, "%s", DRV_SOFTWARE_NAME);
    } else {
        if (resolve_target(name, &t, why, sizeof why) != 0)
            return fail(r, TOOL_ENOENT, "driver_run", name, "%s", why);
        if (target_blocked(&t, why, sizeof why) != 0)
            return fail(r, TOOL_EINVAL, "driver_run", t.name, "%s", why);
    }

    /* ---- the retry budget, checked before anything is assembled ---- */
    unsigned used = attempts_used(t.name);
    if (used >= DRV_ATTEMPT_BUDGET)
        return fail(r, TOOL_EINVAL, "driver_run", t.name,
                    "%d attempts against %s have all failed and no more will be run. "
                    "Stop here: tell the operator what you tried, what the device "
                    "returned, and what you think is wrong with it",
                    DRV_ATTEMPT_BUDGET, t.name);

    /* ---- knobs, each one bounded ---- */
    int bad = 0;
    dvm_trace_t trace = want_trace(&in, &bad);
    if (bad) return fail(r, TOOL_EINVAL, "driver_run", t.name,
                         "\"trace\" must be \"off\", \"io\" or \"all\"");

    int64_t ms = 0, steps = 0;
    rc = f_int(&in, "delay_budget_ms", 0, DRV_DELAY_MAX_MS, &ms);
    if (rc == -1) return fail(r, TOOL_EINVAL, "driver_run", t.name,
                              "\"delay_budget_ms\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "driver_run", t.name,
                              "\"delay_budget_ms\" must be 0-%d; the VM will not spend "
                              "longer than that in one run", DRV_DELAY_MAX_MS);
    uint64_t delay_us = (rc == 1) ? (uint64_t)ms * 1000ull : DRV_DELAY_DEF_US;

    rc = f_int(&in, "max_steps", 1, DRV_STEPS_MAX, &steps);
    if (rc == -1) return fail(r, TOOL_EINVAL, "driver_run", t.name,
                              "\"max_steps\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "driver_run", t.name,
                              "\"max_steps\" must be 1-%lu", (unsigned long)DRV_STEPS_MAX);
    uint64_t max_steps = (rc == 1) ? (uint64_t)steps : DRV_STEPS_DEF;

    rc = want_source(&in, r, "driver_run", t.name);
    if (rc != 0) return rc;

    /* ---- the sandbox: derived from the device, or from nothing ---- */
    ensure_fs_root();
    sandbox_t sb;
    int built = software
        ? build_software_sandbox(&sb, max_steps, delay_us, trace, why, sizeof why)
        : build_sandbox(&t, &sb, max_steps, delay_us, trace, why, sizeof why);
    if (built != 0)
        return fail(r, TOOL_EINVAL, "driver_run", t.name, "%s", why);

    const dvm_io_t *io = software ? software_io() : hardware_io();
    if (!io)
        return fail(r, TOOL_EINVAL, "driver_run", t.name,
                    "this build has no hardware I/O backend, so no program can run");

    dvm_program_t *prog = (dvm_program_t *)kmalloc(sizeof *prog);
    if (!prog)
        return fail(r, TOOL_ENOSPC, "driver_run", t.name,
                    "no memory for a program image right now");

    /* Everything from here on counts as an attempt: the model named a real
     * device and asked for its program to be run against it. The recorder is
     * armed before the assembler runs so that driver_trace can never show a
     * previous run's accesses next to this attempt's verdict. */
    const dvm_io_t *rio = recorder_bind(io);
    memset(&g_last, 0, sizeof g_last);
    snprintf(g_last.target, sizeof g_last.target, "%s", t.name);
    snprintf(g_last.sandbox, sizeof g_last.sandbox, "%s", sb.text);
    g_last.attempt = used + 1;

    dvm_asm_err_t aerr;
    memset(&aerr, 0, sizeof aerr);
    if (dvm_assemble(g_src, g_srclen, prog, &aerr) != 0) {
        attempts_record(t.name, 0);
        r->is_error = 1;
        tool_result_printf(r, "target=%s attempt %u of %d\n",
                           t.name, g_last.attempt, DRV_ATTEMPT_BUDGET);
        put_asm_error(r, &aerr);
        tool_result_printf(r, "the device was not touched\n");
        g_last.valid = 1;
        g_last.res.status = DVM_TRAP_BADOP;
        g_last.res.line = (uint32_t)aerr.line;
        snprintf(g_last.res.msg, sizeof g_last.res.msg, "%s", aerr.msg);
        source_line((uint32_t)aerr.line, g_last.srcline, sizeof g_last.srcline);
        trace_err(TOOL_EINVAL, "driver_run", "%s attempt=%u asm line=%d",
                  t.name, g_last.attempt, aerr.line);
        kfree(prog);
        return TOOL_EINVAL;
    }
    g_last.ninsn = prog->ninsn;

    /* ---- the escalation, done by C for one device ----
     *
     * Bus mastering is what turns "a program that pokes registers" into "a
     * program that can play sound", and it is also the single most dangerous bit
     * on the machine: from here until it is cleared, this device can write
     * anywhere in the low 4 GiB that a value in one of its registers tells it to.
     * There is no IOMMU. So it is set here, in C, for THIS function only, after
     * the sandbox has been proved buildable, and it is undone below if the program
     * did not reach halt. The ISA still has no config-space write. */
    uint16_t cmd_before = 0;
    uint16_t cmd_added  = 0;
    if (!software) {
        uint16_t want_bits = PCI_CMD_MASTER |
                             (sb.pol.nport ? PCI_CMD_IO  : 0) |
                             (sb.pol.nmmio ? PCI_CMD_MEM : 0);
        cmd_added = cmd_enable(&t, want_bits, &cmd_before);
        char bits[48], had[48];
        put_cmd_bits(bits, sizeof bits, cmd_added);
        put_cmd_bits(had,  sizeof had,  (uint16_t)(want_bits & cmd_before));
        trace_ok("driver_run.bme", "%s cmd 0x%04x -> 0x%04x set=%s already=%s",
                 t.name, (unsigned)cmd_before, (unsigned)(cmd_before | cmd_added),
                 bits, had);
    }

    /* ---- run it ---- */
    dvm_result_t res;
    dvm_status_t st = dvm_run(prog, &sb.pol, rio, sb.args, sb.nargs, &res);

    /* Did a DEVICE write outside the buffer? This is the only check on this
     * machine that can notice, and it only notices a near miss. */
    char dma_msg[DVM_MSG_MAX];
    dma_msg[0] = '\0';
    int dma_overrun = dvm_dma_check(dma_msg, sizeof dma_msg);
    if (dma_overrun) trace_err(TOOL_EINVAL, "driver_run.dma", "%s %s", t.name, dma_msg);

    /* A program that halted may have left the device mid-transfer, and taking its
     * bus-master bit away now would stop the sound the run just started. A program
     * that trapped gets it taken back: whatever it did, it is not finished, and a
     * half-configured DMA engine with a live master bit is the one state worth
     * spending a config write to leave. */
    uint16_t cmd_cleared = 0;
    if (st != DVM_OK && cmd_added) {
        cmd_restore(&t, cmd_added);
        cmd_cleared = cmd_added;
        trace_ok("driver_run.bme", "%s program failed: cleared the bits the kernel set",
                 t.name);
    }

    g_last.valid       = 1;
    g_last.res         = res;
    g_last.cmd_before  = cmd_before;
    g_last.cmd_added   = cmd_added;
    g_last.cmd_cleared = cmd_cleared;
    snprintf(g_last.dma_msg, sizeof g_last.dma_msg, "%s", dma_msg);
    if (st != DVM_OK || res.line) {
        source_line(res.line, g_last.srcline, sizeof g_last.srcline);
        if (res.pc < prog->ninsn) dvm_disasm(prog, res.pc, g_last.insn, sizeof g_last.insn);
    }
    kfree(prog);

    /* A success clears the failure count, which is what makes the budget a
     * brake on a NON-CONVERGING conversation rather than a hard cap on work.
     * But "reached halt" is not the same as "made progress": the one-instruction
     * program `halt` reaches halt while touching nothing, so crediting it would
     * let the model clear its own budget between failures - and a bound the
     * model can clear is not a bound. Convergence requires that the program
     * actually spoke to the device. */
    /* An out-of-bounds DMA is a broken driver even when the program itself reached
     * halt: the device wrote outside the only buffer it was given. Crediting that
     * as progress would clear the attempt budget for a program that is corrupting
     * memory, which is the last program that should get unlimited retries. */
    /* A software program has no device to touch, so "it spoke to the device" is
     * the wrong test for it — but "it reached halt" alone would still let the
     * one-instruction program `halt` clear the budget between failures. What
     * counts as progress with no device is that it computed or asked for
     * SOMETHING: memory, a syscall. */
    int did_work = software ? (res.mem_bytes > 0 || res.n_sys > 0)
                            : (res.io_ops > 0);
    int converged = (st == DVM_OK) && did_work && !dma_overrun;
    attempts_record(t.name, converged);

    /* ---- what the model reads ---- */
    if (software)
        tool_result_printf(r, "no device (software program) attempt %u of %d\n",
                           g_last.attempt, DRV_ATTEMPT_BUDGET);
    else
        tool_result_printf(r, "target=%s %02x:%02x.%x %04x:%04x attempt %u of %d\n",
                           t.name, t.bus, t.dev, t.fn,
                           (unsigned)t.vendor, (unsigned)t.device,
                           g_last.attempt, DRV_ATTEMPT_BUDGET);
    tool_result_printf(r, "sandbox: %s\n", g_last.sandbox);
    if (!software) {
        tool_result_printf(r, "also: %u bytes of scratch memory (zeroed), syscalls "
                              "under %s\n", (unsigned)DVM_MEM_SIZE, DRV_FS_ROOT);
        put_bme(r, sb.args[DVM_ARG_DMA_BASE], cmd_before, cmd_added, cmd_cleared);
    }

    if (st == DVM_OK) {
        tool_result_printf(r, "status=OK - the program reached halt at line %lu\n",
                           (unsigned long)res.line);
        if (!converged)
            tool_result_printf(r, "note: it reached halt without %s, so this does not "
                                  "count as progress and the attempt was spent\n",
                               software ? "touching memory or calling the kernel once"
                                        : "touching the device once");
    } else {
        r->is_error = 1;
        tool_result_printf(r, "status=%s at line %lu (pc %lu)\n",
                           dvm_status_name(st), (unsigned long)res.line,
                           (unsigned long)res.pc);
        if (res.msg[0]) tool_result_printf(r, "  %s\n", res.msg);
        if (g_last.insn[0])    tool_result_printf(r, "  instruction: %s\n", g_last.insn);
        if (g_last.srcline[0]) tool_result_printf(r, "  line %lu: %s\n",
                                                  (unsigned long)res.line, g_last.srcline);
    }

    put_counters(r, &res);
    put_registers(r, &res);
    put_scratch(r, &res);
    if (dma_overrun) {
        r->is_error = 1;
        tool_result_printf(r, "DMA WENT OUT OF BOUNDS: %s. The guard band either side of "
                              "the buffer caught it, so nothing else was corrupted this "
                              "time, but a length or descriptor count you gave the device "
                              "is wrong.\n", dma_msg);
    }
    if (!software) put_log_tail(r);

    if (!converged) {
        unsigned used_now = attempts_used(t.name);
        unsigned left = (used_now >= DRV_ATTEMPT_BUDGET)
                            ? 0u : DRV_ATTEMPT_BUDGET - used_now;
        if (left)
            tool_result_printf(r, "%u attempt(s) left on this target. Read the "
                                  "accesses above: they are what the device really "
                                  "did, in order.\n", left);
        else
            tool_result_printf(r, "no attempts left on this target; stop and report "
                                  "to the operator.\n");
    }

    if (st == DVM_OK)
        trace_ok("driver_run", "%s attempt=%u insns=%lu steps=%lu io=%lu",
                 t.name, g_last.attempt, (unsigned long)g_last.ninsn,
                 (unsigned long)res.steps, (unsigned long)res.io_ops);
    else
        trace_err(TOOL_EINVAL, "driver_run", "%s attempt=%u %s line=%lu",
                  t.name, g_last.attempt, dvm_status_name(st), (unsigned long)res.line);

    return st == DVM_OK ? TOOL_OK : TOOL_EINVAL;
}

static const tool_t driver_run_tool = {
    .name = "driver_run",
    .description =
        "Write a program and run it on the kernel's bounded VM. With \"target\" it "
        "is a DEVICE DRIVER for one device from driver_targets, sandboxed to that "
        "device's own BARs and the DMA buffer and nothing else, writing to real "
        "hardware. WITHOUT \"target\" it has no device access at all: scratch "
        "memory, strings and syscalls only: how to build what is not a driver. "
        "Either way it cannot loop forever. On a trap you get the reason, "
        "the source line, that instruction disassembled, the registers, the start "
        "of scratch memory, and the access log with what the device really "
        "returned. Fix it and call again; 5 failed attempts each.\n"
        "MACHINE: 16 registers r0-r15, 64-bit, UNSIGNED; a 32-deep data stack; a "
        "16-deep call stack; 65536 bytes of scratch memory addressed by OFFSET, "
        "zeroed each run. WITH a target, ON ENTRY: r0-r5 = its BAR bases (0 "
        "if unused), r6 = its bdf, r7 = the PHYSICAL address of a 256 KiB DMA "
        "buffer, r8 = its size. Derive every address you hand hardware from r7 and "
        "bound every length by r8; the kernel turns on bus mastering, and with no "
        "IOMMU any other address really is written to.\n"
        "INSTRUCTIONS (a, b = register or number):\n"
        " halt | abort \"why\" | nop | print \"text\"[,a]\n"
        " mov rd,a | add|sub|mul|div|mod|and|or|xor|shl|shr rd,a,b | not rd,a\n"
        "   (`add r1,4` means `add r1,r1,4`)\n"
        " cmp a,b (unsigned) | test a,b (zero = ((a&b)==0))\n"
        " jmp|beq|bne|blt|ble|bgt|bge L | call L | ret | push a | pop rd\n"
        " out8|out16|out32 port,a | in8|in16|in32 rd,port\n"
        " ld8|ld16|ld32 rd,[base+disp] | st8|st16|st32 [base+disp],a  (MMIO or DMA, "
        "aligned)\n"
        " pcicfg rd,bdf,off (READ only; bdf may be 00:05.0) | delay us (a floor)\n"
        "SCRATCH MEMORY is a separate address space: byte offsets 0-65535, no "
        "alignment rule, little-endian, no device can see it.\n"
        " mld8|mld16|mld32|mld64 rd,[off] | mst8|mst16|mst32|mst64 [off],a\n"
        "STRINGS are (offset,length) pairs, never NUL-terminated: slicing is "
        "arithmetic. Every op that writes returns the END offset, so building is a "
        "chain.\n"
        " mstr rd,dst,\"text\" | mcpy rd,dst,src,len (overlap-safe) | mset "
        "rd,dst,byte,len | mitoa rd,dst,val,base\n"
        " mcmp a,b,len   compares len bytes and sets the flags, so beq/bne follow\n"
        " mfind rd,hay,len,\"text\" | mchr rd,hay,len,byte | matoi rd,off,len,base\n"
        "   these set the zero flag on success; rd = where it was found (hay+len if "
        "not) or the value\n"
        "SYSCALLS: push the arguments, then `sys rd,name`. Zero flag = it worked "
        "and rd = the result; else rd = an error code and the report has the "
        "reason. con.write(off,len) prints a line; fs.read(poff,plen,dst,cap), "
        "fs.write(poff,plen,src,len), fs.size(poff,plen) take paths under /vm only; "
        "audio.tone(hz,ms); time.ms().\n"
        "EXAMPLE - build a request, then read a status code out of a reply:\n"
        " mstr r1,0,\"GET /x HTTP/1.1\\r\\nHost: \"\n"
        " mstr r1,r1,\"h.example\\r\\n\\r\\n\" ; r1 = the request's length\n"
        " mstr r2,4096,\"HTTP/1.1 404 Not Found\" ; a reply at 4096\n"
        " sub r2,r2,4096 ; an END offset minus its start IS a length\n"
        " mchr r3,4096,r2,32 ; the space after that\n"
        " add r3,r3,1\n"
        " matoi r4,r3,3,10 ; r4 = 404, zero flag set\n"
        "SYNTAX: one instruction per line; `label:` marks a branch target; comments "
        "start ; # or //; commas optional; case-insensitive; numbers may be "
        "0x hex, 0b binary or contain _; `.equ NAME value` defines a constant.\n"
        "HOW TO WRITE ONE: poll with a COUNTED retry loop and delay as backoff, "
        "never an unbounded one; write a register, read it back, and `abort "
        "\"...\"` with a specific reason if it did not stick; build a device's "
        "descriptors in the buffer at r7; leave answers in the registers and at the "
        "start of scratch memory, because both come back to you.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"target\":{\"type\":\"string\",\"description\":\"device from driver_targets, e.g. \\\"pci00:05.0\\\"; omit it for a program with no device\"},"
        "\"source\":{\"type\":\"string\",\"description\":\"the program text, one instruction per line\"},"
        "\"trace\":{\"type\":\"string\",\"description\":\"console detail: \\\"off\\\", \\\"io\\\" (default, every device access) or \\\"all\\\" (every instruction)\"},"
        "\"delay_budget_ms\":{\"type\":\"integer\",\"description\":\"ms the program may spend in delay, 0-2000 (default 250)\"},"
        "\"max_steps\":{\"type\":\"integer\",\"description\":\"instruction budget, 1-1000000 (default 100000)\"}"
        "},\"required\":[\"source\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_driver_run,
};
REGISTER_TOOL(driver_run_tool);

/* ====================================================================== */
/* driver_install                                                         */
/* ====================================================================== */

/* THE WIRE BETWEEN "I BROUGHT THE CARD UP" AND "THE MACHINE CAN MAKE A SOUND".
 *
 * Why this exists. core/audio.c has had audio_register_vm() — a full, validated,
 * host-tested path for installing a model-authored play program as the machine's
 * audio sink — and nothing in the kernel called it. The consequence was not a
 * missing nicety: it was a dead end that a live model walked into. It found the
 * unclaimed card, brought it up correctly with driver_run, and then spent most of
 * a turn looking for the registration step that did not exist. driver_run leaves
 * a device configured and mastering; audio_tone needs a sink; there was no verb
 * that turned the first into the second. This tool is that verb.
 *
 * WHAT IT DOES NOT DO, deliberately: it does not run the program. Registration
 * validates the image (dvm_program_validate) and the sandbox (dvm_policy_check),
 * so a corrupt program or an impossible policy is refused here, while the model
 * can still fix it. But "does this program actually drive the device" is a
 * question only a real sound can answer, and the kernel refuses to answer it by
 * running the program against an EMPTY buffer: at install time r9/r10/r11 would
 * all be zero, so a correct program would be asked to play nothing and could
 * only look broken. The first audio_tone is the test, and its failure carries the
 * program's own `abort` text back to the model.
 *
 * THE SANDBOX IS THE SAME ONE. build_sandbox() is shared with driver_run, so a
 * play program is granted exactly what a bring-up program was granted for that
 * device, and audio.c re-checks that the DMA grant starts at the arena base and
 * covers the PCM — which is what makes r7 mean the same thing in both programs.
 *
 * THE PRIVILEGE IS THE SAME ONE, and it is now PERMANENT rather than for the
 * length of one run. cmd_enable() sets bus mastering on the named function and
 * this tool does NOT take it back on success, because the sink is expected to
 * DMA on every note from here on. There is still no IOMMU: an installed play
 * program that computes a wrong address DMAs over kernel memory on every sound,
 * not once. dvm_dma_check() after each play turns a near miss into a failed
 * sound; a far miss is not detectable on this machine. See this file's header.
 *
 * The trace console is turned OFF for an installed program on purpose: a sink is
 * re-run for every note, and a per-access trace would bury the operator's
 * transcript in device I/O the moment an app starts beeping. The access log the
 * recorder keeps is left alone for the same reason — driver_trace must go on
 * showing the last driver_run, not whatever the last note did. */

static int t_driver_install(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "driver_install", "", "input must be a JSON object");

    char name[DRV_NAME_MAX + 1];
    int  rc = f_str(&in, "target", name, sizeof name, NULL);
    if (rc == 0)  return fail(r, TOOL_EINVAL, "driver_install", "",
                              "\"target\" is required: the device this play program "
                              "drives, from driver_targets");
    if (rc == -1) return fail(r, TOOL_EINVAL, "driver_install", "",
                              "\"target\" must be a string");
    if (rc == -2) return fail(r, TOOL_ENOENT, "driver_install", "<oversized>",
                              "no such device: a name longer than %d characters cannot "
                              "match anything registered", DRV_NAME_MAX);

    bases_invalidate();

    char why[DVM_MSG_MAX + 96];
    target_t t;
    if (resolve_target(name, &t, why, sizeof why) != 0)
        return fail(r, TOOL_ENOENT, "driver_install", name, "%s", why);

    /* Re-installing on the card that is ALREADY the sink is the normal way to
     * correct a play program, and target_blocked() would otherwise refuse it —
     * the driver bound to that node is this tool's own previous install. So that
     * one case skips the check, and the release that makes it true happens far
     * below, after everything else has been validated, so a refused re-install
     * leaves the sink that works in place. */
    audio_status_t cur;
    audio_status(&cur);
    int replacing = (cur.kind != AUDIO_SINK_NONE && name_eq(cur.device, t.name));

    if (!replacing && target_blocked(&t, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "driver_install", t.name, "%s", why);

    /* ---- the format the model configured the device for ----
     *
     * Required rather than defaulted. The kernel cannot read a codec's sample
     * rate back out of a device it knows nothing about, so this number is the
     * model's testimony about what it programmed, and a wrong one is a sound at
     * the wrong pitch rather than an error. Making it explicit at least puts the
     * claim on the record. */
    int64_t rate = 0, ch = 0;
    rc = f_int(&in, "rate", AUDIO_RATE_MIN, AUDIO_RATE_MAX, &rate);
    if (rc == 0)  return fail(r, TOOL_EINVAL, "driver_install", t.name,
                              "\"rate\" is required: the sample rate in Hz you "
                              "configured the device for");
    if (rc == -1) return fail(r, TOOL_EINVAL, "driver_install", t.name,
                              "\"rate\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "driver_install", t.name,
                              "\"rate\" must be %lu-%lu Hz",
                              (unsigned long)AUDIO_RATE_MIN, (unsigned long)AUDIO_RATE_MAX);

    rc = f_int(&in, "channels", 1, 2, &ch);
    if (rc == 0)  return fail(r, TOOL_EINVAL, "driver_install", t.name,
                              "\"channels\" is required: 1 for mono or 2 for "
                              "interleaved stereo, as you configured the device");
    if (rc == -1) return fail(r, TOOL_EINVAL, "driver_install", t.name,
                              "\"channels\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "driver_install", t.name,
                              "\"channels\" must be 1 (mono) or 2 (stereo)");

    audio_format_t fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.rate_hz  = (uint32_t)rate;
    fmt.channels = (uint8_t)ch;
    fmt.bits     = 16;                 /* the only width the synthesiser emits */

    /* The sink's name in reports and in the device tree is fixed, not chosen. A
     * model-supplied one would be a schema property on every round for no
     * decision worth making — only one sink can exist at a time, and what the
     * operator needs to read in the device tree is what KIND of thing is driving
     * the card, not a label the model invented. */
    const char *sink = "vmaudio";

    rc = want_source(&in, r, "driver_install", t.name);
    if (rc != 0) return rc;

    /* The delay budget is the sink's business, not the model's: core/audio.c
     * raises it on a copy to cover the length of each sound (see run_vm), so a
     * number chosen here would be overwritten for every note anyway. What is
     * passed is the floor a bring-up-style poll loop needs before the first
     * sample is due. */
    /* The step budget is not negotiable here either, and unlike driver_run that is
     * a statement about what a play program IS. It points a device at samples
     * somebody else wrote and waits; DRV_STEPS_DEF is four orders of magnitude more
     * than that needs. A program that wants more is synthesising audio in the VM,
     * one st16 at a time, per note — which is the kernel's job and is already done
     * before this program is entered. */
    sandbox_t sb;
    if (build_sandbox(&t, &sb, DRV_STEPS_DEF, DRV_DELAY_DEF_US, DVM_TRACE_OFF,
                      why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "driver_install", t.name, "%s", why);

    /* AN INSTALLED SINK GETS NO SYSCALLS, and that is a different decision from
     * the one driver_run makes. driver_run grants them for the length of one
     * call the operator asked for; this program becomes RESIDENT and is re-run
     * for every note from here on, so a granted fs.write would be a standing
     * capability to rewrite a file on every beep. A play program points a
     * device at samples and waits — it has nothing to say to the kernel. */
    sb.pol.sys_allow  = 0;
    sb.pol.fs_root[0] = '\0';

    const dvm_io_t *io = hardware_io();
    if (!io)
        return fail(r, TOOL_EINVAL, "driver_install", t.name,
                    "this build has no hardware I/O backend, so no program can run");

    dvm_program_t *prog = (dvm_program_t *)kmalloc(sizeof *prog);
    if (!prog)
        return fail(r, TOOL_ENOSPC, "driver_install", t.name,
                    "no memory for a program image right now");

    dvm_asm_err_t aerr;
    memset(&aerr, 0, sizeof aerr);
    if (dvm_assemble(g_src, g_srclen, prog, &aerr) != 0) {
        r->is_error = 1;
        tool_result_printf(r, "target=%s: the play program was not installed\n", t.name);
        put_asm_error(r, &aerr);
        tool_result_printf(r, "the device was not touched and the audio sink is "
                              "unchanged\n");
        trace_err(TOOL_EINVAL, "driver_install", "%s asm line=%d", t.name, aerr.line);
        kfree(prog);
        return TOOL_EINVAL;
    }

    /* From here the machine changes. Order matters: drop the old sink only now,
     * so everything above could still have refused without costing the operator
     * a working one. */
    if (replacing) audio_release();

    uint16_t cmd_before = 0;
    uint16_t want_bits  = PCI_CMD_MASTER |
                          (sb.pol.nport ? PCI_CMD_IO  : 0) |
                          (sb.pol.nmmio ? PCI_CMD_MEM : 0);
    uint16_t cmd_added  = cmd_enable(&t, want_bits, &cmd_before);

    audio_vm_spec_t spec;
    memset(&spec, 0, sizeof spec);
    spec.program = prog;              /* audio.c COPIES the image */
    spec.policy  = sb.pol;
    spec.io      = io;
    for (int i = 0; i < 6; i++) spec.bar[i] = sb.args[DVM_ARG_BAR0 + i];
    spec.bdf     = sb.args[DVM_ARG_BDF];

    int arc = audio_register_vm(sink, t.node, &fmt, &spec);

    /* audio.c has its own copy now, so this one goes back. Read anything wanted
     * from the image BEFORE the free — reporting ninsn out of freed memory was
     * the first bug in this function. */
    uint32_t ninsn = prog->ninsn;
    kfree(prog);

    if (arc != AUDIO_OK) {
        if (cmd_added) cmd_restore(&t, cmd_added);
        r->is_error = 1;
        tool_result_printf(r, "target=%s: the audio service refused this play "
                              "program\n  %s\n", t.name, audio_last_error());
        if (replacing)
            tool_result_printf(r, "the sink that was installed on this device has "
                                  "been released, so nothing is registered now\n");
        else
            tool_result_printf(r, "the audio sink is unchanged\n");
        trace_err(TOOL_EINVAL, "driver_install", "%s refused", t.name);
        return TOOL_EINVAL;
    }

    /* ---- what the model reads ---- */
    tool_result_printf(r, "installed: \"%s\" is now this machine's audio sink, "
                          "playing through %s %02x:%02x.%x %04x:%04x\n",
                       sink, t.name, t.bus, t.dev, t.fn,
                       (unsigned)t.vendor, (unsigned)t.device);
    tool_result_printf(r, "sandbox: %s\n", sb.text);
    tool_result_printf(r, "program: %lu instructions, re-run from its first one for "
                          "every sound\n", (unsigned long)ninsn);
    tool_result_printf(r, "format: %lu Hz, %u channel(s), 16-bit signed "
                          "little-endian%s\n",
                       (unsigned long)fmt.rate_hz, (unsigned)fmt.channels,
                       fmt.channels == 2 ? ", interleaved" : "");
    {
        char bits[48];
        put_cmd_bits(bits, sizeof bits, cmd_added);
        tool_result_printf(r, "pci cmd 0x%04x -> 0x%04x: kernel set %s, and LEAVES it "
                              "set - this device bus-masters on every sound now\n",
                           (unsigned)cmd_before, (unsigned)(cmd_before | cmd_added),
                           cmd_added ? bits : "nothing (already enabled)");
    }
    tool_result_printf(r, "NOTHING HAS BEEN PLAYED YET. Registration checked the "
                          "image and the sandbox; whether the program really drives "
                          "the device is a question only a sound answers. Call "
                          "audio_tone next: if the program traps or reaches halt "
                          "without touching the device, you get its reason back and "
                          "can install a corrected one.\n");
    if (replacing)
        tool_result_printf(r, "(this replaced the previous sink on the same device)\n");

    /* `ninsn`, NOT prog->ninsn: prog was freed above. This line read the freed
     * image in the first live run and printed 0xDEDEDEDE (mm/heap.c's poison) as
     * an instruction count, inside a kernel trace line — the one kind of output
     * on this machine that is supposed to be ground truth. The result text beside
     * it was correct, which is exactly why it survived: two copies of the same
     * fact, one of them wrong. Pinned by test_install_lets_a_model_driver_play. */
    trace_ok("driver_install", "%s sink=%s insns=%lu %luHz/%uch",
             t.name, sink, (unsigned long)ninsn,
             (unsigned long)fmt.rate_hz, (unsigned)fmt.channels);
    return TOOL_OK;
}

/* THE DESCRIPTION IS DELIBERATELY SHORT, and that is a budget decision worth
 * writing down rather than a lack of things to say. Every tool's schema is sent
 * on EVERY round (see chat.h's cliff note: the registry plus a full history plus
 * the system prompt has ~84 bytes of slack inside CHAT_REQ_BYTES), so a kilobyte
 * spent teaching the play contract here is a kilobyte spent on every turn of
 * every conversation, including the ones that never touch audio. The contract is
 * therefore taught by audio_status's RESULT, which costs nothing until a model
 * actually asks. This text has one job: make the step discoverable and name the
 * tool that explains it. */
static const tool_t driver_install_tool = {
    .name = "driver_install",
    .description =
        "Install a play program as this machine's audio sink: this is what makes "
        "audio_tone and audio_melody work at all. Bring the card up with driver_run "
        "first, then call audio_status for the PLAY CONTRACT and write the short "
        "second program it describes - the kernel has already written the samples, "
        "so it only points the device at them and waits. Same ISA and sandbox as "
        "driver_run. Installing does not play; the first audio_tone does, and gives "
        "back the reason if the program fails.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"target\":{\"type\":\"string\",\"description\":\"the device, from driver_targets\"},"
        "\"source\":{\"type\":\"string\",\"description\":\"the play program\"},"
        "\"rate\":{\"type\":\"integer\",\"description\":\"sample rate Hz you configured the device for\"},"
        "\"channels\":{\"type\":\"integer\",\"description\":\"1 mono or 2 stereo, as configured\"}"
        "},\"required\":[\"target\",\"source\",\"rate\",\"channels\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_driver_install,
};
REGISTER_TOOL(driver_install_tool);

/* ====================================================================== */
/* driver_trace                                                           */
/* ====================================================================== */

static int t_driver_trace(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "driver_trace", "", "input must be a JSON object");

    if (!g_last.valid)
        return fail(r, TOOL_ENOENT, "driver_trace", "",
                    "no program has been run yet, so there is no trace to read");

    int64_t off = 0, lim = 24;
    int rc = f_int(&in, "offset", 0, 0x7FFFFFFF, &off);
    if (rc < 0) return fail(r, TOOL_EINVAL, "driver_trace", "",
                            "\"offset\" must be a non-negative integer");
    rc = f_int(&in, "limit", 1, 64, &lim);
    if (rc < 0) return fail(r, TOOL_EINVAL, "driver_trace", "",
                            "\"limit\" must be an integer 1-64");

    int tail = 0;
    if (f_bool(&in, "tail", &tail) < 0)
        return fail(r, TOOL_EINVAL, "driver_trace", "", "\"tail\" must be a boolean");

    tool_result_printf(r, "last run: target=%s attempt %u status=%s\n",
                       g_last.target, g_last.attempt,
                       dvm_status_name(g_last.res.status));
    if (g_last.res.status != DVM_OK) {
        tool_result_printf(r, "stopped at line %lu: %s\n",
                           (unsigned long)g_last.res.line,
                           g_last.res.msg[0] ? g_last.res.msg : "(no message)");
        if (g_last.srcline[0])
            tool_result_printf(r, "  line %lu: %s\n",
                               (unsigned long)g_last.res.line, g_last.srcline);
    }
    tool_result_printf(r, "sandbox: %s\n", g_last.sandbox);
    {
        char bits[48];
        put_cmd_bits(bits, sizeof bits, g_last.cmd_added);
        tool_result_printf(r, "pci cmd 0x%04x -> 0x%04x: the kernel set %s%s\n",
                           (unsigned)g_last.cmd_before,
                           (unsigned)(g_last.cmd_before | g_last.cmd_added), bits,
                           g_last.cmd_cleared ? ", then cleared it again because the run "
                                                "failed" : "");
    }
    put_counters(r, &g_last.res);
    put_registers(r, &g_last.res);
    if (g_last.dma_msg[0])
        tool_result_printf(r, "dma went out of bounds: %s\n", g_last.dma_msg);

    if (!g_log_n) {
        tool_result_printf(r, "no device events were recorded\n");
        trace_ret(0, "driver_trace", "%s events=0", g_last.target);
        return TOOL_OK;
    }

    /* The log holds the last DRV_LOG_MAX events; index 1 is the oldest one
     * still held, which is stated rather than pretended away. */
    uint64_t first_held = g_log_total - g_log_n + 1;
    uint32_t start;
    if (tail) {
        start = (uint32_t)lim >= g_log_n ? 0 : g_log_n - (uint32_t)lim;
    } else {
        uint64_t want = (uint64_t)off + 1;
        start = (want <= first_held) ? 0 : (uint32_t)(want - first_held);
    }
    if (start >= g_log_n)
        return fail(r, TOOL_EINVAL, "driver_trace", g_last.target,
                    "offset %lu is past the end: %lu events were recorded",
                    (unsigned long)off, (unsigned long)g_log_total);

    if (g_log_total > g_log_n)
        tool_result_printf(r, "%lu events happened; the last %lu are kept\n",
                           (unsigned long)g_log_total, (unsigned long)g_log_n);

    uint32_t shown = 0;
    for (uint32_t i = start; i < g_log_n && shown < (uint32_t)lim; i++) {
        if (room_low(r)) break;
        const acc_t *a = log_at(i);
        if (!a) break;
        put_acc(r, log_index(i), a);
        shown++;
    }
    if (!shown)
        tool_result_printf(r, "no room left for events; ask for a smaller \"limit\"\n");
    else
        tool_result_printf(r, "shown %lu of %lu (events %lu-%lu)\n",
                           (unsigned long)shown, (unsigned long)g_log_total,
                           (unsigned long)log_index(start),
                           (unsigned long)log_index(start + shown - 1));

    trace_ret((long)shown, "driver_trace", "%s events=%lu", g_last.target,
              (unsigned long)g_log_total);
    return TOOL_OK;
}

static const tool_t driver_trace_tool = {
    .name = "driver_trace",
    .description =
        "Read back the device access log of the most recent driver_run: every port "
        "and MMIO access in order with its width and the value written or read, "
        "the delays, the final registers, and why the program stopped. driver_run "
        "returns the tail; use this for the earlier part of a long run. Only the "
        "last 256 events are kept, and the answer says so when older ones were "
        "dropped.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"offset\":{\"type\":\"integer\",\"description\":\"first event to print, 0-based (default 0)\"},"
        "\"limit\":{\"type\":\"integer\",\"description\":\"how many events, 1-64 (default 24)\"},"
        "\"tail\":{\"type\":\"boolean\",\"description\":\"print the LAST \\\"limit\\\" events\"}"
        "},\"required\":[]}",
    .flags  = 0,
    .invoke = t_driver_trace,
};
REGISTER_TOOL(driver_trace_tool);
