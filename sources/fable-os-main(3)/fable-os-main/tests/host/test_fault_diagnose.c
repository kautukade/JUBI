/* test_fault_diagnose.c — the kernel asking the model about its own crash,
 *                         run natively against the scripted mock.
 *
 * WHAT THIS SUITE IS FOR
 *   net/faultchat.c is the one module in this kernel that starts a conversation
 *   on its own initiative, and the thing it says is a crash report. Two ways
 *   that goes wrong badly enough to matter:
 *
 *     1. The request is useless. A prompt that omits the decoded error code, or
 *        the instruction bytes, or whether "skip" is even decodable, sends the
 *        model off to guess — and its guess arms a fault_plan_t that changes
 *        what the CPU executes. So the prompt is asserted field by field, not
 *        just "non-empty".
 *     2. The reply is trusted. It arrives over a network, from a model, in
 *        response to a machine that has just proved it can fault. Every reply
 *        path here is fed malformed, oversized, prose-wrapped, forged and
 *        outright random input, and the invariant is always the same: a legible
 *        refusal, never a plan armed from garbage.
 *
 * THE PROOF THAT NOTHING HERE ALLOCATES
 *   Look at TESTSRC_test_fault_diagnose in the Makefile: mm/heap.c is NOT in
 *   it. If net/faultchat.c ever reached for kmalloc — directly, or through a
 *   helper — this binary would fail to link. The "a fault inside the heap must
 *   not stop the machine building a diagnosis request" claim is therefore
 *   checked by the linker on every single build, which is stronger than any
 *   assertion this file could make.
 *
 * THE SYNTHETIC MACHINE
 *   code_area[] and stack_area[] are ordinary arrays that the fault window is
 *   pointed at, so fault_capture() performs real dereferences and the prompt's
 *   "MEMORY AROUND RIP" block reads real bytes. The frame chain in stack_area[]
 *   is laid out exactly as a compiled prologue would leave it, so the backtrace
 *   in the prompt is a genuine walk. Nothing is simulated except the fault
 *   itself.
 */

#include "harness.h"
#include "faultchat.h"
#include "fault.h"
#include "model.h"
#include "json.h"
#include "tool.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* the tool table, host-side                                              */
/* ====================================================================== */

/* REGISTER_TOOL cannot expand on Mach-O, so tools/fault_tools.c exports a named
 * pointer per tool under FABLEOS_HOSTTEST and this file stands in for the
 * linker, feeding the REAL core/tool.c registry. Identical arrangement to
 * tests/host/test_fault.c — and it matters here, because faultchat_apply_fix()
 * reaches the plan through tool_dispatch("fault_recover") rather than through
 * fault_plan_arm(). If the tool is not registered, nothing is armed, and the
 * suite would be proving the wrong thing. */
extern const tool_t *const fableos_hosttool_fault_status_tool;
extern const tool_t *const fableos_hosttool_fault_report_tool;
extern const tool_t *const fableos_hosttool_fault_history_tool;
extern const tool_t *const fableos_hosttool_fault_decode_tool;
extern const tool_t *const fableos_hosttool_fault_recover_tool;

#if defined(__APPLE__)
#  define SYMPFX "_"
#  define TBL_SECTION __attribute__((section("__DATA,__data")))
#else
#  define SYMPFX ""
#  define TBL_SECTION
#endif

const tool_t *__start_tool_table[5] TBL_SECTION = { 0, 0, 0, 0, 0 };
extern const tool_t *const __stop_tool_table[];

__asm__(".globl " SYMPFX "__stop_tool_table\n"
        ".set "   SYMPFX "__stop_tool_table, " SYMPFX "__start_tool_table + 40\n");
_Static_assert(sizeof(__start_tool_table) == 40,
               "tool count changed: update the __stop_tool_table byte offset");

static void tools_bind(void) {
    __start_tool_table[0] = fableos_hosttool_fault_status_tool;
    __start_tool_table[1] = fableos_hosttool_fault_report_tool;
    __start_tool_table[2] = fableos_hosttool_fault_history_tool;
    __start_tool_table[3] = fableos_hosttool_fault_decode_tool;
    __start_tool_table[4] = fableos_hosttool_fault_recover_tool;
}

/* ====================================================================== */
/* the synthetic machine                                                  */
/* ====================================================================== */

static uint8_t  code_area[512];
static uint64_t stack_area[64];
static fault_window_t W;

#define CODE_OFF 64                       /* where the faulting insn lives */

static void arena_init(void) {
    memset(code_area, 0x90, sizeof code_area);      /* NOPs either side */
    memset(stack_area, 0, sizeof stack_area);

    W.image_lo = (uint64_t)(uintptr_t)code_area;
    W.image_hi = W.image_lo + sizeof code_area;
    W.code_lo  = W.image_lo;
    W.code_hi  = W.image_hi;
    W.stack_lo = (uint64_t)(uintptr_t)stack_area;
    W.stack_hi = W.stack_lo + sizeof stack_area;
    fault_set_window(&W);
}

/* A real {saved RBP, return address} chain, so the backtrace in the prompt is a
 * genuine walk rather than a fabricated list. */
static uint64_t build_chain(void) {
    stack_area[0] = (uint64_t)(uintptr_t)&stack_area[2];
    stack_area[1] = W.code_lo + 0x20;
    stack_area[2] = (uint64_t)(uintptr_t)&stack_area[4];
    stack_area[3] = W.code_lo + 0x30;
    stack_area[4] = 0;
    stack_area[5] = 0;
    return (uint64_t)(uintptr_t)&stack_area[0];
}

/* mov $0x1234,(%rax) — 7 bytes, decodes with certainty, and is exactly what
 * arch/x86_64/fault_inject.c's #PF write injector faults on. */
static const uint8_t INSN_MOV_STORE[] = { 0x48, 0xc7, 0x00, 0x34, 0x12, 0x00, 0x00 };
/* 0x62 is the EVEX prefix, which the length decoder refuses on purpose. */
static const uint8_t INSN_UNDECODABLE[] = { 0x62, 0xf1, 0x7c, 0x48, 0x28, 0xc0 };

/* Raise one fault on the synthetic machine, following isr_dispatch() exactly:
 * capture into the ring, then run the recovery decision. Both halves matter —
 * fault_recover() is what records the disposition and what maintains the
 * same-RIP re-fault counter, and the prompt reports both.
 *
 * A one-use "skip" plan is armed first so the fault RESUMES, because that is
 * the only kind of fault the pump can ever see: an unrecovered fatal fault
 * halts inside the handler and never reaches normal kernel context. The plan is
 * cleared afterwards so each test starts from "nothing armed". */
