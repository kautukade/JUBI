/* dvm.c — the driver VM. See include/dvm.h for the contract and the ISA.
 *
 * LAYOUT OF THIS FILE
 *   1. tiny local string helpers      (no kernel.h: see the note below)
 *   2. the opcode table               (mnemonics, aliases, operand shapes)
 *   3. the assembler                  (two passes over the source text)
 *   4. the validator + disassembler
 *   5. policy: allowlists, the compiled-in deny list, validation
 *   6. the interpreter                (bounds, trace, traps)
 *   7. the hardware I/O backend       (kernel builds only)
 *
 * WHY NO kernel.h
 *   This file is compiled twice: freestanding for the kernel, and natively for
 *   tests/host/test_dvm.c. kernel.h declares memcpy/memset/strlen, which collide
 *   with Apple's fortifying macros in a host TU. The VM needs so little string
 *   handling that carrying its own four-line helpers is cheaper than the
 *   conditional-compilation dance, and it keeps the module free of dependencies
 *   that would stop it running on the host.
 *
 * WHY NO ALLOCATION
 *   Nothing here calls kmalloc. The caller owns the dvm_program_t, the policy
 *   and the result; the interpreter's entire mutable state is one stack frame.
 *   A driver bring-up is exactly the moment when you do not want the allocator
 *   involved, and it makes the module callable from anywhere.
 */

#include "dvm.h"
#include "trace.h"

#include <stdarg.h>
#include <stdio.h>      /* snprintf/vsnprintf; port/stdio.h when freestanding */

/* Kernel errno used for the "-> EINVAL" tail of a trap's trace line. The trace
 * table (lib/trace.c) renders unknown codes as a bare integer, so reuse the
 * canonical one and put the precise, symbolic reason in the argument field. */
#define DVM_TRACE_ERR (-22)     /* VFS_EINVAL */

/* Ceilings on the caller's budgets. A policy above these is refused rather than
 * silently clamped, so "how long can this run" is never a surprise. */
#define CEIL_STEPS        5000000u
#define CEIL_IO           1000000u
#define CEIL_DELAY_US     2000000u    /* 2 s of cumulative delay */
#define CEIL_SINGLE_US    1000000u
#define CEIL_PRINTS       4096u
#define CEIL_TRACE        100000u
/* One access per byte of the arena is the most a program can usefully make: a
 * byte-at-a-time fill of the whole buffer. Anything above that is a loop bug, so
 * it is the ceiling. The default (dvm_policy_init) is half of it, which still
 * covers writing the entire buffer 16 bits at a time. */
#define CEIL_DMA_OPS      DVM_DMA_SIZE
/* Bytes of scratch-memory and string work. 64 MiB is a thousand passes over the
 * whole arena: far past anything a text-shaped program does, and still a bound
 * a polled kernel notices (memcpy at a few GB/s spends tens of milliseconds
 * there, not seconds). The default is a sixteenth of it. */
#define CEIL_MEM_BYTES    0x4000000ull
#define CEIL_SYS          4096u

/* Only the low 4 GiB is mapped (boot.asm maps 2 MiB huge pages up to 4 GiB), so
 * an MMIO access above this would fault instead of being refused. */
#define MMIO_TOP          0x100000000ull

/* ====================================================================== */
/* 1. tiny local helpers                                                  */
/* ====================================================================== */

