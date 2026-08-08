/* screen_tools.c — the console tool family: the model reads and paints its own
 * screen.
 *
 * PURPOSE
 *   The console is a grid of cells, two bytes each (low = a slot in the console
 *   code page, high = a colour attribute). WHERE that grid lives and how big it is
 *   depends on the display the machine came up with: a linear-framebuffer console
 *   (lib/fb.c) keeps 128x48 cells in .bss and rasterises them, while the VGA text
 *   fallback is 80x25 cells at physical 0xB8000. Nothing in this file hardcodes
 *   either — base, rows and columns all come from lib/base.c at runtime. Two
 *   capabilities follow, and they are not symmetrical in interest:
 *
 *   read_screen is the strange one. The console is the only place this machine's
 *   history exists — there is no shell, no dmesg, no log file, and the model's
 *   own replies are printed and then forgotten by the turn loop. Reading the
 *   framebuffer gives the model its own scrollback: the driver banner it printed
 *   at boot, the kernel-emitted trace lines from three tool calls ago, its own
 *   previous answer. "Why did DNS take three tries?" stops being a question the
 *   model has to guess at and becomes a question it can look up, in the exact
 *   bytes the operator is looking at.
 *
 *   write_screen is the other direction: the model paints. A status bar, a
 *   header, a colour scheme of its own choosing, straight into the cells. The
 *   only UI this machine has is the one the model decides to draw.
 *
 *   screen_info is the map. mem_tools.c has mem_map for the same reason: a tool
 *   whose arguments are coordinates is unusable without ground truth about the
 *   coordinate space, and a constant in a schema description can rot while a
 *   value read at runtime cannot. It also reports the one genuinely dynamic
 *   fact — where the console cursor is right now — which is what decides whether
 *   a painted row will survive the next line of output. See below.
 *
 * ============================================================================
 * DECISION: painting is TRANSIENT, and the tool says so instead of pretending.
 * ============================================================================
 *   lib/base.c owns this framebuffer. It keeps a cursor, advances it per
 *   character, and scrolls the whole screen up one row when output runs off the
 *   bottom. So anything write_screen paints is living in someone else's buffer:
 *   the next kputs may overwrite it, and the next scroll will certainly move it.
 *
 *   Three designs were possible.
 *
 *   (a) Reserve a region: teach base.c that rows [0,N) are owned by the model
 *       and must not be scrolled or written. That is the "real" answer and it is
 *       also a rewrite of the console's scroll and cursor logic, i.e. a change to
 *       the one piece of code every other subsystem prints through, in service
 *       of a tool. The blast radius is wrong for the benefit.
 *
 *   (b) Save and restore the cursor around the paint. This sounds prudent and is
 *       actually meaningless here: write_screen addresses cells absolutely and
 *       never calls kputc, so it never moves the cursor and there is nothing to
 *       restore. Implementing it would be theatre.
 *
 *   (c) Do not touch console state at all, and make the tool honest about the
 *       consequence.
 *
 *   (c) is what this file does. write_screen pokes cells and moves nothing: not
 *       the software cursor, not the hardware CRTC cursor, not base.c's idea of
 *       where the next character goes. In exchange, every write_screen result
 *       ends with the truth — where the cursor is, how many more lines of kernel
 *       output fit before the screen scrolls, and therefore how long this paint
 *       can be expected to last. A model that paints a status bar on row 0 is
 *       told, in the same breath, that row 0 is the first row the next scroll
 *       destroys. That is strictly better than a tool that quietly succeeds and
 *       leaves the model to narrate a status bar that vanished two lines later.
 *
 *   The corollary for read_screen: it reads the framebuffer, not a history
 *   buffer. Anything that has already scrolled off the top is gone, and the tool
 *   description says exactly that so the model does not report an absence as a
 *   fact about the boot.
 *
 * ============================================================================
 * BOUNDS
 * ============================================================================
 *   Coordinates come from the model. The cell grid ends at base + rows*cols*2 and
 *   there is no paging protection over it, so a write past the end does not fault
 *   at all — it lands in whatever is next (adjacent .bss on the framebuffer path,
 *   the VGA registers' shadow and then plain RAM on the 0xB8000 path) and the
 *   machine misbehaves. The IDT (arch/x86_64/idt.c) is no help
 *   here: this range is mapped and writable, so there is no exception to catch.
 *   Bounds are the only defence there is. So:
 *
 *   - row and col are rejected, not clamped, when out of range: a rejection
 *     names the geometry and the model fixes it next turn, whereas a clamp
 *     silently paints somewhere the model did not ask for.
 *   - text longer than the row is CLIPPED at the last column and the result says
 *     how many characters were dropped. It is never wrapped: wrapping would
 *     write cells on a row the model never named, and wrapping off the bottom
 *     would have to clamp anyway.
 *   - the innermost loop is bounded by `cells`, computed as `cols - col` from
 *     two values already proven in range, so the cell index can never reach
 *     rows*cols. No pointer arithmetic is performed on an unvalidated number
 *     anywhere in this file.
 *   - the framebuffer base, the row count and the column count all come from
 *     lib/base.c (console_fb / console_geometry), not from constants duplicated
 *     here, so the bound cannot drift away from the console's own idea of the
 *     screen.
 *
 * ============================================================================
 * WHAT IS ACCEPTED AS TEXT, AND WHY IT IS THAT NARROW
 * ============================================================================
 *   Only printable ASCII, 0x20..0x7E.
 *
 *   Control characters are rejected rather than substituted. In a framebuffer
 *   there is no such thing as a newline: byte 0x0A is a cell like any other and
 *   renders as a code-page-437 glyph. Accepting "line one\nline two" would paint
 *   a musical note in the middle of a row. The rejection message tells the model
 *   the actual rule — one call per row — which is the thing it needs to know.
 *
 *   Bytes >= 0x80 are rejected for a related reason: json.h decodes strings to
 *   UTF-8, but the VGA font is code page 437. A box-drawing character the model
 *   sends as one Unicode code point arrives as two or three UTF-8 bytes and
 *   paints two or three unrelated glyphs. Refusing it with an explanation is
 *   more useful than painting mojibake and reporting success.
 *
 * UNTRUSTED INPUT
 *   `call->input` is model output and is read only through json.h. Types are
 *   checked before values, values before ranges, ranges before any memory is
 *   touched. Colours are looked up in a fixed name table — the model never
 *   supplies an attribute byte, because a nibble-packed integer is exactly the
 *   kind of argument a model gets subtly wrong (and "0x1F" as a background is
 *   indistinguishable from a mistake). Nothing here allocates.
 *
 * HOST TESTABILITY
 *   A host process cannot write to a real console. The grid base, geometry and
 *   cursor are reached through three small accessors that, under FABLEOS_HOSTTEST,
 *   resolve to values a test injects (screen_tools_set_framebuffer /
 *   screen_tools_set_cursor) instead of to lib/base.c. The bounds logic, the
 *   parsing, the clipping and the exact cell bytes are then all verifiable
 *   natively against a real array with canaries after it — see
 *   tests/host/test_screen_tools.c.
 */

