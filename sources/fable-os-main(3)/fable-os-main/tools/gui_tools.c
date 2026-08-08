/* gui_tools.c — the GUI tool family: the model inspects and drives the screen.
 *
 * PURPOSE
 *   The operator says "I want a calculator" in a sentence, so the model needs
 *   syscalls that open, inspect, move, close and operate windows. Four tools,
 *   which is fewer than the five capabilities the brief lists, for a reason
 *   worth stating: the assembled tool schema is a shared, nearly exhausted
 *   resource. CHAT_TOOLS_BYTES is 32768 and the 35 tools already registered
 *   spend 27 KB of it; overflowing it offers the model NO tools at all (see
 *   chat.h). focus/move/close/hide are therefore one `gui_window` tool with an
 *   `action`, which costs one enum the model reads easily and saves ~1.5 KB of
 *   schema that the whole machine shares.
 *
 * WHAT EACH ONE IS FOR
 *   gui_list    the map. Windows, z-order, focus, geometry, and every widget's
 *               id, kind and current text. This is also "read what is on
 *               screen" for the GUI: read_screen (screen_tools.c) reads the text
 *               console, and this reads the windows floating over it. A tool
 *               whose arguments are ids and coordinates is unusable without it.
 *   gui_open    open one of the built-in apps by name. The app-authoring phase
 *               replaces this with model-authored layouts; the shape of the
 *               answer (an id, and a reason when there is none) will not change.
 *   gui_window  act on a window: focus, raise, move, resize, hide, show, close.
 *   gui_click   press a widget, by label, by id, or at a pixel. This is the
 *               model operating the interface it asked for — and it is the same
 *               dispatch path a real mouse click takes, not a shortcut into the
 *               app, so anything it proves is true of the hardware too.
 *
 * UNTRUSTED INPUT
 *   `call->input` is model output. Every field is read through json.h, types are
 *   checked before values and values before ranges, and nothing indexes memory
 *   from an unvalidated number: window ids are looked up in the pool (a stale id
 *   resolves to "no such window" because ids are never reused), widget lookups
 *   go through gui_widget_by_id / a bounded label compare, and coordinates are
 *   range-limited here and then clamped again by the WM, which is where the
 *   screen's real size lives. Nothing here allocates.
 *
 *   Rejections are legible and name the ground truth, because a model that is
 *   told "id 7 does not exist; open windows are 3 and 5" fixes itself next turn,
 *   and one that is told "EINVAL" guesses.
 *
 * EVERY CALL EMITS EXACTLY ONE TRACE LINE
 *   Success and failure both, through trace.h, from real return values after the
 *   call happened — including the action name and the resolved ids, so the
 *   transcript shows which window was really closed and not which one the model
 *   said it closed.
 *
 * HOST TESTABILITY
 *   Everything here is gui.h calls, and gui.h attaches to a caller-supplied
 *   fb_surface_t, so tests/host/test_gui.c drives these tools against a malloc'd
 *   framebuffer with no QEMU and asserts both the result text and the pixels.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "gui.h"
#include "widgets.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>       /* vsnprintf, via port/stdio.h when freestanding */

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand. Export a named pointer per tool instead; tests/host/test_gui.c stands
 * in for the linker. Kernel builds are untouched. */
#ifdef FABLEOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const fableos_hosttool_##var = &(var)
#endif

/* Coordinates are clamped by the WM, but a value this far outside the screen is
 * a mistake worth naming rather than silently pulling back to the edge. */
#define COORD_LIMIT  100000

/* Keep this much result budget free so a truncated listing can always say so. */
#define TAIL_RESERVE 160

/* ====================================================================== */
/* shared helpers (the tools/mem_tools.c + screen_tools.c pattern)         */
/* ====================================================================== */

/* Build the trace line's argument field. `args` below is a finished string, not a
 * format — trace_err() renders it with "%s" so that a value which came from the
 * model can never be interpreted as a conversion specifier. */
static void argf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

static int fail(tool_result_t *r, int err, const char *op, const char *args,
                const char *fmt, ...) {
    char    msg[352];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    r->is_error  = 1;
    r->len       = 0;
    r->truncated = 0;
    if (r->buf && r->cap) r->buf[0] = '\0';
    tool_result_printf(r, "error: %s", msg);
    trace_err(err, op, "%s", args ? args : "");
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
    /* A fractional coordinate is a mistake, not a value to truncate. */
    for (size_t i = 0; i < v.len; i++)
        if (v.start[i] == '.' || v.start[i] == 'e' || v.start[i] == 'E') return -1;
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

/* -3 additionally means "the decoded string contains an embedded NUL". A JSON
 * string may legally carry one as an escape, and every comparison below is on a
 * C string: without this check the label "7<NUL>junk" would compare equal to the
 * label "7", and a click the model never described would happen. */
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
    if (!a || !b) return 0;
    size_t i = 0;
    for (; a[i] && a[i] == b[i]; i++)
        if (i > 64) return 0;
    return a[i] == '\0' && b[i] == '\0';
}

