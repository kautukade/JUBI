/* app.h — the app runtime: the model writes a document, a real program appears.
 *
 * PURPOSE
 *   The operator types "hey I want a calculator" and a working calculator has to
 *   appear on screen. gui.h already provides windows, widgets, hit testing and an
 *   event loop, and gui/gui_demo.c proves a calculator can be built on top of them
 *   — in C, compiled in, by a human. This header is the part that removes the
 *   human: it defines WHAT AN APP IS when the model authors one at runtime, and
 *   the runtime (apps/runtime.c, apps/expr.c) that turns such a document into
 *   widgets, behaviour, and a window that can be clicked.
 *
 *   An app is a DOCUMENT, not code. It is parsed by net/json.c, validated in
 *   full before a single pixel is drawn, and executed by a total, bounded
 *   evaluator that cannot loop, cannot allocate, cannot fault and cannot reach
 *   anything but its own window.
 *
 * ============================================================================
 * DECISION 1 — WHY A DECLARATIVE DOCUMENT AND NOT CODE
 * ============================================================================
 *   Three candidates were real:
 *
 *     (a) THE MODEL EMITS C, OR MACHINE CODE. Rejected for the same reason
 *         dvm.h rejects it for drivers: there is no compiler on this machine, and
 *         the failure mode of a wrong jump on a single-threaded ring-0 kernel
 *         with all pages RWX is a triple fault with nothing to read afterwards.
 *
 *     (b) THE MODEL EMITS A PROGRAM FOR vm/dvm.c, the existing bounded VM. This
 *         was the strong candidate in the brief and it was examined closely,
 *         because reusing a sandbox is nearly always right. It was rejected on
 *         the VM's own contract, not on taste:
 *
 *           - dvm's opcodes ARE hardware primitives — in8/out8, MMIO loads,
 *             PCI config reads, delay. Not one of them can read or write a
 *             widget's text, which is the only thing an app's logic does. Using
 *             it would mean adding widget opcodes to vm/dvm.c, i.e. changing the
 *             ISA whose whole value is that it is small, allowlisted and already
 *             fuzzed against a synthetic device.
 *           - "No writable data memory" is a stated containment property of that
 *             VM. A calculator needs a display STRING and an accumulator. There
 *             is nowhere to put them.
 *           - Its values are unsigned 64-bit machine words with unsigned
 *             comparisons ("these are hardware values, not arithmetic"). A
 *             calculator needs signed fixed point, a decimal point, and text
 *             formatting.
 *           - Its trace is per-instruction device access. An app runs a handler
 *             on every click; that trace would be noise, and the kernel's
 *             ground-truth line for an app is "this widget was clicked and the
 *             display now reads 12".
 *
 *         Two sandboxes with different jobs is the honest outcome. dvm.h drives
 *         hardware the kernel has never seen; this drives a window. They share
 *         the design (bounded, allowlisted, refuse rather than guess, every
 *         failure legible) and not one line of code.
 *
 *     (c) A DECLARATIVE DOCUMENT plus a tiny expression language. Chosen.
 *         The whole document arrives through net/json.c, which is hardened,
 *         fuzzed, ASan-clean and already trusted with network data, so the
 *         attack surface facing model output is a parser that was attacked
 *         first. Layout is data (a grid), so it cannot be "wrong" in a way that
 *         corrupts anything — only in a way that looks bad. And the ONLY place
 *         real logic is needed — what a button does — is a small language with
 *         no loops, no functions, no memory and no I/O.
 *
 *   WHAT THE EXPRESSION LANGUAGE DELIBERATELY CANNOT DO. There is no loop and no
 *   recursion, so a handler's cost is bounded by its own length before it runs.
 *   There is no way to name a widget in another window, no way to open or close a
 *   window, no way to print to the console (which also means model-authored text
 *   can never put a '[' in column zero and forge a kernel trace line — see
 *   trace.h), no way to read the filesystem, memory or a device, and no way to
 *   call a tool. An expression is arithmetic and text, and nothing else.
 *
 *   The one thing an app CAN now reach outside its own window is a named
 *   capability, and it is a STATEMENT rather than an expression precisely so that
 *   the property above survives: see DECISION 4. Everything else stays a syscall
 *   the model makes itself, where it is traced.
 *
 * ============================================================================
 * DECISION 1b — WHAT A PERSON ACTUALLY ASKS FOR, AND WHAT THAT COST
 * ============================================================================
 *   The format above ran a calculator. The question that decided every addition
 *   below was the honest one: enumerate the apps somebody asks a computer for in
 *   one sentence, and check each against the format.
 *
 *     hello window, counter, tip calculator, unit/temperature converter, BMI,
 *     score keeper, shopping total, quiz          — EXPRESSIBLE ALREADY
 *     todo list (N fixed rows)                    — expressible, clumsily
 *     stopwatch, countdown timer, clock, alarm,
 *     pomodoro, anything that animates            — needed A PERIODIC EVENT
 *     clock, alarm, "what's the date"             — needed THE WALL CLOCK
 *     dice, coin flip, password, magic 8-ball,
 *     "guess the number", random picker           — needed RANDOMNESS
 *     tip / BMI / converter, shown to 2 decimals  — needed ROUNDING
 *     password, picking a character out of a set  — needed at()
 *     colour picker, progress bar, traffic light  — STILL MISSING: a widget
 *                                                   whose colour an app can set
 *     todo list you can delete from, any list     — STILL MISSING: a list widget
 *
 *   So five additions, each of which unlocks a family rather than one app:
 *   a "tick" event, `now`, the wall clock, `rand()`, `at()` and `round()`. The
 *   two that are still missing both need a widget gui.h does not have yet, and
 *   neither is a language question.
 *
 *   NONE OF THEM MAKES THIS A PROGRAMMING LANGUAGE, and that is the constraint
 *   they were designed against — a declarative document has to stay cheap to
 *   validate and safe to accept from a model:
 *
 *     - "tick" IS NOT A LOOP. A tick handler is the same bounded statement list a
 *       click runs, with the same APP_MAX_STEPS budget, run at most once per
 *       app_tick() call and never more often than APP_TICK_MIN_MS. app_tick()
 *       NEVER CATCHES UP: the next due time is computed from the reading it just
 *       took, so a handler delayed by ten seconds runs once, not a hundred times.
 *       At most APP_MAX_TICKS handlers per app can be periodic, so the work a
 *       document can demand per period is a number this header states.
 *     - `now`, `clock`, `today`, `hour`, `minute`, `second` are LEAVES, not
 *       calls: they read a snapshot taken once when the event began, so they
 *       cannot poll, cannot block, and cannot disagree with each other inside one
 *       handler. The wall clock is sampled at most once per event and only if the
 *       document mentions it.
 *     - `rand(n)` is one 64-bit draw and a multiply — no rejection loop, so no
 *       input can make it iterate. It reuses the machine's existing entropy
 *       source (lib/libc_shim.c's mbedtls_hardware_poll: RDRAND, TSC fallback).
 *     - `at(s,i)` and `round(x)` are pure, total, constant-time, and produce ERR
 *       instead of trapping, exactly like every operator that came before.
 *
 *   An app is therefore still a bounded function from (event, time snapshot,
 *   entropy, its own widgets) to its own widgets' text.
 *
 * ============================================================================
 * DECISION 2 — ERRORS ARE VALUES, SO EVALUATION IS TOTAL
 * ============================================================================
 *   A calculator must survive 1/0 and 999999999*999999999. The usual answers are
 *   to trap (the handler dies half-done, leaving the display holding a lie) or to
 *   wrap (the display holds a different lie). Instead there is a third value
 *   kind: ERR. Division by zero, overflow, num("abc") and arithmetic on a string
 *   all produce ERR; ERR propagates through every operator; text(ERR) is the word
 *   "error"; iserr(x) detects it. So no operation can fail, every handler runs to
 *   completion, and an app can latch a visible error state on purpose.
 *
 *   That is what makes the shipped calculator (apps/examples/calculator.json)
 *   able to say "error" and mean it, with no error handling in the runtime at
 *   all beyond a counter for the record.
 *
 * ============================================================================
 * DECISION 3 — VALIDATE EVERYTHING BEFORE ANY PIXEL
 * ============================================================================
 *   A document is compiled in full — every widget placed, every expression
 *   parsed, every name resolved, every bound checked — into a reserved instance
 *   slot, and only then is a window opened. A rejected document therefore leaves
 *   NOTHING behind: no window, no partial widget set, no slot. That is what makes
 *   the retry loop work: the tool answers with the exact JSON path, the reason,
 *   and (for an expression) the byte offset, the model fixes that one thing, and
 *   the second attempt is a fresh launch rather than a repair.
 *
 *   Every limit below is a static bound with a message that names it, because
 *   "too big" without a number costs the model another turn.
 *
 * ============================================================================
 * DECISION 4 — AN APP MAY CALL A CAPABILITY, AND THE KERNEL BROKERS EVERY ONE
 * ============================================================================
 *   Apps were sealed off from every driver and service on the machine: a
 *   model-built metronome could not tick audibly and a calculator could not beep.
 *   Real operating systems let an app ask the kernel for the hardware, so this
 *   format now has one more statement:
 *
 *     {"call":"audio.tone","with":{"hz":"440","ms":"120"},"into":"status"}
 *
 *   THE SHAPE, and why each part is the way it is:
 *     - "call" NAMES A CAPABILITY out of a static table (apps/cap.c). It is not a
 *       function name, a device, a path or an address — there is nothing in the
 *       format that can express any of those. A name that is not in the table is
 *       refused at LOAD time, with the whole table listed.
 *     - "with" is an object of EXPRESSION STRINGS, exactly like a `set`'s "to".
 *       That is the one syntactic decision that matters for extension: arguments
 *       are the same thing everywhere in this format, so "hz":"base * 2" works,
 *       nothing new had to be added to the language, and a future capability
 *       needs no format change at all — only a row in the table.
 *     - "into" is optional and names a var or widget that receives the OUTCOME:
 *       "ok" once the call really happened, or the sentence saying why not. So an
 *       app can display its own failure ("no audio driver is registered") instead
 *       of looking broken.
 *     - it is a STATEMENT, never an expression. Expressions stay pure, total and
 *       side-effect-free, which is what makes them safe to evaluate anywhere.
 *
 *   THE CAPABILITIES, AND THE BOUND ON EACH. One family exists today; the table
 *   is the enumeration, and apps/cap.c generates this list for the model too, so
 *   the documented bounds and the enforced bounds are the same numbers:
 *
 *     audio.tone   play one tone through whatever driver registered as the sink
 *       hz    REQUIRED. 0 (a deliberate rest) or AUDIO_HZ_MIN..AUDIO_HZ_MAX
 *             (20..20000). A 20 GHz tone is REFUSED, not clamped: a clamped
 *             request would report a sound that is not the one that was asked
 *             for, and a refusal that names the range is what lets a model fix
 *             itself next turn.
 *       ms    optional, default 120. 1..APP_CAP_AUDIO_MS_MAX (200). A 10-hour
 *             tone is refused by this bound. It is far below audio.h's own
 *             AUDIO_MAX_MS (1000) on purpose — see NOT STALLING THE MACHINE.
 *       amp   optional, default 0 = the kernel's own level. 0..AUDIO_AMP_MAX.
 *       wave  optional, default 'sine'. 'sine', 'square' or 'triangle' — an
 *             enumerated set, so no string from a document reaches a driver.
 *
 *     RATE: at most one sound is started per APP_CAP_GAP_MS (500 ms)
 *     machine-wide, and at most one request is in flight. A document asking for
 *     ten thousand tones a second therefore gets two a second and 9998 refusals,
 *     each with a sentence it can display. The refusals are counted in
 *     app_stats() and the last one is app_last_error().
 *
 *   THIS VALIDATION IS THE SECURITY BOUNDARY, AND THERE IS NO MMU BEHIND IT.
 *   State that plainly, because everything above reads like ordinary argument
 *   checking and it is not: this kernel is single-threaded and ring-0 only, with
 *   no privilege separation, no userspace, no memory protection and every page
 *   mapped RWX. A document is model output being interpreted by code with exactly
 *   the same authority as the driver it is asking for. There is no second line of
 *   defence under apps/cap.c — no page fault waiting to catch a bad number, no
 *   ring transition, no IOMMU. If a value from a document reached a driver
 *   unchecked, the only thing between it and the hardware would be that driver's
 *   own opinion of it. Which is why the format cannot name a driver, cannot pass
 *   a pointer, and cannot express an argument that is not a bounded number or one
 *   of an enumerated set of words.
 *
 *   NOT STALLING THE MACHINE. The event loop is polled, single-threaded, and the
 *   same loop serves the operator's prompt. So a handler never performs a call:
 *   app_cap_request() validates it and QUEUES it, and app_service() is the only
 *   code in apps/ that reaches a driver. A click therefore costs the same
 *   arithmetic time it did before this existed.
 *
 *   No sound starts while the machine is inside a model turn — and that is now a
 *   property of the call graph rather than a hope, which it briefly was not. The
 *   pump is reached only from app_tick(), and app_tick() belongs to the IDLE loop;
 *   kernel/main.c's ui fiber, which net/net.c yields to on every pass of every
 *   network wait, calls app_tick_handlers() instead and so cannot perform
 *   hardware. When those were one function, capability calls fired inside TLS
 *   round trips.
 *
 *   The honest remainder: audio.h's play contract is SYNCHRONOUS, because the
 *   kernel refills the one PCM buffer for the next sound and cannot ask a device
 *   where it is reading. So the drain blocks — for the length of the sound with a
 *   native sink, and for LONGER than that when the sink is a driver program, whose
 *   polling budget is the sound plus AUDIO_PLAY_MARGIN_US. APP_CAP_AUDIO_MS_MAX
 *   bounds what an app may ASK for; what bounds the stall is apps/cap.c measuring
 *   each call and keeping the machine quiet for at least that long again
 *   afterwards, so app sounds can never take as much as half of it. The prompt
 *   provably keeps answering (a sentence typed while a 120 ms sound repeats every
 *   second is echoed and answered between two sounds — measured, on a boot).
 *
 *   What that window really costs, stated because it is the sharpest edge here:
 *   input arriving DURING it is only as safe as the hardware's own buffer. A
 *   16550's FIFO is 16 bytes and the 8042's is one, so on real hardware a burst
 *   typed inside a 200 ms stall can be dropped by the UART rather than by this
 *   code. Under QEMU the chardev backend applies backpressure instead, so nothing
 *   is lost there and a test cannot see the difference — which is exactly why the
 *   bound is a small number and why this paragraph exists. A sound that is
 *   genuinely started and left running needs a start/poll split in the audio
 *   service, which is a change to that module and is listed under FUTURE
 *   EXTENSION POINTS.
 *
 *   EVERY PERFORMED CALL EMITS A KERNEL TRACE LINE. An app is model-authored, so
 *   the operator's only evidence is the same bracketed ground truth a tool call
 *   produces:
 *
 *       [app_call cap=audio.tone app=5 hz=440 ms=120 amp=8000 wave=sine -> ok]
 *
 *   Every field comes from kernel source or from a number the broker validated;
 *   the sink's own failure sentence is deliberately NOT in the line, because it
 *   can contain model bytes (a VM play program's `abort "reason"`). It reaches the
 *   app through "into" and app_last_error() instead, with control characters and
 *   brackets replaced — a reason of "\n[vfs_write -> ok]" must not be able to
 *   forge a kernel line anywhere. A REFUSED call touches no hardware and prints
 *   nothing: it is counted, and reported through "into" and app action=state, so
 *   that a document with a tick handler cannot fill the console with refusals.
 *
 * ============================================================================
 * THE DOCUMENT FORMAT
 * ============================================================================
 *   {
 *     "title":  "Calculator",           // window title, optional
 *     "width":  216, "height": 266,     // client-ish size in pixels, clamped
 *     "grid":   { "rows":6, "cols":4, "gap":5, "pad":6 },
 *     "vars":   { "acc":0, "op":"", "err":0 },
 *     "widgets": [
 *       { "kind":"field", "name":"display", "text":"0", "align":"right",
 *         "readonly":true, "row":0, "col":0, "colspan":4 },
 *       { "kind":"button", "text":"7", "tag":"digit", "row":1, "col":0 }
 *     ],
 *     "on": [
 *       { "click":"digit", "do":[ {"set":"display","to":"cat(display,key)"} ] }
 *     ]
 *   }
 *
 *   widgets[]  kind is button | label | field | panel. Placement is either grid
 *              (row/col/rowspan/colspan, exact-tiling via gui_grid) or an
 *              explicit "rect":[x,y,w,h] in client pixels. `name` is a unique
 *              handle; `tag` groups widgets so one handler serves ten digits.
 *              Optional flags: readonly, disabled, border, align.
 *
 *              TWO RULES THE RUNTIME APPLIES ON THE APP'S BEHALF, both because
 *              the alternative is an app that looks right and misbehaves:
 *                - a READONLY field is also made unclickable. A readout is
 *                  scenery, and gui_click resolves a widget by its visible text:
 *                  the moment a display reads "7", a click on "7" would be
 *                  ambiguous with the "7" key.
 *                - a widget any handler is BOUND TO is made clickable, even if
 *                  it is a label, a panel or a readonly field. Hanging a handler
 *                  on scenery would otherwise produce an app that never
 *                  responds, which is the hardest failure to diagnose from a
 *                  document that reads correctly. An explicit handler wins.
 *   vars       named cells, initialised from a JSON number or string. A var and
 *              a widget may not share a name — that would make an expression
 *              ambiguous, so it is refused rather than resolved by a rule nobody
 *              would remember.
 *   on[]       handlers. Exactly one event key per object:
 *                "click"  / "submit" — value is a widget name, a tag, or an
 *                                      array of either.
 *                "tick"             — value is a PERIOD IN MILLISECONDS
 *                                      (APP_TICK_MIN_MS..APP_TICK_MAX_MS). The
 *                                      handler runs that often for as long as the
 *                                      window is open, with `key` empty. This is
 *                                      what a clock, a stopwatch or a countdown
 *                                      is made of. At most APP_MAX_TICKS per app.
 *   statements {"set":<var|widget>,"to":"<expr>"} | {"if":"<expr>","then":[...],
 *              "else":[...]} | {"stop":true} |
 *              {"call":"<capability>","with":{...},"into":<var|widget>}
 *                      ask the kernel to do something the app cannot do itself.
 *                      Every "with" value is an expression string; "into" is
 *                      optional and receives the outcome. At most APP_MAX_CALLS
 *                      call statements per document. See DECISION 4 for the
 *                      capabilities, their bounds, and why this is the security
 *                      boundary.
 *   expressions infix, evaluated left to right with C-like precedence, no side
 *              effects on anything but this app. Operands: numbers (fixed point,
 *              six decimals), 'single-quoted' strings, var names, widget names
 *              (their current text), `key` — the text of the widget that fired
 *              the handler — and the time snapshot:
 *                now      monotonic seconds since boot, millisecond resolution
 *                clock    "HH:MM:SS" (UTC)     today   "YYYY-MM-DD"
 *                hour minute second            numbers, 0-23 / 0-59 / 0-59
 *              Every wall-clock name is ERR on a machine whose clock cannot be
 *              read; `now` always works.
 *              Operators: + - * / % == != < <= > >= && || ! and unary -.
 *              Functions: num text cat len digits has iserr abs min max
 *                round(x)   nearest integer, halves away from zero
 *                rand(n)    a fresh whole number in 0..n-1, n in 1..APP_RAND_MAX
 *                at(s,i)    the i'th BYTE of s as a one-character string,
 *                           counting from 0; ERR when i is outside s
 *
 *   NUMBERS ARE FIXED POINT: int64 scaled by 1,000,000, so six decimal places
 *   and a magnitude limit of APP_NUM_MAX (9e9). No FPU is used or needed (this
 *   kernel is built -mno-sse); multiply and divide go through __int128 so an
 *   intermediate cannot wrap, and anything outside the range is ERR, never a
 *   wrapped number.
 *
 * PUBLIC API
 *   lifecycle   app_launch app_close app_close_all
 *   periodic    app_tick
 *   capability  app_service app_cap_spec app_cap_reset
 *   environment app_set_time_source app_set_random_source
 *   queries     app_count app_at app_title app_is_app app_describe
 *               app_var_count app_var_at app_last_error
 *   stats       app_stats
 *   examples    app_example_calculator, app_example_count / _name / _doc
 *
 * DEPENDENCIES
 *   json.h to read the document, gui.h/widgets.h to instantiate it, and — only in
 *   apps/cap.c, only from app_service(), never from a handler — audio.h for the
 *   one capability that exists and trace.h to print the ground truth for it. The
 *   runtime proper still prints nothing, so it cannot interleave with the console
 *   mid-paint. No heap: instances live in a static pool, for the same reason
 *   gui.h's window pool does — an app that cannot start when memory is tight is an
 *   app that vanishes exactly when the operator needs to be told why. The only
 *   hardware is behind two replaceable function pointers — the wall clock and the
 *   entropy source — plus the capability broker, so the whole runtime, including
 *   the calculator, the clock and a beep into a synthetic sink, still runs on the
 *   host with no device model (tests/host/test_app.c, test_app_format.c,
 *   test_app_audio.c).
 *
 * FUTURE EXTENSION POINTS
 *   - Persisting an app is one VFS write: the document is the app, so "save that
 *     calculator" is fs_write of the bytes the model already sent.
 *   - MORE CAPABILITIES, which is what the shape in DECISION 4 was designed for: a
 *     row in apps/cap.c's table and a case in perform(), with no format change and
 *     no new syntax, because every argument is already an expression string. The
 *     three families worth having next, and the one bound each needs:
 *       fs.write / fs.read   a path allowlist (an app must not reach /etc), a byte
 *                            cap, and the same one-in-flight rule, since the VFS is
 *                            RAM-backed and a write is not instant.
 *       clock.alarm          nothing new: it is a tick handler plus a capability,
 *                            and the bound is APP_MAX_TICKS, which already exists.
 *       net.get              the only one that genuinely cannot be brokered as it
 *                            stands, because a TLS round trip is seconds long on a
 *                            polled kernel. It needs the asynchronous form below
 *                            first, or the machine stops answering while an app
 *                            fetches a web page.
 *   - AN ASYNCHRONOUS CAPABILITY. Everything above assumes a call is short enough
 *     to perform between two input polls, and audio.h's synchronous play contract
 *     is why an app's tone is capped at 200 ms. The honest fix is not a bigger
 *     constant here: it is a start/poll split in the service being brokered (for
 *     audio, "where is the device reading now?", which audio.h already lists as
 *     its own extension point), and then a third request state in apps/cap.c —
 *     started, and drained again on a later pass. The queue, the rate limit, the
 *     trace line and the "into" delivery are all already in the right place for
 *     that; only the perform() step would change.
 *   - Widget kinds are gui.h's business: when a list or a slider appears there,
 *     it becomes a `kind` string here and nothing else changes. The two shapes
 *     the enumeration above still cannot express both want one: a widget whose
 *     COLOUR an app can assign (colour picker, progress bar, traffic light) and a
 *     LIST (a todo list you can delete from). Neither is a language question:
 *     widgets.h already gives every widget an fg/bg, so the colour one is a
 *     `"color"` key plus letting `set` reach it.
 *   - The expression language has room for user functions (a name -> statement
 *     list map) without touching the evaluator, since the compiler already
 *     resolves names to indices at load time.
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <stddef.h>

/* ====================================================================== */
/* bounds — every one static, every one named in the error it causes       */
/* ====================================================================== */

