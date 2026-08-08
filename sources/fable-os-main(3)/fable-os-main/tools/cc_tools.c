/* cc_tools.c — the model's three verbs for the C compiler.
 *
 *     cc_compile   C source -> a compiled, run, and optionally SAVED program
 *     cc_call      run one that was saved, by name
 *     cc_source    read a saved program's source back, or list what exists
 *
 * WHY THREE AND NOT FIVE. Every byte of every description below is spent out of
 * include/chat.h's CHAT_TOOLS_BYTES, which had 4323 bytes unspent when this file
 * was written — and the C SUBSET MANUAL has to come out of the same budget,
 * because a model that does not know what the subset is will write code outside
 * it and burn a turn finding out. So compiling, running and saving are one verb
 * (they are one intention: "make this work and keep it"), and listing and
 * reading back are one verb. Two tools' worth of name, schema and JSON scaffold
 * is roughly 500 bytes, which is a quarter of the manual.
 *
 * THE MANUAL IS A USER INTERFACE, exactly like driver_run's ISA text in
 * tools/dvm_tools.c. It is terse because it must be, and every line of it is
 * load-bearing: what is in the subset, what is not, the entry point, the symbol
 * table, and the two budgets that can stop a program. tests/host/test_cc.c
 * compiles the example in it verbatim, so the manual cannot drift from the
 * compiler.
 *
 * THE PANEL. cc_call's description ends with a live list of saved programs, read
 * off the store, at a FIXED CC_PANEL_CHARS characters. Same trick and same
 * defence as tools/capability_tools.c: a _Static_assert pins the pad literal,
 * cc_panel() sanitises every byte, and cc_panel_check() re-measures at boot and
 * prints both numbers if they ever disagree. Without it the assembled schema
 * size would be a function of what the operator saved last night, and
 * CHAT_REGISTRY_BYTES is a compile-time constant that `make test-qemu` fails on.
 */

#include "cc.h"
#include "tool.h"
#include "trace.h"
#include "json.h"
#include "kernel.h"
#include "vfs.h"

#include <stdio.h>
#include <string.h>

#define CCT_ERR_MAX 256

/* ====================================================================== */
/* the live panel                                                         */
/* ====================================================================== */

#define CCT_PAD64  "                                                                "
#define CCT_PAD128 CCT_PAD64  CCT_PAD64
#define CCT_PAD384 CCT_PAD128 CCT_PAD128 CCT_PAD128

_Static_assert(sizeof(CCT_PAD384) - 1 == CC_PANEL_CHARS,
               "the reserved panel padding must be exactly CC_PANEL_CHARS "
               "characters, or the assembled tool schema changes size when a "
               "program is saved and CHAT_REGISTRY_BYTES goes stale");

#define CCT_CALL_HEAD                                                          \
    "Run a C program this machine compiled and kept, by name. It was authored " \
    "here, so prefer calling it over writing the same code again. \"args\" "    \
    "must hold exactly as many whole numbers as it declares. SAVED HERE "       \
    "(live, off the store): "

static char g_call_desc[] = CCT_CALL_HEAD CCT_PAD384;
#define CCT_PANEL_OFF (sizeof(CCT_CALL_HEAD) - 1)

_Static_assert(sizeof g_call_desc == CCT_PANEL_OFF + CC_PANEL_CHARS + 1,
               "g_call_desc must be exactly the prose plus the panel plus a NUL");

/* #include "name.h" resolves against the compiler's data root, which is also
 * where a compiled program's file_write lands. That is deliberate and it is the
 * one path by which compiled code extends the compiler's own input: a program
 * can write a header that a later compile includes. It cannot reach the program
 * store, which is a different directory (cc_store.c). */
static int include_reader(const char *name, char *dst, size_t cap) {
    char path[VFS_PATH_MAX + 1];
    if (snprintf(path, sizeof path, "%s/%s", cc_data_root(), name) < 0) return -1;
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) return -1;
    int64_t n = vfs_read(f, dst, (uint64_t)(cap - 1));
    vfs_close(f);
    if (n < 0) return -1;
    dst[n] = '\0';
    return (int)n;
}

