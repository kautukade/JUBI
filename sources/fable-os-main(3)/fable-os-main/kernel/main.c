/* main.c — kernel entry.
 *
 * Brings up the hardware, the filesystem and the network, then hands the
 * machine over to natural language. There is no shell here and there is no
 * fallback interface: after boot the only thing this kernel does is read a
 * sentence and let the model act on the machine through tool.h's syscalls.
 *
 * THE TRANSCRIPT
 *   Three voices share one console, and they are told apart by shape, not by
 *   chat-client labels:
 *
 *       > write hello into /etc/motd          <- the operator; only the '>' is
 *                                                printed by the kernel, the rest
 *                                                is the line editor's echo
 *       [vfs_write /etc/motd bytes=5 -> ok]   <- the kernel, in C, after the
 *                                                call returned (trace.h)
 *       Done - /etc/motd now holds "hello".   <- the model, prose, persuasive
 *                                                and unverified
 *
 *   Brackets are facts. Prose is a claim. That distinction is the only audit
 *   trail a machine with no `ls` can offer, so nothing else is allowed to print
 *   in bracket form.
 */

#include "kernel.h"
#include "idt.h"
#include "driver.h"
#include "input.h"
#include "net.h"
#include "fs.h"
#include "vfs.h"
#include "tool.h"
#include "chat.h"
#include "model.h"
#include "gui.h"
#include "app.h"
#include "capability.h"
#include "cc.h"
#include "fiber.h"
#include "agenda.h"
#include "fault.h"
#include "faultchat.h"

/* The root stack, reserved and exported by boot/boot.asm. Declared as arrays
 * because these are addresses, not objects: `extern uint8_t stack_top;` would
 * make &stack_top the only legal use and invite someone to read it. */
extern uint8_t stack_bottom[], stack_top[];

/* Exercise the VFS end-to-end: mount, mkdir, create/write/seek/read a file,
 * stat it, and list a directory. Proves the whole path without a disk. */
static void vfs_selftest(void) {
    if (fs_init() != 0) { kputs("[fs init failed]\n"); return; }
    kputs("\n--- filesystem self-test (ramfs at /) ---\n");

    vfs_mkdir("/etc");
    vfs_mkdir("/tmp");

    /* tests/qemu/cases/{boot,no-nic}.case assert this string AND its length in
     * bytes, so editing it fails two cases until they are updated. Renaming the
     * OS was enough: "talk-os" to "fable-os" is one character, which moved 33 to
     * 34 and went red. The assertion is worth keeping — a motd that reads back
     * the wrong SIZE means the VFS lied about a write. */
    const char *msg = "fable-os native filesystem online\n";
    file_t *f = vfs_open("/etc/motd", O_CREAT | O_RDWR);
    if (!f) { kputs("open /etc/motd failed\n"); return; }
    vfs_write(f, msg, strlen(msg));

    /* Seek back and read it into a buffer to prove read/write/seek. */
    char buf[64];
    vfs_seek(f, 0, SEEK_SET);
    int64_t n = vfs_read(f, buf, sizeof buf - 1);
    if (n > 0) buf[n] = '\0';
    kprintf("read /etc/motd (%d bytes): %s", (int)n, buf);
    vfs_close(f);

    vfs_stat_t st;
    if (vfs_stat("/etc/motd", &st) == VFS_OK)
        kprintf("stat /etc/motd: type=%s size=%u mode=0x%x\n",
                st.type == VNODE_DIR ? "dir" : "file",
                (unsigned)st.size, (unsigned)st.mode);

    kputs("ls / :");
    char name[64];
    for (uint32_t i = 0; vfs_readdir("/", i, name, sizeof name) == VFS_OK; i++)
        kprintf(" %s", name);
    kputs("\n-----------------------------------------\n");
}

/* What the operator is handed after boot: what this machine can do, what it
 * cannot, and how to read what comes next. No command list, because there are
 * no commands — the tool count is the closest thing to a vocabulary and the
 * model is the only one who needs to know the names. */