/* Apps running at once. Four is two more than a 1024x768 screen shows
 * comfortably and keeps the static pool near 40 KiB. */
#define APP_MAX_INSTANCES   4

/* Document bytes accepted. A hand-written calculator is ~2.6 KB; 8 KiB leaves
 * room for something twice as elaborate while staying well inside the response
 * buffer a tool_use block arrives in (chat.h). */
#define APP_DOC_MAX         8192

/* Identifier length, including the NUL: var names, widget names, tags. */
#define APP_NAME_MAX        16

#define APP_MAX_VARS        16
#define APP_MAX_WIDGETS     48      /* == GUI_MAX_WIDGETS                    */
#define APP_MAX_HANDLERS    16

/* The smallest grid cell a launch will accept, in pixels.
 *
 * EIGHT, BECAUSE THE FONT CELL IS 8x16 AND A WIDGET NARROWER THAN ONE GLYPH
 * CANNOT SHOW ANYTHING OR BE AIMED AT. The geometry alone only breaks at zero:
 * gui_grid() returns an empty rect when a cell's edges coincide, so a window
 * two pixels too short produces widgets 0 pixels tall, and one four pixels
 * taller produces widgets 1 pixel tall - equally invisible, equally
 * unclickable, but silently accepted. Rejecting exactly at zero would leave a
 * band of documents that launch, report "N widget(s) ... a real mouse can click
 * it", and show an empty window. This constant moves the refusal to the first
 * size that is honestly usable, and the rejection quotes the width and height
 * the document needs, so the model's second attempt is arithmetic rather than a
 * guess. */