void cc_bringup(void) {
    cc_panel_bind(g_call_desc + CCT_PANEL_OFF, CC_PANEL_CHARS);
    cc_set_include_reader(include_reader);
    cc_init();

    char why[CCT_ERR_MAX];
    if (!cc_panel_check(why, sizeof why))
        kprintf("cc: PANEL SIZE MISMATCH - %s\n", why);

    /* Say the compiler's stack budget and the stack it actually has, in the same
     * line, every boot. CC_STACK_BUDGET is a constant that mirrors a measured
     * quantity, and this tree has been broken twice by exactly that shape going
     * stale unnoticed. Whoever changes the root stack size, or moves the compile
     * onto a different fiber, sees the consequence here on the next boot instead
     * of in a triple fault. */
    cc_stack_report();
}

/* ====================================================================== */
/* shared argument handling                                               */
/* ====================================================================== */

/* One shared source buffer. Static because CC_SRC_MAX on the kernel stack (64
 * KiB, shared with lwIP and mbedTLS) is not a trade worth making, and because
 * exactly one tool call runs at a time. */
static char       g_src[CC_SRC_MAX + 1];
static cc_result_t g_res;

static int fail(tool_result_t *r, int err, const char *op, const char *arg,
                const char *why) {
    r->is_error = 1;
    tool_result_printf(r, "%s", why);
    trace_err(err, op, "%s", arg ? arg : "");
    return TOOL_OK;
}

static int parse_input(const tool_call_t *call, json_value_t *out,
                       char *why, size_t cap) {
    if (!call->input || call->input_len == 0) {
        snprintf(why, cap, "no arguments were given");
        return -1;
    }
    if (json_parse(call->input, call->input_len, out) != JSON_OK ||
        out->type != JSON_OBJECT) {
        snprintf(why, cap, "the arguments are not a JSON object");
        return -1;
    }
    return 0;
}

/* "args":[...] of whole numbers. Absent is zero arguments. */
static int want_args(const json_value_t *in, uint64_t *dst, int *nargs,
                     char *why, size_t cap) {
    *nargs = 0;
    json_value_t arr;
    if (json_get(in, "args", &arr) != JSON_OK) return 0;
    if (arr.type != JSON_ARRAY) {
        snprintf(why, cap, "\"args\" must be an array of whole numbers");
        return -1;
    }
    size_t n = json_count(&arr);
    if (n > CC_MAX_PARAMS) {
        snprintf(why, cap, "\"args\" holds %d values; at most %d are possible",
                 (int)n, CC_MAX_PARAMS);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        json_value_t e;
        int64_t v;
        if (json_at(&arr, i, &e) != JSON_OK || json_int(&e, &v) != JSON_OK) {
            snprintf(why, cap, "argument %d is not a whole number", (int)i + 1);
            return -1;
        }
        dst[i] = (uint64_t)v;
    }
    *nargs = (int)n;
    return 0;
}

static int want_fuel(const json_value_t *in, uint64_t *out,
                     char *why, size_t cap) {
    *out = 0;
    json_value_t v;
    if (json_get(in, "fuel", &v) != JSON_OK) return 0;
    int64_t iv;
    if (json_int(&v, &iv) != JSON_OK || iv < 1) {
        snprintf(why, cap, "\"fuel\" must be a positive whole number (loop "
                           "iterations plus calls); omit it for %d",
                 (int)CC_FUEL_DEFAULT);
        return -1;
    }
    *out = (uint64_t)iv;
    return 0;
}

/* Report a compile failure: every diagnostic, rendered. */
static void report_diags(tool_result_t *r, const cc_result_t *res) {
    char buf[CC_DIAG_MAX * (CC_MSG_MAX + CC_SNIP_MAX + 64)];
    cc_diags_render(res, buf, sizeof buf);
    tool_result_printf(r, "%s", buf);
}