static void ready_banner(int net_up) {
    kputs("\n");
    kputs("==============================================================\n");
    kputs("  fable-os\n");
    kputs("==============================================================\n");
    kprintf("  tools  : %d registered (%u bytes of schema)\n",
            tool_count(), (unsigned)chat_tools_bytes());
    kprintf("  model  : %s\n", net_up ? "reachable over TLS" : "UNREACHABLE");
    kputs("\n");
    kputs("  No shell, no commands. Say what you want done, in a sentence.\n");
    kputs("  Prose is the model talking. [Brackets] are this kernel saying\n");
    kputs("  what it actually did.\n");

    if (!net_up) {
        kputs("\n");
        kputs("  The model cannot be reached, and it is the only interface\n");
        kputs("  this machine has. It will listen, but it cannot act.\n");
    }
    if (tool_count() == 0) {
        kputs("\n");
        kputs("  No tools are registered, so the model can talk about this\n");
        kputs("  machine but cannot change it.\n");
    }
}

/* ======================================================================= */
/* THE UI FIBER — why the machine no longer dies while it thinks           */
/* ======================================================================= */

/* Two things have to happen continuously for this machine to look alive: the
 * pointer, the windows and the repaint (gui_tick), and the periodic handlers
 * of model-authored apps (app_tick). Both are bounded, neither blocks, and
 * both return immediately when nothing is open — see the event-loop section of
 * include/gui.h and the APP_MAX_TICKS / APP_TICK_MIN_MS reasoning in
 * include/app.h.
 *
 * They used to run only in wait_for_sentence(), i.e. only while the machine
 * had nothing to do. The moment an API turn started, the kernel descended into
 * net/net.c and spun there for seconds with the whole machine stopped: a clock
 * app froze mid-second, the pointer stuck where it was, and a window dragged
 * over the console left a hole until the model answered.
 *
 * So this pair now lives in its own fiber, and every wait in the kernel yields
 * to it (net/net.c's net_service(), and wait_for_sentence() below). The body
 * is two calls, a clock read and a yield, and it is that small on purpose: it
 * is the only fiber the kernel creates, it is the one that must never hog the
 * CPU, and both calls in it are documented as bounded — which is the whole of
 * the argument that it cannot hang the machine, given that nothing here can
 * preempt it (see include/fiber.h).
 *
 * ORDER MATTERS: app_tick() first. A tick handler's assignments change widget
 * text, which marks damage; gui_tick()'s gui_sync() is what paints damage. The
 * other way round a clock would be a frame stale, which on a machine whose only
 * moving picture IS the clock is the whole of the visible behaviour. */
static fiber_t *ui_fiber;

/* Evidence, not decoration: how much work the ui fiber got done, so the boot
 * log can state plainly how many times it ran DURING an API call rather than
 * asking anyone to believe it did. */
static uint64_t ui_passes;
static uint64_t ui_ticks;

/* HOW OFTEN THE UI FIBER ACTUALLY DOES ITS WORK, and why it is not "always".
 *
 * The loops that yield to this fiber are polling loops: net/net.c's goes round
 * roughly 8000 times a second during a model turn, and wait_for_sentence()
 * spins as fast as the CPU allows. Repainting the screen and running app tick
 * handlers at that rate is redundant work — nothing can see 8000 frames a
 * second, and app.h's own floor on a periodic handler is APP_TICK_MIN_MS = 50.
 * Worse than redundant on real hardware: gui_sync() calls console_sync(),
 * which rescans every console cell, and a composite writes to a framebuffer
 * that is uncached write-combining memory across PCIe.
 *
 * WHAT THIS IS *NOT* CLAIMED TO BUY, because it was measured and it did not.
 * Timed over seven to nine boots each, the network phase (the "net: up at"
 * line to the "[http]" line — DNS, the TLS handshake and a real round trip to
 * api.anthropic.com) is the same to within its own noise in every variant:
 *
 *     no fibers at all                  median 0.91 s   (min 0.79)
 *     fibers, ui unthrottled            median 0.95 s   (min 0.77)
 *     fibers, ui throttled, clock open  median 0.87 s   (min 0.77)
 *     fibers, ui unthrottled, clock open median 0.79 s  (min 0.66)
 *
 * So neither the context switches nor the repainting cost anything a real
 * network round trip can notice, and the throttle is here because bounding
 * this fiber's duty cycle is right, not because a slowdown was found. Said out
 * loud so nobody later "optimises" against a number that was never real.
 *
 * 4 ms is 250 Hz: far above any display, above the 200 Hz PS/2 mouse, and
 * eight times inside gui_tick()'s GUI_EVENT_BUDGET of 32 events, so no mouse
 * event can back up and no click can be late by anything a person could
 * notice. Between two of those the fiber still yields immediately, so the
 * scheduler is never blocked and the network loop is never delayed. */
