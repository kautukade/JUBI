/* fault.c — decode, capture, render and recover from a CPU fault.
 *
 * Everything in this file is pure logic over a fault_frame_t and a
 * fault_window_t. It has no hardware dependencies, allocates nothing, and reads
 * no memory that the caller-supplied window does not explicitly permit — which
 * is what makes it safe to run from inside an exception handler and, at the same
 * time, testable natively in tests/host/test_fault.c against a synthetic stack.
 *
 * The IDT itself (arch/x86_64/idt.c) can only be verified by booting. This part
 * — which is the part the operator actually reads when the machine dies, and now
 * also the part that decides whether it dies at all — does not have to be, and
 * so it isn't.
 *
 * Each report is rendered ONCE, into the static buffer at the bottom of this
 * file, and both the console print and fault_last_report() hand back that same
 * buffer. The operator's record and the model's record are therefore the same
 * bytes by construction, not by two formatters agreeing.
 *
 * FILE LAYOUT
 *   naming / error-code decoding / window tests / backtrace / capture / report
 *       ... turning a frame into words.
 *   x86_insn_length
 *       ... a partial, deliberately timid instruction-length decoder.
 *   registers, fault_apply_plan
 *       ... the pure half of recovery: a decision plus a frame, in, a patched
 *           frame or a named refusal, out. No globals are touched.
 *   the ring, the plan, fault_recover
 *       ... the live half: which record, which armed plan, how many times in a
 *           row we have seen this exact RIP.
 */

#include "fault.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>      /* vsnprintf — the libc shim's, when freestanding */

/* ====================================================================== */
/* bounded appender                                                       */
/* ====================================================================== */

/* snprintf-style accumulation into a fixed buffer. `*off` is the length that
 * WOULD have been written, so overflow is detectable at the end with one test
 * and every intermediate call stays harmless once the buffer is full. */