/* Report what a run cost and what it produced. */
static void report_run(tool_result_t *r, const cc_result_t *res) {
    tool_result_printf(r, "returned %lld\n",
                       (long long)(int64_t)res->value);
    if (res->out_len)
        tool_result_printf(r, "it printed:\n%s%s", res->out,
                           res->out[res->out_len - 1] == '\n' ? "" : "\n");
    tool_result_printf(r,
                       "cost: %llu of the fuel budget, %llu byte(s) of its own "
                       "stack, %u kernel call(s); the image is %u byte(s) of "
                       "machine code from %u function(s)",
                       (unsigned long long)res->fuel_used,
                       (unsigned long long)res->stack_used,
                       res->ncalls, res->code_bytes, res->nfuncs);
}

/* The one sentence for a run that was stopped rather than finished. */
static void report_stop(tool_result_t *r, const cc_result_t *res, int rc) {
    if (rc == CC_EFUEL)
        tool_result_printf(r,
                           "STOPPED: it used all %llu units of fuel without "
                           "finishing. Fuel is charged once per loop iteration "
                           "and once per call, so this is a loop that does not "
                           "end or a recursion with no base case. Fix the "
                           "program, or pass a larger \"fuel\" if the work is "
                           "genuinely that big (the maximum is %llu).",
                           (unsigned long long)res->fuel_used,
                           (unsigned long long)CC_FUEL_MAX);
    else if (rc == CC_ESTACK)
        tool_result_printf(r,
                           "STOPPED: it used all %d bytes of its own stack. A "
                           "compiled program runs on a private stack so this "
                           "cannot touch the kernel's - but it means the "
                           "recursion has no base case, or a function's locals "
                           "are too large. It got %llu byte(s) deep.",
                           CC_STACK_BYTES - CC_STACK_SLACK,
                           (unsigned long long)res->stack_used);
    else
        tool_result_printf(r, "the program did not run (code %d)", rc);
    if (res->out_len)
        tool_result_printf(r, "\nbefore stopping it printed:\n%s", res->out);
}

/* ====================================================================== */
/* cc_compile                                                             */
/* ====================================================================== */

static int want_params(const json_value_t *in,
                       char store[CC_MAX_PARAMS][CC_PARAM_NAME_MAX],
                       const char *ptr[CC_MAX_PARAMS], int *nparam,
                       char *why, size_t cap) {
    *nparam = 0;
    json_value_t arr;
    if (json_get(in, "params", &arr) != JSON_OK) return 0;
    if (arr.type != JSON_ARRAY) {
        snprintf(why, cap, "\"params\" must be an array of names");
        return -1;
    }
    size_t n = json_count(&arr);
    if (n > CC_MAX_PARAMS) {
        snprintf(why, cap, "\"params\" names %d parameters; at most %d are "
                           "possible", (int)n, CC_MAX_PARAMS);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        json_value_t e;
        if (json_at(&arr, i, &e) != JSON_OK ||
            json_str(&e, store[i], CC_PARAM_NAME_MAX, NULL) != JSON_OK) {
            snprintf(why, cap, "parameter %d must be a name of at most %d "
                               "characters", (int)i + 1, CC_PARAM_NAME_MAX - 1);
            return -1;
        }
        ptr[i] = store[i];
    }
    *nparam = (int)n;
    return 0;
}