#define UI_PERIOD_MS 4u

static void ui_body(void *arg) {
    (void)arg;
    uint64_t due = 0;
    for (;;) {
        uint64_t now = millis();
        /* now < due means the clock went backwards (a suspend, or a test
         * installing another source); do the work rather than freeze until it
         * catches up, the same rule app_tick() applies to itself. */
        if (now >= due || now + UI_PERIOD_MS < due) {
            due = now + UI_PERIOD_MS;
            /* app_tick_handlers(), NOT app_tick(). This fiber runs INSIDE every
             * network wait — net/net.c's net_service() yields to it on every pass
             * of a DNS lookup and of the model turn — so anything it calls happens
             * during a TLS round trip. app_tick() ends with app_service(), the one
             * function in apps/ that touches hardware, and audio.h's play path is
             * synchronous: calling it here made a model-authored document able to
             * block lwIP for hundreds of milliseconds in the middle of an API
             * call, and falsified the invariant apps/cap.c and include/app.h both
             * state as the reason their design is safe ("no sound starts while the
             * machine is inside a model turn"). Handlers only compute and may
             * QUEUE a request; wait_for_sentence() performs it when the machine is
             * actually idle. Do not put app_tick() back here. */
            ui_ticks += (uint64_t)app_tick_handlers();
            gui_tick();
            ui_passes++;
        }
        fiber_yield();
    }
}

/* Do the ui work inline. Used when there is no ui fiber — either because the
 * heap could not hold a stack, or because every slot was taken. A machine that
 * refuses to boot because a cosmetic fiber could not be created would be a
 * worse machine than one that goes back to freezing while it thinks, so the
 * fallback is exactly the pre-fiber behaviour. */
static void ui_inline(void) {
    ui_ticks += (uint64_t)app_tick();
    gui_tick();
    ui_passes++;
}

/* Block for a line, but through the non-blocking poll so there is one obvious
 * place to service anything else the machine needs to do between sentences.
 *
 * Now that the ui work has its own fiber, this loop's job is only to give it
 * the CPU. input_poll() stays HERE and is deliberately not in the ui fiber:
 * the line being assembled belongs to whoever is about to act on it, and two
 * fibers draining the same keyboard would race for characters at exactly the
 * granularity — one keystroke — that makes the result unreproducible.
 *
 * Without this the clock and the pointer stop between sentences too, and a
 * document carrying {"tick": 1000} is validated, accepted, answered with a
 * success trace line and a window — and never updates. Every other failure in
 * this system comes back as an error the model can act on; that one came back
 * as a lie. */
/* ======================================================================= */
/* THE AGENDA AND THE SELF-REPAIR LOOP — what runs when nobody is typing   */
/* ======================================================================= */

/* THE SELF-REPAIR DEMONSTRATION, in diagnostic builds only.
 *
 *     make EXTRA_CFLAGS=-DFABLEOS_REPAIR_DEMO run-nox
 *
 * Schedules a repeating agenda item that calls tools/agenda_tools.c's
 * `repair_demo` tool, which divides by zero. Watch the serial log with nobody
 * touching the keyboard: the action faults, the fault guard returns control here
 * instead of halting, faultchat_pump() asks the model what happened, the model
 * proposes a patch to this kernel's .text, the patch is validated and applied,
 * and the next scheduled run of the same action succeeds.
 *
 * Compiled out of every normal build. The whole premise of this machine is that
 * its only interface is natural language, and such a machine must not carry a
 * function with a deliberate bug in it — the same argument arch/x86_64/
 * fault_inject.c makes about not shipping a "crash yourself" verb. */