static void ap(char *dst, size_t cap, size_t *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void ap(char *dst, size_t cap, size_t *off, const char *fmt, ...) {
    va_list ap_;
    va_start(ap_, fmt);
    size_t used = (*off < cap) ? *off : cap;
    int    n    = vsnprintf(dst + used, cap - used, fmt, ap_);
    va_end(ap_);
    if (n > 0) *off += (size_t)n;
}

/* ====================================================================== */
/* naming                                                                 */
/* ====================================================================== */

/* The architectural exception table. Vectors marked "--" are reserved by Intel;
 * one of them firing is itself a diagnosis (almost always a bad IDT), so they
 * are named rather than silently lumped in with everything else. */
static const struct {
    const char *name;
    const char *desc;
} vec_tab[32] = {
    { "#DE", "Divide Error"                    },  /*  0 */
    { "#DB", "Debug Exception"                 },  /*  1 */
    { "NMI", "Non-Maskable Interrupt"          },  /*  2 */
    { "#BP", "Breakpoint"                      },  /*  3 */
    { "#OF", "Overflow"                        },  /*  4 */
    { "#BR", "BOUND Range Exceeded"            },  /*  5 */
    { "#UD", "Invalid Opcode"                  },  /*  6 */
    { "#NM", "Device Not Available"            },  /*  7 */
    { "#DF", "Double Fault"                    },  /*  8 */
    { "--",  "Coprocessor Segment Overrun"     },  /*  9 (reserved since 486) */
    { "#TS", "Invalid TSS"                     },  /* 10 */
    { "#NP", "Segment Not Present"             },  /* 11 */
    { "#SS", "Stack-Segment Fault"             },  /* 12 */
    { "#GP", "General Protection Fault"        },  /* 13 */
    { "#PF", "Page Fault"                      },  /* 14 */
    { "--",  "Reserved"                        },  /* 15 */
    { "#MF", "x87 Floating-Point Error"        },  /* 16 */
    { "#AC", "Alignment Check"                 },  /* 17 */
    { "#MC", "Machine Check"                   },  /* 18 */
    { "#XM", "SIMD Floating-Point Exception"   },  /* 19 */
    { "#VE", "Virtualization Exception"        },  /* 20 */
    { "#CP", "Control Protection Exception"    },  /* 21 */
    { "--",  "Reserved"                        },  /* 22 */
    { "--",  "Reserved"                        },  /* 23 */
    { "--",  "Reserved"                        },  /* 24 */
    { "--",  "Reserved"                        },  /* 25 */
    { "--",  "Reserved"                        },  /* 26 */
    { "--",  "Reserved"                        },  /* 27 */
    { "#HV", "Hypervisor Injection Exception"  },  /* 28 */
    { "#VC", "VMM Communication Exception"     },  /* 29 */
    { "#SX", "Security Exception"              },  /* 30 */
    { "--",  "Reserved"                        },  /* 31 */
};

const char *fault_vector_name(uint64_t vector) {
    if (vector < 32)  return vec_tab[vector].name;
    if (vector < 256) return "INT";
    return "??";
}

const char *fault_vector_desc(uint64_t vector) {
    if (vector < 32)  return vec_tab[vector].desc;
    if (vector < 256) return "Unexpected external or software interrupt";
    return "Vector out of range";
}

int fault_has_error_code(uint64_t vector) {
    switch (vector) {
        case 8: case 10: case 11: case 12: case 13:
        case 14: case 17: case 21: case 29: case 30:
            return 1;
        default:
            return 0;
    }
}

int fault_is_fatal(uint64_t vector) {
    /* #DB and #BP are traps: RIP already points past the instruction that
     * caused them, so returning simply continues. Nothing in this kernel raises
     * either one today, but a debugger stub or a planted breakpoint will, and
     * halting the machine on a breakpoint would be absurd.
     *
     * Vectors >= 32 cannot fire (IF is 0 and every PIC line is masked), so one
     * arriving means something is wrong with the interrupt configuration, not
     * with the code that was running. Report it, do not kill the machine over
     * it. */
    /* NMI belongs with them, and for the same reason. An NMI is a NOTIFICATION
     * (chipset SERR/parity, a watchdog, IPMI), not "this instruction is
     * unexecutable": the pushed RIP is the address of the NEXT instruction,
     * exactly as for #BP. Routing it into the plan path would let the most
     * natural blanket plan a model can arm — {"action":"skip"} with no "vector"
     * field, which the tool schema invites — decode whatever happens to sit at
     * the resume address and step over an instruction that never faulted. */
    if (vector == 1 || vector == 2 || vector == 3) return 0;
    if (vector >= 32)               return 0;
    return 1;
}

int fault_is_recoverable(uint64_t vector) {
    /* #DF. Not a policy choice — a physical one. The frame was delivered on an
     * IST stack precisely because the interrupted RSP could not be used, and
     * IRETQ would put that same RSP back. The very next push triple-faults and
     * QEMU resets the VM with no console output at all, which is the single
     * outcome this module was written to prevent. */
    if (vector == 8)  return 0;
    /* #MC. A machine check is the hardware telling software it has detected an
     * error it could not correct. "Carry on" is not a decision that can be made
     * from up here. */
    if (vector == 18) return 0;
    /* Everything else, including the continuable traps (which do not need a
     * plan) and the unexpected-interrupt vectors, is fair game. */
    return 1;
}

/* ====================================================================== */
/* error-code decoding                                                    */
/* ====================================================================== */

/* Page-fault error code, Intel SDM Vol 3A 4.7. The bits are the whole
 * diagnosis: bit 0 alone separates "this address has no mapping" from "this
 * address is mapped and you were not allowed to do that to it", which are
 * completely different bugs. */
static size_t decode_pf(uint64_t e, char *dst, size_t cap) {
    size_t off = 0;
    ap(dst, cap, &off, "%s, %s, %s-mode access",
       (e & 0x01) ? "protection violation (page IS present)"
                  : "page not present",
       (e & 0x02) ? "write" : "read",
       (e & 0x04) ? "user" : "kernel");
    if (e & 0x08) ap(dst, cap, &off, ", RESERVED BIT SET in a paging structure");
    /* The I/D bit is architecturally defined only when EFER.NXE = 1. This
     * kernel deliberately never enables NX (boot.asm maps everything RWX and a
     * later phase depends on executable heap pages), so the bit is ALWAYS zero
     * here and "data access" would be a lie for a wild jump. Say so, and point
     * at the test that does work: on an instruction fetch, RIP equals CR2. */
    if (e & 0x10)
        ap(dst, cap, &off, ", instruction fetch");
    else
        ap(dst, cap, &off,
           ", data access - but the I/D bit is only reported when EFER.NXE=1 "
           "and this kernel leaves NX disabled, so compare RIP with CR2 to rule "
           "out an instruction fetch");
    if (e & 0x20)   ap(dst, cap, &off, ", protection-key violation");
    if (e & 0x40)   ap(dst, cap, &off, ", shadow-stack access");
    if (e & 0x8000) ap(dst, cap, &off, ", SGX-induced");
    return off;
}

/* Selector error code, Intel SDM Vol 3A 6.13. Used by #GP #SS #NP #TS #CP #SX.
 * A value of zero is common and means "no segment selector was involved" — for
 * #GP that usually means a non-canonical address or a privileged instruction,
 * which is worth saying out loud rather than printing "index 0". */
static size_t decode_selector(uint64_t e, char *dst, size_t cap) {
    size_t off = 0;
    if (e == 0) {
        ap(dst, cap, &off,
           "0 - no segment selector involved (typically a non-canonical "
           "address, a privileged instruction, or a reserved-bit write)");
        return off;
    }
    const char *table;
    if (e & 0x2)      table = "IDT";
    else if (e & 0x4) table = "LDT";
    else              table = "GDT";
    ap(dst, cap, &off, "selector index %u in the %s%s",
       (unsigned)((e >> 3) & 0x1FFFu), table,
       (e & 0x1) ? ", raised by an external event" : "");
    return off;
}

size_t fault_decode_error(uint64_t vector, uint64_t error_code,
                          char *dst, size_t cap) {
    if (cap) dst[0] = '\0';

    switch (vector) {
        case 14:
            return decode_pf(error_code, dst, cap);
        case 8: {
            size_t off = 0;
            ap(dst, cap, &off,
               "always zero for #DF; the value is not information. A double "
               "fault means the CPU could not deliver an earlier exception - "
               "most often because RSP was unusable.");
            return off;
        }
        case 10: case 11: case 12: case 13: case 21: case 29: case 30:
            return decode_selector(error_code, dst, cap);
        case 17: {
            size_t off = 0;
            ap(dst, cap, &off, "always zero for #AC (an unaligned access with "
                               "alignment checking enabled)");
            return off;
        }
        default: {
            size_t off = 0;
            ap(dst, cap, &off,
               "this vector pushes no error code; the stub pushed zero, so the "
               "value above carries no information");
            return off;
        }
    }
}

size_t fault_decode_rflags(uint64_t f, char *dst, size_t cap) {
    static const struct { uint32_t bit; const char *name; } flg[] = {
        {  0, "CF" }, {  2, "PF" }, {  4, "AF" }, {  6, "ZF" }, {  7, "SF" },
        {  8, "TF" }, {  9, "IF" }, { 10, "DF" }, { 11, "OF" }, { 14, "NT" },
        { 16, "RF" }, { 17, "VM" }, { 18, "AC" }, { 19, "VIF" }, { 20, "VIP" },
        { 21, "ID" },
    };
    size_t off   = 0;
    int    first = 1;
    if (cap) dst[0] = '\0';
    for (unsigned i = 0; i < sizeof flg / sizeof flg[0]; i++) {
        if (!(f & (1ULL << flg[i].bit))) continue;
        ap(dst, cap, &off, "%s%s", first ? "" : " ", flg[i].name);
        first = 0;
    }
    if (first) ap(dst, cap, &off, "(none)");
    ap(dst, cap, &off, " IOPL=%u", (unsigned)((f >> 12) & 3u));
    return off;
}

/* ====================================================================== */
/* window tests                                                           */
/* ====================================================================== */

/* True when [addr, addr+len) lies wholly inside [lo, hi). Written so addr+len
 * is never formed: both operands of the subtraction are already ordered, so
 * there is nothing to overflow. Same discipline as tools/mem_tools.c. */
static int in_range(uint64_t addr, uint64_t len, uint64_t lo, uint64_t hi) {
    if (hi <= lo)        return 0;
    if (len == 0)        return 0;
    if (addr < lo)       return 0;
    if (addr >= hi)      return 0;
    if (len > hi - addr) return 0;
    return 1;
}

/* Read one qword, but only from inside the window's stack range. Returns 0 on
 * refusal, which the callers treat as "chain ends here". */
static int read_stack_u64(uint64_t addr, const fault_window_t *w, uint64_t *out) {
    if (!in_range(addr, 8, w->stack_lo, w->stack_hi)) return 0;
    *out = *(const volatile uint64_t *)(uintptr_t)addr;
    return 1;
}

static int looks_like_code(uint64_t a, const fault_window_t *w) {
    return a >= w->code_lo && a < w->code_hi;
}

/* ====================================================================== */
/* backtrace                                                              */
/* ====================================================================== */

int fault_backtrace(uint64_t rbp, const fault_window_t *window,
                    uint64_t *out, int max) {
    int n = 0;
    if (!window || !out || max <= 0) return 0;

    uint64_t fp = rbp;
    while (n < max) {
        /* A frame pointer is 8-aligned by the ABI. Anything else is garbage and
         * dereferencing it is exactly the mistake this module exists to stop
         * making. */
        if (fp & 7u) break;

        uint64_t next, ret;
        if (!read_stack_u64(fp, window, &next))         break;
        if (!read_stack_u64(fp + 8, window, &ret))      break;
        if (!looks_like_code(ret, window))              break;

        out[n++] = ret;

        /* Stacks grow down, so a caller's frame is always at a HIGHER address.
         * Requiring a strict increase is what makes a corrupted or cyclic chain
         * terminate rather than spin. */
        if (next <= fp) break;
        fp = next;
    }
    return n;
}

int fault_backtrace_scan(uint64_t rsp, const fault_window_t *window,
                         uint64_t *out, int max) {
    int n = 0;
    if (!window || !out || max <= 0) return 0;

    uint64_t p = (rsp + 7u) & ~7ULL;         /* round up to the next qword */
    if (p < rsp) return 0;                   /* wrapped: nothing to scan   */

    for (int i = 0; i < FAULT_SCAN_SLOTS && n < max; i++, p += 8) {
        uint64_t v;
        if (!read_stack_u64(p, window, &v)) break;
        if (looks_like_code(v, window)) out[n++] = v;
    }
    return n;
}

/* ====================================================================== */
/* capture                                                                */
/* ====================================================================== */

static void zero(void *p, size_t n) {
    volatile unsigned char *d = p;
    while (n--) *d++ = 0;
}

void fault_capture(const fault_frame_t *frame, const fault_window_t *window,
                   uint64_t uptime_ms, fault_record_t *out) {
    if (!out) return;
    zero(out, sizeof *out);
    if (!frame || !window) return;

    out->regs      = *frame;
    out->uptime_ms = uptime_ms;
    out->fatal     = (uint8_t)fault_is_fatal(frame->vector);

    /* ---- the bytes at RIP ----
     * The single most useful thing in the whole report: it turns "a fault at
     * 0x1043c1" into "a fault at `mov %rax,(%rbx)`". Only read what the window
     * proves is backed RAM, and clip rather than refuse when RIP sits close to
     * the end of the image. */
    uint64_t rip = frame->rip;
    unsigned want = FAULT_CODE_BYTES;
    if (rip >= window->image_lo && rip < window->image_hi) {
        uint64_t avail = window->image_hi - rip;
        if (avail < want) want = (unsigned)avail;
        const volatile unsigned char *p =
            (const volatile unsigned char *)(uintptr_t)rip;
        for (unsigned i = 0; i < want; i++) out->code[i] = p[i];
        out->code_len   = (uint8_t)want;
        out->code_valid = 1;
    }

    /* ---- backtrace ----
     * The frame-pointer walk is the truth when it works (the kernel is built at
     * -O0, so RBP really is a frame pointer in every C function). It stops
     * working when RBP was the thing that got corrupted, or when the fault was
     * delivered on an IST stack — so fall back to a raw scan, clearly labelled,
     * rather than printing nothing. */
    int n = fault_backtrace(frame->rbp, window, out->bt, FAULT_BT_MAX);
    if (n >= 2) {
        out->bt_count  = (uint8_t)n;
        out->bt_source = FAULT_BT_FRAME;
    } else {
        uint64_t scan[FAULT_BT_MAX];
        int m = fault_backtrace_scan(frame->rsp, window, scan, FAULT_BT_MAX);
        if (m > n) {
            for (int i = 0; i < m; i++) out->bt[i] = scan[i];
            out->bt_count  = (uint8_t)m;
            out->bt_source = FAULT_BT_SCAN;
        } else if (n > 0) {
            out->bt_count  = (uint8_t)n;
            out->bt_source = FAULT_BT_FRAME;
        } else {
            out->bt_count  = 0;
            out->bt_source = FAULT_BT_NONE;
        }
    }

    out->valid = 1;
}

/* ====================================================================== */
/* the report                                                             */
/* ====================================================================== */

#define RULE "================================================================\n"

size_t fault_format_report(const fault_record_t *r, char *dst, size_t cap) {
    size_t off = 0;
    if (cap) dst[0] = '\0';
    if (!r || !r->valid) {
        ap(dst, cap, &off, "no fault has been captured\n");
        return off;
    }

    const fault_frame_t *f = &r->regs;
    /* Big enough for the LONGEST thing decoded into it, which is a #PF error
     * code with every reported bit set: 63 bytes for the three-way summary + 40
     * (reserved bit) + 157 (the NX/EFER.NXE note, the longest single clause) +
     * 26 (protection key) + 21 (shadow stack) + 13 (SGX) = 320 characters, i.e.
     * 321 bytes with the terminator. It was exactly 320, so that combination
     * lost its last character — a silently truncated diagnosis on a machine
     * where this string is the whole account of the fault. */
    char scratch[384];

    ap(dst, cap, &off, "\n" RULE);
    ap(dst, cap, &off, "*** KERNEL FAULT: %s (vector %lu) - %s\n",
       fault_vector_name(f->vector), (unsigned long)f->vector,
       fault_vector_desc(f->vector));
    ap(dst, cap, &off, RULE);
    ap(dst, cap, &off, "  fault #%lu at %lu ms since boot%s\n",
       (unsigned long)r->seq, (unsigned long)r->uptime_ms,
       r->fatal ? "" : "  (continuable trap - execution resumes)");

    /* ---- error code ---- */
    ap(dst, cap, &off, "  error code : 0x%016lx  (%s)\n",
       (unsigned long)f->error_code,
       fault_has_error_code(f->vector) ? "pushed by the CPU"
                                       : "synthesised by the stub");
    fault_decode_error(f->vector, f->error_code, scratch, sizeof scratch);
    ap(dst, cap, &off, "             : %s\n", scratch);

    /* ---- the address, for the one vector that has one ---- */
    if (f->vector == 14) {
        ap(dst, cap, &off, "  CR2        : 0x%016lx   <-- the address that "
                           "could not be accessed\n",
           (unsigned long)f->cr2);
        /* The decisive test for an instruction fetch on a kernel with NX off,
         * where the error code's I/D bit is always zero. */
        if (f->cr2 == f->rip)
            ap(dst, cap, &off,
               "             : CR2 == RIP, so the CPU faulted FETCHING this "
               "instruction, not reading data. Execution jumped somewhere that "
               "is not mapped.\n");
    } else {
        ap(dst, cap, &off, "  CR2        : 0x%016lx   (stale; only #PF sets "
                           "it)\n", (unsigned long)f->cr2);
    }

    /* ---- where ---- */
    fault_decode_rflags(f->rflags, scratch, sizeof scratch);
    ap(dst, cap, &off,
       "  RIP        : 0x%016lx   CS: 0x%04lx\n"
       "  RSP        : 0x%016lx   SS: 0x%04lx\n"
       "  RFLAGS     : 0x%016lx   [%s]\n",
       (unsigned long)f->rip,    (unsigned long)f->cs,
       (unsigned long)f->rsp,    (unsigned long)f->ss,
       (unsigned long)f->rflags, scratch);

    /* ---- general-purpose registers ---- */
    ap(dst, cap, &off,
       "  RAX 0x%016lx  RBX 0x%016lx  RCX 0x%016lx\n"
       "  RDX 0x%016lx  RSI 0x%016lx  RDI 0x%016lx\n"
       "  RBP 0x%016lx  R8  0x%016lx  R9  0x%016lx\n"
       "  R10 0x%016lx  R11 0x%016lx  R12 0x%016lx\n"
       "  R13 0x%016lx  R14 0x%016lx  R15 0x%016lx\n",
       (unsigned long)f->rax, (unsigned long)f->rbx, (unsigned long)f->rcx,
       (unsigned long)f->rdx, (unsigned long)f->rsi, (unsigned long)f->rdi,
       (unsigned long)f->rbp, (unsigned long)f->r8,  (unsigned long)f->r9,
       (unsigned long)f->r10, (unsigned long)f->r11, (unsigned long)f->r12,
       (unsigned long)f->r13, (unsigned long)f->r14, (unsigned long)f->r15);

    /* ---- the instruction ---- */
    if (r->code_valid && r->code_len) {
        size_t k = 0;
        for (unsigned i = 0; i < r->code_len && k + 3 < sizeof scratch; i++) {
            static const char hx[] = "0123456789abcdef";
            scratch[k++] = hx[(r->code[i] >> 4) & 0xF];
            scratch[k++] = hx[r->code[i] & 0xF];
            scratch[k++] = ' ';
        }
        if (k) k--;                     /* drop the trailing space */
        scratch[k] = '\0';
        ap(dst, cap, &off, "  code @RIP  : %s\n", scratch);
    } else {
        ap(dst, cap, &off,
           "  code @RIP  : unavailable - RIP is outside the kernel image, which "
           "is itself the diagnosis (a wild jump or a smashed return address)\n");
    }

    /* ---- how we got here ---- */
    if (r->bt_count == 0) {
        ap(dst, cap, &off,
           "  backtrace  : none - RBP (0x%016lx) is not a usable frame pointer "
           "and no return addresses were found on the stack\n",
           (unsigned long)f->rbp);
    } else if (r->bt_source == FAULT_BT_FRAME) {
        ap(dst, cap, &off, "  backtrace  : RBP chain, innermost first\n");
        ap(dst, cap, &off, "      #0  0x%016lx  (faulting instruction)\n",
           (unsigned long)f->rip);
        for (unsigned i = 0; i < r->bt_count; i++)
            ap(dst, cap, &off, "      #%u  0x%016lx\n",
               i + 1u, (unsigned long)r->bt[i]);
    } else {
        ap(dst, cap, &off,
           "  backtrace  : RBP unusable; these are raw-stack CANDIDATES scanned "
           "upward from RSP and may include stale frames\n");
        ap(dst, cap, &off, "      #0  0x%016lx  (faulting instruction)\n",
           (unsigned long)f->rip);
        for (unsigned i = 0; i < r->bt_count; i++)
            ap(dst, cap, &off, "      ?   0x%016lx\n", (unsigned long)r->bt[i]);
    }

    /* ---- what was done about it ----
     * The registers above are what the CPU handed over. This block is what the
     * kernel then chose, and it is deliberately last: a reader who stops at the
     * backtrace has the diagnosis, and a reader who goes on has the verdict. */
    if (r->fix_set) {
        ap(dst, cap, &off, "  disposition: %s - %s\n",
           fault_fix_name(r->fix), fault_fix_desc(r->fix));
        if (r->fix_detail[0])
            ap(dst, cap, &off, "             : %s\n", r->fix_detail);
        if (r->recovered)
            ap(dst, cap, &off,
               "  RESUMED    : 0x%016lx  <-- execution continued here; the "
               "machine is still running\n", (unsigned long)r->resume_rip);
        else
            ap(dst, cap, &off,
               "  HALTED     : execution did not resume\n");
    }

    ap(dst, cap, &off, RULE);
    return off;
}

/* ====================================================================== */
/* x86-64 instruction length                                              */
/* ====================================================================== */

/* THE RULE THIS CODE IS WRITTEN UNDER
 *   Return the right length, or return 0. There is no third acceptable
 *   behaviour, and "0" is not a failure — it is the answer that keeps the
 *   machine diagnosable. RIP + a wrong length resumes the CPU inside an
 *   instruction, where a displacement byte becomes an opcode and execution
 *   wanders off with no exception to mark where it went wrong.
 *
 *   So: an explicit whitelist of opcodes, and every byte not in it refuses.
 *   The ModRM/SIB/displacement arithmetic below is complete and exact — that
 *   part of x86 is regular. It is the opcode map that is partial, and it is
 *   partial in the safe direction.
 *
 * WHAT IS COVERED
 *   Legacy prefixes, REX, the one-byte map for everything a C compiler emits
 *   (arithmetic, mov, lea, push/pop, the shift and unary groups, jcc/jmp/call,
 *   test, xchg, in/out, the flag instructions, x87 escapes, int3/ud2), and the
 *   0F map for cmovcc, setcc, jcc rel32, movzx/movsx, bit ops, xadd/cmpxchg,
 *   bswap, cpuid/rdtsc/wrmsr, the CR/DR moves and the multi-byte NOPs.
 *
 * WHAT IS DELIBERATELY NOT
 *   VEX (C4/C5) and EVEX (62); the 0F 38 and 0F 3A three-byte maps; every SSE
 *   encoding (this kernel builds with -mno-sse, so one appearing is itself a
 *   bug worth halting on); the moffs forms A0..A3, whose immediate width
 *   depends on the address size in a way that is easy to get subtly wrong;
 *   ENTER, with its two immediates; the far branches, which are invalid in
 *   64-bit mode anyway; and a 66 prefix on a rel32 branch, where the AMD and
 *   Intel manuals disagree about whether the displacement narrows to 16 bits.
 */

#define OPV   0x80u   /* this opcode is known                                */
#define OPM   0x01u   /* has a ModRM byte                                    */
#define OPI1  0x02u   /* 1 immediate byte                                    */
#define OPI2  0x04u   /* 2 immediate bytes, fixed (only RET imm16)           */
#define OPIZ  0x08u   /* imm16 under 0x66, else imm32                        */
#define OPIV  0x10u   /* imm64 under REX.W, imm16 under 0x66, else imm32     */
#define OPBR  0x20u   /* a rel32 branch: refuse under 0x66 (see above)       */
#define OPG3  0x40u   /* F6/F7: an immediate only when the ModRM reg is 0/1  */

#define __  0u
#define V_  (OPV)
#define M_  (OPV | OPM)
#define A1  (OPV | OPI1)
#define A2  (OPV | OPI2)
#define AZ  (OPV | OPIZ)
#define AV  (OPV | OPIV)
#define BZ  (OPV | OPIZ | OPBR)
#define M1  (OPV | OPM | OPI1)
#define MZ  (OPV | OPM | OPIZ)
#define G3  (OPV | OPM | OPG3)

/* One-byte opcode map. Prefix bytes and REX read as __ because they are
 * consumed before the lookup; reaching one here means a prefix followed a REX,
 * which is malformed enough to refuse. */
static const uint8_t map1[256] = {
/*        0   1   2   3   4   5   6   7    8   9   a   b   c   d   e   f  */
/* 00 */ M_, M_, M_, M_, A1, AZ, __, __,  M_, M_, M_, M_, A1, AZ, __, __,
/* 10 */ M_, M_, M_, M_, A1, AZ, __, __,  M_, M_, M_, M_, A1, AZ, __, __,
/* 20 */ M_, M_, M_, M_, A1, AZ, __, __,  M_, M_, M_, M_, A1, AZ, __, __,
/* 30 */ M_, M_, M_, M_, A1, AZ, __, __,  M_, M_, M_, M_, A1, AZ, __, __,
/* 40 */ __, __, __, __, __, __, __, __,  __, __, __, __, __, __, __, __,
/* 50 */ V_, V_, V_, V_, V_, V_, V_, V_,  V_, V_, V_, V_, V_, V_, V_, V_,
/* 60 */ __, __, __, M_, __, __, __, __,  AZ, MZ, A1, M1, V_, V_, V_, V_,
/* 70 */ A1, A1, A1, A1, A1, A1, A1, A1,  A1, A1, A1, A1, A1, A1, A1, A1,
/* 80 */ M1, MZ, __, M1, M_, M_, M_, M_,  M_, M_, M_, M_, M_, M_, M_, M_,
/* 90 */ V_, V_, V_, V_, V_, V_, V_, V_,  V_, V_, __, V_, V_, V_, V_, V_,
/* a0 */ __, __, __, __, V_, V_, V_, V_,  A1, AZ, V_, V_, V_, V_, V_, V_,
/* b0 */ A1, A1, A1, A1, A1, A1, A1, A1,  AV, AV, AV, AV, AV, AV, AV, AV,
/* c0 */ M1, M1, A2, V_, __, __, M1, MZ,  __, V_, __, __, V_, A1, __, V_,
/* d0 */ M_, M_, M_, M_, __, __, __, V_,  M_, M_, M_, M_, M_, M_, M_, M_,
/* e0 */ A1, A1, A1, A1, A1, A1, A1, A1,  BZ, BZ, __, A1, V_, V_, V_, V_,
/* f0 */ __, V_, __, __, V_, V_, G3, G3,  V_, V_, V_, V_, V_, V_, M_, M_,
};

/* Two-byte (0F xx) map. */
static const uint8_t map2[256] = {
/*        0   1   2   3   4   5   6   7    8   9   a   b   c   d   e   f  */
/* 00 */ M_, M_, M_, M_, __, V_, V_, V_,  V_, V_, __, V_, __, M_, __, __,
/* 10 */ __, __, __, __, __, __, __, __,  M_, M_, M_, M_, M_, M_, M_, M_,
/* 20 */ M_, M_, M_, M_, __, __, __, __,  __, __, __, __, __, __, __, __,
/* 30 */ V_, V_, V_, V_, V_, V_, __, __,  __, __, __, __, __, __, __, __,
/* 40 */ M_, M_, M_, M_, M_, M_, M_, M_,  M_, M_, M_, M_, M_, M_, M_, M_,
/* 50 */ __, __, __, __, __, __, __, __,  __, __, __, __, __, __, __, __,
/* 60 */ __, __, __, __, __, __, __, __,  __, __, __, __, __, __, __, __,
/* 70 */ __, __, __, __, __, __, __, __,  __, __, __, __, __, __, __, __,
/* 80 */ BZ, BZ, BZ, BZ, BZ, BZ, BZ, BZ,  BZ, BZ, BZ, BZ, BZ, BZ, BZ, BZ,
/* 90 */ M_, M_, M_, M_, M_, M_, M_, M_,  M_, M_, M_, M_, M_, M_, M_, M_,
/* a0 */ V_, V_, V_, M_, M1, M_, __, __,  V_, V_, V_, M_, M1, M_, M_, M_,
/* b0 */ M_, M_, M_, M_, M_, M_, M_, M_,  M_, M_, M1, M_, M_, M_, M_, M_,
/* c0 */ M_, M_, __, M_, __, __, __, M_,  V_, V_, V_, V_, V_, V_, V_, V_,
/* d0 */ __, __, __, __, __, __, __, __,  __, __, __, __, __, __, __, __,
/* e0 */ __, __, __, __, __, __, __, __,  __, __, __, __, __, __, __, __,
/* f0 */ __, __, __, __, __, __, __, __,  __, __, __, __, __, __, __, __,
};

#undef __
#undef V_
#undef M_
#undef A1
#undef A2
#undef AZ
#undef AV
#undef BZ
#undef M1
#undef MZ
#undef G3

#define X86_MAX_INSN 15   /* architectural maximum; longer never decodes */

/* include/fault.h's FAULT_PATCH_MAX_BYTES calls itself "2 x the longest insn",
 * which is a hand-copy of the number above living in a different file. It happens
 * to be right (32 >= 30). Pin it, because a bound that mirrors a value computed
 * elsewhere goes stale in silence, and a patch buffer smaller than the span a
 * caller is allowed to replace is a truncated instruction stream — the one failure
 * mode in this file that does not fault and leaves no record. */
_Static_assert(FAULT_PATCH_MAX_BYTES >= 2 * X86_MAX_INSN,
               "FAULT_PATCH_MAX_BYTES must hold two maximum-length x86 "
               "instructions, as its own comment claims");

int x86_insn_length(const uint8_t *code, int avail) {
    if (!code || avail <= 0) return 0;
    if (avail > X86_MAX_INSN) avail = X86_MAX_INSN;

    int i     = 0;
    int osz16 = 0;      /* a 0x66 operand-size override was seen */
    int rexw  = 0;

    /* ---- legacy prefixes ----
     * Any number, any order, but they must all precede the REX byte. The loop
     * is bounded by `avail`, which is bounded by 15, so it always terminates. */
    for (;;) {
        if (i >= avail) return 0;
        uint8_t b = code[i];
        if (b == 0x66) { osz16 = 1; i++; continue; }
        if (b == 0x67 ||                                   /* address size   */
            b == 0xF0 || b == 0xF2 || b == 0xF3 ||         /* lock, rep      */
            b == 0x2E || b == 0x36 || b == 0x3E ||         /* segment (null  */
            b == 0x26 || b == 0x64 || b == 0x65) {         /*  in long mode) */
            i++;
            continue;
        }
        break;
    }

    /* ---- REX ----
     * Only the byte immediately before the opcode counts. If another prefix
     * follows one, the REX is architecturally ignored and the encoding is
     * strange enough that the map lookup below will refuse it — the prefix
     * bytes are all __ in map1. */
    if ((code[i] & 0xF0u) == 0x40u) {
        rexw = (code[i] & 0x08u) != 0;
        i++;
        if (i >= avail) return 0;
    }

    /* ---- opcode ---- */
    const uint8_t *map = map1;
    uint8_t        op  = code[i++];
    if (op == 0x0F) {
        if (i >= avail) return 0;
        map = map2;
        op  = code[i++];
    }

    unsigned f = map[op];
    if (!(f & OPV)) return 0;

    /* 0F 20..23 — MOV to/from CR and DR. The ModRM byte is present but its mod
     * field is ARCHITECTURALLY IGNORED: the operand is always a register, so
     * there is never a SIB byte and never a displacement, whatever mod says
     * (Intel SDM Vol 2, MOV control registers). Walking the generic ModRM path
     * below would add 1..5 bytes that are not there and resume PAST the end of
     * the instruction. This kernel really does execute `mov %rax,%cr3` and
     * friends in its boot path, so the encoding class is reachable. */
    if (map == map2 && op >= 0x20u && op <= 0x23u) {
        if (i >= avail) return 0;
        i++;                          /* the ModRM byte, and nothing else */
        if (i > avail)        return 0;
        if (i > X86_MAX_INSN) return 0;
        return i;
    }

    /* ---- ModRM, SIB, displacement ----
     * Exact, and the same for every opcode that has a ModRM byte. The 0x67
     * prefix selects 32-bit addressing rather than 16-bit in long mode, and the
     * encoding is byte-for-byte identical, so it does not affect the length. */
    if (f & OPM) {
        if (i >= avail) return 0;
        uint8_t  modrm = code[i++];
        unsigned mod   = (unsigned)(modrm >> 6);
        unsigned reg   = (unsigned)((modrm >> 3) & 7u);
        unsigned rm    = (unsigned)(modrm & 7u);

        if (mod != 3) {
            unsigned disp = 0;
            if (rm == 4) {                       /* a SIB byte follows */
                if (i >= avail) return 0;
                uint8_t sib = code[i++];
                if (mod == 0 && (sib & 7u) == 5u) disp = 4;   /* disp32, no base */
            } else if (mod == 0 && rm == 5) {
                disp = 4;                        /* RIP-relative */
            }
            if      (mod == 1) disp = 1;
            else if (mod == 2) disp = 4;
            i += (int)disp;
        }

        /* F6/F7 are the one group whose immediate depends on the reg field:
         * /0 and /1 are TEST with an immediate, /2../7 (NOT NEG MUL IMUL DIV
         * IDIV) have none. Getting this backwards on an IDIV — which is exactly
         * what a #DE fault lands on — would mis-skip by four bytes. */
        if ((f & OPG3) && reg <= 1)
            f |= (op == 0xF6) ? OPI1 : OPIZ;
    }

    /* ---- immediates ---- */
    if (f & OPI1) i += 1;
    if (f & OPI2) i += 2;
    if (f & OPIZ) {
        if ((f & OPBR) && osz16) return 0;   /* 66-prefixed rel32: ambiguous */
        /* REX.W WINS over the 0x66 operand-size prefix (Intel SDM Vol 2
         * 2.2.1.2): with both present the operand size is 64, so an imm-z is the
         * 4 bytes it always is at operand size 32 — not 2. Reading osz16 alone
         * here under-measures `66 48 c7 ...` and every other 66+REX.W imm-z form
         * by exactly two bytes, which resumes INSIDE the instruction. gcc never
         * emits these, but a set_rip or an earlier mis-skip can land on them. */
        i += (osz16 && !rexw) ? 2 : 4;
    }
    if (f & OPIV) i += rexw ? 8 : (osz16 ? 2 : 4);

    /* The whole instruction must have been inside the bytes we were given.
     * Beyond that a length would be an extrapolation, and this function does
     * not extrapolate. */
    if (i > avail)        return 0;
    if (i > X86_MAX_INSN) return 0;
    return i;
}

/* ====================================================================== */
/* registers                                                              */
/* ====================================================================== */

static const char *const reg_names[FAULT_REG_COUNT] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
};

