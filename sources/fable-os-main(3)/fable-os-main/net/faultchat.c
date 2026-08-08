/* faultchat.c — the kernel asking the model to diagnose its own crash.
 *
 * See include/faultchat.h for the safe-context argument, which is the only part
 * of this that is hard. The code itself is deliberately dull: fixed buffers, one
 * request, one reply, and every interesting decision delegated to something that
 * already exists (json.h parses, fault_recover validates, fault.c decides).
 *
 * FILE LAYOUT
 *   bounded appender / sanitiser      text that cannot overflow or forge
 *   the system prompt                 what the kernel says it wants back
 *   faultchat_build_prompt            record -> everything a model needs
 *   object span finder / parse_reply  untrusted reply -> faultchat_reply_t
 *   faultchat_apply_fix               reply -> the fault_recover tool
 *   the live half                     the polling pump and its three latches
 *
 * Nothing in this file calls kmalloc, directly or indirectly, and nothing in it
 * runs inside an exception handler. Both of those are load-bearing; see the
 * header.
 */

#include "faultchat.h"

#include "json.h"
#include "kernel.h"
#include "tool.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>      /* vsnprintf — the libc shim's, when freestanding */

/* ====================================================================== */
/* bounded text                                                           */
/* ====================================================================== */

/* snprintf-style accumulation, identical in contract to arch/x86_64/fault.c's:
 * `*off` is the length that WOULD have been written, so one test at the end
 * detects overflow and every intermediate call is harmless once full. */
static void ap(char *dst, size_t cap, size_t *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void ap(char *dst, size_t cap, size_t *off, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    size_t used = (*off < cap) ? *off : cap;
    int    n    = vsnprintf(dst + used, cap - used, fmt, a);
    va_end(a);
    if (n > 0) *off += (size_t)n;
}

/* Copy model-controlled text into a fixed buffer, defanged.
 *
 * Two substitutions, each for a stated reason:
 *   - control characters (and DEL) become spaces. A newline inside the model's
 *     prose would let it start a line of its own on the console.
 *   - '[' and ']' become '(' and ')'. Brackets are this kernel's mark for
 *     "the kernel is stating a fact it performed" (see trace.h). Prose that can
 *     print a bracket can forge one, and the whole audit story on a machine with
 *     no shell rests on that not being possible.
 * Bytes >= 0x80 pass through unchanged, exactly as net/chat.c passes model prose
 * through: they cannot forge anything, and mangling UTF-8 would only make a real
 * answer harder to read. Always NUL-terminates. */
static void sanitize(char *dst, size_t cap, const char *src, size_t len) {
    size_t o = 0;
    if (!dst || cap == 0) return;
    for (size_t i = 0; src && i < len && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7F) c = ' ';
        else if (c == '[')         c = '(';
        else if (c == ']')         c = ')';
        dst[o++] = (char)c;
    }
    /* Trailing whitespace is common (models end on a newline) and looks like a
     * bug on a console that prints the string inline. */
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
}

/* True when [addr, addr+len) lies wholly inside [lo, hi). Written so addr+len
 * is never formed. Same discipline as arch/x86_64/fault.c and tools/mem_tools.c;
 * duplicated rather than exported because the alternative is widening fault.h's
 * surface for four lines. */
static int in_range(uint64_t addr, uint64_t len, uint64_t lo, uint64_t hi) {
    if (hi <= lo)        return 0;
    if (len == 0)        return 0;
    if (addr < lo)       return 0;
    if (addr >= hi)      return 0;
    if (len > hi - addr) return 0;
    return 1;
}

/* ====================================================================== */
/* what the kernel tells the model it wants                                */
/* ====================================================================== */

static const char SYSTEM_PROMPT[] =
    "You are the resident intelligence of fable-os, a bare-metal x86-64 kernel. "
    "This is not an operator turn. The KERNEL is asking you, on its own "
    "initiative, about a CPU exception it just took and survived. Nobody typed "
    "anything; there is no conversation to continue and no follow-up question "
    "you can ask.\n"
    "Reply with ONE JSON object and nothing else - no prose around it, no "
    "markdown fence:\n"
    "{\"diagnosis\":\"one or two plain sentences: what the machine was doing "
    "and why it faulted\","
    "\"fix\":{\"action\":\"skip\",\"vector\":14,\"uses\":1}}\n"
    "\"diagnosis\" is required. \"fix\" is OPTIONAL and arms what this machine "
    "will do the NEXT time it takes a fatal exception; its fields are exactly "
    "the fault_recover tool's:\n"
    "  action    skip | set_register | set_rip | refuse | none\n"
    "  register  set_register only: rax rcx rdx rbx rbp rsi rdi r8..r15 "
    "(rsp is refused)\n"
    "  value     set_register / set_rip only: a hex string like \"0x0\"\n"
    "  then      set_register only: \"retry\" (default) or \"skip\"\n"
    "  vector    optional: apply only to this exception vector, 0..255\n"
    "  uses      optional: how many faults this plan may handle, 1..16\n"
    "OMIT \"fix\" ENTIRELY when you are not sure. A wrong fix is worse than no "
    "fix: it changes what the CPU executes next, it is applied without anyone "
    "confirming it, and this machine has no shell to recover from. A diagnosis "
    "with no fix is a useful, complete answer.\n"
    "{\"action\":\"refuse\"} is NOT the cautious answer and is not a way of "
    "declining: it ARMS a deliberate halt, so the next matching fatal exception "
    "stops this machine instead of surviving it. Use it only when halting is "
    "genuinely what should happen. To decline, omit \"fix\" entirely.\n"
    "You may ALSO include an optional \"patch\" object, which rewrites the "
    "faulting instructions in this running kernel's .text so the bug is gone "
    "rather than survived. Every page here is RWX, so this works with no "
    "compiler and no reboot, and the original bytes are always kept. Its fields "
    "are exactly the fault_patch tool's:\n"
    "  action   \"apply\"\n"
    "  address  the first byte to replace, hex, e.g. \"0x10a4f3\"\n"
    "  expect   REQUIRED: the bytes you believe are there now, hex e.g. "
    "\"48f7f1\". The patch is refused if they are not.\n"
    "  bytes    the replacement, hex, EXACTLY as long as \"expect\"\n"
    "  note     one short line saying why\n"
    "Both spans must cover a whole number of x86-64 instructions; 0x90 is a "
    "one-byte nop and pads any gap. A patch is the right answer when the report "
    "shows the machine ABANDONED a call chain, because that code will be run "
    "again and a recovery plan only survives the next fault instead of removing "
    "it. Omit \"patch\" unless the bytes at RIP are shown to you and you can "
    "say what they should be instead.";

