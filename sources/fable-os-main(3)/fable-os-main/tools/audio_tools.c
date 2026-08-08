/* audio_tools.c — the model's syscalls for making a sound.
 *
 * PURPOSE
 *   core/audio.c is the audio service; this file is the three verbs the model
 *   gets over it:
 *
 *     audio_status   what can this machine play, and who is doing it?
 *     audio_tone     one tone.
 *     audio_melody   a short sequence of them.
 *
 *   These are what turn "the model wrote a driver" into "the model can use the
 *   driver it wrote". Without them a successful bring-up is a fact in a log; with
 *   them it is a machine that makes a noise when asked to.
 *
 * THE HONEST-FAILURE RULE, WHICH IS THE WHOLE POINT
 *   On a machine with no registered audio driver, every one of these fails, with
 *   a sentence saying exactly that and — in audio_status — the contract a driver
 *   has to satisfy to fix it. Nothing here can return success and produce
 *   silence. That matters more than usual here: sound is the one kernel action
 *   whose absence a transcript cannot show. A file that was not written can be
 *   read back; a tone that did not play looks exactly like a muted speaker, so
 *   the tool result is the only evidence there is, and it has to be true.
 *
 * WHERE THE PLAY CONTRACT IS PRINTED, AND WHY NOT IN THE SCHEMA
 *   The contract (AUDIO_VM_CONTRACT, defined in include/audio.h) is 1400 bytes,
 *   and a tool description is sent on EVERY request whether or not it is relevant
 *   — the whole registry has a hard byte budget (chat.h's CHAT_TOOLS_BYTES) and it
 *   is nearly full. So the description names the contract and audio_status PRINTS
 *   it, on request, PAGED: 1400 bytes does not fit CHAT_TOOL_RESULT_CAP either
 *   (1024), and printing it unpaged delivered two fifths of a specification
 *   ending mid-word. See put_contract().
 *
 *   Whichever tool ends up INSTALLING a play program belongs with the driver VM's
 *   tools rather than here — it needs the sandbox derivation that lives in
 *   tools/dvm_tools.c — and it must interpolate the same macro, so the words the
 *   model reads and the registers core/audio.c preloads cannot drift apart.
 *
 * NO SINK CAN BE INSTALLED ON THIS BUILD, AND THIS FILE SAYS SO
 *   Nothing in the kernel calls audio_register_vm() or audio_register_native():
 *   there is no install tool, so audio_tone and audio_melody ALWAYS fail on a
 *   default kernel. That is not a state to describe euphemistically. The old
 *   put_absent() told the model to "register a PLAY program as the sink", which is
 *   an instruction for a button that does not exist, and a live model burned most
 *   of a turn obeying it — see put_absent()'s comment for the transcript. The
 *   no-sink text now names the path that DOES produce audio here (do the whole
 *   job in driver_run, synthesising the PCM into the DMA buffer itself) and says
 *   outright that no install tool exists, so no more turns are spent hunting one.
 *
 * UNTRUSTED INPUT
 *   Everything arrives as a raw, non-NUL-terminated JSON span written by a
 *   language model: 20 GHz frequencies, negative durations, a "notes" array with
 *   4000 elements, strings where numbers belong, deep nesting. Numbers are read
 *   with an explicit range through json.h and rejected outside it; the note array
 *   is bounded before it is walked; every duration and amplitude is re-checked by
 *   core/audio.c against the actual sink. Nothing in this file allocates, and the
 *   largest stack object is AUDIO_SEQ_MAX notes (512 bytes).
 *
 * DEPENDENCIES
 *   audio.h (the service), json.h (reading the span), trace.h (the ground-truth
 *   line), tool.h. No hardware: every device access happens inside the sink, on
 *   the far side of core/audio.c.
 *
 * FUTURE EXTENSION POINTS
 *   - A "play this WAV from the filesystem" tool is audio_play_pcm() plus a
 *     header parser; it wants the VFS and a bound on file size, not new hardware.
 *   - An app widget that plays a note on click needs apps/expr.c to be able to
 *     call a tool, which it cannot yet; the service side is already re-entrant
 *     enough for it.
 */

#include "tool.h"
#include "audio.h"
#include "json.h"
#include "trace.h"
#include "kernel.h"

#include <stdarg.h>
#include <stdio.h>       /* vsnprintf, via port/stdio.h when freestanding */

/* Defaults for the tool surface, as opposed to the service's own limits. A
 * quarter of a second is long enough to hear and short enough that a model
 * experimenting does not hold the machine. */