#define APP_MIN_CELL        8

/* Periodic ("tick") handlers per app, and the period they may ask for.
 *
 * FOUR, 50 ms AND AN HOUR ARE THE NUMBERS THAT BOUND PERIODIC WORK. The kernel
 * is single-threaded and polled: everything an app does happens between two
 * characters of the operator's sentence, so the question is not "is a tick fast"
 * but "what is the most work a document can demand per unit of time". It is
 * APP_MAX_TICKS * APP_MAX_STEPS statements per APP_TICK_MIN_MS per app —
 * 4 * 512 statements per 50 ms — and each statement is one bounded expression.
 * 50 ms is also 20 Hz, which is faster than the compositor can usefully repaint a
 * text field, so a shorter period would buy an app nothing and cost the machine
 * real time. An hour is the longest period worth expressing in a window that
 * lives until it is closed; a daily alarm compares `clock` instead. */
#define APP_MAX_TICKS       4
#define APP_TICK_MIN_MS     50
#define APP_TICK_MAX_MS     3600000

/* Largest n that rand(n) accepts. A million is far past dice, cards, character
 * sets and lottery numbers, and keeps the whole draw inside the fixed-point
 * range with no rounding of the bound itself. */
#define APP_RAND_MAX        1000000

/* ---- capability calls (DECISION 4) ------------------------------------- */