const char *faultchat_system_prompt(void) { return SYSTEM_PROMPT; }

/* ====================================================================== */
/* the prompt                                                              */
/* ====================================================================== */

/* One line of hex bytes read live from `addr`, gated on the image window. Used
 * only for the bytes BEFORE RIP: the bytes at and after RIP come from the record
 * itself, which is what the CPU actually saw at fault time. */
static void hex_line(char *dst, size_t cap, size_t *off,
                     uint64_t addr, unsigned n, const fault_window_t *w) {
    if (!in_range(addr, n, w->image_lo, w->image_hi)) return;
    const volatile unsigned char *p =
        (const volatile unsigned char *)(uintptr_t)addr;
    ap(dst, cap, off, "  0x%016lx: ", (unsigned long)addr);
    for (unsigned i = 0; i < n; i++)
        ap(dst, cap, off, "%02x ", (unsigned)p[i]);
    ap(dst, cap, off, "\n");
}

#define FAULTCHAT_PRE_BYTES 32   /* bytes of instruction stream before RIP */

size_t faultchat_build_prompt(const fault_record_t *rec,
                              const fault_window_t *window,
                              char *dst, size_t cap) {
    size_t off = 0;
    if (cap) dst[0] = '\0';
    if (!rec || !rec->valid) {
        ap(dst, cap, &off, "no fault has been captured\n");
        return off;
    }

    const fault_frame_t *f = &rec->regs;

    ap(dst, cap, &off,
       "This machine took a CPU exception and SURVIVED it - the fault handler "
       "returned and normal kernel code is running again. Everything below was "
       "captured by the kernel at the moment of the fault. Diagnose it.\n");

    /* ---- machine state the report itself does not carry ---- */
    ap(dst, cap, &off, "\nMACHINE\n");
    ap(dst, cap, &off,
       "  uptime            : %lu ms\n"
       "  faults since boot : %lu (this is fault #%lu)\n",
       (unsigned long)rec->uptime_ms,
       (unsigned long)fault_count(), (unsigned long)rec->seq);

    if (fault_refaults() > 1)
        ap(dst, cap, &off,
           "  REPEATING         : 0x%016lx has now faulted %lu times in a row. "
           "The last recovery did NOT work. The kernel gives up and halts after "
           "%u attempts at one address.\n",
           (unsigned long)fault_refault_rip(), (unsigned long)fault_refaults(),
           (unsigned)FAULT_REFAULT_MAX);

    if (window)
        ap(dst, cap, &off,
           "  resume window     : [0x%016lx, 0x%016lx) - every address a "
           "recovery resumes at must be inside this range, or it is refused\n",
           (unsigned long)window->code_lo, (unsigned long)window->code_hi);
    else
        ap(dst, cap, &off,
           "  resume window     : not published on this build, so a set_rip fix "
           "cannot be validated and will be refused\n");

    ap(dst, cap, &off,
       "  vector recoverable: %s\n",
       fault_is_recoverable(f->vector)
           ? "yes"
           : "NO - this exception can never be resumed on this machine, so no "
             "fix will be accepted for it");

    /* Whether "skip" is even available is the single most actionable fact here:
     * proposing it when the length decoder will refuse wastes the one shot. */
    int ilen = (rec->code_valid && rec->code_len)
                   ? x86_insn_length(rec->code, rec->code_len) : 0;
    /* Used again by the patch section below, which is why it is not scoped. */
    if (ilen > 0)
        ap(dst, cap, &off,
           "  \"skip\" available  : yes - the instruction at RIP decodes to %d "
           "bytes with certainty, so a skip would resume at 0x%016lx\n",
           ilen, (unsigned long)(f->rip + (uint64_t)ilen));
    else
        ap(dst, cap, &off,
           "  \"skip\" available  : NO - the kernel's length decoder will not "
           "commit to a length for the bytes at RIP, so an \"action\":\"skip\" "
           "fix WILL be refused. Do not propose one.\n");

    {
        const fault_plan_t *p = fault_plan_get();
        if (!p)
            ap(dst, cap, &off, "  armed plan        : none\n");
        else
            ap(dst, cap, &off,
               "  armed plan        : action %u, %lu use(s) left - proposing a "
               "fix REPLACES it\n",
               (unsigned)p->action, (unsigned long)p->uses);
    }

    /* ---- the report: vector, decoded error, RIP, registers, code, backtrace,
     * and what the kernel decided. Byte-for-byte what the console printed. ---- */
    ap(dst, cap, &off,
       "\nFAULT REPORT (verbatim, exactly as the kernel printed it on the "
       "console)\n");
    {
        size_t used = (off < cap) ? off : cap;
        size_t n    = fault_format_report(rec, dst + used, cap - used);
        off += n;
    }

    /* ---- the instruction stream leading up to RIP ----
     * Read NOW rather than at fault time, and said so: the kernel maps every
     * page RWX, so in principle the bytes could have changed since. In practice
     * they have not, and knowing what runs into the faulting instruction is
     * often what identifies the function. */
    if (window && rec->code_valid) {
        ap(dst, cap, &off,
           "\nMEMORY AROUND RIP (the preceding bytes are read now, not at fault "
           "time)\n");
        uint64_t lo = (f->rip >= FAULTCHAT_PRE_BYTES)
                          ? f->rip - FAULTCHAT_PRE_BYTES : 0;
        for (uint64_t a = lo; a + 16 <= f->rip; a += 16)
            hex_line(dst, cap, &off, a, 16, window);
        ap(dst, cap, &off, "  0x%016lx: ", (unsigned long)f->rip);
        for (unsigned i = 0; i < rec->code_len; i++)
            ap(dst, cap, &off, "%02x ", (unsigned)rec->code[i]);
        ap(dst, cap, &off, "  <-- RIP, the faulting instruction starts here\n");
    }

    /* ---- can this be fixed for good, rather than merely survived? ----
     * The distinction the model has to be handed explicitly, because it is the
     * difference between a machine that keeps limping and a machine that is
     * repaired: a recovery plan changes what happens at the NEXT fault, while a
     * code patch removes the instruction that caused this one. Only the second
     * one is available if RIP is inside .text, and only the second one is any
     * use when the fault was ESCAPED — because escaping means the code was
     * abandoned rather than fixed, and something will run it again. */
    ap(dst, cap, &off, "\nCAN THIS BE PATCHED OUT?\n");
    if (window && window->text_hi > window->text_lo) {
        int in_text = (f->rip >= window->text_lo && f->rip < window->text_hi);
        ap(dst, cap, &off,
           "  patchable .text   : [0x%016lx, 0x%016lx)\n"
           "  RIP is %s\n",
           (unsigned long)window->text_lo, (unsigned long)window->text_hi,
           in_text ? "inside it, so the faulting instruction CAN be rewritten"
                   : "NOT inside it, so no code patch is possible for this "
                     "fault");
        if (in_text && rec->code_valid && rec->code_len) {
            ap(dst, cap, &off,
               "  bytes at RIP      : ");
            for (unsigned i = 0; i < rec->code_len && i < 8; i++)
                ap(dst, cap, &off, "%02x", (unsigned)rec->code[i]);
            ap(dst, cap, &off,
               " (the first %u are shown; the faulting instruction is the "
               "first %d of them)\n",
               (rec->code_len < 8u) ? (unsigned)rec->code_len : 8u,
               ilen > 0 ? ilen : 0);
            if (ilen > 0) {
                ap(dst, cap, &off,
                   "  a patch of it     : \"address\":\"0x%lx\", "
                   "\"expect\":\"", (unsigned long)f->rip);
                for (int i = 0; i < ilen && i < (int)rec->code_len; i++)
                    ap(dst, cap, &off, "%02x", (unsigned)rec->code[i]);
                ap(dst, cap, &off,
                   "\" and \"bytes\" of exactly %d byte(s)\n", ilen);
            } else {
                ap(dst, cap, &off,
                   "  a patch of it     : NOT possible - the length decoder "
                   "will not commit to a length for these bytes, so no span "
                   "starting at RIP can be shown to cover whole "
                   "instructions\n");
            }
        }
    } else {
        ap(dst, cap, &off,
           "  patchable .text   : not published on this build, so every code "
           "patch will be refused. Do not propose one.\n");
    }
    {
        uint32_t np = fault_patch_live();
        const fault_patch_t *cover = fault_patch_covering(f->rip);
        ap(dst, cap, &off,
           "  patches live now  : %lu\n", (unsigned long)np);
        if (cover)
            ap(dst, cap, &off,
               "  WARNING           : patch %lu already covers RIP. Something "
               "has already been rewritten here and it did not fix this. Do NOT "
               "patch it again - say so in the diagnosis instead.\n",
               (unsigned long)cover->id);
        if (faultchat_patches_applied() >= FAULTCHAT_MAX_PATCHES)
            ap(dst, cap, &off,
               "  budget            : this boot has already applied its %u "
               "code patches, so a \"patch\" WILL be refused\n",
               (unsigned)FAULTCHAT_MAX_PATCHES);
        else
            ap(dst, cap, &off,
               "  budget            : %u code patch(es) left this boot\n",
               (unsigned)(FAULTCHAT_MAX_PATCHES -
                          faultchat_patches_applied()));
    }

    /* ---- WHAT HAPPENS IF THE ANSWER IS "NOTHING" ----
     *
     * Added because a real model, asked twice about a live divide-by-zero it had
     * diagnosed perfectly — it named the opcode, the register and the reason —
     * proposed no patch and then {"action":"refuse"}. Everything it needed was
     * already above: RIP inside .text, the bytes, a ready-made expect string.
     * What was missing was the CONSEQUENCE. Every sentence in the contract warned
     * it off ("a wrong fix is worse than no fix", "omit them unless..."), and
     * nothing anywhere said what declining costs — so declining read as the
     * cautious choice when it was the choice that left the bug in the kernel.
     *
     * This is the one fact the kernel holds and the model cannot infer: an
     * ESCAPED fault means the code was ABANDONED, not fixed. Whatever called it
     * will call it again, fault again, and be shut down by its own failure
     * counter, and the per-address diagnosis limit means this question will not
     * be asked a third time. Doing nothing is not neutral here, and saying so is
     * not pressure to guess — the paragraph below states the alternative and the
     * reversibility in the same breath. */
    if (rec->fix_set && rec->fix == FAULT_FIX_ESCAPED) {
        ap(dst, cap, &off,
           "\nIF NOTHING IS CHANGED\n"
           "  This fault was ESCAPED: the call chain was abandoned, not "
           "repaired. The faulting instruction is still in .text exactly as "
           "shown above, so whatever called it will call it again and fault "
           "again in the same place. This kernel asks about any one address at "
           "most %u time(s) per boot, so \"no change\" here is a decision to "
           "leave the bug in place, not a decision to postpone it.\n"
           "  \"refuse\" is NOT a useful answer to a fault the machine has "
           "already survived: it arms a deliberate HALT for next time, which is "
           "strictly worse than the escape that just worked.\n"
           "  A \"patch\" is reversible - the original bytes are kept and can be "
           "restored - and it is budgeted, so a wrong one is bounded. If the "
           "faulting instruction should simply not run, filling its whole span "
           "with 0x90 (nop) deletes it; the destination register then keeps "
           "whatever it already held, which the register dump above shows.\n",
           (unsigned)FAULTCHAT_MAX_PER_RIP);
    }

    /* ---- what an answer looks like, restated with this machine's numbers ---- */
    ap(dst, cap, &off,
       "\nANSWER WITH ONE JSON OBJECT\n"
       "  {\"diagnosis\":\"...\"}                       if you do not know what "
       "to do about it\n"
       "  {\"diagnosis\":\"...\",\"fix\":{\"action\":...}}   to change what "
       "happens at the NEXT fault\n"
       "  {\"diagnosis\":\"...\",\"patch\":{\"action\":\"apply\",...}}  to "
       "remove the faulting instruction for good\n"
       "Both are applied without confirmation and both change what this CPU "
       "executes. Omit them unless the report above actually tells you what to "
       "do.\n");

    return off;
}