#define AT_DEF_MS        250
#define AT_DEF_WAVE      AUDIO_WAVE_SINE
#define AT_WAVE_NAME_MAX 12

/* ====================================================================== */
/* argument helpers                                                       */
/* ====================================================================== */

/* These mirror tools/dvm_tools.c and tools/dev_tools.c. Duplicated rather than
 * shared because those copies are static to their files, and tool.h's contract
 * is that a tool family is a self-contained .c needing no edit anywhere else. */

static int parse_obj(const tool_call_t *c, json_value_t *obj) {
    const char *p = "{}";
    size_t      n = 2;
    if (c && c->input && c->input_len) { p = c->input; n = c->input_len; }
    if (json_parse(p, n, obj) != JSON_OK) return TOOL_EINVAL;
    if (obj->type != JSON_OBJECT) return TOOL_EINVAL;
    return TOOL_OK;
}

/* 1 = present and valid, 0 = absent or null, -1 = wrong type, -2 = out of range. */
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

static int f_str(const json_value_t *o, const char *key, char *dst, size_t cap) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_STRING) return -1;
    rc = json_str(&v, dst, cap, NULL);
    if (rc == JSON_ENOSPC) return -2;
    if (rc != JSON_OK) return -1;
    return 1;
}

/* Emit the failure result AND its trace line, discarding partial output so the
 * model never reads half an answer followed by an error. */
static int fail(tool_result_t *r, int err, const char *op, const char *args,
                const char *fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    r->is_error  = 1;
    r->len       = 0;
    /* Dropping the partial output drops its truncation flag too: tool_dispatch()
     * appends "[result truncated]" to anything still flagged, which on a short
     * error message would be a false claim. */
    r->truncated = 0;
    if (r->buf && r->cap) r->buf[0] = '\0';
    tool_result_printf(r, "error: %s", msg);
    trace_err(err, op, "%s", args ? args : "");
    return err;
}

/* An audio service code becomes a tool code. tool.h defines three, and they are
 * the ones lib/trace.c renders symbolically, so a trace line reads "-> ENOENT"
 * rather than "-> -5". The precise reason always travels in the trace's argument
 * field and in the result text, so nothing is lost by the narrowing. */
static int tool_code(int audio_rc) {
    switch (audio_rc) {
        case AUDIO_OK:      return TOOL_OK;
        case AUDIO_ENOSINK: return TOOL_ENOENT;
        case AUDIO_ENOSPC:  return TOOL_ENOSPC;
        default:            return TOOL_EINVAL;   /* EINVAL and EIO */
    }
}

/* ====================================================================== */
/* audio_status                                                           */
/* ====================================================================== */

/* The contract is longer than one result, so it is PAGED rather than cut.
 *
 * AUDIO_VM_CONTRACT is ~1400 bytes and chat.h's CHAT_TOOL_RESULT_CAP is 1024, so
 * printing it whole delivered 40% of a document and stopped mid-word. A live
 * model hit exactly that: it saw the text end at "the samples are signed 16-bit
 * little-e", correctly concluded it was missing the part that mattered, and
 * spent four rounds trying to read the rest — one of them inventing a
 * /proc/audio_contract to read it from. Truncating a specification silently is
 * worse than not printing it, because the model cannot tell a document that ends
 * from a document that was cut.
 *
 * So: fixed-size pages, each one announcing which page it is and how many there
 * are, and page splits are moved back to a line boundary so no page ends inside
 * a sentence. `part` beyond the last page says so instead of printing nothing. */
#define CONTRACT_PAGE 700

/* Where the page starting at `from` ends: at most CONTRACT_PAGE bytes later, but
 * pulled back to a line boundary so a page never breaks mid-sentence. */
static size_t page_end(const char *c, size_t n, size_t from) {
    size_t to = from + CONTRACT_PAGE;
    if (to >= n) return n;
    size_t cut = to;
    while (cut > from && c[cut - 1] != '\n') cut--;
    return (cut > from) ? cut : to;      /* a single line longer than a page */
}