static void inject(uint64_t vector, uint64_t err,
                   const uint8_t *bytes, size_t n, int with_chain) {
    memcpy(code_area + CODE_OFF, bytes, n);

    fault_plan_t survive = { .action = FAULT_ACT_SKIP, .any_vector = 1,
                             .uses = 1 };
    fault_plan_arm(&survive);

    fault_frame_t f;
    memset(&f, 0, sizeof f);
    f.vector     = vector;
    f.error_code = err;
    f.rip        = W.code_lo + CODE_OFF;
    f.rsp        = W.stack_lo + 8 * 8;
    f.rbp        = with_chain ? build_chain() : 0;
    f.cr2        = 0x0000001000000000ULL;
    f.cs         = 0x08;
    f.ss         = 0x00;
    f.rflags     = 0x0000000000000002ULL;
    f.rax        = 0x0000001000000000ULL;
    f.rbx        = 0x00000000deadbeefULL;

    fault_note(&f, &W, 7);
    fault_recover(&f, &W);
    fault_plan_clear();
}

static void inject_pf(void) {
    inject(14, 0x2, INSN_MOV_STORE, sizeof INSN_MOV_STORE, 1);
}

/* The same fault at a DIFFERENT address each time.
 *
 * net/faultchat.c bounds diagnoses two independent ways — per boot
 * (FAULTCHAT_MAX_DIAGNOSES) and per faulting address (FAULTCHAT_MAX_PER_RIP) —
 * and the second is much tighter. A storm at ONE address is stopped by the
 * per-address bound long before the per-boot budget is reached, so a test that
 * wants to exercise the per-boot budget has to move the fault around. That is
 * not a workaround: it is the two bounds doing different jobs, and each one is
 * asserted separately below. */
static unsigned storm_slot;

static void inject_pf_elsewhere(void) {
    unsigned off = 128 + (storm_slot++ % 16u) * 16u;
    memcpy(code_area + off, INSN_MOV_STORE, sizeof INSN_MOV_STORE);

    fault_plan_t survive = { .action = FAULT_ACT_SKIP, .any_vector = 1,
                             .uses = 1 };
    fault_plan_arm(&survive);

    fault_frame_t f;
    memset(&f, 0, sizeof f);
    f.vector     = 14;
    f.error_code = 0x2;
    f.rip        = W.code_lo + off;
    f.rsp        = W.stack_lo + 8 * 8;
    f.rbp        = build_chain();
    f.cs         = 0x08;
    f.rflags     = 0x2;

    fault_note(&f, &W, 7);
    fault_recover(&f, &W);
    fault_plan_clear();
}

/* ====================================================================== */
/* building Messages API response bodies                                  */
/* ====================================================================== */

static char body_buf[16384];

/* Wrap `text` as the model's reply. Escaped with json.h, so a test can put
 * quotes, newlines and braces in the model's mouth without hand-escaping. */
static const char *reply_body(const char *text) {
    json_writer_t w;
    json_writer_init(&w, body_buf, sizeof body_buf);
    json_put(&w, "{\"id\":\"msg_test\",\"type\":\"message\",\"role\":\"assistant\","
                 "\"content\":[{\"type\":\"text\",\"text\":");
    json_put_string(&w, text);
    json_put(&w, "}],\"stop_reason\":\"end_turn\"}");
    if (!json_writer_ok(&w)) { body_buf[0] = '\0'; }
    return body_buf;
}

/* ====================================================================== */
/* setup                                                                  */
/* ====================================================================== */

static void setup(void) {
    fault_reset();               /* also clears the window and the plan */
    storm_slot = 0;
    arena_init();
    model_mock_reset();
    faultchat_bind(model_mock_transport());
    kcap_reset();
    trace_reset();
}

/* ====================================================================== */
/* the system prompt                                                      */
/* ====================================================================== */

static void test_system_prompt_states_the_contract(void) {
    const char *s = faultchat_system_prompt();
    CHECK(s != NULL);
    /* The kernel must say who is asking: this is not an operator turn, and a
     * model that thinks it is will answer conversationally. */
    CHECK_CONTAINS(s, "KERNEL is asking you");
    CHECK_CONTAINS(s, "Nobody typed anything");
    /* The answer shape. */
    CHECK_CONTAINS(s, "ONE JSON object");
    CHECK_CONTAINS(s, "\"diagnosis\"");
    CHECK_CONTAINS(s, "\"fix\"");
    /* Every action word the fault_recover tool accepts must be advertised, or
     * the model cannot propose it. */
    CHECK_CONTAINS(s, "skip");
    CHECK_CONTAINS(s, "set_register");
    CHECK_CONTAINS(s, "set_rip");
    CHECK_CONTAINS(s, "refuse");
    CHECK_CONTAINS(s, "rsp is refused");
    /* And the most important instruction of all: it is fine not to know. */
    CHECK_CONTAINS(s, "OMIT \"fix\" ENTIRELY when you are not sure");
}

/* ====================================================================== */
/* the prompt                                                             */
/* ====================================================================== */

static char prompt[FAULTCHAT_PROMPT_MAX];

static void test_prompt_carries_what_a_model_needs(void) {
    setup();
    inject_pf();

    size_t n = faultchat_build_prompt(fault_last(), fault_window(),
                                      prompt, sizeof prompt);
    CHECK(n > 0);
    CHECK(n < sizeof prompt);            /* not clipped */

    /* --- the exception itself --- */
    CHECK_CONTAINS(prompt, "#PF");
    CHECK_CONTAINS(prompt, "vector 14");
    CHECK_CONTAINS(prompt, "Page Fault");

    /* --- the decoded error code, in words. "0x2" teaches a model nothing. --- */
    CHECK_CONTAINS(prompt, "page not present");
    CHECK_CONTAINS(prompt, "write");
    CHECK_CONTAINS(prompt, "kernel-mode access");

    /* --- where --- */
    CHECK_CONTAINS(prompt, "RIP        : 0x");
    CHECK_CONTAINS(prompt, "CR2        : 0x0000001000000000");

    /* --- every general-purpose register --- */
    CHECK_CONTAINS(prompt, "RAX 0x0000001000000000");
    CHECK_CONTAINS(prompt, "RBX 0x00000000deadbeef");
    CHECK_CONTAINS(prompt, "R15 0x");

    /* --- the faulting instruction, byte for byte --- */
    CHECK_CONTAINS(prompt, "48 c7 00 34 12 00 00");

    /* --- how it got there --- */
    CHECK_CONTAINS(prompt, "backtrace");
    CHECK_CONTAINS(prompt, "(faulting instruction)");

    /* --- the surrounding instruction stream --- */
    CHECK_CONTAINS(prompt, "MEMORY AROUND RIP");
    CHECK_CONTAINS(prompt, "<-- RIP, the faulting instruction starts here");

    /* --- the machine context a fix has to satisfy --- */
    CHECK_CONTAINS(prompt, "resume window");
    CHECK_CONTAINS(prompt, "vector recoverable: yes");
    CHECK_CONTAINS(prompt, "faults since boot : 1 (this is fault #1)");
    CHECK_CONTAINS(prompt, "armed plan        : none");

    /* --- and the single most actionable fact: is "skip" even possible? --- */
    CHECK_CONTAINS(prompt, "\"skip\" available  : yes");
    CHECK_CONTAINS(prompt, "decodes to 7 bytes");

    /* --- the answer contract, restated --- */
    CHECK_CONTAINS(prompt, "ANSWER WITH ONE JSON OBJECT");
    CHECK_CONTAINS(prompt, "Omit them unless");
    /* And the other half of the contract, added when the loop was closed: a
     * recovery plan changes what happens at the NEXT fault, while a code patch
     * removes the instruction that caused this one. A model that is not told the
     * difference cannot choose between them. */
    CHECK_CONTAINS(prompt, "CAN THIS BE PATCHED OUT?");
    CHECK_CONTAINS(prompt, "to remove the faulting instruction for good");

    printf("      prompt is %u bytes; %u spare in FAULTCHAT_PROMPT_MAX\n",
           (unsigned)n, (unsigned)(sizeof prompt - n));
    /* A crash report that only just fits is a crash report that will not fit
     * the day someone adds a line to it. */
    CHECK(sizeof prompt - n > 1024);
}