/* ====================================================================== */
/* the reply                                                              */
/* ====================================================================== */

/* Find one brace-balanced span starting at the first '{' at or after `from`.
 *
 * This is a LEXER, not a parser: it tracks string literals and backslash escapes
 * so that a '{' inside a string cannot open a span and a '"' cannot be escaped
 * out of, and it does nothing else. The span it finds is handed to json_parse(),
 * which is the only thing in this kernel that interprets JSON. It exists because
 * models wrap answers in ```json fences and in "Here is my analysis:" preambles,
 * and refusing a correct diagnosis over its packaging would be a bad trade.
 *
 * Returns 1 and sets the half-open span [*start, *end) on success, 0 otherwise.
 */
static int find_object(const char *t, size_t len, size_t from,
                       size_t *start, size_t *end) {
    size_t i = from;
    while (i < len && t[i] != '{') i++;
    if (i >= len) return 0;

    size_t depth = 0;
    int    instr = 0, esc = 0;
    for (size_t j = i; j < len; j++) {
        char c = t[j];
        if (instr) {
            if (esc)            esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"')  instr = 0;
            continue;
        }
        if (c == '"') { instr = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            if (depth == 0) break;        /* cannot happen; never underflow */
            depth--;
            if (depth == 0) { *start = i; *end = j + 1; return 1; }
        }
    }
    return 0;
}