const char *fault_reg_name(int reg) {
    if (reg < 0 || reg >= FAULT_REG_COUNT) return "?";
    return reg_names[reg];
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int fault_reg_lookup(const char *name) {
    if (!name) return -1;
    if (*name == '%') name++;
    for (int r = 0; r < FAULT_REG_COUNT; r++) {
        const char *w = reg_names[r];
        int         i = 0;
        while (w[i] && lower(name[i]) == w[i]) i++;
        if (!w[i] && !name[i]) return r;
    }
    return -1;
}

uint64_t *fault_reg_slot(fault_frame_t *f, int reg) {
    if (!f) return NULL;
    switch (reg) {
        case FAULT_REG_RAX: return &f->rax;
        case FAULT_REG_RCX: return &f->rcx;
        case FAULT_REG_RDX: return &f->rdx;
        case FAULT_REG_RBX: return &f->rbx;
        case FAULT_REG_RSP: return &f->rsp;
        case FAULT_REG_RBP: return &f->rbp;
        case FAULT_REG_RSI: return &f->rsi;
        case FAULT_REG_RDI: return &f->rdi;
        case FAULT_REG_R8:  return &f->r8;
        case FAULT_REG_R9:  return &f->r9;
        case FAULT_REG_R10: return &f->r10;
        case FAULT_REG_R11: return &f->r11;
        case FAULT_REG_R12: return &f->r12;
        case FAULT_REG_R13: return &f->r13;
        case FAULT_REG_R14: return &f->r14;
        case FAULT_REG_R15: return &f->r15;
        default:            return NULL;
    }
}

/* ====================================================================== */
/* the recovery decision                                                  */
/* ====================================================================== */

static const struct { const char *name; const char *desc; }
fix_tab[FAULT_FIX_COUNT] = {
    { "ok",
      "the armed recovery plan was applied and execution resumed" },
    { "trap",
      "a continuable trap; RIP already pointed past it, so nothing needed "
      "changing" },
    { "no-plan",
      "no recovery plan was armed, so the fault took its normal course" },
    { "refused",
      "the armed plan explicitly declined to recover from this fault" },
    { "vector",
      "this exception is never recoverable: #DF is raised because the stack "
      "itself is unusable, and #MC reports uncorrected hardware failure" },
    { "wrong-vector",
      "the armed plan was scoped to a different exception vector" },
    { "bad-plan",
      "the plan is malformed: unknown action, or a register that may not be "
      "written from here" },
    { "no-code",
      "RIP is outside the kernel image, so the faulting instruction could not "
      "be read and cannot be stepped over" },
    { "undecodable",
      "the bytes at RIP are not an encoding the length decoder recognises; "
      "skipping would resume in the middle of an instruction" },
    { "rip-window",
      "the resume address is outside [code_lo, code_hi), i.e. outside the "
      "loaded kernel image" },
    { "refault",
      "the same instruction faulted again after every recovery attempt; the "
      "kernel stopped rather than loop" },
    { "exhausted",
      "the armed plan had already been used as many times as it allowed" },
    { "escaped",
      "no in-place fix was possible, so the faulting call chain was abandoned "
      "and control returned to the nearest caller that armed a fault guard" },
    { "no-guard",
      "nothing had armed a fault guard, so there was no known-safe context to "
      "return to and the machine halted" },
    { "guard-spent",
      "the per-boot escape budget is spent; a machine that has abandoned this "
      "many call chains is one nobody can reason about, so it halts" },
    { "guard-corrupt",
      "the innermost fault guard's saved context does not pass its own bounds "
      "check, so returning to it would jump somewhere nobody chose" },
    { "guard-foreign",
      "a fault guard is armed, but on a different stack from the one that "
      "faulted; returning to it would resume this CPU on another fiber's stack, "
      "so the machine halted instead" },
};

const char *fault_fix_name(int fix) {
    if (fix < 0 || fix >= FAULT_FIX_COUNT) return "?";
    return fix_tab[fix].name;
}

const char *fault_fix_desc(int fix) {
    if (fix < 0 || fix >= FAULT_FIX_COUNT) return "unknown disposition";
    return fix_tab[fix].desc;
}

fault_fix_t fault_plan_check(const fault_plan_t *p, const fault_window_t *w) {
    if (!p)                             return FAULT_FIX_BAD_PLAN;
    if (p->action == FAULT_ACT_NONE)    return FAULT_FIX_NO_PLAN;
    if (p->action >= FAULT_ACT_COUNT)   return FAULT_FIX_BAD_PLAN;
    if (p->uses == 0 ||
        p->uses > FAULT_PLAN_USES_MAX)  return FAULT_FIX_BAD_PLAN;

    if (!p->any_vector) {
        if (p->vector > 255)                       return FAULT_FIX_BAD_PLAN;
        if (!fault_is_recoverable(p->vector))      return FAULT_FIX_VECTOR;
    }

    switch (p->action) {
        case FAULT_ACT_SET_REG:
            if (p->reg >= FAULT_REG_COUNT)  return FAULT_FIX_BAD_PLAN;
            /* RSP is refused, and not out of timidity. The saved RSP is what
             * IRETQ reloads; a wrong value there is the #DF scenario, delivered
             * on purpose. Recovery that needs a new stack is not recovery. */
            if (p->reg == FAULT_REG_RSP)    return FAULT_FIX_BAD_PLAN;
            break;
        case FAULT_ACT_SET_RIP:
            if (!w)                              return FAULT_FIX_RIP_WINDOW;
            if (!looks_like_code(p->value, w))   return FAULT_FIX_RIP_WINDOW;
            break;
        default:
            break;
    }
    return FAULT_FIX_OK;
}

/* Compute RIP + the length of the instruction at RIP, validating both the
 * decode and the destination. Returns a fault_fix_t; on FAULT_FIX_OK, *out is
 * the resume address and *len is the decoded length. Nothing is written to the
 * frame — the caller does that only once every check has passed. */
static fault_fix_t step_over(const fault_frame_t *f, const fault_window_t *w,
                             const uint8_t *code, unsigned code_len,
                             uint64_t *out, uint8_t *len) {
    if (!code || code_len == 0) return FAULT_FIX_NO_CODE;

    int n = x86_insn_length(code, (int)code_len);
    if (n <= 0) return FAULT_FIX_UNDECODABLE;
    *len = (uint8_t)n;

    uint64_t next = f->rip + (uint64_t)n;
    if (next < f->rip)              return FAULT_FIX_RIP_WINDOW;  /* wrapped */
    if (!looks_like_code(next, w))  return FAULT_FIX_RIP_WINDOW;
    *out = next;
    return FAULT_FIX_OK;
}

fault_fix_t fault_apply_plan(fault_frame_t *frame, const fault_window_t *window,
                             const fault_plan_t *plan,
                             const uint8_t *code, unsigned code_len,
                             char *why, size_t whycap, uint8_t *insn_len) {
    size_t off = 0;
    if (whycap)   why[0]     = '\0';
    if (insn_len) *insn_len  = 0;

    if (!frame || !window) {
        ap(why, whycap, &off, "no frame or no address window was supplied");
        return FAULT_FIX_BAD_PLAN;
    }
    if (!plan || plan->action == FAULT_ACT_NONE) {
        ap(why, whycap, &off, "no recovery plan was armed");
        return FAULT_FIX_NO_PLAN;
    }
    if (plan->action >= FAULT_ACT_COUNT) {
        ap(why, whycap, &off, "action %u is not one this kernel implements",
           (unsigned)plan->action);
        return FAULT_FIX_BAD_PLAN;
    }

    /* Vector eligibility outranks everything, including an explicit refusal:
     * for #DF the answer must be "this can never be recovered", not "the model
     * happened to say no this time". */
    if (!fault_is_recoverable(frame->vector)) {
        ap(why, whycap, &off, "%s is never resumable on this machine",
           fault_vector_name(frame->vector));
        return FAULT_FIX_VECTOR;
    }
    if (plan->action == FAULT_ACT_REFUSE) {
        ap(why, whycap, &off, "the armed plan chose not to recover");
        return FAULT_FIX_REFUSED;
    }
    if (plan->uses == 0) {
        ap(why, whycap, &off, "the armed plan has no uses left");
        return FAULT_FIX_EXHAUSTED;
    }
    if (!plan->any_vector && plan->vector != frame->vector) {
        ap(why, whycap, &off, "plan is scoped to vector %lu (%s), this is %lu (%s)",
           (unsigned long)plan->vector, fault_vector_name(plan->vector),
           (unsigned long)frame->vector, fault_vector_name(frame->vector));
        return FAULT_FIX_WRONG_VECTOR;
    }

    uint64_t    next = 0;
    uint8_t     len  = 0;
    fault_fix_t rc;

    switch (plan->action) {
        case FAULT_ACT_SKIP:
            rc = step_over(frame, window, code, code_len, &next, &len);
            if (insn_len) *insn_len = len;
            if (rc != FAULT_FIX_OK) {
                ap(why, whycap, &off,
                   "cannot step over the instruction at 0x%lx",
                   (unsigned long)frame->rip);
                return rc;
            }
            frame->rip = next;
            ap(why, whycap, &off,
               "skipped the %u-byte instruction at 0x%lx; resuming at 0x%lx",
               (unsigned)len, (unsigned long)(next - len), (unsigned long)next);
            return FAULT_FIX_OK;

        case FAULT_ACT_SET_REG: {
            if (plan->reg >= FAULT_REG_COUNT) {
                ap(why, whycap, &off, "register index %u does not exist",
                   (unsigned)plan->reg);
                return FAULT_FIX_BAD_PLAN;
            }
            if (plan->reg == FAULT_REG_RSP) {
                ap(why, whycap, &off,
                   "rsp may not be written from a recovery plan");
                return FAULT_FIX_BAD_PLAN;
            }
            uint64_t *slot = fault_reg_slot(frame, (int)plan->reg);
            if (!slot) {
                ap(why, whycap, &off, "register %u has no slot in the frame",
                   (unsigned)plan->reg);
                return FAULT_FIX_BAD_PLAN;
            }
            /* Decide the resume address BEFORE touching the register, so a
             * refusal leaves the frame exactly as the CPU left it. */
            if (plan->then_skip) {
                rc = step_over(frame, window, code, code_len, &next, &len);
                if (insn_len) *insn_len = len;
                if (rc != FAULT_FIX_OK) {
                    ap(why, whycap, &off,
                       "would set %s, but cannot then step over 0x%lx",
                       fault_reg_name((int)plan->reg),
                       (unsigned long)frame->rip);
                    return rc;
                }
            } else if (!looks_like_code(frame->rip, window)) {
                /* "Retry" is the one action that produces a resume address
                 * without computing one — it reuses the faulting RIP. That
                 * still has to satisfy the same rule as every other resume: a
                 * fault taken outside the loaded image means execution has
                 * already left the kernel, and setting a register does not
                 * bring it back. Re-running the instruction there would just
                 * execute whatever is at that address a second time. */
                ap(why, whycap, &off,
                   "0x%lx is outside the kernel image [0x%lx, 0x%lx), so there "
                   "is nothing safe to re-execute",
                   (unsigned long)frame->rip,
                   (unsigned long)window->code_lo,
                   (unsigned long)window->code_hi);
                return FAULT_FIX_RIP_WINDOW;
            }
            *slot = plan->value;
            if (plan->then_skip) {
                frame->rip = next;
                ap(why, whycap, &off,
                   "set %s = 0x%lx, then skipped %u bytes; resuming at 0x%lx",
                   fault_reg_name((int)plan->reg), (unsigned long)plan->value,
                   (unsigned)len, (unsigned long)next);
            } else {
                ap(why, whycap, &off,
                   "set %s = 0x%lx and re-executed the instruction at 0x%lx",
                   fault_reg_name((int)plan->reg), (unsigned long)plan->value,
                   (unsigned long)frame->rip);
            }
            return FAULT_FIX_OK;
        }

        case FAULT_ACT_SET_RIP:
            if (!looks_like_code(plan->value, window)) {
                ap(why, whycap, &off,
                   "0x%lx is outside the kernel image [0x%lx, 0x%lx)",
                   (unsigned long)plan->value,
                   (unsigned long)window->code_lo,
                   (unsigned long)window->code_hi);
                return FAULT_FIX_RIP_WINDOW;
            }
            ap(why, whycap, &off, "resumed at 0x%lx instead of 0x%lx",
               (unsigned long)plan->value, (unsigned long)frame->rip);
            frame->rip = plan->value;
            return FAULT_FIX_OK;

        default:
            ap(why, whycap, &off, "action %u is not implemented",
               (unsigned)plan->action);
            return FAULT_FIX_BAD_PLAN;
    }
}

/* ====================================================================== */
/* the live state: a ring of records, one armed plan, one re-fault counter */
/* ====================================================================== */

/* WHY A RING AND NOT A SLOT
 *   This started as exactly one record, which was sound while every fault was
 *   terminal: there could never be a second one. The moment a fault can resume,
 *   the single slot becomes a use-after-free with extra steps — tools/
 *   fault_tools.c hands the model a pointer into it, the model's next tool call
 *   faults, and the record changes identity underneath a caller that is halfway
 *   through describing it. The ring makes a retained record immutable for
 *   FAULT_RING_SIZE further faults, which is the whole guarantee a reader
 *   needs. */
static fault_record_t g_ring[FAULT_RING_SIZE];
static uint32_t       g_count;         /* faults since boot; also the next seq */
static uint32_t       g_head;          /* ring index of the newest record      */
static char           g_report[FAULT_REPORT_MAX];

static fault_window_t g_window;
static int            g_window_set;

static fault_plan_t   g_plan;
static int            g_plan_armed;

/* Consecutive faults at one address. This is the guard that keeps a recovery
 * loop finite, and it is separate from idt.c's in_handler depth guard on
 * purpose: that one catches a fault raised INSIDE the handler, which is never
 * survivable; this one catches a fault raised AFTER a successful resume, which
 * is survivable up to a point and then is not. */
static uint64_t       g_refault_rip;
static uint32_t       g_refaults;

/* Escape accounting. Declared up here with the rest of the live state because
 * fault_reset() below clears it; the mechanism itself is at the bottom of the
 * file under "the fault guard". */
static uint32_t       g_escapes;
static const char    *g_escape_what = "";
static uint32_t       g_escape_seq;

void fault_set_window(const fault_window_t *w) {
    if (!w) { g_window_set = 0; return; }
    g_window     = *w;
    g_window_set = 1;
}

const fault_window_t *fault_window(void) {
    return g_window_set ? &g_window : NULL;
}

void fault_note(const fault_frame_t *frame, const fault_window_t *window,
                uint64_t uptime_ms) {
    /* Refuse before touching anything. fault_capture() zeroes its destination
     * first, so entering the ring with nothing to record would destroy the
     * oldest retained fault in exchange for an empty slot — the opposite of
     * what the ring is for. */
    if (!frame || !window) return;

    g_count++;
    g_head = (g_count - 1u) % FAULT_RING_SIZE;

    fault_record_t *r = &g_ring[g_head];
    fault_capture(frame, window, uptime_ms, r);
    r->seq = g_count;
    fault_format_report(r, g_report, sizeof g_report);
}

static fault_record_t *current_record(void) {
    if (g_count == 0) return NULL;
    return g_ring[g_head].valid ? &g_ring[g_head] : NULL;
}

/* Bounded copy into the record's detail line. No strncpy: the freestanding
 * build's string.h is a shim and this must not depend on it. */
static void set_detail(fault_record_t *r, const char *s) {
    size_t i = 0;
    if (!r) return;
    if (s) for (; s[i] && i + 1 < sizeof r->fix_detail; i++) r->fix_detail[i] = s[i];
    r->fix_detail[i] = '\0';
}

fault_fix_t fault_recover(fault_frame_t *frame, const fault_window_t *window) {
    char        why[FAULT_DETAIL_MAX];
    size_t      off = 0;
    uint8_t     ilen = 0;
    fault_fix_t fix;

    why[0] = '\0';
    if (!frame) return FAULT_FIX_BAD_PLAN;

    fault_record_t *r = current_record();

    /* Re-fault accounting runs first and unconditionally, so the counter
     * reflects what the machine did rather than what was attempted. */
    if (g_refaults != 0 && frame->rip == g_refault_rip) {
        g_refaults++;
    } else {
        g_refault_rip = frame->rip;
        g_refaults    = 1;
    }

    if (!fault_is_fatal(frame->vector)) {
        /* #DB, #BP, or an unexpected vector >= 32. RIP already points past the
         * trapping instruction; nothing in the frame is touched. Deliberately
         * exempt from the re-fault limit — these are not recoveries, they are
         * the architecture working as designed. */
        fix = FAULT_FIX_TRAP;
        ap(why, sizeof why, &off,
           "%s does not point RIP at an unexecutable instruction, so there is "
           "nothing to step over; the frame was not modified",
           fault_vector_name(frame->vector));
    } else if (g_refaults > FAULT_REFAULT_MAX) {
        fix = FAULT_FIX_REFAULT;
        ap(why, sizeof why, &off,
           "0x%lx has now faulted %lu times in a row; giving up after %u "
           "recovery attempts",
           (unsigned long)frame->rip, (unsigned long)g_refaults,
           (unsigned)FAULT_REFAULT_MAX);
    } else {
        const fault_plan_t *p    = g_plan_armed ? &g_plan : NULL;
        const uint8_t      *code = (r && r->code_valid) ? r->code : NULL;
        unsigned            clen = (r && r->code_valid) ? r->code_len : 0u;

        fix = fault_apply_plan(frame, window, p, code, clen,
                               why, sizeof why, &ilen);

        if (fix == FAULT_FIX_OK) {
            /* A plan is consumed by use. One armed decision must not silently
             * absorb an unbounded stream of faults. */
            if (g_plan.uses > 0) g_plan.uses--;
            if (g_plan.uses == 0) g_plan_armed = 0;
        }
    }

    if (r) {
        r->fix_set    = 1;
        r->fix        = (uint8_t)fix;
        r->insn_len   = ilen;
        r->recovered  = (fix == FAULT_FIX_OK || fix == FAULT_FIX_TRAP) ? 1u : 0u;
        r->resume_rip = r->recovered ? frame->rip : 0;
        set_detail(r, why);
        fault_format_report(r, g_report, sizeof g_report);
    }
    return fix;
}

fault_fix_t fault_plan_arm(const fault_plan_t *p) {
    fault_fix_t rc = fault_plan_check(p, fault_window());
    if (rc != FAULT_FIX_OK) return rc;
    g_plan       = *p;
    g_plan_armed = 1;
    return FAULT_FIX_OK;
}

void fault_plan_clear(void) {
    zero(&g_plan, sizeof g_plan);
    g_plan_armed = 0;
}

const fault_plan_t *fault_plan_get(void) {
    return g_plan_armed ? &g_plan : NULL;
}

uint32_t fault_refaults(void)    { return g_refaults; }
uint64_t fault_refault_rip(void) { return g_refault_rip; }

int fault_occurred(void) { return current_record() != NULL; }

uint32_t fault_count(void) { return g_count; }

const fault_record_t *fault_last(void) { return current_record(); }

const char *fault_last_report(void) {
    return current_record() ? g_report : "no fault has been captured\n";
}

uint32_t fault_ring_len(void) {
    return g_count < FAULT_RING_SIZE ? g_count : (uint32_t)FAULT_RING_SIZE;
}

const fault_record_t *fault_ring_at(uint32_t back) {
    if (back >= fault_ring_len()) return NULL;
    uint32_t idx = (g_head + FAULT_RING_SIZE - back) % FAULT_RING_SIZE;
    return g_ring[idx].valid ? &g_ring[idx] : NULL;
}

const fault_record_t *fault_by_seq(uint32_t seq) {
    if (seq == 0) return NULL;
    for (uint32_t i = 0; i < FAULT_RING_SIZE; i++)
        if (g_ring[i].valid && g_ring[i].seq == seq) return &g_ring[i];
    return NULL;
}

void fault_reset(void) {
    zero(g_ring, sizeof g_ring);
    g_count       = 0;
    g_head        = 0;
    g_report[0]   = '\0';
    g_refault_rip = 0;
    g_refaults    = 0;
    zero(&g_window, sizeof g_window);
    g_window_set  = 0;
    fault_plan_clear();
    /* The guard stack is deliberately NOT cleared here. fault_reset() is a test
     * hook and a test may well be running inside a guard while it calls this;
     * dropping the entry would leave fault_guard_run() popping a depth that no
     * longer describes the machine. The escape COUNTERS are policy and are
     * cleared, because they are what a test needs a clean sheet of. Live code
     * patches are not touched either, for a much sharper reason: forgetting a
     * patch does not un-write it, so it would strand modified .text with no way
     * back. fault_patch_forget_all() exists for a test that really means it. */
    g_escapes      = 0;
    g_escape_what  = "";
    g_escape_seq   = 0;
}

/* ====================================================================== */
/* the fault guard — returning to a known-safe context                    */
/* ====================================================================== */

/* See include/fault.h for the whole argument. This half is bookkeeping plus two
 * bounds checks; the only interesting code is the arch seam below it. */

static fault_guard_t *g_guards[FAULT_GUARD_DEPTH_MAX];
static int            g_gdepth;

/* Who says which stack we are on. NULL = "there is only one", which was true when
 * this was written and is still true of every host suite. See the long note beside
 * fault_guard_t in include/fault.h for what goes wrong without it. */
static fault_stack_id_fn g_stack_id;

void fault_set_stack_id(fault_stack_id_fn fn) { g_stack_id = fn; }

static const void *stack_id_now(void) {
    return g_stack_id ? g_stack_id() : (const void *)0;
}

int fault_guard_depth(void) { return g_gdepth; }

fault_guard_t *fault_guard_innermost(void) {
    return (g_gdepth > 0) ? g_guards[g_gdepth - 1] : NULL;
}

uint32_t fault_escapes(void) { return g_escapes; }

uint32_t fault_escape_budget(void) {
    return (g_escapes >= FAULT_ESCAPE_MAX)
               ? 0u : (uint32_t)FAULT_ESCAPE_MAX - g_escapes;
}

const char *fault_escape_what(void) { return g_escape_what; }
uint32_t    fault_escape_seq(void)  { return g_escape_seq; }

/* ---- the arch seam ----
 *
 * fault_guard_enter() must be the function whose OWN return address is saved:
 * anything that wraps it would have its frame popped and then reused by the body
 * before the escape came back to it, so the epilogue would read garbage. That is
 * why this is hand-written rather than a C wrapper around a helper.
 *
 * The asm addresses `slot` at +0 in the struct, which the assertion below pins.
 * Only the callee-saved set is saved: the kernel is built -mno-sse -mno-mmx with
 * no x87 and IF is always 0, so there is nothing else that survives a call
 * boundary and therefore nothing else a resume has to reconstruct. This is the
 * identical argument arch/x86_64/switch.asm makes for fibers, and if one of them
 * is ever wrong they are both wrong. */

_Static_assert(__builtin_offsetof(fault_guard_t, slot) == 0,
               "the asm below writes slot[] at +0 in fault_guard_t");
_Static_assert(FAULT_GUARD_SLOTS >= 8,
               "the x86-64 context needs eight slots");

#ifdef FABLEOS_HOSTTEST

/* The host is a hosted environment on some other architecture entirely (this
 * tree is developed on arm64), so the transfer is libc's. The saved x86-64
 * context in slot[0..7] is then meaningless, which is why fault_guard_forge()
 * exists: fault_escape()'s register arithmetic is checked against a forged
 * context, and the control transfer is checked against this one. Two tests for
 * what is one mechanism on the machine — said out loud because it is the one
 * place the host build and the kernel build genuinely differ. */
#include <setjmp.h>
_Static_assert(sizeof(jmp_buf) <= FAULT_GUARD_SLOTS * sizeof(uint64_t),
               "FAULT_GUARD_SLOTS must hold a host jmp_buf");

int fault_guard_enter(fault_guard_t *g) {
    return setjmp(*(jmp_buf *)(void *)g->slot);
}

void fault_guard_unwind(fault_guard_t *g, int code) {
    longjmp(*(jmp_buf *)(void *)g->slot, code ? code : FAULT_GUARD_ESCAPED);
}

int fault_guard_forge(fault_guard_t *g, uint64_t rip, uint64_t rsp) {
    if (!g) return -1;
    g->slot[0] = rip;
    g->slot[1] = rsp;
    for (int i = 2; i < 8; i++) g->slot[i] = 0xC0DE0000ULL + (uint64_t)i;
    return 0;
}

#elif defined(__x86_64__)

/* WRITTEN IN INTEL SYNTAX ON PURPOSE, and it is not a style preference.
 *
 * tests/qemu/lint_printf.py scans every string literal in the first-party tree
 * for conversions lib/kfmt.c cannot render, because a `%e` that the kernel's
 * formatter does not implement is echoed literally AND consumes no vararg, so
 * every later argument shifts. It cannot tell a format string from an asm
 * template, and AT&T syntax spells registers `%eax` / `%esi` — four false
 * findings in a file that formats a great deal of model-facing text, in a lint
 * whose whole value is that it has no false positives to train people to ignore.
 * `.intel_syntax noprefix` writes the same instructions with no '%' anywhere.
 * `.att_syntax` restores the assembler's state for the compiler-generated code
 * that follows. */
__asm__(
    ".text\n"
    ".intel_syntax noprefix\n"

    ".globl fault_guard_enter\n"
    ".type  fault_guard_enter,@function\n"
    "fault_guard_enter:\n"           /* rdi = guard                          */
    "    mov  rax, [rsp]\n"          /* slot[0] = where to come back to       */
    "    mov  [rdi], rax\n"
    "    lea  rax, [rsp+8]\n"        /* slot[1] = RSP as of the call site     */
    "    mov  [rdi+8], rax\n"
    "    mov  [rdi+16], rbx\n"
    "    mov  [rdi+24], rbp\n"
    "    mov  [rdi+32], r12\n"
    "    mov  [rdi+40], r13\n"
    "    mov  [rdi+48], r14\n"
    "    mov  [rdi+56], r15\n"
    "    xor  eax, eax\n"            /* 0 = "I have just saved"               */
    "    ret\n"
    ".size fault_guard_enter,.-fault_guard_enter\n"

    ".globl fault_guard_unwind\n"
    ".type  fault_guard_unwind,@function\n"
    "fault_guard_unwind:\n"          /* rdi = guard, esi = code               */
    "    mov  eax, esi\n"
    "    test eax, eax\n"
    "    jne  1f\n"
    "    mov  eax, 1\n"              /* never hand back 0: see the header     */
    "1:\n"
    "    mov  rbx, [rdi+16]\n"
    "    mov  rbp, [rdi+24]\n"
    "    mov  r12, [rdi+32]\n"
    "    mov  r13, [rdi+40]\n"
    "    mov  r14, [rdi+48]\n"
    "    mov  r15, [rdi+56]\n"
    "    mov  rsp, [rdi+8]\n"        /* last, so nothing below is needed      */
    "    jmp  qword ptr [rdi]\n"
    ".size fault_guard_unwind,.-fault_guard_unwind\n"

    ".att_syntax\n"
);

/* Forging a context is a test affordance and must not exist on the machine: a
 * guard whose slots anyone can write is a jump to an address anyone can choose,
 * which is precisely what the bounds checks in fault_escape() are there to stop.
 * So the kernel build refuses. */
int fault_guard_forge(fault_guard_t *g, uint64_t rip, uint64_t rsp) {
    (void)g; (void)rip; (void)rsp;
    return -1;
}

#else
#error "fault_guard_enter needs an implementation for this architecture"
#endif

/* ---- the portable half ---- */

int fault_guard_run(const char *what, void (*body)(void *ctx), void *ctx) {
    fault_guard_t g;

    if (!body) return FAULT_GUARD_EINVAL;
    if (g_gdepth >= FAULT_GUARD_DEPTH_MAX) return FAULT_GUARD_EDEPTH;

    zero(&g, sizeof g);
    g.magic = FAULT_GUARD_MAGIC;
    g.depth = (uint32_t)g_gdepth;
    g.code  = 0;
    g.seq   = g_count;
    g.what  = (what && what[0]) ? what : "an unnamed guarded action";
    g.stack = stack_id_now();     /* whose stack this guard's frame lives on */

    g_guards[g_gdepth++] = &g;

    if (fault_guard_enter(&g) == 0)
        body(ctx);

    /* EVERYTHING BELOW THIS LINE MUST READ MEMORY, NOT REGISTERS.
     *
     * On the escape path execution arrives here through an IRETQ that restored
     * only RSP, RBP, RBX and R12-R15. Any value the compiler chose to keep in a
     * caller-saved register across the call to body() is gone — including, if it
     * felt like it, `body`, `ctx`, or a copy of the depth. `g` is on this frame's
     * own stack, its address is derived from the registers that WERE restored,
     * and the escape code was written into g.code by fault_escape() precisely so
     * that not even the return-value register has to be believed. */
    g_gdepth = (int)g.depth;      /* pops us, and anything nested under us */
    g.magic  = 0;                 /* a stale pointer cannot pass the check */

    return g.code;
}

/* Shared by both entry points: is an escape allowed AT ALL, before any register
 * is looked at? Vector-independent checks only; fault_escape() adds the vector
 * and the bounds. */
static fault_fix_t escape_allowed(fault_guard_t **out) {
    if (out) *out = NULL;
    if (g_gdepth <= 0)                  return FAULT_FIX_NO_GUARD;
    if (g_escapes >= FAULT_ESCAPE_MAX)  return FAULT_FIX_GUARD_SPENT;

    fault_guard_t *g = g_guards[g_gdepth - 1];
    if (!g || g->magic != FAULT_GUARD_MAGIC) return FAULT_FIX_GUARD_CORRUPT;

    /* IDENTITY, NOT ARITHMETIC. The guard's frame is on the stack of whoever armed
     * it; restoring its RSP while a DIFFERENT stack is running would put this CPU
     * on somebody else's stack with `current` still naming the faulting context.
     * The bounds checks in fault_escape() cannot catch that — every fiber stack is
     * inside the one declared stack window — so the only sound test is whether the
     * context that faulted is the context that armed. Not searching further out for
     * a guard that DOES match: fault_guard_run() pops by index, and unwinding past
     * a foreign guard would remove it while its owner is still inside it. That
     * needs a per-fiber guard stack, and fault.h says so. */
    if (g->stack != stack_id_now()) return FAULT_FIX_GUARD_FOREIGN;

    if (out) *out = g;
    return FAULT_FIX_OK;
}

fault_fix_t fault_guard_abort(const char *why) {
    fault_guard_t *g   = NULL;
    fault_fix_t    rc  = escape_allowed(&g);
    if (rc != FAULT_FIX_OK) return rc;

    g_escapes++;
    g_escape_what = why && why[0] ? why : g->what;
    g_escape_seq  = g_count;
    g->code       = FAULT_GUARD_ESCAPED;
    fault_guard_unwind(g, FAULT_GUARD_ESCAPED);
}

/* A REFUSAL IS ALSO A DISPOSITION, and it used to be recorded nowhere.
 *
 * Only the success path wrote into the ring, so a guard that was armed and intact
 * but refused left the record holding fault_recover()'s verdict — almost always
 * "no-plan: no recovery plan was armed", which is FALSE when a guard was armed and
 * is the first sentence both the operator and net/faultchat.c read. The refusal
 * only replaces a NO_PLAN, because every other recover verdict says something
 * specific about a plan that really did exist.
 *
 * Writes nothing but the record and the rendered report: the frame is untouched on
 * every path through here, which is the property that makes refusing always safe. */
static void note_refusal(fault_fix_t why, const char *detail) {
    fault_record_t *r = current_record();
    if (!r) return;
    if (r->fix_set && r->fix != (uint8_t)FAULT_FIX_NO_PLAN) return;
    r->fix_set   = 1;
    r->fix       = (uint8_t)why;
    r->recovered = 0;
    set_detail(r, detail);
    fault_format_report(r, g_report, sizeof g_report);
}

fault_fix_t fault_escape(fault_frame_t *frame, const fault_window_t *window) {
    fault_guard_t *g = NULL;
    fault_fix_t    rc;

    if (!frame || !window) return FAULT_FIX_NO_GUARD;

    /* #DF and #MC first, and for the same reason fault_is_recoverable() gives:
     * every field this mechanism writes is a stack address, and #DF means the
     * CPU has already proved the stack is unusable. There is no version of
     * "abandon the call chain" that helps when the thing that broke is the
     * mechanism used to abandon it. */
    if (!fault_is_recoverable(frame->vector)) return FAULT_FIX_VECTOR;

    rc = escape_allowed(&g);
    if (rc != FAULT_FIX_OK) {
        if (rc == FAULT_FIX_GUARD_FOREIGN)
            note_refusal(rc, "a fault guard is armed, but it was armed on a "
                             "different stack from the one that faulted; "
                             "resuming on it would run this CPU on another "
                             "fiber's stack");
        else if (rc == FAULT_FIX_GUARD_SPENT)
            note_refusal(rc, "a fault guard is armed and intact, but this boot's "
                             "escape budget is spent");
        else if (rc == FAULT_FIX_GUARD_CORRUPT)
            note_refusal(rc, "a fault guard is armed but its magic is wrong; "
                             "something has written over it");
        return rc;
    }

    /* Bounds, re-checked at the moment of use against the live window. The guard
     * lives on the stack of the code being escaped FROM, so the bug being
     * escaped is exactly the kind of bug that could have scribbled on it. */
    uint64_t rip = g->slot[0];
    uint64_t rsp = g->slot[1];
    if (!in_range(rip, 1, window->code_lo, window->code_hi)) {
        note_refusal(FAULT_FIX_GUARD_CORRUPT,
                     "the fault guard's saved resume address is outside this "
                     "kernel's image, so returning to it would jump nowhere");
        return FAULT_FIX_GUARD_CORRUPT;
    }
    if (!in_range(rsp, 8, window->stack_lo, window->stack_hi)) {
        note_refusal(FAULT_FIX_GUARD_CORRUPT,
                     "the fault guard's saved stack pointer is outside the "
                     "kernel's stack window");
        return FAULT_FIX_GUARD_CORRUPT;
    }
    /* The saved RSP must be ABOVE the faulting RSP: the guard's frame has to
     * outlive the frames being discarded, or "return to the caller" is really
     * "jump into memory the handler is standing on". */
    if (rsp <= frame->rsp) {
        note_refusal(FAULT_FIX_GUARD_CORRUPT,
                     "the fault guard's saved stack pointer is not above the "
                     "faulting one, so its frame does not outlive the frames "
                     "being discarded");
        return FAULT_FIX_GUARD_CORRUPT;
    }

    /* Committed. Nothing below can fail. */
    g_escapes++;
    g_escape_what = g->what;
    g_escape_seq  = g_count;
    g->code       = FAULT_GUARD_ESCAPED;

    frame->rip = rip;
    frame->rsp = rsp;
    frame->rbx = g->slot[2];
    frame->rbp = g->slot[3];
    frame->r12 = g->slot[4];
    frame->r13 = g->slot[5];
    frame->r14 = g->slot[6];
    frame->r15 = g->slot[7];
    frame->rax = FAULT_GUARD_ESCAPED;   /* fault_guard_enter's "return value" */

    /* TF would single-step the resumed code and RF would suppress the next
     * instruction breakpoint; neither belongs to the context being resumed. */
    frame->rflags &= ~0x00000100ULL;    /* TF */
    frame->rflags &= ~0x00010000ULL;    /* RF */

    {
        fault_record_t *r = current_record();
        if (r) {
            char   why[FAULT_DETAIL_MAX];
            size_t off = 0;
            why[0] = '\0';
            ap(why, sizeof why, &off,
               "no in-place fix; abandoned %s and returned to its caller at "
               "0x%lx (escape %lu of %u this boot)",
               g->what, (unsigned long)rip,
               (unsigned long)g_escapes, (unsigned)FAULT_ESCAPE_MAX);
            r->fix_set    = 1;
            r->fix        = (uint8_t)FAULT_FIX_ESCAPED;
            r->recovered  = 1;
            r->resume_rip = rip;
            set_detail(r, why);
            fault_format_report(r, g_report, sizeof g_report);
        }
    }
    return FAULT_FIX_ESCAPED;
}

/* ====================================================================== */
/* live code patching                                                     */
/* ====================================================================== */

static const struct { const char *name; const char *desc; }
patch_tab[FAULT_PATCH_COUNT] = {
    { "OK",        "the patch is legal" },
    { "ENOWINDOW", "this build did not publish a .text range, so no address "
                   "can be shown to be executable code and every patch is "
                   "refused" },
    { "ENOMEM",    "no memory accessor is bound, so nothing can be read or "
                   "written" },
    { "ERANGE",    "the target is not inside the kernel's .text; a patch may "
                   "only touch instructions the linker placed" },
    { "ELEN",      "the length is zero, over the cap, or does not match the "
                   "number of expected bytes given" },
    { "EEXPECT",   "the bytes at that address are not the ones stated, so the "
                   "address is stale or wrong and something else lives there" },
    { "EBOUNDARY", "the span does not cover a whole number of instructions, so "
                   "the patch would leave the tail of one to be executed as an "
                   "opcode" },
    { "EOPAQUE",   "the replacement bytes do not decode to exactly that many "
                   "bytes of whole instructions" },
    { "EOVERLAP",  "a patch that is still live already covers those bytes; "
                   "revert it before patching them again" },
    { "ENOSPC",    "every patch slot is in use" },
    { "ENOENT",    "there is no patch with that id" },
    { "EGONE",     "that patch has already been reverted" },
    { "ECHANGED",  "the bytes in memory are not the ones this patch wrote, so "
                   "restoring the original would destroy someone else's edit" },
    { "EIO",       "the bytes did not read back as written" },
};

const char *fault_patch_errname(int e) {
    if (e < 0 || e >= FAULT_PATCH_COUNT) return "?";
    return patch_tab[e].name;
}

const char *fault_patch_errdesc(int e) {
    if (e < 0 || e >= FAULT_PATCH_COUNT) return "unknown patch result";
    return patch_tab[e].desc;
}

static fault_patch_t g_patch[FAULT_PATCH_SLOTS];
static uint32_t      g_patches;        /* slots used; also the next id      */
static uint32_t      g_patch_live;

static const fault_mem_ops_t *g_mem;

#if !defined(FABLEOS_HOSTTEST) && defined(__x86_64__)
/* The kernel's default accessor. Legal because boot.asm identity-maps the low
 * 4 GiB, so a virtual address below 4 GiB IS its physical address, and because
 * every caller has already proved the span lies inside .text. volatile so the
 * compiler cannot decide a byte it just wrote is still the byte it read. */
static int mem_read_direct(uint64_t a, uint8_t *dst, unsigned n, void *ctx) {
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)a;
    (void)ctx;
    for (unsigned i = 0; i < n; i++) dst[i] = p[i];
    return 0;
}