#include "tool.h"
#include "trace.h"
#include "json.h"
#include "kernel.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>       /* vsnprintf, via port/stdio.h when freestanding */

/* On the host, Mach-O rejects a bare section name, so REGISTER_TOOL cannot
 * expand. Export a named pointer per tool instead; tests/host/test_screen_tools.c
 * stands in for the linker and builds the table from those. Kernel builds are
 * untouched. */
#ifdef FABLEOS_HOSTTEST
#undef  REGISTER_TOOL
#define REGISTER_TOOL(var) const tool_t *const fableos_hosttool_##var = &(var)
#endif

/* ====================================================================== */
/* the screen: base address, geometry, cursor                             */
/* ====================================================================== */

#ifdef FABLEOS_HOSTTEST

/* Injected by the test. NULL means "no screen", which every tool must refuse
 * legibly rather than dereference. */
static volatile uint16_t *host_fb;
static int host_rows, host_cols, host_cur_row, host_cur_col;

void screen_tools_set_framebuffer(volatile uint16_t *fb, int rows, int cols);
void screen_tools_set_framebuffer(volatile uint16_t *fb, int rows, int cols) {
    host_fb   = fb;
    host_rows = rows;
    host_cols = cols;
}

void screen_tools_set_cursor(int row, int col);
void screen_tools_set_cursor(int row, int col) {
    host_cur_row = row;
    host_cur_col = col;
}

static volatile uint16_t *screen_fb(void) { return host_fb; }
static void screen_geometry(int *rows, int *cols) {
    *rows = host_rows;
    *cols = host_cols;
}
static void screen_cursor(int *row, int *col) {
    *row = host_cur_row;
    *col = host_cur_col;
}

#else

/* One source of truth: lib/base.c owns the console, so it owns the address and
 * the geometry too. */
static volatile uint16_t *screen_fb(void) { return console_fb(); }
static void screen_geometry(int *rows, int *cols) { console_geometry(rows, cols); }
static void screen_cursor(int *row, int *col)     { console_cursor(row, col); }

#endif

/* Sanity floor/ceiling on whatever geometry we are handed. Both the kernel and
 * the host inject 25x80, but a tool that indexes memory should not take the
 * numbers that bound it on faith. */
#define GEOM_MAX_ROWS  100
#define GEOM_MAX_COLS  200