static void why_say(char *why, size_t cap, const char *s) {
    size_t i = 0;
    if (!why || cap == 0) return;
    for (; s && s[i] && i + 1 < cap; i++) why[i] = s[i];
    why[i] = '\0';
}

/* The concatenated text of the model's reply. Static because this module
 * allocates nothing; sized to the response buffer because a reply that is all
 * text is legal. */
static char reply_text[FAULTCHAT_RESP_BYTES];

int faultchat_parse_reply(const char *body, size_t len,
                          faultchat_reply_t *out, char *why, size_t whycap) {
    if (whycap) why_say(why, whycap, "");
    if (!out) return FAULTCHAT_EINVAL;

    out->diagnosis[0] = '\0';
    out->has_fix      = 0;
    out->fix[0]       = '\0';
    out->fix_len      = 0;
    out->has_patch    = 0;
    out->patch[0]     = '\0';
    out->patch_len    = 0;

    if (!body || len == 0) {
        why_say(why, whycap, "the model returned an empty body");
        return FAULTCHAT_EPROTO;
    }

    json_value_t root;
    if (json_parse(body, len, &root) != JSON_OK) {
        why_say(why, whycap, "the response body was not valid JSON");
        return FAULTCHAT_EPROTO;
    }

    size_t tlen = 0;
    int    rc   = json_msg_text(&root, reply_text, sizeof reply_text, &tlen);
    if (rc != JSON_OK && rc != JSON_ENOSPC) {
        why_say(why, whycap,
                "the response carried no text content block to read");
        return FAULTCHAT_EPROTO;
    }
    if (tlen == 0) {
        why_say(why, whycap, "the model's reply was empty");
        return FAULTCHAT_EPROTO;
    }

    /* Fast path: the whole trimmed text is the object, which is what the system
     * prompt asks for. Fallback: the first brace-balanced span in it. */
    size_t b = 0, e = tlen;
    while (b < e && (reply_text[b] == ' ' || reply_text[b] == '\n' ||
                     reply_text[b] == '\r' || reply_text[b] == '\t')) b++;
    while (e > b && (reply_text[e - 1] == ' ' || reply_text[e - 1] == '\n' ||
                     reply_text[e - 1] == '\r' || reply_text[e - 1] == '\t')) e--;

    json_value_t obj;
    if (b >= e || json_parse(reply_text + b, e - b, &obj) != JSON_OK ||
        obj.type != JSON_OBJECT) {
        /* Try each '{' in turn, not just the first. A brace inside the prose
         * around the answer ("the {} form", a half-quoted sentence) would
         * otherwise shadow the real object, and json_parse is the arbiter of
         * which candidate is real. Bounded so a pathological reply cannot turn
         * this into a quadratic scan. */
        int    found = 0;
        size_t from  = 0;
        for (int attempt = 0; attempt < 16 && !found; attempt++) {
            size_t s2 = 0, e2 = 0;
            if (!find_object(reply_text, tlen, from, &s2, &e2)) break;
            if (json_parse(reply_text + s2, e2 - s2, &obj) == JSON_OK &&
                obj.type == JSON_OBJECT)
                found = 1;
            else
                from = s2 + 1;
        }
        if (!found) {
            why_say(why, whycap,
                    "the reply contained no JSON object - the model answered in "
                    "prose instead of the contract it was given");
            return FAULTCHAT_EPROTO;
        }
    }

    /* ---- diagnosis: required, a string, sanitised ---- */
    json_value_t dv;
    if (json_get(&obj, "diagnosis", &dv) != JSON_OK) {
        why_say(why, whycap, "the reply object has no \"diagnosis\" field");
        return FAULTCHAT_EPROTO;
    }
    if (dv.type != JSON_STRING) {
        why_say(why, whycap, "\"diagnosis\" is not a string");
        return FAULTCHAT_EPROTO;
    }
    {
        /* Decode through json.h (so \n and \" are real characters), then defang.
         * Decoding into the same fixed buffer the caller keeps means an
         * over-long diagnosis is clipped, never refused: the fix beside it may
         * still be the useful half of the answer. */
        static char raw[FAULTCHAT_DIAG_MAX * 2];
        size_t      n = 0;
        int         r = json_str(&dv, raw, sizeof raw, &n);
        if (r != JSON_OK && r != JSON_ENOSPC) {
            why_say(why, whycap, "\"diagnosis\" could not be decoded");
            return FAULTCHAT_EPROTO;
        }
        sanitize(out->diagnosis, sizeof out->diagnosis, raw, n);
    }

    /* ---- fix: optional, must be an object, copied out verbatim ----
     * Not interpreted here. The fault_recover tool is the one place that turns
     * these fields into a fault_plan_t, and it refuses unknown keys, so adding a
     * second reader would be adding a second grammar. */
    json_value_t fv;
    int frc = json_get(&obj, "fix", &fv);
    if (frc == JSON_OK && fv.type != JSON_NULL) {
        if (fv.type != JSON_OBJECT) {
            why_say(why, whycap,
                    "\"fix\" is present but is not a JSON object");
            return FAULTCHAT_EPROTO;
        }
        if (fv.len == 0 || fv.len >= sizeof out->fix) {
            why_say(why, whycap,
                    "\"fix\" is larger than any legitimate recovery plan");
            return FAULTCHAT_EPROTO;
        }
        for (size_t i = 0; i < fv.len; i++) out->fix[i] = fv.start[i];
        out->fix[fv.len] = '\0';
        out->fix_len     = fv.len;
        out->has_fix     = 1;
    }

    /* ---- patch: optional, must be an object, copied out verbatim ----
     * Identical treatment to "fix" one field up, and for the identical reason:
     * tools/fault_tools.c's fault_patch is the one place that turns these fields
     * into bytes in .text, and a second reader would be a second grammar. The
     * only difference is the size bound, because a patch carries hex. */
    json_value_t pv;
    int prc = json_get(&obj, "patch", &pv);
    if (prc == JSON_OK && pv.type != JSON_NULL) {
        if (pv.type != JSON_OBJECT) {
            why_say(why, whycap,
                    "\"patch\" is present but is not a JSON object");
            return FAULTCHAT_EPROTO;
        }
        if (pv.len == 0 || pv.len >= sizeof out->patch) {
            why_say(why, whycap,
                    "\"patch\" is larger than any legitimate code patch");
            return FAULTCHAT_EPROTO;
        }
        for (size_t i = 0; i < pv.len; i++) out->patch[i] = pv.start[i];
        out->patch[pv.len] = '\0';
        out->patch_len     = pv.len;
        out->has_patch     = 1;
    }

    if (out->diagnosis[0] == '\0' && !out->has_fix && !out->has_patch) {
        why_say(why, whycap,
                "the reply parsed but said nothing: no diagnosis text, no fix "
                "and no patch");
        return FAULTCHAT_EPROTO;
    }
    return FAULTCHAT_OK;
}