static int t_compile(const tool_call_t *call, tool_result_t *r) {
    char why[CCT_ERR_MAX];
    json_value_t in;

    if (parse_input(call, &in, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cc_compile", NULL, why);

    size_t srclen = 0;
    {
        json_value_t v;
        if (json_get(&in, "source", &v) != JSON_OK || v.type != JSON_STRING)
            return fail(r, TOOL_EINVAL, "cc_compile", NULL,
                        "\"source\" is required and must be a string of C code");
        if (json_str(&v, g_src, sizeof g_src, &srclen) != JSON_OK) {
            snprintf(why, sizeof why,
                     "\"source\" is longer than %d bytes. Split the work into "
                     "programs that call each other, or use #include of a header "
                     "you saved with file_write", CC_SRC_MAX);
            return fail(r, TOOL_EINVAL, "cc_compile", NULL, why);
        }
    }

    char name[CC_NAME_MAX];
    int  saving = (json_get_str(&in, "name", name, sizeof name) == JSON_OK);
    char doc[CC_DOC_MAX];
    doc[0] = '\0';
    (void)json_get_str(&in, "doc", doc, sizeof doc);

    char pstore[CC_MAX_PARAMS][CC_PARAM_NAME_MAX];
    const char *pptr[CC_MAX_PARAMS];
    int nparam = 0;
    if (want_params(&in, pstore, pptr, &nparam, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cc_compile", NULL, why);

    uint64_t args[CC_MAX_PARAMS];
    int nargs = 0;
    for (int i = 0; i < CC_MAX_PARAMS; i++) args[i] = 0;
    if (want_args(&in, args, &nargs, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cc_compile", NULL, why);
    uint64_t fuel = 0;
    if (want_fuel(&in, &fuel, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cc_compile", NULL, why);

    int want_run = 1;
    {
        json_value_t v;
        int b;
        if (json_get(&in, "run", &v) == JSON_OK && json_bool(&v, &b) == JSON_OK)
            want_run = b;
    }

    /* ONE TRACE LINE PER TOOL CALL. Compiling, saving and running are three
     * kernel actions and it is tempting to emit three lines, but every other tool
     * family in this tree emits exactly one and tests/host asserts it: the
     * operator counts lines to count actions (trace.h). So the outcome is
     * collected and emitted once, at the end, with all of it in the arguments. */
    char what[96];
    int  rc;

    if (saving) {
        char msg[CCT_ERR_MAX];
        rc = cc_save(name, doc, pptr, nparam, g_src, srclen, &g_res,
                     msg, sizeof msg);
        if (rc != CC_OK) {
            r->is_error = 1;
            tool_result_printf(r, "%s\n", msg);
            if (g_res.ndiag) report_diags(r, &g_res);
            trace_err(rc, "cc_compile", "save %s", name);
            return TOOL_OK;
        }
        tool_result_printf(r, "%s\n", msg);
        tool_result_printf(r, "it compiled: %u byte(s) of machine code, %u "
                              "function(s), %u byte(s) of globals and string "
                              "literals.\n",
                           g_res.code_bytes, g_res.nfuncs, g_res.data_bytes);
        if (!want_run) {
            trace_ret((long)g_res.code_bytes, "cc_compile",
                      "save %s v%u, %u insn bytes, not run", name,
                      (unsigned)(cc_find(name) ? cc_find(name)->version : 1),
                      g_res.code_bytes);
            return TOOL_OK;
        }
        if (nargs != nparam) {
            tool_result_printf(r, "It was not run: it takes %d argument(s) and "
                                  "\"args\" gave %d.", nparam, nargs);
            trace_ret((long)g_res.code_bytes, "cc_compile",
                      "save %s, %d args wanted %d given", name, nparam, nargs);
            return TOOL_OK;
        }
        snprintf(what, sizeof what, "save %s v%u, run", name,
                 (unsigned)(cc_find(name) ? cc_find(name)->version : 1));
    } else {
        rc = cc_compile(g_src, srclen, "main.c", &g_res);
        if (rc != CC_OK) {
            r->is_error = 1;
            report_diags(r, &g_res);
            trace_err(rc, "cc_compile", "%d bytes of C", (int)srclen);
            return TOOL_OK;
        }
        tool_result_printf(r, "it compiled: %u byte(s) of machine code, %u "
                              "function(s), %u byte(s) of globals and string "
                              "literals; main() takes %u argument(s).\n",
                           g_res.code_bytes, g_res.nfuncs, g_res.data_bytes,
                           g_res.entry_params);
        if (!want_run) {
            tool_result_printf(r, "It was not run. Nothing was saved - add "
                                  "\"name\", \"doc\" and \"params\" to keep it.");
            trace_ret((long)g_res.code_bytes, "cc_compile",
                      "%d bytes of C -> %u func(s), not run",
                      (int)srclen, g_res.nfuncs);
            return TOOL_OK;
        }
        if (nargs != (int)g_res.entry_params) {
            r->is_error = 1;
            tool_result_printf(r, "It was not run: main() takes %u argument(s) and "
                                  "\"args\" gave %d.", g_res.entry_params, nargs);
            trace_err(TOOL_EINVAL, "cc_compile", "main takes %u args, %d given",
                      g_res.entry_params, nargs);
            return TOOL_OK;
        }
        snprintf(what, sizeof what, "%d bytes of C -> %u func(s), run",
                 (int)srclen, g_res.nfuncs);
    }

    int rrc = cc_run(args, nargs, fuel, &g_res);
    if (rrc != CC_OK) {
        r->is_error = 1;
        report_stop(r, &g_res, rrc);
        trace_err(rrc, "cc_compile", "%s fuel=%llu", what,
                  (unsigned long long)g_res.fuel_used);
        return TOOL_OK;
    }
    report_run(r, &g_res);
    trace_ret((long)(int64_t)g_res.value, "cc_compile", "%s fuel=%llu", what,
              (unsigned long long)g_res.fuel_used);
    return TOOL_OK;
}

static const tool_t cc_compile_def = {
    .name = "cc_compile",
    .description =
        "Compile C source ON THIS MACHINE to native x86-64 and run it. Give "
        "\"name\", \"doc\" and \"params\" as well to KEEP it, callable later and "
        "after a reboot, with cc_call.\n"
        "ENTRY main(), up to 6 whole-number parameters, returns a whole number. "
        "\"args\" are passed to it.\n"
        "IN void/char/short/int/long, signed+unsigned, struct, union, enum, "
        "typedef, pointers, arrays; if else while do for switch break continue "
        "return; every integer operator, ?:, sizeof, casts, ++/--, compound "
        "assignment; string and char literals; { } initialisers; #define #undef "
        "#ifdef #ifndef #else #endif #include. uint8_t..uint64_t, int8_t..int64_t "
        "and size_t need no include.\n"
        "OUT, each REFUSED with a line, a column and the offending text: float "
        "and double; varargs; FUNCTION POINTERS (a call target must be a name - "
        "that is what bounds where compiled code can jump); goto and labels; "
        "struct by value in a parameter or return (pass a pointer; struct "
        "assignment works); bitfields; #if; ##; hosted libc headers.\n"
        "KERNEL CALLS, already declared, and this list is complete: "
        "print(char*) print_num(long) print_hex(unsigned long) print_char(long) "
        "time_ms() strlen(char*) memcpy(d,s,n) memset(d,c,n) memcmp(a,b,n) "
        "file_write(name,buf,len) file_read(name,buf,max) file_size(name). "
        "file_* take a plain name, not a path, and land in one private "
        "directory.\n"
        "BUDGETS it cannot exceed: fuel, charged per loop iteration and per call, "
        "so an endless loop STOPS instead of hanging the machine; and a private "
        "48 KiB stack, so runaway recursion cannot reach the kernel's. There is "
        "NO memory protection: a wild pointer is a real store. EXAMPLE {\"source\":\"int main(long n){long i,s=0;for(i=1;i<=n;"
        "i++)s+=i*i;print(\\\"sum of squares: \\\");print_num(s);return s;}\","
        "\"args\":[10]}",
    /* Terse on purpose: every property is explained in the description above,
     * and saying it twice costs schema this family does not have. */
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
          "\"source\":{\"type\":\"string\"},"
          "\"args\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
          "\"run\":{\"type\":\"boolean\"},"
          "\"name\":{\"type\":\"string\"},"
          "\"doc\":{\"type\":\"string\"},"
          "\"params\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
          "\"fuel\":{\"type\":\"integer\"}},"
        "\"required\":[\"source\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_compile,
};
REGISTER_TOOL(cc_compile_def);

/* ====================================================================== */
/* cc_call                                                                */
/* ====================================================================== */

static int t_call(const tool_call_t *call, tool_result_t *r) {
    char why[CCT_ERR_MAX];
    json_value_t in;

    if (parse_input(call, &in, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cc_call", NULL, why);

    char name[CC_NAME_MAX];
    if (json_get_str(&in, "name", name, sizeof name) != JSON_OK)
        return fail(r, TOOL_EINVAL, "cc_call", NULL,
                    "\"name\" is required and must be the name of a saved "
                    "program");
    if (!cc_name_ok(name, why, sizeof why)) {
        char m[CCT_ERR_MAX];
        snprintf(m, sizeof m, "\"name\": %s", why);
        return fail(r, TOOL_EINVAL, "cc_call", name, m);
    }

    const cc_prog_t *p = cc_find(name);
    if (!p) {
        char m[CCT_ERR_MAX];
        size_t o = (size_t)snprintf(m, sizeof m,
                                    "there is no compiled program called "
                                    "\"%s\". ", name);
        if (cc_count() == 0)
            snprintf(m + o, sizeof m - o, "None are saved. Use cc_compile with "
                                          "a \"name\" to keep one.");
        else {
            o += (size_t)snprintf(m + o, sizeof m - o, "Saved here: ");
            for (int i = 0; i < cc_count() && o + 2 < sizeof m; i++) {
                int w = snprintf(m + o, sizeof m - o, "%s%s",
                                 i ? ", " : "", cc_at(i)->name);
                if (w < 0 || (size_t)w >= sizeof m - o) break;
                o += (size_t)w;
            }
        }
        return fail(r, CC_ENOENT, "cc_call", name, m);
    }

    uint64_t args[CC_MAX_PARAMS];
    int nargs = 0;
    for (int i = 0; i < CC_MAX_PARAMS; i++) args[i] = 0;
    if (want_args(&in, args, &nargs, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cc_call", name, why);
    if (nargs != p->nparam) {
        char m[CCT_ERR_MAX];
        size_t o = (size_t)snprintf(m, sizeof m,
                                    "\"%s\" takes %d argument(s) and %d were "
                                    "given. Its parameters are ",
                                    name, p->nparam, nargs);
        if (p->nparam == 0) snprintf(m + o, sizeof m - o, "none.");
        else for (int i = 0; i < p->nparam && o + 2 < sizeof m; i++) {
            int w = snprintf(m + o, sizeof m - o, "%s%s",
                             i ? ", " : "", p->param[i]);
            if (w < 0 || (size_t)w >= sizeof m - o) break;
            o += (size_t)w;
        }
        return fail(r, TOOL_EINVAL, "cc_call", name, m);
    }
    uint64_t fuel = 0;
    if (want_fuel(&in, &fuel, why, sizeof why) != 0)
        return fail(r, TOOL_EINVAL, "cc_call", name, why);

    int rc = cc_call(name, args, nargs, fuel, &g_res);
    if (rc == CC_OK) {
        trace_ret((long)(int64_t)g_res.value, "cc_call", "%s fuel=%llu",
                  name, (unsigned long long)g_res.fuel_used);
        report_run(r, &g_res);
        return TOOL_OK;
    }
    trace_err(rc, "cc_call", "%s", name);
    r->is_error = 1;
    if (rc == CC_EFUEL || rc == CC_ESTACK) { report_stop(r, &g_res, rc); return TOOL_OK; }
    if (g_res.ndiag) {
        tool_result_printf(r, "\"%s\" no longer compiles, so it did not run. The "
                              "stored source is C text - read it with cc_source "
                              "and save a fixed version:\n", name);
        report_diags(r, &g_res);
        return TOOL_OK;
    }
    if (rc == CC_ECORRUPT)
        tool_result_printf(r, "\"%s\" is on disk but its record failed its own "
                              "checksum, so it was not run.", name);
    else
        tool_result_printf(r, "\"%s\" could not be run (code %d).", name, rc);
    return TOOL_OK;
}

static const tool_t cc_call_def = {
    .name         = "cc_call",
    .description  = g_call_desc,
    .input_schema =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"name\":{\"type\":\"string\"},"
          "\"args\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
          "\"fuel\":{\"type\":\"integer\"}},"
        "\"required\":[\"name\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_call,
};
REGISTER_TOOL(cc_call_def);

/* ====================================================================== */
/* cc_source                                                              */
/* ====================================================================== */

static int t_source(const tool_call_t *call, tool_result_t *r) {
    char why[CCT_ERR_MAX];
    json_value_t in;
    char name[CC_NAME_MAX];
    name[0] = '\0';

    if (call->input && call->input_len) {
        if (parse_input(call, &in, why, sizeof why) != 0)
            return fail(r, TOOL_EINVAL, "cc_source", NULL, why);
        /* Presence with json_get, not json_get_str: an over-long name makes
         * json_get_str return ENOSPC, which read as "absent" would answer a
         * different question from the one that was asked. */
        json_value_t v;
        if (json_get(&in, "name", &v) == JSON_OK) {
            if (json_str(&v, name, sizeof name, NULL) != JSON_OK) {
                snprintf(why, sizeof why, "\"name\" must be at most %d "
                                          "characters", CC_NAME_MAX - 1);
                return fail(r, TOOL_EINVAL, "cc_source", NULL, why);
            }
        }
    }

    if (!name[0]) {
        tool_result_printf(r, "store %s (%s), private file directory %s\n",
                           cc_store_root(),
                           cc_store_persists() ? "survives a reboot"
                                               : "RAM ONLY - nothing here "
                                                 "survives a reboot",
                           cc_data_root());
        if (cc_rejected())
            tool_result_printf(r, "%d stored record(s) were REJECTED at boot "
                                  "(checksum or a name that disagreed with its "
                                  "filename)\n", cc_rejected());
        tool_result_printf(r, "%d saved, %d slot(s) free:\n",
                           cc_count(), CC_MAX_PROGRAMS - cc_count());
        for (int i = 0; i < cc_count(); i++) {
            const cc_prog_t *p = cc_at(i);
            tool_result_printf(r, "%s(", p->name);
            for (int k = 0; k < p->nparam; k++)
                tool_result_printf(r, "%s%s", k ? "," : "", p->param[k]);
            tool_result_printf(r, ") v%u %u byte(s) of C, %u call(s), %u "
                                  "failure(s): %s\n",
                               (unsigned)p->version, p->src_len,
                               p->calls, p->failures, p->doc);
        }
        trace_ret(cc_count(), "cc_source", "list");
        return TOOL_OK;
    }

    const cc_prog_t *p = cc_find(name);
    if (!p) {
        char m[CCT_ERR_MAX];
        snprintf(m, sizeof m, "there is no compiled program called \"%s\"; "
                              "cc_source with no name lists what there is", name);
        return fail(r, CC_ENOENT, "cc_source", name, m);
    }
    int got = cc_source(name, g_src, sizeof g_src);
    if (got < 0) {
        char m[CCT_ERR_MAX];
        snprintf(m, sizeof m, "\"%s\" is registered but its stored record could "
                              "not be read back (code %d)", name, got);
        return fail(r, got, "cc_source", name, m);
    }
    trace_ret(got, "cc_source", "%s v%u", name, (unsigned)p->version);
    tool_result_printf(r, "%s(", p->name);
    for (int k = 0; k < p->nparam; k++)
        tool_result_printf(r, "%s%s", k ? "," : "", p->param[k]);
    tool_result_printf(r, ") v%u: %s\n%s", (unsigned)p->version, p->doc, g_src);
    if (r->truncated)
        tool_result_printf(r, "\n(the source is %d bytes and did not all fit)", got);
    return TOOL_OK;
}

static const tool_t cc_source_def = {
    .name = "cc_source",
    .description =
        "With a \"name\", read a saved C program's source back as it was kept - "
        "which is how you repair one: read it, fix it, cc_compile it under the "
        "same name. With no arguments, list every saved program with its "
        "parameters, size, calls and failures, and whether the store survives a "
        "reboot.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}}}",
    .flags  = 0,
    .invoke = t_source,
};
REGISTER_TOOL(cc_source_def);
