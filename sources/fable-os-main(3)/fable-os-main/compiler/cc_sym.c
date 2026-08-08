/* cc_sym.c — the curated kernel entry points, and the forwarders that bound them.
 *
 * THIS FILE IS THE SECURITY BOUNDARY. There is no MMU behind it: compiled code
 * runs in ring 0 with the same view of memory as the kernel itself, so nothing
 * here can be enforced by hardware. What can be enforced is WHICH FUNCTIONS
 * EXIST, and that is enforced structurally rather than by inspection:
 *
 *   - The table below is the whole surface. cc_declare_symbols() predeclares
 *     exactly these names and nothing else, so a program cannot mention any
 *     other kernel function: an unknown identifier is a diagnostic, not a
 *     link-time guess.
 *   - The subset has no function pointers, no address-of-function, and `switch`
 *     compiles to compares rather than a jump table (cc_x64.c). So the set of
 *     addresses generated code can transfer control to is fixed before the first
 *     instruction runs, and it is {this unit's functions} + {this table}.
 *   - EVERY ENTRY IS A FORWARDER WRITTEN HERE, never a raw kernel symbol. That
 *     is the part that matters, because it is where the bounds live: `print` is
 *     not kputs, `file_write` is not vfs_write. A future entry that exposes a
 *     kernel function directly would be the first hole in this design.
 *
 * WHAT EACH FORWARDER IS FOR, one line each, is in the table's `why` field —
 * which is also what the tool description shows the model, so the justification
 * and the documentation cannot drift apart.
 *
 * THE THREE THINGS THAT ARE BOUNDED HERE AND WHY
 *
 *   CONSOLE OUTPUT. print() goes out through kprintf with a "cc| " prefix and
 *   the bytes forced to printable ASCII. Both are load-bearing. The prefix keeps
 *   the column-zero invariant (trace.h: a '[' in column zero is a kernel trace
 *   line and nothing model-controlled may ever put one there) true even for a
 *   program that prints "[vfs_write fd=3 -> ok]" on purpose. The sanitising stops
 *   a program painting the framebuffer with control codes. Output is also
 *   captured, bounded, and returned in the tool result, because a program whose
 *   output only reached the screen would be invisible to the model that wrote it.
 *
 *   SPAN LENGTHS. memcpy, memset, memcmp and strlen are the only way for a
 *   program to touch a lot of memory WITHOUT SPENDING FUEL — a byte loop costs
 *   one unit per iteration, `memset(p, 0, 1<<40)` costs one. So a length above
 *   CC_RT_SPAN_MAX is REFUSED, the refusal is counted, and the count is reported.
 *   Refusing rather than clamping is deliberate: a clamp is a wrong answer that
 *   looks like a right one.
 *
 *   FILE ACCESS. file_read/file_write/file_size take a NAME, not a path: no '/',
 *   no "..", 1..CC_RT_NAME_MAX bytes of [A-Za-z0-9._-], resolved under
 *   cc_data_root(). There is no traversal to defeat because there is no path to
 *   traverse, and the data root is a SIBLING of the program store, so a compiled
 *   program cannot rewrite the machine's programs — including its own source.
 *
 * WHAT IS NOT HERE, and each is a decision rather than an omission:
 *   no port I/O, no MMIO, no PCI, no DMA          hardware bring-up is
 *       driver_run/driver_install's job and has a policy engine this does not.
 *   no allocator                                  a program gets its frame, its
 *       globals and nothing else, so it cannot leak.
 *   no network                                    there is no synchronous fetch
 *       to forward to, and adding one belongs in net/.
 *   no cc_call, no capability_invoke               a compiled program cannot
 *       re-enter this module. One code image, one program stack, one runtime
 *       block: re-entry would corrupt all three, and refusing it structurally is
 *       cheaper than making them re-entrant.
 *   no delay or sleep                             interrupts are off; a program
 *       that waits is a program that hangs, and fuel is the only clock.
 */

#include "cc_int.h"

/* kernel.h BEFORE <string.h>: kernel.h declares memcpy/memset/memmove/memcmp
 * itself, and a host <string.h> turns those names into fortified macros, so the
 * other order makes the declarations unparseable. Every module in this tree
 * follows the same order for the same reason. */
#include "kernel.h"
#include "vfs.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The most bytes any one runtime call will touch, and the longest file name. */
#define CC_RT_SPAN_MAX   65536
#define CC_RT_NAME_MAX   40
#define CC_RT_LINE_MAX   160