static void test_prompt_warns_when_skip_is_impossible(void) {
    setup();
    inject(6, 0, INSN_UNDECODABLE, sizeof INSN_UNDECODABLE, 1);

    faultchat_build_prompt(fault_last(), fault_window(), prompt, sizeof prompt);
    CHECK_CONTAINS(prompt, "\"skip\" available  : NO");
    CHECK_CONTAINS(prompt, "WILL be refused");
    CHECK_CONTAINS(prompt, "#UD");
}

static void test_prompt_reports_an_unrecoverable_vector(void) {
    setup();
    inject(8, 0, INSN_MOV_STORE, sizeof INSN_MOV_STORE, 0);   /* #DF */

    faultchat_build_prompt(fault_last(), fault_window(), prompt, sizeof prompt);
    CHECK_CONTAINS(prompt, "vector recoverable: NO");
    CHECK_CONTAINS(prompt, "no fix will be accepted for it");
}

static void test_prompt_reports_a_repeat(void) {
    setup();
    inject_pf();
    inject_pf();
    inject_pf();
    faultchat_build_prompt(fault_last(), fault_window(), prompt, sizeof prompt);
    CHECK_CONTAINS(prompt, "REPEATING");
    CHECK_CONTAINS(prompt, "The last recovery did NOT work");
}

static void test_prompt_survives_no_window_and_no_record(void) {
    setup();
    inject_pf();

    /* No window: the memory dump is dropped, everything else still renders and
     * the model is told a set_rip cannot be validated. */
    size_t n = faultchat_build_prompt(fault_last(), NULL, prompt, sizeof prompt);
    CHECK(n > 0);
    CHECK(strstr(prompt, "MEMORY AROUND RIP") == NULL);
    CHECK_CONTAINS(prompt, "not published on this build");

    /* No record at all. */
    n = faultchat_build_prompt(NULL, fault_window(), prompt, sizeof prompt);
    CHECK(n > 0);
    CHECK_CONTAINS(prompt, "no fault has been captured");

    /* A tiny buffer must still leave a NUL-terminated prefix. */
    char tiny[8];
    faultchat_build_prompt(fault_last(), fault_window(), tiny, sizeof tiny);
    CHECK(tiny[sizeof tiny - 1] == '\0');
    faultchat_build_prompt(fault_last(), fault_window(), tiny, 0);   /* no write */
}

/* ====================================================================== */
/* parsing the reply                                                      */
/* ====================================================================== */

static faultchat_reply_t rep;
static char why[256];

static int parse(const char *text) {
    const char *b = reply_body(text);
    return faultchat_parse_reply(b, strlen(b), &rep, why, sizeof why);
}

static void test_parse_a_clean_reply(void) {
    setup();
    CHECK_EQ(parse("{\"diagnosis\":\"A store through rax faulted because "
                   "0x1000000000 is not mapped.\","
                   "\"fix\":{\"action\":\"skip\",\"vector\":14,\"uses\":2}}"),
             FAULTCHAT_OK);
    CHECK_CONTAINS(rep.diagnosis, "0x1000000000 is not mapped");
    CHECK_EQ(rep.has_fix, 1);
    /* Verbatim, uninterpreted: the fault_recover tool is the only thing that
     * gets to read these fields. */
    CHECK_STR(rep.fix, "{\"action\":\"skip\",\"vector\":14,\"uses\":2}");
    CHECK_EQ(rep.fix_len, strlen(rep.fix));
}

static void test_parse_a_reply_with_no_fix(void) {
    setup();
    CHECK_EQ(parse("{\"diagnosis\":\"I do not know what this is.\"}"),
             FAULTCHAT_OK);
    CHECK_EQ(rep.has_fix, 0);
    CHECK_EQ(rep.fix_len, 0);
    CHECK_STR(rep.fix, "");

    /* An explicit null is the same answer written differently. */
    CHECK_EQ(parse("{\"diagnosis\":\"no idea\",\"fix\":null}"), FAULTCHAT_OK);
    CHECK_EQ(rep.has_fix, 0);
}

static void test_parse_tolerates_packaging(void) {
    setup();
    /* A markdown fence, which is what a model does by reflex. */
    CHECK_EQ(parse("```json\n{\"diagnosis\":\"fenced\",\"fix\":{\"action\":"
                   "\"refuse\"}}\n```"), FAULTCHAT_OK);
    CHECK_STR(rep.diagnosis, "fenced");
    CHECK_STR(rep.fix, "{\"action\":\"refuse\"}");

    /* A preamble. */
    CHECK_EQ(parse("Here is my analysis:\n{\"diagnosis\":\"preamble\"}\n"
                   "Let me know if you need more."), FAULTCHAT_OK);
    CHECK_STR(rep.diagnosis, "preamble");

    /* Leading and trailing whitespace only: the fast path. */
    CHECK_EQ(parse("\n\n  {\"diagnosis\":\"spaced\"}  \n"), FAULTCHAT_OK);
    CHECK_STR(rep.diagnosis, "spaced");

    /* Braces inside strings must not confuse the span finder. The object here
     * starts AFTER a decoy brace that lives inside a quoted string. */
    CHECK_EQ(parse("prose \"{not an object\" then "
                   "{\"diagnosis\":\"the } and { here are inside a string\"}"),
             FAULTCHAT_OK);
    CHECK_CONTAINS(rep.diagnosis, "inside a string");

    /* An escaped quote inside a string must not end the string early. */
    CHECK_EQ(parse("{\"diagnosis\":\"he said \\\"} { \\\" and continued\"}"),
             FAULTCHAT_OK);
    CHECK_CONTAINS(rep.diagnosis, "and continued");
}