#ifdef FABLEOS_REPAIR_DEMO
static void demo_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void repair_demo_install(void) {
    agenda_item_t it;
    char          why[AGENDA_WHY_MAX];

    for (size_t i = 0; i < sizeof it; i++) ((char *)&it)[i] = 0;
    demo_copy(it.name, sizeof it.name, "repair-demo");
    demo_copy(it.tool, sizeof it.tool, "repair_demo");
    demo_copy(it.arg,  sizeof it.arg,  "{}");
    demo_copy(it.note, sizeof it.note,
              "divides by zero until the machine patches itself");
    it.when      = AGENDA_WHEN_EVERY;
    it.act       = AGENDA_DO_TOOL;
    it.enabled   = 1;
    it.period_ms = AGENDA_MIN_PERIOD_MS;
    it.max_runs  = 4;

    int rc = agenda_save(&it, why, sizeof why);
    kprintf("agenda: repair demo %s%s%s\n",
            rc == AGENDA_OK ? "scheduled every " : "NOT scheduled: ",
            rc == AGENDA_OK ? "10 s" : why,
            rc == AGENDA_OK
                ? " - it will fault, the machine will diagnose and patch "
                  "itself, and the next run will succeed"
                : "");
}
#else
static void repair_demo_install(void) { }
#endif

/* The sink an AGENDA_DO_SAY item hands its sentence to. A one-line wrapper
 * rather than a direct call so core/agenda.c links against neither net/chat.c
 * nor a transport and stays host-testable; see agenda_bind_say().
 *
 * THE kprintf BELOW PRINTS MODEL-AUTHORED TEXT RAW, AND THAT IS ONLY SAFE BECAUSE
 * OF WHERE THE DEFANGING IS. This line used to forge kernel trace lines: the
 * prefix is 60 characters, the framebuffer console is 128 columns, and
 * AGENDA_ARG_MAX is 288 — so 68 characters of filler put the model's next byte at
 * column ZERO of the following row, with no newline anywhere, and a sentence
 * ending "[vfs_write /etc/shadow bytes=9999 -> ok]" rendered a byte-exact kernel
 * statement between two real ones. Wrap is the third way to column zero and
 * net/chat.c had already documented it.
 *
 * core/agenda.c's arg_copy() now folds brackets out of a SAY sentence at the one
 * point every sentence passes through, on the dictation path AND the
 * store-loading path. Deliberately NOT re-implemented here: two implementations
 * of one rule is how the rule ends up applied in only one of the places. If this
 * function ever prints some OTHER piece of model text, that text needs the same
 * treatment at its own source. */
static int agenda_say(const char *sentence) {
    kprintf("\n[agenda] this machine is starting a turn nobody asked for: "
            "\"%s\"\n", sentence ? sentence : "");
    return chat_ask(sentence);
}

/* Called from the idle loop below, i.e. from the one place in this kernel that
 * is provably between two sentences AND still running.
 *
 * Both halves belong here for the same reason and neither belongs anywhere else:
 *
 *   faultchat_pump()  asks the model about a fault the machine has already
 *                     survived. It needs lwIP, mbedTLS and the heap, so it may
 *                     never run inside an exception handler — include/faultchat.h
 *                     makes that argument at length. Here the handler has long
 *                     since returned, the heap is quiescent and the network is
 *                     up. It is an integer compare when there is nothing to do.
 *   agenda_tick()     runs at most one due autonomous action. Also an integer
 *                     compare when nothing is due.
 *
 * Order matters: diagnose first, act second. If the last autonomous action
 * faulted, the diagnosis (and any code patch that comes out of it) must land
 * before the agenda runs anything again, or the machine retries the same broken
 * code with nothing changed and burns its bounds learning nothing.
 *
 * This is also the entire "with no human" claim: nothing above this line is
 * reached by typing, and the loop it sits in runs whether or not anybody ever
 * touches the keyboard. */
static void idle_work(void) {
    /* PROVABLY OUTSIDE A TURN, WHICH IS WHY THIS BELONGS HERE.
     *
     * chat_ask()'s re-entrancy guard is a flag set on the way in and cleared on
     * the way out, and a fault guard escaping out of a turn does not return
     * through "the way out": core/agenda.c runs a say item inside
     * fault_guard_run(), so a fatal CPU exception under chat_ask() would leave
     * the flag set and every later sentence would be refused for the rest of the
     * boot — a machine that survived the fault exactly as designed and can then
     * never be spoken to again, which is strictly worse than the bug the guard
     * exists to fix.
     *
     * This loop is the one place that is structurally outside every turn: it is
     * what CALLS chat_ask(). Saying so here rather than inside core/agenda.c also
     * keeps that module free of a link dependency on net/chat.c, which
     * include/agenda.h names as a deliberate property. A no-op in the normal
     * case. */
    chat_turn_ended();
    faultchat_pump();
    agenda_tick(millis());
}

