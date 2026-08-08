/* agenda.c — the machine acting without being asked.
 *
 * See include/agenda.h for the argument. This file is the bookkeeping: a fixed
 * table, a clock comparison, one tool dispatch, and a great deal of care about
 * bounds and evidence. Every interesting decision is delegated to something that
 * already exists — fault.h guards the action, core/tool.c performs it, lib/trace.c
 * records it, net/json.c parses the store.
 *
 * FILE LAYOUT
 *   names and errors            the enum -> word tables
 *   validation                  a candidate item -> accepted or a sentence
 *   the store                   render / parse / read / write, through the VFS
 *   running one item            the guard, the dispatch, the accounting
 *   the tick                    which item is due, and the fairness rule
 *   introspection               what the tools and the boot log print
 *
 * ONE INVARIANT WORTH STATING UP FRONT: nothing in this file runs inside an
 * exception handler, and nothing in it is reentrant. g_running is the latch that
 * makes both of those checkable rather than aspirational — an agenda action that
 * somehow reached agenda_tick() again would otherwise be able to nest actions
 * until the stack ran out.
 */

#include "agenda.h"

/* For CHAT_EREENTER: the say sink is chat_ask() on this machine, and telling a
 * "turn already running" apart from a real failure is what stops the agenda
 * reporting a transport condition as disk corruption. */
#include "chat.h"

#include "fault.h"
#include "fs.h"
#include "json.h"
#include "kernel.h"
#include "tool.h"
#include "trace.h"
#include "vfs.h"

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>      /* vsnprintf — the libc shim's, when freestanding */

/* ====================================================================== */
/* small helpers                                                          */
/* ====================================================================== */

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

static void zero(void *p, size_t n) {
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static size_t slen(const char *s) {
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

/* TWO COPIES, AND THE DIFFERENCE BETWEEN THEM IS A BUG THAT SHIPPED FOR AN HOUR.
 *
 * Both bound the length and both strip control characters. Per include/trace.h a
 * kernel statement is a '[' in COLUMN ZERO, and nothing the model authored may
 * ever put one there.
 *
 * AN EARLIER VERSION OF THIS COMMENT GOT THE THREAT WRONG, and the wrong version
 * was load-bearing. It said model text "can only reach column zero by carrying its
 * own newline", because every line this module prints starts with kernel-authored
 * text. That is false, and net/chat.c had already written down why: WRAP is a third
 * way to reach column zero, with no newline involved at all. The framebuffer
 * console is 128 columns and lib/base.c resets the column to 0 when it fills one,
 * so a long enough sentence simply continues at column zero of the next row — and
 * AGENDA_ARG_MAX is 288, more than twice the width.
 *
 * That was live. kernel/main.c's sink prints an AGENDA_DO_SAY sentence with a
 * 60-character prefix, so 68 characters of filler put the model's next byte exactly
 * at column 0 of the following row. A boot item saying
 * "xxx...[vfs_write /etc/shadow bytes=9999 -> ok]" rendered a byte-exact forged
 * kernel trace line, in column zero, sitting between two genuine ones.
 *
 * They differ on brackets, and that is the whole point:
 *
 *   copy_text     for PROSE — a note, the first line of a tool result. Brackets
 *                 become parentheses. Belt and braces on top of trace.c, and
 *                 nothing is lost because nobody parses prose.
 *   copy_payload  for the ARGUMENT, which is a JSON document that a tool will
 *                 parse. '[' and ']' are syntax there. Translating them turns
 *                 {"args":[21]} into {"args":(21)}, which then fails validation
 *                 as "not one JSON object" — and it fails at SAVE time, so the
 *                 operator is told the arguments are malformed when they were
 *                 not, with no hint as to why.
 *
 * That is not hypothetical. It is exactly what happened on the first live run of
 * this module: the model saved a capability, went to schedule a boot item calling
 * it with args [21], was refused six times, correctly isolated the failure to
 * "an array inside arg", concluded the interface could not express what it
 * needed, and — to its credit — left the item DISABLED and reported honestly
 * that it had not finished. A defensive transformation applied one field too
 * widely cost a whole session and looked exactly like a model limitation.
 *
 * SO WHICH ONE THE "arg" FIELD GETS DEPENDS ON THE ACT, because that field is two
 * different things. For AGENDA_DO_TOOL it is a JSON document a tool will parse, and
 * brackets are syntax. For AGENDA_DO_SAY it is a SENTENCE — prose, nobody parses
 * it, and it is printed raw on the operator's console. arg_copy() below makes that
 * choice in the one place both the dictation path and the store-loading path go
 * through, so a sentence cannot arrive pre-armed from a disk either. */
static void copy_text(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c < 0x20 || c == 0x7F) c = ' ';
            else if (c == '[')         c = '(';
            else if (c == ']')         c = ')';
            dst[i] = (char)c;
        }
    }
    dst[i] = '\0';
}

static void copy_payload(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) {
            unsigned char c = (unsigned char)src[i];
            /* Control characters only. A JSON document has no business carrying
             * one outside a string literal, and inside one it must be escaped
             * anyway, so replacing it with a space cannot damage a legal
             * argument.
             *
             * Brackets survive here, so this text must never be printed raw at
             * column zero. It is not: a tool argument reaches the console only
             * through a tool result, which net/chat.c prints through its own
             * column-zero guard, and through lib/trace.c, which escapes both
             * bracket characters in every field it renders. A SENTENCE is the
             * thing that gets printed raw, and a sentence goes through
             * copy_text() instead — see arg_copy(). */
            if (c < 0x20 || c == 0x7F) c = ' ';
            dst[i] = (char)c;
        }
    }
    dst[i] = '\0';
}

/* The "arg" field, copied according to what it IS. See the note above. */
static void arg_copy(char *dst, size_t cap, const char *src, int act) {
    if (act == AGENDA_DO_TOOL) copy_payload(dst, cap, src);
    else                       copy_text(dst, cap, src);
}

/* ====================================================================== */
/* names and errors                                                       */
/* ====================================================================== */

static const char *const when_tab[AGENDA_WHEN_COUNT] = { "boot", "every", "once" };
static const char *const act_tab[AGENDA_DO_COUNT]    = { "tool", "say" };

const char *agenda_when_name(int when) {
    if (when < 0 || when >= AGENDA_WHEN_COUNT) return "?";
    return when_tab[when];
}

const char *agenda_act_name(int act) {
    if (act < 0 || act >= AGENDA_DO_COUNT) return "?";
    return act_tab[act];
}