static void put_contract(tool_result_t *r, unsigned part) {
    const char *c = AUDIO_VM_CONTRACT;
    size_t      n = strlen(c);

    /* PAGE BOUNDARIES ARE WALKED, NOT MULTIPLIED, and that is load-bearing.
     * Starting page k at k*CONTRACT_PAGE looks equivalent and is not: pulling a
     * page back to a line boundary shortens it, so the next page must begin where
     * the previous one ENDED. Multiplying instead skips the bytes in between —
     * silently, which is the precise failure this function exists to prevent. The
     * first version of it had that bug and dropped 204 bytes out of the middle of
     * the contract; test_tool_status_reports_no_sink reassembles every page and
     * compares against the macro, which is what caught it. */
    unsigned pages = 0;
    size_t   from = 0, mine_from = 0, mine_to = 0;
    int      found = 0;
    while (from < n) {
        size_t to = page_end(c, n, from);
        if (pages == part) { mine_from = from; mine_to = to; found = 1; }
        pages++;
        if (to <= from) break;           /* cannot happen; refuses to spin if it does */
        from = to;
    }
    if (pages == 0) pages = 1;

    if (!found) {
        tool_result_printf(r, "the play contract has %u part(s); part %u does not "
                              "exist.\n", pages, part + 1);
        return;
    }

    tool_result_printf(r, "play contract, part %u of %u", part + 1, pages);
    if (mine_to < n)
        tool_result_printf(r, " (ask again with part=%u for the rest)", part + 2);
    /* THE KERNEL'S vsnprintf HAS NO STAR PRECISION, and the failure is not a
     * silent truncation — it is worse than that. lib/libc_shim.c accepts only
     * DIGITS after '.', so it never consumes the '*': it emits the LITERAL TEXT
     * "%*s" and leaves the argument list misaligned for every conversion after
     * it. This line used to read
     *     tool_result_printf(r, ":\n%.*s\n", (int)(mine_to - mine_from), ...);
     * and it rendered the whole contract body as the four characters "%*s".
     *
     * A LIVE MODEL READ THE CONSEQUENCE OFF THE SCREEN. Asked to write a driver
     * for an unknown card, it called audio_status for the play contract, got a
     * blank body, said so in its own words ("the contract body is rendering
     * blank ... so I can't read the exact install-program requirements"), and
     * then wrote its play program by guessing instead. It guessed a fixed 64 KiB
     * of PCM, so every sound it played afterwards was 341.3 ms long no matter
     * what duration was asked for — measurable in the captured WAV as exactly
     * 16384 stereo frames per note. The contract is the one text a play program
     * cannot be written without, and this one line is what withheld it.
     *
     * Host tests could not catch this: they link the HOST libc, where "%.*s"
     * works perfectly. Only the kernel's own formatter is reduced, so the bug
     * was invisible to `make test-host` by construction. tests/qemu/lint_printf.py
     * is the guard that does see it, because it reads the source rather than
     * running it.
     *
     * page_end() never returns a span longer than CONTRACT_PAGE, so the copy is
     * bounded by construction; the clamp is kept anyway so that raising the page
     * size can never turn this into a stack overflow. */
    char   page[CONTRACT_PAGE + 1];
    size_t len = mine_to - mine_from;
    if (len > CONTRACT_PAGE) len = CONTRACT_PAGE;
    memcpy(page, c + mine_from, len);
    page[len] = '\0';
    tool_result_printf(r, ":\n%s\n", page);
}

/* What to say when this machine cannot make a sound.
 *
 * THIS TEXT USED TO SEND A MODEL DOWN A DEAD END, and the history is kept here
 * because it is the reason the wording is this explicit. It said "then register a
 * PLAY program as the sink" at a time when NOTHING in the kernel called
 * audio_register_vm(). A live model followed the instruction faithfully: it found
 * the card, brought it up correctly, and then spent most of a turn hunting for a
 * registration step that did not exist, re-reading this tool three times and
 * searching the filesystem, before concluding — correctly — that its tools could
 * not do it. It never made a sound, and the reason was one sentence of text.
 *
 * The step now exists (driver_install, in tools/dvm_tools.c), so this text names
 * it, in order, with the one fact that model got wrong underneath: r9 bytes of
 * PCM are ALREADY IN THE BUFFER when a play program runs, so a play program that
 * writes samples itself is doing the kernel's job twice.
 *
 * A tool result is a user interface, and it is free: none of this costs schema
 * bytes on a round that never asks. That is why the teaching lives here and
 * driver_install's own description is four lines. */
static void put_absent(tool_result_t *r) {
    tool_result_printf(r,
        "audio: NO OUTPUT DRIVER IS REGISTERED\n"
        "These tools cannot make a sound until one does. Not a volume problem and "
        "not a missing speaker: the kernel has no sink to hand samples to.\n");
    tool_result_printf(r,
        "TO GET ONE, in order: driver_targets to find the unclaimed audio device; "
        "driver_run to bring it up (reset it, set the sample rate, whatever that "
        "device needs); then driver_install with a SECOND, SHORTER program, and audio_tone "
        "starts working. The kernel fills the buffer with samples before each sound "
        "and re-runs that program to play them, so it must not synthesise anything "
        "itself.\n");
    tool_result_printf(r,
        "That second program has to satisfy the PLAY CONTRACT. Call audio_status "
        "with contract=true to read it, and part=2 for the rest; it is longer than "
        "one result.\n");
    tool_result_printf(r,
        "buffer: %lu bytes of PCM at 0x%lx in a %lu-byte DMA arena; the %lu bytes "
        "after the PCM are the driver's own scratch (r12).\n",
        (unsigned long)audio_buffer_bytes(), (unsigned long)audio_buffer_phys(),
        (unsigned long)audio_region_bytes(),
        (unsigned long)(audio_region_bytes() - audio_buffer_bytes()));
}