static const char *kindname(uint8_t k) {
    switch (k) {
    case GUI_PANEL:  return "panel";
    case GUI_LABEL:  return "label";
    case GUI_BUTTON: return "button";
    case GUI_FIELD:  return "field";
    default:         return "?";
    }
}

/* The open window ids, for an error message that lets the model recover. */
static void put_open_ids(tool_result_t *r) {
    int n = gui_window_count();
    if (n == 0) { tool_result_printf(r, "no windows are open"); return; }
    tool_result_printf(r, "open window ids:");
    for (int i = 0; i < n; i++) {
        gui_window_t *w = gui_window_at_z(i);
        if (w) tool_result_printf(r, " %u", (unsigned)w->id);
    }
}

/* Every tool starts here: no framebuffer means no GUI, and saying so once is
 * better than four different confusing failures. */
static int gui_ready(tool_result_t *r, const char *op) {
    if (gui_attached()) return 0;
    return fail(r, TOOL_EINVAL, op, "",
                "this machine has no window manager: there is no linear "
                "framebuffer (the console is VGA text mode). Nothing can be "
                "drawn; the text console tools still work");
}

/* ====================================================================== */
/* gui_list                                                               */
/* ====================================================================== */

static void put_widgets(tool_result_t *r, gui_window_t *w) {
    for (uint16_t i = 0; i < w->nwidgets; i++) {
        gui_widget_t *x = &w->widgets[i];
        if (r->len + TAIL_RESERVE + 96 >= r->cap) {
            tool_result_printf(r, "    [listing truncated at widget %d of %d]\n",
                               (int)i, (int)w->nwidgets);
            return;
        }
        tool_result_printf(r, "    %s id=%u at %d,%d %dx%d",
                           kindname(x->kind), (unsigned)x->id,
                           (int)x->r.x, (int)x->r.y, (int)x->r.w, (int)x->r.h);
        if (x->text[0]) tool_result_printf(r, " text=\"%s\"", x->text);
        if (x->state & GUI_W_DISABLED) tool_result_printf(r, " disabled");
        if (x->state & GUI_W_READONLY) tool_result_printf(r, " readonly");
        if (x->state & GUI_W_FOCUS)    tool_result_printf(r, " focused");
        if (x->state & GUI_W_NOHIT)    tool_result_printf(r, " not-clickable");
        tool_result_printf(r, "\n");
    }
}

static int t_gui_list(const tool_call_t *c, tool_result_t *r) {
    if (gui_ready(r, "gui_list") != 0) return TOOL_EINVAL;

    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "gui_list", "",
                    "input must be a JSON object, e.g. {} or {\"id\":3}");

    int64_t want = 0;
    int rc = f_int(&in, "id", 0, 0x7FFFFFFF, &want);
    if (rc == -1) return fail(r, TOOL_EINVAL, "gui_list", "",
                              "\"id\" must be an integer window id");
    if (rc == -2) return fail(r, TOOL_EINVAL, "gui_list", "",
                              "\"id\" is not a possible window id");

    int widgets = (rc == 1);          /* detail one window by default */
    if (f_bool(&in, "widgets", &widgets) < 0)
        return fail(r, TOOL_EINVAL, "gui_list", "",
                    "\"widgets\" must be a boolean");

    if (rc == 1 && !gui_window((uint32_t)want)) {
        r->is_error = 1;
        tool_result_printf(r, "error: no window with id %u. ", (unsigned)want);
        put_open_ids(r);
        tool_result_printf(r, "\n");
        trace_err(TOOL_ENOENT, "gui_list", "id=%u", (unsigned)want);
        return TOOL_ENOENT;
    }

    int32_t sw = 0, sh = 0;
    gui_screen_size(&sw, &sh);
    int32_t px = 0, py = 0;
    gui_pointer_position(&px, &py);

    int n = gui_window_count();
    tool_result_printf(r,
        "screen: %dx%d pixels; the desktop behind the windows is the text "
        "console\npointer: %d,%d (%s)\nwindows: %d open, focused id=%u\n",
        (int)sw, (int)sh, (int)px, (int)py,
        gui_pointer_visible() ? "visible" : "hidden",
        n, (unsigned)gui_focused());

    int listed = 0;
    /* Top of the z-order first: that is what the operator is looking at. */
    for (int i = n - 1; i >= 0; i--) {
        gui_window_t *w = gui_window_at_z(i);
        if (!w) continue;
        if (rc == 1 && w->id != (uint32_t)want) continue;
        if (r->len + TAIL_RESERVE + 160 >= r->cap) {
            tool_result_printf(r, "[listing truncated: %d window(s) not shown. "
                                  "Call gui_list with \"id\" for one window.]\n",
                               i + 1);
            break;
        }
        tool_result_printf(r,
            "  id=%u \"%s\" at %d,%d %dx%d z=%d%s%s widgets=%d\n",
            (unsigned)w->id, w->title,
            (int)w->frame.x, (int)w->frame.y, (int)w->frame.w, (int)w->frame.h,
            i,
            w->id == gui_focused() ? " focused" : "",
            (w->flags & GUI_WIN_HIDDEN) ? " hidden" : "",
            (int)w->nwidgets);
        if (widgets) put_widgets(r, w);
        listed++;
    }
    if (n == 0)
        tool_result_printf(r,
            "No windows are open. gui_open starts one of the built-in apps.\n");

    trace_ret((long)listed, "gui_list", "windows=%d focus=%u detail=%d",
              n, (unsigned)gui_focused(), widgets);
    return TOOL_OK;
}

