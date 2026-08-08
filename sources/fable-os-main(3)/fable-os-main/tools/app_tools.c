/* app_tools.c — the model builds an application and runs it.
 *
 * PURPOSE
 *   "hey I want a calculator" ends here. gui_open (tools/gui_tools.c) can start
 *   an app that a human compiled into the kernel; this tool lets the model AUTHOR
 *   one — write a document, launch it, look at what it did, fix it, launch it
 *   again — with no C, no compiler and no reboot.
 *
 * ============================================================================
 * ONE TOOL, WITH AN `action` — AND THE REASON IS A SHARED BUDGET, NOT TASTE
 * ============================================================================
 *   The assembled tool schema is a shared resource. chat.h sets
 *   CHAT_TOOLS_BYTES and net/chat.c's tools_load() refuses the WHOLE array if it
 *   does not fit, which offers the model NO TOOLS AT ALL — a total failure, so
 *   the bound is respected rather than tested.
 *
 *   So launch / list / state / close / format are ONE tool with an `action`, the
 *   schema states no enum (the description lists the actions instead), x and y
 *   are accepted but documented in action=format's OUTPUT rather than in the
 *   schema, and the FULL format specification is tool output too, because a
 *   result costs nothing from the schema budget.
 *
 *   HISTORICAL NOTE, because the old number here was quoted for a while after it
 *   stopped being true: this comment used to say CHAT_TOOLS_BYTES was 32768 and
 *   that the file fitted "with FIVE BYTES to spare". CHAT_TOOLS_BYTES is 40960
 *   today. Measured on this tree the whole registry assembles to well under it
 *   with kilobytes free, so the description below could afford to spend a few
 *   hundred bytes buying discoverability. Do not re-derive that headroom from a
 *   comment — tests/host/test_app.c measures the REAL assembled array and prints
 *   both numbers.
 *
 * THE FEEDBACK LOOP IS THE PRODUCT
 *   A model gets a document wrong on the first try — a mistyped key, a widget
 *   name it never declared, an unbalanced bracket in an expression. What makes
 *   the second try work is being told exactly what was wrong, so every rejection
 *   reports the JSON path, the reason, the byte offset inside an expression, and
 *   the names or values that WOULD have been accepted. It also says plainly that
 *   nothing was launched, because a model that believes a half-built window
 *   exists will try to repair it instead of resending it.
 *
 * UNTRUSTED INPUT
 *   `call->input` is model output and is read only through json.h. The document
 *   is handed to app_launch() as a raw span (or decoded out of a JSON string into
 *   one fixed buffer), where it is validated in full before any window exists.
 *   Nothing here indexes memory with a number from the model: ids are looked up
 *   in the instance pool, and coordinates are range-checked here and clamped
 *   again by the WM.
 *
 * EVERY CALL EMITS EXACTLY ONE TRACE LINE
 *   Success and failure both, through trace.h, from real return values after the
 *   call happened — so the transcript shows the id that was really opened and the
 *   path that was really refused, not what the model said it did.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "app.h"
#include "gui.h"
#include "widgets.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>       /* vsnprintf, via port/stdio.h when freestanding */

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand. Export a named pointer instead; tests/host/test_app.c stands in for
 * the linker. Kernel builds are untouched. */
#ifdef FABLEOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const fableos_hosttool_##var = &(var)
#endif

#define COORD_LIMIT  100000

/* Keep this much result budget free so a truncated listing can always say so. */
#define TAIL_RESERVE 120

/* ====================================================================== */
/* helpers (the tools/gui_tools.c + mem_tools.c convention)                */
/* ====================================================================== */

static void argf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

/* `args` is a finished string, never a format: trace_err renders it with "%s" so
 * a value that came from the model can never be read as a conversion. */