static void put_present(tool_result_t *r, const audio_status_t *s) {
    tool_result_printf(r, "audio: sink=\"%s\" kind=%s device=%s\n",
                       s->name, audio_sink_kind_name(s->kind),
                       s->device[0] ? s->device : "(none)");
    tool_result_printf(r,
        "format: %lu Hz, %u channel(s), %u-bit signed little-endian, interleaved\n",
        (unsigned long)s->fmt.rate_hz, (unsigned)s->fmt.channels,
        (unsigned)s->fmt.bits);
    tool_result_printf(r,
        "buffer: %lu bytes of PCM at 0x%lx in a %lu-byte DMA arena (the %lu bytes "
        "after the PCM belong to the driver); longest single sound %lu ms\n",
        (unsigned long)s->buf_bytes, (unsigned long)s->buf_phys,
        (unsigned long)s->region_bytes,
        (unsigned long)(s->region_bytes - s->buf_bytes),
        (unsigned long)s->max_ms);
    tool_result_printf(r, "range: %lu-%lu Hz, amplitude 0-%lu, up to %d notes "
                          "per audio_melody call\n",
                       (unsigned long)AUDIO_HZ_MIN,
                       (unsigned long)((AUDIO_HZ_MAX < s->fmt.rate_hz / 2u)
                                       ? AUDIO_HZ_MAX : s->fmt.rate_hz / 2u - 1u),
                       (unsigned long)AUDIO_AMP_MAX, AUDIO_SEQ_MAX);
    tool_result_printf(r, "sounds played: %lu ok, %lu failed\n",
                       (unsigned long)s->plays, (unsigned long)s->failures);
    if (s->frames_last)
        tool_result_printf(r, "last sound: %lu frames (%lu ms)\n",
                           (unsigned long)s->frames_last,
                           (unsigned long)s->ms_last);

    if (s->kind == AUDIO_SINK_VM) {
        tool_result_printf(r, "play program: %lu instructions",
                           (unsigned long)s->vm_insns);
        if (s->vm_ran)
            tool_result_printf(r, "; last run %s at line %lu, %lu steps, "
                                  "%lu device accesses\n",
                               dvm_status_name(s->vm_status),
                               (unsigned long)s->vm_line,
                               (unsigned long)s->vm_steps,
                               (unsigned long)s->vm_io_ops);
        else
            tool_result_printf(r, "; not run yet\n");
    }
    if (s->last_error[0])
        tool_result_printf(r, "last error: %s\n", s->last_error);
}

static int t_audio_status(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "audio_status", "",
                    "input must be a JSON object");

    int want_contract = 0;
    if (f_bool(&in, "contract", &want_contract) < 0)
        return fail(r, TOOL_EINVAL, "audio_status", "",
                    "\"contract\" must be a boolean");

    /* 1-based for the model (it reads "part 1 of 3"), 0-based internally. A
     * "part" without "contract" is taken as asking for the contract: refusing it
     * on a technicality would be a round spent on nothing. */
    int64_t part = 1;
    int     prc  = f_int(&in, "part", 1, 64, &part);
    if (prc < 0)
        return fail(r, TOOL_EINVAL, "audio_status", "",
                    "\"part\" must be an integer 1-64");
    if (prc == 1) want_contract = 1;

    audio_status_t s;
    audio_status(&s);

    if (!s.present) {
        /* An honest "no" is a successful REPORT, not a failed one: the tool was
         * asked what the machine can do and answered correctly. is_error is for
         * an action that did not happen — and nothing was asked to happen here.
         * audio_tone is where the same condition is an error. */
        /* EITHER the advice OR a contract page, never both: together they are
         * over CHAT_TOOL_RESULT_CAP and the model would get a cut-off mixture of
         * the two. A model that asked for the contract has already been told
         * there is no sink, so one line is enough to repeat it. */
        if (want_contract) {
            tool_result_printf(r, "audio: NO OUTPUT DRIVER IS REGISTERED yet. Write "
                                  "the program below and install it with "
                                  "driver_install.\n");
            put_contract(r, (unsigned)(part - 1));
        } else {
            put_absent(r);
        }
    } else {
        put_present(r, &s);
        if (want_contract) put_contract(r, (unsigned)(part - 1));
    }

    trace_ret(s.present, "audio_status", "sink=%s kind=%s",
              s.present ? s.name : "-", audio_sink_kind_name(s.kind));
    return TOOL_OK;
}