static int wait_for_sentence(char *buf, int cap) {
    if (input_source_count() == 0) return INPUT_NONE;
    for (;;) {
        if (ui_fiber) fiber_yield();
        else          ui_inline();

        idle_work();

        /* THIS is where a capability call an app queued gets performed, and this
         * loop is the only place in the kernel that may do it: the machine is
         * between two sentences here, so it is genuinely idle. The ui fiber
         * deliberately does not, because it also runs inside network waits — see
         * ui_body(). With nothing queued this reads one flag and returns, so it
         * costs nothing on the hot path; ui_inline() already did it on the
         * no-fiber path, and doing it twice is harmless for the same reason. */
        (void)app_service();

        int n = input_poll(buf, cap);
        if (n != INPUT_NONE) return n;
        __asm__ volatile("pause");
    }
}

/* ======================================================================= */
/* Something to LOOK at while the machine thinks (diagnostic builds only)  */
/* ======================================================================= */

/* The counters printed around the boot self-test are the machine-checkable
 * proof that the ui fiber runs during an API call. They are not a picture, and
 * "the screen keeps moving" is the thing an operator actually cares about.
 *
 *     make EXTRA_CFLAGS=-DFABLEOS_CONCURRENCY_DEMO run
 *
 * opens apps/examples/clock.json at boot — a real document, through the real
 * app runtime, with a 500 ms tick handler — and leaves it up. Screenshot the
 * machine at any two moments during a model turn and the seconds differ; on
 * the pre-fiber kernel they could not, because nothing ran.
 *
 * Compiled out of every normal build, for the same reason as
 * arch/x86_64/fault_inject.c and tests/qemu/fixtures/ac97_boot.c: a window this
 * kernel opens by itself is a window the operator did not ask for, and the
 * premise here is that the machine does what it is asked. */
#ifdef FABLEOS_CONCURRENCY_DEMO
static void concurrency_demo(void) {
    const char *doc = app_example_doc("clock");
    if (!doc) { kputs("fiber: demo: no clock example in this build\n"); return; }

    uint32_t    id = 0;
    app_error_t err;
    if (app_launch(doc, 0, 24, 96, &id, &err) != APP_OK) {
        kprintf("fiber: demo: the clock would not launch (%s)\n", err.msg);
        return;
    }
    kprintf("fiber: demo: clock app open as window %u; its 500 ms tick handler "
            "runs in the ui fiber, so it keeps time through a model turn\n",
            (unsigned)id);
}
#else
static void concurrency_demo(void) { }
#endif

/* WHICH STACK THE FAULT HANDLER IS LOOKING AT.
 *
 * arch/x86_64/fault.c decides whether an armed fault guard may be escaped to, and
 * that is only sound if the context that faulted is the context that armed it —
 * the two bounds checks it makes cannot tell one stack from another, because every
 * fiber stack is inside the single declared stack window. It gets the answer
 * through this hook rather than by including fiber.h, so the fault handler keeps
 * linking without the scheduler (several host suites depend on that) and so the
 * dependency runs one way only. See the note beside fault_guard_t. */
static const void *current_stack_id(void) { return (const void *)fiber_self(); }

