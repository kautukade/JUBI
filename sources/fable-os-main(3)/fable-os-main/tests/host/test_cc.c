/* test_cc.c — the C compiler: the accepted subset, every refusal, and fuzz.
 *
 * THIS SUITE EXECUTES THE MACHINE CODE THE COMPILER EMITS. That is the whole
 * design of it and the reason for one line in the Makefile: on an Apple-silicon
 * Mac the binary is built `-arch x86_64` so that the emitted bytes can be called
 * in-process through the same mmap'd, PROT_EXEC buffer the kernel gets from .bss.
 * Nothing about the emitter differs between the two — the trampoline, the fuel
 * checks and the instruction bytes are identical — so an assertion here is an
 * assertion about what runs on the machine, not about what a golden file says
 * should run.
 *
 * On a host that cannot run x86-64, cc_run() returns CC_ENOEXEC and this suite
 * SKIPS the tests that run code and says how many it skipped. It does not print
 * a green it has not earned.
 *
 * FOUR THINGS ARE ASSERTED, in order of how much they matter:
 *
 *   1. THE ANSWER IS RIGHT. `expect()` runs a program and compares the number it
 *      returns. A miscompile is the failure mode this whole file exists to catch,
 *      and the only way to catch one is to run the code. Signedness, widths,
 *      overflow wrapping, division rounding and pointer scaling all get their own
 *      cases, because those are where a naive emitter is wrong in ways that look
 *      plausible.
 *
 *   2. THE REFUSAL SAYS SOMETHING USEFUL. `reject()` asserts that a construct
 *      outside the subset produces a diagnostic, at the right LINE and COLUMN,
 *      whose text contains a phrase a model could act on. A rejection with a
 *      vague message is treated as a failure here, because in production it costs
 *      a turn.
 *
 *   3. NOTHING MODEL-AUTHORED CAN BREAK THE MACHINE. Deep nesting, identifiers
 *      of 10,000 characters, unterminated everything, embedded NULs, a file of
 *      nothing but '(' — plus 4000 generated tool calls. The assertions after
 *      each are always the same three: a result exists, kpanic_hit is zero, and
 *      the tool schema is still exactly the size the header reserved.
 *
 *   4. THE STORE IS HONEST. A program saved, the registry cleared and re-read
 *      from disk (which is what a reboot is), and then called. Plus a corrupted
 *      record refused by its own checksum, a fall back to the backup, and the
 *      fixed-width panel that makes any of it visible to the model.
 *
 * WHERE TO ADD SOMETHING. A new accepted construct goes in the SUBSET table; a
 * new refusal in the REJECT table. Both are data, so adding a case is one line
 * and the diagnostics stay in one place where they can be read together.
 */

#include "kernel.h"

#include "harness.h"
#include "cc.h"
#include "vfs.h"
#include "fs.h"
#include "tool.h"
#include "tooltable.h"
#include "trace.h"
#include "json.h"
#include "heap.h"

#include <stdlib.h>

/* The tool descriptors are static to their file and REGISTER_TOOL's section does
 * not survive a separate host translation unit, so the tool source is compiled
 * INTO this suite. The Makefile lists it as a prerequisite. */
#include "../../tools/cc_tools.c"

/* ====================================================================== */
/* scaffolding                                                            */
/* ====================================================================== */

static int  g_can_exec;        /* 0 on a host that cannot run x86-64 */
static int  g_skipped;
static char g_diag[CC_DIAG_MAX * (CC_MSG_MAX + CC_SNIP_MAX + 64)];
static cc_result_t g_r;

static void fs_bringup(void) {
    static int once = 0;
    if (once) return;
    once = 1;
    vfs_init();
    ramfs_register();
    CHECK_EQ(vfs_mount("/", "ramfs"), VFS_OK);
}

/* Read a file the repository ships, from the working directory `make test-host`
 * uses (). Fails loudly rather than silently skipping, because a suite that
 * quietly stops checking the shipped examples is worse than no suite. */
static size_t slurp(const char *path, char *dst, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        th_checks++;
        th_fails++;
        printf("    FAIL cannot open %s (run make test-host from )\n", path);
        return 0;
    }
    size_t n = fread(dst, 1, cap - 1, f);
    fclose(f);
    dst[n] = '\0';
    return n;
}

/* ---- compile, run, compare ---- */

static int compile_only(const char *src) {
    return cc_compile(src, strlen(src), "t.c", &g_r);
}

/* Run `src` and require it to return `want`. */
static void expect(const char *label, const char *src, long long want) {
    int rc = compile_only(src);
    if (rc != CC_OK) {
        cc_diags_render(&g_r, g_diag, sizeof g_diag);
        th_checks++;
        th_fails++;
        printf("    FAIL %s: did not compile (%d)\n%s\n", label, rc, g_diag);
        return;
    }
    if (!g_can_exec) { g_skipped++; return; }
    rc = cc_run(NULL, 0, 0, &g_r);
    if (rc != CC_OK) {
        th_checks++;
        th_fails++;
        printf("    FAIL %s: did not run (%d)\n", label, rc);
        return;
    }
    th_checks++;
    if ((long long)(int64_t)g_r.value != want) {
        th_fails++;
        printf("    FAIL %s: returned %lld, want %lld\n", label,
               (long long)(int64_t)g_r.value, want);
    }
}

/* Require a refusal, at `line`:`col`, whose message contains `needle`. Pass
 * line/col as 0 to leave the position unchecked. */
static void reject(const char *label, const char *src, int line, int col,
                   const char *needle) {
    int rc = compile_only(src);
    th_checks++;
    if (rc == CC_OK) {
        th_fails++;
        printf("    FAIL %s: this was ACCEPTED and should not be:\n%s\n", label, src);
        return;
    }
    th_checks++;
    if (g_r.ndiag == 0) {
        th_fails++;
        printf("    FAIL %s: refused with no diagnostic at all\n", label);
        return;
    }
    const cc_diag_t *d = &g_r.diag[0];
    th_checks++;
    if (!strstr(d->msg, needle)) {
        th_fails++;
        printf("    FAIL %s: message does not contain \"%s\"\n      got: %s\n",
               label, needle, d->msg);
    }
    if (line) {
        th_checks++;
        if (d->line != line) {
            th_fails++;
            printf("    FAIL %s: diagnostic on line %d, want %d (%s)\n",
                   label, d->line, line, d->msg);
        }
    }
    if (col) {
        th_checks++;
        if (d->col != col) {
            th_fails++;
            printf("    FAIL %s: diagnostic at column %d, want %d (%s)\n",
                   label, d->col, col, d->msg);
        }
    }
}

/* ====================================================================== */
/* 1. the accepted subset, executed                                       */
/* ====================================================================== */

typedef struct { const char *label; const char *src; long long want; } case_t;

/* Everything cc.h's IN list claims, with a case per claim. The values are chosen
 * so that a plausible wrong answer is a different number: `signdiv` is -3 and not
 * -4 only if the emitter used idiv; `intoverflow` is negative only if 32-bit
 * arithmetic really wrapped at 32 bits. */