static void test_parse_rejects_everything_else(void) {
    setup();

    /* Not JSON at all. */
    CHECK_EQ(faultchat_parse_reply("not json", 8, &rep, why, sizeof why),
             FAULTCHAT_EPROTO);
    CHECK_CONTAINS(why, "not valid JSON");

    /* Valid JSON, but not a Messages API response. */
    CHECK_EQ(faultchat_parse_reply("{\"hello\":1}", 11, &rep, why, sizeof why),
             FAULTCHAT_EPROTO);

    /* Empty body / NULL body. */
    CHECK_EQ(faultchat_parse_reply("", 0, &rep, why, sizeof why),
             FAULTCHAT_EPROTO);
    CHECK_EQ(faultchat_parse_reply(NULL, 10, &rep, why, sizeof why),
             FAULTCHAT_EPROTO);
    CHECK_EQ(faultchat_parse_reply("{}", 2, NULL, why, sizeof why),
             FAULTCHAT_EINVAL);

    /* A well-formed response carrying only prose. */
    CHECK_EQ(parse("I think the page tables are wrong, but I cannot be sure."),
             FAULTCHAT_EPROTO);
    CHECK_CONTAINS(why, "no JSON object");

    /* An object with no diagnosis. */
    CHECK_EQ(parse("{\"fix\":{\"action\":\"skip\"}}"), FAULTCHAT_EPROTO);
    CHECK_CONTAINS(why, "no \"diagnosis\" field");

    /* diagnosis of the wrong type. */
    CHECK_EQ(parse("{\"diagnosis\":42}"), FAULTCHAT_EPROTO);
    CHECK_CONTAINS(why, "not a string");
    CHECK_EQ(parse("{\"diagnosis\":{\"text\":\"x\"}}"), FAULTCHAT_EPROTO);
    CHECK_EQ(parse("{\"diagnosis\":[\"x\"]}"), FAULTCHAT_EPROTO);

    /* fix of the wrong type. Every one of these would otherwise be handed to a
     * tool as its argument object. */
    CHECK_EQ(parse("{\"diagnosis\":\"d\",\"fix\":\"skip\"}"), FAULTCHAT_EPROTO);
    CHECK_CONTAINS(why, "not a JSON object");
    CHECK_EQ(parse("{\"diagnosis\":\"d\",\"fix\":[\"skip\"]}"), FAULTCHAT_EPROTO);
    CHECK_EQ(parse("{\"diagnosis\":\"d\",\"fix\":14}"), FAULTCHAT_EPROTO);
    CHECK_EQ(parse("{\"diagnosis\":\"d\",\"fix\":true}"), FAULTCHAT_EPROTO);

    /* Truncated JSON: no balanced object exists. */
    CHECK_EQ(parse("{\"diagnosis\":\"cut off here"), FAULTCHAT_EPROTO);
    CHECK_EQ(parse("{\"diagnosis\":\"d\",\"fix\":{\"action\":\"skip\""),
             FAULTCHAT_EPROTO);

    /* Nothing above may have left a fix behind. */
    CHECK_EQ(rep.has_fix, 0);
}

static void test_parse_bounds_the_fix_and_the_diagnosis(void) {
    setup();

    /* An absurd fix object. Nothing legitimate is over a few dozen bytes, and a
     * huge one is either an attack or a confused model; either way it is not
     * going anywhere near the tool. */
    static char big[9000];
    size_t k = 0;
    k += (size_t)snprintf(big + k, sizeof big - k, "{\"diagnosis\":\"d\",\"fix\":{");
    for (int i = 0; i < 300 && k < sizeof big - 64; i++)
        k += (size_t)snprintf(big + k, sizeof big - k, "\"k%d\":\"vvvvvvvvvvvv\",", i);
    k += (size_t)snprintf(big + k, sizeof big - k, "\"action\":\"skip\"}}");
    CHECK_EQ(parse(big), FAULTCHAT_EPROTO);
    CHECK_CONTAINS(why, "larger than any legitimate recovery plan");
    CHECK_EQ(rep.has_fix, 0);

    /* A 5,000-character diagnosis — ten times FAULTCHAT_DIAG_MAX, and more than
     * max_tokens can actually produce — is CLIPPED, not refused: the fix beside
     * it may still be the useful half of the answer. */
    static char longd[8000];
    k = 0;
    k += (size_t)snprintf(longd + k, sizeof longd - k, "{\"diagnosis\":\"");
    for (int i = 0; i < 5000; i++) longd[k++] = 'x';
    snprintf(longd + k, sizeof longd - k,
             "\",\"fix\":{\"action\":\"refuse\"}}");
    CHECK_EQ(parse(longd), FAULTCHAT_OK);
    CHECK(strlen(rep.diagnosis) < FAULTCHAT_DIAG_MAX);
    CHECK_EQ(rep.has_fix, 1);
    CHECK_STR(rep.fix, "{\"action\":\"refuse\"}");

    /* Beyond the response buffer itself is a different failure and gets a
     * different answer: the reply is refused rather than half-read. */
    static char huge[20000];
    k = 0;
    k += (size_t)snprintf(huge + k, sizeof huge - k, "{\"diagnosis\":\"");
    for (int i = 0; i < 17000; i++) huge[k++] = 'y';
    snprintf(huge + k, sizeof huge - k, "\"}");
    CHECK_EQ(parse(huge), FAULTCHAT_EPROTO);
    CHECK_EQ(rep.has_fix, 0);
}

/* The diagnosis is printed on the same console the kernel writes its
 * [bracketed] ground truth to. A model that can print a bracket can forge a
 * kernel statement, which is the one thing the whole trace convention rests on
 * not being possible. */
static void test_diagnosis_cannot_forge_a_kernel_line(void) {
    setup();
    CHECK_EQ(parse("{\"diagnosis\":\"ok\\n[vfs_write /etc/shadow bytes=9 -> ok]"
                   "\\nand \\u0000 embedded\\ttabs\\rand \\u001b[31m escapes\"}"),
             FAULTCHAT_OK);

    CHECK(strchr(rep.diagnosis, '[')  == NULL);
    CHECK(strchr(rep.diagnosis, ']')  == NULL);
    CHECK(strchr(rep.diagnosis, '\n') == NULL);
    CHECK(strchr(rep.diagnosis, '\r') == NULL);
    CHECK(strchr(rep.diagnosis, '\t') == NULL);
    CHECK(strchr(rep.diagnosis, '\033') == NULL);
    /* An embedded NUL must not truncate the rest of the sentence away. */
    CHECK_CONTAINS(rep.diagnosis, "embedded");
    CHECK_CONTAINS(rep.diagnosis, "escapes");
    /* The forged line is still readable, just unmistakably not the kernel's. */
    CHECK_CONTAINS(rep.diagnosis, "(vfs_write /etc/shadow bytes=9 -> ok)");
}

/* ====================================================================== */
/* applying a proposed fix                                                */
/* ====================================================================== */

