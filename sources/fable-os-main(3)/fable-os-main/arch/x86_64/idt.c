/* idt.c — build and install the GDT, the TSS, and the 256-entry IDT.
 *
 * See include/idt.h for the architecture, the TSS/IST reasoning and the PIC
 * reasoning. This file is the hardware-touching half of fault handling and the
 * one part of it that cannot be host-tested: it can only be verified by booting.
 * Everything that CAN be tested without hardware lives in fault.c instead.
 *
 * ORDER OF OPERATIONS IN idt_init(), and why it is that order:
 *   1. Zero the IST stacks' guard words and build the TSS. Nothing can fault
 *      usefully before this, so it goes first.
 *   2. Build and LGDT the new GDT. The code descriptor at selector 0x08 is
 *      byte-identical to the one boot.asm installed, so CS's cached descriptor
 *      stays valid and no far return is needed to reload it — the riskiest step
 *      in bringing up a new GDT is simply avoided.
 *   3. LTR. Only legal once the GDT holding the TSS descriptor is loaded.
 *   4. Fill all 256 gates from isr.asm's stub table and LIDT.
 *   5. Quiet the 8259s. Last, because until step 4 a stray interrupt would have
 *      nowhere to go — although in practice IF has been 0 since the multiboot
 *      handoff and stays 0 forever.
 */

#include "idt.h"
#include "fault.h"
#include "kernel.h"
#include "io.h"

#include <stdint.h>
#include <stddef.h>

/* isr.asm: 256 stub addresses, index == vector. */
extern const uint64_t isr_stub_table[IDT_ENTRIES];

/* linker.ld: the bounds of the loaded image. Used for the fault window, exactly
 * as tools/mem_tools.c uses them, and for the same reason — they are link-time
 * facts rather than constants a refactor can invalidate. */
extern char __bss_start[];
extern char __bss_end[];
/* The exact executable range, for live code patching only — see linker.ld and
 * include/fault.h's fault_patch_apply(). Deliberately separate from the code
 * range below, which is a plausibility test for return addresses and a superset. */
extern char __text_start[];
extern char __text_end[];
#define KERNEL_IMAGE_BASE 0x0000000000100000ULL

/* ====================================================================== */
/* descriptor formats                                                     */
/* ====================================================================== */

/* A 64-bit interrupt/trap gate: 16 bytes, with the handler offset scattered
 * across three fields for backwards compatibility with the 286. */
typedef struct __attribute__((packed)) idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;        /* bits 0-2 = IST index, rest must be zero */
    uint8_t  type_attr;  /* P | DPL | 0 | gate type                 */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_gate_t;

typedef struct __attribute__((packed)) desc_ptr {
    uint16_t limit;
    uint64_t base;
} desc_ptr_t;

/* The 64-bit TSS. It carries no task state in long mode — it exists for RSP0
 * (unused here: there is no userspace to come back from) and for the IST. */
typedef struct __attribute__((packed)) tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss64_t;

/* Compile-time contracts. Every one of these is a layout the CPU reads
 * directly, so a silent size change here is a triple fault at boot, which is
 * the hardest possible thing to debug on this machine. Make it a build error
 * instead. */
_Static_assert(sizeof(idt_gate_t) == 16, "IDT gate must be 16 bytes");
_Static_assert(sizeof(desc_ptr_t) == 10, "LIDT/LGDT operand must be 10 bytes");
_Static_assert(sizeof(tss64_t)    == 104, "64-bit TSS must be 104 bytes");
/* cr2 + reserved + 15 GPRs + vector + errcode + 5 CPU-pushed = 24 qwords. If
 * this ever disagrees, isr.asm and fault.h have drifted apart and every
 * register in every fault report is being read from the wrong slot. */
_Static_assert(sizeof(fault_frame_t) == 24 * 8, "fault_frame_t must match isr.asm");

/* ====================================================================== */
/* storage                                                                */
/* ====================================================================== */

static idt_gate_t idt[IDT_ENTRIES] __attribute__((aligned(16)));
static desc_ptr_t idtr;

/* GDT: null, kernel code, kernel data, then the TSS descriptor, which is a
 * 16-byte system descriptor and therefore occupies two slots. */
#define GDT_SLOTS 5
static uint64_t   gdt[GDT_SLOTS] __attribute__((aligned(16)));
static desc_ptr_t gdtr;