const char *agenda_strerror(int rc) {
    switch (rc) {
        case AGENDA_OK:       return "ok";
        case AGENDA_EINVAL:   return "the item is malformed";
        case AGENDA_ENOENT:   return "there is no agenda item with that name";
        case AGENDA_ENOSPC:   return "the agenda is full";
        case AGENDA_EEXIST:   return "an item of that name already exists";
        case AGENDA_EIO:      return "the agenda store could not be read or "
                                     "written";
        case AGENDA_EBUSY:    return "an autonomous action is already running";
        case AGENDA_EOFF:     return "the agenda is switched off";
        case AGENDA_ENOTOOL:  return "no tool of that name is registered in this "
                                     "build";
        case AGENDA_ENOSAY:   return "nothing is bound to say a sentence to, so "
                                     "a \"say\" item cannot run";
        case AGENDA_ESAY:     return "the sentence was started but the turn did "
                                     "not complete; this is the model transport, "
                                     "not the store";
        case AGENDA_EFAULT:   return "the action took a fatal CPU exception and "
                                     "its call chain was abandoned";
        case AGENDA_ERETIRED: return "the item is disabled or has used up its "
                                     "runs for this boot";
        case AGENDA_EDENIED:  return "that tool succeeds by ending the boot, so "
                                     "it cannot be scheduled unattended";
        default:              return "unknown agenda result";
    }
}

/* ====================================================================== */
/* state                                                                  */
/* ====================================================================== */

static agenda_item_t g_item[AGENDA_MAX_ITEMS];
static int           g_n;

static int      g_on = 1;
static int      g_running;          /* the re-entrancy latch                  */
static uint32_t g_runs, g_fails, g_escapes;

static int (*g_say)(const char *sentence);

static const char *g_store = AGENDA_STORE_RAM;
static int         g_persist;
static int         g_store_chosen;

static char g_file[AGENDA_FILE_MAX];

void agenda_bind_say(int (*say)(const char *sentence)) { g_say = say; }
void agenda_enable(int on) { g_on = on ? 1 : 0; }
int  agenda_enabled(void)  { return g_on; }

int  agenda_count(void) { return g_n; }

const agenda_item_t *agenda_at(int index) {
    if (index < 0 || index >= g_n) return NULL;
    return &g_item[index];
}

const agenda_item_t *agenda_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_n; i++)
        if (streq(g_item[i].name, name)) return &g_item[i];
    return NULL;
}

static agenda_item_t *find_mut(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_n; i++)
        if (streq(g_item[i].name, name)) return &g_item[i];
    return NULL;
}

uint32_t agenda_runs(void)     { return g_runs; }
uint32_t agenda_failures(void) { return g_fails; }
uint32_t agenda_escapes(void)  { return g_escapes; }

void agenda_reset(void) {
    zero(g_item, sizeof g_item);
    g_n       = 0;
    g_running = 0;
    g_runs    = 0;
    g_fails   = 0;
    g_escapes = 0;
    g_on      = 1;
}

/* ====================================================================== */
/* the store's location                                                   */
/* ====================================================================== */

/* Probed through the VFS rather than compiled in, for the reason
 * core/capability.c gives: whether a disk exists is a fact about this boot, and
 * a machine that reports "saved" when it wrote to a RAM filesystem has told the
 * operator something false about tomorrow. */
static void choose_store(void) {
    vfs_stat_t st;
    if (g_store_chosen) return;
    g_store_chosen = 1;
    if (vfs_stat(FS_DISK_MOUNT, &st) == VFS_OK && st.type == VNODE_DIR) {
        g_store   = AGENDA_STORE_DISK;
        g_persist = 1;
    } else {
        g_store   = AGENDA_STORE_RAM;
        g_persist = 0;
    }
}

const char *agenda_store(void)  { choose_store(); return g_store; }
int         agenda_persistent(void) { choose_store(); return g_persist; }

/* ====================================================================== */
/* validation                                                             */
/* ====================================================================== */

/* A name a human can say and type back: lowercase, digits, '_' and '-'. The same
 * rule core/capability.c applies to a capability name, and for the same reason —
 * these names are spoken aloud, echoed in trace lines and used as map keys, and
 * "Audio Boot!" is three different strings by the time it has been through all
 * three. */
static int name_ok(const char *s) {
    size_t n = slen(s);
    if (n == 0 || n >= AGENDA_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        if (!ok) return 0;
    }
    if (s[0] == '-') return 0;
    return 1;
}

/* THE SEVENTH BOUND, AND IT IS NOT LIKE THE OTHER SIX.
 *
 * The six bounds in agenda.h all bound REPETITION or COST: how often, how many
 * times, how many failures. Not one of them protects against a single action that
 * SUCCEEDS and, in succeeding, ends the boot.
 *
 * power_off and power_reboot are ordinary registered tools. Their only guard is
 * {"confirm":true}, which an agenda item supplies as a literal argument like any
 * other. agenda_bringup() runs when=boot items BEFORE the banner and the "> "
 * prompt are printed, and the store survives a power cycle. So one agenda_save
 * call naming power_off at boot makes the machine power itself off during every
 * boot, for ever — and because there is no shell and no prompt, there is no way
 * to reach agenda_control from inside the machine to undo it. Recovery means
 * detaching the disk and editing it on another computer, which is outside the
 * machine whose only interface is a sentence. With when=every and power_reboot it
 * is an unbroken reboot loop with no window at all.
 *
 * That was reproduced on a real disk image, twice in a row: prompts=0, no
 * framebuffer, the log ending at "power: drivers suspended". It is the one defect
 * on this branch that cannot be recovered from by talking to the machine.
 *
 * So an agenda item may not name a tool whose success ends the boot. Refused at
 * validate() — which the STORE LOADER also calls — so a hand-planted or
 * foreign-build store is disarmed on the way in and named as it is dropped, not
 * merely refused at dictation time.
 *
 * This is a name list and not a tool flag because include/tool.h is not this
 * module's to change; the right shape is a TOOL_ENDS_THE_BOOT flag beside
 * TOOL_MUTATES, set by the tool itself, and that is reported rather than done.
 * A name list rots silently if a tool is renamed, so
 * test_every_denied_tool_still_exists() asserts every name here is really
 * registered — a rename then fails a test instead of quietly re-arming the brick.
 *
 * NOT a general "dangerous tool" list. vfs_delete, fault_patch and
 * driver_install can all do damage, and all of it is damage the machine is still
 * running to talk about afterwards. The line is exactly "the boot does not
 * survive this succeeding", because that is the line past which no error message
 * can reach anybody. A scheduled shutdown is a real thing to want; ask for it in
 * a sentence, while somebody is there. */
static const char *const deny_tab[] = { "power_off", "power_reboot" };

const char *agenda_denied_tool(int i) {
    if (i < 0 || (size_t)i >= sizeof deny_tab / sizeof deny_tab[0]) return NULL;
    return deny_tab[i];
}