static int mem_write_direct(uint64_t a, const uint8_t *src, unsigned n,
                            void *ctx) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)a;
    (void)ctx;
    for (unsigned i = 0; i < n; i++) p[i] = src[i];
    /* Intel SDM 8.1.3: after storing to an instruction that may already be in
     * the pipeline, a serialising instruction is required before executing it.
     * CPUID is the cheapest one that needs no privilege and no operand state. */
    {
        unsigned a_ = 0, b_ = 0, c_ = 0, d_ = 0;
        __asm__ __volatile__("cpuid"
                             : "=a"(a_), "=b"(b_), "=c"(c_), "=d"(d_)
                             : "a"(0) : "memory");
    }
    return 0;
}

static const fault_mem_ops_t mem_direct = {
    mem_read_direct, mem_write_direct, NULL
};
#define MEM_DEFAULT (&mem_direct)
#else
/* On the host there is deliberately no default: a test that forgets to bind an
 * array gets FAULT_PATCH_ENOMEM instead of writing to address 0x1000. */
#define MEM_DEFAULT ((const fault_mem_ops_t *)0)
#endif

void fault_patch_bind(const fault_mem_ops_t *ops) {
    g_mem = ops ? ops : MEM_DEFAULT;
}

static const fault_mem_ops_t *mem_ops(void) {
    return g_mem ? g_mem : MEM_DEFAULT;
}