/* 0 when the screen is usable. */
static int screen_ok(int *rows, int *cols) {
    *rows = 0;
    *cols = 0;
    if (!screen_fb()) return -1;
    screen_geometry(rows, cols);
    if (*rows <= 0 || *cols <= 0) return -1;
    if (*rows > GEOM_MAX_ROWS || *cols > GEOM_MAX_COLS) return -1;
    return 0;
}

/* ====================================================================== */
/* colours                                                                */
/* ====================================================================== */

/* The attribute byte is bg<<4 | fg. Foreground uses all 16 colours. Background
 * uses the low 8 only: in standard VGA text mode the top attribute bit is the
 * blink enable, not a brightness bit, so a "yellow background" is a blinking
 * brown one. Rather than hand the model a colour that renders as something else,
 * bright backgrounds are refused by name. */
typedef struct { const char *name; uint8_t v; } colour_t;

static const colour_t kColours[] = {
    { "black",         0 },  { "blue",           1 },
    { "green",         2 },  { "cyan",           3 },
    { "red",           4 },  { "magenta",        5 },
    { "brown",         6 },  { "light_grey",     7 },
    { "dark_grey",     8 },  { "light_blue",     9 },
    { "light_green",  10 },  { "light_cyan",    11 },
    { "light_red",    12 },  { "light_magenta", 13 },
    { "yellow",       14 },  { "white",         15 },
    /* Spellings a model will reach for. Cheap, and each one is a whole class of
     * avoidable failure. */
    { "grey",          7 },  { "gray",           7 },
    { "light_gray",    7 },  { "dark_gray",      8 },
    { "orange",        6 },  { "purple",         5 },
};
#define NCOLOURS ((int)(sizeof kColours / sizeof kColours[0]))

/* The 16 canonical names, in value order, for help text. */
static const char kColourList[] =
    "black blue green cyan red magenta brown light_grey dark_grey light_blue "
    "light_green light_cyan light_red light_magenta yellow white";

#define COLOUR_NAME_MAX 24

static int colour_by_name(const char *s, uint8_t *out) {
    if (!s) return -1;
    for (int i = 0; i < NCOLOURS; i++) {
        const char *a = kColours[i].name;
        size_t      k = 0;
        for (;; k++) {
            if (a[k] != s[k]) break;
            if (a[k] == '\0') { *out = kColours[i].v; return 0; }
            if (k >= COLOUR_NAME_MAX) break;
        }
    }
    return -1;
}

/* Canonical name for a value, for results and trace lines. */
static const char *colour_name(uint8_t v) {
    for (int i = 0; i < 16; i++)
        if (kColours[i].v == (v & 0x0F)) return kColours[i].name;
    return "?";
}

#define FG_DEFAULT   7    /* light grey on black: what the console itself uses */
#define BG_DEFAULT   0

/* ====================================================================== */
/* small shared helpers                                                   */
/* ====================================================================== */

/* Keep this much of the result free so a row-by-row dump can always append its
 * "stopped early" note. */
#define TAIL_RESERVE 192

/* Emit the failure result *and* its trace line, discarding partial output so the
 * model never sees half an answer followed by an error. */
static int fail(tool_result_t *r, int err, const char *op, const char *args,
                const char *fmt, ...) {
    char    msg[288];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    r->is_error = 1;
    r->len      = 0;
    r->truncated = 0;
    if (r->buf && r->cap) r->buf[0] = '\0';
    tool_result_printf(r, "error: %s", msg);
    trace_err(err, op, "%s", args ? args : "");
    return err;
}

/* Parse the input span as a JSON object. An absent/empty span is treated as `{}`
 * so a tool whose arguments are all optional still works. */
static int parse_obj(const tool_call_t *c, json_value_t *obj) {
    const char *p = "{}";
    size_t      n = 2;
    if (c && c->input && c->input_len) { p = c->input; n = c->input_len; }
    if (json_parse(p, n, obj) != JSON_OK) return TOOL_EINVAL;
    if (obj->type != JSON_OBJECT) return TOOL_EINVAL;
    return TOOL_OK;
}

/* Field readers. 1 = present and valid, 0 = absent (or explicit null),
 * -1 = present but the wrong type, -2 = present but out of range / too long. */