static int ends_the_boot(const char *tool) {
    for (size_t i = 0; i < sizeof deny_tab / sizeof deny_tab[0]; i++)
        if (streq(tool, deny_tab[i])) return 1;
    return 0;
}

static int validate(const agenda_item_t *in, char *why, size_t whycap) {
    size_t off = 0;
    if (whycap) why[0] = '\0';

    if (!in) {
        ap(why, whycap, &off, "no item was described");
        return AGENDA_EINVAL;
    }
    if (!name_ok(in->name)) {
        ap(why, whycap, &off,
           "\"%s\" is not a usable name: 1 to %u characters of lowercase "
           "letters, digits, '_' and '-', not starting with '-'",
           in->name[0] ? in->name : "(empty)",
           (unsigned)(AGENDA_NAME_MAX - 1));
        return AGENDA_EINVAL;
    }
    if (in->when >= AGENDA_WHEN_COUNT) {
        ap(why, whycap, &off, "\"when\" must be boot, every or once");
        return AGENDA_EINVAL;
    }
    if (in->act >= AGENDA_DO_COUNT) {
        ap(why, whycap, &off, "the action must be a tool call or a sentence");
        return AGENDA_EINVAL;
    }
    if (in->when == AGENDA_WHEN_EVERY) {
        if (in->period_ms < AGENDA_MIN_PERIOD_MS) {
            ap(why, whycap, &off,
               "an \"every\" item may not fire more often than every %u ms; %u "
               "was asked for. This floor is not about speed: an unattended "
               "action that repeats faster than the console can be read is "
               "indistinguishable from a runaway loop, and a sentence costs an "
               "API round trip every time",
               (unsigned)AGENDA_MIN_PERIOD_MS, (unsigned)in->period_ms);
            return AGENDA_EINVAL;
        }
    }
    if (in->max_runs == 0 || in->max_runs > AGENDA_MAX_RUNS) {
        ap(why, whycap, &off,
           "\"max_runs\" must be 1..%u; %u was asked for",
           (unsigned)AGENDA_MAX_RUNS, (unsigned)in->max_runs);
        return AGENDA_EINVAL;
    }
    if (slen(in->arg) == 0) {
        ap(why, whycap, &off,
           in->act == AGENDA_DO_TOOL
               ? "a tool item needs its arguments, as a JSON object - \"{}\" if "
                 "the tool takes none"
               : "a sentence item needs the sentence");
        return AGENDA_EINVAL;
    }

    if (in->act == AGENDA_DO_TOOL) {
        if (slen(in->tool) == 0) {
            ap(why, whycap, &off, "a tool item needs the tool's name");
            return AGENDA_EINVAL;
        }
        /* THE CHECK THAT EARNS THIS FUNCTION ITS PLACE. An item naming a tool
         * this build does not have would fail at boot, unattended, with nobody
         * to read the message. Refusing it now means the failure lands while
         * somebody is present, and the refusal can name what does exist. */
        if (!tool_find(in->tool)) {
            ap(why, whycap, &off,
               "no tool called \"%s\" is registered in this build, so this item "
               "would fail at every boot with nobody there to read it. %d tools "
               "exist; name one of them",
               in->tool, tool_count());
            return AGENDA_ENOTOOL;
        }
        /* See ends_the_boot() above: this is the bound that stops one successful
         * unattended action from making the machine unbootable and unreachable. */
        if (ends_the_boot(in->tool)) {
            /* This sentence is deliberately short enough to survive
             * AGENDA_WHY_MAX intact. A refusal truncated mid-clause is a refusal
             * whose advice never arrives; the tool layer prints `why` verbatim and
             * nothing else. The full argument is in the comment above. */
            ap(why, whycap, &off,
               "\"%s\" cannot be scheduled: succeeding ends the boot, and a boot "
               "item runs before the prompt, so nothing could undo it", in->tool);
            return AGENDA_EDENIED;
        }
        json_value_t v;
        if (json_parse(in->arg, slen(in->arg), &v) != JSON_OK ||
            v.type != JSON_OBJECT) {
            ap(why, whycap, &off,
               "the arguments must be one JSON object, e.g. {} or "
               "{\"name\":\"...\"}; this is not");
            return AGENDA_EINVAL;
        }
    }

    return AGENDA_OK;
}

/* ====================================================================== */
/* the store                                                              */
/* ====================================================================== */

/* JSON string emitter. Only the five escapes a name, a note, a sentence or a
 * tool-argument document can legitimately contain — everything else has already
 * been neutralised by copy_text() on the way into the table, so this cannot meet
 * a control character. Bytes >= 0x80 pass through, exactly as net/chat.c does:
 * mangling UTF-8 would break a real sentence for no gain. */
static void put_json_str(char *dst, size_t cap, size_t *off, const char *s) {
    ap(dst, cap, off, "\"");
    for (size_t i = 0; s && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  ap(dst, cap, off, "\\\""); break;
            case '\\': ap(dst, cap, off, "\\\\"); break;
            case '\n': ap(dst, cap, off, "\\n");  break;
            case '\r': ap(dst, cap, off, "\\r");  break;
            case '\t': ap(dst, cap, off, "\\t");  break;
            default:
                if (c < 0x20) ap(dst, cap, off, " ");
                else          ap(dst, cap, off, "%c", (char)c);
        }
    }
    ap(dst, cap, off, "\"");
}

/* Render the whole table. Only the "what was asked for" half: statistics are
 * since-boot facts and writing them would mean a disk write on every tick. */
static size_t render(char *dst, size_t cap) {
    size_t off = 0;
    ap(dst, cap, &off, "{\"agenda\":1,\"items\":[");
    for (int i = 0; i < g_n; i++) {
        const agenda_item_t *it = &g_item[i];
        ap(dst, cap, &off, "%s{\"name\":", i ? "," : "");
        put_json_str(dst, cap, &off, it->name);
        ap(dst, cap, &off, ",\"when\":");
        put_json_str(dst, cap, &off, agenda_when_name(it->when));
        ap(dst, cap, &off, ",\"do\":");
        put_json_str(dst, cap, &off, agenda_act_name(it->act));
        ap(dst, cap, &off,
           ",\"period_ms\":%u,\"max_runs\":%u,\"enabled\":%s,\"attempting\":%s",
           (unsigned)it->period_ms, (unsigned)it->max_runs,
           it->enabled ? "true" : "false",
           it->attempting ? "true" : "false");
        if (it->act == AGENDA_DO_TOOL) {
            ap(dst, cap, &off, ",\"tool\":");
            put_json_str(dst, cap, &off, it->tool);
        }
        ap(dst, cap, &off, ",\"arg\":");
        put_json_str(dst, cap, &off, it->arg);
        ap(dst, cap, &off, ",\"note\":");
        put_json_str(dst, cap, &off, it->note);
        ap(dst, cap, &off, "}");
    }
    ap(dst, cap, &off, "]}\n");
    return off;
}