static const case_t SUBSET[] = {
  { "integer literal", "int main(void){return 42;}", 42 },
  { "hex and octal",   "int main(void){return 0x1f + 010;}", 31 + 8 },
  { "precedence",      "int main(void){return 2+3*4-6/2;}", 11 },
  { "remainder",       "int main(void){return 17%5;}", 2 },
  { "unary minus",     "int main(void){int a=5;return -a;}", -5 },
  { "shifts",          "int main(void){return (1<<10)>>2;}", 256 },
  { "arithmetic shift","int main(void){int a=-16;return a>>2;}", -4 },
  { "logical shift",   "int main(void){unsigned int a=0x80000000u;"
                       "return (int)(a>>28);}", 8 },
  { "comparisons",     "int main(void){return (3<4)+(5<=5)+(9>2)+(1!=2)+(1==1)"
                       "+(2>=2);}", 6 },
  { "and or shortcut", "int g=0; int bump(void){g=1;return 1;}"
                       "int main(void){if(0&&bump()){} if(1||bump()){} return g;}", 0 },
  { "ternary",         "int main(void){int x=7;return x>3?100:200;}", 100 },
  { "comma",           "int main(void){int a=(1,2,3);return a;}", 3 },
  { "not and bitnot",  "int main(void){return !0 + !5 + (~0);}", 0 },
  { "locals",          "int main(void){int a=1,b=2,c;c=a+b;return c*10;}", 30 },
  { "while",           "int main(void){int i=0,s=0;while(i<10){s+=i;i++;}return s;}", 45 },
  { "for",             "int main(void){int s=0;for(int i=1;i<=5;i++)s+=i*i;return s;}", 55 },
  { "do while",        "int main(void){int i=0,n=0;do{n++;i++;}while(i<3);return n;}", 3 },
  { "break continue",  "int main(void){int s=0;for(int i=0;i<10;i++){"
                       "if(i%2)continue;if(i>6)break;s+=i;}return s;}", 12 },
  { "nested loops",    "int main(void){int n=0;for(int i=0;i<4;i++)"
                       "for(int j=0;j<3;j++)n++;return n;}", 12 },
  { "switch",          "int main(void){int s=0;for(int i=0;i<5;i++){switch(i){"
                       "case 0: s+=1; break; case 1: case 2: s+=10; break; "
                       "default: s+=100;}}return s;}", 221 },
  { "continue in switch",
                       "int main(void){int s=0;for(int i=0;i<4;i++){"
                       "switch(i){case 1: continue; default: break;} s+=i;}"
                       "return s;}", 0 + 2 + 3 },
  { "recursion",       "int fib(int n){if(n<2)return n;return fib(n-1)+fib(n-2);}"
                       "int main(void){return fib(15);}", 610 },
  { "six parameters",  "int add(int a,int b,int c,int d,int e,int f){"
                       "return a+b+c+d+e+f;} int main(void){return add(1,2,3,4,5,6);}", 21 },
  { "void function",   "int g=0; void bump(int n){g+=n;} "
                       "int main(void){bump(5);bump(6);return g;}", 11 },
  { "forward decl",    "int later(int n); int main(void){return later(4);} "
                       "int later(int n){return n*n;}", 16 },
  { "pointer",         "int main(void){int x=5;int*p=&x;*p=9;return x;}", 9 },
  { "pointer to pointer",
                       "int main(void){int x=1;int*p=&x;int**q=&p;**q=7;return x;}", 7 },
  { "array",           "int main(void){int a[5];for(int i=0;i<5;i++)a[i]=i*3;"
                       "return a[4];}", 12 },
  { "array init tail zeroed",
                       "int main(void){int a[4]={7,8};return a[0]+a[1]+a[2]+a[3];}", 15 },
  { "2d array",        "int main(void){int m[3][4];for(int i=0;i<3;i++)"
                       "for(int j=0;j<4;j++)m[i][j]=i*10+j;return m[2][3];}", 23 },
  { "pointer arithmetic",
                       "int main(void){int a[5]={0,1,2,3,4};int*p=a+3;"
                       "return *p+(int)(p-a);}", 6 },
  { "string literal",  "int main(void){char*s=\"hello\";return s[1];}", 'e' },
  { "char array init", "int main(void){char s[]=\"abc\";return s[0]+s[2];}", 'a'+'c' },
  { "adjacent literals",
                       "int main(void){char*s=\"ab\" \"cd\";return s[3];}", 'd' },
  { "escapes",         "int main(void){char*s=\"a\\tb\\n\\x41\";"
                       "return s[1]+s[3]+s[4];}", '\t' + '\n' + 'A' },
  { "struct",          "struct P{int x;int y;}; int main(void){struct P p;"
                       "p.x=3;p.y=4;return p.x*p.y;}", 12 },
  { "struct pointer",  "struct P{int x;int y;}; int f(struct P*q){return q->x+q->y;}"
                       "int main(void){struct P p;p.x=10;p.y=20;return f(&p);}", 30 },
  { "struct init",     "struct P{int x;int y;}; int main(void){struct P p={5,6};"
                       "return p.x*10+p.y;}", 56 },
  { "struct assign",   "struct P{int x;int y;}; int main(void){struct P a={1,2},b;"
                       "b=a;return b.x+b.y*10;}", 21 },
  { "struct in array", "struct P{int x;int y;}; int main(void){struct P a[3];"
                       "for(int i=0;i<3;i++){a[i].x=i;a[i].y=i*i;}"
                       "return a[2].x+a[2].y;}", 6 },
  { "self referential struct",
                       "struct N{int v;struct N*next;}; int main(void){"
                       "struct N a,b;a.v=1;b.v=2;a.next=&b;b.next=0;"
                       "return a.next->v;}", 2 },
  { "union",           "union U{int i;char c[4];}; int main(void){union U u;u.i=0;"
                       "u.c[0]=1;u.c[1]=1;return u.i;}", 257 },
  { "enum",            "enum E{A,B,C=10,D}; int main(void){return A+B+C+D;}", 22 },
  { "typedef",         "typedef unsigned char byte; int main(void){byte b=200;"
                       "return b;}", 200 },
  { "typedef struct",  "typedef struct{int a;int b;}pair; int main(void){"
                       "pair p={3,4};return p.a*p.b;}", 12 },
  { "sizeof type",     "struct P{int a;char b;long c;}; int main(void){"
                       "return (int)sizeof(struct P);}", 16 },
  { "sizeof expr",     "int main(void){long v;return (int)sizeof v;}", 8 },
  { "sizeof array",    "int main(void){int a[7];return (int)sizeof a;}", 28 },
  { "global",          "int g=17; int main(void){g+=3;return g;}", 20 },
  { "global array",    "int g[3]={1,2,3}; int main(void){return g[0]+g[1]+g[2];}", 6 },
  { "global string",   "char *m=\"xyz\"; int main(void){return m[2];}", 'z' },
  { "global struct",   "struct P{int x;int y;}; struct P p={4,5};"
                       "int main(void){return p.x*p.y;}", 20 },
  { "static local",    "int bump(void){static int n=0;n++;return n;}"
                       "int main(void){bump();bump();return bump();}", 3 },
  { "unsigned wrap",   "int main(void){unsigned int u=4000000000u;"
                       "return (int)(u/1000000);}", 4000 },
  { "signed division", "int main(void){int a=-7;return a/2;}", -3 },
  { "signed remainder","int main(void){int a=-7;return a%2;}", -1 },
  { "unsigned division",
                       "int main(void){unsigned long a=18446744073709551615UL;"
                       "return (int)(a/3);}", (long long)(int)(18446744073709551615ULL/3) },
  { "int overflow wraps at 32",
                       "int main(void){int a=2000000000;a=a+a;return a;}",
                       (long long)(int)((unsigned)2000000000 + (unsigned)2000000000) },
  { "long does not wrap at 32",
                       "int main(void){long a=2000000000;a=a+a;"
                       "return (int)(a/1000000);}", 4000 },
  { "char is signed",  "int main(void){char c=-1;int i=c;return i;}", -1 },
  { "unsigned char",   "int main(void){unsigned char c=255;int i=c;return i;}", 255 },

  /* THE INTEGER PROMOTIONS. Every row here produces a NEGATIVE intermediate from
   * a narrow unsigned operand, which is the only shape that can tell `promotes
   * to int` (correct, C11 6.3.1.1) from `promotes to unsigned int` (what this
   * compiler used to do). The three narrow-unsigned rows that existed before —
   * "unsigned char", "typedef" and "predeclared types no include" — all give the
   * same number under either rule, which is why 29441 green checks sat on top of
   * `-6 / (unsigned char)2 == 2147483645` for a whole branch. A value that is
   * the same under the wrong rule is not coverage. */
  { "uchar promotes to int",
                       "int main(void){unsigned char u=2;int x=-6;return x/u;}", -3 },
  { "ushort promotes to int",
                       "int main(void){unsigned short u=2;int x=-6;return x/u;}", -3 },
  { "uchar signed compare",
                       "int main(void){unsigned char u=0;int x=-1;return x>u;}", 0 },
  { "uchar signed remainder",
                       "int main(void){unsigned char u=4;int x=-6;return x%u;}", -2 },
  { "uint8_t difference goes negative",
                       "int main(void){uint8_t a=10,b=20;return (a-b)<0;}", 1 },
  { "unary minus on uint8_t",
                       "int main(void){uint8_t a=1;return (-a)<0;}", 1 },
  { "ternary promotes to int",
                       "int main(void){unsigned char u=0;int c=1;"
                       "return (c?-1:u)<0;}", 1 },
  /* The shift's left operand promotes through a SECOND, open-coded copy of the
   * same rule in shift_expr(); it had the same bug and needs its own row. */
  { "short shifts arithmetically",
                       "int main(void){short s=-2;return s>>1;}", -1 },
  { "uint8_t shifts logically",
                       "int main(void){uint8_t u=0x80;return u>>1;}", 0x40 },

  /* THE TYPE OF AN INTEGER CONSTANT, C11 6.4.4.1. The width used to be
   * `v > 0x7fffffff` for every constant regardless of suffix or base, so
   * everything from 2^31 up was 64-bit and `unsigned int` constant arithmetic
   * never wrapped. Note "var form" beside "constant form": the divergence
   * between the two is what made this so hard to see. */
  { "sizeof unsuffixed hex 0xFFFFFFFF",
                       "int main(void){return (int)sizeof(0xFFFFFFFF);}", 4 },
  { "sizeof u-suffixed 2^31",
                       "int main(void){return (int)sizeof(2147483648u);}", 4 },
  { "sizeof u-suffixed 2^32-1",
                       "int main(void){return (int)sizeof(4294967295u);}", 4 },
  { "sizeof unsuffixed decimal 2^32-1",
                       "int main(void){return (int)sizeof(4294967295);}", 8 },
  { "sizeof hex above 2^32",
                       "int main(void){return (int)sizeof(0x100000000);}", 8 },
  { "sizeof l-suffixed 1",
                       "int main(void){return (int)sizeof(1L);}", 8 },
  { "unsigned int constants wrap",
                       "int main(void){return (int)((0xFFFFFFFFu+1u)!=0);}", 0 },
  { "unsigned int variables wrap the same",
                       "int main(void){unsigned int a=0xFFFFFFFFu,b=1u;"
                       "return (int)((a+b)!=0);}", 0 },
  { "complement of 0x80000000",
                       "int main(void){return (int)(~0x80000000==0x7fffffff);}", 1 },
  { "0xFFFFFFFF is -1 as an int",
                       "int main(void){return (int)(0xFFFFFFFF==-1);}", 1 },

  /* A case constant is converted to the controlling expression's type before it
   * is compared (C11 6.8.4.2p5). Unconverted, one side of the emitted cmp obeyed
   * cc_x64.c's 64-bit normalisation invariant and the other did not, so these two
   * labels matched NOTHING and the default arm ran. The second is ordinary C that
   * clang does not warn about. */
  { "switch case wider than the condition",
                       "int main(void){int x=0;switch(x){case 4294967296L: return 1;"
                       "default: return 2;}}", 1 },
  { "switch case -1 against unsigned",
                       "int main(void){unsigned int x=4294967295u;"
                       "switch(x){case -1: return 1; default: return 2;}}", 1 },
  { "short truncates", "int main(void){short s=70000;return s;}", (long long)(short)70000 },
  { "cast truncates",  "int main(void){long a=300;char c=(char)a;return c;}", 44 },
  { "unsigned compare","int main(void){unsigned int a=1;int b=-1;"
                       "return a < (unsigned int)b;}", 1 },
  { "signed compare",  "int main(void){int a=1,b=-1;return a<b;}", 0 },
  { "compound assign", "int main(void){int a=10;a+=5;a-=3;a*=2;a/=4;a%=5;a<<=3;"
                       "a>>=1;a|=1;a&=13;a^=4;return a;}", 1 },
  { "compound on element",
                       "int main(void){int a[3]={1,1,1};int i=0;a[i++]+=10;"
                       "return a[0]*100+a[1]+i;}", 1102 },
  { "prefix increment","int main(void){int i=5;int j=++i;return i*10+j;}", 66 },
  { "postfix increment","int main(void){int i=5;int j=i++;return i*10+j;}", 65 },
  { "pointer increment",
                       "int main(void){int a[4]={9,8,7,6};int*p=a;p++;p++;"
                       "return *p;}", 7 },
  { "decrement",       "int main(void){int i=5;i--;--i;return i;}", 3 },
  { "object macro",    "#define N 6\nint main(void){return N*N;}", 36 },
  { "function macro",  "#define SQ(x) ((x)*(x))\nint main(void){return SQ(7)+SQ(1+1);}", 53 },
  { "nested macro",    "#define A 2\n#define B (A*3)\n#define C(x) ((x)+B)\n"
                       "int main(void){return C(1);}", 7 },
  { "macro not invoked without parens",
                       "#define F(x) 99\nint main(void){int F=5;return F;}", 5 },
  { "undef",           "#define N 1\n#undef N\n#define N 2\nint main(void){return N;}", 2 },
  { "ifdef taken",     "#define ON\n#ifdef ON\nint main(void){return 1;}\n"
                       "#else\nint main(void){return 2;}\n#endif", 1 },
  { "ifndef taken",    "#ifndef OFF\nint main(void){return 3;}\n#else\n"
                       "int main(void){return 4;}\n#endif", 3 },
  { "nested ifdef",    "#define A\n#ifdef A\n#ifndef B\nint main(void){return 9;}\n"
                       "#endif\n#endif", 9 },
  { "builtin header",  "#include <fableos.h>\nint main(void){uint32_t x=7;"
                       "return (int)x;}", 7 },
  { "predeclared types no include",
                       "int main(void){uint8_t a=250;int16_t b=-3;size_t c=4;"
                       "return (int)(a+b+c);}", 251 },
  { "comments",        "int main(void){/* a */ return /*b*/ 5; // c\n}", 5 },
  { "line continuation","int main(void){int a = 1 + \\\n2; return a;}", 3 },
  { "const ignored",   "int main(void){const int a=4;return a;}", 4 },
  { "volatile ignored","int main(void){volatile int a=4;return a+1;}", 5 },
  { "empty statement", "int main(void){;;;return 1;}", 1 },
  { "fall off returns zero",
                       "int f(void){} int main(void){return f();}", 0 },
};

static void test_the_accepted_subset_runs_and_is_right(void) {
    for (size_t i = 0; i < sizeof SUBSET / sizeof SUBSET[0]; i++)
        expect(SUBSET[i].label, SUBSET[i].src, SUBSET[i].want);
}

/* ====================================================================== */
/* 2. every refusal, with its message                                     */
/* ====================================================================== */

typedef struct {
    const char *label, *src, *needle;
    int line, col;
} rej_t;