static const tool_t gui_list_tool = {
    .name = "gui_list",
    .description =
        "List the graphical windows on screen: each window's id, title, "
        "position, size, z-order (higher is in front), whether it has focus, and "
        "every widget's id, kind (button/label/field/panel) and current text. "
        "This reads the graphical screen; read_screen reads the text console "
        "behind it. Call it before gui_window or gui_click, which take ids.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"integer\",\"description\":\"only this window\"},"
        "\"widgets\":{\"type\":\"boolean\",\"description\":\"list widgets "
        "(default true for one window, false for all)\"}"
        "},\"required\":[]}",
    .flags  = 0,
    .invoke = t_gui_list,
};
REGISTER_TOOL(gui_list_tool);

/* ====================================================================== */
/* gui_open                                                               */
/* ====================================================================== */

static int t_gui_open(const tool_call_t *c, tool_result_t *r) {
    if (gui_ready(r, "gui_open") != 0) return TOOL_EINVAL;

    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "gui_open", "",
                    "input must be a JSON object, e.g. {\"app\":\"calculator\"}");

    char app[32];
    int  rc = f_str(&in, "app", app, sizeof app);
    if (rc <= 0) {
        const char *const *names = gui_demo_names();
        r->is_error = 1;
        tool_result_printf(r, "error: \"app\" is required and must be one of:");
        for (int i = 0; names[i]; i++) tool_result_printf(r, " %s", names[i]);
        tool_result_printf(r, "\n");
        trace_err(TOOL_EINVAL, "gui_open", "app=?");
        return TOOL_EINVAL;
    }

    char args[64];
    argf(args, sizeof args, "app=%s", app);

    int64_t x = 0, y = 0;
    int has_x = f_int(&in, "x", -COORD_LIMIT, COORD_LIMIT, &x);
    int has_y = f_int(&in, "y", -COORD_LIMIT, COORD_LIMIT, &y);
    if (has_x < 0 || has_y < 0)
        return fail(r, TOOL_EINVAL, "gui_open", args,
                    "\"x\" and \"y\" must be integer pixel positions within "
                    "+/-%d when present", COORD_LIMIT);

    const char *why = "";
    /* Open with the WM's own cascade placement, then move if a position was
     * asked for: that keeps the app-name check in exactly one place. */
    uint32_t id = gui_demo_open(app, &why);
    if (id && (has_x == 1 || has_y == 1)) {
        gui_window_t *nw = gui_window(id);
        (void)gui_window_move(id, has_x == 1 ? (int32_t)x : nw->frame.x,
                                  has_y == 1 ? (int32_t)y : nw->frame.y);
    }

    if (!id) {
        const char *const *names = gui_demo_names();
        r->is_error = 1;
        tool_result_printf(r, "error: could not open \"%s\": %s. Available apps:",
                           app, why);
        for (int i = 0; names[i]; i++) tool_result_printf(r, " %s", names[i]);
        tool_result_printf(r, "\n");
        trace_err(TOOL_EINVAL, "gui_open", "app=%s", app);
        return TOOL_EINVAL;
    }

    gui_sync();      /* on screen now, not on the next input poll */

    gui_window_t *w = gui_window(id);
    tool_result_printf(r,
        "opened \"%s\" as window id=%u at %d,%d %dx%d with %d widget(s), "
        "focused and on top. It is clickable with the mouse; gui_click presses a "
        "widget by label. gui_list shows its widgets.\n",
        app, (unsigned)id, (int)w->frame.x, (int)w->frame.y,
        (int)w->frame.w, (int)w->frame.h, (int)w->nwidgets);

    trace_ret((long)id, "gui_open", "app=%s at=%d,%d size=%dx%d widgets=%d",
              app, (int)w->frame.x, (int)w->frame.y,
              (int)w->frame.w, (int)w->frame.h, (int)w->nwidgets);
    return TOOL_OK;
}