/* ====================================================================== */
/* applying a proposed fix — through the one door                          */
/* ====================================================================== */

/* The fault_recover tool's answer. Static, like everything else here. Only the
 * first sentence is ever quoted back to the operator, and refusals put their
 * reason first, so this does not need to hold the whole usage text. */
static char fix_result[1024];

int faultchat_apply_fix(const faultchat_reply_t *reply, char *why, size_t whycap) {
    if (whycap) why_say(why, whycap, "");
    if (!reply) return FAULTCHAT_EINVAL;
    if (!reply->has_fix) {
        why_say(why, whycap, "the model proposed no fix");
        return FAULTCHAT_ENONE;
    }

    tool_result_t r;
    tool_result_init(&r, fix_result, sizeof fix_result);

    tool_call_t c;
    c.id        = "toolu_kernel_faultchat";
    c.name      = "fault_recover";
    c.input     = reply->fix;
    c.input_len = reply->fix_len;

    /* tool_dispatch emits the [fault_recover ...] trace line itself, in C, from
     * the real return value. That line is the operator's proof that a fix was
     * armed — this module never prints one of its own in bracket form. */
    int rc = tool_dispatch(&c, &r);
    if (rc == TOOL_ENOENT) {
        why_say(why, whycap,
                "the fault_recover tool is not registered in this build");
        return FAULTCHAT_EFIX;
    }
    if (rc != TOOL_OK || r.is_error) {
        /* Quote the tool's own words: they are the same sentence the model would
         * have been shown, so the console and the model agree about why. */
        char raw[FAULTCHAT_WHY_MAX], one[FAULTCHAT_WHY_MAX];
        size_t n = 0;
        while (n + 1 < sizeof raw && n < r.len &&
               fix_result[n] != '\n') { raw[n] = fix_result[n]; n++; }
        raw[n] = '\0';
        /* ...but sanitise it first, because it is not all kernel-authored.
         * recover_fail() in tools/fault_tools.c echoes the model's own words
         * back ("unknown field \"...\"", the action word, the register name),
         * decoded through json_str(), so up to FAULT_WORD_MAX-1 bytes of the
         * model's choosing survive - brackets and control characters included.
         * This string is printed inside a kernel-prefixed "[fault-diagnose] ..."
         * line, so an un-defanged '[' is a forged kernel statement mid-line and
         * a raw CR is a forged one at column zero on any terminal that honours
         * it. One line above, lib/trace.c escapes the identical bytes; the two
         * voices must not disagree about the rule in the same output. */
        sanitize(one, sizeof one, raw, n);
        why_say(why, whycap, one[0] ? one : "the fault_recover tool refused it");
        return FAULTCHAT_EFIX;
    }
    why_say(why, whycap, "armed");
    return FAULTCHAT_OK;
}

/* ---- and the same door for a code patch ---- */

/* Declared here rather than in the live-half block below because
 * faultchat_apply_patch() is one of the pure-ish functions and must not depend
 * on where the counters happen to be defined. */
static uint32_t g_patches;                  /* code patches actually applied */

int faultchat_apply_patch(const faultchat_reply_t *reply,
                          char *why, size_t whycap) {
    if (whycap) why_say(why, whycap, "");
    if (!reply) return FAULTCHAT_EINVAL;
    if (!reply->has_patch) {
        why_say(why, whycap, "the model proposed no code patch");
        return FAULTCHAT_ENONE;
    }

    /* THE BUDGET IS CHECKED HERE, NOT IN THE TOOL, and the distinction matters:
     * the tool cannot tell an operator's deliberate patch from the kernel's own
     * unattended one, and only the second needs a cap. "How much of this kernel
     * has been rewritten with nobody watching" is this module's question. */
    if (g_patches >= FAULTCHAT_MAX_PATCHES) {
        why_say(why, whycap,
                "this boot has already applied its allowance of unattended code "
                "patches; nothing was written");
        return FAULTCHAT_EPATCH;
    }

    tool_result_t r;
    tool_result_init(&r, fix_result, sizeof fix_result);

    tool_call_t c;
    c.id        = "toolu_kernel_faultpatch";
    c.name      = "fault_patch";
    c.input     = reply->patch;
    c.input_len = reply->patch_len;

    int rc = tool_dispatch(&c, &r);
    if (rc == TOOL_ENOENT) {
        why_say(why, whycap,
                "the fault_patch tool is not registered in this build");
        return FAULTCHAT_EPATCH;
    }
    if (rc != TOOL_OK || r.is_error) {
        /* Same sanitising argument as faultchat_apply_fix(): the tool's refusal
         * echoes model-chosen text (a hex string, a note, a field name) and this
         * ends up inside a kernel-prefixed console line. */
        char raw[FAULTCHAT_WHY_MAX], one[FAULTCHAT_WHY_MAX];
        size_t n = 0;
        while (n + 1 < sizeof raw && n < r.len &&
               fix_result[n] != '\n') { raw[n] = fix_result[n]; n++; }
        raw[n] = '\0';
        sanitize(one, sizeof one, raw, n);
        why_say(why, whycap, one[0] ? one : "the fault_patch tool refused it");
        return FAULTCHAT_EPATCH;
    }

    g_patches++;
    why_say(why, whycap, "applied");
    return FAULTCHAT_OK;
}