static void test_a_good_fix_is_armed_through_the_real_tool(void) {
    setup();
    inject_pf();
    CHECK_EQ(parse("{\"diagnosis\":\"d\",\"fix\":{\"action\":\"skip\","
                   "\"vector\":14,\"uses\":2}}"), FAULTCHAT_OK);

    kcap_reset();
    trace_reset();
    CHECK_EQ(faultchat_apply_fix(&rep, why, sizeof why), FAULTCHAT_OK);

    const fault_plan_t *p = fault_plan_get();
    CHECK(p != NULL);
    if (p) {
        CHECK_EQ(p->action, FAULT_ACT_SKIP);
        CHECK_EQ(p->vector, 14);
        CHECK_EQ(p->any_vector, 0);
        CHECK_EQ(p->uses, 2);
    }
    /* The kernel's own ground-truth line, emitted in C by tool_dispatch from
     * the real return value. Without it the operator has no evidence that a
     * fix was armed at all. */
    CHECK_EQ(trace_count(), 1);
    CHECK_CONTAINS(kcap_text(), "[fault_recover");
    CHECK_CONTAINS(kcap_text(), "action=skip");
}

static void test_a_bad_fix_is_refused_and_arms_nothing(void) {
    static const char *const bad[] = {
        "{\"action\":\"rm -rf /\"}",                 /* not an action        */
        "{\"action\":\"set_register\",\"register\":\"rsp\",\"value\":\"0x0\"}",
        "{\"action\":\"set_register\"}",             /* no register, no value */
        "{\"action\":\"set_rip\",\"value\":\"0x1000000000\"}",  /* out of image */
        "{\"action\":\"skip\",\"uses\":999}",        /* out of range          */
        "{\"action\":\"skip\",\"vector\":9999}",     /* not a vector          */
        "{\"action\":\"skip\",\"reg\":\"rax\"}",     /* unknown field         */
        "{\"vector\":14}",                           /* no action             */
        "{}",                                        /* nothing at all        */
        "{\"action\":\"skip\",\"vector\":8}",        /* #DF is never resumable*/
    };

    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        setup();
        inject_pf();

        memset(&rep, 0, sizeof rep);
        rep.has_fix = 1;
        snprintf(rep.fix, sizeof rep.fix, "%s", bad[i]);
        rep.fix_len = strlen(rep.fix);

        int rc = faultchat_apply_fix(&rep, why, sizeof why);
        if (rc != FAULTCHAT_EFIX)
            printf("    (fix %u accepted unexpectedly: %s)\n", i, bad[i]);
        CHECK_EQ(rc, FAULTCHAT_EFIX);
        CHECK(why[0] != '\0');
        /* The decisive assertion: nothing is armed. */
        CHECK(fault_plan_get() == NULL);
    }
}

/* The refusal text is quoted onto the console inside a kernel-prefixed
 * "[fault-diagnose] the proposed fix was NOT armed: ..." line, and it is NOT all
 * kernel-authored: recover_fail() in tools/fault_tools.c echoes the model's own
 * words back (the unknown field name, the action word, the register name),
 * decoded through json_str(), so up to FAULT_WORD_MAX-1 bytes of the model's
 * choosing survive into it. Those bytes must be defanged exactly as the
 * diagnosis is, or the model gets to put a '[' inside a kernel line — and a raw
 * CR gets to put one at column zero on any terminal that honours it. */
static void test_a_refusal_cannot_forge_a_kernel_line(void) {
    static const char *const forging[] = {
        /* an unknown field name whose text looks like kernel ground truth */
        "{\"[vfs_write /etc/shadow -> ok]\":1}",
        /* the same, prefixed with a CR so a terminal redraws from column zero */
        "{\"\\r[fault_recover -> ok]\":1}",
        /* the action word, and an escape sequence for good measure */
        "{\"action\":\"\\u001b[31m[x]\"}",
        /* the register name */
        "{\"action\":\"set_register\",\"register\":\"]a[\",\"value\":\"0x1\"}",
    };

    for (unsigned i = 0; i < sizeof forging / sizeof forging[0]; i++) {
        setup();
        inject_pf();

        memset(&rep, 0, sizeof rep);
        rep.has_fix = 1;
        snprintf(rep.fix, sizeof rep.fix, "%s", forging[i]);
        rep.fix_len = strlen(rep.fix);

        CHECK_EQ(faultchat_apply_fix(&rep, why, sizeof why), FAULTCHAT_EFIX);
        CHECK(fault_plan_get() == NULL);

        /* Not one byte the model chose can survive into the quoted sentence. */
        CHECK(strchr(why, '[')    == NULL);
        CHECK(strchr(why, ']')    == NULL);
        CHECK(strchr(why, '\r')   == NULL);
        CHECK(strchr(why, '\n')   == NULL);
        CHECK(strchr(why, '\t')   == NULL);
        CHECK(strchr(why, '\033') == NULL);
        /* ...and it is still a sentence, not blanked out. */
        CHECK(why[0] != '\0');
        CHECK_CONTAINS(why, "fault_recover");
    }

    /* lib/trace.c escapes the identical bytes one line earlier. The two voices
     * must agree about the rule inside a single console transcript. */
    CHECK_CONTAINS(kcap_text(), "\\x5b");
}

static void test_apply_with_nothing_to_apply(void) {
    setup();
    memset(&rep, 0, sizeof rep);
    CHECK_EQ(faultchat_apply_fix(&rep, why, sizeof why), FAULTCHAT_ENONE);
    CHECK_EQ(faultchat_apply_fix(NULL, why, sizeof why), FAULTCHAT_EINVAL);
    CHECK(fault_plan_get() == NULL);
}

/* ====================================================================== */
/* the pump, end to end against the mock                                  */
/* ====================================================================== */