static const tool_t gui_open_tool = {
    .name = "gui_open",
    .description =
        "Open one of the two demo apps a human already compiled into this kernel: "
        "calculator (a working fixed-point calculator) or notes (a field you can "
        "type one line into). Those two are all THIS tool has, and they are not "
        "the limit of what this machine can put on screen - for any other window, "
        "including a differently-behaved calculator, author one with the `app` "
        "tool instead of reporting that it does not exist. Opens at once, "
        "focused, on top, clickable with the mouse or gui_click; the position is "
        "chosen for you unless you give x and y.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"app\":{\"type\":\"string\",\"description\":\"a built-in demo name: "
        "calculator or notes. Not an enum of what this machine can display - "
        "anything else is the `app` tool\"},"
        "\"x\":{\"type\":\"integer\",\"description\":\"left edge, pixels\"},"
        "\"y\":{\"type\":\"integer\",\"description\":\"top edge, pixels\"}"
        "},\"required\":[\"app\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_gui_open,
};
REGISTER_TOOL(gui_open_tool);

/* ====================================================================== */
/* gui_window — focus / raise / move / resize / hide / show / close        */
/* ====================================================================== */

static int t_gui_window(const tool_call_t *c, tool_result_t *r) {
    if (gui_ready(r, "gui_window") != 0) return TOOL_EINVAL;

    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "gui_window", "",
                    "input must be a JSON object, e.g. "
                    "{\"id\":3,\"action\":\"move\",\"x\":100,\"y\":80}");

    int64_t id64 = 0;
    int rc = f_int(&in, "id", 1, 0x7FFFFFFF, &id64);
    if (rc == 0)  return fail(r, TOOL_EINVAL, "gui_window", "",
                              "\"id\" is required; gui_list shows the open ids");
    if (rc == -1) return fail(r, TOOL_EINVAL, "gui_window", "",
                              "\"id\" must be an integer window id");
    if (rc == -2) return fail(r, TOOL_EINVAL, "gui_window", "",
                              "\"id\" is not a possible window id");

    char act[24];
    int  arc = f_str(&in, "action", act, sizeof act);
    char args[80];
    if (arc <= 0) {
        argf(args, sizeof args, "id=%u action=?", (unsigned)id64);
        return fail(r, TOOL_EINVAL, "gui_window", args,
                    "\"action\" is required: focus, raise, move, resize, hide, "
                    "show or close");
    }
    argf(args, sizeof args, "action=%s id=%u", act, (unsigned)id64);

    uint32_t      id = (uint32_t)id64;
    gui_window_t *w  = gui_window(id);
    if (!w) {
        r->is_error = 1;
        tool_result_printf(r, "error: no window with id %u. ", (unsigned)id);
        put_open_ids(r);
        tool_result_printf(r, "\n");
        trace_err(TOOL_ENOENT, "gui_window", "action=%s id=%u", act, (unsigned)id);
        return TOOL_ENOENT;
    }

    int64_t x = w->frame.x, y = w->frame.y;
    int64_t ww = w->frame.w, wh = w->frame.h;
    if (f_int(&in, "x", -COORD_LIMIT, COORD_LIMIT, &x) < 0)
        return fail(r, TOOL_EINVAL, "gui_window", args,
                    "\"x\" must be an integer within +/-%d", COORD_LIMIT);
    if (f_int(&in, "y", -COORD_LIMIT, COORD_LIMIT, &y) < 0)
        return fail(r, TOOL_EINVAL, "gui_window", args,
                    "\"y\" must be an integer within +/-%d", COORD_LIMIT);
    if (f_int(&in, "width", 1, COORD_LIMIT, &ww) < 0)
        return fail(r, TOOL_EINVAL, "gui_window", args,
                    "\"width\" must be a positive integer number of pixels");
    if (f_int(&in, "height", 1, COORD_LIMIT, &wh) < 0)
        return fail(r, TOOL_EINVAL, "gui_window", args,
                    "\"height\" must be a positive integer number of pixels");

    /* The title is captured before the action, because "close" frees it. */
    char title[GUI_TITLE_MAX];
    size_t ti = 0;
    for (; w->title[ti] && ti < sizeof title - 1; ti++) title[ti] = w->title[ti];
    title[ti] = '\0';

    int done = 0;
    if (streq(act, "close")) {
        int crc = gui_window_close(id);
        if (crc == -2)
            return fail(r, TOOL_EINVAL, "gui_window", args,
                        "window %u (\"%s\") refused to close: the application "
                        "declined the request", (unsigned)id, title);
        gui_sync();
        tool_result_printf(r, "closed window id=%u \"%s\". %d window(s) remain.\n",
                           (unsigned)id, title, gui_window_count());
        trace_ret((long)gui_window_count(), "gui_window",
                  "action=close id=%u title=%s", (unsigned)id, title);
        return TOOL_OK;
    } else if (streq(act, "focus")) {
        done = gui_window_focus(id) == 0;
    } else if (streq(act, "raise")) {
        done = gui_window_raise(id) == 0;
    } else if (streq(act, "move")) {
        done = gui_window_move(id, (int32_t)x, (int32_t)y) == 0;
    } else if (streq(act, "resize")) {
        done = gui_window_resize(id, (int32_t)ww, (int32_t)wh) == 0;
    } else if (streq(act, "hide")) {
        done = gui_window_show(id, 0) == 0;
    } else if (streq(act, "show")) {
        done = gui_window_show(id, 1) == 0;
    } else {
        return fail(r, TOOL_EINVAL, "gui_window", args,
                    "unknown action \"%s\". Valid actions: focus, raise, move, "
                    "resize, hide, show, close", act);
    }
    (void)done;

    gui_sync();
    w = gui_window(id);
    tool_result_printf(r,
        "window id=%u \"%s\": %s -> now at %d,%d %dx%d, z=%d of %d, %s%s\n",
        (unsigned)id, title, act,
        (int)w->frame.x, (int)w->frame.y, (int)w->frame.w, (int)w->frame.h,
        gui_window_count() - 1, gui_window_count(),
        gui_focused() == id ? "focused" : "not focused",
        (w->flags & GUI_WIN_HIDDEN) ? ", hidden" : "");
    if (x != w->frame.x || y != w->frame.y ||
        ww != w->frame.w || wh != w->frame.h)
        tool_result_printf(r,
            "note: the values were clamped so the window stays a usable size "
            "and its title bar stays reachable on screen.\n");

    trace_ok("gui_window", "action=%s id=%u at=%d,%d size=%dx%d focus=%u",
             act, (unsigned)id, (int)w->frame.x, (int)w->frame.y,
             (int)w->frame.w, (int)w->frame.h, (unsigned)gui_focused());
    return TOOL_OK;
}