static void dz(void *p, size_t n) {                 /* zero fill */
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static char dlower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive compare of a counted token against a NUL-terminated word. */
static int tok_ieq(const char *t, int tlen, const char *word) {
    int i = 0;
    for (; i < tlen; i++) {
        if (!word[i]) return 0;
        if (dlower(t[i]) != dlower(word[i])) return 0;
    }
    return word[i] == '\0';
}

/* Bounded string builder used by the disassembler and every message. */
typedef struct { char *p; size_t cap; size_t len; } sb_t;

static void sb_init(sb_t *s, char *buf, size_t cap) {
    s->p = buf; s->cap = cap; s->len = 0;
    if (cap) buf[0] = '\0';
}

static void sb_addf(sb_t *s, const char *fmt, ...) {
    if (!s->cap || s->len + 1 >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->p + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    s->len += (size_t)n;
    if (s->len >= s->cap) s->len = s->cap - 1;
}

/* Copy model-authored text into a kernel-owned buffer, replacing anything that
 * is not printable ASCII. Trace lines get escaped by trace.c anyway; this is for
 * dvm_result_t.msg, which callers hand to the model and may print elsewhere. */
static void copy_printable(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    if (!cap) return;
    for (size_t i = 0; src && src[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    dst[o] = '\0';
}

/* ====================================================================== */
/* 2. the opcode table                                                    */
/* ====================================================================== */

typedef enum {
    F_NONE,          /* halt, ret, nop                          */
    F_STR,           /* abort "text"                            */
    F_STR_OPT_A,     /* print "text" [, a]                      */
    F_RD_A,          /* mov rd, a ; not rd, a                   */
    F_RD_A_B,        /* add rd, a, b  (2-operand form allowed)  */
    F_A_B,           /* cmp a, b                                */
    F_A,             /* push a ; delay a                        */
    F_RD,            /* pop rd                                  */
    F_LBL,           /* jmp/branch/call L                       */
    F_PORT_A,        /* out8 port, a                            */
    F_RD_PORT,       /* in8 rd, port                            */
    F_RD_ADDR,       /* ld8 rd, [base+disp] ; mld8 rd, [off]    */
    F_ADDR_A,        /* st8 [base+disp], a  ; mst8 [off], a     */
    F_RD_BDF_OFF,    /* pcicfg rd, bdf, off                     */
    /* The string family. A string literal always lands in operand slot 0,
     * whatever order it was written in, because dvm_program_validate() enforces
     * "a string may only be operand 0" structurally — keeping that rule true is
     * worth more than matching the source order in the encoding. */
    F_RD_A_B_C,      /* mcpy rd, dst, src, len                  */
    F_A_B_C,         /* mcmp a, b, len                          */
    F_RD_STR_A,      /* mstr rd, dst, "text"                    */
    F_RD_STR_A_B,    /* mfind rd, hay, len, "text"              */
    F_SYS            /* sys rd, name                            */
} fmt_t;

static const struct { const char *name; uint8_t fmt; } op_info[DVM_OP__COUNT] = {
    [DVM_HALT]   = { "halt",   F_NONE       },
    [DVM_ABORT]  = { "abort",  F_STR        },
    [DVM_NOP]    = { "nop",    F_NONE       },
    [DVM_MOV]    = { "mov",    F_RD_A       },
    [DVM_ADD]    = { "add",    F_RD_A_B     },
    [DVM_SUB]    = { "sub",    F_RD_A_B     },
    [DVM_MUL]    = { "mul",    F_RD_A_B     },
    [DVM_DIV]    = { "div",    F_RD_A_B     },
    [DVM_MOD]    = { "mod",    F_RD_A_B     },
    [DVM_AND]    = { "and",    F_RD_A_B     },
    [DVM_OR]     = { "or",     F_RD_A_B     },
    [DVM_XOR]    = { "xor",    F_RD_A_B     },
    [DVM_NOT]    = { "not",    F_RD_A       },
    [DVM_SHL]    = { "shl",    F_RD_A_B     },
    [DVM_SHR]    = { "shr",    F_RD_A_B     },
    [DVM_CMP]    = { "cmp",    F_A_B        },
    [DVM_TEST]   = { "test",   F_A_B        },
    [DVM_JMP]    = { "jmp",    F_LBL        },
    [DVM_BEQ]    = { "beq",    F_LBL        },
    [DVM_BNE]    = { "bne",    F_LBL        },
    [DVM_BLT]    = { "blt",    F_LBL        },
    [DVM_BLE]    = { "ble",    F_LBL        },
    [DVM_BGT]    = { "bgt",    F_LBL        },
    [DVM_BGE]    = { "bge",    F_LBL        },
    [DVM_CALL]   = { "call",   F_LBL        },
    [DVM_RET]    = { "ret",    F_NONE       },
    [DVM_PUSH]   = { "push",   F_A          },
    [DVM_POP]    = { "pop",    F_RD         },
    [DVM_OUT8]   = { "out8",   F_PORT_A     },
    [DVM_OUT16]  = { "out16",  F_PORT_A     },
    [DVM_OUT32]  = { "out32",  F_PORT_A     },
    [DVM_IN8]    = { "in8",    F_RD_PORT    },
    [DVM_IN16]   = { "in16",   F_RD_PORT    },
    [DVM_IN32]   = { "in32",   F_RD_PORT    },
    [DVM_LD8]    = { "ld8",    F_RD_ADDR    },
    [DVM_LD16]   = { "ld16",   F_RD_ADDR    },
    [DVM_LD32]   = { "ld32",   F_RD_ADDR    },
    [DVM_ST8]    = { "st8",    F_ADDR_A     },
    [DVM_ST16]   = { "st16",   F_ADDR_A     },
    [DVM_ST32]   = { "st32",   F_ADDR_A     },
    [DVM_PCICFG] = { "pcicfg", F_RD_BDF_OFF },
    [DVM_DELAY]  = { "delay",  F_A          },
    [DVM_PRINT]  = { "print",  F_STR_OPT_A  },
    [DVM_MLD8]   = { "mld8",   F_RD_ADDR    },
    [DVM_MLD16]  = { "mld16",  F_RD_ADDR    },
    [DVM_MLD32]  = { "mld32",  F_RD_ADDR    },
    [DVM_MLD64]  = { "mld64",  F_RD_ADDR    },
    [DVM_MST8]   = { "mst8",   F_ADDR_A     },
    [DVM_MST16]  = { "mst16",  F_ADDR_A     },
    [DVM_MST32]  = { "mst32",  F_ADDR_A     },
    [DVM_MST64]  = { "mst64",  F_ADDR_A     },
    [DVM_MSTR]   = { "mstr",   F_RD_STR_A   },
    [DVM_MCPY]   = { "mcpy",   F_RD_A_B_C   },
    [DVM_MSET]   = { "mset",   F_RD_A_B_C   },
    [DVM_MCMP]   = { "mcmp",   F_A_B_C      },
    [DVM_MFIND]  = { "mfind",  F_RD_STR_A_B },
    [DVM_MCHR]   = { "mchr",   F_RD_A_B_C   },
    [DVM_MATOI]  = { "matoi",  F_RD_A_B_C   },
    [DVM_MITOA]  = { "mitoa",  F_RD_A_B_C   },
    [DVM_SYS]    = { "sys",    F_SYS        },
};

const char *dvm_op_name(dvm_op_t op) {
    if ((unsigned)op >= DVM_OP__COUNT || !op_info[op].name) return "?";
    return op_info[op].name;
}

/* Mnemonics the assembler accepts, canonical name first. The aliases exist
 * because the model writes this text from memory of other assemblers, and a
 * rejected program costs a whole turn. */
static const struct { const char *m; uint8_t op; } mnemonic[] = {
    { "halt", DVM_HALT }, { "hlt", DVM_HALT }, { "stop", DVM_HALT },
    { "end",  DVM_HALT },
    { "abort", DVM_ABORT }, { "fail", DVM_ABORT },
    { "nop", DVM_NOP },
    { "mov", DVM_MOV }, { "li", DVM_MOV }, { "set", DVM_MOV },
    { "add", DVM_ADD }, { "sub", DVM_SUB }, { "mul", DVM_MUL },
    { "div", DVM_DIV }, { "mod", DVM_MOD }, { "rem", DVM_MOD },
    { "and", DVM_AND }, { "or", DVM_OR }, { "xor", DVM_XOR },
    { "not", DVM_NOT },
    { "shl", DVM_SHL }, { "lsl", DVM_SHL },
    { "shr", DVM_SHR }, { "lsr", DVM_SHR },
    { "cmp", DVM_CMP }, { "test", DVM_TEST }, { "tst", DVM_TEST },
    { "jmp", DVM_JMP }, { "j", DVM_JMP }, { "b", DVM_JMP },
    { "beq", DVM_BEQ }, { "je", DVM_BEQ }, { "jz", DVM_BEQ }, { "bz", DVM_BEQ },
    { "bne", DVM_BNE }, { "jne", DVM_BNE }, { "jnz", DVM_BNE }, { "bnz", DVM_BNE },
    { "blt", DVM_BLT }, { "jb", DVM_BLT }, { "jl", DVM_BLT },
    { "ble", DVM_BLE }, { "jbe", DVM_BLE }, { "jle", DVM_BLE },
    { "bgt", DVM_BGT }, { "ja", DVM_BGT }, { "jg", DVM_BGT },
    { "bge", DVM_BGE }, { "jae", DVM_BGE }, { "jge", DVM_BGE },
    { "call", DVM_CALL }, { "ret", DVM_RET },
    { "push", DVM_PUSH }, { "pop", DVM_POP },
    { "out8", DVM_OUT8 }, { "outb", DVM_OUT8 },
    { "out16", DVM_OUT16 }, { "outw", DVM_OUT16 },
    { "out32", DVM_OUT32 }, { "outl", DVM_OUT32 }, { "outd", DVM_OUT32 },
    { "in8", DVM_IN8 }, { "inb", DVM_IN8 },
    { "in16", DVM_IN16 }, { "inw", DVM_IN16 },
    { "in32", DVM_IN32 }, { "inl", DVM_IN32 }, { "ind", DVM_IN32 },
    { "ld8", DVM_LD8 }, { "mr8", DVM_LD8 },
    { "ld16", DVM_LD16 }, { "mr16", DVM_LD16 },
    { "ld32", DVM_LD32 }, { "mr32", DVM_LD32 },
    { "st8", DVM_ST8 }, { "mw8", DVM_ST8 },
    { "st16", DVM_ST16 }, { "mw16", DVM_ST16 },
    { "st32", DVM_ST32 }, { "mw32", DVM_ST32 },
    { "pcicfg", DVM_PCICFG }, { "pciread", DVM_PCICFG }, { "pcird", DVM_PCICFG },
    { "delay", DVM_DELAY }, { "udelay", DVM_DELAY }, { "usleep", DVM_DELAY },
    { "print", DVM_PRINT }, { "log", DVM_PRINT },
    { "mld8", DVM_MLD8 }, { "mld16", DVM_MLD16 },
    { "mld32", DVM_MLD32 }, { "mld64", DVM_MLD64 },
    { "mst8", DVM_MST8 }, { "mst16", DVM_MST16 },
    { "mst32", DVM_MST32 }, { "mst64", DVM_MST64 },
    { "mstr", DVM_MSTR }, { "mcpy", DVM_MCPY }, { "mmove", DVM_MCPY },
    { "mset", DVM_MSET }, { "mfill", DVM_MSET },
    { "mcmp", DVM_MCMP }, { "mfind", DVM_MFIND }, { "mchr", DVM_MCHR },
    { "matoi", DVM_MATOI }, { "atoi", DVM_MATOI },
    { "mitoa", DVM_MITOA }, { "itoa", DVM_MITOA },
    { "sys", DVM_SYS }, { "syscall", DVM_SYS },
};
#define NMNEMONIC ((int)(sizeof mnemonic / sizeof mnemonic[0]))

const char *dvm_status_name(dvm_status_t s) {
    switch (s) {
        case DVM_OK:                return "OK";
        case DVM_TRAP_ABORT:        return "ABORT";
        case DVM_TRAP_STEPS:        return "STEP_LIMIT";
        case DVM_TRAP_IO_BUDGET:    return "IO_LIMIT";
        case DVM_TRAP_DELAY_BUDGET: return "DELAY_LIMIT";
        case DVM_TRAP_PRINT_BUDGET: return "PRINT_LIMIT";
        case DVM_TRAP_PORT_DENIED:  return "PORT_DENIED";
        case DVM_TRAP_MMIO_DENIED:  return "MMIO_DENIED";
        case DVM_TRAP_PCI_DENIED:   return "PCI_DENIED";
        case DVM_TRAP_ALIGN:        return "MISALIGNED";
        case DVM_TRAP_RANGE:        return "OPERAND_RANGE";
        case DVM_TRAP_PC:           return "BAD_PC";
        case DVM_TRAP_STACK:        return "STACK";
        case DVM_TRAP_DIV0:         return "DIVIDE_BY_ZERO";
        case DVM_TRAP_BADOP:        return "BAD_PROGRAM";
        case DVM_TRAP_NOIO:         return "NO_BACKEND";
        case DVM_TRAP_POLICY:       return "BAD_POLICY";
        case DVM_TRAP_DMA_BUDGET:   return "DMA_LIMIT";
        case DVM_TRAP_MEM:          return "MEM_RANGE";
        case DVM_TRAP_MEM_BUDGET:   return "MEM_LIMIT";
        case DVM_TRAP_SYS_DENIED:   return "SYS_DENIED";
        case DVM_TRAP_SYS_BUDGET:   return "SYS_LIMIT";
        case DVM_TRAP_REENTRY:      return "REENTRY";
        default:                    return "?";
    }
}

/* ---- the syscall table ----
 *
 * One row per hole. The arity is what `sys` pops off the data stack, and it
 * lives here rather than in the interpreter so that the name a program writes,
 * the number the policy allows and the number of arguments popped cannot drift
 * apart. */
static const struct { const char *name; int8_t arity; } sys_info[DVM_SYS__COUNT] = {
    [DVM_SYS_CON_WRITE]  = { "con.write",  2 },
    [DVM_SYS_FS_READ]    = { "fs.read",    4 },
    [DVM_SYS_FS_WRITE]   = { "fs.write",   4 },
    [DVM_SYS_FS_SIZE]    = { "fs.size",    2 },
    [DVM_SYS_AUDIO_TONE] = { "audio.tone", 2 },
    [DVM_SYS_TIME_MS]    = { "time.ms",    0 },
};

const char *dvm_sys_name(dvm_sys_nr_t nr) {
    if ((unsigned)nr >= DVM_SYS__COUNT || !sys_info[nr].name) return "?";
    return sys_info[nr].name;
}

int dvm_sys_arity(dvm_sys_nr_t nr) {
    if ((unsigned)nr >= DVM_SYS__COUNT || !sys_info[nr].name) return -1;
    return sys_info[nr].arity;
}

/* A syscall is 0 or 1 fs paths' worth of trust; the mask has to hold one bit
 * per number and nothing here may quietly outgrow it. */
_Static_assert(DVM_SYS__COUNT <= 32, "sys_allow is a uint32_t bitmask");
/* The interpreter pops into a fixed four-slot array; a fifth argument needs
 * that array widened, not a syscall row that quietly overruns it. */
#define DVM_SYS_MAXARGS 4

/* ====================================================================== */
/* 3. the assembler                                                       */
/* ====================================================================== */

typedef struct { const char *s; int len; } tok_t;
#define MAXTOK 8

typedef struct {
    dvm_program_t *p;
    dvm_asm_err_t *err;
    int            failed;
    int            line;                       /* 1-based, current            */
    struct { char name[DVM_LABEL_MAX]; uint64_t val; } equ[DVM_MAX_EQU];
    int            nequ;
} as_t;

/* Record the first error and stop; later errors are almost always fallout. */
static int aerr(as_t *as, const char *fmt, ...) {
    if (as->failed) return -1;
    as->failed = 1;
    if (as->err) {
        as->err->line = as->line;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(as->err->msg, sizeof as->err->msg, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ---- name classification ---- */

static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
}
static int is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* "r7" / "R7" -> 7, "r16" -> -2 (a register that does not exist), else -1. */
static int reg_index(const char *t, int len) {
    if (len < 2 || (t[0] != 'r' && t[0] != 'R')) return -1;
    int v = 0;
    for (int i = 1; i < len; i++) {
        if (t[i] < '0' || t[i] > '9') return -1;
        v = v * 10 + (t[i] - '0');
        if (v > 999) return -2;
    }
    return v < DVM_NREGS ? v : -2;
}

/* Parse a numeric literal: 0x.. 0b.. decimal, optional leading '-' (two's
 * complement). Returns 0 on success. Overflow is an error, not a wrap. */
static int parse_num(const char *t, int len, uint64_t *out) {
    int i = 0, neg = 0;
    if (len <= 0) return -1;
    if (t[i] == '-' || t[i] == '+') { neg = (t[i] == '-'); i++; }
    if (i >= len) return -1;

    unsigned base = 10;
    if (len - i > 2 && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) { base = 16; i += 2; }
    else if (len - i > 2 && t[i] == '0' && (t[i + 1] == 'b' || t[i + 1] == 'B')) { base = 2; i += 2; }

    uint64_t v = 0;
    int digits = 0;
    for (; i < len; i++) {
        char c = dlower(t[i]);
        unsigned d;
        if (c == '_') continue;                       /* 0xdead_beef */
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else return -1;
        if (d >= base) return -1;
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / base) return -2;   /* overflow */
        v = v * base + d;
        digits++;
    }
    if (!digits) return -1;
    *out = neg ? (uint64_t)(-(int64_t)v) : v;
    return 0;
}

/* "00:03.0" -> bdf ((bus<<8)|(dev<<3)|fn), the way pci_list prints an address. */
static int parse_bdf(const char *t, int len, uint64_t *out) {
    int i = 0, v, n;
    unsigned bus, dev, fn;

    for (n = 0, v = 0; i < len && n < 2; n++) {
        char c = dlower(t[i]);
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d; i++;
    }
    if (!n || i >= len || t[i] != ':') return -1;
    bus = (unsigned)v; i++;

    for (n = 0, v = 0; i < len && n < 2; n++) {
        char c = dlower(t[i]);
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d; i++;
    }
    if (!n || i >= len || t[i] != '.' || v > 31) return -1;
    dev = (unsigned)v; i++;

    if (i >= len) return -1;
    if (t[i] < '0' || t[i] > '7') return -1;
    fn = (unsigned)(t[i] - '0'); i++;
    if (i != len) return -1;

    *out = ((uint64_t)bus << 8) | ((uint64_t)dev << 3) | fn;
    return 0;
}

static int equ_lookup(const as_t *as, const char *t, int len, uint64_t *out) {
    for (int i = 0; i < as->nequ; i++)
        if (tok_ieq(t, len, as->equ[i].name)) { *out = as->equ[i].val; return 1; }
    return 0;
}

static int label_lookup(const dvm_program_t *p, const char *t, int len, uint32_t *pc) {
    for (uint32_t i = 0; i < p->nlabel; i++)
        if (tok_ieq(t, len, p->label[i].name)) { *pc = p->label[i].pc; return 1; }
    return 0;
}

/* ---- tokenising one line ---- */

/* Strip a trailing comment (';', '#', or '//') that is not inside a string.
 * The escape state is tracked properly rather than by peeking one character
 * back, so a string ending in a literal backslash ("a\\") does not swallow the
 * rest of the line. */
static void strip_comment(char *s) {
    int q = 0, esc = 0;
    for (int i = 0; s[i]; i++) {
        if (q) {
            if (esc)                esc = 0;
            else if (s[i] == '\\')  esc = 1;
            else if (s[i] == '"')   q = 0;
            continue;
        }
        if (s[i] == '"') { q = 1; continue; }
        if (s[i] == ';' || s[i] == '#' || (s[i] == '/' && s[i + 1] == '/')) {
            s[i] = '\0';
            return;
        }
    }
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == ','; }

/* Split into tokens. A quoted string and a [bracketed] address each stay whole.
 * Returns the token count, or -1 on an unterminated quote/bracket. */
static int tokenise(as_t *as, char *s, tok_t *out, int max) {
    int n = 0;
    for (int i = 0; s[i];) {
        if (is_space(s[i])) { i++; continue; }
        if (n >= max) return aerr(as, "too many operands (at most %d per instruction)", max - 1);

        if (s[i] == '"') {
            int j = i + 1, esc = 0;
            while (s[j]) {
                if (esc)               esc = 0;
                else if (s[j] == '\\') esc = 1;
                else if (s[j] == '"')  break;
                j++;
            }
            if (s[j] != '"') return aerr(as, "unterminated string literal");
            out[n].s = &s[i]; out[n].len = j - i + 1; n++;
            i = j + 1;
        } else if (s[i] == '[') {
            int j = i + 1;
            while (s[j] && s[j] != ']') j++;
            if (s[j] != ']') return aerr(as, "unterminated '[' - write an address as [r1+0x10]");
            out[n].s = &s[i]; out[n].len = j - i + 1; n++;
            i = j + 1;
        } else {
            int j = i;
            while (s[j] && !is_space(s[j]) && s[j] != '"' && s[j] != '[') j++;
            out[n].s = &s[i]; out[n].len = j - i; n++;
            i = j;
        }
    }
    return n;
}

/* ---- operand parsing ---- */

/* A register, a number, a .equ constant, or a bdf literal. Never a label. */
static int operand(as_t *as, const char *what, tok_t t, uint8_t *kind, uint64_t *val) {
    int r = reg_index(t.s, t.len);
    if (r >= 0) { *kind = DVM_O_REG; *val = (uint64_t)r; return 0; }
    if (r == -2)
        return aerr(as, "%s: register \"%.*s\" does not exist (this machine has r0-r%d)",
                    what, t.len, t.s, DVM_NREGS - 1);

    uint64_t v;
    int rc = parse_num(t.s, t.len, &v);
    if (rc == 0)  { *kind = DVM_O_IMM; *val = v; return 0; }
    if (rc == -2) return aerr(as, "%s: number \"%.*s\" does not fit in 64 bits", what, t.len, t.s);

    if (equ_lookup(as, t.s, t.len, &v)) { *kind = DVM_O_IMM; *val = v; return 0; }

    return aerr(as, "%s: expected a register (r0-r%d), a number, or a .equ name, got \"%.*s\"",
                what, DVM_NREGS - 1, t.len, t.s);
}

/* An address operand: "[base+disp]", "[base]", or a bare base. Fills two
 * operand slots. `have_disp` says whether a following bare token supplied the
 * displacement (only consulted for the unbracketed form). */
static int addr_operand(as_t *as, tok_t t, uint8_t *kind, uint64_t *val) {
    kind[0] = DVM_O_IMM; val[0] = 0;
    kind[1] = DVM_O_IMM; val[1] = 0;

    if (t.len >= 2 && t.s[0] == '[') {
        tok_t in = { t.s + 1, t.len - 2 };
        /* trim */
        while (in.len && is_space(in.s[0])) { in.s++; in.len--; }
        while (in.len && is_space(in.s[in.len - 1])) in.len--;
        if (!in.len) return aerr(as, "empty address []");

        int plus = -1;
        for (int i = 0; i < in.len; i++) if (in.s[i] == '+') { plus = i; break; }
        if (plus < 0) return operand(as, "address", in, &kind[0], &val[0]);

        tok_t b = { in.s, plus }, d = { in.s + plus + 1, in.len - plus - 1 };
        while (b.len && is_space(b.s[b.len - 1])) b.len--;
        while (d.len && is_space(d.s[0])) { d.s++; d.len--; }
        if (!b.len || !d.len)
            return aerr(as, "malformed address \"%.*s\"; expected [base+displacement]",
                        t.len, t.s);
        if (operand(as, "address base", b, &kind[0], &val[0]) != 0) return -1;
        return operand(as, "address displacement", d, &kind[1], &val[1]);
    }
    return operand(as, "address", t, &kind[0], &val[0]);
}

/* ---- string literals ---- */

/* Decode "..." (with \\ \" \n \t \r \0 escapes) into the program's pool.
 * Returns the string index, or -1. */
static int intern_string(as_t *as, tok_t t) {
    dvm_program_t *p = as->p;
    if (t.len < 2 || t.s[0] != '"' || t.s[t.len - 1] != '"')
        return aerr(as, "expected a quoted string, got \"%.*s\"", t.len, t.s);
    if (p->nstr >= DVM_MAX_STRINGS)
        return aerr(as, "too many strings (max %d)", DVM_MAX_STRINGS);

    uint32_t start = p->strused;
    for (int i = 1; i < t.len - 1; i++) {
        char c = t.s[i];
        if (c == '\\' && i + 1 < t.len - 1) {
            i++;
            switch (t.s[i]) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': c = '\0'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                default:
                    return aerr(as, "unknown escape \"\\%c\" in a string literal", t.s[i]);
            }
        }
        if (p->strused + 1 >= DVM_STRPOOL)
            return aerr(as, "string pool full (%d bytes total across the program)",
                        DVM_STRPOOL);
        p->strpool[p->strused++] = c;
    }
    if (p->strused + 1 > DVM_STRPOOL)
        return aerr(as, "string pool full (%d bytes total across the program)", DVM_STRPOOL);
    p->strpool[p->strused++] = '\0';
    p->stroff[p->nstr] = (uint16_t)start;
    return (int)p->nstr++;
}

static const char *str_at(const dvm_program_t *p, uint64_t idx) {
    if (idx >= p->nstr) return "";
    uint16_t off = p->stroff[idx];
    if (off >= DVM_STRPOOL) return "";
    return &p->strpool[off];
}

/* ---- one instruction ---- */

/* Read a destination register operand, distinguishing "that is not a register"
 * from "that register does not exist on this machine". */
static int dst_reg(as_t *as, const char *mn, tok_t t, uint8_t *dst) {
    int r = reg_index(t.s, t.len);
    if (r >= 0) { *dst = (uint8_t)r; return 0; }
    if (r == -2)
        return aerr(as, "%s: register \"%.*s\" does not exist (this machine has r0-r%d)",
                    mn, t.len, t.s, DVM_NREGS - 1);
    return aerr(as, "%s: destination must be a register (r0-r%d), got \"%.*s\"",
                mn, DVM_NREGS - 1, t.len, t.s);
}

static int need_operands(as_t *as, const char *mn, int got, int want) {
    if (got == want) return 0;
    return aerr(as, "%s takes %d operand%s, got %d", mn, want, want == 1 ? "" : "s", got);
}

static int assemble_insn(as_t *as, uint8_t op, tok_t *t, int nt, const char *mn) {
    dvm_program_t *p = as->p;
    if (p->ninsn >= DVM_MAX_INSNS)
        return aerr(as, "program too long (max %d instructions)", DVM_MAX_INSNS);

    dvm_insn_t *ins = &p->insn[p->ninsn];
    dz(ins, sizeof *ins);
    ins->op   = op;
    ins->dst  = DVM_NOREG;
    ins->line = (uint32_t)as->line;

    int rd;
    switch (op_info[op].fmt) {
    case F_NONE:
        if (need_operands(as, mn, nt, 0) != 0) return -1;
        break;

    case F_STR:
        if (need_operands(as, mn, nt, 1) != 0) return -1;
        rd = intern_string(as, t[0]);
        if (rd < 0) return -1;
        ins->kind[0] = DVM_O_STR; ins->val[0] = (uint64_t)rd;
        break;

    case F_STR_OPT_A:
        if (nt < 1 || nt > 2)
            return aerr(as, "%s takes a string and an optional value, got %d operand%s",
                        mn, nt, nt == 1 ? "" : "s");
        rd = intern_string(as, t[0]);
        if (rd < 0) return -1;
        ins->kind[0] = DVM_O_STR; ins->val[0] = (uint64_t)rd;
        if (nt == 2 && operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;

    case F_RD_A:
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        if (operand(as, mn, t[1], &ins->kind[0], &ins->val[0]) != 0) return -1;
        break;

    case F_RD_A_B:
        if (nt < 2 || nt > 3)
            return aerr(as, "%s takes 2 or 3 operands (\"%s rd, a, b\" or the shorthand "
                            "\"%s rd, b\"), got %d", mn, mn, mn, nt);
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        if (nt == 2) {          /* "add r1, 4" == "add r1, r1, 4" */
            ins->kind[0] = DVM_O_REG; ins->val[0] = ins->dst;
            if (operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        } else {
            if (operand(as, mn, t[1], &ins->kind[0], &ins->val[0]) != 0) return -1;
            if (operand(as, mn, t[2], &ins->kind[1], &ins->val[1]) != 0) return -1;
        }
        break;

    case F_A_B:
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        if (operand(as, mn, t[0], &ins->kind[0], &ins->val[0]) != 0) return -1;
        if (operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;

    case F_A:
        if (need_operands(as, mn, nt, 1) != 0) return -1;
        if (operand(as, mn, t[0], &ins->kind[0], &ins->val[0]) != 0) return -1;
        break;

    case F_RD:
        if (need_operands(as, mn, nt, 1) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        break;

    case F_LBL: {
        if (need_operands(as, mn, nt, 1) != 0) return -1;
        uint32_t target;
        if (!label_lookup(p, t[0].s, t[0].len, &target))
            return aerr(as, "%s: no label named \"%.*s\" in this program", mn, t[0].len, t[0].s);
        ins->kind[0] = DVM_O_PC; ins->val[0] = target;
        break;
    }

    case F_PORT_A: {
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        tok_t pt = t[0];
        if (pt.len >= 2 && pt.s[0] == '[') { pt.s++; pt.len -= 2; }   /* out8 [0x61], 1 */
        if (operand(as, "port", pt, &ins->kind[0], &ins->val[0]) != 0) return -1;
        if (operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;
    }

    case F_RD_PORT: {
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        tok_t pt = t[1];
        if (pt.len >= 2 && pt.s[0] == '[') { pt.s++; pt.len -= 2; }
        if (operand(as, "port", pt, &ins->kind[0], &ins->val[0]) != 0) return -1;
        break;
    }

    case F_RD_ADDR:
        if (nt < 2 || nt > 3)
            return aerr(as, "%s takes a destination register and an address "
                            "(\"%s rd, [base+disp]\" or \"%s rd, base, disp\"), got %d operands",
                        mn, mn, mn, nt);
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        if (nt == 2) {
            if (addr_operand(as, t[1], ins->kind, ins->val) != 0) return -1;
        } else {
            if (t[1].len >= 2 && t[1].s[0] == '[')
                return aerr(as, "%s: give either [base+disp] or two plain operands, not both", mn);
            if (operand(as, "address base", t[1], &ins->kind[0], &ins->val[0]) != 0) return -1;
            if (operand(as, "address displacement", t[2], &ins->kind[1], &ins->val[1]) != 0) return -1;
        }
        break;

    case F_ADDR_A:
        if (nt < 2 || nt > 3)
            return aerr(as, "%s takes an address and a value (\"%s [base+disp], value\" or "
                            "\"%s base, disp, value\"), got %d operands", mn, mn, mn, nt);
        if (nt == 2) {
            if (addr_operand(as, t[0], ins->kind, ins->val) != 0) return -1;
            if (operand(as, "value", t[1], &ins->kind[2], &ins->val[2]) != 0) return -1;
        } else {
            if (t[0].len >= 2 && t[0].s[0] == '[')
                return aerr(as, "%s: give either [base+disp] or two plain operands, not both", mn);
            if (operand(as, "address base", t[0], &ins->kind[0], &ins->val[0]) != 0) return -1;
            if (operand(as, "address displacement", t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
            if (operand(as, "value", t[2], &ins->kind[2], &ins->val[2]) != 0) return -1;
        }
        break;

    case F_RD_A_B_C:
        if (need_operands(as, mn, nt, 4) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        for (int k = 0; k < 3; k++)
            if (operand(as, mn, t[k + 1], &ins->kind[k], &ins->val[k]) != 0) return -1;
        break;

    case F_A_B_C:
        if (need_operands(as, mn, nt, 3) != 0) return -1;
        for (int k = 0; k < 3; k++)
            if (operand(as, mn, t[k], &ins->kind[k], &ins->val[k]) != 0) return -1;
        break;

    case F_RD_STR_A:
        if (need_operands(as, mn, nt, 3) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        /* Source order is `mstr rd, dst, "text"`; the string is interned into
         * slot 0 and the offset into slot 1. See fmt_t. */
        rd = intern_string(as, t[2]);
        if (rd < 0) return -1;
        ins->kind[0] = DVM_O_STR; ins->val[0] = (uint64_t)rd;
        if (operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;

    case F_RD_STR_A_B:
        if (need_operands(as, mn, nt, 4) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        rd = intern_string(as, t[3]);
        if (rd < 0) return -1;
        ins->kind[0] = DVM_O_STR; ins->val[0] = (uint64_t)rd;
        if (operand(as, mn, t[1], &ins->kind[1], &ins->val[1]) != 0) return -1;
        if (operand(as, mn, t[2], &ins->kind[2], &ins->val[2]) != 0) return -1;
        break;

    case F_SYS: {
        if (need_operands(as, mn, nt, 2) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        /* A name first, so `sys r1, fs.read` reads as what it does. A bare
         * number still works, because a program generated from a table should
         * not have to know the spelling. */
        int nr = -1;
        for (int k = 0; k < DVM_SYS__COUNT; k++)
            if (sys_info[k].name && tok_ieq(t[1].s, t[1].len, sys_info[k].name)) {
                nr = k; break;
            }
        if (nr >= 0) {
            ins->kind[0] = DVM_O_IMM; ins->val[0] = (uint64_t)nr;
            break;
        }
        uint64_t v;
        if (parse_num(t[1].s, t[1].len, &v) == 0 && v < DVM_SYS__COUNT) {
            ins->kind[0] = DVM_O_IMM; ins->val[0] = v;
            break;
        }
        {
            char names[160];
            sb_t s; sb_init(&s, names, sizeof names);
            for (int k = 0; k < DVM_SYS__COUNT; k++)
                sb_addf(&s, "%s%s", k ? " " : "", dvm_sys_name((dvm_sys_nr_t)k));
            return aerr(as, "sys: \"%.*s\" is not a syscall on this machine; it has %s",
                        t[1].len, t[1].s, names);
        }
    }

    case F_RD_BDF_OFF: {
        if (need_operands(as, mn, nt, 3) != 0) return -1;
        if (dst_reg(as, mn, t[0], &ins->dst) != 0) return -1;
        uint64_t bdf;
        if (parse_bdf(t[1].s, t[1].len, &bdf) == 0) {
            ins->kind[0] = DVM_O_IMM; ins->val[0] = bdf;
        } else if (operand(as, "bdf", t[1], &ins->kind[0], &ins->val[0]) != 0) {
            return -1;
        }
        if (operand(as, "config offset", t[2], &ins->kind[1], &ins->val[1]) != 0) return -1;
        break;
    }
    }

    p->ninsn++;
    return 0;
}

/* ---- .equ ---- */

static int do_equ(as_t *as, tok_t *t, int nt) {
    if (nt != 2)
        return aerr(as, ".equ takes a name and a value, e.g. \".equ NAM_BAR 0x10\"");
    if (as->nequ >= DVM_MAX_EQU)
        return aerr(as, "too many .equ constants (max %d)", DVM_MAX_EQU);
    if (t[0].len <= 0 || t[0].len >= DVM_LABEL_MAX)
        return aerr(as, "constant name \"%.*s\" is too long (max %d characters)",
                    t[0].len, t[0].s, DVM_LABEL_MAX - 1);
    if (!is_ident_start(t[0].s[0]))
        return aerr(as, "constant name \"%.*s\" must start with a letter or '_'",
                    t[0].len, t[0].s);
    for (int i = 0; i < t[0].len; i++)
        if (!is_ident_char(t[0].s[i]))
            return aerr(as, "constant name \"%.*s\" contains an illegal character",
                        t[0].len, t[0].s);
    if (reg_index(t[0].s, t[0].len) != -1)
        return aerr(as, "\"%.*s\" is a register name and cannot be a constant",
                    t[0].len, t[0].s);
    uint64_t dummy;
    uint32_t dummypc;
    if (equ_lookup(as, t[0].s, t[0].len, &dummy))
        return aerr(as, "constant \"%.*s\" is already defined", t[0].len, t[0].s);
    if (label_lookup(as->p, t[0].s, t[0].len, &dummypc))
        return aerr(as, "\"%.*s\" is already a label", t[0].len, t[0].s);

    uint64_t v;
    int rc = parse_num(t[1].s, t[1].len, &v);
    if (rc == -2) return aerr(as, ".equ %.*s: number \"%.*s\" does not fit in 64 bits",
                              t[0].len, t[0].s, t[1].len, t[1].s);
    if (rc != 0 && !equ_lookup(as, t[1].s, t[1].len, &v))
        return aerr(as, ".equ %.*s: expected a number, got \"%.*s\"",
                    t[0].len, t[0].s, t[1].len, t[1].s);

    for (int i = 0; i < t[0].len; i++) as->equ[as->nequ].name[i] = t[0].s[i];
    as->equ[as->nequ].name[t[0].len] = '\0';
    as->equ[as->nequ].val = v;
    as->nequ++;
    return 0;
}

static int add_label(as_t *as, const char *name, int len) {
    dvm_program_t *p = as->p;
    if (len <= 0 || len >= DVM_LABEL_MAX)
        return aerr(as, "label \"%.*s\" is too long (max %d characters)",
                    len, name, DVM_LABEL_MAX - 1);
    if (!is_ident_start(name[0]))
        return aerr(as, "label \"%.*s\" must start with a letter, '_' or '.'", len, name);
    for (int i = 0; i < len; i++)
        if (!is_ident_char(name[i]))
            return aerr(as, "label \"%.*s\" contains an illegal character", len, name);
    if (reg_index(name, len) != -1)
        return aerr(as, "\"%.*s\" is a register name and cannot be a label", len, name);
    for (int i = 0; i < NMNEMONIC; i++)
        if (tok_ieq(name, len, mnemonic[i].m))
            return aerr(as, "\"%.*s\" is an instruction name and cannot be a label", len, name);
    uint32_t where;
    uint64_t dummy;
    if (label_lookup(p, name, len, &where))
        return aerr(as, "label \"%.*s\" is already defined", len, name);
    if (equ_lookup(as, name, len, &dummy))
        return aerr(as, "\"%.*s\" is already a .equ constant", len, name);
    if (p->nlabel >= DVM_MAX_LABELS)
        return aerr(as, "too many labels (max %d)", DVM_MAX_LABELS);

    for (int i = 0; i < len; i++) p->label[p->nlabel].name[i] = name[i];
    p->label[p->nlabel].name[len] = '\0';
    p->label[p->nlabel].pc = p->ninsn;
    p->nlabel++;
    return 0;
}

/* One pass over the source. pass 1 collects labels and constants (and counts
 * instructions so a forward branch resolves); pass 2 emits. */
static int assemble_pass(as_t *as, const char *src, size_t len, int pass) {
    size_t i = 0;
    as->p->ninsn = 0;
    as->line = 0;

    while (i <= len) {
        /* Extract one line. A final line without a newline still counts. */
        size_t start = i;
        while (i < len && src[i] != '\n') i++;
        size_t n = i - start;
        if (i < len) i++;                 /* consume the '\n' */
        else if (n == 0 && start >= len) break;
        else i = len + 1;                 /* last line, then stop */

        as->line++;
        if (n >= DVM_LINE_MAX)
            return aerr(as, "line is %lu characters; the limit is %d",
                        (unsigned long)n, DVM_LINE_MAX - 1);

        char buf[DVM_LINE_MAX];
        for (size_t k = 0; k < n; k++) buf[k] = src[start + k];
        buf[n] = '\0';

        /* An embedded NUL truncates the line rather than being an error: the
         * source arrives as a JSON string span and a stray NUL is a model
         * artefact, not an attack. Everything after it on that line is ignored. */
        strip_comment(buf);

        tok_t t[MAXTOK];
        int nt = tokenise(as, buf, t, MAXTOK);
        if (nt < 0) return -1;
        if (nt == 0) continue;

        /* label: */
        if (t[0].len > 1 && t[0].s[t[0].len - 1] == ':') {
            if (pass == 1 && add_label(as, t[0].s, t[0].len - 1) != 0) return -1;
            for (int k = 1; k < nt; k++) t[k - 1] = t[k];
            nt--;
            if (nt == 0) continue;
        }

        /* .directive */
        if (t[0].s[0] == '.') {
            if (tok_ieq(t[0].s, t[0].len, ".equ") || tok_ieq(t[0].s, t[0].len, ".def") ||
                tok_ieq(t[0].s, t[0].len, ".set")) {
                if (pass == 1 && do_equ(as, t + 1, nt - 1) != 0) return -1;
                continue;
            }
            return aerr(as, "unknown directive \"%.*s\" (only .equ is supported)",
                        t[0].len, t[0].s);
        }

        /* mnemonic */
        int op = -1;
        for (int k = 0; k < NMNEMONIC; k++)
            if (tok_ieq(t[0].s, t[0].len, mnemonic[k].m)) { op = mnemonic[k].op; break; }
        if (op < 0) {
            /* The overwhelmingly common near-miss is an access width this ISA
             * does not spell that way ("outq", "ldw"), so name the family. */
            static const struct { const char *pfx, *hint; } near[] = {
                { "ou", " (did you mean out8, out16 or out32?)" },
                { "in", " (did you mean in8, in16 or in32?)"    },
                { "ld", " (did you mean ld8, ld16 or ld32? scratch memory is "
                        "mld8/mld16/mld32/mld64)"               },
                { "mr", " (did you mean ld8, ld16 or ld32?)"    },
                { "st", " (did you mean st8, st16 or st32? scratch memory is "
                        "mst8/mst16/mst32/mst64)"               },
                { "mw", " (did you mean st8, st16 or st32?)"    },
                { "ml", " (did you mean mld8, mld16, mld32 or mld64?)" },
                { "pc", " (did you mean pcicfg?)"               },
            };
            const char *hint = "";
            for (size_t k = 0; k < sizeof near / sizeof near[0]; k++)
                if (t[0].len >= 2 && dlower(t[0].s[0]) == near[k].pfx[0] &&
                    dlower(t[0].s[1]) == near[k].pfx[1]) { hint = near[k].hint; break; }
            return aerr(as, "unknown instruction \"%.*s\"%s", t[0].len, t[0].s, hint);
        }

        if (pass == 1) {
            if (as->p->ninsn >= DVM_MAX_INSNS)
                return aerr(as, "program too long (max %d instructions)", DVM_MAX_INSNS);
            as->p->ninsn++;
        } else {
            char mn[16];
            int  ml = t[0].len < 15 ? t[0].len : 15;
            for (int k = 0; k < ml; k++) mn[k] = dlower(t[0].s[k]);
            mn[ml] = '\0';
            if (assemble_insn(as, (uint8_t)op, t + 1, nt - 1, mn) != 0) return -1;
        }
    }
    as->p->src_lines = (uint32_t)as->line;
    return 0;
}

int dvm_assemble(const char *src, size_t len, dvm_program_t *out, dvm_asm_err_t *err) {
    if (err) { err->line = 0; err->msg[0] = '\0'; }
    if (!out) return -1;
    dz(out, sizeof *out);

    as_t as;
    dz(&as, sizeof as);
    as.p = out; as.err = err;

    if (!src && len) return aerr(&as, "no program text");
    if (len > DVM_SRC_MAX) {
        as.line = 0;
        return aerr(&as, "program is %lu bytes; the limit is %d",
                    (unsigned long)len, DVM_SRC_MAX);
    }

    if (assemble_pass(&as, src, len, 1) != 0) { dz(out, sizeof *out); return -1; }
    /* Labels survive pass 1; the instruction stream is rebuilt in pass 2. */
    if (assemble_pass(&as, src, len, 2) != 0) { dz(out, sizeof *out); return -1; }

    if (out->ninsn == 0) {
        as.line = 0;
        aerr(&as, "the program contains no instructions");
        dz(out, sizeof *out);
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* 4. validator + disassembler                                            */
/* ====================================================================== */

static int kind_is_value(uint8_t k) {
    return k == DVM_O_NONE || k == DVM_O_REG || k == DVM_O_IMM;
}

/* Which formats write a register, and which operand slot (if any) must be a
 * branch target or a string. Checked structurally so a hand-built program
 * cannot, say, hand a PC-kind operand to `out32` and have it treated as data. */
static int fmt_wants_dst(int fmt) {
    return fmt == F_RD || fmt == F_RD_A || fmt == F_RD_A_B || fmt == F_RD_PORT ||
           fmt == F_RD_ADDR || fmt == F_RD_BDF_OFF || fmt == F_RD_A_B_C ||
           fmt == F_RD_STR_A || fmt == F_RD_STR_A_B || fmt == F_SYS;
}

/* Formats whose operand 0 is a string literal, and for which it is REQUIRED. */
static int fmt_wants_str(int fmt) {
    return fmt == F_STR || fmt == F_STR_OPT_A || fmt == F_RD_STR_A ||
           fmt == F_RD_STR_A_B;
}

dvm_status_t dvm_program_validate(const dvm_program_t *p, dvm_asm_err_t *err) {
    if (err) { err->line = 0; err->msg[0] = '\0'; }
    if (!p) return DVM_TRAP_BADOP;

    if (p->ninsn == 0 || p->ninsn > DVM_MAX_INSNS) {
        if (err) snprintf(err->msg, sizeof err->msg,
                          "program has %lu instructions (must be 1-%d)",
                          (unsigned long)p->ninsn, DVM_MAX_INSNS);
        return DVM_TRAP_BADOP;
    }
    if (p->nstr > DVM_MAX_STRINGS || p->strused > DVM_STRPOOL ||
        p->nlabel > DVM_MAX_LABELS) {
        if (err) snprintf(err->msg, sizeof err->msg, "program tables are out of range");
        return DVM_TRAP_BADOP;
    }

    for (uint32_t i = 0; i < p->ninsn; i++) {
        const dvm_insn_t *in = &p->insn[i];
        if (err) err->line = (int)in->line;

        if (in->op >= DVM_OP__COUNT || !op_info[in->op].name) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu: opcode %u does not exist",
                              (unsigned long)i, in->op);
            return DVM_TRAP_BADOP;
        }
        int fmt = op_info[in->op].fmt;
        if (in->dst != DVM_NOREG && in->dst >= DVM_NREGS) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (%s): destination register r%u does not exist",
                              (unsigned long)i, dvm_op_name(in->op), in->dst);
            return DVM_TRAP_RANGE;
        }
        if (fmt_wants_dst(fmt) && in->dst == DVM_NOREG) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (%s): no destination register",
                              (unsigned long)i, dvm_op_name(in->op));
            return DVM_TRAP_BADOP;
        }
        /* Only a branch may carry a PC operand, and only in slot 0; only abort
         * and print may carry a string, and only in slot 0. */
        int want_pc  = (fmt == F_LBL);
        int want_str = fmt_wants_str(fmt);
        for (int k = 0; k < 3; k++) {
            int is_pc  = (in->kind[k] == DVM_O_PC);
            int is_str = (in->kind[k] == DVM_O_STR);
            if ((is_pc && !(want_pc && k == 0)) || (is_str && !(want_str && k == 0)) ||
                (!is_pc && !is_str && !kind_is_value(in->kind[k]))) {
                if (err) snprintf(err->msg, sizeof err->msg,
                                  "instruction %lu (%s): operand %d has kind %u, which this "
                                  "instruction cannot take",
                                  (unsigned long)i, dvm_op_name(in->op), k, in->kind[k]);
                return DVM_TRAP_BADOP;
            }
        }
        if (want_pc && in->kind[0] != DVM_O_PC) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (%s): operand 0 is not a branch target",
                              (unsigned long)i, dvm_op_name(in->op));
            return DVM_TRAP_BADOP;
        }
        if (fmt != F_STR_OPT_A && want_str && in->kind[0] != DVM_O_STR) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (%s): operand 0 is not a string",
                              (unsigned long)i, dvm_op_name(in->op));
            return DVM_TRAP_BADOP;
        }
        /* A syscall number is baked in by the assembler and is never a
         * register: which hole a program reaches through must be decidable by
         * reading the program, not by running it. */
        if (fmt == F_SYS && (in->kind[0] != DVM_O_IMM || in->val[0] >= DVM_SYS__COUNT)) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "instruction %lu (sys): syscall number %lu is not one of "
                              "the %d this VM has",
                              (unsigned long)i, (unsigned long)in->val[0],
                              (int)DVM_SYS__COUNT);
            return DVM_TRAP_BADOP;
        }
        for (int k = 0; k < 3; k++) {
            switch (in->kind[k]) {
                case DVM_O_NONE: case DVM_O_IMM:
                    break;
                case DVM_O_REG:
                    if (in->val[k] >= DVM_NREGS) {
                        if (err) snprintf(err->msg, sizeof err->msg,
                                          "instruction %lu (%s): register r%lu does not exist",
                                          (unsigned long)i, dvm_op_name(in->op),
                                          (unsigned long)in->val[k]);
                        return DVM_TRAP_RANGE;
                    }
                    break;
                case DVM_O_PC:
                    if (in->val[k] >= p->ninsn) {
                        if (err) snprintf(err->msg, sizeof err->msg,
                                          "instruction %lu (%s): branch target %lu is outside "
                                          "the program (0-%lu)", (unsigned long)i,
                                          dvm_op_name(in->op), (unsigned long)in->val[k],
                                          (unsigned long)p->ninsn - 1);
                        return DVM_TRAP_PC;
                    }
                    break;
                case DVM_O_STR:
                    if (in->val[k] >= p->nstr || p->stroff[in->val[k]] >= DVM_STRPOOL) {
                        if (err) snprintf(err->msg, sizeof err->msg,
                                          "instruction %lu (%s): string %lu is outside the pool",
                                          (unsigned long)i, dvm_op_name(in->op),
                                          (unsigned long)in->val[k]);
                        return DVM_TRAP_BADOP;
                    }
                    break;
                default:
                    if (err) snprintf(err->msg, sizeof err->msg,
                                      "instruction %lu (%s): operand %d has an unknown kind %u",
                                      (unsigned long)i, dvm_op_name(in->op), k, in->kind[k]);
                    return DVM_TRAP_BADOP;
            }
        }
    }
    /* The pool must be NUL-terminated wherever a string starts. */
    for (uint32_t i = 0; i < p->nstr; i++) {
        uint32_t off = p->stroff[i], j = off;
        while (j < DVM_STRPOOL && p->strpool[j]) j++;
        if (j >= DVM_STRPOOL) {
            if (err) snprintf(err->msg, sizeof err->msg,
                              "string %lu is not terminated", (unsigned long)i);
            return DVM_TRAP_BADOP;
        }
    }
    if (err) err->line = 0;
    return DVM_OK;
}

static const char *label_at(const dvm_program_t *p, uint32_t pc) {
    for (uint32_t i = 0; i < p->nlabel; i++)
        if (p->label[i].pc == pc) return p->label[i].name;
    return NULL;
}

/* A string operand is model-authored text, and intern_string() has already
 * decoded \n, \r, \t and \0 into real bytes in the pool. Re-emit it the way
 * copy_printable() emits dvm_result_t.msg — printable ASCII only — because
 * dvm.h offers dvm_disasm() as "one instruction back to text, for listings",
 * i.e. text a caller may print anywhere. Raw, a program whose literal contained
 * a newline followed by '[' could be listed into something indistinguishable
 * from a kernel trace line in column zero. */
static void put_str_operand(sb_t *s, const char *str) {
    sb_addf(s, "\"");
    while (str && *str && s->len + 1 < s->cap) {
        unsigned char c = (unsigned char)*str++;
        s->p[s->len++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        s->p[s->len]   = '\0';
    }
    sb_addf(s, "\"");
}

static void put_operand(sb_t *s, const dvm_program_t *p, uint8_t kind, uint64_t v) {
    switch (kind) {
        case DVM_O_REG: sb_addf(s, "r%lu", (unsigned long)v); break;
        case DVM_O_IMM:
            if (v < 10) sb_addf(s, "%lu", (unsigned long)v);
            else        sb_addf(s, "0x%lx", (unsigned long)v);
            break;
        case DVM_O_PC: {
            const char *l = label_at(p, (uint32_t)v);
            if (l) sb_addf(s, "%s", l);
            else   sb_addf(s, "@%lu", (unsigned long)v);
            break;
        }
        case DVM_O_STR: put_str_operand(s, str_at(p, v)); break;
        default: sb_addf(s, "-"); break;
    }
}

size_t dvm_disasm(const dvm_program_t *p, uint32_t pc, char *buf, size_t cap) {
    if (!p || !buf || !cap) return 0;
    buf[0] = '\0';
    if (pc >= p->ninsn) return 0;

    const dvm_insn_t *in = &p->insn[pc];
    sb_t s;
    sb_init(&s, buf, cap);
    sb_addf(&s, "%s", dvm_op_name(in->op));

    int fmt = (in->op < DVM_OP__COUNT) ? op_info[in->op].fmt : F_NONE;
    switch (fmt) {
        case F_NONE:
            break;
        case F_RD: case F_RD_A: case F_RD_A_B: case F_RD_PORT: case F_RD_BDF_OFF:
        case F_RD_A_B_C:
            sb_addf(&s, " r%u", in->dst);
            for (int k = 0; k < 3; k++)
                if (in->kind[k] != DVM_O_NONE) {
                    sb_addf(&s, ", ");
                    put_operand(&s, p, in->kind[k], in->val[k]);
                }
            break;
        /* The string is encoded in slot 0 but written last, so the listing has
         * to reorder it back: a disassembly a model cannot paste into the next
         * attempt is not a listing, it is a riddle. */
        case F_RD_STR_A: case F_RD_STR_A_B:
            sb_addf(&s, " r%u, ", in->dst);
            put_operand(&s, p, in->kind[1], in->val[1]);
            if (fmt == F_RD_STR_A_B) {
                sb_addf(&s, ", ");
                put_operand(&s, p, in->kind[2], in->val[2]);
            }
            sb_addf(&s, ", ");
            put_operand(&s, p, in->kind[0], in->val[0]);
            break;
        case F_SYS:
            sb_addf(&s, " r%u, %s", in->dst,
                    dvm_sys_name((dvm_sys_nr_t)in->val[0]));
            break;
        case F_RD_ADDR:
            sb_addf(&s, " r%u, [", in->dst);
            put_operand(&s, p, in->kind[0], in->val[0]);
            if (!(in->kind[1] == DVM_O_IMM && in->val[1] == 0)) {
                sb_addf(&s, "+");
                put_operand(&s, p, in->kind[1], in->val[1]);
            }
            sb_addf(&s, "]");
            break;
        case F_ADDR_A:
            sb_addf(&s, " [");
            put_operand(&s, p, in->kind[0], in->val[0]);
            if (!(in->kind[1] == DVM_O_IMM && in->val[1] == 0)) {
                sb_addf(&s, "+");
                put_operand(&s, p, in->kind[1], in->val[1]);
            }
            sb_addf(&s, "], ");
            put_operand(&s, p, in->kind[2], in->val[2]);
            break;
        default:
            for (int k = 0, first = 1; k < 3; k++)
                if (in->kind[k] != DVM_O_NONE) {
                    sb_addf(&s, first ? " " : ", ");
                    put_operand(&s, p, in->kind[k], in->val[k]);
                    first = 0;
                }
            break;
    }
    return s.len;
}

/* ====================================================================== */
/* 5. policy                                                              */
/* ====================================================================== */

/* Ports no caller may open, whatever it asks for. Everything here can take the
 * machine down or blind the operator, and none of it is a device a runtime
 * driver has any business bringing up.
 *
 * The reason strings end up in dvm_result_t.msg, so keep them plain ASCII:
 * copy_printable() replaces anything else, and the VGA console cannot render it
 * anyway. */
static const struct { uint16_t lo, hi; const char *why; } port_deny[] = {
    { 0x0020, 0x0021, "master PIC" },
    { 0x0040, 0x0043, "PIT: millis() and mdelay() are calibrated against it" },
    { 0x0060, 0x0064, "PS/2 controller: 0x64 can pulse the CPU reset line" },
    { 0x0070, 0x0071, "CMOS/NMI" },
    { 0x0092, 0x0092, "fast A20 / system reset" },
    { 0x00A0, 0x00A1, "slave PIC" },
    { 0x02F8, 0x02FF, "COM2" },
    { 0x03F8, 0x03FF, "COM1: the console this trace is printed on" },
    { 0x0CF8, 0x0CFF, "PCI config space: use pcicfg, which is read-only" },

    /* Below: not "can crash the machine" but "can destroy something that does
     * not come back". A window derived from a BAR is bounded by alignment and a
     * cap rather than by the BAR's true size, so it can be wider than the
     * device; these are the fixed legacy ranges that an over-wide window has
     * anything to lose by reaching. None of them is ever a PCI BAR. */
    { 0x0000, 0x000F, "ISA DMA controller 1: it bus-masters into RAM" },
    { 0x0080, 0x008F, "ISA DMA page registers" },
    { 0x00C0, 0x00DF, "ISA DMA controller 2: it bus-masters into RAM" },
    { 0x0170, 0x0177, "secondary ATA task file: it can write the disk" },
    { 0x01F0, 0x01F7, "primary ATA task file: it can write the disk" },
    { 0x03B0, 0x03DF, "legacy VGA registers: the operator's console at its "
                      "fixed address, the same asset as 0xb8000" },
    { 0x03F0, 0x03F7, "floppy controller" },
};
#define NPORT_DENY ((int)(sizeof port_deny / sizeof port_deny[0]))

/* IOAPIC / HPET / local APIC. See mmio_gate(). */
#define DVM_PLATFORM_LO 0xFEC00000ull
#define DVM_PLATFORM_HI 0xFEF00000ull

/* End of everything the kernel owns: the real-mode IVT, the VGA text buffer at
 * 0xB8000 (which shares a 2 MiB huge page with page 0 and so can never be
 * unmapped), the kernel image, and the heap arena in .bss. Rounded up to a huge
 * page. No MMIO window may start below this. */
#ifndef FABLEOS_HOSTTEST
extern char __bss_end[];                 /* provided by linker.ld */
#endif

static uint64_t kernel_top(void) {
#ifndef FABLEOS_HOSTTEST
    uint64_t e = (uint64_t)(uintptr_t)__bss_end;
    e = (e + 0x1FFFFFull) & ~0x1FFFFFull;
    return e < 0x1000000ull ? 0x1000000ull : e;
#else
    return 0x1000000ull;                 /* 16 MiB on the host, for tests */
#endif
}

/* ---- the DMA scratch arena ----
 *
 * ONE static array, and the only memory a driver program can ever write. It is
 * deliberately NOT heap-allocated: a device keeps DMAing out of it after
 * dvm_run() has returned (that is the whole point of starting playback), so a
 * buffer that could be freed and handed to another allocation would be live DMA
 * memory in somebody else's object. .bss is the only lifetime that is honest
 * here, and it also means the address never changes between runs, so a program
 * that failed can be retried against the same buffer.
 *
 * Alignment and size are argued in include/dvm.h. `volatile` because a device
 * reads it behind the compiler's back, so the stores a program makes must not be
 * elided or reordered past the register write that starts the transfer.
 *
 * On a host test build this is just an array in the test process, and the VM's
 * ld/st reach it through exactly the same code path — which is what makes the
 * host suite a real proof of the kernel behaviour rather than a model of it.
 *
 * THE GUARD BANDS. The arena sits in the middle of a larger block with
 * DVM_DMA_GUARD bytes of poison either side. The VM itself can never reach them
 * (dma_gate bounds every access to the granted range), so they exist for the
 * half of the threat model the VM cannot police: a DEVICE that was handed a
 * length in the wrong units, or a descriptor list one entry too long, DMAs past
 * the end of the buffer. Without the guard that is silent corruption of whatever
 * .bss object the linker happened to place next. With it, dvm_dma_check() turns
 * the commonest form of the accident into a reported fact, with a byte offset,
 * which is also exactly the diagnostic the model needs to fix its descriptors.
 *
 * One array rather than three, so C guarantees the guards are adjacent to the
 * arena; three objects would be at the linker's discretion. DVM_DMA_GUARD is a
 * multiple of DVM_DMA_ALIGN, so the arena inside it stays page-aligned.
 *
 * Stated plainly: this catches an overrun of up to DVM_DMA_GUARD bytes. A device
 * pointed at a completely wrong address writes there and the guard never sees
 * it. Only an IOMMU fixes that. */
#define DVM_DMA_POISON   0xA5

static volatile uint8_t dvm_dma_block[DVM_DMA_GUARD + DVM_DMA_SIZE + DVM_DMA_GUARD]
    __attribute__((aligned(DVM_DMA_ALIGN)));

static volatile uint8_t *dma_arena(void) { return dvm_dma_block + DVM_DMA_GUARD; }

void dvm_dma_region(uint64_t *base, uint64_t *size) {
    if (base) *base = (uint64_t)(uintptr_t)dma_arena();
    if (size) *size = DVM_DMA_SIZE;
}

/* Re-poison both guard bands. .bss starts zeroed, so an unarmed guard would read
 * as "0 != 0xa5" and report a false overrun on the very first check; arming is
 * therefore not optional and dvm_dma_check() does it for the caller. */
static int dma_guard_armed;

static void dma_guard_arm(void) {
    for (uint64_t i = 0; i < DVM_DMA_GUARD; i++) {
        dvm_dma_block[i] = DVM_DMA_POISON;
        dvm_dma_block[DVM_DMA_GUARD + DVM_DMA_SIZE + i] = DVM_DMA_POISON;
    }
    dma_guard_armed = 1;
}

int dvm_dma_check(char *msg, size_t cap) {
    sb_t s;
    sb_init(&s, msg, cap);

    /* Never armed means no run has carried a DMA grant, so there is nothing to
     * report — and reporting anything would be a lie: .bss starts at zero, which
     * is not the poison value, so an unguarded check would read as a huge overrun
     * on a machine where no DMA has ever been possible. Arm and say nothing. */
    if (!dma_guard_armed) { dma_guard_arm(); return 0; }

    /* Report the FIRST damaged byte on each side and how far out it was: "12
     * bytes past the end" is actionable, "the guard is dirty" is not. */
    int64_t over = -1, under = -1;
    for (uint64_t i = 0; i < DVM_DMA_GUARD; i++)
        if (dvm_dma_block[DVM_DMA_GUARD + DVM_DMA_SIZE + i] != DVM_DMA_POISON) {
            over = (int64_t)i; break;
        }
    for (uint64_t i = DVM_DMA_GUARD; i > 0; i--)
        if (dvm_dma_block[i - 1] != DVM_DMA_POISON) {
            under = (int64_t)(DVM_DMA_GUARD - i + 1); break;
        }

    if (over < 0 && under < 0) return 0;

    if (over >= 0 && under >= 0)
        sb_addf(&s, "a device wrote %lu byte(s) past the end of the dma buffer AND "
                    "%lu byte(s) before its start",
                (unsigned long)over + 1, (unsigned long)under);
    else if (over >= 0)
        sb_addf(&s, "a device wrote past the end of the dma buffer: the guard band is "
                    "damaged from +%lu, so a descriptor length or count was too big",
                (unsigned long)over);
    else
        sb_addf(&s, "a device wrote %lu byte(s) BELOW the start of the dma buffer, so "
                    "an address handed to it was under the buffer base",
                (unsigned long)under);

    dma_guard_arm();          /* re-arm, or every later run inherits this verdict */
    return 1;
}

/* ---- the scratch arena ----
 *
 * The program's own memory: an address space of its own, addressed by offset,
 * that no device sees and no policy governs. Plain .bss, not volatile (nothing
 * outside this file reads it while a program runs) and not guarded (nothing can
 * overrun it — every access is bounds-checked against DVM_MEM_SIZE before the
 * pointer is formed, which is a stronger property than the DMA arena's guard
 * bands can offer, because there the threat is hardware).
 *
 * Zeroed by dvm_run() before the first instruction, so a program cannot read
 * what the last one left. The kernel still can, until the next run: see
 * dvm_mem_peek(). */
static uint8_t dvm_mem[DVM_MEM_SIZE];

size_t dvm_mem_peek(uint64_t off, void *dst, size_t n) {
    if (!dst || !n) return 0;
    if (off >= DVM_MEM_SIZE) return 0;
    uint64_t room = (uint64_t)DVM_MEM_SIZE - off;
    if ((uint64_t)n > room) n = (size_t)room;
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = dvm_mem[off + i];
    return n;
}

/* A path is acceptable if it is absolute, printable, bounded, and made only of
 * components that are neither "." nor "..". Shared by dvm_policy_set_fs_root()
 * and the fs.* argument check, so a root and a path are held to one rule.
 * Returns 0, or -1 with a reason in `why`. */
static int path_ok(const char *s, size_t len, char *why, size_t cap) {
    sb_t w;
    sb_init(&w, why, cap);
    if (!len)              { sb_addf(&w, "the path is empty"); return -1; }
    if (len >= DVM_PATH_MAX) {
        sb_addf(&w, "the path is %lu bytes; the limit is %d",
                (unsigned long)len, DVM_PATH_MAX - 1);
        return -1;
    }
    if (s[0] != '/')       { sb_addf(&w, "the path must be absolute (start with '/')");
                             return -1; }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c >= 0x7F) {
            sb_addf(&w, "byte %lu of the path is 0x%02x, which is not printable ASCII",
                    (unsigned long)i, (unsigned)c);
            return -1;
        }
    }
    /* Component walk. ".." is refused rather than resolved: resolving it here
     * and in the VFS would be two implementations of one rule, and the pair
     * that disagrees is how a confinement gets escaped. */
    for (size_t i = 0; i < len;) {
        while (i < len && s[i] == '/') i++;
        size_t j = i;
        while (j < len && s[j] != '/') j++;
        size_t n = j - i;
        if ((n == 1 && s[i] == '.') || (n == 2 && s[i] == '.' && s[i + 1] == '.')) {
            sb_addf(&w, "the path contains a \"%.*s\" component; write the whole path "
                        "out instead", (int)n, s + i);
            return -1;
        }
        i = j;
    }
    return 0;
}

int dvm_policy_set_fs_root(dvm_policy_t *p, const char *root) {
    if (!p || !root) return -1;
    size_t n = 0;
    while (n < DVM_PATH_MAX && root[n]) n++;
    if (n >= DVM_PATH_MAX) return -1;
    char why[DVM_MSG_MAX];
    if (path_ok(root, n, why, sizeof why) != 0) return -1;
    for (size_t i = 0; i < n; i++) p->fs_root[i] = root[i];
    for (size_t i = n; i < DVM_PATH_MAX; i++) p->fs_root[i] = '\0';
    return 0;
}

int dvm_policy_allow_sys(dvm_policy_t *p, dvm_sys_nr_t nr) {
    if (!p || (unsigned)nr >= DVM_SYS__COUNT || !sys_info[nr].name) return -1;
    p->sys_allow |= (uint32_t)1u << (unsigned)nr;
    return 0;
}

/* 1 if this syscall names a file. Used twice: to demand an fs_root in
 * dvm_policy_check(), and to know whether to build a path in the interpreter. */
static int sys_is_fs(dvm_sys_nr_t nr) {
    return nr == DVM_SYS_FS_READ || nr == DVM_SYS_FS_WRITE || nr == DVM_SYS_FS_SIZE;
}

int dvm_policy_allow_dma(dvm_policy_t *p, uint64_t base, uint64_t size) {
    if (!p || !size) return -1;
    if (base + size < base) return -1;                 /* wraps */

    uint64_t abase, asize;
    dvm_dma_region(&abase, &asize);
    /* The load-bearing line in this function: the grant must be a subrange of
     * the arena this file owns. A caller passes a number, but it cannot pass a
     * number that means anything except "part of dvm_dma_arena". */
    if (base < abase || base + size > abase + asize) return -1;

    p->dma_base = base;
    p->dma_size = size;
    return 0;
}

void dvm_policy_init(dvm_policy_t *p) {
    if (!p) return;
    dz(p, sizeof *p);
    p->max_steps           = 100000;
    p->max_io              = 4096;
    p->max_dma_ops         = DVM_DMA_SIZE / 2;   /* the whole buffer as st16 */
    /* 64 passes over the whole scratch arena. A text-shaped program does single
     * digits; this is generous enough that hitting it means a loop is wrong. */
    p->max_mem_bytes       = 64ull * DVM_MEM_SIZE;
    p->max_sys             = 64;
    p->max_delay_us        = 200000;     /* 200 ms */
    p->max_single_delay_us = 50000;      /* 50 ms  */
    p->max_prints          = 64;
    p->max_trace           = 512;
    p->trace               = DVM_TRACE_IO;
    /* dma_base/dma_size stay 0: deny-all includes the buffer. */
}

int dvm_policy_allow_ports(dvm_policy_t *p, uint16_t lo, uint16_t hi) {
    if (!p || lo > hi) return -1;
    if (p->nport < 0 || p->nport >= DVM_MAX_PORT_RANGES) return -1;
    p->port[p->nport].lo = lo;
    p->port[p->nport].hi = hi;
    p->nport++;
    return 0;
}

int dvm_policy_allow_mmio(dvm_policy_t *p, uint64_t base, uint64_t size, uint32_t flags) {
    if (!p || !size) return -1;
    if (flags & ~(uint32_t)DVM_MMIO_RW) return -1;
    if (!(flags & DVM_MMIO_RW)) return -1;
    if (base + size < base) return -1;                 /* wraps */
    if (p->nmmio < 0 || p->nmmio >= DVM_MAX_MMIO_WINS) return -1;
    p->mmio[p->nmmio].base  = base;
    p->mmio[p->nmmio].size  = size;
    p->mmio[p->nmmio].flags = flags;
    p->nmmio++;
    return 0;
}

int dvm_policy_allow_pci(dvm_policy_t *p, uint8_t bus, uint8_t dev, uint8_t fn) {
    if (!p || dev > 31 || fn > 7) return -1;
    if (p->npci < 0 || p->npci >= DVM_MAX_PCI_FNS) return -1;
    p->pci[p->npci].bus = bus;
    p->pci[p->npci].dev = dev;
    p->pci[p->npci].fn  = fn;
    p->npci++;
    return 0;
}

dvm_status_t dvm_policy_check(const dvm_policy_t *p, char *msg, size_t cap) {
    sb_t s;
    sb_init(&s, msg, cap);
    if (!p) { sb_addf(&s, "no policy supplied"); return DVM_TRAP_POLICY; }

    if (p->nport < 0 || p->nport > DVM_MAX_PORT_RANGES ||
        p->nmmio < 0 || p->nmmio > DVM_MAX_MMIO_WINS ||
        p->npci  < 0 || p->npci  > DVM_MAX_PCI_FNS) {
        sb_addf(&s, "policy table counts are out of range");
        return DVM_TRAP_POLICY;
    }

    for (int i = 0; i < p->nport; i++) {
        if (p->port[i].lo > p->port[i].hi) {
            sb_addf(&s, "port range %d is inverted (0x%04x > 0x%04x)", i,
                    (unsigned)p->port[i].lo, (unsigned)p->port[i].hi);
            return DVM_TRAP_POLICY;
        }
        for (int d = 0; d < NPORT_DENY; d++)
            if (p->port[i].lo <= port_deny[d].hi && p->port[i].hi >= port_deny[d].lo) {
                sb_addf(&s, "port range 0x%04x-0x%04x overlaps 0x%04x-0x%04x, which is "
                            "never allowed: %s",
                        (unsigned)p->port[i].lo, (unsigned)p->port[i].hi,
                        (unsigned)port_deny[d].lo, (unsigned)port_deny[d].hi,
                        port_deny[d].why);
                return DVM_TRAP_POLICY;
            }
    }

    /* The DMA grant, re-checked here and not only in dvm_policy_allow_dma(), for
     * the same reason the port deny list is applied twice: a policy that was
     * hand-built, memcpy'd or corrupted never went through the setter. Every run
     * therefore re-proves that the granted range is inside this file's own arena
     * before a single st8 can reach it. */
    if (p->dma_base || p->dma_size) {
        uint64_t abase, asize;
        dvm_dma_region(&abase, &asize);
        if (!p->dma_size || p->dma_base + p->dma_size < p->dma_base ||
            p->dma_base < abase || p->dma_base + p->dma_size > abase + asize) {
            sb_addf(&s, "dma window 0x%lx+0x%lx is not inside the VM's scratch arena "
                        "(0x%lx+0x%lx); a program's only memory is that arena",
                    (unsigned long)p->dma_base, (unsigned long)p->dma_size,
                    (unsigned long)abase, (unsigned long)asize);
            return DVM_TRAP_POLICY;
        }
#ifndef FABLEOS_HOSTTEST
        /* Only the low 4 GiB is mapped, and a 32-bit bus master cannot address
         * anything else, so an arena that landed higher would be a buffer no
         * device could reach — silence, with no error anywhere. It is placed in
         * .bss of an image linked at 1 MiB so this cannot currently happen; the
         * check is here because "cannot currently happen" is a property of the
         * linker script, not of this file. Host builds are exempt: the arena is
         * an array in a 64-bit test process and its address means nothing to any
         * hardware. */
        if (abase + asize > MMIO_TOP) {
            sb_addf(&s, "the dma scratch arena is at 0x%lx+0x%lx, above the 4 GiB a "
                        "32-bit bus master can address; no device could reach it",
                    (unsigned long)abase, (unsigned long)asize);
            return DVM_TRAP_POLICY;
        }
#endif
        /* An MMIO window that overlaps the arena would make the same bytes
         * reachable under the device-access budget as well, and would make the
         * trace lie about which of the two a given access was. It cannot happen
         * through dvm_policy_allow_mmio (the arena is below kernel_top, so that
         * path already refuses it), so this is a belt on a brace. */
        for (int i = 0; i < p->nmmio; i++)
            if (p->mmio[i].base < p->dma_base + p->dma_size &&
                p->dma_base < p->mmio[i].base + p->mmio[i].size) {
                sb_addf(&s, "mmio window 0x%lx+0x%lx overlaps the dma scratch buffer "
                            "0x%lx+0x%lx; the two may not alias",
                        (unsigned long)p->mmio[i].base, (unsigned long)p->mmio[i].size,
                        (unsigned long)p->dma_base, (unsigned long)p->dma_size);
                return DVM_TRAP_POLICY;
            }
    }

    uint64_t ktop = kernel_top();
    for (int i = 0; i < p->nmmio; i++) {
        const dvm_mmio_win_t *w = &p->mmio[i];
        if (!w->size || w->base + w->size < w->base) {
            sb_addf(&s, "mmio window %d (0x%lx+0x%lx) is empty or wraps", i,
                    (unsigned long)w->base, (unsigned long)w->size);
            return DVM_TRAP_POLICY;
        }
        if (!(w->flags & DVM_MMIO_RW) || (w->flags & ~(uint32_t)DVM_MMIO_RW)) {
            sb_addf(&s, "mmio window %d has no valid access flags (0x%lx)", i,
                    (unsigned long)w->flags);
            return DVM_TRAP_POLICY;
        }
        if (w->base < ktop) {
            sb_addf(&s, "mmio window 0x%lx+0x%lx reaches below 0x%lx, which is kernel "
                        "memory (the VGA text buffer at 0xb8000, the kernel image and "
                        "the heap all live there)",
                    (unsigned long)w->base, (unsigned long)w->size, (unsigned long)ktop);
            return DVM_TRAP_POLICY;
        }
        if (w->base + w->size > MMIO_TOP) {
            sb_addf(&s, "mmio window 0x%lx+0x%lx runs past 0x100000000; only the low "
                        "4 GiB is mapped",
                    (unsigned long)w->base, (unsigned long)w->size);
            return DVM_TRAP_POLICY;
        }
        if (w->base + w->size > DVM_PLATFORM_LO && w->base < DVM_PLATFORM_HI) {
            sb_addf(&s, "mmio window 0x%lx+0x%lx overlaps the platform block "
                        "(0x%lx-0x%lx: IOAPIC, HPET, local APIC), which is never "
                        "allowed",
                    (unsigned long)w->base, (unsigned long)w->size,
                    (unsigned long)DVM_PLATFORM_LO, (unsigned long)DVM_PLATFORM_HI - 1);
            return DVM_TRAP_POLICY;
        }
    }

    for (int i = 0; i < p->npci; i++)
        if (p->pci[i].dev > 31 || p->pci[i].fn > 7) {
            sb_addf(&s, "pci entry %d (%02x:%02x.%x) is not a valid address", i,
                    p->pci[i].bus, p->pci[i].dev, p->pci[i].fn);
            return DVM_TRAP_POLICY;
        }

    if (p->max_steps == 0 || p->max_steps > CEIL_STEPS) {
        sb_addf(&s, "max_steps %lu is outside 1-%lu",
                (unsigned long)p->max_steps, (unsigned long)CEIL_STEPS);
        return DVM_TRAP_POLICY;
    }
    if (p->max_io > CEIL_IO) {
        sb_addf(&s, "max_io %lu exceeds %lu", (unsigned long)p->max_io, (unsigned long)CEIL_IO);
        return DVM_TRAP_POLICY;
    }
    if (p->max_dma_ops > CEIL_DMA_OPS) {
        sb_addf(&s, "max_dma_ops %lu exceeds %lu, which is one access per byte of "
                    "the scratch buffer",
                (unsigned long)p->max_dma_ops, (unsigned long)CEIL_DMA_OPS);
        return DVM_TRAP_POLICY;
    }
    if (p->max_mem_bytes > CEIL_MEM_BYTES) {
        sb_addf(&s, "max_mem_bytes %lu exceeds %lu",
                (unsigned long)p->max_mem_bytes, (unsigned long)CEIL_MEM_BYTES);
        return DVM_TRAP_POLICY;
    }
    if (p->max_sys > CEIL_SYS) {
        sb_addf(&s, "max_sys %lu exceeds %lu",
                (unsigned long)p->max_sys, (unsigned long)CEIL_SYS);
        return DVM_TRAP_POLICY;
    }
    /* Syscalls. A bit with no syscall behind it is a corrupt policy, not a
     * harmless one: it would mean somebody built this mask by arithmetic
     * instead of by dvm_policy_allow_sys(), and the next number added to the
     * enum would silently become granted. */
    if (p->sys_allow >> DVM_SYS__COUNT) {
        sb_addf(&s, "sys_allow 0x%lx has bits above the %d syscalls this VM has",
                (unsigned long)p->sys_allow, (int)DVM_SYS__COUNT);
        return DVM_TRAP_POLICY;
    }
    {
        /* fs_root must be NUL-terminated inside its own array before anything
         * reads it as a string, and must be a legal path if any fs syscall is
         * open. An open fs syscall with no root is refused rather than treated
         * as "/": a confinement that defaults to everything is not one. */
        size_t rl = 0;
        while (rl < DVM_PATH_MAX && p->fs_root[rl]) rl++;
        if (rl >= DVM_PATH_MAX) {
            sb_addf(&s, "fs_root is not terminated inside its %d-byte field",
                    DVM_PATH_MAX);
            return DVM_TRAP_POLICY;
        }
        int fs_open = 0;
        for (int i = 0; i < DVM_SYS__COUNT; i++)
            if ((p->sys_allow & ((uint32_t)1u << i)) && sys_is_fs((dvm_sys_nr_t)i))
                fs_open = 1;
        if (rl) {
            char why[DVM_MSG_MAX];
            if (path_ok(p->fs_root, rl, why, sizeof why) != 0) {
                sb_addf(&s, "fs_root \"%s\" is not usable: %s", p->fs_root, why);
                return DVM_TRAP_POLICY;
            }
        } else if (fs_open) {
            sb_addf(&s, "a filesystem syscall is allowed but fs_root is empty; a "
                        "program may not name a file until a caller has said which "
                        "subtree it may name one in");
            return DVM_TRAP_POLICY;
        }
    }
    if (p->max_delay_us > CEIL_DELAY_US) {
        sb_addf(&s, "max_delay_us %lu exceeds %lu",
                (unsigned long)p->max_delay_us, (unsigned long)CEIL_DELAY_US);
        return DVM_TRAP_POLICY;
    }
    if (p->max_single_delay_us > CEIL_SINGLE_US) {
        sb_addf(&s, "max_single_delay_us %lu exceeds %lu",
                (unsigned long)p->max_single_delay_us, (unsigned long)CEIL_SINGLE_US);
        return DVM_TRAP_POLICY;
    }
    if (p->max_prints > CEIL_PRINTS) {
        sb_addf(&s, "max_prints %lu exceeds %lu",
                (unsigned long)p->max_prints, (unsigned long)CEIL_PRINTS);
        return DVM_TRAP_POLICY;
    }
    if (p->max_trace > CEIL_TRACE) {
        sb_addf(&s, "max_trace %lu exceeds %lu",
                (unsigned long)p->max_trace, (unsigned long)CEIL_TRACE);
        return DVM_TRAP_POLICY;
    }
    if ((int)p->trace < DVM_TRACE_OFF || (int)p->trace > DVM_TRACE_ALL) {
        sb_addf(&s, "trace level %d is not 0, 1 or 2", (int)p->trace);
        return DVM_TRAP_POLICY;
    }
    return DVM_OK;
}

/* ====================================================================== */
/* 6. the interpreter                                                     */
/* ====================================================================== */

typedef struct {
    const dvm_program_t *p;
    const dvm_policy_t  *pol;
    const dvm_io_t      *io;
    dvm_result_t        *res;

    uint64_t reg[DVM_NREGS];
    uint64_t dstack[DVM_DSTACK];
    uint32_t dsp;
    uint32_t cstack[DVM_CSTACK];
    uint32_t csp;

    uint32_t pc;
    uint32_t line;
    int      zero, below;     /* flags from cmp/test */
    int      quiet;           /* trace budget spent  */
} run_t;

/* ---- trace ---- */

static int tracing(const run_t *st, dvm_trace_t level) {
    return st->pol->trace >= level;
}

/* Returns 1 when a line may be emitted. Once the budget is spent it says so
 * ONCE, then counts every line it swallows so the result can report how much of
 * the transcript is missing — a silently truncated trace would be worse than a
 * loud one. Trap lines bypass this entirely. */
static int trace_budget(run_t *st) {
    if (st->quiet || st->res->trace_lines >= st->pol->max_trace) {
        if (!st->quiet) {
            st->quiet = 1;
            st->res->trace_lines++;
            trace_ok("dvm.trace", "line budget %lu reached; further dvm lines suppressed",
                     (unsigned long)st->pol->max_trace);
        }
        st->res->trace_dropped++;
        return 0;
    }
    st->res->trace_lines++;
    return 1;
}

static void tline(run_t *st, const char *fmt, ...) {
    if (!trace_budget(st)) return;
    char buf[TRACE_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    trace_ok("dvm", "%s", buf);
}

/* ---- traps ---- */

static dvm_status_t trap(run_t *st, dvm_status_t s, const char *fmt, ...) {
    char reason[DVM_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof reason, fmt, ap);
    va_end(ap);

    st->res->status = s;
    st->res->pc     = st->pc;
    st->res->line   = st->line;
    copy_printable(st->res->msg, sizeof st->res->msg, reason);

    if (st->pol->trace != DVM_TRACE_OFF) {
        /* A trap line is never suppressed: it is the one line that explains the
         * run, and the budget exists to stop noise, not evidence. */
        st->res->trace_lines++;
        trace_err(DVM_TRACE_ERR, "dvm.trap", "pc=%03lu ln=%lu %s: %s",
                  (unsigned long)st->pc, (unsigned long)st->line,
                  dvm_status_name(s), reason);
    }
    return s;
}

/* ---- operand access ---- */

static uint64_t rd(const run_t *st, const dvm_insn_t *in, int k) {
    switch (in->kind[k]) {
        case DVM_O_REG: return st->reg[in->val[k] & (DVM_NREGS - 1)];
        case DVM_O_IMM:
        case DVM_O_PC:
        case DVM_O_STR: return in->val[k];
        default:        return 0;
    }
}

/* ---- access checks ---- */

static const char *port_denied_why(uint32_t port, uint32_t width) {
    for (int d = 0; d < NPORT_DENY; d++)
        if (port <= port_deny[d].hi && port + width - 1 >= port_deny[d].lo)
            return port_deny[d].why;
    return NULL;
}

static int port_allowed(const dvm_policy_t *p, uint32_t port, uint32_t width) {
    for (int i = 0; i < p->nport; i++)
        if (port >= p->port[i].lo && port + width - 1 <= p->port[i].hi) return 1;
    return 0;
}

/* Buffers for the three "what was this program allowed to touch" strings, sized
 * from the policy limits rather than guessed, so a diagnostic can never hide a
 * grant the program actually has. One port range renders as "0xffff-0xffff,"
 * (14 bytes). dvm_policy_check() has already bounded every window to the low
 * 4 GiB by the time any of these run, so one MMIO window is at most
 * "0x1fffffff+0x100000000rw," (26) and one PCI function "00:1f.7," (9).
 *
 * These were 96 and 128 bytes, i.e. below the maximum policy: a program with a
 * full port table was told about its first six ranges and not the other two.
 * Note that a TRAP message is separately bounded by DVM_MSG_MAX, so a maximal
 * policy can still be clipped there — but it is clipped in one place, with a
 * stated size, instead of twice. */
#define PORT_GRANTS_CAP (DVM_MAX_PORT_RANGES * 14 + 1)
#define MMIO_GRANTS_CAP (DVM_MAX_MMIO_WINS  * 26 + 1)
#define PCI_GRANTS_CAP  (DVM_MAX_PCI_FNS    *  9 + 1)
/* One buffer serves all three lines in dvm_run(): the widest of them. */
#define GRANTS_CAP MMIO_GRANTS_CAP
_Static_assert(GRANTS_CAP >= PORT_GRANTS_CAP && GRANTS_CAP >= PCI_GRANTS_CAP,
               "GRANTS_CAP must hold the widest grant list");

static void put_port_grants(sb_t *s, const dvm_policy_t *p) {
    if (!p->nport) { sb_addf(s, "none"); return; }
    for (int i = 0; i < p->nport; i++)
        sb_addf(s, "%s0x%04x-0x%04x", i ? "," : "",
                (unsigned)p->port[i].lo, (unsigned)p->port[i].hi);
}

static void put_mmio_grants(sb_t *s, const dvm_policy_t *p) {
    if (!p->nmmio) { sb_addf(s, "none"); return; }
    for (int i = 0; i < p->nmmio; i++)
        sb_addf(s, "%s0x%lx+0x%lx%s%s", i ? "," : "",
                (unsigned long)p->mmio[i].base, (unsigned long)p->mmio[i].size,
                (p->mmio[i].flags & DVM_MMIO_R) ? "r" : "",
                (p->mmio[i].flags & DVM_MMIO_W) ? "w" : "");
}

/* Every port access funnels through here. Order matters: a refusal is reported
 * ahead of an exhausted budget, so "it tried to touch the PIC" is never hidden
 * behind "it ran out of I/O credit". */
static dvm_status_t port_gate(run_t *st, uint64_t rawport, uint32_t width,
                              const char *what) {
    if (rawport > 0xFFFFu || rawport + width - 1 > 0xFFFFu)
        return trap(st, DVM_TRAP_RANGE,
                    "%s: port 0x%lx is outside the 0x0000-0xffff I/O space",
                    what, (unsigned long)rawport);

    const char *why = port_denied_why((uint32_t)rawport, width);
    if (why)
        return trap(st, DVM_TRAP_PORT_DENIED,
                    "%s: port 0x%04lx is never accessible from a driver program (%s)",
                    what, (unsigned long)rawport, why);

    if (!port_allowed(st->pol, (uint32_t)rawport, width)) {
        char g[PORT_GRANTS_CAP];
        sb_t s; sb_init(&s, g, sizeof g);
        put_port_grants(&s, st->pol);
        return trap(st, DVM_TRAP_PORT_DENIED,
                    "%s: port 0x%04lx is not in this program's allowed ranges (%s)",
                    what, (unsigned long)rawport, g);
    }
    if (st->res->io_ops >= st->pol->max_io)
        return trap(st, DVM_TRAP_IO_BUDGET,
                    "device access budget of %lu reached at %s",
                    (unsigned long)st->pol->max_io, what);
    st->res->io_ops++;
    return DVM_OK;
}

/* ---- the DMA scratch buffer ----
 *
 * ld/st inside the granted arena range are NOT device accesses: no bus cycle
 * happens, nothing is clear-on-read, and a program filling a second of PCM makes
 * tens of thousands of them. So they are gated, counted and traced separately
 * from MMIO, and they are performed here rather than through dvm_io_t. Routing
 * them through the backend would buy nothing (on hardware the hook would do
 * exactly this store) and would cost the one property that makes the buffer
 * testable: on a host build the arena is real memory, so a test can read back
 * the bytes the VM wrote through the same code the kernel runs. */

/* 1 when the access touches ANY byte of the granted buffer, even partially. A
 * straddling access must land here and be refused, never silently fall through
 * to the MMIO allowlist. */
static int dma_touches(const dvm_policy_t *p, uint64_t addr, uint32_t width) {
    if (!p->dma_size) return 0;
    return addr < p->dma_base + p->dma_size && addr + width > p->dma_base;
}

static dvm_status_t dma_gate(run_t *st, uint64_t addr, uint32_t width,
                             const char *what) {
    const dvm_policy_t *p = st->pol;
    if (addr < p->dma_base || addr + width > p->dma_base + p->dma_size)
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: 0x%lx+%lu runs off the end of the dma buffer "
                    "(0x%lx+0x%lx); an access must be wholly inside it",
                    what, (unsigned long)addr, (unsigned long)width,
                    (unsigned long)p->dma_base, (unsigned long)p->dma_size);
    if (addr % width)
        return trap(st, DVM_TRAP_ALIGN,
                    "%s: address 0x%lx is not %lu-byte aligned; the dma buffer is "
                    "accessed at natural alignment, like a device register",
                    what, (unsigned long)addr, (unsigned long)width);
    if ((uint64_t)st->res->n_dma_rd + st->res->n_dma_wr >= p->max_dma_ops)
        return trap(st, DVM_TRAP_DMA_BUDGET,
                    "dma buffer access budget of %lu reached at %s; fill the buffer "
                    "with wider stores or write less of it",
                    (unsigned long)p->max_dma_ops, what);
    return DVM_OK;
}

/* The gate has already proved the address is inside the arena and naturally
 * aligned, so these two are the only unchecked pointer casts in the file and
 * they are three lines away from the check that licenses them. */
static uint32_t dma_load(uint64_t addr, uint32_t width) {
    volatile void *p = (volatile void *)(uintptr_t)addr;
    switch (width) {
        case 1:  return *(volatile uint8_t  *)p;
        case 2:  return *(volatile uint16_t *)p;
        default: return *(volatile uint32_t *)p;
    }
}

static void dma_store(uint64_t addr, uint32_t width, uint32_t val) {
    volatile void *p = (volatile void *)(uintptr_t)addr;
    switch (width) {
        case 1:  *(volatile uint8_t  *)p = (uint8_t)val;  break;
        case 2:  *(volatile uint16_t *)p = (uint16_t)val; break;
        default: *(volatile uint32_t *)p = val;           break;
    }
}

static dvm_status_t mmio_gate(run_t *st, uint64_t addr, uint32_t width, int write,
                              const char *what) {
    if (addr + width < addr || addr + width > MMIO_TOP)
        return trap(st, DVM_TRAP_RANGE,
                    "%s: address 0x%lx is above the mapped 4 GiB of physical space",
                    what, (unsigned long)addr);
    if (addr < kernel_top())
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: address 0x%lx is kernel memory (below 0x%lx: the VGA text "
                    "buffer, the kernel image and the heap live there)",
                    what, (unsigned long)addr, (unsigned long)kernel_top());
    /* The platform block: IOAPIC, HPET, local APIC. Interrupt routing lives
     * here. No BAR belongs in it, so nothing legitimate is being refused, and a
     * caller that over-granted a window is not able to hand it out by accident.
     * tools/dvm_tools.c refuses to derive a window here too; this is the same
     * rule one layer down, where every caller of dvm_policy_allow_mmio() gets
     * it whether it thought about the question or not. */
    if (addr + width > DVM_PLATFORM_LO && addr < DVM_PLATFORM_HI)
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: address 0x%lx is in the platform block (0x%lx-0x%lx: "
                    "IOAPIC, HPET and the local APIC). Interrupt routing is not "
                    "reachable from a driver program",
                    what, (unsigned long)addr,
                    (unsigned long)DVM_PLATFORM_LO, (unsigned long)DVM_PLATFORM_HI - 1);

    const dvm_mmio_win_t *w = NULL;
    for (int i = 0; i < st->pol->nmmio; i++) {
        const dvm_mmio_win_t *c = &st->pol->mmio[i];
        if (addr >= c->base && addr + width <= c->base + c->size) { w = c; break; }
    }
    if (!w) {
        char g[MMIO_GRANTS_CAP];
        sb_t s; sb_init(&s, g, sizeof g);
        put_mmio_grants(&s, st->pol);
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: address 0x%lx+%lu is not inside any allowed window (%s)",
                    what, (unsigned long)addr, (unsigned long)width, g);
    }
    if (write && !(w->flags & DVM_MMIO_W))
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: window 0x%lx+0x%lx is read-only",
                    what, (unsigned long)w->base, (unsigned long)w->size);
    if (!write && !(w->flags & DVM_MMIO_R))
        return trap(st, DVM_TRAP_MMIO_DENIED,
                    "%s: window 0x%lx+0x%lx is write-only",
                    what, (unsigned long)w->base, (unsigned long)w->size);
    if (addr % width)
        return trap(st, DVM_TRAP_ALIGN,
                    "%s: address 0x%lx is not %lu-byte aligned; device registers "
                    "require a naturally aligned access",
                    what, (unsigned long)addr, (unsigned long)width);
    if (st->res->io_ops >= st->pol->max_io)
        return trap(st, DVM_TRAP_IO_BUDGET,
                    "device access budget of %lu reached at %s",
                    (unsigned long)st->pol->max_io, what);
    st->res->io_ops++;
    return DVM_OK;
}

/* ====================================================================== */
/* scratch memory and the string family                                   */
/* ====================================================================== */

/* Bound one span against the arena. `len` may be 0, and a zero-length span at
 * DVM_MEM_SIZE is legal — that is the value every writing op returns, and
 * refusing to accept it back would make "the end offset" a trap waiting for the
 * program that filled the arena exactly. */
static dvm_status_t mem_span(run_t *st, uint64_t off, uint64_t len, const char *what) {
    if (off > DVM_MEM_SIZE || len > (uint64_t)DVM_MEM_SIZE - off)
        return trap(st, DVM_TRAP_MEM,
                    "%s: scratch offset 0x%lx+%lu runs outside the %u-byte arena "
                    "(valid offsets are 0-%u)",
                    what, (unsigned long)off, (unsigned long)len,
                    (unsigned)DVM_MEM_SIZE, (unsigned)DVM_MEM_SIZE - 1);
    return DVM_OK;
}

/* Charge `n` bytes of memory work. Called as the work happens, not before it,
 * so a search that runs away stops part-done instead of after. */
static dvm_status_t mem_charge(run_t *st, uint64_t n, const char *what) {
    if (st->res->mem_bytes + n < st->res->mem_bytes ||
        st->res->mem_bytes + n > st->pol->max_mem_bytes)
        return trap(st, DVM_TRAP_MEM_BUDGET,
                    "%s: the %lu-byte scratch memory budget is spent (%lu used). "
                    "Copy less, or search a shorter span",
                    what, (unsigned long)st->pol->max_mem_bytes,
                    (unsigned long)st->res->mem_bytes);
    st->res->mem_bytes += n;
    return DVM_OK;
}

/* Both of these are called only after mem_span() has proved the range, which is
 * why they index without checking. Little-endian and unaligned by design: see
 * include/dvm.h. */
static uint64_t mem_load(uint64_t off, uint32_t width) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < width; i++) v |= (uint64_t)dvm_mem[off + i] << (i * 8);
    return v;
}

static void mem_store(run_t *st, uint64_t off, uint32_t width, uint64_t val) {
    for (uint32_t i = 0; i < width; i++) dvm_mem[off + i] = (uint8_t)(val >> (i * 8));
    if (off + width > st->res->mem_hwm) st->res->mem_hwm = (uint32_t)(off + width);
}

/* ====================================================================== */
/* syscalls                                                               */
/* ====================================================================== */

/* Build a NUL-terminated, kernel-owned path out of a scratch span, applying
 * every rule in one place: bounds, length, printability, "no . or .." and the
 * policy's fs_root. The backend never sees a byte that has not been through
 * here. Returns DVM_OK, or a trap already reported. */
static dvm_status_t sys_path(run_t *st, uint64_t off, uint64_t len,
                             char *out, const char *what) {
    dvm_status_t rc = mem_span(st, off, len, what);
    if (rc != DVM_OK) return rc;
    rc = mem_charge(st, len, what);
    if (rc != DVM_OK) return rc;

    if (len >= DVM_PATH_MAX)
        return trap(st, DVM_TRAP_SYS_DENIED,
                    "%s: the path is %lu bytes; the limit is %d",
                    what, (unsigned long)len, DVM_PATH_MAX - 1);
    for (uint64_t i = 0; i < len; i++) out[i] = (char)dvm_mem[off + i];
    out[len] = '\0';

    char why[DVM_MSG_MAX];
    if (path_ok(out, (size_t)len, why, sizeof why) != 0) {
        /* The path itself is model bytes, so it is NOT echoed raw into a trace
         * line; path_ok has already refused anything unprintable, but the
         * message is built from its verdict rather than from the path. */
        return trap(st, DVM_TRAP_SYS_DENIED, "%s: %s", what, why);
    }

    const char *root = st->pol->fs_root;
    size_t rl = 0;
    while (rl < DVM_PATH_MAX && root[rl]) rl++;
    /* dvm_policy_check() has already refused an fs syscall with no root, so rl
     * is non-zero here; the test is kept because this function is one call away
     * from the filesystem and a defence that costs a compare is worth its line. */
    if (!rl)
        return trap(st, DVM_TRAP_SYS_DENIED,
                    "%s: no filesystem subtree has been granted to this program", what);
    int under = 1;
    for (size_t i = 0; i < rl; i++)
        if (out[i] != root[i]) { under = 0; break; }
    /* "/vm" must match "/vm/notes" but not "/vmother". A root of "/" matches
     * everything, and is the one case where no separator follows it. */
    if (under && rl > 1 && out[rl] != '/' && out[rl] != '\0') under = 0;
    if (!under)
        return trap(st, DVM_TRAP_SYS_DENIED,
                    "%s: \"%s\" is outside \"%s\", the only subtree this program may "
                    "name", what, out, root);
    return DVM_OK;
}

/* Record a syscall's failure text once, where the caller can find it. */
static void sys_fail(run_t *st, const char *name, int64_t code, const char *why) {
    char line[DVM_MSG_MAX];
    snprintf(line, sizeof line, "%s: %s", name, why && why[0] ? why : "failed");
    copy_printable(st->res->sys_msg, sizeof st->res->sys_msg, line);
    if (tracing(st, DVM_TRACE_IO))
        tline(st, "sys %s failed rc=%ld: %s", name, (long)code, st->res->sys_msg);
}

/* One syscall. `args` holds arity() values in the order they were pushed.
 * Everything a backend receives has been resolved and bounded here. */
static dvm_status_t sys_call(run_t *st, dvm_sys_nr_t nr, const uint64_t *args,
                             uint64_t *out, int *ok) {
    const dvm_sys_t *sys = st->io->sys;
    const char      *name = dvm_sys_name(nr);
    char             err[DVM_MSG_MAX];
    int64_t          rc = -1;
    dvm_status_t     s;

    err[0] = '\0';
    *ok = 0;
    *out = 0;

    switch (nr) {
    case DVM_SYS_CON_WRITE: {
        uint64_t off = args[0], len = args[1];
        if ((s = mem_span(st, off, len, "con.write")) != DVM_OK) return s;
        if (len > DVM_SAY_MAX)
            return trap(st, DVM_TRAP_SYS_DENIED,
                        "con.write: %lu bytes is more than the %d a console line may "
                        "carry; split it", (unsigned long)len, DVM_SAY_MAX);
        if ((s = mem_charge(st, len, "con.write")) != DVM_OK) return s;
        if (st->res->prints >= st->pol->max_prints)
            return trap(st, DVM_TRAP_PRINT_BUDGET,
                        "print budget of %lu lines reached at con.write",
                        (unsigned long)st->pol->max_prints);
        if (!sys->con_write)
            return trap(st, DVM_TRAP_NOIO, "con.write: this VM has no console backend");
        /* Sanitised HERE, before the kernel sees it. A program's bytes must not
         * be able to put a '[' in column zero, and the only way to be sure is
         * that the kernel is handed printable text and prints it through
         * trace.c's own prefix. */
        char text[DVM_SAY_MAX + 1];
        size_t n = 0;
        for (uint64_t i = 0; i < len; i++) {
            unsigned char c = dvm_mem[off + i];
            text[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        text[n] = '\0';
        st->res->prints++;
        rc = sys->con_write(sys->ctx, text, n, err, sizeof err);
        break;
    }

    case DVM_SYS_FS_READ: {
        char path[DVM_PATH_MAX];
        uint64_t dst = args[2], cap = args[3];
        if ((s = sys_path(st, args[0], args[1], path, "fs.read")) != DVM_OK) return s;
        if ((s = mem_span(st, dst, cap, "fs.read")) != DVM_OK) return s;
        if ((s = mem_charge(st, cap, "fs.read")) != DVM_OK) return s;
        if (!sys->fs_read)
            return trap(st, DVM_TRAP_NOIO, "fs.read: this VM has no filesystem backend");
        rc = sys->fs_read(sys->ctx, path, dvm_mem + dst, (size_t)cap, err, sizeof err);
        if (rc > 0) {
            /* A backend that reports more than it was given room for has
             * already overrun the arena, so this cannot undo it — but it is a
             * KERNEL bug, not a program error, and it stops the run loudly
             * instead of handing the program a length it can trust. */
            if ((uint64_t)rc > cap)
                return trap(st, DVM_TRAP_NOIO,
                            "fs.read: the filesystem returned %ld bytes into a %lu-byte "
                            "buffer; that is a kernel bug, not a problem with this "
                            "program", (long)rc, (unsigned long)cap);
            if (dst + (uint64_t)rc > st->res->mem_hwm)
                st->res->mem_hwm = (uint32_t)(dst + (uint64_t)rc);
        }
        break;
    }

    case DVM_SYS_FS_WRITE: {
        char path[DVM_PATH_MAX];
        uint64_t src = args[2], len = args[3];
        if ((s = sys_path(st, args[0], args[1], path, "fs.write")) != DVM_OK) return s;
        if ((s = mem_span(st, src, len, "fs.write")) != DVM_OK) return s;
        if ((s = mem_charge(st, len, "fs.write")) != DVM_OK) return s;
        if (!sys->fs_write)
            return trap(st, DVM_TRAP_NOIO, "fs.write: this VM has no filesystem backend");
        rc = sys->fs_write(sys->ctx, path, dvm_mem + src, (size_t)len, err, sizeof err);
        break;
    }

    case DVM_SYS_FS_SIZE: {
        char path[DVM_PATH_MAX];
        if ((s = sys_path(st, args[0], args[1], path, "fs.size")) != DVM_OK) return s;
        if (!sys->fs_size)
            return trap(st, DVM_TRAP_NOIO, "fs.size: this VM has no filesystem backend");
        rc = sys->fs_size(sys->ctx, path, err, sizeof err);
        break;
    }

    case DVM_SYS_AUDIO_TONE: {
        uint64_t hz = args[0], ms = args[1];
        /* Ranges checked here rather than in audio.c so that a program gets the
         * numbers back in its own terms, and so that the check exists on a host
         * build where core/audio.c is not linked. */
        if (hz < 20 || hz > 20000)
            return trap(st, DVM_TRAP_SYS_DENIED,
                        "audio.tone: %lu Hz is outside 20-20000", (unsigned long)hz);
        if (ms < 1 || ms > 5000)
            return trap(st, DVM_TRAP_SYS_DENIED,
                        "audio.tone: %lu ms is outside 1-5000", (unsigned long)ms);
        if (!sys->audio_tone)
            return trap(st, DVM_TRAP_NOIO, "audio.tone: this VM has no audio backend");
        rc = sys->audio_tone(sys->ctx, (uint32_t)hz, (uint32_t)ms, err, sizeof err);
        break;
    }

    case DVM_SYS_TIME_MS:
        if (!sys->time_ms)
            return trap(st, DVM_TRAP_NOIO, "time.ms: this VM has no clock backend");
        rc = sys->time_ms(sys->ctx, err, sizeof err);
        break;

    default:
        return trap(st, DVM_TRAP_BADOP, "sys: syscall %d is not implemented", (int)nr);
    }

    if (rc < 0) {
        sys_fail(st, name, rc, err);
        /* The error code, not the result, and the zero flag says which. A
         * negative return is a fact about the world (no such file), not a
         * broken program, so it is the program's to branch on. */
        uint64_t mag = (rc == (int64_t)0x8000000000000000ll)
                           ? 0x8000000000000000ull : (uint64_t)(-rc);
        *out = mag;
        return DVM_OK;
    }
    *ok  = 1;
    *out = (uint64_t)rc;
    return DVM_OK;
}

/* Opcodes that write a register AND emit their own trace line, so the generic
 * "op rN=value" line at the bottom of the loop would be a duplicate. */
static int traced_itself(dvm_op_t op) {
    switch (op) {
        case DVM_IN8: case DVM_IN16: case DVM_IN32:
        case DVM_LD8: case DVM_LD16: case DVM_LD32:
        case DVM_MLD8: case DVM_MLD16: case DVM_MLD32: case DVM_MLD64:
        case DVM_MATOI: case DVM_SYS: case DVM_PCICFG:
            return 1;
        default:
            return 0;
    }
}

/* ---- the loop ---- */

static dvm_status_t execute(run_t *st) {
    const dvm_program_t *p   = st->p;
    const dvm_policy_t  *pol = st->pol;
    dvm_result_t        *res = st->res;

    for (;;) {
        if (res->steps >= pol->max_steps) {
            st->line = (st->pc < p->ninsn) ? p->insn[st->pc].line : 0;
            return trap(st, DVM_TRAP_STEPS,
                        "instruction budget of %lu exhausted; the program did not "
                        "reach halt (an unterminated loop?)",
                        (unsigned long)pol->max_steps);
        }
        if (st->pc >= p->ninsn) {
            st->line = p->insn[p->ninsn - 1].line;
            return trap(st, DVM_TRAP_PC,
                        "ran past the last instruction; every path must end in halt "
                        "or abort");
        }

        const dvm_insn_t *in = &p->insn[st->pc];
        st->line = in->line;
        res->steps++;
        uint32_t next = st->pc + 1;

        uint64_t a = rd(st, in, 0), b = rd(st, in, 1), c = rd(st, in, 2);
        uint64_t v = 0;
        int      is_alu = 0;

        switch ((dvm_op_t)in->op) {

        /* ---- control ---- */
        case DVM_HALT:
            res->status = DVM_OK;
            res->pc = st->pc;
            res->msg[0] = '\0';
            return DVM_OK;

        case DVM_ABORT: {
            /* Sanitised HERE and not only inside trap(): trap() copy_printable()s
             * what lands in dvm_result_t.msg, but it hands the raw formatted
             * reason to trace_err(), which would escape a control byte as \x0a
             * rather than fold it to '?'. Doing it first makes the trap line and
             * the returned message read identically. */
            char msg[DVM_MSG_MAX];
            copy_printable(msg, sizeof msg, str_at(p, in->val[0]));
            return trap(st, DVM_TRAP_ABORT, "%s", msg);
        }

        case DVM_NOP:
            if (tracing(st, DVM_TRACE_ALL)) tline(st, "pc=%03lu ln=%lu nop",
                                                  (unsigned long)st->pc, (unsigned long)st->line);
            break;

        /* ---- data / ALU ---- */
        case DVM_MOV: v = a;      is_alu = 1; break;
        case DVM_ADD: v = a + b;  is_alu = 1; break;
        case DVM_SUB: v = a - b;  is_alu = 1; break;
        case DVM_MUL: v = a * b;  is_alu = 1; break;
        case DVM_DIV:
            if (!b) return trap(st, DVM_TRAP_DIV0, "div by zero");
            v = a / b; is_alu = 1; break;
        case DVM_MOD:
            if (!b) return trap(st, DVM_TRAP_DIV0, "mod by zero");
            v = a % b; is_alu = 1; break;
        case DVM_AND: v = a & b;  is_alu = 1; break;
        case DVM_OR:  v = a | b;  is_alu = 1; break;
        case DVM_XOR: v = a ^ b;  is_alu = 1; break;
        case DVM_NOT: v = ~a;     is_alu = 1; break;
        case DVM_SHL: v = a << (b & 63); is_alu = 1; break;
        case DVM_SHR: v = a >> (b & 63); is_alu = 1; break;

        case DVM_CMP:
            st->zero  = (a == b);
            st->below = (a < b);
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu cmp 0x%lx,0x%lx eq=%d below=%d",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)a, (unsigned long)b, st->zero, st->below);
            break;

        case DVM_TEST:
            st->zero  = ((a & b) == 0);
            st->below = 0;
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu test 0x%lx&0x%lx zero=%d",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)a, (unsigned long)b, st->zero);
            break;

        /* ---- branches ---- */
        case DVM_JMP: case DVM_BEQ: case DVM_BNE:
        case DVM_BLT: case DVM_BLE: case DVM_BGT: case DVM_BGE: {
            int take = 0;
            switch ((dvm_op_t)in->op) {
                case DVM_JMP: take = 1; break;
                case DVM_BEQ: take = st->zero; break;
                case DVM_BNE: take = !st->zero; break;
                case DVM_BLT: take = st->below; break;
                case DVM_BLE: take = st->below || st->zero; break;
                case DVM_BGT: take = !st->below && !st->zero; break;
                case DVM_BGE: take = !st->below; break;
                default: break;
            }
            uint64_t target = in->val[0];
            if (take) {
                if (in->kind[0] != DVM_O_PC || target >= p->ninsn)
                    return trap(st, DVM_TRAP_PC,
                                "%s: branch target %lu is outside the program (0-%lu)",
                                dvm_op_name(in->op), (unsigned long)target,
                                (unsigned long)p->ninsn - 1);
                next = (uint32_t)target;
            }
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu %s %s pc->%03lu",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      dvm_op_name(in->op), take ? "taken" : "not-taken",
                      (unsigned long)next);
            break;
        }

        case DVM_CALL:
            if (st->csp >= DVM_CSTACK)
                return trap(st, DVM_TRAP_STACK,
                            "call stack overflow (depth %d); recursion is bounded",
                            DVM_CSTACK);
            if (in->kind[0] != DVM_O_PC || in->val[0] >= p->ninsn)
                return trap(st, DVM_TRAP_PC, "call: target %lu is outside the program",
                            (unsigned long)in->val[0]);
            st->cstack[st->csp++] = next;
            next = (uint32_t)in->val[0];
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu call pc->%03lu depth=%lu",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)next, (unsigned long)st->csp);
            break;

        case DVM_RET:
            if (st->csp == 0)
                return trap(st, DVM_TRAP_STACK, "ret with an empty call stack");
            next = st->cstack[--st->csp];
            if (next > p->ninsn)
                return trap(st, DVM_TRAP_PC, "ret: return address %lu is outside the program",
                            (unsigned long)next);
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu ret pc->%03lu depth=%lu",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)next, (unsigned long)st->csp);
            break;

        case DVM_PUSH:
            if (st->dsp >= DVM_DSTACK)
                return trap(st, DVM_TRAP_STACK, "data stack overflow (depth %d)", DVM_DSTACK);
            st->dstack[st->dsp++] = a;
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu push 0x%lx depth=%lu",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)a, (unsigned long)st->dsp);
            break;

        case DVM_POP:
            if (st->dsp == 0)
                return trap(st, DVM_TRAP_STACK, "pop from an empty data stack");
            v = st->dstack[--st->dsp];
            is_alu = 1;
            break;

        /* ---- port I/O ---- */
        case DVM_OUT8: case DVM_OUT16: case DVM_OUT32: {
            uint32_t width = in->op == DVM_OUT8 ? 1 : in->op == DVM_OUT16 ? 2 : 4;
            const char *mn = dvm_op_name(in->op);
            dvm_status_t rc = port_gate(st, a, width, mn);
            if (rc != DVM_OK) return rc;
            if (!st->io->port_write)
                return trap(st, DVM_TRAP_NOIO, "%s: this VM has no port I/O backend", mn);
            uint32_t val = (uint32_t)(width == 1 ? (b & 0xFF) : width == 2 ? (b & 0xFFFF)
                                                              : (b & 0xFFFFFFFFu));
            st->io->port_write(st->io->ctx, (uint16_t)a, (uint8_t)width, val);
            res->n_port_wr++;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu %s port=0x%04lx val=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)a, (unsigned long)val);
            break;
        }

        case DVM_IN8: case DVM_IN16: case DVM_IN32: {
            uint32_t width = in->op == DVM_IN8 ? 1 : in->op == DVM_IN16 ? 2 : 4;
            const char *mn = dvm_op_name(in->op);
            dvm_status_t rc = port_gate(st, a, width, mn);
            if (rc != DVM_OK) return rc;
            if (!st->io->port_read)
                return trap(st, DVM_TRAP_NOIO, "%s: this VM has no port I/O backend", mn);
            v = st->io->port_read(st->io->ctx, (uint16_t)a, (uint8_t)width);
            if (width == 1) v &= 0xFF; else if (width == 2) v &= 0xFFFF;
            res->n_port_rd++;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu %s port=0x%04lx r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)a, in->dst, (unsigned long)v);
            break;
        }

        /* ---- MMIO ---- */
        case DVM_LD8: case DVM_LD16: case DVM_LD32: {
            uint32_t width = in->op == DVM_LD8 ? 1 : in->op == DVM_LD16 ? 2 : 4;
            const char *mn = dvm_op_name(in->op);
            uint64_t addr = a + b;
            if (dma_touches(pol, addr, width)) {
                dvm_status_t rc = dma_gate(st, addr, width, mn);
                if (rc != DVM_OK) return rc;
                v = dma_load(addr, width);
                res->n_dma_rd++;
                is_alu = 1;
                /* DMA-buffer traffic traces only at TRACE_ALL: at TRACE_IO a
                 * buffer fill would spend the whole line budget and silence the
                 * device accesses that actually explain the run. */
                if (tracing(st, DVM_TRACE_ALL))
                    tline(st, "pc=%03lu ln=%lu %s dma+0x%lx r%u=0x%lx",
                          (unsigned long)st->pc, (unsigned long)st->line, mn,
                          (unsigned long)(addr - pol->dma_base), in->dst,
                          (unsigned long)v);
                break;
            }
            dvm_status_t rc = mmio_gate(st, addr, width, 0, mn);
            if (rc != DVM_OK) return rc;
            if (!st->io->mmio_read)
                return trap(st, DVM_TRAP_NOIO, "%s: this VM has no MMIO backend", mn);
            v = st->io->mmio_read(st->io->ctx, addr, (uint8_t)width);
            if (width == 1) v &= 0xFF; else if (width == 2) v &= 0xFFFF;
            res->n_mmio_rd++;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu %s addr=0x%lx r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)addr, in->dst, (unsigned long)v);
            break;
        }

        case DVM_ST8: case DVM_ST16: case DVM_ST32: {
            uint32_t width = in->op == DVM_ST8 ? 1 : in->op == DVM_ST16 ? 2 : 4;
            const char *mn = dvm_op_name(in->op);
            uint64_t addr = a + b;
            uint32_t val = (uint32_t)(width == 1 ? (c & 0xFF) : width == 2 ? (c & 0xFFFF)
                                                              : (c & 0xFFFFFFFFu));
            if (dma_touches(pol, addr, width)) {
                dvm_status_t rc = dma_gate(st, addr, width, mn);
                if (rc != DVM_OK) return rc;
                dma_store(addr, width, val);
                res->n_dma_wr++;
                if (tracing(st, DVM_TRACE_ALL))
                    tline(st, "pc=%03lu ln=%lu %s dma+0x%lx val=0x%lx",
                          (unsigned long)st->pc, (unsigned long)st->line, mn,
                          (unsigned long)(addr - pol->dma_base), (unsigned long)val);
                break;
            }
            dvm_status_t rc = mmio_gate(st, addr, width, 1, mn);
            if (rc != DVM_OK) return rc;
            if (!st->io->mmio_write)
                return trap(st, DVM_TRAP_NOIO, "%s: this VM has no MMIO backend", mn);
            st->io->mmio_write(st->io->ctx, addr, (uint8_t)width, val);
            res->n_mmio_wr++;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu %s addr=0x%lx val=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)addr, (unsigned long)val);
            break;
        }

        /* ---- PCI config read ---- */
        case DVM_PCICFG: {
            if (a > 0xFFFFu)
                return trap(st, DVM_TRAP_RANGE,
                            "pcicfg: bdf 0x%lx is out of range (bus<<8 | dev<<3 | fn)",
                            (unsigned long)a);
            uint8_t bus = (uint8_t)((a >> 8) & 0xFF);
            uint8_t dev = (uint8_t)((a >> 3) & 0x1F);
            uint8_t fn  = (uint8_t)(a & 7);
            if (b > 0xFF)
                return trap(st, DVM_TRAP_RANGE,
                            "pcicfg: offset 0x%lx is outside the 256-byte config space",
                            (unsigned long)b);
            int ok = 0;
            for (int i = 0; i < pol->npci; i++)
                if (pol->pci[i].bus == bus && pol->pci[i].dev == dev && pol->pci[i].fn == fn) {
                    ok = 1; break;
                }
            if (!ok)
                return trap(st, DVM_TRAP_PCI_DENIED,
                            "pcicfg: %02x:%02x.%x is not one of the %d function(s) this "
                            "program may read", bus, dev, fn, pol->npci);
            if (b & 3)
                return trap(st, DVM_TRAP_ALIGN,
                            "pcicfg: offset 0x%02lx must be 4-byte aligned; config space is "
                            "read a dword at a time", (unsigned long)b);
            if (res->io_ops >= pol->max_io)
                return trap(st, DVM_TRAP_IO_BUDGET,
                            "device access budget of %lu reached at pcicfg",
                            (unsigned long)pol->max_io);
            if (!st->io->pci_read32)
                return trap(st, DVM_TRAP_NOIO, "pcicfg: this VM has no PCI backend");
            res->io_ops++;
            v = st->io->pci_read32(st->io->ctx, bus, dev, fn, (uint8_t)b);
            res->n_pci_rd++;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu pcicfg %02x:%02x.%x off=0x%02lx r%u=0x%08lx",
                      (unsigned long)st->pc, (unsigned long)st->line, bus, dev, fn,
                      (unsigned long)b, in->dst, (unsigned long)v);
            break;
        }

        /* ---- scratch memory ---- */
        case DVM_MLD8: case DVM_MLD16: case DVM_MLD32: case DVM_MLD64: {
            uint32_t width = 1u << (in->op - DVM_MLD8);
            const char *mn = dvm_op_name(in->op);
            uint64_t off = a + b;
            if (off < a)                       /* the operand sum wrapped */
                return trap(st, DVM_TRAP_MEM,
                            "%s: the address 0x%lx+0x%lx wraps past 2^64",
                            mn, (unsigned long)a, (unsigned long)b);
            dvm_status_t rc = mem_span(st, off, width, mn);
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, width, mn);
            if (rc != DVM_OK) return rc;
            v = mem_load(off, width);
            res->n_mem_rd++;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu %s m+0x%lx r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)off, in->dst, (unsigned long)v);
            break;
        }

        case DVM_MST8: case DVM_MST16: case DVM_MST32: case DVM_MST64: {
            uint32_t width = 1u << (in->op - DVM_MST8);
            const char *mn = dvm_op_name(in->op);
            uint64_t off = a + b;
            if (off < a)
                return trap(st, DVM_TRAP_MEM,
                            "%s: the address 0x%lx+0x%lx wraps past 2^64",
                            mn, (unsigned long)a, (unsigned long)b);
            dvm_status_t rc = mem_span(st, off, width, mn);
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, width, mn);
            if (rc != DVM_OK) return rc;
            mem_store(st, off, width, c);
            res->n_mem_wr++;
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu %s m+0x%lx val=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line, mn,
                      (unsigned long)off, (unsigned long)c);
            break;
        }

        /* ---- strings ---- */
        case DVM_MSTR: {
            const char *text = str_at(p, in->val[0]);
            uint64_t n = 0;
            while (text[n]) n++;              /* the pool is NUL-terminated */
            uint64_t dst = b;
            dvm_status_t rc = mem_span(st, dst, n, "mstr");
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, n, "mstr");
            if (rc != DVM_OK) return rc;
            for (uint64_t i = 0; i < n; i++) dvm_mem[dst + i] = (uint8_t)text[i];
            if (dst + n > res->mem_hwm) res->mem_hwm = (uint32_t)(dst + n);
            res->n_mem_wr++;
            v = dst + n;
            is_alu = 1;
            break;
        }

        case DVM_MCPY: {
            uint64_t dst = a, src = b, n = c;
            dvm_status_t rc = mem_span(st, dst, n, "mcpy");
            if (rc != DVM_OK) return rc;
            rc = mem_span(st, src, n, "mcpy");
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, n * 2, "mcpy");
            if (rc != DVM_OK) return rc;
            /* Overlap-safe, in both directions. Building a message in place is
             * exactly the case that overlaps, and a copy that corrupts itself
             * when two spans touch would be a trap for the one program shape
             * this family exists to make easy. */
            if (dst < src)
                for (uint64_t i = 0; i < n; i++) dvm_mem[dst + i] = dvm_mem[src + i];
            else
                for (uint64_t i = n; i > 0; i--) dvm_mem[dst + i - 1] = dvm_mem[src + i - 1];
            if (dst + n > res->mem_hwm) res->mem_hwm = (uint32_t)(dst + n);
            res->n_mem_rd++;
            res->n_mem_wr++;
            v = dst + n;
            is_alu = 1;
            break;
        }

        case DVM_MSET: {
            uint64_t dst = a, n = c;
            dvm_status_t rc = mem_span(st, dst, n, "mset");
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, n, "mset");
            if (rc != DVM_OK) return rc;
            for (uint64_t i = 0; i < n; i++) dvm_mem[dst + i] = (uint8_t)b;
            if (dst + n > res->mem_hwm) res->mem_hwm = (uint32_t)(dst + n);
            res->n_mem_wr++;
            v = dst + n;
            is_alu = 1;
            break;
        }

        case DVM_MCMP: {
            uint64_t x = a, y = b, n = c;
            dvm_status_t rc = mem_span(st, x, n, "mcmp");
            if (rc != DVM_OK) return rc;
            rc = mem_span(st, y, n, "mcmp");
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, n * 2, "mcmp");
            if (rc != DVM_OK) return rc;
            st->zero = 1; st->below = 0;
            for (uint64_t i = 0; i < n; i++) {
                uint8_t u = dvm_mem[x + i], w = dvm_mem[y + i];
                if (u != w) { st->zero = 0; st->below = (u < w); break; }
            }
            res->n_mem_rd++;
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu mcmp %lu bytes eq=%d below=%d",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)n, st->zero, st->below);
            break;
        }

        case DVM_MFIND: {
            const char *needle = str_at(p, in->val[0]);
            uint64_t nl = 0;
            while (needle[nl]) nl++;
            uint64_t hay = b, n = c;
            dvm_status_t rc = mem_span(st, hay, n, "mfind");
            if (rc != DVM_OK) return rc;
            uint64_t at = hay + n;             /* "not found" is the end offset */
            st->zero = 0;
            /* An empty needle matches at the start: that is what every other
             * search on earth does, and a program that computed a zero length
             * should get a defined answer rather than a special case. */
            if (!nl) { at = hay; st->zero = 1; }
            else if (nl <= n) {
                for (uint64_t i = 0; i + nl <= n; i++) {
                    rc = mem_charge(st, 1, "mfind");
                    if (rc != DVM_OK) return rc;
                    if (dvm_mem[hay + i] != (uint8_t)needle[0]) continue;
                    uint64_t k = 1;
                    while (k < nl && dvm_mem[hay + i + k] == (uint8_t)needle[k]) k++;
                    rc = mem_charge(st, k - 1, "mfind");
                    if (rc != DVM_OK) return rc;
                    if (k == nl) { at = hay + i; st->zero = 1; break; }
                }
            }
            res->n_mem_rd++;
            v = at;
            is_alu = 1;
            break;
        }

        case DVM_MCHR: {
            uint64_t hay = a, n = b;
            dvm_status_t rc = mem_span(st, hay, n, "mchr");
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, n, "mchr");
            if (rc != DVM_OK) return rc;
            uint8_t want = (uint8_t)c;
            uint64_t at = hay + n;
            st->zero = 0;
            for (uint64_t i = 0; i < n; i++)
                if (dvm_mem[hay + i] == want) { at = hay + i; st->zero = 1; break; }
            res->n_mem_rd++;
            v = at;
            is_alu = 1;
            break;
        }

        case DVM_MATOI: {
            uint64_t off = a, n = b, base = c;
            if (base < 2 || base > 16)
                return trap(st, DVM_TRAP_RANGE,
                            "matoi: base %lu is outside 2-16", (unsigned long)base);
            dvm_status_t rc = mem_span(st, off, n, "matoi");
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, n, "matoi");
            if (rc != DVM_OK) return rc;
            /* No sign, no whitespace skipping, no prefix. Digits are consumed
             * until one is not a digit in this base, and the flag says whether
             * there was at least one and whether it all fitted. A parser that
             * quietly returns 0 for "abc" is how a status code becomes a lie. */
            uint64_t acc = 0;
            uint64_t digits = 0;
            int overflow = 0;
            for (uint64_t i = 0; i < n; i++) {
                char ch = dlower((char)dvm_mem[off + i]);
                unsigned d;
                if (ch >= '0' && ch <= '9')      d = (unsigned)(ch - '0');
                else if (ch >= 'a' && ch <= 'f') d = (unsigned)(ch - 'a' + 10);
                else break;
                if (d >= base) break;
                if (acc > (0xFFFFFFFFFFFFFFFFull - d) / base) { overflow = 1; break; }
                acc = acc * base + d;
                digits++;
            }
            st->zero  = (digits > 0 && !overflow);
            st->below = 0;
            v = st->zero ? acc : 0;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_ALL))
                tline(st, "pc=%03lu ln=%lu matoi m+0x%lx digits=%lu ok=%d r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)off, (unsigned long)digits, st->zero,
                      in->dst, (unsigned long)v);
            break;
        }

        case DVM_MITOA: {
            uint64_t dst = a, val = b, base = c;
            if (base < 2 || base > 16)
                return trap(st, DVM_TRAP_RANGE,
                            "mitoa: base %lu is outside 2-16", (unsigned long)base);
            char tmp[64];
            int  n = 0;
            do {
                unsigned d = (unsigned)(val % base);
                tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                val /= base;
            } while (val && n < (int)sizeof tmp);
            dvm_status_t rc = mem_span(st, dst, (uint64_t)n, "mitoa");
            if (rc != DVM_OK) return rc;
            rc = mem_charge(st, (uint64_t)n, "mitoa");
            if (rc != DVM_OK) return rc;
            for (int i = 0; i < n; i++) dvm_mem[dst + i] = (uint8_t)tmp[n - 1 - i];
            if (dst + (uint64_t)n > res->mem_hwm) res->mem_hwm = (uint32_t)(dst + n);
            res->n_mem_wr++;
            v = dst + (uint64_t)n;
            is_alu = 1;
            break;
        }

        /* ---- the kernel ---- */
        case DVM_SYS: {
            dvm_sys_nr_t nr = (dvm_sys_nr_t)in->val[0];
            int arity = dvm_sys_arity(nr);
            if (arity < 0)
                return trap(st, DVM_TRAP_BADOP, "sys: %lu is not a syscall",
                            (unsigned long)in->val[0]);
            if (!(pol->sys_allow & ((uint32_t)1u << (unsigned)nr))) {
                char g[128];
                sb_t gs; sb_init(&gs, g, sizeof g);
                int any = 0;
                for (int i = 0; i < DVM_SYS__COUNT; i++)
                    if (pol->sys_allow & ((uint32_t)1u << i))
                        { sb_addf(&gs, "%s%s", any++ ? "," : "",
                                  dvm_sys_name((dvm_sys_nr_t)i)); }
                if (!any) sb_addf(&gs, "none");
                return trap(st, DVM_TRAP_SYS_DENIED,
                            "sys %s: this program was not granted that syscall (it has %s)",
                            dvm_sys_name(nr), g);
            }
            if (res->n_sys >= pol->max_sys)
                return trap(st, DVM_TRAP_SYS_BUDGET,
                            "syscall budget of %lu reached at sys %s",
                            (unsigned long)pol->max_sys, dvm_sys_name(nr));
            if (!st->io->sys)
                return trap(st, DVM_TRAP_NOIO,
                            "sys %s: this VM has no kernel backend", dvm_sys_name(nr));
            if (st->dsp < (uint32_t)arity)
                return trap(st, DVM_TRAP_STACK,
                            "sys %s takes %d argument(s) pushed onto the data stack, "
                            "and only %lu are on it",
                            dvm_sys_name(nr), arity, (unsigned long)st->dsp);
            if (arity > DVM_SYS_MAXARGS)
                return trap(st, DVM_TRAP_BADOP,
                            "sys %s: its arity of %d is past this VM's %d-argument "
                            "limit, which is a kernel bug",
                            dvm_sys_name(nr), arity, DVM_SYS_MAXARGS);
            uint64_t sargs[DVM_SYS_MAXARGS];
            for (int i = 0; i < DVM_SYS_MAXARGS; i++) sargs[i] = 0;
            for (int i = arity - 1; i >= 0; i--) sargs[i] = st->dstack[--st->dsp];
            res->n_sys++;
            int okflag = 0;
            dvm_status_t rc = sys_call(st, nr, sargs, &v, &okflag);
            if (rc != DVM_OK) return rc;
            st->zero  = okflag;
            st->below = 0;
            is_alu = 1;
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu sys %s %s r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      dvm_sys_name(nr), okflag ? "ok" : "FAILED",
                      in->dst, (unsigned long)v);
            break;
        }

        /* ---- delay ---- */
        case DVM_DELAY: {
            if (a > pol->max_single_delay_us)
                return trap(st, DVM_TRAP_DELAY_BUDGET,
                            "delay of %lu us exceeds the %lu us single-delay limit",
                            (unsigned long)a, (unsigned long)pol->max_single_delay_us);
            if (res->delay_us + a > pol->max_delay_us)
                return trap(st, DVM_TRAP_DELAY_BUDGET,
                            "delay budget exhausted: %lu us already spent, %lu us "
                            "requested, %lu us allowed",
                            (unsigned long)res->delay_us, (unsigned long)a,
                            (unsigned long)pol->max_delay_us);
            if (!st->io->delay_us)
                return trap(st, DVM_TRAP_NOIO, "delay: this VM has no timer backend");
            res->delay_us += a;
            st->io->delay_us(st->io->ctx, (uint32_t)a);
            if (tracing(st, DVM_TRACE_IO))
                tline(st, "pc=%03lu ln=%lu delay %lu us (total %lu)",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      (unsigned long)a, (unsigned long)res->delay_us);
            break;
        }

        /* ---- print ---- */
        case DVM_PRINT: {
            if (res->prints >= pol->max_prints)
                return trap(st, DVM_TRAP_PRINT_BUDGET,
                            "print budget of %lu lines reached",
                            (unsigned long)pol->max_prints);
            res->prints++;
            const char *text = str_at(p, in->val[0]);
            if (pol->trace != DVM_TRACE_OFF && trace_budget(st)) {
                if (in->kind[1] != DVM_O_NONE)
                    trace_ok("dvm.print", "%s = 0x%lx", text, (unsigned long)b);
                else
                    trace_ok("dvm.print", "%s", text);
            }
            break;
        }

        default:
            return trap(st, DVM_TRAP_BADOP, "opcode %u is not implemented", in->op);
        }

        if (is_alu) {
            if (in->dst >= DVM_NREGS)
                return trap(st, DVM_TRAP_RANGE, "%s: destination register r%u does not exist",
                            dvm_op_name(in->op), in->dst);
            st->reg[in->dst] = v;
            /* Anything that already printed its own line above — every I/O
             * read, and the memory ops whose interesting part is the offset or
             * the flag rather than the value — is skipped here. */
            if (tracing(st, DVM_TRACE_ALL) && !traced_itself((dvm_op_t)in->op))
                tline(st, "pc=%03lu ln=%lu %s r%u=0x%lx",
                      (unsigned long)st->pc, (unsigned long)st->line,
                      dvm_op_name(in->op), in->dst, (unsigned long)v);
        }

        st->pc = next;
    }
}