static int fail(tool_result_t *r, int err, const char *args,
                const char *fmt, ...) {
    char    msg[420];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    r->is_error  = 1;
    r->len       = 0;
    r->truncated = 0;
    if (r->buf && r->cap) r->buf[0] = '\0';
    tool_result_printf(r, "error: %s", msg);
    trace_err(err, "app", "%s", args ? args : "");
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

/* 1 = present and valid, 0 = absent/null, -1 = wrong type, -2 = out of range. */
static int f_int(const json_value_t *o, const char *key,
                 int64_t lo, int64_t hi, int64_t *out) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_NUMBER) return -1;
    for (size_t i = 0; i < v.len; i++)
        if (v.start[i] == '.' || v.start[i] == 'e' || v.start[i] == 'E') return -1;
    int64_t x = 0;
    if (json_int(&v, &x) != JSON_OK) return -2;
    if (x < lo || x > hi) return -2;
    *out = x;
    return 1;
}

/* -3 additionally means "the decoded string contains an embedded NUL". A JSON
 * string may legally carry one and every comparison below is on a C string. */
static int f_str(const json_value_t *o, const char *key, char *dst, size_t cap) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_STRING) return -1;
    size_t n = 0;
    rc = json_str(&v, dst, cap, &n);
    if (rc == JSON_ENOSPC) return -2;
    if (rc != JSON_OK) return -1;
    for (size_t i = 0; i < n; i++) if (dst[i] == '\0') return -3;
    return 1;
}

static int streq(const char *a, const char *b) {
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && a[i] == b[i]; i++) if (i > 64) return 0;
    return a[i] == '\0' && b[i] == '\0';
}

static void put_running(tool_result_t *r) {
    int n = app_count();
    if (!n) { tool_result_printf(r, "no apps are running"); return; }
    tool_result_printf(r, "running app ids:");
    for (int i = 0; i < n; i++)
        tool_result_printf(r, " %u", (unsigned)app_at(i));
}

static void put_widgets(tool_result_t *r, uint32_t id, const char *indent) {
    char   desc[560];
    size_t want = app_describe(id, desc, sizeof desc);
    tool_result_printf(r, "%s", desc);
    if (want >= sizeof desc)
        tool_result_printf(r, "%s[widget list clipped]\n", indent);
}

static void put_vars(tool_result_t *r, uint32_t id) {
    int nv = app_var_count(id);
    if (!nv) return;
    tool_result_printf(r, "vars:");
    for (int i = 0; i < nv && r->len + TAIL_RESERVE + 40 < r->cap; i++) {
        const char *name = "";
        char        val[APP_TEXT_MAX];
        if (app_var_at(id, i, &name, val, sizeof val) == 0)
            tool_result_printf(r, " %s=%s", name, val);
    }
    tool_result_printf(r, "\n");
}

/* ====================================================================== */
/* action=format — the specification, as OUTPUT                            */
/* ====================================================================== */

/* This is where the format is documented, because a tool result costs nothing
 * from the schema budget (see the header). It is a real, launchable document
 * rather than a grammar, because that is what a model can copy and adapt.
 *
 * IT MUST FIT CHAT_TOOL_RESULT_CAP (1024 bytes). The turn loop clips anything
 * longer, and a specification that stops mid-sentence is worse than a terser one
 * that finishes: the model would be missing exactly the part it did not know was
 * missing. tests/host/test_app.c asserts the length, so an edit here cannot
 * quietly lose the last paragraph. What did not fit is carried by the rejection
 * messages instead, which is where a model reads it anyway — a wrong widget key
 * is answered with the list of accepted keys, an unknown function with the list
 * of functions, and every bound with its number, so none of that has to be spent
 * here. */