static const tool_t audio_status_tool = {
    .name = "audio_status",
    .description =
        "Report whether this machine can make a sound and who is making it: the "
        "registered sink and its device, the sample format, the PCM buffer, the "
        "frequency and duration limits, and what a model-authored driver's play "
        "program last did. When NOTHING is registered it says so and prints the "
        "PLAY CONTRACT: write that program, install it with driver_install, and "
        "these tools start working.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"contract\":{\"type\":\"boolean\",\"description\":\"print the play contract a registered sink's program must satisfy\"},"
        "\"part\":{\"type\":\"integer\",\"description\":\"which part of the contract to print, 1-based; it is longer than one result. Implies contract\"}"
        "},\"required\":[]}",
    .flags  = 0,
    .invoke = t_audio_status,
};
REGISTER_TOOL(audio_status_tool);

/* ====================================================================== */
/* shared note reading                                                    */
/* ====================================================================== */

/* Read a waveform name from `o`. Returns 0 on success (leaving *w untouched when
 * the key is absent), -1 with `bad` naming the problem otherwise. */
static int want_wave(const json_value_t *o, audio_wave_t *w, const char **bad) {
    char name[AT_WAVE_NAME_MAX];
    int rc = f_str(o, "wave", name, sizeof name);
    if (rc == 0) return 0;
    if (rc == -1) { *bad = "\"wave\" must be a string"; return -1; }
    if (rc == -2 || audio_wave_from_name(name, w) != 0) {
        *bad = "\"wave\" must be \"sine\", \"square\" or \"triangle\"";
        return -1;
    }
    return 0;
}

/* ====================================================================== */
/* audio_tone                                                             */
/* ====================================================================== */