static void test_pump_end_to_end(void) {
    setup();

    CHECK_EQ(faultchat_pending(), 0);
    CHECK_EQ(faultchat_pump(), FAULTCHAT_ENONE);

    inject_pf();
    CHECK_EQ(faultchat_pending(), 1);

    model_mock_queue(200, reply_body(
        "{\"diagnosis\":\"The instruction at RIP stores through rax, which "
        "holds 0x1000000000 - an address above the identity map. Stepping over "
        "it is safe here.\","
        "\"fix\":{\"action\":\"skip\",\"vector\":14,\"uses\":1}}"));

    CHECK_EQ(faultchat_pump(), FAULTCHAT_OK);

    /* --- the request the kernel actually sent --- */
    CHECK_EQ(model_mock_request_count(), 1);
    const char *req = model_mock_last_request();
    CHECK(req != NULL);
    CHECK_CONTAINS(req, "\"role\":\"user\"");
    CHECK_CONTAINS(req, "KERNEL FAULT: #PF (vector 14) - Page Fault");
    CHECK_CONTAINS(req, "48 c7 00 34 12 00 00");
    CHECK_CONTAINS(req, "ANSWER WITH ONE JSON OBJECT");
    /* The system prompt rides along, so the model is told the answer shape in
     * the same request that asks the question. */
    CHECK_CONTAINS(req, "\"system\":");
    CHECK_CONTAINS(req, "OMIT");
    /* No tools: a crash is the worst possible moment to hand the model a
     * syscall table. */
    CHECK(strstr(req, "\"tools\":") == NULL);
    CHECK_EQ(model_mock_request_truncated(0), 0);
    CHECK_EQ(model_mock_pending(), 0);

    /* --- what the operator saw --- */
    const char *out = kcap_text();
    CHECK_CONTAINS(out, "[fault-diagnose] fault #1 (#PF, vector 14");
    CHECK_CONTAINS(out, "asking the model what it was");
    CHECK_CONTAINS(out, "an address above the identity map");   /* model prose */
    CHECK_CONTAINS(out, "[fault_recover");                      /* kernel fact */
    CHECK_CONTAINS(out, "the fix is armed");

    /* --- and the machine state that results --- */
    const fault_plan_t *p = fault_plan_get();
    CHECK(p != NULL);
    if (p) CHECK_EQ(p->action, FAULT_ACT_SKIP);
    CHECK_EQ(faultchat_diagnoses(), 1);
    CHECK_EQ(faultchat_fixes_armed(), 1);
    CHECK_EQ(faultchat_last_seq(), 1);
    CHECK_EQ(faultchat_last_result(), FAULTCHAT_OK);
    CHECK_CONTAINS(faultchat_last_diagnosis(), "Stepping over it is safe");

    /* --- one fault, one diagnosis. The pump must not re-ask. --- */
    CHECK_EQ(faultchat_pending(), 0);
    CHECK_EQ(faultchat_pump(), FAULTCHAT_ENONE);
    CHECK_EQ(model_mock_request_count(), 1);
}

static void test_pump_with_a_diagnosis_and_no_fix(void) {
    setup();
    inject_pf();
    model_mock_queue(200, reply_body(
        "{\"diagnosis\":\"Something dereferenced a pointer 64 GiB up. I cannot "
        "tell what from this alone.\"}"));

    CHECK_EQ(faultchat_pump(), FAULTCHAT_OK);
    CHECK(fault_plan_get() == NULL);
    CHECK_EQ(faultchat_fixes_armed(), 0);
    CHECK_CONTAINS(kcap_text(),
                   "proposed neither a fix nor a code patch, which is a "
                   "complete answer");
}

static void test_pump_when_the_model_refuses(void) {
    setup();
    inject_pf();
    model_mock_queue(200, reply_body(
        "{\"diagnosis\":\"I do not know.\",\"fix\":{\"action\":\"refuse\"}}"));

    CHECK_EQ(faultchat_pump(), FAULTCHAT_OK);
    const fault_plan_t *p = fault_plan_get();
    CHECK(p != NULL);
    /* "refuse" is a real, armed decision and is NOT the same as no plan: the
     * report will say the machine chose to halt rather than that nobody
     * decided. */
    if (p) CHECK_EQ(p->action, FAULT_ACT_REFUSE);
}

static void test_pump_without_a_transport(void) {
    setup();
    faultchat_bind(NULL);
    inject_pf();
    CHECK_EQ(faultchat_pump(), FAULTCHAT_ETRANSPORT);
    CHECK_CONTAINS(kcap_text(), "no transport is bound");
    CHECK(fault_plan_get() == NULL);
    /* Still claimed: the machine must not re-ask about the same crash forever. */
    CHECK_EQ(faultchat_pump(), FAULTCHAT_ENONE);

    /* And it costs budget. Without that, a machine with no network and a fault
     * storm announces every single one of them for the rest of the boot. Budget
     * is spent on the ATTEMPT, not on the send, which is exactly why a machine
     * with no transport is bounded at all. */
    CHECK_EQ(faultchat_diagnoses(), 1);
    for (unsigned i = 1; i < FAULTCHAT_MAX_DIAGNOSES; i++) {
        inject_pf_elsewhere();
        CHECK_EQ(faultchat_pump(), FAULTCHAT_ETRANSPORT);
    }
    inject_pf_elsewhere();
    CHECK_EQ(faultchat_pump(), FAULTCHAT_EOFF);
    CHECK_EQ(faultchat_disabled(), 1);
}

/* The tighter of the two bounds, on its own. A storm at ONE address is a machine
 * failing to make progress rather than a machine with many problems, and every
 * round of it is a paid API call, so it is capped well below the per-boot
 * budget — and NOT by latching the whole feature off, because giving up on one
 * bug is not the same as giving up on the machine. */
static void test_pump_gives_up_on_one_address_but_not_on_the_machine(void) {
    setup();
    faultchat_bind(NULL);

    for (unsigned i = 0; i < FAULTCHAT_MAX_PER_RIP; i++) {
        inject_pf();
        CHECK_EQ(faultchat_pump(), FAULTCHAT_ETRANSPORT);
    }
    inject_pf();
    CHECK_EQ(faultchat_pump(), FAULTCHAT_EOFF);
    CHECK_CONTAINS(kcap_text(), "already asked about");
    CHECK_EQ(faultchat_diagnoses(), FAULTCHAT_MAX_PER_RIP);
    /* Not latched: a fault somewhere else still gets asked about. */
    CHECK_EQ(faultchat_disabled(), 0);
    inject_pf_elsewhere();
    CHECK_EQ(faultchat_pump(), FAULTCHAT_ETRANSPORT);
    CHECK_EQ(faultchat_diagnoses(), FAULTCHAT_MAX_PER_RIP + 1);
}

static void test_pump_transport_and_http_failures(void) {
    setup();
    inject_pf();
    model_mock_queue_error(MODEL_ETIMEOUT, "mock: no route to the model");
    CHECK_EQ(faultchat_pump(), FAULTCHAT_ETRANSPORT);
    CHECK_CONTAINS(kcap_text(), "no route to the model");
    CHECK(fault_plan_get() == NULL);

    setup();
    inject_pf();
    model_mock_queue(401, "{\"error\":{\"message\":\"invalid x-api-key\"}}");
    CHECK_EQ(faultchat_pump(), FAULTCHAT_EHTTP);
    CHECK_CONTAINS(kcap_text(), "401");
    CHECK(fault_plan_get() == NULL);

    setup();
    inject_pf();
    model_mock_queue(500, "oops");
    CHECK_EQ(faultchat_pump(), FAULTCHAT_EHTTP);
    CHECK(fault_plan_get() == NULL);
}