/* WHAT A LIVE RUN CHANGED HERE. apps/ grew round(), rand(), at() and the time
 * leaves (now, clock, today, hour, minute, second) and NOTHING on the tool
 * surface said so, which means the model could not have used them: it does not
 * guess function names into a validator that rejects unknown ones. They are
 * named in the list below, and the two idioms worth a sentence (money rounding
 * and 0..n-1) are glossed there rather than in the description, which has to
 * stay readable.
 *
 * WHAT PAID FOR THEM, and the honest cost: "(+rowspan/colspan)" came out. This
 * text was 1005 of the 1023 usable bytes, so 60 bytes of new material had to
 * come from somewhere. Spans are the safest thing to lose, because they are the
 * one omission this file's own error path repairs for free: apps/runtime.c
 * validates widget keys against a list and names every accepted key when one is
 * unknown, so a model that reaches for a span is told it exists, whereas a
 * function name it never sees is a function it never calls. rect:[x,y,w,h] also
 * still covers any layout a span could express.
 *
 * "tick(ms 50+)" cost 16 bytes and was paid for by "round(x) nearest whole" ->
 * "round(x) whole" (8) and by the compact spelling of the event itself. The
 * text is measured, not estimated: tests/host/test_app.c prints its exact size
 * and fails the suite at CHAT_TOOL_RESULT_CAP, so an edit here that overflows
 * takes the last paragraph off the model's copy and is caught immediately.
 *
 * WHAT THE CAPABILITY STATEMENT COST, since this text is a zero-sum budget and
 * the next person to add a line needs to know where the bytes came from. Adding
 * {call,with,into} plus one clause naming what it is for spent about 70 bytes, and
 * the spec was already 1005 of the 1023 the model ever sees. Three things paid:
 *   - "round(x) whole; rand(n) 0..n-1; at(s,i) one char;" (48 bytes) came out. It
 *     was DUPLICATE: the always-sent description above says "rand(n) is 0..n-1,
 *     round(x*100)/100 is money, at(s,i) picks a character" in more words, and the
 *     three names are still in the function list here, so nothing became
 *     undiscoverable. This is the cheapest 48 bytes in the file for exactly that
 *     reason — prefer duplicated bytes over unique ones when trimming.
 *   - "title":"Counter" left the worked document (18 bytes). Also duplicated: the
 *     description's own example carries "title":"Hi", and the key is optional, so
 *     the document still launches — which a test proves by extracting and running
 *     it. It is the only key in that example that is not load-bearing.
 *   - four words of prose ("place by", "is", "gives"), which cost nothing.
 * The full capability specification is NOT here: it is action=caps, a second page,
 * because it needs ~800 bytes and no amount of trimming would have found those.
 * put_caps() explains that split. tests/host/test_app.c measures this text and
 * fails at CHAT_TOOL_RESULT_CAP, and the golden transcript in that suite pins two
 * literal fragments of it ("Launch this document, then adapt it" and "iserr()
 * finds it"), so an edit here is checked in three ways before it ships.
 *
 * "tick" IS NOW DOCUMENTED, and only because the pump exists. It was left out
 * on purpose for as long as nothing called app_tick(): a tick handler was
 * accepted, registered and never run, so advertising it would have made this
 * tool lie about a window that says 0.0 s forever. kernel/main.c's
 * wait_for_sentence() now calls app_tick() before gui_tick(), so the event is
 * real and the line below says so. If that call is ever removed, this word
 * comes out in the same commit - and note that removing it from here is not
 * enough on its own, because apps/runtime.c's own rejection messages enumerate
 * the accepted event keys, so a model that mistypes {"every":1000} is told
 * "tick" exists whatever this text says. */