static const tool_t gui_window_tool = {
    .name = "gui_window",
    .description =
        "Act on one window: focus (which also raises it), raise, move, resize, "
        "hide, show, or close. Ids come from gui_list. Pixel values are clamped "
        "so a window is never left unreachable or unusably small, and the result "
        "says when that happened.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"integer\",\"description\":\"id from gui_list\"},"
        "\"action\":{\"type\":\"string\",\"enum\":[\"focus\",\"raise\",\"move\","
        "\"resize\",\"hide\",\"show\",\"close\"]},"
        "\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},"
        "\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"}"
        "},\"required\":[\"id\",\"action\"]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_gui_window,
};
REGISTER_TOOL(gui_window_tool);

/* ====================================================================== */
/* gui_click                                                              */
/* ====================================================================== */

/* Find a hit-testable widget whose text matches `label` exactly. Exact rather
 * than fuzzy: "1" must never match "12", and a model that has been told the
 * labels by gui_list has no reason to approximate.
 *
 * BUTTONS ARE PREFERRED, in a second pass, and that is not a nicety. A widget's
 * label is not unique: the moment a calculator's display reads "7", a request to
 * click "7" matches both the display and the key. The one the operator means is
 * always the control, so controls win. (The built-in calculator also marks its
 * display GUI_W_NOHIT, but an app written later may not, and a click on the
 * wrong widget is the kind of failure that looks like the app is broken.) */