/* ===================================================================== */
/* run state                                                             */
/* ===================================================================== */

static char     g_line[CC_RT_LINE_MAX + 1];
static size_t   g_line_len;
static char     g_out[CC_OUT_MAX];
static size_t   g_out_len;
static uint32_t g_calls;
static uint32_t g_refused;

void cc_rt_reset(void) {
    g_line_len = 0;
    g_line[0]  = '\0';
    g_out_len  = 0;
    g_out[0]   = '\0';
    g_calls    = 0;
    g_refused  = 0;
}

uint32_t cc_rt_calls(void)    { return g_calls; }
uint32_t cc_rt_refusals(void) { return g_refused; }

static void out_push(const char *s, size_t n) {
    for (size_t i = 0; i < n && g_out_len + 1 < sizeof g_out; i++)
        g_out[g_out_len++] = s[i];
    g_out[g_out_len] = '\0';
}

/* One line of program output, through the kernel's own formatter with a prefix
 * that keeps column zero the kernel's. */
static void flush_line(void) {
    g_line[g_line_len] = '\0';
    kprintf("cc| %s\n", g_line);
    out_push(g_line, g_line_len);
    out_push("\n", 1);
    g_line_len = 0;
    g_line[0] = '\0';
}

size_t cc_rt_output(char *dst, size_t cap) {
    if (g_line_len) flush_line();
    if (!cap) return 0;
    size_t n = g_out_len < cap - 1 ? g_out_len : cap - 1;
    memcpy(dst, g_out, n);
    dst[n] = '\0';
    return n;
}

static void put_byte(int c) {
    if (c == '\n') { flush_line(); return; }
    if (c == '\t') c = ' ';
    if (c < 0x20 || c > 0x7e) c = '.';
    if (g_line_len >= CC_RT_LINE_MAX) flush_line();
    g_line[g_line_len++] = (char)c;
}

/* A bounded, NUL-terminated read of a program-supplied string. Returns the
 * length, or -1 if no terminator was found inside the bound. There is no way to
 * make this safe against a wild pointer — that is C, and cc.h says so — but it
 * IS safe against a string that simply has no terminator. */
static long span_of(const char *s, long cap) {
    if (!s) return -1;
    for (long i = 0; i < cap; i++) if (!s[i]) return i;
    return -1;
}

/* ===================================================================== */
/* the forwarders                                                        */
/* ===================================================================== */

static void cc_rt_print(const char *s) {
    g_calls++;
    long n = span_of(s, CC_RT_LINE_MAX * 8);
    if (n < 0) { g_refused++; return; }
    for (long i = 0; i < n; i++) put_byte((unsigned char)s[i]);
}

static void cc_rt_print_num(long v) {
    g_calls++;
    char buf[24];
    snprintf(buf, sizeof buf, "%ld", v);
    for (const char *p = buf; *p; p++) put_byte((unsigned char)*p);
}

static void cc_rt_print_hex(unsigned long v) {
    g_calls++;
    char buf[24];
    snprintf(buf, sizeof buf, "0x%lx", v);
    for (const char *p = buf; *p; p++) put_byte((unsigned char)*p);
}

static void cc_rt_print_char(long c) {
    g_calls++;
    put_byte((int)(c & 0xff));
}

static long cc_rt_time_ms(void) {
    g_calls++;
    return (long)millis();
}

static long cc_rt_strlen(const char *s) {
    g_calls++;
    long n = span_of(s, CC_RT_SPAN_MAX);
    if (n < 0) { g_refused++; return 0; }
    return n;
}

static int span_ok(long n) {
    if (n < 0 || n > CC_RT_SPAN_MAX) { g_refused++; return 0; }
    return 1;
}

static void *cc_rt_memcpy(void *d, const void *s, long n) {
    g_calls++;
    if (!d || !s || !span_ok(n)) return d;
    memmove(d, s, (size_t)n);           /* memmove: overlap is a mistake, not a fault */
    return d;
}

static void *cc_rt_memset(void *d, long c, long n) {
    g_calls++;
    if (!d || !span_ok(n)) return d;
    memset(d, (int)(c & 0xff), (size_t)n);
    return d;
}

static long cc_rt_memcmp(const void *a, const void *b, long n) {
    g_calls++;
    if (!a || !b || !span_ok(n)) return 0;
    return (long)memcmp(a, b, (size_t)n);
}