static int f_int(const json_value_t *o, const char *key,
                 int64_t lo, int64_t hi, int64_t *out) {
    json_value_t v;
    int rc = json_get(o, key, &v);
    if (rc == JSON_ENOENT) return 0;
    if (rc != JSON_OK) return -1;
    if (v.type == JSON_NULL) return 0;
    if (v.type != JSON_NUMBER) return -1;
    /* json_int() truncates a fractional part toward zero, which for a screen
     * coordinate would silently turn 0.5 into "row 0" — an adjustment, not the
     * value asked for. Coordinates are rejected rather than adjusted, so a
     * non-integral literal is a type error here. */
    for (size_t i = 0; i < v.len; i++)
        if (v.start[i] == '.' || v.start[i] == 'e' || v.start[i] == 'E') return -1;
    int64_t x;
    if (json_int(&v, &x) != JSON_OK) return -2;    /* out of int64 range */
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

/* A colour argument: absent leaves *out alone. Returns 0 on success, else it has
 * already written the failure result. */
static int want_colour(const json_value_t *in, tool_result_t *r, const char *op,
                       const char *key, int background, uint8_t *out) {
    char s[COLOUR_NAME_MAX + 1];
    int  rc = f_str(in, key, s, sizeof s);
    if (rc == 0) return 0;
    if (rc == -1)
        return fail(r, TOOL_EINVAL, op, "",
                    "\"%s\" must be a colour name (a string), not a number or an "
                    "attribute byte. Valid names: %s", key, kColourList);
    if (rc == -2)
        return fail(r, TOOL_EINVAL, op, "",
                    "\"%s\" is not a colour name (too long). Valid names: %s",
                    key, kColourList);

    uint8_t v;
    if (colour_by_name(s, &v) != 0)
        return fail(r, TOOL_EINVAL, op, "",
                    "unknown colour \"%s\" for \"%s\". Valid names: %s", s, key,
                    kColourList);

    if (background && v > 7)
        return fail(r, TOOL_EINVAL, op, "",
                    "\"%s\" cannot be a bright colour (\"%s\"): in VGA text mode "
                    "the high background bit is the blink enable, so a bright "
                    "background blinks instead of being bright. Use one of the "
                    "eight dark colours: black blue green cyan red magenta brown "
                    "light_grey", key, s);

    *out = v;
    return 0;
}

/* How many more lines the kernel can print before the screen scrolls, given the
 * cursor. Also the number of rows of paint that are safe for that long. */
static int lines_until_scroll(int rows, int cur_row) {
    int n = (rows - 1) - cur_row;
    return n < 0 ? 0 : n;
}

/* The paragraph every write_screen success ends with. See the header comment:
 * the tool changes no console state, so the honest thing is to say what that
 * costs. */
static void put_transience(tool_result_t *r, int rows, int cur_row, int cur_col,
                           int painted_row) {
    int slack = lines_until_scroll(rows, cur_row);
    tool_result_printf(r,
        "note: this paint is transient. The kernel console cursor is at row %d, "
        "col %d and was NOT moved by this call. The next %d line(s) of kernel "
        "output fit below it; after that the console scrolls and every painted "
        "row moves up one row per line printed",
        cur_row, cur_col, slack);
    if (painted_row >= 0 && painted_row <= cur_row)
        tool_result_printf(r,
            ". Row %d is at or above the cursor, so the very next scroll shifts "
            "it (row 0 is discarded)", painted_row);
    else if (painted_row > cur_row)
        tool_result_printf(r,
            ". Row %d is below the cursor, so ordinary console output will "
            "overwrite it before any scroll does", painted_row);
    tool_result_printf(r, ". Repaint after printing, or call screen_info to see "
                          "where the cursor is now.\n");
}

/* ====================================================================== */
/* screen_info                                                            */
/* ====================================================================== */

static int t_screen_info(const tool_call_t *c, tool_result_t *r) {
    (void)c;                          /* no arguments: nothing to misparse */

    int rows, cols;
    if (screen_ok(&rows, &cols) != 0)
        return fail(r, TOOL_EINVAL, "screen_info", "",
                    "no text-mode framebuffer is available on this machine");

    int cur_row, cur_col;
    screen_cursor(&cur_row, &cur_col);

    volatile uint16_t *fb = screen_fb();

    /* How much of the screen currently has anything on it — the cheapest useful
     * summary, and it tells the model whether read_screen is worth calling. */
    int used = 0;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            uint8_t ch = (uint8_t)(fb[y * cols + x] & 0xFF);
            if (ch != ' ' && ch != 0) { used++; break; }
        }
    }

    tool_result_printf(r,
        "geometry: %d rows x %d columns, rows 0-%d top to bottom, columns 0-%d "
        "left to right\n"
        "framebuffer: %p, %d bytes, 2 bytes per cell (low byte = character, "
        "high byte = colour attribute = background<<4 | foreground)\n"
        "cursor: row %d, col %d - this is where the kernel prints next\n"
        "scroll: %d more line(s) of output fit before the console scrolls up; "
        "after that row 0 is discarded and everything moves up\n"
        "content: %d of %d rows are non-blank\n"
        "foreground colours: %s\n"
        "background colours: black blue green cyan red magenta brown light_grey "
        "(the 8 dark ones only - the bright background bit is VGA's blink enable)\n"
        "history: the framebuffer is the ONLY record of what this machine "
        "printed. Anything that has scrolled off the top is gone, and painting is "
        "transient because the console scrolls over it.\n",
        rows, cols, rows - 1, cols - 1,
        (void *)fb, rows * cols * 2,
        cur_row, cur_col,
        lines_until_scroll(rows, cur_row),
        used, rows,
        kColourList);

    trace_ok("screen_info", "geom=%dx%d cursor=%d,%d used_rows=%d",
             cols, rows, cur_row, cur_col, used);
    return TOOL_OK;
}