static const rej_t REJECT[] = {
  /* ---- things outside the subset, each refused BY NAME ---- */
  { "float type",      "int main(void){float f;return 0;}", "floating point", 1, 16 },
  { "double type",     "int main(void){double d;return 0;}", "floating point", 1, 16 },
  { "float literal",   "int main(void){int a=1.5;return a;}",
                       "floating point is not supported: '1.5'", 1, 22 },
  { "exponent literal","int main(void){int a=1e5;return a;}", "floating point", 1, 22 },
  { "varargs",         "int f(int a,...){return a;} int main(void){return f(1);}",
                       "variadic functions", 1, 13 },
  { "function pointer declarator",
                       "int main(void){int (*f)(void);return 0;}",
                       "function pointers are not supported", 1, 20 },
  { "address of function",
                       "int f(void){return 1;} int main(void){long a=(long)&f;"
                       "return (int)a;}", "function pointers are not supported", 1, 53 },
  { "function as value",
                       "int f(void){return 1;} int main(void){int a=f;return a;}",
                       "can only be called", 1, 45 },
  { "goto",            "int main(void){goto x;}", "goto is not supported", 1, 16 },
  { "label",           "int main(void){x: return 1;}", "labels are not supported", 1, 16 },
  { "struct by value parameter",
                       "struct P{int x;}; int f(struct P p){return p.x;}"
                       "int main(void){return 0;}", "cannot be passed by value", 1, 34 },
  { "struct return",   "struct P{int x;}; struct P f(void){struct P p;p.x=1;return p;}"
                       "int main(void){return 0;}", "cannot return a", 1, 28 },
  { "bitfield",        "struct P{int x:3;}; int main(void){return 0;}",
                       "bitfields are not supported", 1, 15 },
  { "hash if",         "#if 1\nint main(void){return 1;}\n#endif", "#if is not supported", 1, 2 },
  { "token paste",     "#define J(a,b) a##b\nint main(void){return 1;}",
                       "not supported", 1, 17 },
  { "stringize",       "#define S(a) #a\nint main(void){return 1;}", "not supported", 1, 14 },
  { "variadic macro",  "#define V(...) 1\nint main(void){return 1;}",
                       "variadic macros", 1, 11 },
  { "hosted header",   "#include <stdio.h>\nint main(void){return 0;}",
                       "no hosted libc", 1, 10 },
  { "unknown header",  "#include \"nope.h\"\nint main(void){return 0;}",
                       "include root", 1, 10 },
  { "bool",            "int main(void){_Bool b;return 0;}", "_Bool is not supported", 1, 16 },
  { "static assert",   "int main(void){_Static_assert(1,\"x\");return 0;}",
                       "not supported", 1, 16 },
  { "compound literal","struct P{int x;}; int main(void){struct P p=(struct P){1};"
                       "return p.x;}", "compound literals", 1, 45 },
  { "designated initialiser",
                       "int main(void){int a[3]={[1]=5};return a[1];}",
                       "designated initialisers", 1, 26 },

  /* ---- real mistakes, refused with the repair in the message ---- */
  { "undeclared variable",
                       "int main(void){return zzz;}", "'zzz' is not declared", 1, 23 },
  { "undeclared function",
                       "int main(void){return printf(1);}",
                       "there is no function called 'printf'", 1, 23 },
  { "unknown member",  "struct P{int x;int y;}; int main(void){struct P p;"
                       "return p.z;}", "has no member 'z'. It has: x, y", 1, 60 },
  { "dot on a pointer","struct P{int x;}; int main(void){struct P*p;return p.x;}",
                       "Use '->' instead", 1, 54 },
  { "arrow on a value","struct P{int x;}; int main(void){struct P p;return p->x;}",
                       "Use '.' instead", 1, 55 },
  { "unknown struct tag",
                       "int main(void){struct Q*p;return 0;}",
                       "there is no struct named 'Q'", 1, 23 },
  { "wrong pointer type",
                       "int main(void){int*p;char*q=0;p=q;return 0;}",
                       "point at different types", 1, 32 },
  { "integer to pointer",
                       "int main(void){int a=1;int*p=a;return 0;}",
                       "a whole number is not a pointer", 1, 30 },
  { "pointer to integer",
                       "int main(void){char*q=0;int a=q;return a;}",
                       "a pointer is not a whole number", 1, 31 },
  { "too few arguments",
                       "int f(int a,int b){return a+b;} int main(void){return f(1);}",
                       "f takes 2 argument(s) but 1 was given", 1, 55 },
  { "too many arguments",
                       "int f(int a){return a;} int main(void){return f(1,2);}",
                       "f takes 1 argument(s) but 2 were given", 1, 47 },
  { "wrong argument type",
                       "int f(int*p){return *p;} int main(void){return f(3);}",
                       "argument 1 of f", 1, 50 },
  { "dereference a number",
                       "int main(void){int a=1;return *a;}", "'*' needs a pointer", 1, 31 },
  { "index a number",  "int main(void){int a=1;return a[0];}", "cannot be indexed", 1, 32 },
  { "address of an expression",
                       "int main(void){int a=1;int*p=&(a+1);return 0;}",
                       "'&' needs something with an address", 1, 30 },
  { "assign to a literal",
                       "int main(void){1=2;return 0;}", "must be something that can "
                       "be assigned to", 1, 17 },
  { "assign to an array",
                       "int main(void){int a[2],b[2];a=b;return 0;}",
                       "an array cannot be assigned to", 1, 31 },
  { "add two pointers","int main(void){int*p=0;int*q=0;long a=(long)(p+q);return 0;}",
                       "two pointers cannot be added", 1, 47 },
  { "void dereference","int main(void){void*p=0;return *(int*)p+*p;}",
                       "cannot be dereferenced", 1, 41 },
  { "return value from void",
                       "void f(void){return 1;} int main(void){f();return 0;}",
                       "returns void", 1, 14 },
  { "no return value","int f(void){return;} int main(void){return f();}",
                       "'return' needs a value", 1, 13 },
  { "no main",         "int f(void){return 1;}", "there is no main()", 0, 0 },
  { "declared not defined",
                       "int helper(int n); int main(void){return helper(1);}",
                       "declared but never defined", 0, 0 },
  { "defined twice",   "int f(void){return 1;} int f(void){return 2;}"
                       "int main(void){return f();}", "is defined twice", 1, 28 },
  { "redeclared local","int main(void){int a=1;int a=2;return a;}",
                       "already declared in this block", 1, 28 },
  { "duplicate parameter name",
                       "int f(int a,int a){return a;} int main(void){return f(1,2);}",
                       "used twice as a parameter name", 1, 17 },
  { "kernel name reused",
                       "void print(char*s){} int main(void){return 0;}",
                       "kernel function this compiler provides", 1, 6 },
  { "break outside a loop",
                       "int main(void){break;}", "only meaningful inside a loop", 1, 16 },
  { "continue outside a loop",
                       "int main(void){continue;}", "only meaningful inside a loop", 1, 16 },
  { "case outside a switch",
                       "int main(void){case 1: return 1;}",
                       "only appear inside a switch", 1, 16 },
  { "duplicate case",  "int main(void){switch(1){case 2: break; case 2: break;}"
                       "return 0;}", "already has a case for 2", 1, 41 },
  { "two defaults",    "int main(void){switch(1){default: break; default: break;}"
                       "return 0;}", "already has a 'default'", 1, 42 },
  /* Duplicates are only visible AFTER the case constants are converted to the
   * controlling expression's type. `case 0` and `case 4294967296L` on an int
   * switch are one label; unconverted, both were silently accepted and neither
   * could ever match. clang makes this a hard error and so does this. */
  { "duplicate case after conversion",
                       "int main(void){int x=0;switch(x){case 0: break;"
                       " case 4294967296L: break;}return 0;}",
                       "already has a case for 0", 1, 49 },
  { "switch on a pointer",
                       "int main(void){char*p=0;switch(p){default: break;}return 0;}",
                       "needs a whole number", 1, 25 },
  { "array with no size",
                       "int main(void){int a[];return 0;}", "needs an array size", 1, 20 },
  { "zero length array",
                       "int main(void){int a[0];return 0;}", "positive size", 1, 23 },
  { "non constant array size",
                       "int main(void){int n=3;int a[n];return 0;}",
                       "not a compile-time constant", 1, 30 },
  { "void object",     "int main(void){void v;return 0;}", "cannot be void", 1, 21 },
  { "int int",         "int main(void){int int a;return 0;}", "'int int'", 1, 24 },
  { "signed unsigned", "int main(void){signed unsigned a;return 0;}",
                       "cannot both appear", 1, 32 },
  { "type name as value",
                       "typedef int myint; int main(void){return myint;}",
                       "is a type name, not a value", 1, 42 },
  { "constant division by zero",
                       "int main(void){int a[1/0];return 0;}", "division by zero", 1, 23 },
  { "extern object",   "extern int nothing; int main(void){return nothing;}",
                       "provides no such object", 1, 12 },

  /* ---- malformed input ---- */
  { "missing semicolon",
                       "int main(void){return 1}", "expected ';'", 1, 24 },
  { "unclosed brace",  "int main(void){return 1;", "never closed", 1, 15 },
  { "unclosed paren",  "int main(void){return (1;}", "expected ')'", 1, 25 },
  { "unclosed string", "int main(void){char*s=\"abc;return 0;}",
                       "never closed", 1, 23 },
  { "unclosed comment","int main(void){/* return 0;}", "never closed", 1, 16 },
  { "unclosed struct", "struct P{int x; int main(void){return 0;}",
                       "a struct member cannot be a function", 1, 21 },
  { "unclosed ifdef",  "#ifdef A\nint main(void){return 1;}",
                       "never closed with #endif", 0, 0 },
  { "stray character", "int main(void){return 1;}@", "no meaning in C", 1, 26 },
  { "empty char literal",
                       "int main(void){return '';}", "a character literal needs one", 1, 24 },
  { "multi char literal",
                       "int main(void){return 'ab';}", "exactly one character", 1, 25 },
  { "bad number suffix",
                       "int main(void){return 12abc;}", "cannot follow a number", 1, 25 },
  { "bad octal digit", "int main(void){return 018;}", "not an octal digit", 1, 25 },
  { "empty source",    "", "defines no functions", 0, 0 },
  { "only a comment",  "/* nothing */", "defines no functions", 0, 0 },
  { "keyword as name", "int main(void){int return;return 0;}", "is a keyword", 1, 20 },
  { "empty struct",    "struct P{}; int main(void){return 0;}", "empty struct", 0, 0 },
  { "seven parameters","int f(int a,int b,int c,int d,int e,int g,int h){return a;}"
                       "int main(void){return 0;}", "at most 6 parameters", 1, 47 },
};

static void test_every_refusal_says_something_useful(void) {
    for (size_t i = 0; i < sizeof REJECT / sizeof REJECT[0]; i++)
        reject(REJECT[i].label, REJECT[i].src,
               REJECT[i].line, REJECT[i].col, REJECT[i].needle);
}

/* ====================================================================== */
/* 3. the rendered diagnostic: three lines, a caret under the column      */
/* ====================================================================== */

static void test_a_diagnostic_renders_with_a_caret_under_the_column(void) {
    static const char *src =
        "int main(void)\n"
        "{\n"
        "    int a = 1;\n"
        "    int *p = a;\n"
        "    return *p;\n"
        "}\n";
    CHECK(compile_only(src) != CC_OK);
    CHECK_EQ(g_r.ndiag, 1);
    CHECK_EQ(g_r.diag[0].line, 4);

    cc_diag_render(&g_r.diag[0], g_diag, sizeof g_diag);
    CHECK_CONTAINS(g_diag, "t.c:4:14:");
    CHECK_CONTAINS(g_diag, "a whole number is not a pointer");
    CHECK_CONTAINS(g_diag, "    int *p = a;");

    /* The caret must sit under the reported column, counting from the four
     * spaces cc_diag_render indents with. */
    const char *last = strrchr(g_diag, '\n');
    CHECK(last != NULL);
    if (last) {
        int caret = -1;
        for (int i = 1; last[i]; i++) if (last[i] == '^') caret = i;
        /* 4 spaces of indent + (col-1) spaces => index col+3 */
        CHECK_EQ(caret, g_r.diag[0].col + 4);
    }

    /* A very long line is windowed, not overflowed, and stays printable. */
    {
        char big[4096];
        size_t o = (size_t)snprintf(big, sizeof big, "int main(void){int a=1;");
        for (int i = 0; i < 200; i++)
            o += (size_t)snprintf(big + o, sizeof big - o, "a=a+%d;", i);
        snprintf(big + o, sizeof big - o, "int*p=a;return 0;}");
        CHECK(compile_only(big) != CC_OK);
        CHECK_EQ(g_r.ndiag, 1);
        CHECK(strlen(g_r.diag[0].text) < CC_SNIP_MAX);
        for (const char *q = g_r.diag[0].text; *q; q++)
            CHECK((unsigned char)*q >= 0x20 && (unsigned char)*q < 0x7f);
        CHECK(g_r.diag[0].caret >= 0);
        CHECK(g_r.diag[0].caret <= (int)strlen(g_r.diag[0].text) + 1);
    }
}

static void test_a_failure_always_carries_a_diagnostic(void) {
    /* Whatever the input, a non-zero return means at least one readable
     * diagnostic. A failure with nothing to read is a compiler bug and this is
     * the assertion that would catch it. */
    static const char *awkward[] = {
        "", "}", "{", "int", "int main", "int main(", "int main(void",
        "int main(void)", "int main(void){", "###", "#", "#define",
        "#include", "#ifdef", "#endif", "typedef", "struct", "struct {",
        "int main(void){return", "int main(void){if", "int main(void){for(;;",
        "*", "()", ";;;", "int x = ;", "int main(void){0[0];}",
        NULL
    };
    for (int i = 0; awkward[i]; i++) {
        int rc = cc_compile(awkward[i], strlen(awkward[i]), "t.c", &g_r);
        CHECK(rc != CC_OK);
        CHECK(g_r.ndiag >= 1);
        CHECK(g_r.diag[0].msg[0] != '\0');
        CHECK_EQ(kpanic_hit, 0);
    }
}

/* ====================================================================== */
/* 4. the two budgets: fuel and the private stack                         */
/* ====================================================================== */

static void test_an_endless_loop_stops_instead_of_hanging(void) {
    if (!g_can_exec) { g_skipped++; return; }

    static const char *src = "int main(void){while(1){}return 1;}";
    int rc = cc_compile_run(src, strlen(src), "t.c", NULL, 0, 5000, &g_r);
    CHECK_EQ(rc, CC_EFUEL);
    CHECK_EQ(g_r.fuel_used, 5000);
    CHECK_EQ(g_r.ran, 1);

    /* And a `for(;;)`, and a recursion with no base case, which spends fuel on
     * calls rather than back-edges. */
    static const char *forever = "int main(void){for(;;){int a=1;a=a+1;}return 1;}";
    CHECK_EQ(cc_compile_run(forever, strlen(forever), "t.c", NULL, 0, 4000, &g_r),
             CC_EFUEL);

    /* The machine is still usable afterwards, which is the whole point. */
    expect("alive after fuel exhaustion", "int main(void){return 123;}", 123);
}

static void test_runaway_recursion_stops_at_its_own_stack(void) {
    if (!g_can_exec) { g_skipped++; return; }

    /* Each frame is large enough that fuel is not what stops this. */
    static const char *src =
        "int f(int n){int pad[64];pad[0]=n;return f(n+1)+pad[0];}"
        "int main(void){return f(1);}";
    int rc = cc_compile_run(src, strlen(src), "t.c", NULL, 0, CC_FUEL_MAX, &g_r);
    CHECK_EQ(rc, CC_ESTACK);
    /* It got deep, but not deeper than the window it was given. */
    CHECK(g_r.stack_used > (uint64_t)(CC_STACK_BYTES / 2));
    CHECK(g_r.stack_used <= (uint64_t)CC_STACK_BYTES);

    expect("alive after stack exhaustion", "int main(void){return 77;}", 77);
}

static void test_a_frame_bigger_than_the_guard_is_refused_at_compile_time(void) {
    char src[256];
    snprintf(src, sizeof src, "int main(void){char big[%d];big[0]=1;return big[0];}",
             CC_MAX_FRAME + 64);
    reject("oversize frame", src, 0, 0, "one function may use at most");

    /* And one that fits still works, so the limit is a limit and not a wall. */
    snprintf(src, sizeof src,
             "int main(void){char b[%d];b[0]=7;b[%d]=8;return b[0]+b[%d];}",
             CC_MAX_FRAME - 128, CC_MAX_FRAME - 129, CC_MAX_FRAME - 129);
    expect("frame just under the limit", src, 15);
}