dvm_status_t dvm_run(const dvm_program_t *p, const dvm_policy_t *pol,
                     const dvm_io_t *io, const uint64_t *args, int nargs,
                     dvm_result_t *res) {
    if (!res) return DVM_TRAP_POLICY;
    dz(res, sizeof *res);

    /* ONE PROGRAM AT A TIME, ENFORCED — not merely intended.
     *
     * Two pieces of per-machine state are owned by "the run in progress" and
     * there is exactly one copy of each: the scratch arena `dvm_mem`, and the
     * DMA guard bands. Both are (re)initialised a few dozen lines below, at
     * entry, before the first instruction executes. So a SECOND dvm_run()
     * starting while a first is still on the stack does not merely share them —
     * it destroys the first one's, silently, mid-execution:
     *
     *   - dz(dvm_mem, ...) erases the outer program's working store. Every
     *     later mld/mcmp/mfind reads zeros, and nothing traps. include/dvm.h
     *     promises the model the exact opposite ("the only ways bytes get in
     *     are the program's own instructions and a syscall's output"); a
     *     syscall was silently taking every byte OUT. put_scratch() in
     *     tools/dvm_tools.c sizes its dump from mem_hwm, which still records
     *     the outer program's pre-syscall stores, so the tool result reported
     *     the program's answer as that many NUL bytes — the kernel lying to the
     *     model about what its own program computed.
     *   - dma_guard_arm() re-poisons both guard bands. Any overrun the outer
     *     program's device had ALREADY committed was wiped before the caller's
     *     dvm_dma_check() ran. That is worse than losing a diagnostic:
     *     tools/dvm_tools.c computes `converged = (st == DVM_OK) && did_work &&
     *     !dma_overrun` precisely so that a program corrupting memory spends an
     *     attempt instead of clearing its budget. With the evidence erased,
     *     dma_overrun was always 0, so the one program that must not get
     *     unlimited retries got them, and neither the model nor the
     *     [driver_run.dma ...] trace line ever mentioned the overrun.
     *
     * This was reachable, not theoretical, and by the project's intended path:
     * driver_run grants DVM_SYS_AUDIO_TONE to every program it runs, and once
     * the model has installed its own audio driver, `sys audio.tone` goes
     * k_audio_tone -> audio_tone() -> play_buffer() -> run_vm() -> dvm_run().
     * driver_install strips sys_allow from the resident play program, so nesting
     * could only ever be one deep — but one deep is all it took.
     *
     * The fix is to make the documented invariant true rather than to make the
     * shared state re-entrant. Saving and restoring 64 KiB of scratch plus two
     * 64 KiB guard bands per nesting level would cost more .bss than the arena
     * itself, and it would still leave the deeper problem: the inner program's
     * device may be mastering into the same arena the outer program just handed
     * to its own device, and no bookkeeping fixes that. There is one machine and
     * one set of hardware; one program at a time is the honest contract.
     *
     * Refused BEFORE any side effect, so the outer run is untouched: no zeroing,
     * no re-arming, no trace line, and the caller gets a sentence that says what
     * to do instead. A native (C) audio sink is unaffected — it never re-enters
     * the VM — so this only ever refuses the VM-sink case, where it must. */
    static int in_run;
    if (in_run) {
        res->status = DVM_TRAP_REENTRY;
        /* This text has a HARD length budget. It comes back to the program
         * through sys_fail(), which prefixes the syscall name ("audio.tone: ")
         * and then truncates the whole thing to DVM_MSG_MAX. A sentence that
         * only just fits on its own loses its most useful clause — the one
         * saying what to do instead — the moment it is prefixed. Keep it inside
         * DVM_MSG_MAX minus room for the longest syscall name. */
        copy_printable(res->msg, sizeof res->msg,
                       "one driver program at a time: a second run would reset the "
                       "scratch memory and DMA guard bands under the first. Ask for "
                       "this after the run finishes, not inside it");
        return DVM_TRAP_REENTRY;
    }

    run_t st;
    dz(&st, sizeof st);
    st.p   = p;
    st.pol = pol;          /* the null check below runs before anything reads it */
    st.io  = io;
    st.res = res;

    if (!p || !pol || !io) {
        res->status = DVM_TRAP_POLICY;
        copy_printable(res->msg, sizeof res->msg,
                       !p   ? "no program supplied" :
                       !pol ? "no policy supplied"  : "no I/O backend supplied");
        return DVM_TRAP_POLICY;
    }

    char pmsg[DVM_MSG_MAX];
    dvm_status_t ps = dvm_policy_check(pol, pmsg, sizeof pmsg);
    if (ps != DVM_OK) {
        res->status = ps;
        copy_printable(res->msg, sizeof res->msg, pmsg);
        if (pol->trace != DVM_TRACE_OFF)
            trace_err(DVM_TRACE_ERR, "dvm.trap", "%s: %s", dvm_status_name(ps), pmsg);
        return ps;
    }

    dvm_asm_err_t verr;
    dvm_status_t vs = dvm_program_validate(p, &verr);
    if (vs != DVM_OK) {
        res->status = vs;
        res->line   = (uint32_t)verr.line;
        copy_printable(res->msg, sizeof res->msg, verr.msg);
        if (pol->trace != DVM_TRACE_OFF)
            trace_err(DVM_TRACE_ERR, "dvm.trap", "%s: %s", dvm_status_name(vs), verr.msg);
        return vs;
    }

    if (nargs < 0) nargs = 0;
    if (nargs > DVM_NREGS) nargs = DVM_NREGS;
    for (int i = 0; i < nargs && args; i++) st.reg[i] = args[i];

    /* State the sandbox before running inside it: these lines are the record of
     * what the program was permitted to touch, which is half of any later
     * diagnosis of what it did. */
    if (pol->trace != DVM_TRACE_OFF) {
        char g[GRANTS_CAP];
        sb_t s;

        sb_init(&s, g, sizeof g);
        put_port_grants(&s, pol);
        res->trace_lines++;
        trace_ok("dvm.grant", "ports %s", g);

        sb_init(&s, g, sizeof g);
        put_mmio_grants(&s, pol);
        res->trace_lines++;
        trace_ok("dvm.grant", "mmio %s", g);

        sb_init(&s, g, sizeof g);
        if (!pol->npci) sb_addf(&s, "none");
        for (int i = 0; i < pol->npci; i++)
            sb_addf(&s, "%s%02x:%02x.%x", i ? "," : "",
                    pol->pci[i].bus, pol->pci[i].dev, pol->pci[i].fn);
        res->trace_lines++;
        trace_ok("dvm.grant", "pci %s", g);

        /* The DMA grant is stated as loudly as the others, and it says what it
         * is: writable memory, and the hardware's route into it. This line is
         * the operator's only notice that a run could have caused DMA. */
        res->trace_lines++;
        if (pol->dma_size)
            trace_ok("dvm.grant", "dma 0x%lx+0x%lx rw (scratch buffer; a device "
                                  "pointed at it will master into it)",
                     (unsigned long)pol->dma_base, (unsigned long)pol->dma_size);
        else
            trace_ok("dvm.grant", "dma none");

        /* Scratch memory is stated too, and stated as unconditional: it is not
         * a grant, it is a property of the machine, and a model reading the
         * trace should not have to wonder whether this run had memory. */
        sb_init(&s, g, sizeof g);
        {
            int any = 0;
            for (int i = 0; i < DVM_SYS__COUNT; i++)
                if (pol->sys_allow & ((uint32_t)1u << i))
                    sb_addf(&s, "%s%s", any++ ? "," : "", dvm_sys_name((dvm_sys_nr_t)i));
            if (!any) sb_addf(&s, "none");
        }
        res->trace_lines++;
        trace_ok("dvm.grant", "mem %u bytes scratch (zeroed); sys %s%s%s",
                 (unsigned)DVM_MEM_SIZE, g,
                 pol->fs_root[0] ? " under " : "", pol->fs_root);

        res->trace_lines++;
        trace_ok("dvm.run", "insns=%lu args=%d steps<=%lu io<=%lu delay<=%lu us trace=%d",
                 (unsigned long)p->ninsn, nargs, (unsigned long)pol->max_steps,
                 (unsigned long)pol->max_io, (unsigned long)pol->max_delay_us,
                 (int)pol->trace);
    }

    /* Arm the guard bands here, so a caller cannot forget to and so damage found
     * by dvm_dma_check() after this call belongs to THIS run. A device that keeps
     * mastering after the run returns can still dirty the guard between the check
     * and the next run's arming, in which case the next run is blamed — that is
     * the honest limit of attribution without an IOMMU. */
    /* From here to the end of execute() this machine has a program running, and
     * the two blocks below are the state that makes that exclusive. Set after
     * every validation failure has already returned, so a rejected call does not
     * lock anything out. */
    in_run = 1;

    if (pol->dma_size) dma_guard_arm();

    /* Zero the scratch arena. Unconditional, and here rather than at the end of
     * the previous run: a program must not be able to read what another left,
     * and "the last run cleaned up after itself" is a property that fails
     * exactly when the last run trapped. The kernel's chance to read a
     * program's output is the window between dvm_run() returning and the next
     * one starting — see dvm_mem_peek(). */
    dz(dvm_mem, sizeof dvm_mem);

    dvm_status_t s = execute(&st);

    /* Released the instant execution stops, however it stopped. execute() has no
     * path that longjmps or panics out, so this is the single exit. */
    in_run = 0;

    for (int i = 0; i < DVM_NREGS; i++) res->reg[i] = st.reg[i];
    res->status = s;
    res->pc     = st.pc;
    res->line   = st.line;

    if (pol->trace != DVM_TRACE_OFF) {
        res->trace_lines++;
        if (s == DVM_OK)
            trace_ok("dvm.halt", "pc=%03lu ln=%lu steps=%lu io=%lu (pr=%lu pw=%lu mr=%lu "
                                 "mw=%lu pci=%lu) dma=%lu (rd=%lu wr=%lu) delay=%lu us "
                                 "prints=%lu mem=%lu B sys=%lu",
                     (unsigned long)res->pc, (unsigned long)res->line,
                     (unsigned long)res->steps, (unsigned long)res->io_ops,
                     (unsigned long)res->n_port_rd, (unsigned long)res->n_port_wr,
                     (unsigned long)res->n_mmio_rd, (unsigned long)res->n_mmio_wr,
                     (unsigned long)res->n_pci_rd,
                     (unsigned long)res->n_dma_rd + res->n_dma_wr,
                     (unsigned long)res->n_dma_rd, (unsigned long)res->n_dma_wr,
                     (unsigned long)res->delay_us, (unsigned long)res->prints,
                     (unsigned long)res->mem_bytes, (unsigned long)res->n_sys);
        else
            trace_err(DVM_TRACE_ERR, "dvm.stop", "%s steps=%lu io=%lu delay=%lu us",
                      dvm_status_name(s), (unsigned long)res->steps,
                      (unsigned long)res->io_ops, (unsigned long)res->delay_us);
    }
    return s;
}