static int t_audio_tone(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "audio_tone", "",
                    "input must be a JSON object");

    /* "Is a sound possible at all" is answered before any argument is read, so a
     * machine with no driver never gets a lecture about frequency ranges. */
    if (!audio_available())
        return fail(r, TOOL_ENOENT, "audio_tone", "no-sink",
                    "%s. Call audio_status for the contract a driver must satisfy",
                    audio_no_sink_reason());

    /* The bounds here are gross ones, and deliberately so: the SINK's real limit
     * depends on its sample rate, which core/audio.c knows and this file must not
     * duplicate. Anything inside these bounds but wrong for the sink comes back
     * from the service with a sentence naming the actual rate. */
    int64_t hz = 0, ms = AT_DEF_MS, amp = AUDIO_AMP_DEFAULT;
    int rc = f_int(&in, "hz", (int64_t)AUDIO_HZ_MIN, (int64_t)AUDIO_HZ_MAX, &hz);
    if (rc == 0)  return fail(r, TOOL_EINVAL, "audio_tone", "",
                              "\"hz\" is required: the frequency in hertz, "
                              "%lu-%lu", (unsigned long)AUDIO_HZ_MIN,
                              (unsigned long)AUDIO_HZ_MAX);
    if (rc == -1) return fail(r, TOOL_EINVAL, "audio_tone", "",
                              "\"hz\" must be an integer number of hertz");
    if (rc == -2) return fail(r, TOOL_EINVAL, "audio_tone", "",
                              "\"hz\" must be an integer between %lu and %lu; "
                              "nothing outside that is audible",
                              (unsigned long)AUDIO_HZ_MIN,
                              (unsigned long)AUDIO_HZ_MAX);

    rc = f_int(&in, "ms", 1, (int64_t)AUDIO_MAX_MS, &ms);
    if (rc == -1) return fail(r, TOOL_EINVAL, "audio_tone", "",
                              "\"ms\" must be an integer number of milliseconds");
    if (rc == -2) return fail(r, TOOL_EINVAL, "audio_tone", "",
                              "\"ms\" must be 1-%lu; one sound cannot be longer "
                              "than the kernel's PCM buffer holds",
                              (unsigned long)AUDIO_MAX_MS);

    rc = f_int(&in, "amplitude", 0, (int64_t)AUDIO_AMP_MAX, &amp);
    if (rc == -1) return fail(r, TOOL_EINVAL, "audio_tone", "",
                              "\"amplitude\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "audio_tone", "",
                              "\"amplitude\" must be 0-%lu (full scale for "
                              "16-bit samples); it is not decibels",
                              (unsigned long)AUDIO_AMP_MAX);

    audio_wave_t wave = AT_DEF_WAVE;
    const char *bad = NULL;
    if (want_wave(&in, &wave, &bad) != 0)
        return fail(r, TOOL_EINVAL, "audio_tone", "", "%s", bad);

    audio_status_t before;
    audio_status(&before);

    int arc = audio_tone_ex((uint32_t)hz, (uint32_t)ms, (uint32_t)amp, wave);
    if (arc != AUDIO_OK) {
        char args[96];
        snprintf(args, sizeof args, "hz=%lu ms=%lu wave=%s sink=%s",
                 (unsigned long)hz, (unsigned long)ms, audio_wave_name(wave),
                 before.name);
        return fail(r, tool_code(arc), "audio_tone", args, "%s",
                    audio_last_error());
    }

    audio_status_t s;
    audio_status(&s);
    tool_result_printf(r,
        "played %lu Hz %s for %lu ms at amplitude %lu: %lu frames through sink "
        "\"%s\"\n", (unsigned long)hz, audio_wave_name(wave), (unsigned long)ms,
        (unsigned long)amp, (unsigned long)s.frames_last, s.name);
    if (s.kind == AUDIO_SINK_VM && s.vm_ran) {
        tool_result_printf(r, "the play program ran %lu steps and made %lu device "
                              "accesses\n",
                           (unsigned long)s.vm_steps, (unsigned long)s.vm_io_ops);
        /* WHAT "played" ABOVE ACTUALLY MEANS, said here because a live model got
         * it wrong in the only way that matters. On an intel-hda machine a play
         * program reached `halt` having touched the device 20 times; this tool
         * returned 9600 frames, the app that called it logged `-> ok`, and the
         * model told the operator it had just heard a 440 Hz note. QEMU's wav
         * backend had captured ZERO frames. Nothing had reached the codec.
         *
         * The kernel cannot close that gap for a VM sink, and pretending
         * otherwise is worse than admitting it. Success here means the program
         * ran to completion and touched the device — that is the entire test.
         * There is no way to ask a device "did you emit sound", the DMA buffer is
         * READ by the engine so memory is unchanged whether it read or not, and
         * the position counter that would settle it is device-specific knowledge
         * this kernel deliberately does not have. The one honest move is to say
         * so, in the result, every time, so the claim the model passes on to a
         * human is calibrated to what was actually established. */
        tool_result_printf(r,
            "NOT PROOF OF SOUND: this says the play program ran and touched the "
            "device, which is all the kernel can check. A program that programs "
            "the wrong registers and waits out the duration looks exactly like "
            "this. If you cannot confirm the device is really streaming (a "
            "position counter that advances, a status bit that changes while it "
            "plays), say the sound is unverified rather than that it played.\n");
    }

    trace_ret((long)s.frames_last, "audio_tone",
              "hz=%lu ms=%lu wave=%s amp=%lu sink=%s",
              (unsigned long)hz, (unsigned long)ms, audio_wave_name(wave),
              (unsigned long)amp, s.name);
    return TOOL_OK;
}

static const tool_t audio_tone_tool = {
    .name = "audio_tone",
    .description =
        "Play one tone through the registered audio sink. hz is the frequency "
        "(20-20000, and below half the sink's sample rate), ms the duration "
        "(1-1000), amplitude 0-32767 (8000 is comfortable), wave one of sine, "
        "square or triangle. The kernel synthesises the samples; the call returns "
        "when the sound is over. With no driver registered this FAILS: it never "
        "reports success for silence. See audio_status.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"hz\":{\"type\":\"integer\",\"description\":\"frequency in hertz, 20-20000\"},"
        "\"ms\":{\"type\":\"integer\",\"description\":\"duration in ms, 1-1000 (default 250)\"},"
        "\"amplitude\":{\"type\":\"integer\",\"description\":\"peak sample, 0-32767 (default 8000)\"},"
        "\"wave\":{\"type\":\"string\",\"description\":\"sine (default), square or triangle\"}"
        "},\"required\":[\"hz\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_audio_tone,
};
REGISTER_TOOL(audio_tone_tool);

/* ====================================================================== */
/* audio_melody                                                           */
/* ====================================================================== */

/* Read one element of "notes" into `nt`, applying the call-level defaults that
 * are already in it. Returns 0, or -1 with a sentence in `why`. */