/* Longest sound one {"call":"audio.tone"} may ask for, in milliseconds, and the
 * minimum QUIET INTERVAL the kernel keeps after one finishes, machine-wide.
 *
 * READ THIS BEFORE TRUSTING EITHER NUMBER. audio.h's play contract is synchronous
 * — a driver's play() must not return until the device has finished reading the
 * buffer, because the kernel refills it for the next sound — so the time a sound
 * takes is time the single-threaded polled loop is not answering the prompt.
 *
 * APP_CAP_AUDIO_MS_MAX BOUNDS THE DURATION AN APP MAY ASK FOR. IT DOES NOT BOUND
 * HOW LONG THE MACHINE BLOCKS. This header used to say the two constants bounded
 * the stall to "200 ms in any 500 ms, i.e. at most 40% of the machine", and that
 * was measurably false in the configuration this project exists to produce. When
 * the audio sink is a driver program the model wrote, core/audio.c's run_vm()
 * raises that program's cumulative delay budget to the sound's length PLUS
 * AUDIO_PLAY_MARGIN_US (250 ms), and the program spends it polling the device
 * with mdelay(), which yields to nothing. A 200 ms app tone was measured holding
 * the machine for 450 ms, and because the old interval was timed from the moment
 * a sound STARTED, that 450 ms came out of the 500 ms gap: 90% of the machine,
 * not 40%.
 *
 * WHAT ACTUALLY BOUNDS IT NOW is measurement, in apps/cap.c: the pump reads the
 * clock either side of every call, and refuses the next one until the machine has
 * been free for at least as long as the last call blocked, PLUS APP_CAP_GAP_MS,
 * counted from the moment it finished. So the worst-case duty cycle is
 * block/(2*block + 500) — strictly under half however slow a driver is, and under
 * 4% for the 20-150 ms beeps a document actually asks for. A slow driver buys
 * itself a longer silence rather than a bigger share of the machine.
 *
 * APP_CAP_AUDIO_MS_MAX is still worth having, and is still far below audio.h's
 * own AUDIO_MAX_MS (1000), because it bounds the size of a single slice: a UI beep
 * is 60-150 ms, and a sound long enough to be music is the model's own audio_tone
 * tool, which is a traced tool call and not on this path.
 *
 * APP_CAP_QUIET_MAX_MS is the ceiling on a measured interval. A clock that jumps,
 * or a driver that spins its whole budget, must not be able to mute every app for
 * the rest of the boot; 4 s is well past any legitimate play and short enough that
 * a document recovers on its own. */