/* ====================================================================== */
/* 7. the hardware backend                                                */
/* ====================================================================== */

#ifndef FABLEOS_HOSTTEST

#include "io.h"
#include "pci.h"
#include "kernel.h"     /* mdelay, millis */

static void hw_port_write(void *ctx, uint16_t port, uint8_t width, uint32_t val) {
    (void)ctx;
    switch (width) {
        case 1: outb(port, (uint8_t)val); break;
        case 2: outw(port, (uint16_t)val); break;
        default: outl(port, val); break;
    }
}

static uint32_t hw_port_read(void *ctx, uint16_t port, uint8_t width) {
    (void)ctx;
    switch (width) {
        case 1: return inb(port);
        case 2: return inw(port);
        default: return inl(port);
    }
}

static void hw_mmio_write(void *ctx, uint64_t addr, uint8_t width, uint32_t val) {
    (void)ctx;
    volatile void *p = (volatile void *)(uintptr_t)addr;
    switch (width) {
        case 1: *(volatile uint8_t *)p  = (uint8_t)val; break;
        case 2: *(volatile uint16_t *)p = (uint16_t)val; break;
        default: *(volatile uint32_t *)p = val; break;
    }
}

static uint32_t hw_mmio_read(void *ctx, uint64_t addr, uint8_t width) {
    (void)ctx;
    volatile void *p = (volatile void *)(uintptr_t)addr;
    switch (width) {
        case 1: return *(volatile uint8_t *)p;
        case 2: return *(volatile uint16_t *)p;
        default: return *(volatile uint32_t *)p;
    }
}