void kernel_main(void) {
    console_init();
    idt_init();               /* before anything can fault: see include/idt.h */
    time_init();              /* fiber_init() timestamps against millis() */
    /* Hand the scheduler the root stack's real bounds BEFORE it adopts this
     * context, so the root gets a poison band like every other fiber. What sits
     * directly below stack_bottom is p2_tables — the identity map — so an
     * unguarded overflow here is a triple fault with no report at all. See
     * "THE ROOT FIBER IS GUARDED NOW" in include/fiber.h. */
    fiber_root_stack_set(stack_bottom, stack_top);
    fiber_init();             /* adopt this context as fiber 0, "main" */
    fault_set_stack_id(current_stack_id);   /* after fiber_init: it must answer */
    drivers_init();           /* serial, pci, nic, keyboard, mouse */
    gui_init();               /* window manager; no windows, so nothing drawn */

    /* From here on the machine has somewhere else to be. Created before
     * net_init() on purpose: the DNS lookup and the boot HTTPS self-test are
     * the first multi-second waits of the boot, and there is no reason for the
     * screen to be dead through them. With no windows open gui_tick() is a
     * pointer compare and a return (gui/wm.c: gui_sync() early-outs on no
     * damage), so this costs nothing until there is something to draw. */
    ui_fiber = fiber_create("ui", ui_body, (void *)0, FIBER_STACK_DEFAULT);
    if (!ui_fiber)
        kputs("fiber: no ui fiber - the GUI will freeze during model turns, "
              "exactly as it did before fibers existed\n");

    /* Device model self-test: list the discovered device tree, then exercise
     * the driver power lifecycle (suspend everything capable, then resume). */
    kputs("\n--- device tree ---\n");
    driver_report();
    kputs("--- power cycle: suspend all, then resume all ---\n");
    drivers_suspend_all();
    drivers_resume_all();
    kputs("-------------------\n");

    vfs_selftest();

    /* After the filesystem, because the capability store is a directory in it,
     * and before the tool schema is ever assembled, because the list of saved
     * capabilities is advertised INSIDE that schema — see include/capability.h
     * under DISCOVERABILITY. One call: it binds the live list into the
     * capability_call description and then loads the store. */
    capability_bringup();

    /* The C compiler's store, for exactly the same two reasons and in exactly the
     * same shape: after the filesystem because the store is a directory in it,
     * before the first schema because the list of saved programs is advertised
     * inside cc_call's description. See include/cc.h under NAMED, SAVED, REUSED.
     *
     * Tell the compiler where the stack it will recurse on ends, FIRST, so
     * cc_bringup()'s report can check its budget against a real number. Asked of
     * the scheduler rather than read off stack_bottom directly, so that moving a
     * compile to another fiber changes the answer instead of silently keeping
     * the root's. */
    {
        void *stack_lo = (void *)0;
        if (fiber_stack_bounds(fiber_self(), &stack_lo, (void **)0) == 0)
            cc_stack_limit_set(stack_lo);
    }
    cc_bringup();

    concurrency_demo();

    /* Networking is required to *act* on anything — natural language is the
     * only interface, and the model lives on the far side of TLS. But a failure
     * here must not halt: the machine still boots, still reports its own state,
     * and still listens, so it can say why it cannot answer. */
    int net_up = (net_init() == 0);
    if (!net_up)
        kputs("\n[net init failed - no actions possible until networking works]\n");

    /* Boot self-test: prove the HTTPS round-trip works without needing input.
     * With no API key supplied over fw_cfg, Anthropic answers 401 — which still means
     * the TLS handshake, request, and response all succeeded. */
    if (net_up) {
        kputs("\n--- self-test: HTTPS to api.anthropic.com ---\n");

        /* THE DELIVERABLE, MEASURED. Before fibers this call was dead air:
         * one caller, spinning in net/net.c, for as long as DNS + the TLS
         * handshake + the model took. The three counters below are taken
         * either side of it and printed as a fact, so "the machine stays alive
         * while it thinks" is something the boot log states with numbers
         * instead of something a screenshot has to be trusted for. */
        uint64_t t0 = millis(), s0 = fiber_switches();
        uint64_t p0 = ui_passes, k0 = ui_ticks;

        net_ask("Reply with exactly: fable-os online");

        uint64_t ms = millis() - t0;
        kprintf("fiber: that %u ms API call was not dead air - the ui fiber "
                "serviced the screen %u times and fired %u app tick handler(s) "
                "during it, across %u context switches\n",
                (unsigned)ms, (unsigned)(ui_passes - p0),
                (unsigned)(ui_ticks - k0), (unsigned)(fiber_switches() - s0));
        kputs("---------------------------------------------\n");
    }

    /* The TLS round-trip above churns thousands of allocations. Verify the heap
     * is still structurally intact and report how much it reclaimed — proof the
     * allocator's free path works (in-use should be far below the peak). */
    heap_check();
    heap_dump();

    /* And the same question for the stacks. Every fiber stack has a poison
     * band at each end that fiber_yield() re-checks on every switch, so by the
     * time this prints, the bands have survived however many thousand switches
     * the self-test above cost. The high-water figure is measured, not
     * declared: it is what sizes FIBER_STACK_DEFAULT honestly. */
    fiber_report();

    /* Bind the turn loop to the real transport. NULL when the network never
     * came up, which chat_ask() reports rather than papering over. */
    chat_init(net_up ? model_tls_transport() : (model_transport_t *)0);

    /* THE KERNEL GETS ITS OWN VOICE ON THE TRANSPORT.
     *
     * chat_init() above binds the loop a human drives. This binds the one the
     * hardware drives: after a fault the machine has survived, net/faultchat.c
     * asks the model what happened and arms what comes back, with nobody having
     * typed anything. Bound to the same transport and to nothing when the
     * network never came up, which the pump reports rather than pretending. */
    faultchat_bind(net_up ? model_tls_transport() : (model_transport_t *)0);

    /* THE AGENDA. Last in the boot sequence, and every dependency is a reason:
     * after the filesystem, because the schedule is a file in it; after the tool
     * table exists, because an item names a tool and is refused if that tool is
     * not registered; after chat_init(), because a "say" item runs a whole model
     * turn. Its boot items are the first things this machine does on its own —
     * before the prompt is printed, so a reinstalled driver is in place before
     * anyone could have asked for it. */
    agenda_bind_say(agenda_say);
    kputs("\n--- agenda: what this machine does unprompted ---\n");
    agenda_bringup(millis());
    repair_demo_install();
    agenda_report();
    kputs("-------------------------------------------------\n");

    ready_banner(net_up);

    static char line[INPUT_LINE_MAX];
    for (;;) {
        /* The prompt states where the next line is going, because it is not
         * always the model. A click on a text field borrows the keyboard for
         * exactly one line — and gui_click_at() is reached by the model's
         * gui_click tool as well as by a physical click, so the model can arm it
         * without the operator asking. The one-line advisory printed at arm time
         * scrolls away behind the rest of a turn; a prompt cannot. Without this,
         * an operator types an instruction and it silently becomes the contents
         * of a text box, with no shell to check with and nothing to tell them. */
        const char *field = gui_key_target();
        if (field) kprintf("\n[typing into \"%s\", not to the model] > ", field);
        else       kputs("\n> ");

        int n = wait_for_sentence(line, sizeof line);
        if (n == INPUT_NONE) {
            kputs("\n[no input device - this machine cannot be spoken to]\n");
            for (;;) __asm__ volatile("hlt");
        }
        if (n == 0) continue;                    /* bare Enter: ask again */

        /* A sentence that lost its tail is a different sentence, and this one
         * gets executed. Refuse it loudly rather than act on half of it. */
        if (input_line_was_truncated()) {
            kprintf("[input truncated at %d characters - not sent. A cut-off "
                    "sentence could mean something you did not say; "
                    "say it again, shorter.]\n", n);
            continue;
        }

        /* A click on a text field in a window borrows the keyboard for exactly
         * one line (include/gui.h, DECISION 3). If one is armed, this line was
         * meant for that field and not for the model; the grab is consumed
         * either way, so the prompt can never be captured for longer. After the
         * truncation check, because a cut-off sentence is not what was typed and
         * must be refused before anything consumes it. */
        if (gui_take_line(line, n)) continue;

        /* One re-resolve per sentence while the model is unreachable.
         *
         * net_init() resolves the API host exactly once, so a single lost DNS
         * query at boot used to end the session: chat holds a NULL transport,
         * every sentence is refused, and the operator cannot ask for a retry
         * because asking is what needs the network. There is no shell to fall
         * back to and no other way in. Retrying here is the whole remedy — it
         * costs nothing when the network is up, and the attempt is bounded and
         * announced so a machine with no NIC answers promptly instead of
         * looking hung. Deliberately not a background timer: nothing else in
         * this kernel runs on its own, and a retry is only useful at the moment
         * somebody actually wants something done. */
        if (!net_up && net_retry_resolve() == 0) {
            net_up = 1;
            chat_init(model_tls_transport());
            kputs("[net recovered - the model is reachable again]\n");
        }

        chat_ask(line);
    }
}