static void test_pump_with_an_unintelligible_reply(void) {
    static const char *const junk[] = {
        "I have no idea what happened, sorry.",
        "{\"diagnosis\":}",
        "{\"fix\":{\"action\":\"skip\"}}",
        "{\"diagnosis\":\"d\",\"fix\":\"skip\"}",
        "",
    };
    for (unsigned i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        setup();
        inject_pf();
        model_mock_queue(200, reply_body(junk[i]));
        CHECK_EQ(faultchat_pump(), FAULTCHAT_EPROTO);
        CHECK_CONTAINS(kcap_text(), "was not a diagnosis");
        CHECK(fault_plan_get() == NULL);
    }

    /* A response body that is not a Messages API message at all. */
    setup();
    inject_pf();
    model_mock_queue(200, "[]");
    CHECK_EQ(faultchat_pump(), FAULTCHAT_EPROTO);
    CHECK(fault_plan_get() == NULL);
}

static void test_pump_with_a_refused_fix(void) {
    setup();
    inject_pf();
    model_mock_queue(200, reply_body(
        "{\"diagnosis\":\"rsp is wrong\",\"fix\":{\"action\":\"set_register\","
        "\"register\":\"rsp\",\"value\":\"0x1000\"}}"));

    CHECK_EQ(faultchat_pump(), FAULTCHAT_EFIX);
    CHECK(fault_plan_get() == NULL);
    CHECK_EQ(faultchat_fixes_armed(), 0);
    CHECK_CONTAINS(kcap_text(), "was NOT armed");
    /* The diagnosis is still printed and kept: a refused fix does not make the
     * explanation worthless. */
    CHECK_CONTAINS(kcap_text(), "rsp is wrong");
    CHECK_CONTAINS(faultchat_last_diagnosis(), "rsp is wrong");
}

static void test_pump_budget_is_finite(void) {
    setup();
    /* A different address every round, so what is being measured here is the
     * PER-BOOT budget and not the tighter per-address one (which has its own
     * test above). */
    for (unsigned i = 0; i < FAULTCHAT_MAX_DIAGNOSES; i++) {
        inject_pf_elsewhere();
        model_mock_queue(200, reply_body("{\"diagnosis\":\"again\"}"));
        CHECK_EQ(faultchat_pump(), FAULTCHAT_OK);
    }
    CHECK_EQ(faultchat_diagnoses(), FAULTCHAT_MAX_DIAGNOSES);

    inject_pf_elsewhere();
    CHECK_EQ(faultchat_pending(), 0);
    CHECK_EQ(faultchat_pump(), FAULTCHAT_EOFF);
    CHECK_CONTAINS(kcap_text(), "already spent its");
    CHECK_EQ(faultchat_disabled(), 1);
    CHECK_CONTAINS(faultchat_disabled_reason(), "budget");
    /* And it stays off: no further request is built. */
    size_t sent = model_mock_request_count();
    inject_pf_elsewhere();
    CHECK_EQ(faultchat_pump(), FAULTCHAT_EOFF);
    CHECK_EQ(model_mock_request_count(), sent);
}

static void test_enable_switch(void) {
    setup();
    faultchat_enable(0);
    inject_pf();
    CHECK_EQ(faultchat_pending(), 0);
    CHECK_EQ(faultchat_pump(), FAULTCHAT_EOFF);
    CHECK_EQ(model_mock_request_count(), 0);

    faultchat_enable(1);
    model_mock_queue(200, reply_body("{\"diagnosis\":\"back on\"}"));
    CHECK_EQ(faultchat_pump(), FAULTCHAT_OK);
}

/* ====================================================================== */
/* THE ONE THAT MATTERS: a fault while a diagnosis is in flight            */
/* ====================================================================== */

/* A transport that faults halfway through sending, exactly as lwIP or mbedTLS
 * would if the original fault had left the heap mid-mutation. It also tries to
 * re-enter the pump from inside itself — which is what a naive "diagnose on
 * every fault" hook would do — so the re-entrancy latch is proved, not assumed.
 *
 * It then returns a PERFECTLY GOOD reply carrying a valid fix. That is the point
 * of the test: the reply is discarded anyway, because a transport that was
 * interrupted mid-state-machine cannot be trusted to have produced it. */
static int      hostile_entries;
static int      hostile_nested_rc;
static int      hostile_fault;      /* raise a fault during send? */

static int hostile_send(model_transport_t *t,
                        const char *req_body, size_t req_len,
                        char *resp_buf, size_t resp_cap,
                        model_response_t *out) {
    (void)t; (void)req_body; (void)req_len;
    hostile_entries++;

    if (hostile_fault) {
        /* A fault taken and RECOVERED inside the network stack. */
        inject(14, 0x0, INSN_MOV_STORE, sizeof INSN_MOV_STORE, 1);
        /* ...and the naive reaction to it: diagnose the new fault right now,
         * from inside the transport that is still running. */
        hostile_nested_rc = faultchat_pump();
    }

    const char *b = reply_body(
        "{\"diagnosis\":\"looks like a null store\","
        "\"fix\":{\"action\":\"skip\",\"vector\":14,\"uses\":4}}");
    size_t n = strlen(b);
    if (n >= resp_cap) n = resp_cap - 1;
    memcpy(resp_buf, b, n);
    resp_buf[n] = '\0';

    out->http_status = 200;
    snprintf(out->status_line, sizeof out->status_line, "HTTP/1.1 200 OK");
    out->body      = resp_buf;
    out->body_len  = n;
    out->truncated = 0;
    return MODEL_OK;
}

static const char *hostile_last_error(model_transport_t *t) {
    (void)t; return "";
}

static model_transport_t hostile = {
    "hostile", hostile_send, hostile_last_error, NULL
};

static void test_a_fault_during_diagnosis_does_not_recurse(void) {
    fault_reset();
    arena_init();
    hostile_entries   = 0;
    hostile_nested_rc = 12345;
    hostile_fault     = 1;
    faultchat_bind(&hostile);
    kcap_reset();

    inject_pf();
    int rc = faultchat_pump();

    /* 1. The outer diagnosis reports the recursion, not success. */
    CHECK_EQ(rc, FAULTCHAT_ERECURSE);

    /* 2. The nested call — the one made from inside the transport — did
     *    nothing at all. This is the property that keeps a diagnosis from
     *    becoming a fault loop through a non-reentrant network stack. */
    CHECK_EQ(hostile_nested_rc, FAULTCHAT_EBUSY);

    /* 3. The transport was entered exactly once. No recursion happened. */
    CHECK_EQ(hostile_entries, 1);

    /* 4. The reply was good and carried a valid fix, and it was still thrown
     *    away. Nothing is armed. */
    CHECK(fault_plan_get() == NULL);
    CHECK_EQ(faultchat_fixes_armed(), 0);

    /* 5. Diagnosis is latched off for the rest of the boot. */
    CHECK_EQ(faultchat_disabled(), 1);
    CHECK_CONTAINS(faultchat_disabled_reason(), "in flight");
    CHECK_CONTAINS(kcap_text(),
                   "A FAULT OCCURRED WHILE THE DIAGNOSIS REQUEST WAS IN FLIGHT");
    CHECK_CONTAINS(kcap_text(), "not reentrant and will not be entered again");

    /* 6. And it never re-arms. More faults, more pumps, no more requests. */
    for (int i = 0; i < 5; i++) {
        inject_pf();
        CHECK_EQ(faultchat_pending(), 0);
        CHECK_EQ(faultchat_pump(), FAULTCHAT_EOFF);
    }
    CHECK_EQ(hostile_entries, 1);

    /* 7. Only faultchat_bind()/faultchat_reset() clears it — i.e. a deliberate
     *    act, not the passage of time. */
    faultchat_bind(&hostile);
    hostile_fault = 0;
    inject_pf();
    CHECK_EQ(faultchat_pump(), FAULTCHAT_OK);
    CHECK_EQ(hostile_entries, 2);
}