static const tool_t screen_info_tool = {
    .name = "screen_info",
    .description =
        "Describe this machine's text console before reading or painting it: its "
        "geometry (rows and columns, and which way they are numbered), the "
        "framebuffer address and cell layout, where the kernel's output cursor is "
        "right now, how many more lines of output fit before the screen scrolls, "
        "how many rows currently have anything on them, and the colour names "
        "write_screen accepts. Call this first if you are about to aim at "
        "coordinates - the cursor position in particular changes constantly and "
        "decides how long anything you paint will survive. Takes no arguments.",
    .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    .flags        = 0,
    .invoke       = t_screen_info,
};
REGISTER_TOOL(screen_info_tool);

/* ====================================================================== */
/* read_screen                                                            */
/* ====================================================================== */

static int t_read_screen(const tool_call_t *c, tool_result_t *r) {
    int rows, cols;
    if (screen_ok(&rows, &cols) != 0)
        return fail(r, TOOL_EINVAL, "read_screen", "",
                    "no text-mode framebuffer is available on this machine");

    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "read_screen", "",
                    "input must be a JSON object, e.g. {} or "
                    "{\"first_row\":0,\"last_row\":9}");

    int64_t first = 0, last = rows - 1;
    int     rc;

    rc = f_int(&in, "first_row", 0, rows - 1, &first);
    if (rc == -1) return fail(r, TOOL_EINVAL, "read_screen", "",
                              "\"first_row\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "read_screen", "",
                              "\"first_row\" is out of range: this screen has "
                              "rows 0-%d", rows - 1);

    rc = f_int(&in, "last_row", 0, rows - 1, &last);
    if (rc == -1) return fail(r, TOOL_EINVAL, "read_screen", "",
                              "\"last_row\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "read_screen", "",
                              "\"last_row\" is out of range: this screen has "
                              "rows 0-%d", rows - 1);

    if (last < first)
        return fail(r, TOOL_EINVAL, "read_screen", "",
                    "\"last_row\" (%d) is above \"first_row\" (%d); rows are "
                    "numbered downwards from 0 at the top",
                    (int)last, (int)first);

    int numbers = 1;
    if (f_bool(&in, "line_numbers", &numbers) < 0)
        return fail(r, TOOL_EINVAL, "read_screen", "",
                    "\"line_numbers\" must be a boolean");

    volatile uint16_t *fb = screen_fb();

    /* Find the last row in the range that has anything on it, so a screen with
     * 6 lines of boot log and 19 blank rows costs 6 lines of budget, not 25.
     * `blank_tail` is reported rather than hidden: a model must be able to tell
     * "nothing there" from "clipped". */
    int lastnb = (int)first - 1;
    for (int y = (int)first; y <= (int)last; y++) {
        for (int x = 0; x < cols; x++) {
            uint8_t ch = (uint8_t)(fb[y * cols + x] & 0xFF);
            if (ch != ' ' && ch != 0) { lastnb = y; break; }
        }
    }
    int blank_tail = (int)last - lastnb;

    tool_result_printf(r, "screen rows %d-%d of 0-%d (%d columns", (int)first,
                       (int)last, rows - 1, cols);
    if (numbers) tool_result_printf(r, ", \"NN|\" prefix is the row number");
    tool_result_printf(r, "):\n");

    /* -1, not 0: row 0 is a legal place to run out of room, and using 0 as the
     * "did not stop" sentinel meant a buffer that filled on the very first row
     * produced no resume hint and then fell through to the blank-tail branch,
     * reporting rows as blank when they had merely never been rendered. */
    int emitted = 0, stopped = -1;
    for (int y = (int)first; y <= lastnb; y++) {
        char line[GEOM_MAX_COLS + 1];
        int  n = 0;
        for (int x = 0; x < cols; x++) {
            uint8_t ch = (uint8_t)(fb[y * cols + x] & 0xFF);
            if (ch == 0) ch = ' ';                     /* never written yet   */
            else if (ch < 0x20 || ch > 0x7E) ch = '.'; /* CP437 glyph, no ASCII */
            line[n++] = (char)ch;
        }
        while (n > 0 && line[n - 1] == ' ') n--;       /* rstrip: the budget   */
        line[n] = '\0';

        /* Stop while there is still room to say that we stopped. */
        if (r->len + (size_t)n + 8 + TAIL_RESERVE >= r->cap) { stopped = y; break; }

        if (numbers) tool_result_printf(r, "%02d|%s\n", y, line);
        else         tool_result_printf(r, "%s\n", line);
        emitted++;
    }

    if (stopped >= 0)
        tool_result_printf(r,
            "[stopped at row %d: the result buffer is full. Call read_screen "
            "again with first_row=%d.]\n", stopped, stopped);
    else if (blank_tail > 0)
        tool_result_printf(r, "[rows %d-%d are blank]\n", lastnb + 1, (int)last);
    if (lastnb < (int)first)
        tool_result_printf(r, "[every row in this range is blank]\n");

    trace_ret((long)emitted, "read_screen", "rows=%d-%d blank_tail=%d bytes=%lu",
              (int)first, (int)last, blank_tail, (unsigned long)r->len);
    return TOOL_OK;
}