/* Bounded copy of a kernel- or model-authored note. */
static void note_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) {
            unsigned char c = (unsigned char)src[i];
            /* This string is printed inside a kernel-prefixed line and stored in
             * a record the model reads back. Control characters and brackets
             * would let it forge a trace line; see include/trace.h. */
            if (c < 0x20 || c == 0x7F) c = ' ';
            else if (c == '[')         c = '(';
            else if (c == ']')         c = ')';
            dst[i] = (char)c;
        }
    }
    dst[i] = '\0';
}

/* Walk instructions across exactly `len` bytes. Returns the instruction count
 * when the lengths sum to `len` precisely, or 0 when they do not — which covers
 * both "one instruction runs off the end" (x86_insn_length is given only the
 * remaining bytes and refuses) and "an encoding the decoder does not know". */
static int span_insns(const uint8_t *b, unsigned len) {
    unsigned off = 0;
    int      n   = 0;
    while (off < len) {
        int l = x86_insn_length(b + off, (int)(len - off));
        if (l <= 0) return 0;
        if ((unsigned)l > len - off) return 0;   /* cannot happen; never trust */
        off += (unsigned)l;
        n++;
        if (n > FAULT_PATCH_MAX_BYTES) return 0;
    }
    return (off == len) ? n : 0;
}