static gui_widget_t *widget_by_label(gui_window_t *w, const char *label) {
    for (uint16_t i = 0; i < w->nwidgets; i++) {
        gui_widget_t *x = &w->widgets[i];
        if (x->state & (GUI_W_NOHIT | GUI_W_DISABLED)) continue;
        if (x->kind == GUI_BUTTON && streq(x->text, label)) return x;
    }
    for (uint16_t i = 0; i < w->nwidgets; i++) {
        gui_widget_t *x = &w->widgets[i];
        if (x->state & (GUI_W_NOHIT | GUI_W_DISABLED)) continue;
        if (streq(x->text, label)) return x;
    }
    return (gui_widget_t *)0;
}

static void put_labels(tool_result_t *r, gui_window_t *w) {
    tool_result_printf(r, "clickable labels in \"%s\":", w->title);
    int any = 0;
    for (uint16_t i = 0; i < w->nwidgets; i++) {
        gui_widget_t *x = &w->widgets[i];
        if (x->state & (GUI_W_NOHIT | GUI_W_DISABLED)) continue;
        if (r->len + TAIL_RESERVE + 24 >= r->cap) break;
        tool_result_printf(r, " \"%s\"", x->text);
        any = 1;
    }
    if (!any) tool_result_printf(r, " (none)");
}