static void put_format(tool_result_t *r) {
    tool_result_printf(r,
        "Launch this document, then adapt it:\n"
        "{\"width\":180,\"height\":120,"
        "\"grid\":{\"rows\":2,\"cols\":1},\"vars\":{\"n\":0},"
        "\"widgets\":["
        "{\"kind\":\"field\",\"name\":\"out\",\"text\":\"0\","
        "\"readonly\":true,\"row\":0,\"col\":0},"
        "{\"kind\":\"button\",\"text\":\"+1\",\"name\":\"up\",\"row\":1,"
        "\"col\":0}],"
        "\"on\":[{\"click\":\"up\",\"do\":["
        "{\"set\":\"n\",\"to\":\"n + 1\"},"
        "{\"set\":\"out\",\"to\":\"text(n)\"}]}]}\n");
    tool_result_printf(r,
        "kind: button label field panel; row/col or rect:[x,y,w,h]. "
        "name is unique and how a handler reaches it; tag is shared: "
        "one handler serves ten keys. Events: click, submit, tick(ms 50+). "
        "Statements: "
        "{set,to} {if,then,else} {stop:true} {call,with,into}; no loops. "
        "{call} asks the KERNEL for a sound: app action=caps.\n"
        "EXPRESSIONS are strings: numbers (6dp), 'text', vars, a widget name "
        "(its text), key (the firer's), + - * / %% == != < <= > >= && || !, "
        "num text cat len digits has iserr abs min max round rand at. num(x) "
        "reads a widget's number, text(x) writes back; now clock today hour "
        "minute second are the time. 1/0 or overflow is an error: text() is "
        "\"error\", iserr() finds it.\n");
}

/* ====================================================================== */
/* action=caps — what a handler may ask the kernel to do                   */
/* ====================================================================== */

/* A SECOND PAGE RATHER THAN MORE OF action=format, for the reason the header
 * gives: a tool RESULT is free and the schema is not. put_format() was already
 * 1005 of the 1023 usable bytes, so the capability specification could not go
 * there; what went into that text instead is the one line that says this page
 * exists and shows the statement, because a model does not call for a page it has
 * never heard of.
 *
 * THE ARGUMENT TABLE IS GENERATED, by app_cap_spec(), out of the same table
 * apps/cap.c validates against. The bounds a model reads and the bounds that are
 * enforced are therefore the same numbers, and a capability that gains an
 * argument documents itself. Only the prose and the example below are written by
 * hand — and the example is a document that provably launches (a host test runs
 * it), because that is what a model copies.
 *
 * IT IS AT 994 OF THE 1023 BYTES THE MODEL SEES, with one capability in the
 * table. So the NEXT capability does not fit, and the answer is not to trim this
 * page again: it is to page it (action=caps with a name, or one line per
 * capability and the arguments on request). tests/host/test_app_audio.c measures
 * the real result through tool_dispatch() and fails at CHAT_TOOL_RESULT_CAP, so
 * whoever adds fs.write or clock.alarm will be told this by a red test rather
 * than by a model that read half a specification. */
static void put_caps(tool_result_t *r) {
    char spec[640];

    tool_result_printf(r,
        "A handler can ask the KERNEL for hardware. This launches and beeps:\n"
        "{\"grid\":{\"rows\":2,\"cols\":1},"
        "\"widgets\":[{\"kind\":\"label\",\"name\":\"s\",\"text\":\"-\","
        "\"row\":0,\"col\":0},"
        "{\"kind\":\"button\",\"text\":\"beep\",\"name\":\"b\",\"row\":1,"
        "\"col\":0}],"
        "\"on\":[{\"click\":\"b\",\"do\":[{\"call\":\"audio.tone\","
        "\"with\":{\"hz\":\"440\"},\"into\":\"s\"}]}]}\n");
    tool_result_printf(r,
        "Each \"with\" value is an expression string like a \"to\": \"440\" or "
        "\"num(key)\" (the button's text). Optional \"into\" is a var or widget "
        "that gets \"ok\" when it really played, or why not.\n");
    app_cap_spec(spec, sizeof spec);
    tool_result_printf(r, "%s", spec);
}

/* ====================================================================== */
/* the retry skeleton — what makes ATTEMPT TWO land                        */
/* ====================================================================== */