/* Shared gate. `out_insns_out` / `out_insns_in` are filled on success so
 * fault_patch_apply() does not decode twice. */
static fault_patch_err_t patch_gate(const fault_patch_req_t *req,
                                    const fault_window_t *w,
                                    uint8_t *found,
                                    int *out_insns_out, int *out_insns_in,
                                    char *why, size_t whycap) {
    size_t off = 0;
    if (whycap) why[0] = '\0';

    if (!req) {
        ap(why, whycap, &off, "no patch was described");
        return FAULT_PATCH_ELEN;
    }
    /* ---- gate 0: can we do this at all ---- */
    if (!w || w->text_hi <= w->text_lo) {
        ap(why, whycap, &off,
           "this build published no .text range, so no address can be shown to "
           "be executable code");
        return FAULT_PATCH_ENOWINDOW;
    }
    if (!mem_ops() || !mem_ops()->read || !mem_ops()->write) {
        ap(why, whycap, &off, "no memory accessor is bound");
        return FAULT_PATCH_ENOMEM;
    }

    /* ---- gate 1: lengths ---- */
    if (req->len == 0 || req->len > FAULT_PATCH_MAX_BYTES) {
        ap(why, whycap, &off,
           "a patch is 1 to %u bytes; this one is %u",
           (unsigned)FAULT_PATCH_MAX_BYTES, (unsigned)req->len);
        return FAULT_PATCH_ELEN;
    }
    if (req->expect_len != req->len) {
        ap(why, whycap, &off,
           "\"expect\" must be exactly as long as the replacement: %u byte(s) "
           "of replacement, %u of expected",
           (unsigned)req->len, (unsigned)req->expect_len);
        return FAULT_PATCH_ELEN;
    }

    /* ---- gate 2: inside .text ---- */
    if (!in_range(req->addr, req->len, w->text_lo, w->text_hi)) {
        ap(why, whycap, &off,
           "[0x%lx, 0x%lx) is not inside this kernel's .text, which is "
           "[0x%lx, 0x%lx)",
           (unsigned long)req->addr,
           (unsigned long)(req->addr + req->len),
           (unsigned long)w->text_lo, (unsigned long)w->text_hi);
        return FAULT_PATCH_ERANGE;
    }

    /* ---- gate 3: the caller must already know what is there ---- */
    if (mem_ops()->read(req->addr, found, req->len, mem_ops()->ctx) != 0) {
        ap(why, whycap, &off, "the bytes at 0x%lx could not be read",
           (unsigned long)req->addr);
        return FAULT_PATCH_ENOMEM;
    }
    for (unsigned i = 0; i < req->len; i++) {
        if (found[i] == req->expect[i]) continue;
        ap(why, whycap, &off,
           "byte %u at 0x%lx is 0x%02x, not the 0x%02x you said to expect - "
           "the address is stale or belongs to something else. Found:",
           i, (unsigned long)(req->addr + i),
           (unsigned)found[i], (unsigned)req->expect[i]);
        for (unsigned j = 0; j < req->len; j++)
            ap(why, whycap, &off, " %02x", (unsigned)found[j]);
        return FAULT_PATCH_EEXPECT;
    }

    /* ---- gate 4: whole instructions out ---- */
    int nout = span_insns(found, req->len);
    if (nout == 0) {
        ap(why, whycap, &off,
           "the %u byte(s) at 0x%lx are not a whole number of instructions, so "
           "replacing them would leave the tail of one to be executed as an "
           "opcode. Pick a span that starts and ends on an instruction",
           (unsigned)req->len, (unsigned long)req->addr);
        return FAULT_PATCH_EBOUNDARY;
    }

    /* ---- gate 5: whole instructions in ---- */
    int nin = span_insns(req->bytes, req->len);
    if (nin == 0) {
        ap(why, whycap, &off,
           "the replacement bytes do not decode to exactly %u byte(s) of whole "
           "instructions. 0x90 is a one-byte nop and pads any gap",
           (unsigned)req->len);
        return FAULT_PATCH_EOPAQUE;
    }

    /* ---- gate 6: not on top of a live patch ---- */
    for (uint32_t i = 0; i < g_patches && i < FAULT_PATCH_SLOTS; i++) {
        const fault_patch_t *p = &g_patch[i];
        if (!p->live) continue;
        if (req->addr + req->len <= p->addr) continue;
        if (p->addr + p->len <= req->addr)   continue;
        ap(why, whycap, &off,
           "patch %lu is still live over [0x%lx, 0x%lx); revert it first",
           (unsigned long)p->id, (unsigned long)p->addr,
           (unsigned long)(p->addr + p->len));
        return FAULT_PATCH_EOVERLAP;
    }

    if (out_insns_out) *out_insns_out = nout;
    if (out_insns_in)  *out_insns_in  = nin;
    return FAULT_PATCH_OK;
}