static uint32_t hw_pci_read32(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    (void)ctx;
    return pci_cfg_read32(bus, dev, fn, off);
}

/* DELAY is documented as a floor, not a precise timer. mdelay() covers whole
 * milliseconds; the remainder is a TSC spin calibrated once against millis().
 * Both the calibration loops are iteration-bounded, because a delay primitive
 * that can hang is worse than one that is imprecise. */
static uint64_t tsc_per_us;      /* 0 = uncalibrated, -1 = unusable */

static void calibrate_tsc(void) {
    uint64_t t0 = millis();
    uint64_t guard = 0;
    while (millis() == t0 && ++guard < 50000000ull) { }
    if (guard >= 50000000ull) { tsc_per_us = (uint64_t)-1; return; }

    uint64_t start = rdtsc(), t1 = millis();
    guard = 0;
    while (millis() - t1 < 2 && ++guard < 50000000ull) { }
    uint64_t ticks = rdtsc() - start;
    tsc_per_us = ticks / 2000u;
    if (!tsc_per_us) tsc_per_us = (uint64_t)-1;
}

static void hw_delay_us(void *ctx, uint32_t us) {
    (void)ctx;
    uint32_t ms = us / 1000u, rem = us % 1000u;
    if (ms) mdelay(ms);
    if (!rem) return;
    if (!tsc_per_us) calibrate_tsc();
    if (tsc_per_us == (uint64_t)-1) {
        for (volatile uint32_t i = 0; i < rem * 20u; i++) { }   /* bounded fallback */
        return;
    }
    uint64_t end = rdtsc() + (uint64_t)rem * tsc_per_us;
    while (rdtsc() < end) { }
}