uint32_t faultchat_patches_applied(void) { return g_patches; }

/* ====================================================================== */
/* error names                                                            */
/* ====================================================================== */

const char *faultchat_strerror(int rc) {
    switch (rc) {
        case FAULTCHAT_OK:         return "a diagnosis completed";
        case FAULTCHAT_ENONE:      return "there was nothing new to diagnose";
        case FAULTCHAT_EOFF:       return "fault diagnosis is switched off or "
                                          "out of budget for this boot";
        case FAULTCHAT_EBUSY:      return "a diagnosis request is already in "
                                          "flight";
        case FAULTCHAT_EINVAL:     return "bad arguments";
        case FAULTCHAT_ENOSPC:     return "the fault does not fit one request";
        case FAULTCHAT_ETRANSPORT: return "the model could not be reached";
        case FAULTCHAT_EPROTO:     return "the reply was not an intelligible "
                                          "diagnosis";
        case FAULTCHAT_EHTTP:      return "the API answered, but not with 200";
        case FAULTCHAT_EFIX:       return "the proposed fix was refused";
        case FAULTCHAT_ERECURSE:   return "a fault occurred while the diagnosis "
                                          "request was in flight";
        case FAULTCHAT_EPATCH:     return "the proposed code patch was refused";
        default:                   return "unknown result";
    }
}

/* ====================================================================== */
/* the live half                                                          */
/* ====================================================================== */

static model_transport_t *g_transport;
static int                g_enabled = 1;

/* The three independent guards described in the header. */
static int         g_busy;              /* a request is in flight right now   */
static int         g_off;               /* latched off for the rest of boot   */
static const char *g_off_why = "";

static uint32_t g_seen;                 /* fault_count() as of the last pump  */
static uint32_t g_diagnoses;            /* requests actually sent             */
static uint32_t g_fixes;                /* proposed fixes that were armed     */

/* PER-ADDRESS ACCOUNTING — the bound that stops an autonomous loop spiralling.
 *
 * A fixed, tiny table rather than a counter, because "we have tried this address
 * twice" and "we have tried two addresses once each" are different situations
 * and only the first one is a machine failing to make progress. Four slots is
 * more than FAULTCHAT_MAX_DIAGNOSES can fill, so the table cannot be a source of
 * false negatives; when it does overflow the oldest slot is reused, which errs
 * towards asking again rather than towards silently giving up. */
#define FAULTCHAT_RIP_SLOTS 4
static struct { uint64_t rip; uint32_t tries; } g_rip[FAULTCHAT_RIP_SLOTS];
static uint32_t g_rip_next;

static uint32_t rip_tries(uint64_t rip) {
    for (unsigned i = 0; i < FAULTCHAT_RIP_SLOTS; i++)
        if (g_rip[i].tries && g_rip[i].rip == rip) return g_rip[i].tries;
    return 0;
}

static uint32_t rip_bump(uint64_t rip) {
    for (unsigned i = 0; i < FAULTCHAT_RIP_SLOTS; i++)
        if (g_rip[i].tries && g_rip[i].rip == rip) return ++g_rip[i].tries;
    unsigned slot = g_rip_next % FAULTCHAT_RIP_SLOTS;
    g_rip_next++;
    g_rip[slot].rip   = rip;
    g_rip[slot].tries = 1;
    return 1;
}

uint32_t faultchat_rip_attempts(uint64_t rip) { return rip_tries(rip); }

uint64_t faultchat_worst_rip(void) {
    uint64_t worst = 0;
    uint32_t best  = 0;
    for (unsigned i = 0; i < FAULTCHAT_RIP_SLOTS; i++)
        if (g_rip[i].tries > best) { best = g_rip[i].tries; worst = g_rip[i].rip; }
    return worst;
}
static int      g_last_result = FAULTCHAT_ENONE;
static uint32_t g_last_seq;
static char     g_last_why[FAULTCHAT_WHY_MAX];

static char   g_prompt[FAULTCHAT_PROMPT_MAX];
static size_t g_prompt_len;
static char   g_req[FAULTCHAT_REQ_BYTES];
static char   g_resp[FAULTCHAT_RESP_BYTES];

static faultchat_reply_t g_reply;

void faultchat_bind(model_transport_t *t) {
    g_transport = t;
    faultchat_reset();
}

void faultchat_reset(void) {
    g_busy        = 0;
    g_off         = 0;
    g_off_why     = "";
    g_seen        = fault_count();
    g_diagnoses   = 0;
    g_fixes       = 0;
    g_last_result = FAULTCHAT_ENONE;
    g_last_seq    = 0;
    g_last_why[0] = '\0';
    g_prompt[0]   = '\0';
    g_prompt_len  = 0;
    g_reply.diagnosis[0] = '\0';
    g_reply.has_fix      = 0;
    g_reply.fix[0]       = '\0';
    g_reply.fix_len      = 0;
    g_reply.has_patch    = 0;
    g_reply.patch[0]     = '\0';
    g_reply.patch_len    = 0;
    g_patches     = 0;
    g_rip_next    = 0;
    for (unsigned i = 0; i < FAULTCHAT_RIP_SLOTS; i++) {
        g_rip[i].rip   = 0;
        g_rip[i].tries = 0;
    }
    g_enabled     = 1;
}

void faultchat_enable(int on) { g_enabled = on ? 1 : 0; }

static void latch_off(const char *why) {
    g_off     = 1;
    g_off_why = why;
}

int faultchat_pending(void) {
    if (!g_enabled || g_off || g_busy) return 0;
    if (g_diagnoses >= FAULTCHAT_MAX_DIAGNOSES) return 0;
    return fault_count() != g_seen;
}

static int finish(int rc, const char *why) {
    g_last_result = rc;
    why_say(g_last_why, sizeof g_last_why, why ? why : faultchat_strerror(rc));
    return rc;
}