static int read_note(const json_value_t *el, int index, audio_note_t *nt,
                     char *why, size_t cap) {
    /* A bare number is a frequency at the call's default duration. Models write
     * [440, 494, 523] far more often than the object form, and refusing it would
     * cost a turn to learn nothing. */
    if (el->type == JSON_NUMBER) {
        int64_t hz;
        if (json_int(el, &hz) != JSON_OK || hz < 0 || hz > (int64_t)AUDIO_HZ_MAX) {
            snprintf(why, cap,
                     "note %d must be a whole number of hertz, 0-%lu (0 is a rest)",
                     index + 1, (unsigned long)AUDIO_HZ_MAX);
            return -1;
        }
        nt->hz = (uint32_t)hz;
        return 0;
    }
    if (el->type != JSON_OBJECT) {
        snprintf(why, cap,
                 "note %d must be an object like {\"hz\":440,\"ms\":200} or a "
                 "plain frequency like 440", index + 1);
        return -1;
    }

    int64_t v;
    int rc = f_int(el, "hz", 0, (int64_t)AUDIO_HZ_MAX, &v);
    if (rc == -1) { snprintf(why, cap, "note %d: \"hz\" must be an integer", index + 1); return -1; }
    if (rc == -2) {
        snprintf(why, cap,
                 "note %d: \"hz\" must be %lu-%lu, or 0 for a rest", index + 1,
                 (unsigned long)AUDIO_HZ_MIN, (unsigned long)AUDIO_HZ_MAX);
        return -1;
    }
    if (rc == 1) nt->hz = (uint32_t)v;
    else {
        snprintf(why, cap, "note %d has no \"hz\": give a frequency, or 0 for a "
                           "rest", index + 1);
        return -1;
    }

    rc = f_int(el, "ms", 1, (int64_t)AUDIO_MAX_MS, &v);
    if (rc == -1) { snprintf(why, cap, "note %d: \"ms\" must be an integer", index + 1); return -1; }
    if (rc == -2) {
        snprintf(why, cap, "note %d: \"ms\" must be 1-%lu", index + 1,
                 (unsigned long)AUDIO_MAX_MS);
        return -1;
    }
    if (rc == 1) nt->ms = (uint32_t)v;

    rc = f_int(el, "amplitude", 0, (int64_t)AUDIO_AMP_MAX, &v);
    if (rc == -1) { snprintf(why, cap, "note %d: \"amplitude\" must be an integer", index + 1); return -1; }
    if (rc == -2) {
        snprintf(why, cap, "note %d: \"amplitude\" must be 0-%lu", index + 1,
                 (unsigned long)AUDIO_AMP_MAX);
        return -1;
    }
    if (rc == 1) nt->amplitude = (uint32_t)v;

    audio_wave_t w = (audio_wave_t)nt->wave;
    const char *bad = NULL;
    if (want_wave(el, &w, &bad) != 0) {
        snprintf(why, cap, "note %d: %s", index + 1, bad);
        return -1;
    }
    nt->wave = (uint8_t)w;
    return 0;
}