static int file_write(const char *path, const char *buf, size_t len) {
    file_t *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    size_t  done = 0;
    if (!f) return AGENDA_EIO;
    while (done < len) {
        int64_t n = vfs_write(f, buf + done, (uint64_t)(len - done));
        if (n <= 0) { vfs_close(f); return AGENDA_EIO; }
        done += (size_t)n;
    }
    vfs_close(f);      /* flushes; see include/vfs.h */
    return AGENDA_OK;
}

static int file_read(const char *path, char *buf, size_t cap, size_t *out) {
    file_t *f = vfs_open(path, O_RDONLY);
    size_t  got = 0;
    *out = 0;
    if (!f) return AGENDA_ENOENT;
    for (;;) {
        if (got + 1 >= cap) { vfs_close(f); return AGENDA_EIO; }
        int64_t n = vfs_read(f, buf + got, (uint64_t)(cap - 1 - got));
        if (n < 0) { vfs_close(f); return AGENDA_EIO; }
        if (n == 0) break;
        got += (size_t)n;
    }
    vfs_close(f);
    buf[got] = '\0';
    *out = got;
    return AGENDA_OK;
}

int agenda_flush(void) {
    size_t n = render(g_file, sizeof g_file);
    if (n >= sizeof g_file) {
        /* Cannot happen with the bounds in agenda.h, which is exactly why it is
         * checked: a clipped store is a corrupt store, and writing one would
         * lose items that were accepted. */
        kprintf("agenda: the agenda does not fit %u bytes; NOT writing a "
                "truncated store to %s\n",
                (unsigned)sizeof g_file, agenda_store());
        return AGENDA_EIO;
    }
    return file_write(agenda_store(), g_file, n);
}

static int when_of(const char *w) {
    for (int i = 0; i < AGENDA_WHEN_COUNT; i++)
        if (streq(w, when_tab[i])) return i;
    return -1;
}

static int act_of(const char *a) {
    for (int i = 0; i < AGENDA_DO_COUNT; i++)
        if (streq(a, act_tab[i])) return i;
    return -1;
}

/* `payload` selects copy_payload over copy_text — see the comment on those two.
 * A stored argument must come back byte-identical or it is a different call. */
static int get_str(const json_value_t *o, const char *key, char *dst,
                   size_t cap, int payload) {
    json_value_t v;
    dst[0] = '\0';
    if (json_get(o, key, &v) != JSON_OK) return 0;
    if (v.type != JSON_STRING) return -1;
    /* ENOSPC is a refusal, not an absence: an over-long field means the store
     * was written by something that does not share this build's bounds, and
     * quietly loading a clipped sentence would run a different instruction than
     * the one saved. */
    static char raw[AGENDA_ARG_MAX * 2];
    size_t      n = 0;
    if (json_str(&v, raw, sizeof raw, &n) != JSON_OK) return -1;
    if (n >= cap) return -1;
    if (payload) copy_payload(dst, cap, raw);
    else         copy_text(dst, cap, raw);
    return 1;
}