static tss64_t tss __attribute__((aligned(16)));

/* One dedicated stack per IST slot. 16 KiB each: enough for the report path
 * (which keeps its buffers static precisely so this can stay small) with a wide
 * margin. Page-aligned so an overrun is obvious in a hex dump. These live in
 * .bss, which boot.asm zeroes before any C runs. */
static uint8_t ist_stack[IST_USED][IST_STACK_SIZE] __attribute__((aligned(4096)));

/* A recognisable word at the low end of each IST stack. If a report ever shows
 * an RSP below one of these, the emergency stack itself overflowed. */
#define IST_GUARD 0x1157C0DE1157C0DEULL

static int installed;

/* ====================================================================== */
/* GDT + TSS                                                              */
/* ====================================================================== */

static void gdt_install(void) {
    /* Byte-for-byte the descriptor boot.asm built:
     *   (1<<43) type=code | (1<<44) S=1 | (1<<47) P=1 | (1<<53) L=1
     * Reusing the exact value is what lets CS keep working across the LGDT
     * without a far return to reload it. */
    const uint64_t code_desc = 0x00209A0000000000ULL;
    /* Writable data segment. Nothing loads it today — boot.asm left every data
     * selector null, which is legal at CPL 0 in 64-bit mode, and changing that
     * on a machine that currently works would be a gratuitous risk. It exists
     * so the descriptor is there the day something needs it (an IRET to ring 3,
     * or a compatibility-mode trampoline). */
    const uint64_t data_desc = 0x0000920000000000ULL;

    gdt[0] = 0;
    gdt[1] = code_desc;
    gdt[2] = data_desc;

    /* 16-byte system descriptor for the TSS, split across gdt[3] and gdt[4].
     * type 9 = available 64-bit TSS; 0x89 = present, DPL 0, type 9. */
    uint64_t base  = (uint64_t)(uintptr_t)&tss;
    uint32_t limit = (uint32_t)(sizeof tss - 1);

    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFFu);
    low |= ((uint64_t)(base & 0xFFFFu)) << 16;
    low |= ((uint64_t)((base >> 16) & 0xFFu)) << 32;
    low |= ((uint64_t)0x89u) << 40;
    low |= ((uint64_t)((limit >> 16) & 0xFu)) << 48;
    low |= ((uint64_t)((base >> 24) & 0xFFu)) << 56;

    gdt[3] = low;
    gdt[4] = (base >> 32) & 0xFFFFFFFFULL;

    gdtr.limit = (uint16_t)(sizeof gdt - 1);
    gdtr.base  = (uint64_t)(uintptr_t)gdt;
    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");
}

static void tss_install(void) {
    for (size_t i = 0; i < sizeof tss; i++)
        ((volatile uint8_t *)&tss)[i] = 0;

    for (int i = 0; i < IST_USED; i++) {
        *(volatile uint64_t *)&ist_stack[i][0] = IST_GUARD;
        /* The IST holds the address the CPU loads into RSP, so it is the TOP of
         * the stack. Keep it 16-byte aligned: the CPU aligns RSP on delivery
         * anyway, but an aligned base makes the arithmetic in a dump obvious. */
        tss.ist[i] = (uint64_t)(uintptr_t)&ist_stack[i][IST_STACK_SIZE];
        tss.ist[i] &= ~0xFULL;
    }

    /* No userspace exists, so RSP0 is never consulted. Point it at the #DF
     * stack anyway rather than leaving a zero that would be loaded verbatim if
     * a ring transition ever happened by accident. */
    tss.rsp0 = tss.ist[IST_DOUBLE_FAULT - 1];

    /* An I/O map base >= the TSS limit means "no I/O permission bitmap". */
    tss.iomap_base = (uint16_t)sizeof tss;

    __asm__ volatile("ltr %w0" : : "r"((uint16_t)SEL_TSS));
}

/* ====================================================================== */
/* IDT                                                                    */
/* ====================================================================== */

static void set_gate(int vector, uint64_t handler, uint8_t ist, uint8_t type) {
    idt_gate_t *g = &idt[vector];
    g->offset_low  = (uint16_t)(handler & 0xFFFFu);
    g->selector    = SEL_KERNEL_CODE;
    g->ist         = (uint8_t)(ist & 0x7u);
    /* P=1, DPL=0, type. DPL is written as a literal zero rather than masked
     * from a parameter because there is no ring 3 on this machine: nothing can
     * reach a gate from userspace, so no gate ever needs a DPL above 0. */
    g->type_attr   = (uint8_t)(0x80u | (type & 0xFu));
    g->offset_mid  = (uint16_t)((handler >> 16) & 0xFFFFu);
    g->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFu);
    g->zero        = 0;
}