fault_patch_err_t fault_patch_check(const fault_patch_req_t *req,
                                    const fault_window_t *window,
                                    char *why, size_t whycap) {
    uint8_t found[FAULT_PATCH_MAX_BYTES];
    return patch_gate(req, window, found, NULL, NULL, why, whycap);
}

fault_patch_err_t fault_patch_apply(const fault_patch_req_t *req,
                                    const fault_window_t *window,
                                    uint64_t now_ms, uint32_t *out_id,
                                    char *why, size_t whycap) {
    uint8_t           found[FAULT_PATCH_MAX_BYTES];
    uint8_t           back[FAULT_PATCH_MAX_BYTES];
    int               nout = 0, nin = 0;
    fault_patch_err_t rc;
    size_t            off = 0;

    if (out_id) *out_id = 0;

    rc = patch_gate(req, window, found, &nout, &nin, why, whycap);
    if (rc != FAULT_PATCH_OK) return rc;

    if (g_patches >= FAULT_PATCH_SLOTS) {
        if (whycap) why[0] = '\0';
        ap(why, whycap, &off,
           "all %u patch slots have been used this boot",
           (unsigned)FAULT_PATCH_SLOTS);
        return FAULT_PATCH_ENOSPC;
    }

    /* THE ORIGINAL BYTES ARE RECORDED BEFORE THE FIRST ONE IS OVERWRITTEN.
     * Not after, not alongside: a write that half-succeeds must still leave a
     * complete record of what was there, or the patch is not reversible and the
     * fifth gate in the header is a lie. */
    fault_patch_t *p = &g_patch[g_patches];
    zero(p, sizeof *p);
    p->id         = g_patches + 1u;
    p->addr       = req->addr;
    p->len        = req->len;
    p->insns_out  = (uint8_t)nout;
    p->insns_in   = (uint8_t)nin;
    p->applied_ms = now_ms;
    for (unsigned i = 0; i < req->len; i++) {
        p->orig[i]  = found[i];
        p->bytes[i] = req->bytes[i];
    }
    note_copy(p->note, sizeof p->note, req->note);

    if (mem_ops()->write(req->addr, req->bytes, req->len, mem_ops()->ctx) != 0) {
        if (whycap) why[0] = '\0';
        ap(why, whycap, &off, "the write to 0x%lx failed",
           (unsigned long)req->addr);
        zero(p, sizeof *p);
        return FAULT_PATCH_EIO;
    }

    /* Read it back. On a machine with no memory protection this is the only
     * evidence that the address was writable RAM and not, say, a memory-mapped
     * register that ignored the store. */
    if (mem_ops()->read(req->addr, back, req->len, mem_ops()->ctx) != 0) {
        if (whycap) why[0] = '\0';
        ap(why, whycap, &off, "the patched bytes could not be read back");
        goto undo;
    }
    for (unsigned i = 0; i < req->len; i++) {
        if (back[i] == req->bytes[i]) continue;
        if (whycap) why[0] = '\0';
        ap(why, whycap, &off,
           "byte %u read back as 0x%02x, not the 0x%02x written - that address "
           "is not plain writable RAM",
           i, (unsigned)back[i], (unsigned)req->bytes[i]);
        goto undo;
    }

    p->live = 1;
    g_patches++;
    g_patch_live++;
    if (out_id) *out_id = p->id;
    if (whycap) why[0] = '\0';
    ap(why, whycap, &off,
       "%u byte(s) at 0x%lx replaced (%d instruction(s) -> %d); the original is "
       "kept and patch %lu can be reverted",
       (unsigned)req->len, (unsigned long)req->addr, nout, nin,
       (unsigned long)p->id);
    return FAULT_PATCH_OK;

undo:
    /* Best effort: put back what we recorded, then report the failure. The slot
     * is released because nothing durable happened. */
    mem_ops()->write(req->addr, p->orig, req->len, mem_ops()->ctx);
    zero(p, sizeof *p);
    return FAULT_PATCH_EIO;
}