static const tool_t read_screen_tool = {
    .name = "read_screen",
    .description =
        "Read what is actually on this machine's console right now and return it "
        "as text, with the colour attribute bytes stripped. This is your own "
        "scrollback: the boot log, the kernel's bracketed trace lines, and your "
        "own earlier replies are all still in the framebuffer, so use this to "
        "answer questions about what happened earlier in this session instead of "
        "guessing. Trailing blank space on each row and trailing blank rows are "
        "removed. Rows are numbered from 0 at the top; pass first_row/last_row to "
        "read just a region. Note that the framebuffer is not a history buffer - "
        "anything that has already scrolled off the top is gone, not blank.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"first_row\":{\"type\":\"integer\",\"description\":\"first row to read, "
        "0 = the top line (default 0). Omit both bounds to read all of it.\"},"
        "\"last_row\":{\"type\":\"integer\",\"description\":\"last row to read, "
        "inclusive. Defaults to the BOTTOM row, which is the most recent output - "
        "leave it out rather than guessing, because guessing 24 on a 48-row screen "
        "reads the oldest half.\"},"
        "\"line_numbers\":{\"type\":\"boolean\",\"description\":\"prefix each "
        "line with its row number as \\\"NN|\\\" (default true). Keep it on if you "
        "intend to paint at coordinates afterwards.\"}"
        "},\"required\":[]}",
    .flags  = 0,
    .invoke = t_read_screen,
};
REGISTER_TOOL(read_screen_tool);

/* ====================================================================== */
/* write_screen                                                           */
/* ====================================================================== */

/* Longest text we will even decode. One row is at most GEOM_MAX_COLS cells, so
 * anything longer is going to be clipped anyway — but it is decoded in full
 * first so the result can honestly say how much was dropped. */
#define TEXT_MAX 512