static int t_gui_click(const tool_call_t *c, tool_result_t *r) {
    if (gui_ready(r, "gui_click") != 0) return TOOL_EINVAL;

    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "gui_click", "",
                    "input must be a JSON object, e.g. "
                    "{\"id\":3,\"label\":\"7\"}");

    int64_t id64 = 0, wid = 0, x = 0, y = 0;
    int has_id  = f_int(&in, "id", 1, 0x7FFFFFFF, &id64);
    int has_wid = f_int(&in, "widget", 0, 0x7FFFFFFF, &wid);
    int has_x   = f_int(&in, "x", -COORD_LIMIT, COORD_LIMIT, &x);
    int has_y   = f_int(&in, "y", -COORD_LIMIT, COORD_LIMIT, &y);
    if (has_id < 0 || has_wid < 0 || has_x < 0 || has_y < 0)
        return fail(r, TOOL_EINVAL, "gui_click", "",
                    "\"id\", \"widget\", \"x\" and \"y\" must all be integers "
                    "when present");

    char label[GUI_TEXT_MAX];
    int  has_label = f_str(&in, "label", label, sizeof label);
    if (has_label == -1)
        return fail(r, TOOL_EINVAL, "gui_click", "",
                    "\"label\" must be a string (a widget's visible text)");
    if (has_label == -2)
        return fail(r, TOOL_EINVAL, "gui_click", "",
                    "\"label\" is longer than any widget text (%d bytes max)",
                    (int)sizeof label - 1);
    if (has_label == -3)
        return fail(r, TOOL_EINVAL, "gui_click", "",
                    "\"label\" contains an embedded NUL character, so it cannot "
                    "be the visible text of any widget");

    /* Screen coordinates: no window needed, and the click goes wherever the
     * pointer would have gone — including onto a title bar or a close box. */
    if (!has_id && !has_label && !has_wid && has_x == 1 && has_y == 1) {
        /* This branch can DESTROY a window: the close box is chrome, so the only
         * route to it is a screen coordinate. Resolve and remember which window
         * is under the point BEFORE the press, so the one trace line this call
         * emits can name the window that was really hit and say whether it
         * survived. Without this the entire kernel record of closing every
         * window the operator was using is "[gui_click at=390,108 -> 1]", and
         * the result text says "a window took it" about a window that no longer
         * exists. gui_window action=close has named its target since it was
         * written; this path is the asymmetry. */
        uint32_t      before_id = gui_window_at((int32_t)x, (int32_t)y);
        gui_window_t *bw        = before_id ? gui_window(before_id) : (gui_window_t *)0;
        char          btitle[GUI_TITLE_MAX];
        size_t        ti = 0;
        if (bw) { for (; bw->title[ti] && ti < sizeof btitle - 1; ti++) btitle[ti] = bw->title[ti]; }
        btitle[ti] = '\0';

        int hit    = gui_click_at((int32_t)x, (int32_t)y);
        int closed = before_id && !gui_window(before_id);
        gui_sync();

        if (!before_id)
            tool_result_printf(r,
                "clicked at %d,%d - the desktop (no window is there, so nothing "
                "happened).\n", (int)x, (int)y);
        else if (closed)
            tool_result_printf(r,
                "clicked at %d,%d - that was window %u \"%s\", and THE CLICK "
                "CLOSED IT. The window and its app are gone; %d window(s) "
                "remain.\n", (int)x, (int)y, (unsigned)before_id, btitle,
                gui_window_count());
        else
            tool_result_printf(r,
                "clicked at %d,%d - window %u \"%s\" %s.\n", (int)x, (int)y,
                (unsigned)before_id, btitle,
                hit ? "took it" : "is there but nothing consumed the press");

        trace_ret((long)hit, "gui_click", "at=%d,%d window=%u title=%s%s",
                  (int)x, (int)y, (unsigned)before_id,
                  before_id ? btitle : "-", closed ? " CLOSED" : "");
        return TOOL_OK;
    }

    if (has_id != 1) {
        r->is_error = 1;
        tool_result_printf(r,
            "error: give \"id\" plus \"label\" (or \"widget\"), or \"x\" and "
            "\"y\" to click a screen position. ");
        put_open_ids(r);
        tool_result_printf(r, "\n");
        trace_err(TOOL_EINVAL, "gui_click", "id=?");
        return TOOL_EINVAL;
    }

    gui_window_t *w = gui_window((uint32_t)id64);
    if (!w) {
        r->is_error = 1;
        tool_result_printf(r, "error: no window with id %u. ", (unsigned)id64);
        put_open_ids(r);
        tool_result_printf(r, "\n");
        trace_err(TOOL_ENOENT, "gui_click", "id=%u", (unsigned)id64);
        return TOOL_ENOENT;
    }

    gui_widget_t *target = (gui_widget_t *)0;
    if (has_label == 1) target = widget_by_label(w, label);
    else if (has_wid == 1) {
        target = gui_widget_by_id(w, (uint32_t)wid);
        if (target && (target->state & (GUI_W_NOHIT | GUI_W_DISABLED)))
            target = (gui_widget_t *)0;
    } else if (has_x == 1 && has_y == 1) {
        /* Client-relative when a window was named: an app's coordinates do not
         * change when the operator drags its window. */
        target = gui_widget_at(w, (int32_t)x, (int32_t)y);
    }

    if (!target) {
        r->is_error = 1;
        tool_result_printf(r, "error: nothing clickable found in window %u for ",
                           (unsigned)id64);
        if (has_label == 1)     tool_result_printf(r, "label \"%s\". ", label);
        else if (has_wid == 1)  tool_result_printf(r, "widget id %u. ", (unsigned)wid);
        else                    tool_result_printf(r, "that position. ");
        put_labels(r, w);
        tool_result_printf(r, "\n");
        trace_err(TOOL_ENOENT, "gui_click", "id=%u label=%s", (unsigned)id64,
                  has_label == 1 ? label : "-");
        return TOOL_ENOENT;
    }

    char was[GUI_TEXT_MAX];
    size_t i = 0;
    for (; target->text[i] && i < sizeof was - 1; i++) was[i] = target->text[i];
    was[i] = '\0';

    gui_rect_t cl = gui_client_rect(w);
    int32_t    cx = cl.x + target->r.x + target->r.w / 2;
    int32_t    cy = cl.y + target->r.y + target->r.h / 2;

    /* A widget's rect is allowed to fall outside the client area - the
     * compositor clips it, so it simply is not drawn. Pressing its centre is a
     * different matter: gui_click_at() re-resolves the pixel to whatever window
     * is topmost there, so an escaped rect presses SOMEBODY ELSE'S widget and
     * runs their handler, while this result would happily say we pressed our
     * own. That needs no malice to reach: a button declared 400 wide inside a
     * 200-wide window does it, and so does resizing a window smaller after
     * launch (gui_window action=resize deliberately does not re-lay out). */
    if (!gui_rect_contains(w->frame, cx, cy)) {
        r->is_error = 1;
        tool_result_printf(r,
            "error: the %s labelled \"%s\" (widget id=%u) is not inside window "
            "%u. Its centre works out at screen %d,%d, and that window covers "
            "%d,%d to %d,%d - so the widget is off the window, invisible (the "
            "window manager clips to the client area) and unclickable. Nothing "
            "was pressed. Its rect is [%d,%d %dx%d] relative to a client area "
            "only %dx%d: give it a smaller rect, use grid row/col placement, or "
            "make the window bigger.\n",
            kindname(target->kind), was, (unsigned)target->id, (unsigned)id64,
            (int)cx, (int)cy,
            (int)w->frame.x, (int)w->frame.y,
            (int)(w->frame.x + w->frame.w - 1), (int)(w->frame.y + w->frame.h - 1),
            (int)target->r.x, (int)target->r.y, (int)target->r.w, (int)target->r.h,
            (int)cl.w, (int)cl.h);
        trace_err(TOOL_EINVAL, "gui_click", "id=%u widget=%u off-window at=%d,%d",
                  (unsigned)id64, (unsigned)target->id, (int)cx, (int)cy);
        return TOOL_EINVAL;
    }

    /* A zero-area widget is inside the frame but cannot be hit: the click lands
     * on the window, some other window chrome takes it, and the handler never
     * runs. Catch it here rather than reporting a press that did not happen. */
    if (target->r.w <= 0 || target->r.h <= 0) {
        r->is_error = 1;
        tool_result_printf(r,
            "error: the %s labelled \"%s\" (widget id=%u) in window %u has zero "
            "area (rect [%d,%d %dx%d]), so there is no pixel to press and "
            "nothing was clicked. This happens when a grid has more rows or "
            "columns than the window has room for. Relaunch the app with a "
            "taller or wider window, or fewer grid cells.\n",
            kindname(target->kind), was, (unsigned)target->id, (unsigned)id64,
            (int)target->r.x, (int)target->r.y, (int)target->r.w, (int)target->r.h);
        trace_err(TOOL_EINVAL, "gui_click", "id=%u widget=%u zero-area",
                  (unsigned)id64, (unsigned)target->id);
        return TOOL_EINVAL;
    }

    int hit = gui_click_at(cx, cy);
    gui_sync();

    tool_result_printf(r,
        "clicked the %s labelled \"%s\" (widget id=%u) in window %u at screen "
        "%d,%d.\n", kindname(target->kind), was, (unsigned)target->id,
        (unsigned)id64, (int)cx, (int)cy);
    if (!hit)
        tool_result_printf(r,
            "But NOTHING CONSUMED IT: the press reached the desktop, so no "
            "handler ran and nothing changed.\n");

    /* What the click actually did, read back out of the window rather than
     * asserted: this is the difference between "I pressed 7" and "the display
     * now reads 7". */
    gui_window_t *after = gui_window((uint32_t)id64);
    if (!after) {
        tool_result_printf(r, "The window closed as a result.\n");
    } else {
        tool_result_printf(r, "window now shows:");
        int shown = 0;
        for (uint16_t k = 0; k < after->nwidgets; k++) {
            gui_widget_t *x2 = &after->widgets[k];
            if (x2->kind != GUI_FIELD && x2->kind != GUI_LABEL) continue;
            if (!x2->text[0]) continue;
            if (r->len + TAIL_RESERVE + 64 >= r->cap) break;
            tool_result_printf(r, " %s=\"%s\"", kindname(x2->kind), x2->text);
            shown = 1;
        }
        if (!shown) tool_result_printf(r, " (no text widgets)");
        tool_result_printf(r, "\n");
    }

    trace_ret((long)hit, "gui_click", "id=%u widget=%u label=%s at=%d,%d",
              (unsigned)id64, (unsigned)target->id, was, (int)cx, (int)cy);
    return TOOL_OK;
}

static const tool_t gui_click_tool = {
    .name = "gui_click",
    .description =
        "Press a widget: the same press-and-release a real mouse performs, "
        "through the same dispatch path. Give the window \"id\" plus the widget's "
        "visible \"label\" (id=3 label=\"7\", then label=\"+\", then label=\"=\") "
        "or its \"widget\" id or client \"x\"/\"y\". With no id, \"x\"/\"y\" "
        "click a screen position, including a title bar or close box. The result "
        "lists every field and label afterwards, so you can see what changed.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"integer\",\"description\":\"id from gui_list\"},"
        "\"label\":{\"type\":\"string\",\"description\":\"visible text of the "
        "widget, matched exactly\"},"
        "\"widget\":{\"type\":\"integer\",\"description\":\"widget id instead of "
        "a label\"},"
        "\"x\":{\"type\":\"integer\",\"description\":\"client-relative with an "
        "id, screen-absolute without\"},"
        "\"y\":{\"type\":\"integer\"}"
        "},\"required\":[]}",
    .flags  = TOOL_MUTATES,
    .invoke = t_gui_click,
};
REGISTER_TOOL(gui_click_tool);