int agenda_load(void) {
    size_t len = 0;
    int    store_dirty = 0;   /* something was retired on the way in */
    int    rc  = file_read(agenda_store(), g_file, sizeof g_file, &len);

    zero(g_item, sizeof g_item);
    g_n = 0;

    if (rc == AGENDA_ENOENT) return 0;       /* no agenda yet: not an error */
    if (rc != AGENDA_OK || len == 0) {
        kprintf("agenda: %s could not be read; starting with an empty agenda\n",
                agenda_store());
        return AGENDA_EIO;
    }

    json_value_t root, items;
    if (json_parse(g_file, len, &root) != JSON_OK || root.type != JSON_OBJECT ||
        json_get(&root, "items", &items) != JSON_OK ||
        items.type != JSON_ARRAY) {
        kprintf("agenda: %s is not an agenda document; starting EMPTY rather "
                "than with half a schedule nobody wrote\n", agenda_store());
        return AGENDA_EIO;
    }

    size_t count = json_count(&items);
    if (count > AGENDA_MAX_ITEMS)
        kprintf("agenda: %s holds %u items and this build has room for %u; the "
                "last %u will NOT be loaded and the next write will erase them\n",
                agenda_store(), (unsigned)count, (unsigned)AGENDA_MAX_ITEMS,
                (unsigned)(count - AGENDA_MAX_ITEMS));

    for (size_t i = 0; i < count && g_n < AGENDA_MAX_ITEMS; i++) {
        json_value_t e;

        /* EVERY REJECTION BELOW IS NAMED OUT LOUD, and it took a defect to get
         * here. This loop used to `continue` silently on any field problem, and
         * only the validate() failure at the bottom printed. Eight distinct ways
         * for a stored item to disappear without a word — a wrong JSON type, an
         * unknown "when", an over-long "tool", a missing "arg", "act" instead of
         * "do" — while the only feedback was the count in "N item(s) loaded",
         * which the operator has no baseline to compare against.
         *
         * That is the exact failure shape this file already calls the worst kind
         * of persistence bug (see the `used` note further down): the evidence all
         * says it worked. It is also permanent rather than transient, because the
         * next agenda_flush() re-renders the store from the truncated table and
         * the dropped item is gone from the disk with no message ever printed.
         *
         * Each message LEADS WITH THE NAME where there is one, and with the
         * position in the document where there is not, because "which one" is the
         * only question the operator can act on. The store path is not repeated in
         * every line: the "N item(s) loaded from ..." line prints it once, below. */
#define drop(...) do { kprintf("agenda: dropping stored item ");                  \
                       kprintf(__VA_ARGS__); kprintf("\n"); } while (0)

        if (json_at(&items, i, &e) != JSON_OK || e.type != JSON_OBJECT) {
            drop("%u of %u: it is not a JSON object",
                 (unsigned)(i + 1), (unsigned)count);
            continue;
        }

        agenda_item_t it;
        char          word[24];
        zero(&it, sizeof it);

        if (get_str(&e, "name", it.name, sizeof it.name, 0) != 1) {
            drop("%u of %u: it has no usable \"name\" (a string of at most %u "
                 "characters)", (unsigned)(i + 1), (unsigned)count,
                 (unsigned)(AGENDA_NAME_MAX - 1));
            continue;
        }
        if (get_str(&e, "when", word, sizeof word, 0) != 1) {
            drop("\"%s\": it has no \"when\" string", it.name);
            continue;
        }
        int w = when_of(word);
        if (w < 0) {
            drop("\"%s\": it says when=\"%s\", and this build knows boot, every "
                 "and once", it.name, word);
            continue;
        }
        it.when = (uint8_t)w;
        if (get_str(&e, "do", word, sizeof word, 0) != 1) {
            drop("\"%s\": it has no \"do\" string (a store written by a build that "
                 "called this field something else looks exactly like this)",
                 it.name);
            continue;
        }
        int a = act_of(word);
        if (a < 0) {
            drop("\"%s\": it says do=\"%s\", and this build knows tool and say",
                 it.name, word);
            continue;
        }
        it.act = (uint8_t)a;
        if (get_str(&e, "tool", it.tool, sizeof it.tool, 0) < 0) {
            drop("\"%s\": its \"tool\" is not a string, or is longer than the %u "
                 "characters this build can hold", it.name,
                 (unsigned)(AGENDA_TOOL_MAX - 1));
            continue;
        }
        if (get_str(&e, "arg", it.arg, sizeof it.arg,
                    it.act == AGENDA_DO_TOOL) != 1) {
            drop("\"%s\": it has no \"arg\" string, or one longer than %u "
                 "characters. An object or array here rather than a string "
                 "reads as absent", it.name, (unsigned)(AGENDA_ARG_MAX - 1));
            continue;
        }
        if (get_str(&e, "note", it.note, sizeof it.note, 0) < 0) {
            drop("\"%s\": its \"note\" is not a string, or is longer than %u "
                 "characters", it.name, (unsigned)(AGENDA_NOTE_MAX - 1));
            continue;
        }

        int64_t n64;
        json_value_t v;
        it.period_ms = AGENDA_MIN_PERIOD_MS;
        it.max_runs  = AGENDA_MAX_RUNS;
        if (json_get(&e, "period_ms", &v) == JSON_OK && v.type == JSON_NUMBER &&
            json_int(&v, &n64) == JSON_OK && n64 > 0 && n64 < 0x7FFFFFFF)
            it.period_ms = (uint32_t)n64;
        if (json_get(&e, "max_runs", &v) == JSON_OK && v.type == JSON_NUMBER &&
            json_int(&v, &n64) == JSON_OK && n64 > 0 && n64 <= AGENDA_MAX_RUNS)
            it.max_runs = (uint32_t)n64;
        it.enabled = 1;
        if (json_get(&e, "enabled", &v) == JSON_OK && v.type == JSON_BOOL) {
            int b = 1;
            if (json_bool(&v, &b) == JSON_OK && !b) it.enabled = 0;
        }

        /* THE MARKER THE PREVIOUS BOOT LEFT BEHIND. Set on disk immediately
         * before a when=boot action is attempted and cleared once it resolves,
         * so finding it still set means the last boot never came back out of
         * this item. Disable it, DURABLY, and say what happened — otherwise the
         * item re-arms (a when=boot item must, by design) and this boot dies in
         * exactly the same place, for ever, with no interface left to stop it.
         * See agenda_item_t.attempting in include/agenda.h. */
        if (json_get(&e, "attempting", &v) == JSON_OK && v.type == JSON_BOOL) {
            int b = 0;
            if (json_bool(&v, &b) == JSON_OK && b) {
                it.enabled = 0;
                it.attempting = 0;
                store_dirty = 1;
                kprintf("agenda: \"%s\" was still marked as RUNNING in the "
                        "store, which means the previous boot did not survive "
                        "it. It has been DISABLED so this boot can reach a "
                        "prompt. Re-enable it with agenda_control if you know "
                        "why it crashed.\n",
                        it.name[0] ? it.name : "(unnamed)");
            }
        }

        /* A STORED ITEM IS REVALIDATED FROM SCRATCH, exactly as if it had just
         * been dictated. The disk may have been written by a different build,
         * with a different tool table or different bounds, and "it was in the
         * store" is not evidence that it is runnable here. An item that fails is
         * named and dropped, so the operator learns which one rather than
         * discovering an agenda that is quietly one item short. */
        char why[AGENDA_WHY_MAX];
        int  vrc = validate(&it, why, sizeof why);
        if (vrc != AGENDA_OK) {
            drop("\"%s\": %s", it.name[0] ? it.name : "(unnamed)", why);
            continue;
        }
        if (agenda_find(it.name)) {
            /* Nothing is lost — one copy of the name is already loaded — but say
             * so anyway, because a store with two items of one name means
             * something wrote it that was not this module. */
            drop("\"%s\": that name appears twice in the store; keeping the first",
                 it.name);
            continue;
        }
#undef drop
        /* `used` IS LOAD-BEARING AND WAS ONCE FORGOTTEN HERE.
         *
         * agenda_save() set it; this loader did not, so every item read back off
         * a disk was skipped by run_boot() and pick_due() and NOTHING an operator
         * had scheduled ever ran again after a power cycle. The agenda listed the
         * item, reported it enabled, and silently never fired it — the worst shape
         * a persistence bug can have, because the evidence all says it worked.
         * Caught by a real reboot, not by a test, which is why
         * test_a_reloaded_item_actually_runs() now exists and why the two
         * schedulers below no longer gate on this field at all: g_n is the single
         * authority for which slots are occupied, and a second source of that
         * truth is a second thing that can be wrong. */
        it.used = 1;
        g_item[g_n++] = it;
    }

    /* Rewrite the store if anything was retired on the way in, so the marker is
     * cleared and the disable is durable even if this boot never touches the
     * agenda again. A failed write is not fatal — the item is already disabled
     * in memory, which is the half that stops this boot dying — but it must be
     * said, because it means the next boot will have to do the same thing. */
    if (store_dirty && agenda_flush() != AGENDA_OK)
        kprintf("agenda: could not rewrite %s, so an item retired above will "
                "have to be retired again at the next boot\n", agenda_store());

    return g_n;
}

/* ====================================================================== */
/* editing                                                               */
/* ====================================================================== */