static void idt_install(void) {
    for (int v = 0; v < IDT_ENTRIES; v++) {
        uint8_t ist = IST_NONE;
        switch (v) {
            case 2:  ist = IST_NMI;           break;
            case 8:  ist = IST_DOUBLE_FAULT;  break;
            case 18: ist = IST_MACHINE_CHECK; break;
            default: break;
        }
        /* Interrupt gates throughout: IF is cleared on entry, so the reporting
         * path can never be interrupted halfway through printing. */
        set_gate(v, isr_stub_table[v], ist, IDT_GATE_INTERRUPT);
    }

    idtr.limit = (uint16_t)(sizeof idt - 1);
    idtr.base  = (uint64_t)(uintptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

/* ====================================================================== */
/* the 8259s                                                              */
/* ====================================================================== */

#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1

/* Give the ISA bus a moment to settle between the ICW writes. An unused port
 * write is the traditional way to do this and costs nothing here. */
static void io_wait(void) { outb(0x80, 0); }

static void pic_remap_and_mask(void) {
    /* As the BIOS leaves them, IRQ0..7 sit on vectors 0x08..0x0F, i.e. right on
     * top of #DF, #TS, #NP, #SS, #GP, #PF. Move them out of the exception range
     * before anything can deliver one. See idt.h for the full argument. */
    outb(PIC1_CMD, 0x11); io_wait();       /* ICW1: init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DAT, 0x20); io_wait();       /* ICW2: master base = 0x20 */
    outb(PIC2_DAT, 0x28); io_wait();       /* ICW2: slave  base = 0x28 */
    outb(PIC1_DAT, 0x04); io_wait();       /* ICW3: slave on IRQ2      */
    outb(PIC2_DAT, 0x02); io_wait();       /* ICW3: slave cascade id   */
    outb(PIC1_DAT, 0x01); io_wait();       /* ICW4: 8086 mode          */
    outb(PIC2_DAT, 0x01); io_wait();

    /* Then mask every line. Nothing in this kernel uses an IRQ: the keyboard,
     * the NIC and the clock are all polled, and the PIT is gated by hand
     * through port 0x61 rather than through IRQ0. Masking changes no observable
     * behaviour and removes the possibility of a spurious delivery if anything
     * ever sets IF. */
    outb(PIC1_DAT, 0xFF);
    outb(PIC2_DAT, 0xFF);
}

/* ====================================================================== */
/* the fault window                                                       */
/* ====================================================================== */

static void window_fill(fault_window_t *w) {
    w->image_lo = KERNEL_IMAGE_BASE;
    w->image_hi = (uint64_t)(uintptr_t)__bss_end;
    /* Return addresses live in .text. There is no __text_end symbol, so bound
     * by __bss_start: that covers .text/.rodata/.data, a superset which admits
     * some false positives in the raw-stack scan (and the report labels that
     * scan as candidates for exactly this reason) but cannot admit an address
     * that is not part of the loaded image. */
    w->code_lo  = KERNEL_IMAGE_BASE;
    w->code_hi  = (uint64_t)(uintptr_t)__bss_start;
    /* boot.asm's 64 KiB kernel stack and the IST stacks are all inside .bss,
     * and boot.asm zeroed every byte of it before calling into C, so the whole
     * span is provably backed RAM. */
    w->stack_lo = (uint64_t)(uintptr_t)__bss_start;
    w->stack_hi = (uint64_t)(uintptr_t)__bss_end;
    /* Instructions, and nothing else. A live code patch is bounded by this and
     * not by code_lo..code_hi, so a patch cannot land in .rodata or .data. */
    w->text_lo  = (uint64_t)(uintptr_t)__text_start;
    w->text_hi  = (uint64_t)(uintptr_t)__text_end;
}

/* ====================================================================== */
/* the last-resort path                                                   */
/* ====================================================================== */

/* Print a 64-bit value with no formatter, no buffer and no call into anything
 * that could itself be broken. This is what runs when a fault happens INSIDE
 * the fault handler, so it may not depend on a single thing that the first
 * fault could have damaged. */
static void emit_hex64(uint64_t v) {
    static const char hx[] = "0123456789abcdef";
    kputs("0x");
    for (int i = 60; i >= 0; i -= 4) kputc(hx[(v >> i) & 0xF]);
}

static void minimal_report(const fault_frame_t *f, const char *why) {
    kputs("\n*** ");
    kputs(why);
    kputs(": vector=");
    emit_hex64(f->vector);
    kputs(" err=");
    emit_hex64(f->error_code);
    kputs(" rip=");
    emit_hex64(f->rip);
    kputs(" cr2=");
    emit_hex64(f->cr2);
    kputs(" rsp=");
    emit_hex64(f->rsp);
    kputs(" ***\n");
}

static void halt_forever(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

/* ====================================================================== */
/* the C entry point isr.asm calls                                        */
/* ====================================================================== */

/* Nesting guard, and NOT the recovery loop guard — the two are different
 * failures and they are counted in different places.
 *
 *   in_handler        a fault raised while this function is still running: the
 *                     capture path itself read something it should not have, or
 *                     the console died. That is never survivable, because the
 *                     machinery that would report it is the machinery that just
 *                     broke. Say the bare minimum and stop.
 *
 *   fault_refaults()  a fault raised AFTER a successful resume, at the same RIP
 *                     as the last one. That is a recovery that did not work,
 *                     and it is survivable up to FAULT_REFAULT_MAX attempts.
 *                     fault.c owns that counter; see fault_recover().
 *
 * Before recovery existed the first guard did both jobs by accident, because a
 * fault could never resume and therefore could never come back. It cannot do
 * both any more: in_handler is cleared on every resume path below, so a resumed
 * fault arrives here looking exactly like a first one — which is precisely why
 * the same-RIP counter has to exist. */
static int in_handler;

void isr_dispatch(fault_frame_t *frame) {
    if (in_handler) {
        /* Second fault while handling the first. Say the bare minimum with the
         * simplest possible code path, then stop. Do not try to format, do not
         * touch the record, do not call millis(). */
        minimal_report(frame, "DOUBLE FAULT IN THE FAULT HANDLER");
        halt_forever();
    }
    in_handler = 1;

    /* An immediate one-liner, before anything clever runs. If the rich path
     * dies, this is still on the console — and on a machine with no shell, that
     * line may be the only thing the operator ever gets. */
    minimal_report(frame, "FAULT");

    fault_window_t w;
    window_fill(&w);
    fault_note(frame, &w, millis());

    /* Consult the armed plan and, if every check passes, patch the LIVE frame
     * so the IRETQ at the end of isr.asm's common path resumes with the new
     * RIP / register. fault_recover() also records the disposition into the
     * ring slot and re-renders its report, so the text printed below already
     * states what was decided — the operator and the model see one account. */
    fault_fix_t fix = fault_recover(frame, &w);

    /* THE THIRD DISPOSITION. No plan could fix this in place — but if some
     * caller wrapped what it was doing in a fault guard, the frame can be
     * patched to return to THAT instead of halting the machine. The faulting
     * call chain is abandoned, not resumed; fault.h states what that costs and
     * bounds how often it may happen. This is what lets a fatal fault be
     * diagnosed at all: the ask needs lwIP, mbedTLS and the heap, none of which
     * may be entered from here, so the handler's job is to get back to normal
     * kernel context and let somebody else do the asking.
     *
     * fault_escape() itself allocates nothing, touches no device and refuses
     * unless the guard's saved RIP and RSP still pass their bounds checks. */
    if (fix != FAULT_FIX_OK && fix != FAULT_FIX_TRAP) {
        fault_fix_t esc = fault_escape(frame, &w);
        /* THE REFUSAL IS THE INTERESTING HALF, and it used to be thrown away.
         *
         * Only FAULT_FIX_ESCAPED was looked at, so a guard that was armed and
         * INTACT but refused — spent budget, a scribbled-on saved context, or a
         * guard belonging to another fiber's stack — halted the machine under
         * fault_recover()'s verdict, which is almost always "no-plan: no recovery
         * plan was armed". That sentence is false in every one of those cases, it
         * is the first thing the operator and the model both read, and it sends
         * both of them looking for a missing plan rather than at the real reason.
         *
         * Only a genuine "there was no guard" is left as the recover verdict; every
         * other outcome replaces it, because it is more specific than the thing it
         * replaces. */
        if (esc == FAULT_FIX_ESCAPED)         fix = FAULT_FIX_ESCAPED;
        else if (esc != FAULT_FIX_NO_GUARD)   fix = esc;
    }

    kputs(fault_last_report());

    if (fix == FAULT_FIX_OK || fix == FAULT_FIX_TRAP ||
        fix == FAULT_FIX_ESCAPED) {
        in_handler = 0;
        return;
    }

    kprintf("\n*** The machine is halted: %s.\n"
            "*** %s\n"
            "*** There is no shell to fall back to, so the report above is the\n"
            "*** whole record. Arm a recovery plan before the fault (see the\n"
            "*** fault_recover tool) or reset to continue.\n",
            fault_fix_name(fix), fault_fix_desc(fix));
    halt_forever();
}

/* ====================================================================== */
/* bring-up                                                               */
/* ====================================================================== */

int      idt_installed(void) { return installed; }
uint64_t idt_base(void)      { return idtr.base; }
uint16_t idt_limit(void)     { return idtr.limit; }

uint64_t idt_gate_offset(int vector) {
    if (vector < 0 || vector >= IDT_ENTRIES) return 0;
    const idt_gate_t *g = &idt[vector];
    return (uint64_t)g->offset_low
         | ((uint64_t)g->offset_mid << 16)
         | ((uint64_t)g->offset_high << 32);
}

int idt_gate_ist(int vector) {
    if (vector < 0 || vector >= IDT_ENTRIES) return -1;
    return idt[vector].ist & 0x7;
}

/* Read every gate back through the same accessors a test would use and check it
 * against what was asked for. The gate format scatters a 64-bit offset across
 * three fields at three different widths, so a packing mistake is easy to make
 * and produces a handler address that is subtly wrong — which is a triple fault
 * at the worst possible moment, with no console output to explain it. Verifying
 * 256 descriptors costs microseconds once at boot. Returns the number of
 * problems found. */
static int idt_verify(void) {
    int bad = 0;
    for (int v = 0; v < IDT_ENTRIES; v++) {
        if (idt_gate_offset(v) != isr_stub_table[v]) bad++;
        if (idt[v].selector != SEL_KERNEL_CODE)      bad++;
        if ((idt[v].type_attr & 0x80u) == 0)         bad++;   /* present */
    }
    if (idt_gate_ist(8)  != IST_DOUBLE_FAULT)  bad++;
    if (idt_gate_ist(2)  != IST_NMI)           bad++;
    if (idt_gate_ist(18) != IST_MACHINE_CHECK) bad++;
    if (idt_gate_ist(14) != IST_NONE)          bad++;   /* #PF must NOT switch */
    return bad;
}

void idt_init(void) {
    if (installed) return;

    gdt_install();
    tss_install();
    idt_install();
    pic_remap_and_mask();

    /* Publish the window once, from the same window_fill() the fault path uses,
     * so a recovery plan is validated against exactly the bounds a resume will
     * be judged by. Two copies of "where the code is" would eventually
     * disagree, and the field they disagree about is the one that decides
     * whether the CPU jumps somewhere real. */
    fault_window_t w;
    window_fill(&w);
    fault_set_window(&w);

    int bad = idt_verify();
    if (bad) {
        /* Do not panic: a machine with a partly-wrong IDT still boots and is
         * still worth talking to, and the operator needs to be told exactly
         * what is untrustworthy rather than losing the console. */
        kprintf("idt: WARNING - %d descriptor checks failed. Exception "
                "handling is NOT trustworthy on this boot.\n", bad);
    }

    installed = 1;

    /* The verdict comes from `bad`, not from a literal: this line used to say
     * "all verified" unconditionally, directly under the WARNING saying the
     * opposite. On a machine whose console is its only record, a summary that
     * cannot report failure is worse than no summary. */
    kprintf("idt: 256 gates at %p (limit %u), %s; TSS with %d IST "
            "stacks of %u KiB (#DF=1 NMI=2 #MC=3); PIC remapped to 0x20/0x28 "
            "and fully masked\n",
            (void *)(uintptr_t)idtr.base, (unsigned)idtr.limit,
            bad ? "NOT verified - see the WARNING above" : "all verified",
            IST_USED, (unsigned)(IST_STACK_SIZE / 1024));
}