/* The same transport, without the fault: proves the recursion verdict above is
 * caused by the fault and not by anything else about this transport. */
static void test_the_same_transport_succeeds_without_a_fault(void) {
    fault_reset();
    arena_init();
    hostile_entries = 0;
    hostile_fault   = 0;
    faultchat_bind(&hostile);
    kcap_reset();

    inject_pf();
    CHECK_EQ(faultchat_pump(), FAULTCHAT_OK);
    CHECK_EQ(faultchat_disabled(), 0);
    CHECK(fault_plan_get() != NULL);
    CHECK_EQ(hostile_entries, 1);
}

/* ====================================================================== */
/* fuzz                                                                   */
/* ====================================================================== */

static uint32_t rng_state = 0x5eed1234u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Random bytes through the reply parser. The contract is absolute: it returns,
 * it never writes outside its buffers, the strings it leaves behind are always
 * NUL-terminated, and a fix that survives is always one the plan checker
 * accepts. */
static void test_fuzz_replies(void) {
    static const char alphabet[] =
        "{}[]\":,\\ \n\tabcdefgxyz0123456789-+.eE/*"
        "diagnosisfixactionskipset_registerset_ripvalueusesvectorrspraxnulltrue";

    for (int iter = 0; iter < 4000; iter++) {
        setup();
        inject_pf();

        char text[256];
        size_t n = rnd() % (sizeof text - 1);
        for (size_t i = 0; i < n; i++)
            text[i] = alphabet[rnd() % (sizeof alphabet - 1)];
        text[n] = '\0';

        const char *b = reply_body(text);
        memset(&rep, 0xAA, sizeof rep);
        int rc = faultchat_parse_reply(b, strlen(b), &rep, why, sizeof why);

        CHECK(rc == FAULTCHAT_OK || rc == FAULTCHAT_EPROTO);
        /* Pre-poisoned with 0xAA, so "a NUL exists inside the array" is a real
         * assertion that the writer terminated within bounds. */
        CHECK(memchr(rep.diagnosis, 0, sizeof rep.diagnosis) != NULL);
        CHECK(memchr(rep.fix, 0, sizeof rep.fix) != NULL);
        CHECK(why[sizeof why - 1] == '\0');

        if (rc == FAULTCHAT_OK) {
            CHECK(strlen(rep.diagnosis) < FAULTCHAT_DIAG_MAX);
            if (rep.has_fix) {
                CHECK_EQ(rep.fix_len, strlen(rep.fix));
                /* Whatever came out, the tool is still the gate. */
                int arc = faultchat_apply_fix(&rep, why, sizeof why);
                CHECK(arc == FAULTCHAT_OK || arc == FAULTCHAT_EFIX);
                if (arc == FAULTCHAT_OK) {
                    const fault_plan_t *p = fault_plan_get();
                    CHECK(p != NULL);
                    if (p)
                        CHECK_EQ(fault_plan_check(p, fault_window()),
                                 FAULT_FIX_OK);
                }
            } else {
                CHECK(fault_plan_get() == NULL);
            }
        } else {
            CHECK_EQ(rep.has_fix, 0);
            CHECK(fault_plan_get() == NULL);
        }
    }
}

/* Random bytes straight into the whole pump, as a response body: the parser is
 * only one of the things that has to survive a hostile reply. */
static void test_fuzz_pump(void) {
    static char raw[512];

    for (int iter = 0; iter < 1500; iter++) {
        setup();
        inject_pf();

        size_t n = rnd() % (sizeof raw - 1);
        for (size_t i = 0; i < n; i++)
            raw[i] = (char)(0x20 + (rnd() % 0x5e));
        raw[n] = '\0';

        model_mock_queue((rnd() & 7) ? 200 : 429, raw);
        int rc = faultchat_pump();
        CHECK(rc == FAULTCHAT_OK || rc == FAULTCHAT_EPROTO ||
              rc == FAULTCHAT_EHTTP || rc == FAULTCHAT_EFIX);
        /* Random bytes cannot produce a plan. If they ever did, the plan would
         * still have to be a valid one — assert both. */
        const fault_plan_t *p = fault_plan_get();
        if (p) CHECK_EQ(fault_plan_check(p, fault_window()), FAULT_FIX_OK);
    }
}

/* ====================================================================== */

int main(void) {
    tools_bind();

    RUN(test_system_prompt_states_the_contract);

    RUN(test_prompt_carries_what_a_model_needs);
    RUN(test_prompt_warns_when_skip_is_impossible);
    RUN(test_prompt_reports_an_unrecoverable_vector);
    RUN(test_prompt_reports_a_repeat);
    RUN(test_prompt_survives_no_window_and_no_record);

    RUN(test_parse_a_clean_reply);
    RUN(test_parse_a_reply_with_no_fix);
    RUN(test_parse_tolerates_packaging);
    RUN(test_parse_rejects_everything_else);
    RUN(test_parse_bounds_the_fix_and_the_diagnosis);
    RUN(test_diagnosis_cannot_forge_a_kernel_line);

    RUN(test_a_good_fix_is_armed_through_the_real_tool);
    RUN(test_a_bad_fix_is_refused_and_arms_nothing);
    RUN(test_a_refusal_cannot_forge_a_kernel_line);
    RUN(test_apply_with_nothing_to_apply);

    RUN(test_pump_end_to_end);
    RUN(test_pump_with_a_diagnosis_and_no_fix);
    RUN(test_pump_when_the_model_refuses);
    RUN(test_pump_without_a_transport);
    RUN(test_pump_transport_and_http_failures);
    RUN(test_pump_with_an_unintelligible_reply);
    RUN(test_pump_with_a_refused_fix);
    RUN(test_pump_gives_up_on_one_address_but_not_on_the_machine);
    RUN(test_pump_budget_is_finite);
    RUN(test_enable_switch);

    RUN(test_a_fault_during_diagnosis_does_not_recurse);
    RUN(test_the_same_transport_succeeds_without_a_fault);

    RUN(test_fuzz_replies);
    RUN(test_fuzz_pump);

    return th_report("fault_diagnose");
}