/* ---- files, under the data root, by NAME ---- */

/* Build "<data root>/<name>", refusing anything that is not a plain name. */
static int resolve(const char *name, char *dst, size_t cap) {
    long n = span_of(name, CC_RT_NAME_MAX + 1);
    if (n < 1) return 0;
    for (long i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
    }
    if (name[0] == '.') return 0;                    /* no ".", "..", no dotfiles */
    int w = snprintf(dst, cap, "%s/%s", cc_data_root(), name);
    return w > 0 && (size_t)w < cap;
}

static long cc_rt_file_write(const char *name, const char *buf, long len) {
    g_calls++;
    char path[VFS_PATH_MAX + 1];
    if (!resolve(name, path, sizeof path) || !buf || !span_ok(len)) {
        g_refused++;
        return -1;
    }
    file_t *f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return -1;
    int64_t w = vfs_write(f, buf, (uint64_t)len);
    vfs_close(f);
    return (long)w;
}

static long cc_rt_file_read(const char *name, char *buf, long max) {
    g_calls++;
    char path[VFS_PATH_MAX + 1];
    if (!resolve(name, path, sizeof path) || !buf || !span_ok(max)) {
        g_refused++;
        return -1;
    }
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) return -1;
    int64_t r = vfs_read(f, buf, (uint64_t)max);
    vfs_close(f);
    return (long)r;
}

static long cc_rt_file_size(const char *name) {
    g_calls++;
    char path[VFS_PATH_MAX + 1];
    if (!resolve(name, path, sizeof path)) { g_refused++; return -1; }
    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK) return -1;
    return (long)st.size;
}

/* ===================================================================== */
/* the table                                                             */
/* ===================================================================== */

/* Signature codes: v void, i int, l long, u unsigned long, p char *, P void *.
 * A one-character code per position keeps the table readable and keeps the
 * declared C signature (in `decl`, which the model reads) beside it, so a
 * mismatch between the two is visible on one line. */
/* `fn` is a function pointer, not a void *, because a cast from a function
 * pointer to an object pointer is not an address constant and a static
 * initialiser may not contain one. The cast happens in cc_declare_symbols(). */
typedef void (*rawfn_t)(void);

typedef struct {
    const char *name;
    const char *sig;        /* [0] = return, [1..] = parameters */
    rawfn_t     fn;
    const char *decl;
    const char *why;
} entry_t;

static const entry_t g_table[] = {
    { "print", "vp", (rawfn_t)cc_rt_print,
      "void print(char *s)",
      "write text to the console; bytes are forced printable and the line is "
      "prefixed so it can never look like a kernel trace line" },
    { "print_num", "vl", (rawfn_t)cc_rt_print_num,
      "void print_num(long n)",
      "write a signed whole number" },
    { "print_hex", "vu", (rawfn_t)cc_rt_print_hex,
      "void print_hex(unsigned long n)",
      "write a number as 0x hex" },
    { "print_char", "vl", (rawfn_t)cc_rt_print_char,
      "void print_char(long c)",
      "write one character" },
    { "time_ms", "l", (rawfn_t)cc_rt_time_ms,
      "long time_ms(void)",
      "milliseconds since boot; the only clock, since there is no sleep" },
    { "strlen", "lp", (rawfn_t)cc_rt_strlen,
      "long strlen(char *s)",
      "bounded: stops at 65536 bytes and returns 0 for a string with no "
      "terminator, rather than reading until it faults" },
    { "memcpy", "PPPl", (rawfn_t)cc_rt_memcpy,
      "void *memcpy(void *d, void *s, long n)",
      "n above 65536 is refused and counted: this and memset are the only ways "
      "to touch a lot of memory without spending fuel" },
    { "memset", "PPll", (rawfn_t)cc_rt_memset,
      "void *memset(void *d, long c, long n)",
      "same bound as memcpy" },
    { "memcmp", "lPPl", (rawfn_t)cc_rt_memcmp,
      "long memcmp(void *a, void *b, long n)",
      "same bound as memcpy" },
    { "file_write", "lppl", (rawfn_t)cc_rt_file_write,
      "long file_write(char *name, char *buf, long len)",
      "a plain NAME, never a path, under the data root - so a program can keep "
      "what it computed but cannot reach the program store" },
    { "file_read", "lppl", (rawfn_t)cc_rt_file_read,
      "long file_read(char *name, char *buf, long max)",
      "reads back what file_write kept; returns the byte count or -1" },
    { "file_size", "lp", (rawfn_t)cc_rt_file_size,
      "long file_size(char *name)",
      "bytes, or -1 if there is no such file" },
};