/* THE TWO-STEP DANCE IS A TRAP, SO IT IS NOT REQUIRED.
 *   action=format used to be the only place the format existed, which meant a
 *   model had to know it needed the spec BEFORE it could want it. Under any
 *   pressure it skips that call, guesses, fails, and then has one error line to
 *   guess again from. So every rejection carries a document that provably
 *   launches, plus the handful of rules a first guess actually gets wrong
 *   (which kinds exist, that placement is mandatory, that a handler may only
 *   name widgets or vars that were declared).
 *
 *   IT IS DELIBERATELY NOT put_format(). That text is 1005 bytes and
 *   CHAT_TOOL_RESULT_CAP is 1024, so appending it to an error would clip the
 *   error, the spec, or both — and a spec that stops mid-sentence is worse than
 *   a terse one that finishes (see put_format). This is the compact form, sized
 *   so that even a maximum-length app_error_t (72-byte path + 224-byte msg)
 *   leaves it intact; tests/host/test_app.c pins that worst case. */
static void put_retry(tool_result_t *r) {
    tool_result_printf(r,
        "This document DOES launch - copy it and adapt:\n"
        "{\"title\":\"Hi\",\"width\":200,\"height\":90,"
        "\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":\"label\","
        "\"text\":\"hello\",\"row\":0,\"col\":0}]}\n");
    tool_result_printf(r,
        "Rules a first guess usually misses: kind is label, button, field or "
        "panel; every widget needs row+col (or rect:[x,y,w,h]); each name is "
        "unique; a handler in \"on\" may only name a widget or a var you "
        "declared. Fix the one thing above and call app action=launch again "
        "with the whole corrected document. Full spec: app action=format.\n");
}

/* ====================================================================== */
/* action=launch                                                          */
/* ====================================================================== */

/* A document sent as a JSON *string* is decoded into this. One launch at a time
 * (one thread, and an app cannot launch an app), and 8 KiB is too much for the
 * 64 KiB kernel stack to spend on a maybe. */
static char doc_buf[APP_DOC_MAX + 1];