static int t_write_screen(const tool_call_t *c, tool_result_t *r) {
    int rows, cols;
    if (screen_ok(&rows, &cols) != 0)
        return fail(r, TOOL_EINVAL, "write_screen", "",
                    "no text-mode framebuffer is available on this machine");

    json_value_t in;
    if (parse_obj(c, &in) != TOOL_OK)
        return fail(r, TOOL_EINVAL, "write_screen", "",
                    "input must be a JSON object, e.g. "
                    "{\"row\":0,\"col\":0,\"text\":\"hello\",\"fg\":\"white\","
                    "\"bg\":\"blue\"}");

    /* ---- text (required) ----
     * Decoded with the length out-parameter rather than through f_str, because a
     * JSON string may legally contain an escaped NUL, and strlen() would stop
     * there, so the validation below would never see the rest of the string. */
    json_value_t tv;
    int          jrc = json_get(&in, "text", &tv);
    if (jrc == JSON_ENOENT || (jrc == JSON_OK && tv.type == JSON_NULL))
        return fail(r, TOOL_EINVAL, "write_screen", "",
                    "\"text\" is required (the characters to paint)");
    if (jrc != JSON_OK || tv.type != JSON_STRING)
        return fail(r, TOOL_EINVAL, "write_screen", "", "\"text\" must be a string");

    char   text[TEXT_MAX + 1];
    size_t tlen = 0;
    int    drc  = json_str(&tv, text, sizeof text, &tlen);
    if (drc == JSON_ENOSPC)
        return fail(r, TOOL_EINVAL, "write_screen", "",
                    "\"text\" is longer than %d characters; one row of this "
                    "screen holds %d", TEXT_MAX, cols);
    if (drc != JSON_OK)
        return fail(r, TOOL_EINVAL, "write_screen", "",
                    "\"text\" could not be decoded as a string");

    /* Validate every byte before touching a single cell. */
    for (size_t i = 0; i < tlen; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch >= 0x20 && ch <= 0x7E) continue;
        if (ch < 0x20 || ch == 0x7F)
            return fail(r, TOOL_EINVAL, "write_screen", "",
                        "\"text\" contains control character 0x%02x at offset "
                        "%lu. The framebuffer has no newlines or tabs - every "
                        "byte becomes one cell on one row. Paint one row per "
                        "call, changing \"row\" each time",
                        (unsigned)ch, (unsigned long)i);
        return fail(r, TOOL_EINVAL, "write_screen", "",
                    "\"text\" contains non-ASCII byte 0x%02x at offset %lu. The "
                    "VGA font is code page 437, not Unicode, so a box-drawing or "
                    "block character arrives as several unrelated glyphs. Use "
                    "printable ASCII (0x20-0x7E) - '-' '=' '|' '+' '*' '#' draw "
                    "well",
                    (unsigned)ch, (unsigned long)i);
    }

    /* ---- position ---- */
    int64_t row = 0, col = 0;
    int rc = f_int(&in, "row", 0, rows - 1, &row);
    if (rc == 0)  return fail(r, TOOL_EINVAL, "write_screen", "",
                              "\"row\" is required (0 = the top line, %d = the "
                              "bottom)", rows - 1);
    if (rc == -1) return fail(r, TOOL_EINVAL, "write_screen", "",
                              "\"row\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "write_screen", "",
                              "\"row\" is outside the screen: this console has "
                              "rows 0-%d (top to bottom). Nothing was painted",
                              rows - 1);

    rc = f_int(&in, "col", 0, cols - 1, &col);
    if (rc == -1) return fail(r, TOOL_EINVAL, "write_screen", "",
                              "\"col\" must be an integer");
    if (rc == -2) return fail(r, TOOL_EINVAL, "write_screen", "",
                              "\"col\" is outside the screen: this console has "
                              "columns 0-%d (left to right). Nothing was painted",
                              cols - 1);

    /* ---- THE COLUMN-ZERO BRACKET RULE (trace.h's invariant, defended here) ----
     *
     * A kernel trace line is a '[' in column zero. lib/trace.c escapes brackets
     * inside every argument, and net/chat.c space-prefixes model prose, both so
     * that no model-authored byte can ever open one. This tool is the fourth
     * path to those same cells: screen_fb() IS console_fb(), and lib/base.c
     * rasterises what we poke with the same font and the same palette as real
     * kernel output. A painted "[vfs_write /etc/shadow bytes=9999 -> ok]" at
     * col 0 is not similar to a trace line, it is byte-identical to one.
     *
     * So refuse it, rather than shifting it silently: a status bar that begins
     * with '[' is a real (if rare) thing to want, and a tool that quietly moved
     * the operator's text one column right would be lying about its output in a
     * smaller way. The message names the one-column fix, so attempt two lands.
     * Only column zero is restricted; brackets anywhere else are untouched. */
    if (col == 0 && tlen > 0 && text[0] == '[')
        return fail(r, TOOL_EINVAL, "write_screen", "",
                    "\"text\" starts with '[' and \"col\" is 0. A '[' in column "
                    "zero is this kernel's mark for a line IT printed about what "
                    "it really did, so text painted there would be "
                    "indistinguishable from kernel ground truth. Nothing was "
                    "painted. Paint from col 1 (or start the text with any other "
                    "character); brackets are fine at any other column");

    /* Painting at column zero over a row that currently begins with '[' erases
     * a kernel line. That is legitimate - the console is scratch space and it
     * scrolls - but it must not be silent, so the trace line below carries it.
     * Read before anything is written. */
    int over_trace = 0;
    if (col == 0) {
        volatile uint16_t *peek = screen_fb();
        over_trace = ((uint8_t)(peek[(size_t)row * (size_t)cols] & 0xFF) == '[');
    }

    /* ---- colours ---- */
    uint8_t fg = FG_DEFAULT, bg = BG_DEFAULT;
    if (want_colour(&in, r, "write_screen", "fg", 0, &fg) != 0) return TOOL_EINVAL;
    if (want_colour(&in, r, "write_screen", "bg", 1, &bg) != 0) return TOOL_EINVAL;

    int fill = 0;
    if (f_bool(&in, "fill_row", &fill) < 0)
        return fail(r, TOOL_EINVAL, "write_screen", "",
                    "\"fill_row\" must be a boolean");

    /* ---- the bound. `room` is a subtraction of two in-range values, so the
     * highest cell index below is (row * cols) + (cols - 1) = rows*cols - 1 in
     * the worst case. Nothing past the framebuffer is reachable from here. ---- */
    size_t room    = (size_t)(cols - (int)col);
    size_t painted = tlen < room ? tlen : room;
    size_t dropped = tlen - painted;

    /* High byte = bg<<4 | fg; low byte is the character. */
    uint16_t attr = (uint16_t)(((((uint16_t)bg & 0x0F) << 4) |
                                 ((uint16_t)fg & 0x0F)) << 8);

    volatile uint16_t *fb   = screen_fb();
    size_t             base = (size_t)row * (size_t)cols + (size_t)col;

    for (size_t i = 0; i < painted; i++)
        fb[base + i] = (uint16_t)(attr | (uint8_t)text[i]);

    size_t filled = 0;
    if (fill) {
        for (size_t i = painted; i < room; i++)
            fb[base + i] = (uint16_t)(attr | (uint8_t)' ');
        filled = room - painted;
    }

    int cur_row, cur_col;
    screen_cursor(&cur_row, &cur_col);

    tool_result_printf(r,
        "painted %lu character(s) at row %d, col %d (fg=%s bg=%s, attribute byte "
        "0x%02x)",
        (unsigned long)painted, (int)row, (int)col,
        colour_name(fg), colour_name(bg),
        (unsigned)((bg << 4) | fg));
    if (filled)
        tool_result_printf(r, "; filled the remaining %lu column(s) of the row "
                              "with the same colours", (unsigned long)filled);
    tool_result_printf(r, ".\n");

    if (dropped)
        tool_result_printf(r,
            "CLIPPED: %lu character(s) did not fit and were dropped. Column %d "
            "plus %lu characters runs past column %d; text is never wrapped onto "
            "the next row.\n",
            (unsigned long)dropped, (int)col, (unsigned long)tlen, cols - 1);

    put_transience(r, rows, cur_row, cur_col, (int)row);

    if (over_trace)
        tool_result_printf(r,
            "NOTE: row %d began with '[', so this paint covered a line the "
            "kernel had printed about what it really did. The transcript records "
            "that it was overwritten.\n", (int)row);

    trace_ret((long)painted, "write_screen",
              "row=%d col=%d len=%lu fg=%s bg=%s attr=0x%02x fill=%d clipped=%lu"
              "%s",
              (int)row, (int)col, (unsigned long)tlen,
              colour_name(fg), colour_name(bg),
              (unsigned)((bg << 4) | fg), fill, (unsigned long)dropped,
              over_trace ? " erased-kernel-line=1" : "");
    return TOOL_OK;
}