int faultchat_pump(void) {
    /* Guard 1: re-entrancy. Nothing reachable from here calls back into the
     * pump today, and this is what guarantees that stays true. */
    if (g_busy) return FAULTCHAT_EBUSY;

    if (!g_enabled) return FAULTCHAT_EOFF;
    if (g_off)      return FAULTCHAT_EOFF;

    uint32_t now = fault_count();
    if (now == g_seen) return FAULTCHAT_ENONE;

    const fault_record_t *rec = fault_last();
    if (!rec) { g_seen = now; return FAULTCHAT_ENONE; }

    /* Claim every fault seen so far, before anything can fail. A diagnosis that
     * goes wrong must not be retried on the next pump: the machine would spend
     * its whole life re-asking about the same crash. */
    g_seen     = now;
    g_last_seq = rec->seq;

    /* NOTE ON FORMAT STRINGS, because it has cost more than one person a day:
     * kprintf() below is lib/base.c's formatter and has NO 'l' length modifier —
     * "%lu" prints the literal text and does not consume its argument, shifting
     * every later one. The ap() calls further up go through vsnprintf (the
     * lib/libc_shim.c one, freestanding) which DOES support it. Two formatters,
     * one name-shaped difference, and the failure is silent. Everything reaching
     * kprintf here is %s / %d / %u / %p only, and 64-bit values go through %p. */
    if (g_diagnoses >= FAULTCHAT_MAX_DIAGNOSES) {
        latch_off("the per-boot diagnosis budget is spent");
        kprintf("[fault-diagnose] fault #%u survived, but this boot has "
                "already spent its %u diagnoses; not asking again\n",
                (unsigned)rec->seq, (unsigned)FAULTCHAT_MAX_DIAGNOSES);
        return finish(FAULTCHAT_EOFF, "diagnosis budget spent");
    }

    /* HAVE WE ALREADY TRIED THIS EXACT ADDRESS ENOUGH TIMES?
     *
     * The per-boot diagnosis budget above bounds how much of a boot is spent
     * talking about crashes; this bounds how much of it is spent talking about
     * the SAME crash. Without it, one instruction that keeps faulting after
     * every attempted repair eats every remaining diagnosis - and each one is a
     * paid API call - while the machine learns nothing new. Note that this is
     * not a latch: a fault at a DIFFERENT address is still diagnosed, because
     * giving up on one bug is not the same as giving up on the machine. */
    if (rip_tries(rec->regs.rip) >= FAULTCHAT_MAX_PER_RIP) {
        kprintf("[fault-diagnose] fault #%u is at %p again, which this boot has "
                "already asked about %u time(s) without fixing it. Not asking a "
                "third; a diagnosis that has not worked twice will not work by "
                "repetition, and each one costs a round trip. Faults elsewhere "
                "will still be diagnosed.\n",
                (unsigned)rec->seq, (void *)(uintptr_t)rec->regs.rip,
                (unsigned)FAULTCHAT_MAX_PER_RIP);
        return finish(FAULTCHAT_EOFF, "this address has had its attempts");
    }

    /* Budget is spent HERE, on the attempt, not on the send. A machine with no
     * transport and a fault storm would otherwise announce every one of them
     * forever, and "how much of this boot is spent talking about crashes" is
     * the quantity the cap is actually about. */
    g_diagnoses++;
    rip_bump(rec->regs.rip);

    kprintf("[fault-diagnose] fault #%u (%s, vector %u, at %p) was "
            "survived; asking the model what it was\n",
            (unsigned)rec->seq, fault_vector_name(rec->regs.vector),
            (unsigned)rec->regs.vector,
            (void *)(uintptr_t)rec->regs.rip);

    if (!g_transport) {
        kputs("[fault-diagnose] no transport is bound, so nobody can be asked. "
              "The report above is the whole record.\n");
        return finish(FAULTCHAT_ETRANSPORT, "no transport bound");
    }

    /* ---- build the prompt. Static buffers only: if the fault was inside the
     * allocator, this still works. ---- */
    g_prompt_len = faultchat_build_prompt(rec, fault_window(),
                                          g_prompt, sizeof g_prompt);
    if (g_prompt_len >= sizeof g_prompt) {
        /* The report is bounded by FAULT_REPORT_MAX and the rest is fixed text,
         * so this cannot happen with the current constants — which is exactly
         * why it must be checked rather than assumed. */
        kprintf("[fault-diagnose] the machine state does not fit %u bytes; "
                "not sending a clipped crash report\n",
                (unsigned)sizeof g_prompt);
        return finish(FAULTCHAT_ENOSPC, "prompt over budget");
    }

    model_msg_t msg;
    msg.role        = "user";
    msg.content     = g_prompt;      /* escaped by model_build: raw = 0 */
    msg.content_len = 0;
    msg.raw         = 0;

    model_request_t rq;
    rq.model         = (const char *)0;       /* model.c's compiled default */
    rq.max_tokens    = 1024;
    rq.system        = SYSTEM_PROMPT;
    rq.tools         = (const char *)0;       /* no tool loop: see the header */
    rq.tools_len     = 0;
    rq.messages      = &msg;
    rq.message_count = 1;

    int n = model_build(g_req, sizeof g_req, &rq);
    if (n < 0) {
        kprintf("[fault-diagnose] could not build the request (%s)\n",
                model_strerror(n));
        return finish(n == MODEL_ENOSPC ? FAULTCHAT_ENOSPC : FAULTCHAT_EINVAL,
                      "request could not be built");
    }

    kprintf("[fault-diagnose] sending %u bytes of machine state over \"%s\"\n",
            (unsigned)n, g_transport->name ? g_transport->name : "?");

    /* ---- the exchange. Guards 2 and 3 bracket exactly this. ---- */
    uint32_t before = fault_count();
    g_busy = 1;

    model_response_t resp;
    int rc = model_send(g_transport, g_req, (size_t)n,
                        g_resp, sizeof g_resp, &resp);

    g_busy = 0;

    /* Guard 2: did the machine fault while we were talking? If so the transport
     * was interrupted mid-state-machine and its answer means nothing, even if it
     * parses. Guard 3: never ask again this boot. */
    if (fault_count() != before) {
        latch_off("a fault occurred while a diagnosis request was in flight");
        kprintf("\n[fault-diagnose] A FAULT OCCURRED WHILE THE DIAGNOSIS "
                "REQUEST WAS IN FLIGHT (%u -> %u faults).\n"
                "[fault-diagnose] Asking about a crash is what crashed the "
                "machine, so the reply is discarded, no fix is armed, and "
                "diagnosis is switched off for the rest of this boot. The "
                "network stack is not reentrant and will not be entered again.\n",
                (unsigned)before, (unsigned)fault_count());
        return finish(FAULTCHAT_ERECURSE, "faulted during the request");
    }

    if (rc != MODEL_OK) {
        const char *w = model_last_error(g_transport);
        kprintf("[fault-diagnose] the model could not be reached: %s\n",
                (w && w[0]) ? w : model_strerror(rc));
        return finish(FAULTCHAT_ETRANSPORT, "the model could not be reached");
    }
    if (resp.http_status != 200) {
        kprintf("[fault-diagnose] the model answered %s; the crash report above "
                "is the whole record\n",
                resp.status_line[0] ? resp.status_line : "with an HTTP error");
        return finish(FAULTCHAT_EHTTP, "the API did not answer 200");
    }
    if (resp.truncated) {
        kprintf("[fault-diagnose] the reply exceeded %u bytes and was cut off\n",
                (unsigned)sizeof g_resp);
        return finish(FAULTCHAT_EPROTO, "the reply was truncated");
    }

    /* ---- interpret it ---- */
    char why[FAULTCHAT_WHY_MAX];
    int  prc = faultchat_parse_reply(resp.body, resp.body_len, &g_reply,
                                     why, sizeof why);
    if (prc != FAULTCHAT_OK) {
        kprintf("[fault-diagnose] the reply was not a diagnosis: %s\n", why);
        return finish(FAULTCHAT_EPROTO, why);
    }

    /* The model's voice, prose, no prefix — the same convention net/chat.c uses.
     * Already sanitised, so it cannot start a line that looks like the kernel's. */
    if (g_reply.diagnosis[0]) {
        kputs(g_reply.diagnosis);
        kputc('\n');
    }

    if (!g_reply.has_fix && !g_reply.has_patch) {
        kputs("[fault-diagnose] the model proposed neither a fix nor a code "
              "patch, which is a complete answer. Nothing was changed.\n");
        return finish(FAULTCHAT_OK, "diagnosed, nothing proposed");
    }

    /* THE CODE PATCH GOES FIRST, and the order is a decision.
     *
     * A patch removes the faulting instruction; a recovery plan survives the
     * next fault at it. If both are offered, the patch is the repair and the
     * plan is the safety net for the attempt, so the repair must be in place
     * before anything runs again. Each is reported separately: a machine that
     * rewrote its own .text and a machine that armed a plan are different
     * machines, and the console must not blur them into one "fixed". */
    int patch_rc = FAULTCHAT_ENONE, arc = FAULTCHAT_ENONE;

    if (g_reply.has_patch) {
        kputs("[fault-diagnose] the model proposed a CODE PATCH; handing it to "
              "the fault_patch tool, which validates it exactly as if the model "
              "had called it directly\n");
        patch_rc = faultchat_apply_patch(&g_reply, why, sizeof why);
        if (patch_rc != FAULTCHAT_OK)
            kprintf("[fault-diagnose] the proposed code patch was NOT applied: "
                    "%s\n", why);
        else
            kputs("[fault-diagnose] this kernel's .text has been rewritten. The "
                  "original bytes are kept and the patch can be reverted; "
                  "nothing has verified that the new bytes are CORRECT.\n");
    }

    if (g_reply.has_fix) {
        kputs("[fault-diagnose] the model proposed a recovery fix; handing it "
              "to the fault_recover tool, which validates it exactly as if the "
              "model had called it directly\n");
        arc = faultchat_apply_fix(&g_reply, why, sizeof why);
        if (arc != FAULTCHAT_OK) {
            kprintf("[fault-diagnose] the proposed fix was NOT armed: %s\n",
                    why);
        } else {
            g_fixes++;
            /* SAY WHICH PLAN WAS ARMED, NOT WHAT A PLAN USUALLY DOES.
             *
             * Two of the five actions the fault_recover grammar accepts are not
             * recoveries at all, and a real model picks one of them: asked twice
             * about a live divide-by-zero, it answered {"action":"refuse"} —
             * "let it halt, deliberately", which fault.h defines as the exact
             * OPPOSITE of resuming. The single sentence that used to be printed
             * here said "the next matching exception will be handled by it
             * instead of halting the machine", so the console recorded a
             * recovery at the precise moment the machine had been armed to die,
             * and it recorded it for an action nobody was present to see.
             *
             * A wrong sentence is worse here than anywhere else in the module,
             * because this is the only record of an unattended decision. So the
             * plan is READ BACK from fault_plan_get() — the live plan, not the
             * model's JSON — and described in its own terms. */
            const fault_plan_t *armed = fault_plan_get();
            unsigned act = armed ? (unsigned)armed->action
                                 : (unsigned)FAULT_ACT_NONE;
            if (act == FAULT_ACT_REFUSE)
                kputs("[fault-diagnose] the model armed a DELIBERATE REFUSAL, "
                      "which is not a repair: the next matching fatal exception "
                      "is to be allowed to HALT this machine unless some caller "
                      "has armed a fault guard for it to escape into. The "
                      "faulting instruction is untouched and will fault "
                      "again.\n");
            else if (act == FAULT_ACT_NONE)
                kputs("[fault-diagnose] the model's fix arms no action, so "
                      "nothing about the machine has changed: the next fault "
                      "takes its normal course and the faulting instruction is "
                      "still there.\n");
            else
                kputs("[fault-diagnose] the fix is armed. The next matching "
                      "exception will be handled by it instead of halting the "
                      "machine.\n");
        }
    }

    /* One outcome code, and it reports the WORST thing that happened: a caller
     * that only looks at the return value must not be told OK when half of what
     * was proposed was refused. */
    if (patch_rc != FAULTCHAT_OK && patch_rc != FAULTCHAT_ENONE)
        return finish(FAULTCHAT_EPATCH, "the proposed code patch was refused");
    if (arc != FAULTCHAT_OK && arc != FAULTCHAT_ENONE)
        return finish(FAULTCHAT_EFIX, "the proposed fix was refused");
    return finish(FAULTCHAT_OK,
                  patch_rc == FAULTCHAT_OK
                      ? "diagnosed and this kernel's code was patched"
                      : "diagnosed and a fix was armed");
}

/* ---- introspection ---- */

uint32_t    faultchat_diagnoses(void)       { return g_diagnoses; }
uint32_t    faultchat_fixes_armed(void)     { return g_fixes; }
int         faultchat_last_result(void)     { return g_last_result; }
uint32_t    faultchat_last_seq(void)        { return g_last_seq; }
const char *faultchat_last_diagnosis(void)  { return g_reply.diagnosis; }
const char *faultchat_last_prompt(void)     { return g_prompt; }
size_t      faultchat_last_prompt_len(void) { return g_prompt_len; }
const char *faultchat_last_why(void)        { return g_last_why; }
int         faultchat_disabled(void)        { return g_off; }
const char *faultchat_disabled_reason(void) { return g_off_why; }