/* ---- the kernel-call backend ----
 *
 * Six functions, each one a hole this file has already argued for. What they
 * are NOT is a place to put policy: by the time one of these runs, the VM has
 * bounded every span, built and confined every path, and range-checked every
 * scalar. These translate, and nothing else — a check that appears here and not
 * in the portable half is a check no host test can prove.
 *
 * They return >= 0 or a negative code with a sentence in `err`. The sentence is
 * what the model reads next turn, so it says what the kernel refused, not that
 * something went wrong. */

#include "vfs.h"
#include "audio.h"

static int64_t k_con_write(void *ctx, const char *text, size_t len,
                           char *err, size_t errcap) {
    (void)ctx; (void)err; (void)errcap;
    /* Through trace.c, so the '[' in column zero stays the kernel's. The text
     * is already printable-only: the VM sanitised it before this was called. */
    trace_ok("dvm.say", "%s", text);
    return (int64_t)len;
}

/* One sentence per VFS code, because "-2" is not something a model can act on
 * and "no such file" is. */
static const char *vfs_why(int64_t rc) {
    switch (rc) {
        case VFS_ENOENT:  return "no such file or directory";
        case VFS_EEXIST:  return "it already exists";
        case VFS_ENOTDIR: return "a component of the path is not a directory";
        case VFS_ENOSPC:  return "the filesystem is full";
        case VFS_EINVAL:  return "the filesystem refused the path or the access mode";
        default:          return "the filesystem refused the operation";
    }
}