static fault_patch_t *patch_slot_by_id(uint32_t id) {
    if (id == 0 || id > g_patches) return NULL;
    fault_patch_t *p = &g_patch[id - 1];
    return (p->id == id) ? p : NULL;
}

fault_patch_err_t fault_patch_revert(uint32_t id, uint64_t now_ms,
                                     char *why, size_t whycap) {
    uint8_t now[FAULT_PATCH_MAX_BYTES];
    uint8_t back[FAULT_PATCH_MAX_BYTES];
    size_t  off = 0;
    if (whycap) why[0] = '\0';

    fault_patch_t *p = patch_slot_by_id(id);
    if (!p) {
        ap(why, whycap, &off, "there is no patch %lu; %lu have been applied",
           (unsigned long)id, (unsigned long)g_patches);
        return FAULT_PATCH_ENOENT;
    }
    if (!p->live) {
        ap(why, whycap, &off, "patch %lu was already reverted",
           (unsigned long)id);
        return FAULT_PATCH_EGONE;
    }
    if (!mem_ops() || !mem_ops()->read || !mem_ops()->write) {
        ap(why, whycap, &off, "no memory accessor is bound");
        return FAULT_PATCH_ENOMEM;
    }
    if (mem_ops()->read(p->addr, now, p->len, mem_ops()->ctx) != 0) {
        ap(why, whycap, &off, "the bytes at 0x%lx could not be read",
           (unsigned long)p->addr);
        return FAULT_PATCH_ENOMEM;
    }
    /* Refuse to "restore" over somebody else's edit. Reverting blindly would
     * make this the second writer to lose, silently, and there is no way to tell
     * afterwards which generation the bytes belong to. */
    for (unsigned i = 0; i < p->len; i++) {
        if (now[i] == p->bytes[i]) continue;
        ap(why, whycap, &off,
           "byte %u at 0x%lx is 0x%02x, not the 0x%02x patch %lu wrote - "
           "something has edited it since, and restoring the original would "
           "destroy that instead",
           i, (unsigned long)(p->addr + i), (unsigned)now[i],
           (unsigned)p->bytes[i], (unsigned long)id);
        return FAULT_PATCH_ECHANGED;
    }

    if (mem_ops()->write(p->addr, p->orig, p->len, mem_ops()->ctx) != 0) {
        ap(why, whycap, &off, "the write to 0x%lx failed",
           (unsigned long)p->addr);
        return FAULT_PATCH_EIO;
    }
    if (mem_ops()->read(p->addr, back, p->len, mem_ops()->ctx) != 0) {
        ap(why, whycap, &off, "the restored bytes could not be read back");
        return FAULT_PATCH_EIO;
    }
    for (unsigned i = 0; i < p->len; i++) {
        if (back[i] == p->orig[i]) continue;
        ap(why, whycap, &off,
           "byte %u read back as 0x%02x, not the original 0x%02x", i,
           (unsigned)back[i], (unsigned)p->orig[i]);
        return FAULT_PATCH_EIO;
    }

    p->live        = 0;
    p->reverted_ms = now_ms;
    if (g_patch_live) g_patch_live--;
    ap(why, whycap, &off,
       "patch %lu reverted: the original %u byte(s) are back at 0x%lx",
       (unsigned long)id, (unsigned)p->len, (unsigned long)p->addr);
    return FAULT_PATCH_OK;
}

uint32_t fault_patch_revert_all(uint64_t now_ms) {
    uint32_t n = 0;
    /* Newest first: overlapping generations cannot exist while both are live
     * (gate 6), but a slot reverted and re-patched can, and the last edit must
     * be the first undone. */
    for (uint32_t i = g_patches; i > 0; i--)
        if (g_patch[i - 1].live &&
            fault_patch_revert(g_patch[i - 1].id, now_ms, NULL, 0)
                == FAULT_PATCH_OK)
            n++;
    return n;
}

uint32_t fault_patch_count(void) { return g_patches; }
uint32_t fault_patch_live(void)  { return g_patch_live; }

const fault_patch_t *fault_patch_at(uint32_t index) {
    if (index >= g_patches || index >= FAULT_PATCH_SLOTS) return NULL;
    return &g_patch[index];
}

const fault_patch_t *fault_patch_by_id(uint32_t id) {
    return patch_slot_by_id(id);
}

const fault_patch_t *fault_patch_covering(uint64_t addr) {
    for (uint32_t i = 0; i < g_patches && i < FAULT_PATCH_SLOTS; i++) {
        const fault_patch_t *p = &g_patch[i];
        if (!p->live) continue;
        if (addr >= p->addr && addr < p->addr + p->len) return p;
    }
    return NULL;
}

void fault_patch_forget_all(void) {
    zero(g_patch, sizeof g_patch);
    g_patches    = 0;
    g_patch_live = 0;
}