#define APP_CAP_AUDIO_MS_MAX  200u
#define APP_CAP_GAP_MS        500u
#define APP_CAP_QUIET_MAX_MS  4000u

#define APP_MAX_SELECTORS   8       /* names/tags one handler may bind        */
#define APP_MAX_STMTS       128
#define APP_MAX_EXPRS       96
#define APP_MAX_OPS         512     /* compiled expression ops, all handlers  */
#define APP_MAX_CONSTS      64      /* distinct numeric literals             */
#define APP_STRPOOL         768     /* bytes of string literals              */
#define APP_TEXT_MAX        40      /* == GUI_TEXT_MAX: a value's string      */

/* Structural nesting of "if" inside "do". Deeper is refused, so statement
 * execution's recursion is bounded before anything runs. */
#define APP_MAX_IF_DEPTH    8

/* Expression stack slots. The compiler computes each expression's high-water
 * mark and refuses one that would need more, so the evaluator cannot overflow. */
#define APP_STACK           16

/* Statements executed per event. There are no loops, so this is only reachable
 * by a pathological document; it exists so the bound is stated, not discovered. */
#define APP_MAX_STEPS       512

/* Fixed-point scale and magnitude limit. 1e6 gives six decimals; 9e9 is the
 * largest magnitude a value may hold (as gui/gui_demo.c's calculator uses). */