static void test_fuel_is_charged_per_iteration_and_per_call(void) {
    if (!g_can_exec) { g_skipped++; return; }

    /* A loop of exactly 100 iterations costs 100 back-edges plus 1 for main's
     * own entry. Asserting the exact number is what makes "fuel bounds the
     * program" a measurement rather than a hope. */
    static const char *loop = "int main(void){int i;for(i=0;i<100;i++){}return i;}";
    CHECK_EQ(cc_compile_run(loop, strlen(loop), "t.c", NULL, 0, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 100);
    CHECK_EQ(g_r.fuel_used, 101);

    static const char *calls = "int f(void){return 1;} "
                               "int main(void){int i,s=0;for(i=0;i<10;i++)s+=f();"
                               "return s;}";
    CHECK_EQ(cc_compile_run(calls, strlen(calls), "t.c", NULL, 0, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 10);
    /* 1 (main) + 10 back-edges + 10 calls + 10 callee entries */
    CHECK_EQ(g_r.fuel_used, 31);

    /* Fuel above the maximum is clamped, not honoured. */
    CHECK_EQ(cc_compile_run(loop, strlen(loop), "t.c", NULL, 0,
                            CC_FUEL_MAX * 4, &g_r), CC_OK);
    CHECK_EQ(g_r.fuel_used, 101);
}

/* ====================================================================== */
/* 5. calling the kernel                                                  */
/* ====================================================================== */

static void test_a_program_can_call_the_kernel(void) {
    if (!g_can_exec) { g_skipped++; return; }

    static const char *src =
        "int main(void){"
        "print(\"hello from compiled C\\n\");"
        "print(\"n=\");print_num(-42);print(\" h=\");print_hex(255);"
        "print_char('!');print(\"\\n\");"
        "return (int)strlen(\"abcdef\");}";
    kcap_reset();
    CHECK_EQ(cc_compile_run(src, strlen(src), "t.c", NULL, 0, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 6);
    CHECK_CONTAINS(g_r.out, "hello from compiled C");
    CHECK_CONTAINS(g_r.out, "n=-42 h=0xff!");
    CHECK(g_r.ncalls >= 8);

    /* It reached the real console too, with the prefix that keeps column zero
     * the kernel's. */
    CHECK_CONTAINS(kcap_text(), "cc| hello from compiled C");
}

static void test_console_output_cannot_forge_a_kernel_trace_line(void) {
    if (!g_can_exec) { g_skipped++; return; }

    /* A program that prints exactly what a trace line looks like, on its own
     * line, with control bytes and a very long tail. Every line of console
     * output must still begin with something that is not '['. */
    static const char *src =
        "int main(void){"
        "print(\"[vfs_write fd=3 bytes=34 -> ok]\\n\");"
        "print(\"\\n[cc.run -> ok]\\n\");"
        "print(\"\\x01\\x02\\x1b[31m red \\x07\\n\");"
        "return 0;}";
    kcap_reset();
    CHECK_EQ(cc_compile_run(src, strlen(src), "t.c", NULL, 0, 0, &g_r), CC_OK);

    const char *text = kcap_text();
    int lines = 0;
    for (const char *p = text; *p; ) {
        CHECK(*p != '[');
        lines++;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    CHECK(lines >= 3);
    /* The escape sequence is gone: forced to printable ASCII inside the
     * forwarder, before the kernel ever sees it. */
    CHECK(strstr(text, "\x1b[31m") == NULL);
    CHECK_CONTAINS(text, "cc| [vfs_write fd=3 bytes=34 -> ok]");
}

static void test_a_program_keeps_a_file_but_only_in_its_own_directory(void) {
    if (!g_can_exec) { g_skipped++; return; }

    static const char *src =
        "int main(void){"
        "char buf[32];char back[32];long i;"
        "for(i=0;i<12;i++)buf[i]=(char)('a'+i);"
        "if(file_write(\"kept.dat\",buf,12)!=12)return -1;"
        "if(file_size(\"kept.dat\")!=12)return -2;"
        "memset(back,0,32);"
        "if(file_read(\"kept.dat\",back,32)!=12)return -3;"
        "if(memcmp(buf,back,12)!=0)return -4;"
        "return back[11];}";
    CHECK_EQ(cc_compile_run(src, strlen(src), "t.c", NULL, 0, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 'l');

    /* The bytes really landed in the VFS, under the data root and nowhere else. */
    {
        char path[VFS_PATH_MAX + 1];
        snprintf(path, sizeof path, "%s/kept.dat", cc_data_root());
        vfs_stat_t st;
        CHECK_EQ(vfs_stat(path, &st), VFS_OK);
        CHECK_EQ(st.size, 12);
    }

    /* Every shape of escape a program might try is REFUSED, counted, and leaves
     * nothing behind. A name is not a path here, so there is no traversal to
     * resolve — the refusal is structural. */
    static const char *escapes[] = {
        "/cc/square.cc", "../cc/square.cc", "..", ".", "a/b", "/etc/passwd",
        "sub/../../x", ".hidden", "", "name with space", "semi;colon",
        NULL
    };
    for (int i = 0; escapes[i]; i++) {
        char probe[512];
        snprintf(probe, sizeof probe,
                 "int main(void){return (int)file_write(\"%s\",\"x\",1);}",
                 escapes[i]);
        int rc = cc_compile_run(probe, strlen(probe), "t.c", NULL, 0, 0, &g_r);
        if (rc != CC_OK) continue;              /* refused at compile time is fine */
        CHECK_EQ((long)(int64_t)g_r.value, -1);
    }
    /* The store is untouched: no file with a program's name was created by any
     * of the above. */
    {
        char path[VFS_PATH_MAX + 1];
        snprintf(path, sizeof path, "%s/square.cc", cc_store_root());
        vfs_stat_t st;
        int existed = (vfs_stat(path, &st) == VFS_OK);
        /* square may legitimately exist from a store test; what must NOT exist
         * is a file whose contents came from the escape attempts. */
        if (existed) CHECK(st.size > 1);
    }
}

static void test_a_span_longer_than_the_bound_is_refused_not_clamped(void) {
    if (!g_can_exec) { g_skipped++; return; }

    static const char *src =
        "int main(void){char b[64];"
        "memset(b,0,64);"
        "memset(b,65,1000000);"          /* refused: above the span bound  */
        "memcpy(b,b,-1);"                /* refused: negative             */
        "return b[0];}";
    CHECK_EQ(cc_compile_run(src, strlen(src), "t.c", NULL, 0, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 0);              /* the refused memset did nothing */
    CHECK_EQ(cc_rt_refusals(), 2);

    /* strlen on a string with no terminator inside the bound returns 0 rather
     * than reading until it faults. */
    static const char *nostop =
        "char big[4096]; int main(void){long i;"
        "for(i=0;i<4096;i++)big[i]='x';"
        "return (int)strlen(big);}";
    CHECK_EQ(cc_compile_run(nostop, strlen(nostop), "t.c", NULL, 0, 0, &g_r), CC_OK);
    /* The array is 4096 non-NUL bytes; whatever follows it in the data image is
     * this compiler's own, so the only assertion worth making is that it
     * returned a bounded number and did not crash. */
    CHECK(g_r.value <= 65536);
}

/* ====================================================================== */
/* 6. the shipped examples, compiled verbatim                             */
/* ====================================================================== */

static char g_file[24576];

static void test_the_shipped_examples_compile(void) {
    size_t n;

    n = slurp("compiler/examples/square.c", g_file, sizeof g_file);
    if (n) {
        uint64_t a[1] = { 7 };
        int rc = cc_compile(g_file, n, "square.c", &g_r);
        if (rc != CC_OK) {
            cc_diags_render(&g_r, g_diag, sizeof g_diag);
            th_checks++; th_fails++;
            printf("    FAIL square.c:\n%s\n", g_diag);
        } else if (g_can_exec) {
            CHECK_EQ(g_r.entry_params, 1);
            CHECK_EQ(cc_run(a, 1, 0, &g_r), CC_OK);
            CHECK_EQ(g_r.value, 49);
            CHECK_CONTAINS(g_r.out, "square(7) = 49");
        } else g_skipped++;
    }

    n = slurp("compiler/examples/histogram.c", g_file, sizeof g_file);
    if (n) {
        uint64_t a[1] = { 100 };
        int rc = cc_compile(g_file, n, "histogram.c", &g_r);
        if (rc != CC_OK) {
            cc_diags_render(&g_r, g_diag, sizeof g_diag);
            th_checks++; th_fails++;
            printf("    FAIL histogram.c:\n%s\n", g_diag);
        } else if (g_can_exec) {
            CHECK_EQ(cc_run(a, 1, 0, &g_r), CC_OK);
            CHECK_EQ(g_r.value, 13);
            CHECK_CONTAINS(g_r.out, "histogram of 100 values");
            CHECK_CONTAINS(g_r.out, "read back: identical");
            /* The README quotes this number, so it is asserted here. */
            CHECK_EQ(g_r.code_bytes, 2856);
        } else g_skipped++;
    }

    /* notes.c needs the header a compiled program would have written. Without it
     * the diagnostic must name the file; with it the program runs. */
    n = slurp("compiler/examples/notes.c", g_file, sizeof g_file);
    if (n) {
        char saved[24576];
        size_t saved_n = n;
        memcpy(saved, g_file, n + 1);

        cc_set_include_reader(NULL);
        CHECK(cc_compile(saved, saved_n, "notes.c", &g_r) != CC_OK);
        CHECK_CONTAINS(g_r.diag[0].msg, "shared.h");
        cc_set_include_reader(include_reader);

        CHECK(cc_compile(saved, saved_n, "notes.c", &g_r) != CC_OK);
        CHECK_CONTAINS(g_r.diag[0].msg, "include root");

        {
            static const char *hdr =
                "#define GREETING \"hello from a header this machine wrote\"\n"
                "#define LIMIT 5\n";
            char path[VFS_PATH_MAX + 1];
            snprintf(path, sizeof path, "%s/shared.h", cc_data_root());
            file_t *f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
            CHECK(f != NULL);
            if (f) {
                CHECK_EQ(vfs_write(f, hdr, strlen(hdr)), (int64_t)strlen(hdr));
                vfs_close(f);
            }
        }
        int rc = cc_compile(saved, saved_n, "notes.c", &g_r);
        if (rc != CC_OK) {
            cc_diags_render(&g_r, g_diag, sizeof g_diag);
            th_checks++; th_fails++;
            printf("    FAIL notes.c with its header:\n%s\n", g_diag);
        } else if (g_can_exec) {
            CHECK_EQ(cc_run(NULL, 0, 0, &g_r), CC_OK);
            CHECK_EQ(g_r.value, 5);
            CHECK_CONTAINS(g_r.out, "hello from a header this machine wrote");
        } else g_skipped++;
    }

    /* broken.c must NOT compile, and the first diagnostic is the float. */
    n = slurp("compiler/examples/broken.c", g_file, sizeof g_file);
    if (n) {
        CHECK(cc_compile(g_file, n, "broken.c", &g_r) != CC_OK);
        CHECK_EQ(g_r.ndiag, 1);
        CHECK_CONTAINS(g_r.diag[0].msg, "floating point is not supported: '0.5'");
    }
}

/* The five diagnostics compiler/examples/README.md quotes, each from the mistake
 * on its own. If a message here changes, that file is stale. */
static void test_the_five_documented_diagnostics_are_exact(void) {
    reject("README 1",
           "struct point { int x; int y; };\n"
           "static double half = 0.5;\n"
           "long main(void){return 0;}",
           2, 22, "floating point is not supported: '0.5'");
    reject("README 2",
           "struct point { int x; int y; };\n"
           "static int bad_member(struct point *p)\n"
           "{\n"
           "    return p->z;\n"
           "}\n"
           "long main(void){return bad_member(0);}",
           4, 15, "'struct point' has no member 'z'. It has: x, y");
    reject("README 3",
           "static int wrong_pointer(char *text)\n"
           "{\n"
           "    int *numbers = text;\n"
           "    return numbers[0];\n"
           "}\n"
           "long main(void){return wrong_pointer(0);}",
           3, 20, "cannot use 'char *' where 'int *' is needed");
    reject("README 4",
           "static int scale(int value, int factor)\n"
           "{\n"
           "    return value * factor;\n"
           "}\n"
           "long main(void)\n"
           "{\n"
           "    return scale(10);\n"
           "}",
           7, 12, "scale takes 2 argument(s) but 1 was given");
    reject("README 5",
           "long main(void)\n"
           "{\n"
           "    int i = 0;\n"
           "again:\n"
           "    i++;\n"
           "    if (i < 10)\n"
           "        goto again;\n"
           "    return i;\n"
           "}",
           4, 1, "labels are not supported");
}

/* The worked example inside cc_compile's own description must run. That text is
 * a user interface and a model will copy it verbatim; if it is wrong, every
 * first attempt is wrong. */
static void test_the_documented_example_runs(void) {
    static const char *src =
        "int main(long n){long i,s=0;for(i=1;i<=n;i++)s+=i*i;"
        "print(\"sum of squares: \");print_num(s);return s;}";
    int rc = cc_compile(src, strlen(src), "main.c", &g_r);
    CHECK_EQ(rc, CC_OK);
    CHECK_EQ(g_r.entry_params, 1);
    if (rc == CC_OK && g_can_exec) {
        uint64_t a[1] = { 10 };
        CHECK_EQ(cc_run(a, 1, 0, &g_r), CC_OK);
        CHECK_EQ(g_r.value, 385);
        CHECK_CONTAINS(g_r.out, "sum of squares: 385");
    } else if (rc == CC_OK) g_skipped++;

    /* And the description quotes it as JSON, so the JSON must parse and its
     * "source" must be that program. */
    const tool_t *t = tool_find("cc_compile");
    CHECK(t != NULL);
    if (t) {
        const char *ex = strstr(t->description, "EXAMPLE ");
        CHECK(ex != NULL);
        if (ex) {
            json_value_t v;
            CHECK_EQ(json_parse_str(ex + 8, &v), JSON_OK);
            char got[512];
            CHECK_EQ(json_get_str(&v, "source", got, sizeof got), JSON_OK);
            CHECK_STR(got, src);
        }
    }
}

/* ====================================================================== */
/* 7. the store: save, reboot, call                                       */
/* ====================================================================== */

static const char *SQUARE_SRC =
    "long main(long n){long r=n*n;print(\"square=\");print_num(r);return r;}";

static void reboot_and_reload(void) {
    /* What a reboot is, from this module's point of view: the registry is gone
     * and the only thing that carries over is the bytes on disk. */
    cc_init();
}

static void test_a_saved_program_survives_a_reboot(void) {
    char msg[256];
    const char *params[1] = { "n" };

    CHECK_EQ(cc_save("square", "square(n): n*n", params, 1,
                     SQUARE_SRC, strlen(SQUARE_SRC), &g_r, msg, sizeof msg), CC_OK);
    CHECK_CONTAINS(msg, "square");
    CHECK(cc_find("square") != NULL);
    CHECK_EQ(cc_find("square")->version, 1);
    CHECK_EQ(cc_find("square")->nparam, 1);

    /* The record is a real file with the documented layout. */
    {
        char path[VFS_PATH_MAX + 1];
        snprintf(path, sizeof path, "%s/square.cc", cc_store_root());
        file_t *f = vfs_open(path, O_RDONLY);
        CHECK(f != NULL);
        if (f) {
            char buf[CC_RECORD_MAX + 1];
            int64_t n = vfs_read(f, buf, sizeof buf - 1);
            vfs_close(f);
            CHECK(n > 18);
            buf[n] = '\0';
            CHECK_EQ(buf[16], '\n');
            for (int i = 0; i < 16; i++)
                CHECK((buf[i] >= '0' && buf[i] <= '9') ||
                      (buf[i] >= 'a' && buf[i] <= 'f'));
            json_value_t v;
            CHECK_EQ(json_parse(buf + 17, (size_t)n - 17, &v), JSON_OK);
            char nm[CC_NAME_MAX];
            CHECK_EQ(json_get_str(&v, "name", nm, sizeof nm), JSON_OK);
            CHECK_STR(nm, "square");
            /* The SOURCE is what is kept, not the machine code. */
            CHECK_CONTAINS(buf + 17, "print_num");
        }
    }

    reboot_and_reload();
    CHECK_EQ(cc_rejected(), 0);
    CHECK(cc_find("square") != NULL);
    CHECK_EQ(cc_find("square")->version, 1);

    if (!g_can_exec) { g_skipped++; return; }
    uint64_t a[1] = { 9 };
    CHECK_EQ(cc_call("square", a, 1, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 81);
    CHECK_CONTAINS(g_r.out, "square=81");

    /* Reading the source back is how a later turn repairs it. */
    char back[CC_SRC_MAX + 1];
    int got = cc_source("square", back, sizeof back);
    CHECK_EQ(got, (int)strlen(SQUARE_SRC));
    CHECK_STR(back, SQUARE_SRC);
}

static void test_the_cached_image_is_never_the_wrong_program(void) {
    if (!g_can_exec) { g_skipped++; return; }

    char msg[256];
    const char *pn[1] = { "n" };
    static const char *cube =
        "long main(long n){return n*n*n;}";
    CHECK_EQ(cc_save("cube", "cube(n): n*n*n", pn, 1, cube, strlen(cube),
                     &g_r, msg, sizeof msg), CC_OK);

    uint64_t a[1] = { 3 };
    CHECK_EQ(cc_call("square", a, 1, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 9);
    CHECK_EQ(cc_call("cube", a, 1, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 27);
    CHECK_EQ(cc_call("square", a, 1, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 9);

    /* An anonymous compile between two calls invalidates the cache. Without the
     * compile serial in cc_store.c, this is where the wrong bytes would run. */
    CHECK_EQ(cc_call("cube", a, 1, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 27);
    CHECK_EQ(compile_only("int main(void){return 1;}"), CC_OK);
    CHECK_EQ(cc_call("cube", a, 1, 0, &g_r), CC_OK);
    CHECK_EQ(g_r.value, 27);
}

static void test_replacing_a_program_is_a_new_version_with_a_backup(void) {
    char msg[256];
    const char *pn[1] = { "n" };
    static const char *v2 = "long main(long n){return n*n+1;}";

    CHECK_EQ(cc_save("square", "square(n): n*n plus one now", pn, 1,
                     v2, strlen(v2), &g_r, msg, sizeof msg), CC_OK);
    CHECK_EQ(cc_find("square")->version, 2);
    CHECK_CONTAINS(msg, "ONE level of undo");
    /* One registry slot, not two. */
    int n = 0;
    for (int i = 0; i < cc_count(); i++)
        if (strcmp(cc_at(i)->name, "square") == 0) n++;
    CHECK_EQ(n, 1);

    if (g_can_exec) {
        uint64_t a[1] = { 5 };
        CHECK_EQ(cc_call("square", a, 1, 0, &g_r), CC_OK);
        CHECK_EQ(g_r.value, 26);           /* the NEW code ran */
    } else g_skipped++;

    /* And the backup holds v1. */
    char path[VFS_PATH_MAX + 1];
    snprintf(path, sizeof path, "%s/square.bak", cc_store_root());
    vfs_stat_t st;
    CHECK_EQ(vfs_stat(path, &st), VFS_OK);
}

static void test_a_corrupted_record_is_rejected_and_the_backup_used(void) {
    char path[VFS_PATH_MAX + 1];
    snprintf(path, sizeof path, "%s/square.cc", cc_store_root());

    char buf[CC_RECORD_MAX + 1];
    size_t len = 0;
    {
        file_t *f = vfs_open(path, O_RDONLY);
        CHECK(f != NULL);
        if (!f) return;
        int64_t n = vfs_read(f, buf, sizeof buf - 1);
        vfs_close(f);
        CHECK(n > 40);
        len = (size_t)n;
    }

    /* Flip one byte deep inside the record: the checksum must catch it. */
    char damaged[CC_RECORD_MAX + 1];
    memcpy(damaged, buf, len);
    damaged[len - 8] ^= 0x20;
    {
        file_t *f = vfs_open(path, O_WRONLY | O_TRUNC);
        CHECK(f != NULL);
        if (f) { vfs_write(f, damaged, len); vfs_close(f); }
    }

    kcap_reset();
    reboot_and_reload();
    CHECK(cc_rejected() >= 1);
    CHECK_CONTAINS(kcap_text(), "REJECTED");
    CHECK_CONTAINS(kcap_text(), "checksum mismatch");
    CHECK_CONTAINS(kcap_text(), "recovered from its backup");

    /* The recovered version is v1, and its SOURCE comes from the backup too —
     * loading metadata from one file and code from another is exactly the shape
     * of bug this test exists for. */
    CHECK(cc_find("square") != NULL);
    if (cc_find("square")) CHECK_EQ(cc_find("square")->version, 1);
    if (g_can_exec) {
        uint64_t a[1] = { 6 };
        CHECK_EQ(cc_call("square", a, 1, 0, &g_r), CC_OK);
        CHECK_EQ(g_r.value, 36);          /* v1's code, not v2's 37 */
    } else g_skipped++;

    /* Every one of the 17 header bytes, swept individually. */
    for (int i = 0; i < 17; i++) {
        memcpy(damaged, buf, len);
        damaged[i] = (damaged[i] == 'z') ? 'y' : 'z';
        file_t *f = vfs_open(path, O_WRONLY | O_TRUNC);
        if (f) { vfs_write(f, damaged, len); vfs_close(f); }
        kcap_reset();
        cc_init();
        CHECK(cc_rejected() >= 1);
        CHECK_EQ(kpanic_hit, 0);
    }

    /* Truncated, empty and malformed records: each refused, none fatal. */
    static const size_t cuts[] = { 0, 1, 16, 17, 18, 30 };
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        file_t *f = vfs_open(path, O_WRONLY | O_TRUNC);
        if (f) { vfs_write(f, buf, cuts[i]); vfs_close(f); }
        kcap_reset();
        cc_init();
        CHECK_EQ(kpanic_hit, 0);
        CHECK(cc_rejected() >= 1);
    }

    /* Restore the good record so later tests start from a known store. */
    {
        file_t *f = vfs_open(path, O_WRONLY | O_TRUNC);
        if (f) { vfs_write(f, buf, len); vfs_close(f); }
    }
    cc_init();
    CHECK_EQ(cc_rejected(), 0);
}

static void test_a_record_under_the_wrong_name_is_refused(void) {
    char src[VFS_PATH_MAX + 1], dst[VFS_PATH_MAX + 1];
    snprintf(src, sizeof src, "%s/square.cc", cc_store_root());
    snprintf(dst, sizeof dst, "%s/impostor.cc", cc_store_root());

    char buf[CC_RECORD_MAX + 1];
    size_t len = 0;
    file_t *f = vfs_open(src, O_RDONLY);
    CHECK(f != NULL);
    if (!f) return;
    int64_t n = vfs_read(f, buf, sizeof buf - 1);
    vfs_close(f);
    len = (size_t)(n < 0 ? 0 : n);

    f = vfs_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) { vfs_write(f, buf, len); vfs_close(f); }

    kcap_reset();
    cc_init();
    CHECK(cc_rejected() >= 1);
    CHECK_CONTAINS(kcap_text(), "the two disagree");
    CHECK(cc_find("impostor") == NULL);

    /* Clean up through the tool, which is also a test of cc_delete. */
    char msg[256];
    CHECK_EQ(cc_delete("impostor", msg, sizeof msg), CC_ENOENT);
    {
        /* It is not in the registry, so delete it from the store directly. */
        file_t *d = vfs_open(cc_store_root(), O_RDONLY);
        if (d && d->node && d->node->ops && d->node->ops->unlink)
            d->node->ops->unlink(d->node, "impostor.cc");
        if (d) vfs_close(d);
    }
    cc_init();
    CHECK_EQ(cc_rejected(), 0);
}

static void test_bad_saves_change_nothing(void) {
    char msg[256];
    const char *pn[2] = { "a", "b" };
    int before = cc_count();

    struct { const char *name, *doc, *src; int nparam; const char *needle; } bad[] = {
      { "Square", "doc", "int main(void){return 1;}", 0, "lowercase" },
      { "9lives", "doc", "int main(void){return 1;}", 0, "lowercase letter" },
      { "has-dash", "doc", "int main(void){return 1;}", 0, "not allowed" },
      { "ok", "", "int main(void){return 1;}", 0, "\"doc\" is required" },
      { "ok", "line\nbreak", "int main(void){return 1;}", 0, "one line" },
      { "ok", "doc", "int main(void){return zzz;}", 0, "does not compile" },
      { "ok", "doc", "", 0, "does not compile" },
      { "ok", "doc", "int main(void){return 1;}", 2, "\"params\" names 2" },
      { "ok", "doc", "int main(int a,int b){return a+b;}", 1, "\"params\" names 1" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = cc_save(bad[i].name, bad[i].doc, pn, bad[i].nparam,
                         bad[i].src, strlen(bad[i].src), &g_r, msg, sizeof msg);
        CHECK(rc != CC_OK);
        CHECK_CONTAINS(msg, bad[i].needle);
        CHECK(cc_find(bad[i].name) == NULL || strcmp(bad[i].name, "square") == 0);
    }
    CHECK_EQ(cc_count(), before);
    reboot_and_reload();
    CHECK_EQ(cc_count(), before);
}

static void test_the_registry_fills_up_and_says_so(void) {
    char msg[256];
    static const char *src = "int main(void){return 1;}";
    int added = 0;
    while (cc_count() < CC_MAX_PROGRAMS) {
        char name[CC_NAME_MAX];
        snprintf(name, sizeof name, "filler%d", added++);
        CHECK_EQ(cc_save(name, "filler", NULL, 0, src, strlen(src), &g_r,
                         msg, sizeof msg), CC_OK);
        CHECK(added < CC_MAX_PROGRAMS + 2);
    }
    CHECK_EQ(cc_count(), CC_MAX_PROGRAMS);
    CHECK_EQ(cc_save("onetoomany", "no room", NULL, 0, src, strlen(src), &g_r,
                     msg, sizeof msg), CC_ENOSPC);
    CHECK_CONTAINS(msg, "Delete one first");
    CHECK_EQ(cc_count(), CC_MAX_PROGRAMS);

    /* Replacing an existing name still works when full: it is not a new slot. */
    const char *pn[1] = { "n" };
    CHECK_EQ(cc_save("square", "square(n): n*n", pn, 1, SQUARE_SRC,
                     strlen(SQUARE_SRC), &g_r, msg, sizeof msg), CC_OK);
    CHECK_EQ(cc_count(), CC_MAX_PROGRAMS);

    /* And a delete makes room again. */
    CHECK_EQ(cc_delete("filler0", msg, sizeof msg), CC_OK);
    CHECK_EQ(cc_count(), CC_MAX_PROGRAMS - 1);
    CHECK(cc_find("filler0") == NULL);
    reboot_and_reload();
    CHECK_EQ(cc_count(), CC_MAX_PROGRAMS - 1);
    CHECK(cc_find("filler0") == NULL);
    CHECK_EQ(cc_delete("filler0", msg, sizeof msg), CC_ENOENT);
}

/* ====================================================================== */
/* 8. the panel, and the fixed schema size                                */
/* ====================================================================== */

static void test_the_panel_is_exactly_the_width_the_schema_reserved(void) {
    char why[256];
    CHECK_EQ((int)strlen(cc_panel()), CC_PANEL_CHARS);
    CHECK_EQ(cc_panel_check(why, sizeof why), 1);
    for (int i = 0; i < CC_PANEL_CHARS; i++) {
        unsigned char c = (unsigned char)cc_panel()[i];
        CHECK(c >= 0x20 && c < 0x7f);
        CHECK(c != '"');
        CHECK(c != '\\');
    }
    /* A full panel ends with a pointer at the tool that shows the rest, never
     * half a name. */
    if (cc_count() >= CC_MAX_PROGRAMS - 1)
        CHECK_CONTAINS(cc_panel(), "more (cc_source)");
}

/* THE invariant: the assembled schema is the same size whatever is on the disk.
 * Without it CHAT_REGISTRY_BYTES is a function of what the operator saved last
 * night, and three QEMU cases go red at the next boot. */
static void test_the_schema_size_does_not_depend_on_the_store(void) {
    static char a[131072], b[131072];
    char msg[256];

    /* Empty store. */
    while (cc_count() > 0) CHECK_EQ(cc_delete(cc_at(0)->name, msg, sizeof msg), CC_OK);
    CHECK_EQ(cc_count(), 0);
    size_t na = tools_build_schema(a, sizeof a);
    CHECK_EQ((int)strlen(cc_panel()), CC_PANEL_CHARS);

    /* One program. */
    const char *pn[1] = { "n" };
    CHECK_EQ(cc_save("square", "square(n): n*n", pn, 1, SQUARE_SRC,
                     strlen(SQUARE_SRC), &g_r, msg, sizeof msg), CC_OK);
    size_t nb = tools_build_schema(b, sizeof b);
    /* The SIZE is the invariant, not the bytes: the panel's CONTENT is supposed
     * to change when a program is saved - that is the whole point of it. What
     * must not change is how many bytes the assembled schema occupies, because
     * that number is a compile-time constant in include/chat.h. */
    CHECK_EQ(na, nb);

    /* A full store, with the longest names and docs the store accepts. */
    while (cc_count() < CC_MAX_PROGRAMS) {
        char name[CC_NAME_MAX], doc[CC_DOC_MAX];
        snprintf(name, sizeof name, "wwwwwwwwwwwwwwwwwwwwwwwwwwwww%d", cc_count());
        for (size_t i = 0; i < sizeof doc - 1; i++) doc[i] = 'M';
        doc[sizeof doc - 1] = '\0';
        static const char *src = "int main(void){return 1;}";
        CHECK_EQ(cc_save(name, doc, NULL, 0, src, strlen(src), &g_r,
                         msg, sizeof msg), CC_OK);
    }
    size_t nc = tools_build_schema(b, sizeof b);
    CHECK_EQ(na, nc);
    /* Everything OUTSIDE the panel must be byte-identical, which is what proves
     * the difference is confined to the padded window. */
    {
        const char *pa = strstr(a, "SAVED HERE"), *pb = strstr(b, "SAVED HERE");
        CHECK(pa != NULL && pb != NULL);
        if (pa && pb) {
            CHECK_EQ((long)(pa - a), (long)(pb - b));
            CHECK_EQ(memcmp(a, b, (size_t)(pa - a)), 0);
            size_t after = (size_t)(pa - a) + 12 + CC_PANEL_CHARS;
            if (after < na) CHECK_EQ(memcmp(a + after, b + after, na - after), 0);
        }
    }
    CHECK_EQ((int)strlen(cc_panel()), CC_PANEL_CHARS);
    CHECK_EQ(cc_panel_check(msg, sizeof msg), 1);

    /* Clean up, then re-establish square for the tool tests. */
    while (cc_count() > 0) CHECK_EQ(cc_delete(cc_at(0)->name, msg, sizeof msg), CC_OK);
    CHECK_EQ(cc_save("square", "square(n): n*n and says so", pn, 1, SQUARE_SRC,
                     strlen(SQUARE_SRC), &g_r, msg, sizeof msg), CC_OK);
}

/* ====================================================================== */
/* 9. the tools                                                           */
/* ====================================================================== */

static char g_tool_buf[TOOL_RESULT_MAX];

static int call_tool(const char *name, const char *input, tool_result_t *r) {
    tool_result_init(r, g_tool_buf, sizeof g_tool_buf);
    tool_call_t c = { .id = "toolu_test", .name = name,
                      .input = input, .input_len = input ? strlen(input) : 0 };
    return tool_dispatch(&c, r);
}

static void test_the_tools_are_registered_and_their_schemas_parse(void) {
    static const char *names[] = { "cc_compile", "cc_call", "cc_source", NULL };
    for (int i = 0; names[i]; i++) {
        const tool_t *t = tool_find(names[i]);
        CHECK(t != NULL);
        if (!t) continue;
        CHECK(t->description && strlen(t->description) > 40);
        json_value_t v;
        CHECK_EQ(json_parse_str(t->input_schema, &v), JSON_OK);
        CHECK_EQ(v.type, JSON_OBJECT);
        /* A description must never contain a raw control byte: it is JSON-escaped
         * into the request, and a '\n' is fine but a 0x01 is not. */
        for (const char *p = t->description; *p; p++)
            CHECK((unsigned char)*p >= 0x20 || *p == '\n');
    }
    CHECK_TOOL_FLAGS("cc_compile", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("cc_call", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("cc_source", TOOL_MUTATES, 0);
}

static void test_cc_compile_end_to_end_through_the_tool(void) {
    tool_result_t r;
    CHECK_EQ(call_tool("cc_compile",
                       "{\"source\":\"int main(long a,long b){"
                       "print(\\\"a+b=\\\");print_num(a+b);return a+b;}\","
                       "\"args\":[20,22]}", &r), TOOL_OK);
    if (g_can_exec) {
        CHECK_EQ(r.is_error, 0);
        CHECK_CONTAINS(g_tool_buf, "returned 42");
        CHECK_CONTAINS(g_tool_buf, "a+b=42");
        CHECK_CONTAINS(g_tool_buf, "cost:");
    } else g_skipped++;

    /* A compile error comes back as the rendered diagnostic, not a code. */
    CHECK_EQ(call_tool("cc_compile", "{\"source\":\"int main(void){return q;}\"}", &r),
             TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(g_tool_buf, "main.c:1:23");
    CHECK_CONTAINS(g_tool_buf, "'q' is not declared");

    /* run:false compiles and says nothing was saved. */
    CHECK_EQ(call_tool("cc_compile",
                       "{\"source\":\"int main(void){return 1;}\",\"run\":false}",
                       &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(g_tool_buf, "It was not run");
    CHECK_CONTAINS(g_tool_buf, "Nothing was saved");

    /* An argument count that does not match main() is refused, with both. */
    CHECK_EQ(call_tool("cc_compile",
                       "{\"source\":\"int main(long a){return (int)a;}\","
                       "\"args\":[1,2]}", &r), TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(g_tool_buf, "main() takes 1 argument(s) and \"args\" gave 2");

    /* Endless loop through the tool: an error the model can act on. */
    if (g_can_exec) {
        CHECK_EQ(call_tool("cc_compile",
                           "{\"source\":\"int main(void){while(1){}return 0;}\","
                           "\"fuel\":2000}", &r), TOOL_OK);
        CHECK_EQ(r.is_error, 1);
        CHECK_CONTAINS(g_tool_buf, "STOPPED");
        CHECK_CONTAINS(g_tool_buf, "loop that does not end");
    } else g_skipped++;
}

static void test_cc_compile_saves_and_cc_call_runs_it(void) {
    tool_result_t r;
    CHECK_EQ(call_tool("cc_compile",
                       "{\"source\":\"long main(long n){long i,s=0;"
                       "for(i=1;i<=n;i++)s+=i;return s;}\","
                       "\"name\":\"triangle\","
                       "\"doc\":\"triangle(n): 1+2+...+n\","
                       "\"params\":[\"n\"],\"args\":[10]}", &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(g_tool_buf, "saved \"triangle\" v1");
    CHECK(cc_find("triangle") != NULL);
    if (g_can_exec) CHECK_CONTAINS(g_tool_buf, "returned 55");
    else g_skipped++;

    CHECK_EQ(call_tool("cc_call", "{\"name\":\"triangle\",\"args\":[4]}", &r), TOOL_OK);
    if (g_can_exec) {
        CHECK_EQ(r.is_error, 0);
        CHECK_CONTAINS(g_tool_buf, "returned 10");
    } else g_skipped++;

    /* A name that does not exist names what does. */
    CHECK_EQ(call_tool("cc_call", "{\"name\":\"nosuch\"}", &r), TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(g_tool_buf, "there is no compiled program called \"nosuch\"");
    CHECK_CONTAINS(g_tool_buf, "triangle");

    /* The wrong arity names the parameters. */
    CHECK_EQ(call_tool("cc_call", "{\"name\":\"triangle\"}", &r), TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(g_tool_buf, "takes 1 argument(s) and 0 were given");
    CHECK_CONTAINS(g_tool_buf, "parameters are n");

    /* cc_source with a name reads the source back; with none, it lists. */
    CHECK_EQ(call_tool("cc_source", "{\"name\":\"triangle\"}", &r), TOOL_OK);
    CHECK_EQ(r.is_error, 0);
    CHECK_CONTAINS(g_tool_buf, "for(i=1;i<=n;i++)s+=i;");
    CHECK_EQ(call_tool("cc_source", "{}", &r), TOOL_OK);
    CHECK_CONTAINS(g_tool_buf, "triangle(n) v1");
    CHECK_CONTAINS(g_tool_buf, "RAM ONLY");

    /* An over-long name is a different answer from no name at all. */
    CHECK_EQ(call_tool("cc_source",
                       "{\"name\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
                       &r), TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(g_tool_buf, "at most");

    char msg[256];
    CHECK_EQ(cc_delete("triangle", msg, sizeof msg), CC_OK);
}

static void test_a_broken_saved_program_is_reported_with_its_source(void) {
    /* Rewrite a stored record so it no longer compiles: an older compiler could
     * have accepted it, or the source could have been edited by hand. cc_call
     * must say so and point at cc_source rather than fail opaquely. */
    char path[VFS_PATH_MAX + 1];
    snprintf(path, sizeof path, "%s/wrong.cc", cc_store_root());

    char msg[256];
    static const char *good = "int main(void){return 1;}";
    CHECK_EQ(cc_save("wrong", "will be broken", NULL, 0, good, strlen(good),
                     &g_r, msg, sizeof msg), CC_OK);

    /* Build a valid record (correct checksum) around source that will not
     * compile, using the store's own writer via a second save, then patch. */
    static const char *bad_src = "int main(void){return nope;}";
    {
        char body[4096];
        json_writer_t w;
        json_writer_init(&w, body, sizeof body);
        json_put(&w, "{\"cc\":1,\"name\":\"wrong\",\"doc\":\"broken now\","
                     "\"version\":9,\"params\":[],\"src\":");
        json_put_string(&w, bad_src);
        json_put(&w, "}");
        CHECK_EQ(json_writer_ok(&w), 1);
        size_t blen = json_writer_len(&w);

        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < blen; i++) { h ^= (unsigned char)body[i]; h *= 0x100000001b3ull; }
        char rec[4200];
        static const char *hexd = "0123456789abcdef";
        for (int i = 0; i < 16; i++) rec[i] = hexd[(h >> (60 - 4 * i)) & 0xf];
        rec[16] = '\n';
        memcpy(rec + 17, body, blen);

        file_t *f = vfs_open(path, O_WRONLY | O_TRUNC);
        CHECK(f != NULL);
        if (f) { vfs_write(f, rec, 17 + blen); vfs_close(f); }
    }

    kcap_reset();
    cc_init();
    /* It loads: the record is well formed. The failure only appears at call
     * time, which is the honest place for it — the store keeps source, and
     * whether source compiles is a question for the compiler. */
    CHECK(cc_find("wrong") != NULL);

    tool_result_t r;
    CHECK_EQ(call_tool("cc_call", "{\"name\":\"wrong\"}", &r), TOOL_OK);
    CHECK_EQ(r.is_error, 1);
    CHECK_CONTAINS(g_tool_buf, "no longer compiles");
    CHECK_CONTAINS(g_tool_buf, "cc_source");
    CHECK_CONTAINS(g_tool_buf, "'nope' is not declared");
    CHECK_EQ(cc_find("wrong")->failures, 1);

    CHECK_EQ(cc_delete("wrong", msg, sizeof msg), CC_OK);
}

static void test_exactly_one_trace_line_per_tool_call(void) {
    static const char *calls[] = {
        "cc_compile", "{\"source\":\"int main(void){return 3;}\"}",
        "cc_compile", "{\"source\":\"int main(void){return q;}\"}",
        "cc_compile", "{}",
        "cc_call",     "{\"name\":\"square\",\"args\":[2]}",
        "cc_call",     "{\"name\":\"nosuch\"}",
        "cc_source",   "{}",
        "cc_source",   "{\"name\":\"square\"}",
        NULL
    };
    for (int i = 0; calls[i]; i += 2) {
        tool_result_t r;
        trace_reset();
        call_tool(calls[i], calls[i + 1], &r);
        CHECK_EQ(trace_count(), 1);
    }
}

/* ====================================================================== */
/* 10. adversarial input                                                  */
/* ====================================================================== */

static char g_big[CC_SRC_MAX * 2 + 64];

static void test_adversarial_source_is_refused_not_survived(void) {
    /* Deep nesting, in each of the shapes that recurses. */
    struct { const char *label, *open, *close, *head, *tail; int n; } deep[] = {
      { "parens",  "(", ")", "int main(void){return ", "1;}", 4000 },
      { "braces",  "{", "}", "int main(void)", "", 4000 },
      { "unary",   "!", "",  "int main(void){return ", "1;}", 4000 },
      { "stars",   "*", "",  "int main(void){int x=1;int*p=&x;return ", "p;}", 2000 },
      { "index",   "a[", "]", "int main(void){int a[1];return ", "0;}", 1000 },
      { "ternary", "1?", "",  "int main(void){return ", "1:2;}", 1000 },
    };
    for (size_t i = 0; i < sizeof deep / sizeof deep[0]; i++) {
        size_t o = (size_t)snprintf(g_big, sizeof g_big, "%s", deep[i].head);
        for (int k = 0; k < deep[i].n && o + 8 < CC_SRC_MAX; k++)
            o += (size_t)snprintf(g_big + o, sizeof g_big - o, "%s", deep[i].open);
        o += (size_t)snprintf(g_big + o, sizeof g_big - o, "%s", deep[i].tail);
        for (int k = 0; k < deep[i].n && o + 8 < sizeof g_big; k++)
            o += (size_t)snprintf(g_big + o, sizeof g_big - o, "%s", deep[i].close);
        int rc = cc_compile(g_big, o, "t.c", &g_r);
        CHECK(rc != CC_OK);
        CHECK(g_r.ndiag >= 1);
        CHECK_EQ(kpanic_hit, 0);
        (void)deep[i].label;
    }

    /* An enormous identifier, an enormous string, and a source over the limit. */
    {
        size_t o = (size_t)snprintf(g_big, sizeof g_big, "int ");
        for (int k = 0; k < 10000; k++) g_big[o++] = 'z';
        snprintf(g_big + o, sizeof g_big - o, ";int main(void){return 0;}");
        CHECK(cc_compile(g_big, strlen(g_big), "t.c", &g_r) != CC_OK);
        CHECK_CONTAINS(g_r.diag[0].msg, "identifier");
    }
    {
        /* One enormous string literal is LEGAL - it fits the data image - so the
         * assertion is that it works, and that a set of them which does not fit
         * is refused by naming the image rather than by overrunning it. */
        size_t o = (size_t)snprintf(g_big, sizeof g_big, "char*s=\"");
        for (int k = 0; k < 8000; k++) g_big[o++] = 'x';
        snprintf(g_big + o, sizeof g_big - o,
                 "\";int main(void){return (int)strlen(s);}");
        CHECK_EQ(cc_compile(g_big, strlen(g_big), "t.c", &g_r), CC_OK);
        CHECK_EQ(g_r.data_bytes, 8001 + 8);

        /* String literals alone cannot overfill the data image, because every
         * byte of a literal costs at least a byte of source and the two limits
         * are the same. Global arrays can: a declaration of thirteen characters
         * asks for four kilobytes. */
        o = 0;
        for (int k = 0; k < CC_DATA_MAX / 4000 + 2; k++)
            o += (size_t)snprintf(g_big + o, sizeof g_big - o, "int g%d[1000];", k);
        snprintf(g_big + o, sizeof g_big - o, "int main(void){return g0[0];}");
        CHECK(cc_compile(g_big, strlen(g_big), "t.c", &g_r) != CC_OK);
        CHECK(g_r.ndiag >= 1);
        CHECK_CONTAINS(g_r.diag[0].msg, "globals and string literals");
    }
    {
        memset(g_big, 'a', sizeof g_big - 1);
        g_big[sizeof g_big - 1] = '\0';
        CHECK_EQ(cc_compile(g_big, CC_SRC_MAX + 1, "t.c", &g_r), CC_ELIMIT);
        CHECK_CONTAINS(g_r.diag[0].msg, "the limit is");
    }

    /* An embedded NUL, a high byte, and every control byte. */
    {
        char nul[64];
        memcpy(nul, "int main(void){return 0;}", 25);
        nul[10] = '\0';
        CHECK(cc_compile(nul, 25, "t.c", &g_r) != CC_OK);
        CHECK_CONTAINS(g_r.diag[0].msg, "not valid C text");
    }
    for (int c = 1; c < 256; c++) {
        if (c == '\n' || c == '\t' || c == '\r') continue;
        if (c >= 0x20 && c < 0x7f) continue;
        char probe[64];
        int n = snprintf(probe, sizeof probe, "int main(void){return 0;}");
        probe[5] = (char)c;
        int rc = cc_compile(probe, (size_t)n, "t.c", &g_r);
        CHECK(rc != CC_OK);
        CHECK_EQ(kpanic_hit, 0);
    }

    /* A source that is only one repeated token, in each direction. */
    static const char *floods[] = { "(", ")", "{", "}", "*", ";", ",", "int ",
                                    "a", "#", "\"", "'", "/*", "*/", NULL };
    for (int i = 0; floods[i]; i++) {
        size_t o = 0;
        while (o + 8 < 4096) o += (size_t)snprintf(g_big + o, sizeof g_big - o,
                                                   "%s", floods[i]);
        int rc = cc_compile(g_big, o, "t.c", &g_r);
        CHECK(rc != CC_OK);
        CHECK(g_r.ndiag >= 1);
        CHECK_EQ(kpanic_hit, 0);
    }

    /* Macro recursion, direct and mutual, both stopped by name. */
    reject("self referential macro",
           "#define A A\nint main(void){return A;}", 0, 0, "not declared");
    reject("mutual macro",
           "#define A B\n#define B A\nint main(void){return A;}", 0, 0, "not declared");
    {
        /* A macro that doubles its own expansion: bounded by the token budget. */
        size_t o = 0;
        o += (size_t)snprintf(g_big + o, sizeof g_big - o, "#define M0 1\n");
        for (int k = 1; k < 24; k++)
            o += (size_t)snprintf(g_big + o, sizeof g_big - o,
                                  "#define M%d M%d+M%d\n", k, k - 1, k - 1);
        snprintf(g_big + o, sizeof g_big - o, "int main(void){return M23;}");
        int rc = cc_compile(g_big, strlen(g_big), "t.c", &g_r);
        CHECK(rc != CC_OK);
        CHECK(g_r.ndiag >= 1);
        CHECK_EQ(kpanic_hit, 0);
    }
    /* An #include that includes itself: bounded by depth, named. */
    {
        char path[VFS_PATH_MAX + 1];
        snprintf(path, sizeof path, "%s/loop.h", cc_data_root());
        static const char *self = "#include \"loop.h\"\n";
        file_t *f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
        CHECK(f != NULL);
        if (f) { vfs_write(f, self, strlen(self)); vfs_close(f); }
        static const char *src = "#include \"loop.h\"\nint main(void){return 0;}";
        CHECK(cc_compile(src, strlen(src), "t.c", &g_r) != CC_OK);
        CHECK(g_r.ndiag >= 1);
        CHECK_EQ(kpanic_hit, 0);
    }
}

/* ======================================================================
 * THE COMPILER'S OWN STACK
 *
 * This suite cannot see the bug it is written for by nesting harder, and that
 * is the whole point of it existing. gen_expr() recurses once per AST NODE, and
 * a left-leaning chain `1+1+1+...` parses at CONSTANT depth (cc_parse.c's
 * add_expr() is an iterative precedence loop, DEEPER/SHALLOWER balanced inside
 * each iteration) while building an N-deep tree. So CC_MAX_BLOCK_DEPTH bounded
 * nothing here, and on the kernel ~2 KB of legal, in-subset, successfully
 * COMPILING C walked off the root fiber's 64 KiB stack into the identity-map
 * page tables and triple-faulted: no #PF, no fault record, no diagnosis.
 *
 * A host has an 8 MB stack, so the recursion is free here and the crash is
 * structurally unobservable. What IS observable, and what these tests pin, is
 * that the floor fires and that it fires as a DIAGNOSTIC — the mechanism that
 * makes the kernel case survivable. See "THE COMPILER'S OWN STACK" in
 * include/cc.h. */
static void test_a_flat_chain_is_bounded_by_the_stack_floor(void) {
    /* Deliberately not through nesting: the source below is 3 tokens deep. */
    size_t o = (size_t)snprintf(g_big, sizeof g_big, "int main(void){int x=0");
    for (int k = 0; k < 2000; k++)
        o += (size_t)snprintf(g_big + o, sizeof g_big - o, "+1");
    snprintf(g_big + o, sizeof g_big - o, "; return x;}");

    int rc = cc_compile(g_big, strlen(g_big), "chain.c", &g_res);
    CHECK(rc != CC_OK);
    CHECK(g_res.ndiag >= 1);
    /* A diagnostic, with a position, that tells the model what to do about it —
     * not a truncation, not a fault, not a silent success. */
    if (g_res.ndiag) {
        CHECK_CONTAINS(g_res.diag[0].msg, "too deeply nested for the compiler to walk");
        CHECK_CONTAINS(g_res.diag[0].msg, "named intermediate variables");
        CHECK(g_res.diag[0].line == 1);
    }
    /* Exactly one, not one per frame on the way out: the unwind passes back
     * through every level, and CC_DIAG_MAX copies of the same line would push
     * out the one diagnostic that says where. */
    CHECK(g_res.ndiag == 1);

    /* And the compiler still works afterwards. A bound that leaves the module
     * wedged is not a bound, it is a different crash. */
    expect("a short chain still compiles",
           "int main(void){int x=0+1+1+1+1+1+1+1+1+1+1; return x;}", 10);
}

static void test_the_stack_floor_respects_a_declared_stack_end(void) {
    /* cc_stack_limit_set() is what covers being called from an already-deep
     * caller: an agenda item three frames inside a model turn has far less room
     * below it than kernel_main did. Declaring an end 1 KiB below this frame
     * must refuse EVERY compile rather than descend into it — the reserve is
     * CC_STACK_RESERVE and 1 KiB is inside it. */
    char here;
    cc_stack_limit_set(&here - 1024);
    int rc = cc_compile("int main(void){return 1;}", 25, "tight.c", &g_res);
    CHECK(rc != CC_OK);
    CHECK(g_res.ndiag == 1);
    if (g_res.ndiag) CHECK_CONTAINS(g_res.diag[0].msg, "kernel stack");

    /* Declaring a generous end must not refuse anything. */
    cc_stack_limit_set(&here - (CC_STACK_BUDGET + CC_STACK_RESERVE + 65536u));
    expect("a generous stack compiles normally",
           "int main(void){return 7;}", 7);

    /* Forgetting the hint leaves the budget alone, still enforced from
     * cc_compile()'s own frame. */
    cc_stack_limit_set(NULL);
    expect("no declared end compiles normally",
           "int main(void){return 9;}", 9);

    /* cc_stack_report() prints both numbers whichever way it goes, and returns
     * 0 only when the budget fits. With no declared end there is nothing to
     * disagree with, so it must be quiet about a mismatch it cannot know of. */
    kcap_reset();
    CHECK(cc_stack_report() == 0);
    CHECK_CONTAINS(kcap_text(), "24576");

    cc_stack_limit_set(&here - 1024);
    kcap_reset();
    CHECK(cc_stack_report() != 0);
    /* BOTH numbers, so the next person does not have to measure it again. */
    CHECK_CONTAINS(kcap_text(), "STACK BUDGET DOES NOT FIT");
    CHECK_CONTAINS(kcap_text(), "30720");     /* budget + reserve */
    cc_stack_limit_set(NULL);
}

static void test_too_much_of_a_legal_thing_is_a_diagnostic(void) {
    /* Each of the compiler's own limits, hit deliberately, must produce a
     * message that names the limit rather than a truncation or a fault. */
    size_t o = 0;
    for (int k = 0; k < CC_MAX_FUNCS + 4; k++)
        o += (size_t)snprintf(g_big + o, sizeof g_big - o,
                              "int f%d(void){return %d;}", k, k);
    snprintf(g_big + o, sizeof g_big - o, "int main(void){return f0();}");
    reject("too many functions", g_big, 0, 0, "at most");

    o = 0;
    o += (size_t)snprintf(g_big + o, sizeof g_big - o, "struct S{");
    for (int k = 0; k < CC_MAX_MEMBERS + 4; k++)
        o += (size_t)snprintf(g_big + o, sizeof g_big - o, "int m%d;", k);
    snprintf(g_big + o, sizeof g_big - o, "}; int main(void){return 0;}");
    reject("too many members", g_big, 0, 0, "at most");

    o = (size_t)snprintf(g_big, sizeof g_big, "int main(void){switch(1){");
    for (int k = 0; k < CC_MAX_SWITCH_CASES + 4; k++)
        o += (size_t)snprintf(g_big + o, sizeof g_big - o, "case %d: break;", k);
    snprintf(g_big + o, sizeof g_big - o, "}return 0;}");
    reject("too many cases", g_big, 0, 0, "at most");

    o = (size_t)snprintf(g_big, sizeof g_big, "int main(void){int a=1;a");
    for (int k = 0; k < CC_TEMP_SLOTS + 4; k++)
        o += (size_t)snprintf(g_big + o, sizeof g_big - o, "+=a++");
    snprintf(g_big + o, sizeof g_big - o, ";return a;}");
    reject("too many temporaries in one statement", g_big, 0, 0, "temporaries");

    o = 0;
    for (int k = 0; k < CC_MAX_MACROS + 4; k++)
        o += (size_t)snprintf(g_big + o, sizeof g_big - o, "#define M%d %d\n", k, k);
    snprintf(g_big + o, sizeof g_big - o, "int main(void){return M0;}");
    reject("too many macros", g_big, 0, 0, "macros are defined");

    /* Too much machine code, and too many tokens. Both must be diagnostics. */
    o = (size_t)snprintf(g_big, sizeof g_big, "int main(void){long a=0;");
    while (o < CC_SRC_MAX - 40)
        o += (size_t)snprintf(g_big + o, sizeof g_big - o, "a=a+1;");
    snprintf(g_big + o, sizeof g_big - o, "return (int)a;}");
    {
        int rc = cc_compile(g_big, strlen(g_big), "t.c", &g_r);
        CHECK(rc != CC_OK);
        CHECK(g_r.ndiag >= 1);
        CHECK(strstr(g_r.diag[0].msg, "machine code") != NULL ||
              strstr(g_r.diag[0].msg, "tokens") != NULL ||
              strstr(g_r.diag[0].msg, "working memory") != NULL);
    }

    /* And the arena, filled by a great many distinct declarations. */
    o = 0;
    for (int k = 0; o + 32 < CC_SRC_MAX; k++)
        o += (size_t)snprintf(g_big + o, sizeof g_big - o,
                              "int gg%d;struct s%d{int a;};", k, k);
    snprintf(g_big + o, sizeof g_big - o, "int main(void){return 0;}");
    {
        int rc = cc_compile(g_big, strlen(g_big), "t.c", &g_r);
        /* It may well succeed; what must not happen is a silent failure. */
        if (rc != CC_OK) CHECK(g_r.ndiag >= 1);
        CHECK_EQ(kpanic_hit, 0);
    }
}

/* ---- fuzzing the tools ---- */

static uint32_t g_seed = 0x5eed1234u;
static uint32_t rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

/* Fragments weighted so that a generated program is often ALMOST valid, which is
 * where a compiler is most likely to be wrong. Random bytes almost never reach
 * the code generator; a source that nearly parses does. */
static const char *FRAG[] = {
    "int ", "long ", "char ", "void ", "unsigned ", "struct ", "union ", "enum ",
    "static ", "const ", "typedef ", "sizeof ", "return ", "if", "else",
    "while", "for", "do", "switch", "case ", "default", "break;", "continue;",
    "main", "f", "x", "y", "p", "a", "0", "1", "-1", "0x7fffffffffffffff",
    "18446744073709551615", "'c'", "\\\"s\\\"", "(", ")", "{", "}", "[", "]",
    ";", ",", "=", "==", "+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^",
    "~", "!", "&&", "||", "?", ":", ".", "->", "++", "--", "+=", "<", ">",
    "#define ", "#ifdef ", "#endif", "#include ", "\\n", " ", "/*", "*/", "//",
    "print", "print_num", "memcpy", "file_write", "time_ms", "strlen",
    "int main(void){", "return 0;}", "float ", "goto ", "...", "##",
};
#define NFRAG ((int)(sizeof FRAG / sizeof FRAG[0]))

static void test_fuzz_the_tools(void) {
    const char *tools[] = { "cc_compile", "cc_call", "cc_source" };
    char input[8192];
    int compiled = 0, ran = 0, refused = 0;

    for (int iter = 0; iter < 4000; iter++) {
        size_t o = 0;
        int which = (int)(rnd() % 3);

        o = (size_t)snprintf(input, sizeof input, "{");
        if (which == 0) {
            o += (size_t)snprintf(input + o, sizeof input - o, "\"source\":\"");
            /* One in six is a program that really compiles, sometimes with one
             * fragment spliced into it. Purely random fragments almost never
             * parse, and a fuzzer that only ever exercises the error paths says
             * nothing about the ones that matter. */
            if (rnd() % 6 == 0) {
                static const char *good[] = {
                    "int main(void){return 7;}",
                    "long main(long n){long i,s=0;for(i=0;i<n;i++)s+=i;return s;}",
                    "int f(int n){return n<2?n:f(n-1)+f(n-2);}"
                    "int main(void){return f(8);}",
                    "int main(void){char b[8];memset(b,65,8);print(b);return b[0];}",
                    "struct P{int x;int y;};int main(void){struct P p={1,2};"
                    "return p.x+p.y;}",
                };
                o += (size_t)snprintf(input + o, sizeof input - o, "%s",
                                      good[rnd() % 5]);
                if (rnd() % 2)
                    o += (size_t)snprintf(input + o, sizeof input - o, "%s",
                                          FRAG[rnd() % NFRAG]);
            } else {
                int nfrag = 1 + (int)(rnd() % 40);
                for (int k = 0; k < nfrag && o + 64 < sizeof input; k++)
                    o += (size_t)snprintf(input + o, sizeof input - o, "%s",
                                          FRAG[rnd() % NFRAG]);
            }
            o += (size_t)snprintf(input + o, sizeof input - o, "\"");
            if (rnd() % 3 == 0)
                o += (size_t)snprintf(input + o, sizeof input - o,
                                      ",\"args\":[%u,%u]", rnd() % 9, rnd() % 9);
            if (rnd() % 4 == 0)
                o += (size_t)snprintf(input + o, sizeof input - o,
                                      ",\"fuel\":%u", rnd() % 100000);
            if (rnd() % 5 == 0)
                o += (size_t)snprintf(input + o, sizeof input - o,
                                      ",\"name\":\"fz%u\",\"doc\":\"fuzz\","
                                      "\"params\":[]", rnd() % 4);
        } else if (which == 1) {
            o += (size_t)snprintf(input + o, sizeof input - o,
                                  "\"name\":\"%s\",\"args\":[%u]",
                                  (rnd() % 2) ? "square" : "zz", rnd() % 5);
        } else {
            if (rnd() % 2)
                o += (size_t)snprintf(input + o, sizeof input - o,
                                      "\"name\":\"%s\"",
                                      (rnd() % 2) ? "square" : "nope");
        }
        o += (size_t)snprintf(input + o, sizeof input - o, "}");

        /* Every fourth call is truncated mid-JSON. */
        if (iter % 4 == 3 && o > 4) input[o / 2] = '\0';

        tool_result_t r;
        kcap_reset();
        CHECK_EQ(call_tool(tools[which], input, &r), TOOL_OK);
        CHECK(r.len > 0 || r.is_error);
        CHECK_EQ(kpanic_hit, 0);
        CHECK_EQ(kfmt_violations, 0);
        /* The panel is still exactly the size the schema reserved. */
        CHECK_EQ((int)strlen(cc_panel()), CC_PANEL_CHARS);
        /* Column zero stays the kernel's: every line of captured console output
         * either starts a trace line the KERNEL wrote, or does not start with
         * '[' at all. */
        for (const char *p = kcap_text(); *p; ) {
            if (*p == '[') {
                /* Only trace.c writes these, and they always close. */
                CHECK(strchr(p, ']') != NULL);
            }
            const char *nl = strchr(p, '\n');
            if (!nl) break;
            p = nl + 1;
        }
        if (r.is_error) refused++;
        else if (which == 0) compiled++;
        if (strstr(g_tool_buf, "returned")) ran++;
    }
    printf("      fuzz: %d refused, %d compiled, %d ran\n", refused, compiled, ran);
    /* The generator must actually be producing valid programs sometimes, or this
     * test is only exercising the error paths. */
    CHECK(compiled > 0);

    /* And the store still reloads cleanly after all of that. */
    char msg[256];
    cc_init();
    CHECK_EQ(cc_rejected(), 0);
    for (int i = cc_count() - 1; i >= 0; i--)
        if (strncmp(cc_at(i)->name, "fz", 2) == 0)
            CHECK_EQ(cc_delete(cc_at(i)->name, msg, sizeof msg), CC_OK);
}

/* ====================================================================== */
/* 11. the symbol table itself                                            */
/* ====================================================================== */

static void test_the_symbol_table_is_documented_and_callable(void) {
    CHECK(cc_symbol_count() >= 10);
    for (int i = 0; i < cc_symbol_count(); i++) {
        const cc_symbol_t *s = cc_symbol_at(i);
        CHECK(s != NULL);
        if (!s) continue;
        CHECK(s->decl && strlen(s->decl) > 6);
        CHECK(s->why && strlen(s->why) > 10);
        /* Every declaration in the table must name a function this compiler will
         * accept a call to — which is the same as saying the table and the
         * predeclared scope cannot disagree. */
        char probe[512];
        char nm[64];
        const char *open = strchr(s->decl, '(');
        const char *start = open;
        CHECK(open != NULL);
        if (!open) continue;
        while (start > s->decl && (start[-1] != ' ' && start[-1] != '*')) start--;
        size_t nl = (size_t)(open - start);
        CHECK(nl > 0 && nl < sizeof nm);
        memcpy(nm, start, nl);
        nm[nl] = '\0';
        snprintf(probe, sizeof probe,
                 "int main(void){long a=0;(void)a;%s(", nm);
        /* Just check the name resolves: the arity error proves it is declared. */
        strncat(probe, ");return 0;}", sizeof probe - strlen(probe) - 1);
        int rc = cc_compile(probe, strlen(probe), "t.c", &g_r);
        if (rc != CC_OK) {
            CHECK(g_r.ndiag >= 1);
            CHECK(strstr(g_r.diag[0].msg, "there is no function called") == NULL);
        }
    }

    /* And the tool description lists exactly what the table holds, so a model is
     * never told about a function that is not there. */
    const tool_t *t = tool_find("cc_compile");
    CHECK(t != NULL);
    if (t)
        for (int i = 0; i < cc_symbol_count(); i++) {
            const cc_symbol_t *s = cc_symbol_at(i);
            char nm[64];
            const char *open = strchr(s->decl, '(');
            const char *start = open;
            if (!open) continue;
            while (start > s->decl && (start[-1] != ' ' && start[-1] != '*')) start--;
            size_t nl = (size_t)(open - start);
            if (nl >= sizeof nm) continue;
            memcpy(nm, start, nl);
            nm[nl] = '\0';
            th_checks++;
            if (!strstr(t->description, nm)) {
                th_fails++;
                printf("    FAIL cc_compile's description does not mention the "
                       "kernel function '%s'\n", nm);
            }
        }
}

static void test_no_call_target_is_ever_a_computed_value(void) {
    /* The security property, tested the only way a black box can test it: every
     * construct that would need an indirect jump is refused. If one of these
     * ever starts compiling, the claim in cc.h is no longer true. */
    reject("function pointer variable",
           "int f(void){return 1;} int main(void){int(*p)(void)=f;return p();}",
           0, 0, "function pointers");
    reject("array of function pointers",
           "int f(void){return 1;} int main(void){int(*t[2])(void);return 0;}",
           0, 0, "function pointer");
    reject("function pointer parameter",
           "int g(int(*cb)(void)){return cb();} int main(void){return 0;}",
           0, 0, "function pointer");
    reject("function pointer member",
           "struct S{int(*fn)(void);}; int main(void){return 0;}",
           0, 0, "function");
    reject("calling through a variable",
           "int main(void){long a=0;return ((int)a)();}", 0, 0, "expected");
    reject("taking a function's address",
           "int f(void){return 1;} int main(void){long a=(long)&f;return (int)a;}",
           0, 0, "function pointers are not supported");
    reject("function as an argument",
           "int f(void){return 1;} int g(long x){return (int)x;} "
           "int main(void){return g((long)f);}", 0, 0, "can only be called");
}

/* ====================================================================== */
/* main                                                                   */
/* ====================================================================== */

int main(void) {
    console_init();
    fs_bringup();

    /* Detect whether this build can execute what it emits, and say so once. */
    {
        static const char *probe = "int main(void){return 1;}";
        cc_result_t r;
        if (cc_compile(probe, strlen(probe), "probe.c", &r) == CC_OK &&
            cc_run(NULL, 0, 0, &r) == CC_OK && r.value == 1)
            g_can_exec = 1;
    }
    printf("  (this build %s the machine code it emits)\n",
           g_can_exec ? "EXECUTES" : "CANNOT EXECUTE");

    /* The real boot path, including the panel binding and the include reader. */
    cc_bringup();
    CHECK_EQ(cc_store_persists(), 0);        /* no disk in a host test */
    CHECK_STR(cc_store_root(), "/cc");
    /* The data root must not be inside the store, or file_write could rewrite
     * the machine's programs. Asserted at both ends. */
    CHECK(strncmp(cc_data_root(), cc_store_root(), strlen(cc_store_root())) != 0);
    CHECK(strncmp(cc_store_root(), cc_data_root(), strlen(cc_data_root())) != 0);

    RUN(test_the_accepted_subset_runs_and_is_right);
    RUN(test_every_refusal_says_something_useful);
    RUN(test_a_diagnostic_renders_with_a_caret_under_the_column);
    RUN(test_a_failure_always_carries_a_diagnostic);
    RUN(test_an_endless_loop_stops_instead_of_hanging);
    RUN(test_runaway_recursion_stops_at_its_own_stack);
    RUN(test_a_frame_bigger_than_the_guard_is_refused_at_compile_time);
    RUN(test_fuel_is_charged_per_iteration_and_per_call);
    RUN(test_a_program_can_call_the_kernel);
    RUN(test_console_output_cannot_forge_a_kernel_trace_line);
    RUN(test_a_program_keeps_a_file_but_only_in_its_own_directory);
    RUN(test_a_span_longer_than_the_bound_is_refused_not_clamped);
    RUN(test_the_shipped_examples_compile);
    RUN(test_the_five_documented_diagnostics_are_exact);
    RUN(test_the_documented_example_runs);
    RUN(test_a_saved_program_survives_a_reboot);
    RUN(test_the_cached_image_is_never_the_wrong_program);
    RUN(test_replacing_a_program_is_a_new_version_with_a_backup);
    RUN(test_a_corrupted_record_is_rejected_and_the_backup_used);
    RUN(test_a_record_under_the_wrong_name_is_refused);
    RUN(test_bad_saves_change_nothing);
    RUN(test_the_registry_fills_up_and_says_so);
    RUN(test_the_panel_is_exactly_the_width_the_schema_reserved);
    RUN(test_the_schema_size_does_not_depend_on_the_store);
    RUN(test_the_tools_are_registered_and_their_schemas_parse);
    RUN(test_cc_compile_end_to_end_through_the_tool);
    RUN(test_cc_compile_saves_and_cc_call_runs_it);
    RUN(test_a_broken_saved_program_is_reported_with_its_source);
    RUN(test_exactly_one_trace_line_per_tool_call);
    RUN(test_the_symbol_table_is_documented_and_callable);
    RUN(test_no_call_target_is_ever_a_computed_value);
    RUN(test_adversarial_source_is_refused_not_survived);
    RUN(test_a_flat_chain_is_bounded_by_the_stack_floor);
    RUN(test_the_stack_floor_respects_a_declared_stack_end);
    RUN(test_too_much_of_a_legal_thing_is_a_diagnostic);
    RUN(test_fuzz_the_tools);

    if (g_skipped)
        printf("  NOTE: %d test(s) that need to RUN emitted code were skipped: "
               "this build is not x86-64.\n", g_skipped);
    return th_report("cc");
}