static int do_launch(const json_value_t *in, tool_result_t *r) {
    json_value_t doc;

    int64_t x = 0, y = 0;
    int has_x = f_int(in, "x", -COORD_LIMIT, COORD_LIMIT, &x);
    int has_y = f_int(in, "y", -COORD_LIMIT, COORD_LIMIT, &y);
    if (has_x < 0 || has_y < 0)
        return fail(r, TOOL_EINVAL, "action=launch",
                    "\"x\" and \"y\" must be whole numbers of pixels within "
                    "+/-%d when present", COORD_LIMIT);

    if (json_get(in, "document", &doc) != JSON_OK) {
        int e = fail(r, TOOL_EINVAL, "action=launch",
                     "\"document\" is required: the app document itself, as a "
                     "JSON object");
        put_retry(r);
        return e;
    }

    const char *text = (const char *)0;
    size_t      len  = 0;

    if (doc.type == JSON_OBJECT) {
        /* The preferred case: a nested object is already a bounded span of the
         * request, so it needs no copy and no decoding at all. */
        text = doc.start;
        len  = doc.len;
    } else if (doc.type == JSON_STRING) {
        /* Also accepted: the whole document escaped inside one JSON string. It
         * costs a decode and it is easy to get the escaping wrong, but refusing
         * it outright would cost a turn to explain. */
        size_t n = 0;
        int    rc = json_str(&doc, doc_buf, sizeof doc_buf, &n);
        if (rc == JSON_ENOSPC)
            return fail(r, TOOL_ENOSPC, "action=launch",
                        "the document is longer than %d bytes", APP_DOC_MAX);
        if (rc != JSON_OK)
            return fail(r, TOOL_EINVAL, "action=launch",
                        "\"document\" is a string but could not be decoded");
        text = doc_buf;
        len  = n;
    } else {
        int e = fail(r, TOOL_EINVAL, "action=launch",
                     "\"document\" must be a JSON object (preferred), or a string "
                     "containing the document's JSON - not a number, an array or "
                     "a boolean");
        put_retry(r);
        return e;
    }

    uint32_t    id = 0;
    app_error_t e;
    int rc = app_launch(text, len,
                        has_x == 1 ? (int32_t)x : GUI_POS_AUTO,
                        has_y == 1 ? (int32_t)y : GUI_POS_AUTO,
                        &id, &e);
    if (rc != APP_OK) {
        char args[96];
        argf(args, sizeof args, "action=launch rejected at %s",
             e.path[0] ? e.path : "the document");
        r->is_error  = 1;
        r->len       = 0;
        r->truncated = 0;
        if (r->buf && r->cap) r->buf[0] = '\0';
        tool_result_printf(r, "error: the app was NOT launched - nothing was "
                              "created and there is nothing on screen to "
                              "repair.\n");
        if (e.path[0]) tool_result_printf(r, "where: %s\n", e.path);
        tool_result_printf(r, "problem: %s\n", e.msg);
        put_retry(r);
        trace_err(rc, "app", "%s", args);
        return rc == APP_ENOSPC ? TOOL_ENOSPC : TOOL_EINVAL;
    }

    gui_sync();                 /* on screen now, not on the next input poll */

    gui_window_t *w = gui_window(id);
    tool_result_printf(r,
        "launched \"%s\" as app id=%u (also its window id) at %d,%d %dx%d with "
        "%d widget(s), focused and on top. It is live: a real mouse can click it, "
        "and gui_click id=%u label=\"<text>\" presses a widget.\n",
        app_title(id), (unsigned)id, (int)w->frame.x, (int)w->frame.y,
        (int)w->frame.w, (int)w->frame.h, (int)w->nwidgets, (unsigned)id);
    put_vars(r, id);
    put_widgets(r, id, "");

    /* GEOMETRY BELONGS IN THE TRACE LINE, NOT ONLY IN THE MODEL'S RESULT.
     * This is the one tool that can put a 1024x768 window at 0,0 - which covers
     * the console, the banner, the prompt and every line above it - and its
     * ground truth used to read exactly like a 200x90 hello window. gui_open,
     * which can only open two fixed-size demos, has printed at=/size= all along.
     * The values are free: `w` is dereferenced three lines up. Appended AFTER
     * the existing fields on purpose - tests/host/test_app.c pins the prefix. */
    trace_ret((long)id, "app", "action=launch title=%s widgets=%d vars=%d "
                               "bytes=%d at=%d,%d size=%dx%d",
              app_title(id), (int)w->nwidgets, app_var_count(id), (int)len,
              (int)w->frame.x, (int)w->frame.y,
              (int)w->frame.w, (int)w->frame.h);
    return TOOL_OK;
}

/* ====================================================================== */
/* the tool                                                               */
/* ====================================================================== */