#define NENTRY ((int)(sizeof g_table / sizeof g_table[0]))

/* The documentation view, kept in a parallel static array so cc_symbol_at() can
 * hand out a stable pointer with no allocation. */
static cc_symbol_t g_doc[NENTRY];

int cc_symbol_count(void) { return NENTRY; }

const cc_symbol_t *cc_symbol_at(int i) {
    if (i < 0 || i >= NENTRY) return NULL;
    g_doc[i].decl = g_table[i].decl;
    g_doc[i].why  = g_table[i].why;
    return &g_doc[i];
}

static cc_type_t *type_of_code(char c) {
    switch (c) {
    case 'v': return cc_ty_void;
    case 'i': return cc_ty_int;
    case 'l': return cc_ty_long;
    case 'u': return cc_ty_ulong;
    case 'p': return cc_pointer_to(cc_ty_char);
    case 'P': return cc_pointer_to(cc_ty_void);
    default:  return NULL;
    }
}

/* The predeclared integer typedefs. A program that writes uint32_t without an
 * #include is the common case, and failing it would waste a turn for no reason;
 * <fableos.h> declares the same names again and the parser accepts an identical
 * typedef, so both spellings work. */
int cc_declare_symbols(void) {
    static const struct { const char *name; char code; } tds[] = {
        { "int8_t",   'c' }, { "uint8_t",  'C' },
        { "int16_t",  's' }, { "uint16_t", 'S' },
        { "int32_t",  'n' }, { "uint32_t", 'N' },
        { "int64_t",  'l' }, { "uint64_t", 'L' },
        { "size_t",   'L' }, { "ssize_t",  'l' },
        { "intptr_t", 'l' }, { "uintptr_t",'L' },
    };
    for (size_t i = 0; i < sizeof tds / sizeof tds[0]; i++) {
        cc_type_t *t;
        switch (tds[i].code) {
        case 'c': t = cc_ty_char;  break;
        case 'C': t = cc_ty_uchar; break;
        case 's': t = cc_ty_short; break;
        case 'S': t = cc_ty_ushort;break;
        case 'n': t = cc_ty_int;   break;
        case 'N': t = cc_ty_uint;  break;
        case 'l': t = cc_ty_long;  break;
        default:  t = cc_ty_ulong; break;
        }
        cc_sym_t *s = cc_alloc(sizeof *s);
        if (!s) return 0;
        s->kind = SY_TYPEDEF;
        s->name = cc_intern(tds[i].name, strlen(tds[i].name));
        s->type = t;
        s->scope_depth = 0;
        s->next = cc_ctx.scope;
        cc_ctx.scope = s;
    }

    for (int i = 0; i < NENTRY; i++) {
        cc_type_t *fn = cc_alloc(sizeof *fn);
        if (!fn) return 0;
        fn->kind = TY_FUNC;
        fn->size = 1;
        fn->align = 1;
        fn->base = type_of_code(g_table[i].sig[0]);
        if (!fn->base)
            return cc_errorf(NULL, "internal: symbol table entry %d has a bad "
                                   "return code", i);
        for (const char *p = g_table[i].sig + 1; *p; p++) {
            if (fn->nparam >= CC_MAX_PARAMS)
                return cc_errorf(NULL, "internal: symbol '%s' has too many "
                                       "parameters", g_table[i].name);
            cc_type_t *pt = type_of_code(*p);
            if (!pt)
                return cc_errorf(NULL, "internal: symbol '%s' has a bad parameter "
                                       "code", g_table[i].name);
            fn->param[fn->nparam++] = pt;
        }
        cc_sym_t *s = cc_alloc(sizeof *s);
        if (!s) return 0;
        s->kind = SY_EXTERN;
        s->name = cc_intern(g_table[i].name, strlen(g_table[i].name));
        s->type = fn;
        s->addr = (void *)(uintptr_t)g_table[i].fn;
        s->defined = 1;
        s->scope_depth = 0;
        s->next = cc_ctx.scope;
        cc_ctx.scope = s;
    }
    return 1;
}