#define APP_NUM_SCALE       1000000LL
#define APP_NUM_MAX         9000000000000000LL

/* ====================================================================== */
/* errors                                                                 */
/* ====================================================================== */

#define APP_OK          0
#define APP_EINVAL    -22    /* the document is wrong; err says exactly how   */
#define APP_ENOSPC    -28    /* a static bound was exceeded; err names it     */
#define APP_ENOGUI    -19    /* no framebuffer, so no window manager          */
#define APP_EBUSY     -16    /* all APP_MAX_INSTANCES slots are in use        */

/* Why a document was refused, in the terms the model needs to fix it:
 *   path    where in the document, in JSON path form ("widgets[3].kind")
 *   msg     what is wrong and what was expected
 *   offset  byte offset inside an expression string, or -1
 * Always NUL-terminated; never contains a newline, so it composes into a
 * one-line tool result and can never break a trace line. */
typedef struct app_error {
    char path[72];
    char msg[224];
    int  offset;
} app_error_t;

/* Cumulative for the life of the machine. */
typedef struct app_stats {
    uint32_t launches;         /* documents accepted                          */
    uint32_t rejections;       /* documents refused (with a reason)           */
    uint32_t closes;
    uint32_t events;           /* handlers run                                */
    uint32_t steps;            /* statements executed                         */
    uint32_t sets;             /* assignments performed                       */
    uint32_t err_values;       /* ERR values produced by an operation         */
    uint32_t step_limits;      /* handlers cut off by APP_MAX_STEPS           */
    uint32_t ticks;            /* periodic handlers run (a subset of events)  */
    /* Capability calls (DECISION 4). `calls` counted them being ACCEPTED and
     * queued, `calls_refused` counted the ones a bound, the rate limit or a
     * missing driver turned away, and `hw_actions` counted the ones the kernel
     * actually performed — which is also the number of trace lines apps/ has
     * emitted. accepted != performed: an app that closes before the idle loop
     * drains its request does not get a sound. */
    uint32_t calls;
    uint32_t calls_refused;
    uint32_t hw_actions;
} app_stats_t;