int agenda_save(const agenda_item_t *in, char *why, size_t whycap) {
    char scratch[AGENDA_WHY_MAX];
    if (!why || whycap == 0) { why = scratch; whycap = sizeof scratch; }

    /* Normalise into a local first, so a refusal cannot have half-written a
     * slot and so the table only ever holds defanged text. */
    agenda_item_t it;
    zero(&it, sizeof it);
    if (in) {
        copy_text(it.name, sizeof it.name, in->name);
        copy_text(it.note, sizeof it.note, in->note);
        copy_text(it.tool, sizeof it.tool, in->tool);
        it.act       = in->act;
        /* NOT unconditionally copy_payload: a "say" arg is prose. See arg_copy(). */
        arg_copy(it.arg, sizeof it.arg, in->arg, in->act);
        it.when      = in->when;
        it.period_ms = in->period_ms ? in->period_ms : AGENDA_MIN_PERIOD_MS;
        it.max_runs  = in->max_runs  ? in->max_runs  : AGENDA_MAX_RUNS;
        it.enabled   = in->enabled ? 1 : 0;
    }

    int rc = validate(&it, why, whycap);
    if (rc != AGENDA_OK) return rc;

    agenda_item_t *slot = find_mut(it.name);
    if (!slot) {
        if (g_n >= AGENDA_MAX_ITEMS) {
            size_t off = 0;
            if (whycap) why[0] = '\0';
            ap(why, whycap, &off,
               "the agenda already holds %u items, which is all it holds. "
               "Remove one first", (unsigned)AGENDA_MAX_ITEMS);
            return AGENDA_ENOSPC;
        }
        slot = &g_item[g_n++];
    }
    /* A replacement keeps nothing: the statistics belonged to the old item's
     * schedule and reporting them as the new one's would attribute one item's
     * failures to another. core/capability.c fixed exactly this bug once. */
    zero(slot, sizeof *slot);
    *slot      = it;
    slot->used = 1;
    slot->next_ms = 0;

    int frc = agenda_flush();
    if (frc != AGENDA_OK) {
        size_t off = 0;
        if (whycap) why[0] = '\0';
        ap(why, whycap, &off,
           "the item is live in memory but could not be written to %s, so it "
           "will NOT survive a reboot", agenda_store());
        return AGENDA_EIO;
    }
    return AGENDA_OK;
}

int agenda_remove(const char *name) {
    for (int i = 0; i < g_n; i++) {
        if (!streq(g_item[i].name, name)) continue;
        for (int j = i; j + 1 < g_n; j++) g_item[j] = g_item[j + 1];
        zero(&g_item[--g_n], sizeof g_item[0]);
        agenda_flush();
        return AGENDA_OK;
    }
    return AGENDA_ENOENT;
}

int agenda_set_enabled(const char *name, int on) {
    agenda_item_t *it = find_mut(name);
    if (!it) return AGENDA_ENOENT;
    it->enabled = on ? 1 : 0;
    if (on) it->failures = 0;        /* re-enabling forgives the retirement */
    agenda_flush();
    return AGENDA_OK;
}

/* ====================================================================== */
/* running one item                                                       */
/* ====================================================================== */

/* What the guarded body needs. On the stack of run_item(), which is above the
 * guard, so an escape leaves it intact and readable. */
typedef struct run_ctx {
    agenda_item_t *it;
    int            rc;
    char           result[512];
    int            is_error;
} run_ctx_t;

/* THE ONLY CODE IN THIS FILE THAT RUNS INSIDE A FAULT GUARD.
 *
 * It is deliberately tiny and it deliberately touches no agenda state: an escape
 * abandons this function wherever it happens to be, so anything it half-updated
 * would stay half-updated. Everything it learns comes back through *ctx, which
 * lives on the caller's frame, and the caller does all the accounting after the
 * guard has resolved one way or the other. */
static void run_body(void *arg) {
    run_ctx_t     *c  = (run_ctx_t *)arg;
    agenda_item_t *it = c->it;

    if (it->act == AGENDA_DO_SAY) {
        if (!g_say) { c->rc = AGENDA_ENOSAY; return; }
        int rc = g_say(it->arg);
        /* DO NOT FLATTEN THIS INTO AGENDA_EIO. It used to, and AGENDA_EIO reads
         * "the agenda store could not be read or written" — so a failure in the
         * MODEL TRANSPORT was reported to the model as disk corruption. Observed
         * live: a nested chat_ask() returned non-zero, the model was told the
         * FAT32 store was broken, and it spent six rounds and two paid API calls
         * investigating a disk that was perfect before delivering a degraded
         * workaround and repeating the kernel's false claim to the operator.
         *
         * On a machine whose entire interface is a model reading error text, an
         * error code that is a lie is worse than a crash. Two outcomes are
         * distinguishable here and both are named:
         *   - the sink refused because a turn is already running, which is a
         *     fact about WHEN, not about whether the item works;
         *   - anything else, which is a failure of the sentence itself. */
        if (rc == 0)                   c->rc = AGENDA_OK;
        else if (rc == CHAT_EREENTER)  c->rc = AGENDA_EBUSY;
        else                           c->rc = AGENDA_ESAY;
        return;
    }

    tool_result_t r;
    tool_result_init(&r, c->result, sizeof c->result);

    tool_call_t call;
    call.id        = "toolu_kernel_agenda";
    call.name      = it->tool;
    call.input     = it->arg;
    call.input_len = slen(it->arg);

    /* Through the one door: tool_dispatch() emits the tool's own trace line, in
     * C, from the real return value. An autonomous tool call and a model-issued
     * one are therefore indistinguishable in the log except for the
     * [agenda.run ...] line around it, which is the point — the reader can see
     * both that it happened and that nobody asked for it. */
    int rc = tool_dispatch(&call, &r);
    c->is_error = r.is_error;
    if (rc == TOOL_ENOENT)      c->rc = AGENDA_ENOTOOL;
    else if (rc != TOOL_OK)     c->rc = AGENDA_EINVAL;
    else if (r.is_error)        c->rc = AGENDA_EINVAL;
    else                        c->rc = AGENDA_OK;
}

/* First line of the tool's result, clipped and defanged — enough for the record
 * without letting a long result fill the console. */
static void first_line(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && src[i] != '\n' && i + 1 < cap) i++;
    copy_text(dst, (i + 1 < cap) ? i + 1 : cap, src);
}