static int t_audio_melody(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "audio_melody", "",
                    "input must be a JSON object");

    if (!audio_available())
        return fail(r, TOOL_ENOENT, "audio_melody", "no-sink",
                    "%s. Call audio_status for the contract a driver must satisfy",
                    audio_no_sink_reason());

    /* ---- call-level defaults, applied to any note that omits them ---- */
    int64_t d_ms = AT_DEF_MS, d_amp = AUDIO_AMP_DEFAULT;
    int rc = f_int(&in, "ms", 1, (int64_t)AUDIO_MAX_MS, &d_ms);
    if (rc == -1) return fail(r, TOOL_EINVAL, "audio_melody", "",
                              "\"ms\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "audio_melody", "",
                              "\"ms\" must be 1-%lu", (unsigned long)AUDIO_MAX_MS);
    rc = f_int(&in, "amplitude", 0, (int64_t)AUDIO_AMP_MAX, &d_amp);
    if (rc == -1) return fail(r, TOOL_EINVAL, "audio_melody", "",
                              "\"amplitude\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "audio_melody", "",
                              "\"amplitude\" must be 0-%lu",
                              (unsigned long)AUDIO_AMP_MAX);

    audio_wave_t d_wave = AT_DEF_WAVE;
    const char *bad = NULL;
    if (want_wave(&in, &d_wave, &bad) != 0)
        return fail(r, TOOL_EINVAL, "audio_melody", "", "%s", bad);

    /* ---- the notes ---- */
    json_value_t arr;
    int jrc = json_get(&in, "notes", &arr);
    if (jrc == JSON_ENOENT)
        return fail(r, TOOL_EINVAL, "audio_melody", "",
                    "\"notes\" is required: an array of up to %d frequencies, or "
                    "of objects like {\"hz\":440,\"ms\":200}", AUDIO_SEQ_MAX);
    if (jrc != JSON_OK || arr.type != JSON_ARRAY)
        return fail(r, TOOL_EINVAL, "audio_melody", "",
                    "\"notes\" must be an array");

    size_t cnt = json_count(&arr);
    if (cnt == 0)
        return fail(r, TOOL_EINVAL, "audio_melody", "",
                    "\"notes\" is empty: there is nothing to play");
    if (cnt > (size_t)AUDIO_SEQ_MAX)
        return fail(r, TOOL_EINVAL, "audio_melody", "",
                    "%lu notes is more than one call may hold (%d). Split the "
                    "melody across several calls",
                    (unsigned long)cnt, AUDIO_SEQ_MAX);

    audio_note_t notes[AUDIO_SEQ_MAX];
    uint32_t     total_ms = 0;
    char         why[160];

    for (size_t i = 0; i < cnt; i++) {
        json_value_t el;
        if (json_at(&arr, i, &el) != JSON_OK)
            return fail(r, TOOL_EINVAL, "audio_melody", "",
                        "note %lu could not be read", (unsigned long)i + 1);

        notes[i].hz        = 0;
        notes[i].ms        = (uint32_t)d_ms;
        notes[i].amplitude = (uint32_t)d_amp;
        notes[i].wave      = (uint8_t)d_wave;

        if (read_note(&el, (int)i, &notes[i], why, sizeof why) != 0)
            return fail(r, TOOL_EINVAL, "audio_melody", "", "%s", why);
        total_ms += notes[i].ms;
    }

    audio_status_t before;
    audio_status(&before);

    /* audio_play_notes validates every note against the sink before the first
     * one sounds, so a bad note nine in is an error rather than eight notes and
     * then an error. */
    int arc = audio_play_notes(notes, (int)cnt);
    if (arc != AUDIO_OK) {
        char args[96];
        snprintf(args, sizeof args, "notes=%lu ms=%lu sink=%s",
                 (unsigned long)cnt, (unsigned long)total_ms, before.name);
        return fail(r, tool_code(arc), "audio_melody", args, "%s",
                    audio_last_error());
    }

    audio_status_t s;
    audio_status(&s);
    tool_result_printf(r,
        "played %lu note(s), about %lu ms of audio, through sink \"%s\"\n",
        (unsigned long)cnt, (unsigned long)total_ms, s.name);
    tool_result_printf(r, "sounds played: %lu ok, %lu failed since this sink "
                          "registered\n",
                       (unsigned long)s.plays, (unsigned long)s.failures);
    /* A sequence longer than one buffer-full is played in pieces, and the seam
     * is audible. Saying so beats the model concluding the driver is broken. */
    if (s.plays - before.plays > 1)
        tool_result_printf(r,
            "it did not fit in one buffer, so it played as %lu pieces; the joins "
            "are audible\n", (unsigned long)(s.plays - before.plays));

    trace_ret((long)cnt, "audio_melody", "notes=%lu ms=%lu sink=%s",
              (unsigned long)cnt, (unsigned long)total_ms, s.name);
    return TOOL_OK;
}

static const tool_t audio_melody_tool = {
    .name = "audio_melody",
    .description =
        "Play a short sequence of tones through the registered audio sink. "
        "\"notes\" is an array of up to 32 entries; each is either a plain "
        "frequency (440) or an object {\"hz\":440,\"ms\":200,\"amplitude\":8000,"
        "\"wave\":\"sine\"}. An hz of 0 is a rest. Top-level \"ms\", "
        "\"amplitude\" and \"wave\" default every note that omits them, so a scale "
        "is just a list of frequencies. Every note is checked before the first one "
        "sounds. Fails loudly if no driver is registered.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"notes\":{\"type\":\"array\",\"description\":\"up to 32: a frequency, or an object with hz/ms/amplitude/wave. hz 0 is a rest\",\"items\":{}},"
        "\"ms\":{\"type\":\"integer\",\"description\":\"default per-note duration, 1-1000 (default 250)\"},"
        "\"amplitude\":{\"type\":\"integer\",\"description\":\"default peak sample, 0-32767 (default 8000)\"},"
        "\"wave\":{\"type\":\"string\",\"description\":\"default wave: sine, square or triangle\"}"
        "},\"required\":[\"notes\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_audio_melody,
};
REGISTER_TOOL(audio_melody_tool);