/* ====================================================================== */
/* lifecycle                                                              */
/* ====================================================================== */

/* Compile `doc[0..len)` and, only if every part of it validates, open a window
 * for it. `len` may be 0 for a NUL-terminated document. x/y are screen pixels or
 * GUI_POS_AUTO. On success *out_id is the window id (which is also the app id)
 * and the return is APP_OK; on failure nothing at all is left behind and *err
 * (may be NULL) says why. The document is NOT retained: everything needed is
 * compiled, so the caller's buffer may be reused immediately. */
int app_launch(const char *doc, size_t len, int32_t x, int32_t y,
               uint32_t *out_id, app_error_t *err);

/* ====================================================================== */
/* the periodic event                                                     */
/* ====================================================================== */

/* Run every "tick" handler of every running app that is due, and return how many
 * ran. Cheap and non-blocking: with no periodic handler anywhere it reads nothing
 * and returns 0, so the idle loop can call it as often as it likes.
 *
 * WHERE IT BELONGS. Next to gui_tick() in the loop that waits for a sentence,
 * BEFORE it — a handler's assignments mark damage, and gui_tick()'s gui_sync()
 * is what puts the pixels on the screen:
 *
 *     for (;;) { app_tick(); gui_tick(); if (input_poll(...)) ... }
 *
 * IT CANNOT RUN AWAY. One call runs each due handler at most once and then sets
 * that handler's next due time from the reading it just took, so no backlog can
 * build up and no call can take longer than APP_MAX_TICKS handlers * their step
 * budgets. If the clock jumps backwards (a suspend, or a test installing another
 * source) a handler's due time is pulled back to at most one period away rather
 * than freezing until the clock catches up. */
int app_tick(void);

/* The tick handlers WITHOUT the capability pump: computation and widget text
 * only, no device access, no blocking, bounded by the same per-handler step
 * budgets as app_tick().
 *
 * This exists so that a caller running inside a network wait can keep documents
 * alive without also performing hardware. kernel/main.c's ui fiber is exactly
 * that caller: net/net.c's net_service() yields to it on every pass of every
 * network wait, so anything it calls runs DURING a model turn. Use this there,
 * and app_tick() (or app_service()) from the idle loop. See app_service(). */
int app_tick_handlers(void);

/* ====================================================================== */
/* the capability pump                                                    */
/* ====================================================================== */

/* Perform at most one capability call an app has asked for, and return 1 if one
 * was performed. THIS IS THE ONLY FUNCTION IN apps/ THAT CAN CAUSE A HARDWARE
 * EFFECT — a handler queues, this performs — which is what makes "a click cannot
 * block the prompt" a property of the call graph rather than a promise.
 *
 * WHERE IT BELONGS, AND WHERE IT MUST NOT GO. app_tick() calls it, and app_tick()
 * belongs in the IDLE loop (kernel/main.c's wait_for_sentence()), so a sound
 * starts between two input polls. It must NOT be reached from a fiber that runs
 * inside a network wait, because net/net.c's net_service() yields to such a fiber
 * on every pass of a DNS lookup or a model turn — which is how this invariant was
 * broken once already, when app_tick() became the ui fiber's body and capability
 * calls started firing inside TLS round trips. That is what app_tick_handlers()
 * is for. A second call site in a non-waiting loop is harmless: with nothing
 * queued this reads one flag and returns 0.
 *
 * IT CAN BLOCK, and for LONGER THAN THE SOUND IT PLAYS. audio.h's play path is
 * synchronous, so this returns only when the device has finished reading — and
 * when the audio sink is a driver program (the normal outcome of driver_install),
 * that program polls the device under a delay budget of the sound's length PLUS
 * AUDIO_PLAY_MARGIN_US, so a 200 ms sound can hold the machine for 450 ms.
 * APP_CAP_AUDIO_MS_MAX bounds the DURATION REQUESTED; it does not bound the
 * blocking time, and those are not the same number. What bounds the blocking time
 * is apps/cap.c's duty-cycle rule: the pump MEASURES how long each call blocked
 * and then refuses the next one until the machine has been free for at least that
 * long again, on top of APP_CAP_GAP_MS. So app sounds can never occupy as much as
 * half the machine, whatever a driver does — see DECISION 4. */