static int64_t k_fs_read(void *ctx, const char *path, void *dst, size_t cap,
                         char *err, size_t errcap) {
    (void)ctx;
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) {
        snprintf(err, errcap, "cannot open %s: %s", path, vfs_why(VFS_ENOENT));
        return VFS_ENOENT;
    }
    int64_t n = vfs_read(f, dst, (uint64_t)cap);
    vfs_close(f);
    if (n < 0) {
        snprintf(err, errcap, "reading %s: %s", path, vfs_why(n));
        return n;
    }
    return n;
}

static int64_t k_fs_write(void *ctx, const char *path, const void *src, size_t len,
                          char *err, size_t errcap) {
    (void)ctx;
    file_t *f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        snprintf(err, errcap, "cannot create %s: %s", path, vfs_why(VFS_EINVAL));
        return VFS_EINVAL;
    }
    int64_t n = len ? vfs_write(f, src, (uint64_t)len) : 0;
    vfs_close(f);
    if (n < 0) {
        snprintf(err, errcap, "writing %s: %s", path, vfs_why(n));
        return n;
    }
    return n;
}

static int64_t k_fs_size(void *ctx, const char *path, char *err, size_t errcap) {
    (void)ctx;
    vfs_stat_t stt;
    int rc = vfs_stat(path, &stt);
    if (rc != VFS_OK) {
        snprintf(err, errcap, "%s: %s", path, vfs_why(rc));
        return rc;
    }
    return (int64_t)stt.size;
}