static int run_item(agenda_item_t *it, uint64_t now_ms) {
    run_ctx_t c;

    if (!it) return AGENDA_ENOENT;
    if (!g_on) return AGENDA_EOFF;
    if (g_running) return AGENDA_EBUSY;
    if (g_runs >= AGENDA_MAX_RUNS_TOTAL) {
        kprintf("agenda: this boot has taken its %u autonomous actions; \"%s\" "
                "was NOT run. Nothing further will run without a reset.\n",
                (unsigned)AGENDA_MAX_RUNS_TOTAL, it->name);
        it->enabled = 0;
        return AGENDA_ERETIRED;
    }

    zero(&c, sizeof c);
    c.it = it;
    c.rc = AGENDA_OK;

    g_running = 1;
    it->runs++;
    it->last_ms = now_ms;
    g_runs++;

    /* BEFORE, not only after. An action that hangs or that halts the machine in
     * some way this guard does not cover leaves a line saying what was starting,
     * which on an unattended machine is the difference between a diagnosable
     * event and a mystery. */
    trace_ok("agenda.run", "%s %s %s runs=%u", it->name,
             agenda_when_name(it->when), agenda_act_name(it->act),
             (unsigned)it->runs);

    /* DURABLE "I AM ABOUT TO TRY THIS", for boot items only.
     *
     * The trace line above is a console line, and a console line does not
     * survive a power cycle. Everything below that could retire a broken item —
     * it->failures, the durable disable, agenda_flush() — is on the far side of
     * the guard call, and an action that hangs or triple-faults never gets
     * there, so the store still said "enabled": true and a when=boot item
     * re-armed into the same crash at the next boot, for ever. This is the one
     * write that happens BEFORE the attempt, and agenda_load() is what reads it.
     * See agenda_item_t.attempting in include/agenda.h.
     *
     * Cost: two extra store writes per boot item per boot, and only for boot
     * items, because they are the ones that run before any interface exists. */
    int marked = (it->when == AGENDA_WHEN_BOOT);
    if (marked) {
        it->attempting = 1;
        if (agenda_flush() != AGENDA_OK) {
            /* Not fatal, and not silent: without the marker this boot is back to
             * the old behaviour, and the operator should know which item is
             * running unprotected. */
            it->attempting = 0;
            marked         = 0;
            kprintf("agenda: could not record that \"%s\" was starting, so if "
                    "this action kills the machine the next boot will try it "
                    "again\n", it->name);
        }
    }

    /* THE GUARD. A fatal CPU exception inside the action returns here instead of
     * halting the machine — see include/fault.h. This is what makes an
     * autonomous action safe to take with nobody watching, and it is also what
     * gives net/faultchat.c a fault to diagnose: the machine is still running,
     * in normal kernel context, on the far side of the handler. */
    int grc = fault_guard_run(it->name, run_body, &c);

    /* IT CAME BACK, SO CLEAR THE MARKER ON DISK, NOT ONLY IN MEMORY. Clearing it
     * only in memory would leave "attempting": true on the disk for the rest of
     * the boot, and any LATER crash — one this item had nothing to do with —
     * would retire it at the next boot. A safeguard that retires innocent items
     * teaches the operator to ignore it. */
    if (marked) {
        it->attempting = 0;
        if (agenda_flush() != AGENDA_OK)
            kprintf("agenda: \"%s\" finished, but the store still records it as "
                    "running; if this boot ends badly it will be disabled at the "
                    "next one\n", it->name);
    }

    g_running = 0;

    if (grc == FAULT_GUARD_ESCAPED) {
        it->escapes++;
        g_escapes++;
        c.rc = AGENDA_EFAULT;
        copy_text(it->last_why, sizeof it->last_why,
                  "a fatal CPU exception abandoned this action's call chain; "
                  "the machine survived and the fault is in the fault ring");
    } else if (grc < 0) {
        /* The guard could not be armed, so the body was NOT run. Undo the run
         * accounting: counting an action that never happened would make the
         * bounds lie in the one direction that matters. */
        it->runs--;
        if (g_runs) g_runs--;
        c.rc = AGENDA_EBUSY;
        copy_text(it->last_why, sizeof it->last_why,
                  "no fault guard could be armed, so the action was not "
                  "attempted; an unguarded autonomous action could halt the "
                  "machine");
        trace_err(c.rc, "agenda.run", "%s not-guarded", it->name);
        it->last_rc = c.rc;
        return c.rc;
    } else if (c.rc == AGENDA_OK) {
        first_line(it->last_why, sizeof it->last_why,
                   it->act == AGENDA_DO_TOOL ? c.result : "sentence delivered");
    } else {
        first_line(it->last_why, sizeof it->last_why,
                   c.result[0] ? c.result : agenda_strerror(c.rc));
    }

    it->last_rc = c.rc;
    if (c.rc == AGENDA_OK) {
        it->failures = 0;
        trace_ret((long)it->runs, "agenda.done", "%s ok", it->name);
    } else {
        it->failures++;
        it->total_failures++;
        g_fails++;
        trace_err(c.rc, "agenda.done", "%s fail=%u", it->name,
                  (unsigned)it->failures);
    }

    /* ---- retirement, said out loud, and TWO OF THE FOUR ARE DURABLE ----
     *
     * An item that has stopped working must stop costing something, and it must
     * be obvious that it stopped. Every branch disables the item rather than
     * skipping it, so agenda_list shows a disabled item with a reason instead of
     * a schedule that looks fine and never fires.
     *
     * WHICH DISABLES SURVIVE A POWER CYCLE IS A DECISION, NOT AN ACCIDENT, and it
     * used to be an accident: none of them were written to the store, so the
     * on-disk document still said "enabled":true and store_load() re-enabled
     * everything. A permanently broken item therefore re-armed and failed three
     * more times at every boot for ever, and a once-item fired once per boot for
     * ever — both of which the operator had been told, in these words, had
     * stopped. For a do:"say" item that is three paid API calls per boot,
     * unattended, indefinitely. Worse, whether the retirement stuck at all
     * depended on whether the model happened to edit some OTHER item later in the
     * same boot, because agenda_flush() rewrites the whole table.
     *
     *   failures    DURABLE. "Three consecutive failures" is a property of the
     *               item, not of this boot. An item that is broken because the
     *               network was not up yet is the case for forgiving it, and the
     *               forgiveness is explicit: agenda_control enable clears
     *               failures. A retirement the operator was told about must not
     *               undo itself while nobody is looking.
     *   once        DURABLE. include/agenda.h promises "at the next tick, then
     *               disable itself", and a once-item that re-fires every boot is
     *               not once.
     *   max_runs    PER BOOT, deliberately, and agenda.h says so. The runs
     *               counter is a since-boot number; persisting the disable it
     *               causes would silently convert every item into a once-item.
     *   boot        PER BOOT, necessarily: a when=boot item MUST re-arm, or it
     *               would run on exactly one boot in the machine's life.
     *
     * A failed flush is not fatal here and must not become one: the item is
     * already disabled in memory, which is the half that stops the cost now. */
    int durable = 0;
    if (it->failures >= AGENDA_MAX_FAILURES) {
        it->enabled = 0;
        durable     = 1;
        kprintf("agenda: \"%s\" has failed %u times in a row and is now "
                "DISABLED, on disk as well as in memory. Last reason: %s\n",
                it->name, (unsigned)it->failures, it->last_why);
    } else if (it->runs >= it->max_runs) {
        it->enabled = 0;
        kprintf("agenda: \"%s\" has run its %u times for this boot and is now "
                "disabled until the next boot\n",
                it->name, (unsigned)it->max_runs);
    } else if (it->when == AGENDA_WHEN_ONCE) {
        it->enabled = 0;
        durable     = 1;
    } else if (it->when == AGENDA_WHEN_BOOT) {
        it->enabled = 0;                 /* re-armed by the next boot's load */
    } else {
        it->next_ms = now_ms + (uint64_t)it->period_ms;
    }

    if (durable && agenda_flush() != AGENDA_OK)
        kprintf("agenda: \"%s\" is disabled in memory but %s could not be "
                "written, so it will be back after a reboot\n",
                it->name, agenda_store());

    return c.rc;
}