static int t_app(const tool_call_t *c, tool_result_t *r) {
    json_value_t in;
    char         act[16];

    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "",
                    "input must be a JSON object, e.g. {\"action\":\"format\"} "
                    "or {\"action\":\"launch\",\"document\":{...}}");

    int arc = f_str(&in, "action", act, sizeof act);
    if (arc <= 0)
        return fail(r, TOOL_EINVAL, "",
                    "\"action\" is required: format, caps, launch, list, state "
                    "or close");

    /* format needs no window manager: a model may read the spec on a machine
     * that cannot draw, and then be told why launching is impossible. */
    if (streq(act, "format")) {
        put_format(r);
        trace_ok("app", "%s", "action=format");
        return TOOL_OK;
    }

    /* Like format, this needs no window manager: what a handler may ask the
     * kernel for is worth knowing on a machine that cannot draw. */
    if (streq(act, "caps")) {
        put_caps(r);
        trace_ok("app", "%s", "action=caps");
        return TOOL_OK;
    }

    if (!gui_attached())
        return fail(r, TOOL_EINVAL, "",
                    "this machine has no window manager: there is no linear "
                    "framebuffer (the console is VGA text mode), so an app "
                    "cannot be drawn. The text console tools still work");

    if (streq(act, "launch")) return do_launch(&in, r);

    int64_t id64 = 0;
    int has_id = f_int(&in, "id", 1, 0x7FFFFFFF, &id64);
    if (has_id < 0)
        return fail(r, TOOL_EINVAL, "",
                    "\"id\" must be the integer id that launching returned");

    char args[64];
    argf(args, sizeof args, "action=%s id=%u", act, (unsigned)id64);

    if (streq(act, "list")) {
        int n = app_count();
        tool_result_printf(r,
            "%d of %d app slots in use. These are apps launched from a document; "
            "gui_list shows every window, including built-in ones.\n",
            n, APP_MAX_INSTANCES);
        for (int i = 0; i < n && r->len + TAIL_RESERVE + 80 < r->cap; i++) {
            uint32_t wid = app_at(i);
            tool_result_printf(r, "  id=%u \"%s\"%s%s\n", (unsigned)wid,
                               app_title(wid),
                               gui_window(wid) ? "" : " (window gone)",
                               gui_focused() == wid ? " focused" : "");
        }
        const app_stats_t *s = app_stats();
        tool_result_printf(r, "totals: %u launched, %u refused, %u closed, "
                              "%u handler run(s)\n",
                           (unsigned)s->launches, (unsigned)s->rejections,
                           (unsigned)s->closes, (unsigned)s->events);
        trace_ret((long)n, "app", "action=list running=%d", n);
        return TOOL_OK;
    }

    if (!streq(act, "state") && !streq(act, "close"))
        return fail(r, TOOL_EINVAL, args,
                    "unknown action \"%s\". Valid: format (the document format "
                    "and a worked example), caps (what a handler may ask the "
                    "kernel to do, e.g. play a sound), launch, list, state, "
                    "close", act);

    if (has_id != 1) {
        r->is_error = 1;
        tool_result_printf(r, "error: \"%s\" needs \"id\". ", act);
        put_running(r);
        tool_result_printf(r, "\n");
        trace_err(TOOL_EINVAL, "app", "action=%s id=?", act);
        return TOOL_EINVAL;
    }
    if (!app_is_app((uint32_t)id64)) {
        r->is_error = 1;
        tool_result_printf(r, "error: no app with id %u%s. ", (unsigned)id64,
                           gui_window((uint32_t)id64)
                               ? " (that window exists, but it was not launched "
                                 "from a document)" : "");
        put_running(r);
        tool_result_printf(r, "\n");
        trace_err(TOOL_ENOENT, "app", "%s", args);
        return TOOL_ENOENT;
    }

    if (streq(act, "state")) {
        tool_result_printf(r, "app id=%u \"%s\"%s\n", (unsigned)id64,
                           app_title((uint32_t)id64),
                           gui_focused() == (uint32_t)id64 ? " (focused)" : "");
        put_vars(r, (uint32_t)id64);
        put_widgets(r, (uint32_t)id64, "  ");
        const char *why = app_last_error((uint32_t)id64);
        if (why && why[0]) tool_result_printf(r, "note: %s\n", why);
        trace_ok("app", "%s", args);
        return TOOL_OK;
    }

    /* close — the title is captured first, because closing frees it. */
    char   title[GUI_TITLE_MAX];
    size_t i = 0;
    for (const char *t = app_title((uint32_t)id64);
         t[i] && i < sizeof title - 1; i++) title[i] = t[i];
    title[i] = '\0';

    int crc = app_close((uint32_t)id64);
    gui_sync();
    if (crc != APP_OK)
        return fail(r, TOOL_EINVAL, args, "app %u could not be closed",
                    (unsigned)id64);
    tool_result_printf(r,
        "closed app id=%u \"%s\". %d app(s) and %d window(s) remain.\n",
        (unsigned)id64, title, app_count(), gui_window_count());
    trace_ret((long)app_count(), "app", "action=close id=%u title=%s",
              (unsigned)id64, title);
    return TOOL_OK;
}