static int64_t k_audio_tone(void *ctx, uint32_t hz, uint32_t ms,
                            char *err, size_t errcap) {
    (void)ctx;
    if (!audio_available()) {
        snprintf(err, errcap, "this machine has no audio sink installed; "
                              "driver_install puts one there");
        return -1;
    }
    int rc = audio_tone(hz, ms);
    if (rc != AUDIO_OK) {
        snprintf(err, errcap, "the audio service refused: %s", audio_last_error());
        return -1;
    }
    return 0;
}

static int64_t k_time_ms(void *ctx, char *err, size_t errcap) {
    (void)ctx; (void)err; (void)errcap;
    return (int64_t)millis();
}

static const dvm_sys_t hw_sys = {
    .ctx        = 0,
    .con_write  = k_con_write,
    .fs_read    = k_fs_read,
    .fs_write   = k_fs_write,
    .fs_size    = k_fs_size,
    .audio_tone = k_audio_tone,
    .time_ms    = k_time_ms,
};

const dvm_sys_t *dvm_sys_kernel(void) { return &hw_sys; }

static const dvm_io_t hw_io = {
    .ctx        = 0,
    .port_write = hw_port_write,
    .port_read  = hw_port_read,
    .mmio_write = hw_mmio_write,
    .mmio_read  = hw_mmio_read,
    .pci_read32 = hw_pci_read32,
    .delay_us   = hw_delay_us,
    .sys        = &hw_sys,
};

const dvm_io_t *dvm_io_hardware(void) { return &hw_io; }

#else   /* host build: there is no hardware and no kernel to reach */

const dvm_io_t  *dvm_io_hardware(void) { return 0; }
const dvm_sys_t *dvm_sys_kernel(void)  { return 0; }

#endif