int app_service(void);

/* The capability specification a model reads, generated from the same table the
 * broker validates against so the two cannot drift: names, arguments, bounds,
 * defaults, the rate limit and the trace line. snprintf-style — returns the number
 * of bytes it WANTED to write. tools/app_tools.c prints it for action=caps, where
 * it costs nothing from the tool schema budget (chat.h). */
size_t app_cap_spec(char *dst, size_t cap);

/* Test hook: forget the queued call and the rate limiter. Nothing in the kernel
 * calls it; a suite needs it to start each case from a known state, exactly like
 * audio.h's audio_reset(). */
void app_cap_reset(void);

/* ====================================================================== */
/* where time and randomness come from                                    */
/* ====================================================================== */

/* A wall-clock reading, in UTC. Deliberately the same shape as rtc_time_t, and
 * deliberately NOT that type: apps/ must stay linkable without the RTC driver, so
 * that the whole app runtime keeps being testable on the host with no device
 * model at all. The kernel's default source is one rtc_read() call. */
typedef struct app_wall {
    int32_t year;                       /* e.g. 2026                        */
    uint8_t month, day;                 /* 1-12, 1-31                       */
    uint8_t hour, minute, second;       /* 0-23, 0-59, 0-59                 */
} app_wall_t;

typedef uint64_t (*app_ms_fn)(void);            /* monotonic milliseconds    */
typedef int      (*app_wall_fn)(app_wall_t *);  /* 0 = filled in, <0 = none  */
typedef uint64_t (*app_rand_fn)(void);          /* 64 fresh bits             */

/* Replace either source; NULL restores this machine's own. The defaults are
 * millis() plus the RTC, and the entropy source lib/libc_shim.c already uses for
 * TLS. These exist for the same reason rtc_set_backend() does: a test has to be
 * able to say what time it is and what the dice rolled, or "the stopwatch counts
 * up" is a claim about a screenshot instead of a proven property. */
void app_set_time_source(app_ms_fn ms, app_wall_fn wall);
void app_set_random_source(app_rand_fn fn);

/* Close a running app and release its slot. Returns APP_OK, or APP_EINVAL when
 * no app has that id. Closing the window by any other route (the close box,
 * gui_window_close, gui_close_all) releases the slot too — the runtime learns it
 * from GUI_EV_CLOSE, so there is exactly one teardown path. */
int app_close(uint32_t id);
void app_close_all(void);

/* ====================================================================== */
/* queries                                                                */
/* ====================================================================== */

int      app_count(void);
uint32_t app_at(int index);              /* 0 when out of range              */
int      app_is_app(uint32_t id);        /* 1 if that window is a launched app */
const char *app_title(uint32_t id);      /* "" when unknown                  */

/* Named vars and their current values, for the state view a tool prints.
 * `value` receives the value rendered as text (a number in the calculator's own
 * formatting, a string verbatim, or "error"). */
int  app_var_count(uint32_t id);
int  app_var_at(uint32_t id, int index, const char **name,
                char *value, size_t cap);

/* The last runtime note for this app: a step-limit cut-off or an ERR value that
 * reached a widget. "" when nothing has gone wrong. Never NULL. */
const char *app_last_error(uint32_t id);

/* One-line-per-widget human description (kind, id, name, text, and whether it
 * is clickable) for a tool result. Returns the number of bytes that WOULD be
 * written, snprintf-style.
 *
 * NOT the tag, and this line used to claim otherwise. A tag exists only while a
 * document is being compiled — it lives in the parser's scratch table, not in
 * app_inst_t, because nothing at run time needs it once handler masks are
 * resolved — so there is no tag here to print. The place a tag has to be
 * legible is the rejection a mistyped selector earns, and parse_selectors()
 * now lists every name AND every tag there. */
size_t app_describe(uint32_t id, char *dst, size_t cap);

const app_stats_t *app_stats(void);

/* ====================================================================== */
/* the shipped examples                                                   */
/* ====================================================================== */

/* The hand-written calculator document (apps/examples/calculator.json, compiled
 * in by apps/examples/calculator_json.h). It exists for the same reason
 * vm/programs/ac97_bringup.dvm does: prove the RUNTIME can run a calculator
 * before asking a model to write one, so that a model's failure is attributable
 * to the model. Never NULL. */
const char *app_example_calculator(void);

/* THE REST OF THE WORKED EXAMPLES, one per shape a person asks for in a sentence:
 * "hello" (a window that only says hello), "clock", "stopwatch", "dice",
 * "password", "beeper" (the worked example for DECISION 4's capability call: three
 * buttons that really make a sound, and a status line that shows the kernel's
 * answer when this machine has no audio driver) — plus "calculator". Each is a
 * real document from apps/examples/,
 * embedded by apps/examples/examples_json.h, small on purpose: they are also what
 * a model pattern-matches against when it is shown one.
 *
 * app_example_doc() returns NULL for an unknown name, and app_example_name()
 * returns NULL past the end, so a caller can list them without knowing any. */
int         app_example_count(void);
const char *app_example_name(int index);
const char *app_example_doc(const char *name);

#endif /* APP_H */