/* THE TWO STRINGS BELOW ARE SPENT FROM A SHARED BUDGET. Read the header before
 * editing them, and re-run tests/host/test_app.c's
 * test_tool_registry_and_schema_cost(), which measures the REAL assembled array
 * and fails if the headroom against CHAT_TOOLS_BYTES gets thin.
 *
 * "THE THREE MISTAKES THAT COST A ROUND" IS NOT PADDING - IT IS THE CHEAPEST
 * BYTES IN THIS FILE. Two live sessions were transcribed while a real model
 * authored a calculator and a stopwatch, and it made the same three format
 * errors both times: handlers written inside the widget instead of in a
 * top-level "on"; "span" instead of rowspan/colspan; and a C ternary inside an
 * expression. Each one is recoverable - apps/runtime.c names the exact path and
 * the accepted keys, and the model fixed every one on the next round - but a
 * retry is not free: the rejected document stays in the conversation, so five
 * attempts at a 2-4 KiB calculator exhausted CHAT_HISTORY_BYTES and the turn
 * died with nothing on screen ("[chat: no room left to remember this turn]").
 * Forty bytes of warning here is worth ~10 KiB of history and two paid rounds,
 * and it is spent from schema headroom that has 6 KiB spare. If you find a
 * fourth mistake in a transcript, put it here rather than in action=format:
 * these bytes are read before the first attempt, and that text is not. */
static const tool_t app_tool = {
    .name = "app",
    .description =
        "Build a graphical app from a JSON document you write and run it in a "
        "real window. THIS IS THE ANSWER TO \"I want <anything>\" - a "
        "stopwatch, a tip calculator, a window that only says hello: you author "
        "it, so never tell the operator this machine has no such app (gui_open's "
        "two demos are not the limit). action=launch with document, e.g. "
        "{\"title\":\"Hi\",\"width\":200,\"height\":90,"
        "\"grid\":{\"rows\":1,\"cols\":1},\"widgets\":[{\"kind\":\"label\","
        "\"text\":\"hello\",\"row\":0,\"col\":0}]} - that document works as-is. "
        "kind: label button field panel, each with row+col; add "
        "\"vars\" and \"on\":[{\"click\":\"<widget name>\",\"do\":[{\"set\":\"<name>\","
        "\"to\":\"<expression>\"}]}] for behaviour. Expressions also do chance "
        "and time: rand(n) is 0..n-1, round(x*100)/100 is money, at(s,i) picks "
        "a character, now is seconds since boot, and clock/today/hour/minute/"
        "second read the time. APPS CAN RUN ON THEIR OWN: a handler written "
        "{\"tick\":1000,\"do\":[...]} fires every 1000 ms with no click, so a "
        "clock, a stopwatch or a countdown really do keep moving - do not offer "
        "a Refresh button instead. AND THEY CAN REACH HARDWARE: a do statement "
        "{\"call\":\"audio.tone\",\"with\":{\"hz\":\"440\"}} really makes a "
        "sound (app action=caps for the bounds). Send it compact: a reply past "
        "the output "
        "limit loses the arguments. MISTAKES THAT COST A ROUND, seen live: "
        "on is top-level, never inside a widget; click matches a widget's name "
        "or shared tag, NEVER its visible text, so name every button a handler "
        "fires; every to is a STRING holding an expression, with no ?: and no "
        "if-object in it, because {if,then,else} is a statement in do; widen a "
        "cell with rowspan/colspan, not span; at most 16 handlers, so give a "
        "keypad one tag and branch on key. Otherwise guessing is safe: a "
        "rejection names the exact fault and carries a working skeleton, so "
        "attempt two lands. action=format is the full spec. "
        "Also list, state (id), close (id).",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},"
        "\"document\":{\"type\":\"object\"},\"id\":{\"type\":\"integer\"}},"
        "\"required\":[\"action\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_app,
};
REGISTER_TOOL(app_tool);