int agenda_run_now(const char *name, uint64_t now_ms) {
    agenda_item_t *it = find_mut(name);
    if (!it) return AGENDA_ENOENT;
    return run_item(it, now_ms);
}

/* ====================================================================== */
/* the tick                                                               */
/* ====================================================================== */

static agenda_item_t *pick_due(uint64_t now_ms) {
    for (int i = 0; i < g_n; i++) {
        agenda_item_t *it = &g_item[i];
        if (!it->enabled)                   continue;
        if (it->runs >= it->max_runs)       continue;
        if (it->when == AGENDA_WHEN_BOOT)   continue;   /* bringup's business */
        if (it->when == AGENDA_WHEN_ONCE)   return it;
        /* AGENDA_WHEN_EVERY. next_ms == 0 means "never run": fire on the first
         * tick rather than waiting a whole period, so an item saved now proves
         * itself now. */
        if (it->next_ms == 0 || now_ms >= it->next_ms) return it;
    }
    return NULL;
}

const char *agenda_due(uint64_t now_ms) {
    agenda_item_t *it;
    if (!g_on || g_running) return NULL;
    it = pick_due(now_ms);
    return it ? it->name : NULL;
}

uint32_t agenda_tick(uint64_t now_ms) {
    agenda_item_t *it;
    if (!g_on || g_running) return 0;
    it = pick_due(now_ms);
    if (!it) return 0;
    run_item(it, now_ms);
    /* ONE per call. The caller is the loop that also polls the keyboard, so a
     * backlog must not be able to make the machine stop listening; it drains at
     * one item per pass through the idle loop, which is thousands per second. */
    return 1;
}

uint32_t agenda_run_boot(uint64_t now_ms) {
    uint32_t n = 0;
    if (!g_on) return 0;
    for (int i = 0; i < g_n; i++) {
        agenda_item_t *it = &g_item[i];
        if (!it->enabled)                  continue;
        if (it->when != AGENDA_WHEN_BOOT)  continue;
        run_item(it, now_ms);
        n++;
    }
    return n;
}

/* THE DENYLIST IS A LIST OF NAMES, SO IT CAN GO STALE, SO IT IS CHECKED.
 *
 * A name list that no longer matches the registry fails SILENTLY in the unsafe
 * direction: rename power_off and the entry stops matching anything, the check
 * stops firing, and a machine that cannot boot becomes schedulable again with
 * every test still green. This runs on every boot against the real tool table —
 * which is the only place the truth lives — and says both names out loud when one
 * does not resolve. Returns the number that did not.
 *
 * It is a print and not a panic: a build that genuinely has no power tools is a
 * legitimate build, and refusing to boot over it would be worse than saying so. */
/* A BOUND THAT MIRRORS SOMETHING COMPUTED IS CHECKED AGAINST THE THING IT MIRRORS.
 *
 * AGENDA_TOOL_MAX is now derived from TOOL_NAME_MAX rather than guessed at, and the
 * _Static_assert below pins that. But TOOL_NAME_MAX is a promise about tool names,
 * not an enforcement of them: core/tool.c does not truncate or refuse a longer one,
 * and this module is the place that would silently lose an item over it — a stored
 * item naming a too-long tool drops with "its tool is longer than N characters" on a
 * machine where the tool itself works perfectly.
 *
 * So the live registry is walked once at boot and any name that does not fit is
 * named, with BOTH numbers, which is the whole point: whoever reads it should not
 * have to go and look either of them up. Returns the number that did not fit. */
_Static_assert(AGENDA_TOOL_MAX > TOOL_NAME_MAX,
               "AGENDA_TOOL_MAX must hold TOOL_NAME_MAX characters plus a NUL");

int agenda_bounds_ok(void) {
    int bad = 0;
    for (int i = 0, n = tool_count(); i < n; i++) {
        const tool_t *t = tool_at(i);
        if (!t || !t->name) continue;
        size_t len = slen(t->name);
        if (len < AGENDA_TOOL_MAX) continue;
        bad++;
        kprintf("agenda: \"%s\" is %u characters and this build can only store %u "
                "in an agenda item, so it can be called in conversation but NOT "
                "scheduled. Raise AGENDA_TOOL_MAX in include/agenda.h\n",
                t->name, (unsigned)len, (unsigned)(AGENDA_TOOL_MAX - 1));
    }
    return bad;
}

int agenda_denylist_unresolved(void) {
    int bad = 0;
    for (int i = 0;; i++) {
        const char *name = agenda_denied_tool(i);
        if (!name) break;
        if (tool_find(name)) continue;
        bad++;
        kprintf("agenda: DENYLIST STALE - \"%s\" is refused as an agenda item but "
                "no tool of that name is registered in this build. If it was "
                "renamed, the rename re-armed a schedule that can make this "
                "machine unbootable; fix deny_tab in core/agenda.c\n", name);
    }
    return bad;
}

uint32_t agenda_bringup(uint64_t now_ms) {
    int loaded;

    agenda_bounds_ok();               /* constants vs the live registry */
    agenda_denylist_unresolved();     /* the denylist vs the live registry */
    loaded = agenda_load();

    kprintf("agenda: %d item(s) loaded from %s (%s)\n",
            loaded > 0 ? loaded : 0, agenda_store(),
            agenda_persistent() ? "survives a reboot"
                                : "RAM ONLY - lost at reboot");

    uint32_t n = agenda_run_boot(now_ms);
    if (n)
        kprintf("agenda: %u boot item(s) ran before anyone typed anything\n",
                (unsigned)n);
    return n;
}

/* ====================================================================== */
/* introspection                                                          */
/* ====================================================================== */

void agenda_report(void) {
    kprintf("agenda: %d item(s), store %s (%s), master switch %s\n",
            g_n, agenda_store(),
            agenda_persistent() ? "persistent" : "RAM only",
            g_on ? "on" : "OFF");
    if (g_runs)
        kprintf("agenda: %u autonomous action(s) this boot, %u failed, %u "
                "abandoned by a CPU fault\n",
                (unsigned)g_runs, (unsigned)g_fails, (unsigned)g_escapes);
    for (int i = 0; i < g_n; i++) {
        const agenda_item_t *it = &g_item[i];
        kprintf("  %s %s %s%s%s runs=%u/%u %s%s\n",
                it->name, agenda_when_name(it->when),
                agenda_act_name(it->act),
                it->act == AGENDA_DO_TOOL ? " " : "",
                it->act == AGENDA_DO_TOOL ? it->tool : "",
                (unsigned)it->runs, (unsigned)it->max_runs,
                it->enabled ? "enabled" : "DISABLED",
                it->last_rc ? " (last run failed)" : "");
    }
}