static const tool_t write_screen_tool = {
    .name = "write_screen",
    .description =
        "Paint text directly into the console framebuffer at a given row and "
        "column, in a chosen foreground and background colour. This is how you "
        "give this machine a user interface: a status bar, a header, a coloured "
        "banner. Row 0 is the top, column 0 the left, and screen_info reports how "
        "many of each this display has (128x48 on the framebuffer console, 80x25 "
        "on the VGA fallback). Text is clipped at the right edge, never wrapped, and "
        "out-of-range coordinates are refused rather than adjusted. Only "
        "printable ASCII is accepted. IMPORTANT: this does not move the kernel's "
        "output cursor and does not reserve the area, so anything you paint is "
        "transient - normal console output will overwrite it and scrolling will "
        "move it upwards and eventually off the top. The result tells you exactly "
        "how many lines of output the paint has left; screen_info reports the "
        "cursor at any time.",
    .input_schema =
        "{\"type\":\"object\",\"properties\":{"
        "\"row\":{\"type\":\"integer\",\"description\":\"row to paint on, 0 = top. "
        "Out of range is refused with the real extents.\"},"
        "\"col\":{\"type\":\"integer\",\"description\":\"starting column, 0 = "
        "leftmost (default 0).\"},"
        "\"text\":{\"type\":\"string\",\"description\":\"printable ASCII to paint "
        "(0x20-0x7E). No newlines or tabs: one call paints one row. Anything past the "
        "last column is clipped. Text starting with '[' is refused at col 0 only - "
        "that position is reserved for the kernel's own [...] lines.\"},"
        "\"fg\":{\"type\":\"string\",\"description\":\"text colour, one of: black "
        "blue green cyan red magenta brown light_grey dark_grey light_blue "
        "light_green light_cyan light_red light_magenta yellow white (default "
        "light_grey, which is what the console itself uses).\"},"
        "\"bg\":{\"type\":\"string\",\"description\":\"background colour, one of "
        "the eight dark colours only: black blue green cyan red magenta brown "
        "light_grey (default black). Bright backgrounds are refused because that "
        "attribute bit is VGA's blink enable.\"},"
        "\"fill_row\":{\"type\":\"boolean\",\"description\":\"also paint the rest "
        "of the row, from the end of the text to the last column, in the same colours. "
        "Use this for a status bar so the background runs edge to edge (default "
        "false).\"}"
        "},\"required\":[\"row\",\"text\"]}",
    /* Really changes machine state: these bytes are the display. */
    .flags  = TOOL_MUTATES,
    .invoke = t_write_screen,
};
REGISTER_TOOL(write_screen_tool);
