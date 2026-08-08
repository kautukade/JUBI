/* test_agency.c — can this machine actually SOLVE a stated problem?
 *
 * tests/host/test_chat.c proves the turn loop is safe: bounded, unforgeable,
 * hostile-input-proof. That is a different question from the one here, which is
 * whether an operator can state a job and have the machine finish it. So this
 * suite drives whole multi-step jobs through the REAL loop (net/chat.c), the
 * REAL registry (core/tool.c), the REAL filesystem tools (tools/vfs_tools.c),
 * the REAL agent tools (tools/agent_tools.c) and the REAL ramfs, with the model
 * scripted by net/model_mock.c. Nothing is faked except the model's replies.
 *
 * Every scenario is asserted on three independent witnesses, because any one of
 * them alone is exactly what a convincing-but-idle machine would produce:
 *
 *   1. THE KERNEL'S TRACE LINES  (kcap_text) — emitted in C by lib/trace.c after
 *      each call returned. Proof that a step ran, and in what order.
 *   2. REAL KERNEL STATE         (vfs_open/vfs_read, chat_action_at) — read
 *      behind the tools' backs. Proof that the step DID something.
 *   3. WHAT WENT BACK ON THE WIRE (model_mock_request) — proof the model was
 *      given the results, the errors and the round budget it needs to carry on.
 *
 * The five jobs that matter:
 *   - a task needing several tools in sequence, ending verified;
 *   - a task whose first attempt fails and whose second succeeds;
 *   - a task that can only be answered by reading state back;
 *   - a turn killed by the round cap, whose work is still recoverable next turn;
 *   - a rate-limited link that must not throw completed work away.
 */

#include "harness.h"

#include "chat.h"
#include "model.h"
#include "json.h"
#include "tool.h"
#include "trace.h"
#include "vfs.h"
#include "fs.h"

#include <stdlib.h>

/* wait_ms PUMPS THE GUI AND THE APP RUNTIME while it waits, so agent_tools.c
 * references gui_tick() and app_tick(). This binary links the agent and vfs
 * tool families only — pulling in gui/wm.c and apps/runtime.c to satisfy two
 * calls would drag the compositor, the font, the framebuffer and the mouse
 * driver into a test about the agent tools. These are the whole of what those
 * two do when nothing is open (see include/gui.h's event-loop section and
 * app_tick()'s contract in include/app.h), so the stubs are faithful, and the
 * behaviour under test here — the wait budget and its arithmetic — does not
 * depend on either. tests/host/test_app_format.c owns the real app_tick. */
void gui_tick(void) { }
int  app_tick(void) { return 0; }

/* REGISTER_TOOL collects pointers in a linker section; Mach-O has no
 * __start_/__stop_ symbols, so alias them to the linker-generated ones. The
 * tools themselves are compiled as their own translation units (see the
 * FABLEOS_HOSTTEST branch in tools/vfs_tools.c and tools/agent_tools.c), so what
 * this suite exercises is the same table the kernel's linker builds. */
#ifdef __APPLE__
__asm__(".globl ___start_tool_table\n"
        ".set   ___start_tool_table, section$start$__DATA$tool_table\n"
        ".globl ___stop_tool_table\n"
        ".set   ___stop_tool_table, section$end$__DATA$tool_table\n");
#endif

/* ====================================================================== */
/* scripted model turns                                                    */
/* ====================================================================== */

#define MSG(blocks, stop) \
    "{\"id\":\"msg_x\",\"type\":\"message\",\"role\":\"assistant\"," \
    "\"content\":[" blocks "],\"stop_reason\":\"" stop "\"}"

#define TEXT(t)     "{\"type\":\"text\",\"text\":\"" t "\"}"
#define USE(id, nm, in) \
    "{\"type\":\"tool_use\",\"id\":\"" id "\",\"name\":\"" nm "\"," \
    "\"input\":" in "}"

/* One tool_use per round: the shape a real model produces when each step
 * depends on the last one's result. */
#define STEP(id, nm, in) MSG(USE(id, nm, in), "tool_use")
#define SAY(t)           MSG(TEXT(t), "end_turn")

/* ====================================================================== */
/* helpers                                                                 */
/* ====================================================================== */

static void fresh(void) {
    model_mock_reset();
    trace_reset();
    kcap_reset();
    chat_init(model_mock_transport());       /* also clears the action journal */
}

static void queue(const char *body) { CHECK_EQ(model_mock_queue(200, body), MODEL_OK); }

/* Read a file the way nothing the model controls can influence: straight through
 * the VFS. This is witness 2 — did the job actually happen? */
static size_t slurp(const char *path, char *dst, size_t cap) {
    file_t *f = vfs_open(path, O_RDONLY);
    if (!f) return (size_t)-1;
    int64_t n = vfs_read(f, dst, cap - 1);
    vfs_close(f);
    if (n < 0) n = 0;
    dst[n] = '\0';
    return (size_t)n;
}

/* Assert that `a` appears before `b` in the console transcript: the trace lines
 * are the record of the ORDER the machine did things in, which is most of what
 * "solved it step by step" means. */
static void check_order(const char *what, const char *a, const char *b) {
    const char *out = kcap_text();
    const char *pa  = out ? strstr(out, a) : NULL;
    const char *pb  = pa ? strstr(pa, b) : NULL;
    th_checks++;
    if (!pa || !pb) {
        th_fails++;
        printf("    FAIL %s: expected \"%s\" then \"%s\"\n  in: %s\n",
               what, a, b, out ? out : "(null)");
    }
}

/* Every request must be a valid alternating conversation, as test_chat.c
 * requires — repeated here because these turns are much longer and an eviction
 * or a mis-shaped tool_result block would show up first as a broken sequence. */
static void check_request_shape(const char *body) {
    json_value_t root, msgs, m;
    char role[16];

    CHECK(body != NULL);
    if (!body) return;
    CHECK_EQ(json_parse_str(body, &root), JSON_OK);
    CHECK_EQ(json_get(&root, "messages", &msgs), JSON_OK);

    size_t n = json_count(&msgs);
    CHECK(n > 0);
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(json_at(&msgs, i, &m), JSON_OK);
        CHECK_EQ(json_get_str(&m, "role", role, sizeof role), JSON_OK);
        CHECK_STR(role, (i % 2 == 0) ? "user" : "assistant");
    }
}

/* The mock keeps only the first MODEL_MOCK_REQ_CAP bytes of a request, and the
 * messages come last in the body — so a silently truncated recording would make
 * every "the model was told X" assertion vacuous. */
static void check_recorded_whole(size_t i) {
    CHECK_EQ(model_mock_request_truncated(i), 0);
}

/* Direct dispatch, for the argument-level and hostile-input passes where a whole
 * turn would only get in the way.
 *
 * tool_dispatch() returns TOOL_OK whenever the handler RAN — a logical failure
 * lives in out->is_error, which is also the flag the loop puts on the wire — so
 * that is what this reports. Returning the dispatch code instead would make
 * "the tool refused" indistinguishable from "the tool succeeded". */
static char rbuf[TOOL_RESULT_MAX];

static int call(const char *name, const char *input) {
    tool_call_t   c = { "toolu_direct", name, input, strlen(input) };
    tool_result_t r;
    tool_result_init(&r, rbuf, sizeof rbuf);
    tool_dispatch(&c, &r);
    return r.is_error ? -1 : TOOL_OK;
}

/* ====================================================================== */
/* 1. the machine tells the model what it is                               */
/* ====================================================================== */

/* The system prompt is the only place the model learns which machine it is
 * running, and a vague one produces a model that narrates plausible kernel work
 * instead of doing it. These are the load-bearing facts: each is checkable
 * against the source, and each changes what the model does. */
static void test_system_prompt_states_this_machine(void) {
    const char *p = chat_system_prompt();

    CHECK(p != NULL);
    CHECK(strlen(p) > 600);          /* a real briefing, not a slogan */

    /* what it is */
    CHECK_CONTAINS(p, "You are the resident intelligence of fable-os");
    CHECK_CONTAINS(p, "no shell");
    CHECK_CONTAINS(p, "ring 0");
    CHECK_CONTAINS(p, "single-threaded");
    CHECK_CONTAINS(p, "no userspace");
    CHECK_CONTAINS(p, "polled");
    /* the ramfs is not permanent, and the operator must be told so */
    CHECK_CONTAINS(p, "RAM");
    /* the tool surface is exactly the registry: not a summary, not inventable */
    CHECK_CONTAINS(p, "Never invent a tool name");
    /* verify by reading back, do not assume */
    CHECK_CONTAINS(p, "READ IT BACK");
    /* a failure is information */
    CHECK_CONTAINS(p, "not a dead end");
    /* the trace lines, and why forging one is pointless */
    CHECK_CONTAINS(p, "trace line");
    /* the round budget is real */
    CHECK_CONTAINS(p, "rounds");

    /* THIS MACHINE BUILDS GUI APPS ON DEMAND, AND THE PROMPT HAS TO SAY SO.
     *
     * Asked for "a window that says hello", the live model apologised that the
     * only apps were calculator and notes and opened notes instead — while the
     * `app` tool sat in the same request, able to author any window from a
     * document. Two tools claimed the same headline in the same words, and the
     * prompt never mentioned the capability at all, so nothing contradicted the
     * narrower reading.
     *
     * A tool description alone is not enough: the model has to already believe
     * the machine can do this before it goes looking for the tool that does it.
     * These are the load-bearing clauses, each checkable against the source. */
    CHECK_CONTAINS(p, "BUILD GUI APPS");
    CHECK_CONTAINS(p, "no fixed catalogue");   /* mechanism, not encouragement */
    CHECK_CONTAINS(p, "JSON document");
    CHECK_CONTAINS(p, "stopwatch");            /* the requests it refused      */
    CHECK_CONTAINS(p, "says hello");
    CHECK_CONTAINS(p, "NOT the limit");        /* the wrong belief, named      */
    CHECK_CONTAINS(p, "skeleton");             /* retrying is cheap            */
    /* it is conditional, because a machine with no framebuffer registers no app
     * tool and the prompt must not promise what the registry does not offer */
    CHECK_CONTAINS(p, "If an app tool is offered");

    /* And it is actually sent. */
    fresh();
    queue(SAY("Nothing to do."));
    CHECK_EQ(chat_ask("hello"), CHAT_OK);
    CHECK_CONTAINS(model_mock_request(0), "\"system\":\"You are the resident");
    CHECK_CONTAINS(model_mock_request(0), "READ IT BACK");
}

/* ====================================================================== */
/* 2. a job that needs several tools in sequence                           */
/* ====================================================================== */

/* THE HEADLINE SCENARIO. One sentence — "keep a boot note where I can find it" —
 * turned into six real kernel actions across six model round-trips: plan, make
 * the directory, write the file, read it back, mark the plan, answer.
 *
 * The assertions deliberately do not trust the model's closing sentence at all.
 * They check the kernel's trace lines for each step, then read the bytes out of
 * the ramfs directly, then check that each round's request actually carried the
 * previous result forward. */
static void test_multi_step_job_runs_to_completion(void) {
    fresh();

    queue(STEP("t1", "plan_set",
               "{\"goal\":\"a boot note under /var/log, verified\","
               "\"steps\":[\"create /var/log\",\"write the note\","
               "\"read it back to confirm\"]}"));
    queue(STEP("t2", "make_dir", "{\"path\":\"/var/log\",\"parents\":true}"));
    queue(STEP("t3", "write_file",
               "{\"path\":\"/var/log/boot.txt\",\"content\":\"booted ok\"}"));
    queue(STEP("t4", "read_file", "{\"path\":\"/var/log/boot.txt\"}"));
    queue(STEP("t5", "plan_mark",
               "{\"step\":3,\"state\":\"done\",\"note\":\"read back 9 bytes\"}"));
    queue(SAY("/var/log/boot.txt holds 9 bytes; I read them back to check."));

    CHECK_EQ(chat_ask("keep a boot note somewhere I can find it later"), CHAT_OK);

    /* --- witness 1: the kernel's own account, in order --- */
    check_order("plan then mkdir",  "[plan_set steps=3", "[vfs_mkdir /var/log");
    check_order("mkdir then write", "[vfs_mkdir /var/log", "[vfs_write /var/log/boot.txt");
    check_order("write then read",  "[vfs_write /var/log/boot.txt", "[vfs_read /var/log/boot.txt");
    check_order("read then mark",   "[vfs_read /var/log/boot.txt", "[plan_mark step=3/3");
    CHECK_CONTAINS(kcap_text(), "bytes=9");
    CHECK_EQ(trace_count(), 5);              /* five actions, no more, no fewer */
    CHECK_EQ(chat_last_tool_calls(), 5);
    CHECK_EQ(chat_last_rounds(), 6);         /* five tool rounds plus the answer */

    /* --- witness 2: the bytes are really there, read behind the tools --- */
    char got[64];
    CHECK_EQ(slurp("/var/log/boot.txt", got, sizeof got), 9u);
    CHECK_STR(got, "booted ok");

    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/var/log", &st), VFS_OK);
    CHECK_EQ((int)st.type, (int)VNODE_DIR);

    /* --- witness 3: each round carried the last result forward --- */
    CHECK_EQ(model_mock_request_count(), 6);
    for (size_t i = 0; i < 6; i++) {
        check_recorded_whole(i);
        check_request_shape(model_mock_request(i));
    }
    CHECK_CONTAINS(model_mock_request(1), "plan recorded, 3 steps");
    CHECK_CONTAINS(model_mock_request(2), "created directory /var/log");
    CHECK_CONTAINS(model_mock_request(3), "created /var/log/boot.txt and wrote 9");
    CHECK_CONTAINS(model_mock_request(4), "booted ok");
    /* nothing failed, so no is_error anywhere in the exchange */
    CHECK(strstr(model_mock_request(5), "is_error") == NULL);

    /* the round budget was visible in every tool_result turn */
    CHECK_CONTAINS(model_mock_request(1), "kernel: tool round 1 of 16 used");
    CHECK_CONTAINS(model_mock_request(5), "kernel: tool round 5 of 16 used");

    /* --- and the kernel's journal agrees with all three --- */
    CHECK_EQ(chat_action_total(), 5u);
    CHECK_EQ(chat_action_count(), 5u);
    CHECK_STR(chat_action_at(0)->name, "plan_set");
    CHECK_STR(chat_action_at(1)->name, "make_dir");
    CHECK_STR(chat_action_at(2)->name, "write_file");
    CHECK_STR(chat_action_at(3)->name, "read_file");
    CHECK_STR(chat_action_at(4)->name, "plan_mark");
    for (unsigned i = 0; i < 5; i++) CHECK_EQ(chat_action_at(i)->failed, 0);
    CHECK_CONTAINS(chat_action_at(2)->detail, "created /var/log/boot.txt");
}

/* ====================================================================== */
/* 3. the first attempt fails; the second succeeds                         */
/* ====================================================================== */

/* A failed call must be a usable instruction, not a wall. The model writes into
 * a directory that does not exist, reads the error, creates the parent and
 * writes again — the recovery loop the system prompt promises.
 *
 * What is really being tested is that the FAILURE TEXT reaches the model intact
 * and says what to do: "parent directory /srv does not exist; create it with
 * make_dir (parents=true) first". A tool that returned -2 instead would leave
 * the model guessing, and guessing is how a turn burns its whole budget. */
static void test_failure_then_recovery(void) {
    fresh();

    queue(STEP("f1", "write_file",
               "{\"path\":\"/srv/notes.txt\",\"content\":\"first try\"}"));
    queue(STEP("f2", "make_dir", "{\"path\":\"/srv\"}"));
    queue(STEP("f3", "write_file",
               "{\"path\":\"/srv/notes.txt\",\"content\":\"first try\"}"));
    queue(STEP("f4", "stat_path", "{\"path\":\"/srv/notes.txt\"}"));
    queue(SAY("The first write failed because /srv did not exist; I created it "
              "and the file is now 9 bytes."));

    CHECK_EQ(chat_ask("save a note in /srv/notes.txt"), CHAT_OK);

    /* the failure is on the record as a failure, in the kernel's voice ... */
    CHECK_CONTAINS(kcap_text(), "[vfs_write /srv/notes.txt -> ENOENT]");
    /* ... and the recovery follows it, in order */
    check_order("failed write then mkdir",
                "[vfs_write /srv/notes.txt -> ENOENT]", "[vfs_mkdir /srv");
    check_order("mkdir then successful write",
                "[vfs_mkdir /srv", "[vfs_write /srv/notes.txt mode=overwrite");
    check_order("write then verify",
                "[vfs_write /srv/notes.txt mode=overwrite", "[vfs_stat /srv/notes.txt");

    /* the model was told exactly what was wrong, and what to do about it */
    const char *after_fail = model_mock_request(1);
    CHECK_CONTAINS(after_fail, "\"is_error\":true");
    CHECK_CONTAINS(after_fail, "parent directory /srv does not exist");
    CHECK_CONTAINS(after_fail, "make_dir");
    CHECK_CONTAINS(after_fail, "1 failed");        /* the budget note counts it */
    check_request_shape(after_fail);

    /* and the second attempt really landed */
    char got[64];
    CHECK_EQ(slurp("/srv/notes.txt", got, sizeof got), 9u);
    CHECK_STR(got, "first try");

    /* the journal records the failure and the success as separate facts */
    CHECK_EQ(chat_action_total(), 4u);
    CHECK_EQ(chat_action_at(0)->failed, 1);
    CHECK_STR(chat_action_at(0)->name, "write_file");
    CHECK_EQ(chat_action_at(2)->failed, 0);
    CHECK_CONTAINS(chat_action_at(0)->detail, "parent directory /srv");

    /* the turn still ended with the operator being told the truth */
    CHECK_CONTAINS(kcap_text(), "The first write failed because /srv did not exist");
}

/* A mistake of the model's own making — a tool name that does not exist — is
 * recoverable for the same reason: the error carries the real list back. */
static void test_wrong_tool_name_is_recoverable(void) {
    fresh();

    queue(STEP("w1", "write_fileZ", "{\"path\":\"/tmp/x\",\"content\":\"hi\"}"));
    queue(STEP("w2", "write_file", "{\"path\":\"/tmp/x\",\"content\":\"hi\"}"));
    queue(SAY("Wrote /tmp/x."));

    CHECK_EQ(chat_ask("write hi to /tmp/x"), CHAT_OK);

    CHECK_CONTAINS(kcap_text(), "[tool_dispatch write_fileZ -> ENOENT]");
    const char *req = model_mock_request(1);
    CHECK_CONTAINS(req, "no such tool: write_fileZ");
    CHECK_CONTAINS(req, "write_file");           /* the real name is offered */
    CHECK_CONTAINS(req, "plan_set");             /* the whole surface, in fact */

    char got[16];
    CHECK_EQ(slurp("/tmp/x", got, sizeof got), 2u);
    CHECK_STR(got, "hi");

    /* an unknown name still becomes a journal entry: it was dispatched, and it
     * failed, and both of those are facts the next turn may need */
    CHECK_EQ(chat_action_at(0)->failed, 1);
    CHECK_STR(chat_action_at(0)->name, "write_fileZ");
}

/* ====================================================================== */
/* 4. a question that can only be answered by reading state back           */
/* ====================================================================== */

/* The operator asks about a file the model never touched, seeded here through
 * the VFS. The only way to the answer is a tool call, and the value in the
 * answer has to come from the kernel.
 *
 * The mock's closing sentence is scripted, so this cannot prove the model chose
 * the right words — what it proves is the part the kernel owns: that no answer
 * was printed before the read happened, and that the number the model was handed
 * is the real size of the real file. */
static void test_answer_requires_reading_state_back(void) {
    fresh();

    file_t *f = vfs_open("/etc/seeded", O_CREAT | O_RDWR);
    CHECK(f != NULL);
    if (f) { vfs_write(f, "0123456789abcdefghij", 20); vfs_close(f); }

    queue(STEP("r1", "stat_path", "{\"path\":\"/etc/seeded\"}"));
    queue(SAY("/etc/seeded is 20 bytes."));

    CHECK_EQ(chat_ask("how big is /etc/seeded?"), CHAT_OK);

    /* the read really happened, and the kernel's line carries the real size */
    CHECK_CONTAINS(kcap_text(), "[vfs_stat /etc/seeded type=file size=20 -> ok]");
    CHECK_EQ(trace_count(), 1);

    /* the model could not have answered before it read: the trace line comes
     * first in the transcript, the prose after */
    check_order("read before answer",
                "[vfs_stat /etc/seeded", "/etc/seeded is 20 bytes.");

    /* and the figure it was given is the kernel's, not a guess */
    CHECK_CONTAINS(model_mock_request(1), "type=file size=20");

    /* a claim with no call behind it leaves no trace and no journal entry —
     * which is how the operator can tell the two apart */
    fresh();
    queue(SAY("/etc/seeded is 999 bytes."));
    CHECK_EQ(chat_ask("how big is /etc/seeded?"), CHAT_OK);
    CHECK_EQ(trace_count(), 0);
    CHECK_EQ(chat_action_total(), 0u);
}

/* action_log is the same idea one level up: the model asking the kernel what it
 * did, rather than remembering. */
static void test_action_log_is_the_kernels_record(void) {
    fresh();

    queue(STEP("a1", "write_file", "{\"path\":\"/tmp/a\",\"content\":\"x\"}"));
    queue(STEP("a2", "write_file", "{\"path\":\"/nope/b\",\"content\":\"x\"}"));
    queue(STEP("a3", "action_log", "{}"));
    queue(SAY("Two writes: /tmp/a worked, /nope/b did not."));

    CHECK_EQ(chat_ask("write to /tmp/a and /nope/b, then tell me how it went"),
             CHAT_OK);

    /* The log the model was handed names both calls and marks the failure. It
     * lists TWO, not three: the loop records an action after the call returns, so
     * action_log cannot see itself. That is the right way round — an entry means
     * the call finished, not that it started. */
    const char *req = model_mock_request(3);
    CHECK_CONTAINS(req, "#1 turn=1 write_file ok:");
    CHECK_CONTAINS(req, "#2 turn=1 write_file FAILED:");
    CHECK_CONTAINS(req, "2 call(s) dispatched since boot");
    CHECK_CONTAINS(req, "not a summary of what was said");

    /* failed_only narrows it, and reading the log is itself an action */
    fresh();
    queue(STEP("b1", "write_file", "{\"path\":\"/nope/c\",\"content\":\"x\"}"));
    queue(STEP("b2", "action_log", "{\"failed_only\":true,\"limit\":4}"));
    queue(SAY("One failure."));
    CHECK_EQ(chat_ask("what failed?"), CHAT_OK);
    CHECK_CONTAINS(model_mock_request(2), "write_file FAILED");
    CHECK_CONTAINS(kcap_text(), "[action_log rows=1");
}

/* ====================================================================== */
/* 5. the round cap is a pause, not a dead end                             */
/* ====================================================================== */

/* A model that keeps calling tools is stopped — that is test_chat.c's contract
 * and it still holds. What this adds is the recovery: the turn is rolled back,
 * but the PLAN and the KERNEL'S RECORD are not, so the next sentence can find
 * out where the job got to and finish it. Before the journal and the plan
 * existed, "carry on" had nothing to carry on from. */
static void test_round_cap_leaves_the_job_recoverable(void) {
    fresh();
    chat_set_max_rounds(3);

    queue(STEP("c1", "plan_set",
               "{\"steps\":[\"make /work\",\"write /work/one\",\"verify\"]}"));
    queue(STEP("c2", "make_dir", "{\"path\":\"/work\"}"));
    queue(STEP("c3", "make_dir", "{\"path\":\"/work/sub\"}"));
    queue(STEP("c4", "make_dir", "{\"path\":\"/work/sub2\"}"));

    CHECK_EQ(chat_ask("build me a work tree"), CHAT_ELIMIT);
    CHECK_CONTAINS(kcap_text(), "stopped after 3 tool rounds");
    CHECK_CONTAINS(kcap_text(), "the kernel's record of them survives");

    /* the conversation is gone ... */
    CHECK_EQ(chat_history_messages(), 0u);
    /* ... but three real actions happened and are on the record */
    CHECK_EQ(chat_action_total(), 3u);
    CHECK_STR(chat_action_at(1)->name, "make_dir");

    /* The budget was visible from the first round, and the final request both
     * says it is the last AND turns tool calling off — which is what gives a real
     * model the chance to answer instead of being cut off mid-job.
     *
     * Note WHICH mechanism: tool_choice=none, with the schema still attached. The
     * history at this point references tools by id, so dropping the schema would
     * risk a 400 and turn the last round into an HTTP error instead of a report. */
    CHECK_CONTAINS(model_mock_request(1), "kernel: tool round 1 of 3 used, 2 left");
    CHECK_CONTAINS(model_mock_request(2),
                   "THIS IS YOUR LAST ROUND AND TOOL CALLING IS NOW OFF");
    /* The LAST clause too, not just the prefix. This note is the longest of the
     * variants and used to be silently cut mid-word at 287 bytes by a note[288],
     * losing the consequence that makes the model answer rather than call another
     * tool — while an assertion on the prefix alone stayed green. */
    CHECK_CONTAINS(model_mock_request(2), "the operator is told nothing.");
    CHECK_CONTAINS(model_mock_request(2), "\"tool_choice\":{\"type\":\"none\"}");
    CHECK_CONTAINS(model_mock_request(2), "\"tools\":[");
    check_request_shape(model_mock_request(2));       /* still a valid body */
    /* and the earlier rounds did NOT suppress tool calling */
    CHECK(strstr(model_mock_request(0), "tool_choice") == NULL);
    CHECK(strstr(model_mock_request(1), "tool_choice") == NULL);
    CHECK_CONTAINS(model_mock_request(0), "\"tools\":[");

    /* NEXT TURN: the operator says "carry on" and the machine finds its place */
    chat_set_max_rounds(0);
    model_mock_reset();
    kcap_reset();

    queue(STEP("d1", "plan_show", "{}"));
    queue(STEP("d2", "action_log", "{\"limit\":8}"));
    queue(STEP("d3", "write_file", "{\"path\":\"/work/one\",\"content\":\"done\"}"));
    queue(SAY("Picked the job back up: /work existed already, /work/one now has "
              "4 bytes."));

    CHECK_EQ(chat_ask("carry on"), CHAT_OK);

    /* the plan written during the ABANDONED turn is still there */
    const char *after_plan = model_mock_request(1);
    CHECK_CONTAINS(after_plan, "make /work");
    CHECK_CONTAINS(after_plan, "write /work/one");
    CHECK_CONTAINS(after_plan, "plan set in turn 1 (this is turn 2)");
    /* so is the record of what already ran */
    CHECK_CONTAINS(model_mock_request(2), "make_dir ok");

    /* and the job finished for real */
    char got[16];
    CHECK_EQ(slurp("/work/one", got, sizeof got), 4u);
    CHECK_STR(got, "done");
}

/* ====================================================================== */
/* 6. a rate-limited link must not throw completed work away               */
/* ====================================================================== */

/* Before this, one 429 in the middle of a five-step job abandoned the turn: the
 * mutations stayed, the conversation carrying them was rolled back, and the
 * operator was told the machine could not act. A retry is not optimism here, it
 * is the difference between finishing and losing the work. */
static void test_transient_http_error_is_retried(void) {
    fresh();

    queue(STEP("h1", "make_dir", "{\"path\":\"/retry\"}"));
    CHECK_EQ(model_mock_queue(429,
        "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\","
        "\"message\":\"slow down\"}}"), MODEL_OK);
    queue(STEP("h2", "write_file", "{\"path\":\"/retry/ok\",\"content\":\"y\"}"));
    queue(SAY("Done, after a rate limit."));

    CHECK_EQ(chat_ask("make /retry and put a file in it"), CHAT_OK);

    CHECK_CONTAINS(kcap_text(), "[model HTTP/1.1 429 Too Many Requests]");
    CHECK_CONTAINS(kcap_text(), "the API said try later; retrying, attempt 1 of 3");

    /* the retry did not cost a round: three answers, three rounds */
    CHECK_EQ(model_mock_request_count(), 4);
    CHECK_EQ(chat_last_rounds(), 3);

    /* the work done before the rate limit was kept, not repeated */
    CHECK_EQ(chat_action_total(), 2u);
    char got[8];
    CHECK_EQ(slurp("/retry/ok", got, sizeof got), 1u);

    /* a 401 is NOT retried: it will never succeed, and the operator needs to
     * know now */
    fresh();
    CHECK_EQ(model_mock_queue(401,
        "{\"type\":\"error\",\"error\":{\"message\":\"x-api-key required\"}}"),
        MODEL_OK);
    CHECK_EQ(chat_ask("hello"), CHAT_EHTTP);
    CHECK_EQ(model_mock_request_count(), 1);
    CHECK_CONTAINS(kcap_text(), "x-api-key required");
    CHECK(strstr(kcap_text(), "retrying") == NULL);
}

/* A dropped connection mid-job is the same argument. The mock's queue is empty
 * after the failure, which is not a retryable condition, so the loop stops
 * there — what matters is that the completed step survived and was reported. */
static void test_transport_failure_keeps_the_completed_work(void) {
    fresh();

    queue(STEP("x1", "make_dir", "{\"path\":\"/half\"}"));
    CHECK_EQ(model_mock_queue_error(MODEL_ETIMEOUT, "read timed out"), MODEL_OK);

    CHECK_EQ(chat_ask("make /half then tell me"), CHAT_ETRANSPORT);
    CHECK_CONTAINS(kcap_text(), "[vfs_mkdir /half");
    CHECK_CONTAINS(kcap_text(), "[model unreachable: read timed out]");
    CHECK_CONTAINS(kcap_text(), "retrying, attempt 1 of 3");

    /* the directory is really there, and the record says who made it */
    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/half", &st), VFS_OK);
    CHECK_EQ(chat_action_total(), 1u);
    CHECK_STR(chat_action_at(0)->name, "make_dir");

    /* and the machine is usable on the next sentence */
    model_mock_reset();
    queue(SAY("Still here."));
    CHECK_EQ(chat_ask("still there?"), CHAT_OK);
}

/* ====================================================================== */
/* 7. the plan and wait tools, on their own terms                          */
/* ====================================================================== */

/* Runs first, before anything sets a plan: the empty state is only observable
 * once, because a plan deliberately outlives chat_init(). */
static void test_plan_starts_empty(void) {
    CHECK_EQ(call("plan_show", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "no plan recorded");
    CHECK(call("plan_mark", "{\"step\":1,\"state\":\"todo\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "no plan to mark");

    CHECK_EQ(call("action_log", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "no tool calls recorded");
}

static void test_plan_readback_and_states(void) {
    fresh();

    CHECK_EQ(call("plan_set", "{\"steps\":[\"one\",\"two\"],\"goal\":\"g\"}"),
             TOOL_OK);
    CHECK_CONTAINS(rbuf, "2 steps");

    /* every write is readable back, byte for byte */
    CHECK_EQ(call("plan_show", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "1. [todo] one");
    CHECK_CONTAINS(rbuf, "2. [todo] two");
    CHECK_CONTAINS(rbuf, "goal: g");

    CHECK_EQ(call("plan_mark", "{\"step\":1,\"state\":\"doing\"}"), TOOL_OK);
    CHECK_EQ(call("plan_mark",
                  "{\"step\":1,\"state\":\"done\",\"note\":\"read it back\"}"),
             TOOL_OK);
    CHECK_EQ(call("plan_show", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "1. [done] one -- read it back");
    CHECK_CONTAINS(rbuf, "1 done, 0 failed, 0 doing, 1 todo");

    /* done/failed without evidence is refused: that is the case where a model is
     * most likely to be guessing */
    CHECK(call("plan_mark", "{\"step\":2,\"state\":\"done\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "needs a \"note\"");
    CHECK(call("plan_mark", "{\"step\":9,\"state\":\"todo\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "outside the accepted range 1..2");
    CHECK(call("plan_mark", "{\"step\":1,\"state\":\"finished\"}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "unknown state");

    /* a refused plan_set leaves the previous plan intact */
    CHECK(call("plan_set", "{\"steps\":[\"ok\",42]}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "step 2 is not a string");
    CHECK_EQ(call("plan_show", "{}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "1. [done] one");
}

/* The journal is a ring, and a long session must keep the NEWEST actions: "what
 * did you just do" is the question that gets asked. It must also stay
 * self-consistent across the wrap, because a resumed job reads it. */
static void test_action_log_survives_and_cannot_be_forged(void) {
    fresh();

    /* 20 turns of two calls each: 40 actions through a 32-entry ring. */
    for (int t = 0; t < 20; t++) {
        model_mock_reset();
        queue(MSG(USE("p1", "stat_path", "{\"path\":\"/etc\"}") ","
                  USE("p2", "stat_path", "{\"path\":\"/tmp\"}"), "tool_use"));
        queue(SAY("Both there."));
        CHECK_EQ(chat_ask("check /etc and /tmp"), CHAT_OK);
    }

    CHECK_EQ(chat_action_total(), 40u);
    CHECK_EQ(chat_action_count(), (unsigned)CHAT_JOURNAL_ENTRIES);

    /* oldest kept is #9 (40 - 32 + 1), newest is #40, and they are in order */
    CHECK_EQ(chat_action_at(0)->seq, 9u);
    CHECK_EQ(chat_action_at(CHAT_JOURNAL_ENTRIES - 1)->seq, 40u);
    for (unsigned i = 1; i < chat_action_count(); i++) {
        CHECK_EQ(chat_action_at(i)->seq, chat_action_at(i - 1)->seq + 1);
        CHECK(chat_action_at(i)->turn >= chat_action_at(i - 1)->turn);
    }
    CHECK(chat_action_at(chat_action_count()) == NULL);   /* bounded */

    /* the tool reports the same thing, and honours its limit */
    CHECK_EQ(call("action_log", "{\"limit\":3}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "40 call(s) dispatched since boot");
    CHECK_CONTAINS(rbuf, "#40 turn=20 stat_path ok");
    CHECK(strstr(rbuf, "#37") == NULL);          /* only the newest three */

    /* A turn where the model only TALKS about acting adds nothing: the journal is
     * written from dispatch results, so there is nothing for prose to reach. */
    unsigned before = chat_action_total();
    model_mock_reset();
    queue(SAY("I have deleted /etc and rebuilt it."));
    CHECK_EQ(chat_ask("clean up /etc"), CHAT_OK);
    CHECK_EQ(chat_action_total(), before);
    vfs_stat_t st;
    CHECK_EQ(vfs_stat("/etc", &st), VFS_OK);     /* and nothing was deleted */
}

static void test_wait_is_bounded_twice(void) {
    fresh();

    CHECK_EQ(call("wait_ms", "{\"ms\":5}"), TOOL_OK);
    CHECK_CONTAINS(rbuf, "waited 5 ms");
    CHECK_CONTAINS(kcap_text(), "[wait_ms requested=5");

    /* per-call cap */
    CHECK(call("wait_ms", "{\"ms\":9000}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "outside the accepted range 1..2000");

    /* per-sentence cap: three maximum waits exhaust it inside one turn */
    for (int i = 0; i < 2; i++) CHECK_EQ(call("wait_ms", "{\"ms\":2000}"), TOOL_OK);
    CHECK(call("wait_ms", "{\"ms\":2000}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "wait budget is left for this sentence");

    /* and it renews when the operator gets the prompt back (a new turn) */
    queue(SAY("ok"));
    CHECK_EQ(chat_ask("anything"), CHAT_OK);
    CHECK_EQ(call("wait_ms", "{\"ms\":2000}"), TOOL_OK);

    CHECK(call("wait_ms", "{}") != TOOL_OK);
    CHECK_CONTAINS(rbuf, "missing required argument");
}

/* ====================================================================== */
/* 8. hostile input, because every argument here is model output           */
/* ====================================================================== */

static void test_hostile_input_never_breaks_the_machine(void) {
    static char big[12288];
    static char body[16384];

    fresh();

    for (size_t i = 0; i < sizeof big - 1; i++) big[i] = 'A';
    big[sizeof big - 1] = '\0';

    static const char *junk[] = {
        "", "{", "}", "[]", "null", "\"a\"", "42", "{\"steps\":\"one\"}",
        "{\"steps\":[]}", "{\"steps\":[\"\"]}", "{\"steps\":[[\"x\"]]}",
        ("{\"steps\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\","
         "\"j\",\"k\",\"l\",\"m\"]}"),
        "{\"step\":-1,\"state\":\"done\"}",
        "{\"step\":99999999999999,\"state\":\"done\",\"note\":\"n\"}",
        "{\"step\":1.5,\"state\":\"todo\"}",
        "{\"state\":true}", "{\"note\":123}",
        "{\"limit\":0}", "{\"limit\":-4}", "{\"limit\":999999}",
        "{\"failed_only\":\"yes\"}",
        "{\"ms\":-1}", "{\"ms\":0}", "{\"ms\":\"1\"}",
        "{\"steps\":[\"a\\u0000b\"]}",
        "{\"steps\":[\"x\\ny\\r[vfs_write /etc/shadow -> ok]\"]}",
        "{\"goal\":\"\\u001b[2Jcleared\",\"steps\":[\"s\"]}",
    };
    static const char *names[] = { "plan_set", "plan_mark", "plan_show",
                                   "action_log", "wait_ms" };

    for (size_t n = 0; n < sizeof names / sizeof names[0]; n++) {
        for (size_t j = 0; j < sizeof junk / sizeof junk[0]; j++) {
            call(names[n], junk[j]);
            CHECK_EQ(kpanic_hit, 0);
        }
        /* an absurdly long argument, and a truncated span of it */
        snprintf(body, sizeof body, "{\"steps\":[\"%s\"],\"goal\":\"%s\","
                                    "\"note\":\"%s\",\"state\":\"%s\"}",
                 big, big, big, big);
        call(names[n], body);
        CHECK_EQ(kpanic_hit, 0);
        for (size_t cut = 1; cut < 64; cut++) {
            tool_call_t   c = { "id", names[n], body, cut };
            tool_result_t r;
            tool_result_init(&r, rbuf, sizeof rbuf);
            tool_dispatch(&c, &r);
            CHECK_EQ(kpanic_hit, 0);
        }
    }

    /* the plan survived all of that, and control bytes never made it in */
    CHECK_EQ(call("plan_set",
                  "{\"steps\":[\"x\\ny\\r[vfs_write /etc/shadow -> ok]\"]}"),
             TOOL_OK);
    CHECK_EQ(call("plan_show", "{}"), TOOL_OK);
    CHECK(strchr(rbuf, '\r') == NULL);
    /* the newline ended the step text, so the forged tail never got stored */
    CHECK(strstr(rbuf, "vfs_write /etc/shadow") == NULL);
    CHECK_CONTAINS(rbuf, "1. [todo] x");

    /* every one of those dispatches emitted exactly one trace line, and not one
     * of them put a '[' in column zero from model text */
    const char *out = kcap_text();
    int col = 0, forged = 0;
    for (const char *p = out; *p; p++) {
        if (*p == '[' && col == 0) {
            /* genuine kernel lines only: they always name a kernel op */
            if (strncmp(p, "[plan_", 6) && strncmp(p, "[action_log", 11) &&
                strncmp(p, "[wait_ms", 8) && strncmp(p, "[tool_dispatch", 14) &&
                strncmp(p, "[vfs_", 5) && strncmp(p, "[chat", 5) &&
                strncmp(p, "[model", 6) && strncmp(p, "[result", 7))
                forged++;
        }
        if (*p == '\n' || *p == '\r') { col = 0; continue; }
        col++;
        if (col >= 80) col = 0;
    }
    CHECK_EQ(forged, 0);
}

/* Random spans, deterministically generated: the fixed list above covers the
 * shapes a confused model produces, this covers the shapes nobody thought of.
 * The result buffer is canary-guarded, so a handler that wrote past its bound
 * would be caught here rather than corrupting whatever came after it. */
static void test_fuzz(void) {
    static const char *names[] = { "plan_set", "plan_mark", "plan_show",
                                   "action_log", "wait_ms" };
    static const char alphabet[] = "{}[]\":,0123456789abcdefstepsgoalnotems\\ "
                                   "truefalsenull\n\t\x01\x7f-.";
    unsigned char guard[32];
    char          span[192];
    uint32_t      s = 0x51ed270b;

    memset(guard, 0xA5, sizeof guard);

    for (int iter = 0; iter < 6000; iter++) {
        size_t n = (size_t)(iter % (sizeof span - 1)) + 1;
        for (size_t i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            span[i] = alphabet[(s >> 16) % (sizeof alphabet - 1)];
        }
        span[n] = '\0';

        struct { char buf[512]; unsigned char post[32]; } g;
        memset(g.post, 0xA5, sizeof g.post);

        tool_call_t   c = { "toolu_fuzz", names[(size_t)iter % 5], span, n };
        tool_result_t r;
        tool_result_init(&r, g.buf, sizeof g.buf);
        tool_dispatch(&c, &r);

        CHECK_EQ(kpanic_hit, 0);
        CHECK_EQ(memcmp(g.post, guard, sizeof g.post), 0);
        CHECK(r.len < sizeof g.buf);
        CHECK_EQ(g.buf[r.len], '\0');
    }
}

/* ====================================================================== */
/* 9. the surface itself                                                   */
/* ====================================================================== */

static void test_registration_and_schema(void) {
    /* the agent family is registered and reachable by name */
    static const char *want[] = { "plan_set", "plan_mark", "plan_show",
                                  "action_log", "wait_ms" };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        const tool_t *t = tool_find(want[i]);
        CHECK(t != NULL);
        if (!t) continue;
        CHECK(t->description && strlen(t->description) > 40);
        CHECK(t->input_schema != NULL);
        json_value_t v;
        CHECK_EQ(json_parse_str(t->input_schema, &v), JSON_OK);
    }

    /* the two that change kernel state say so, and the readers do not */
    CHECK_TOOL_FLAGS("plan_set", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("plan_mark", TOOL_MUTATES, TOOL_MUTATES);
    CHECK_TOOL_FLAGS("plan_show", TOOL_MUTATES, 0);
    CHECK_TOOL_FLAGS("action_log", TOOL_MUTATES, 0);

    /* EVERY WRITE HAS A READ. This is the property that lets the model verify
     * its own work instead of assuming, so it is asserted as a property of the
     * linked surface rather than left to review. */
    CHECK(tool_find("plan_set")   && tool_find("plan_show"));    /* plan     */
    CHECK(tool_find("write_file") && tool_find("read_file"));    /* contents */
    CHECK(tool_find("make_dir")   && tool_find("list_dir"));     /* dirs     */
    CHECK(tool_find("delete_path")&& tool_find("stat_path"));    /* existence*/

    /* and the whole registry still serialises into something sendable */
    static char schema[CHAT_TOOLS_BYTES];
    size_t n = tools_build_schema(schema, sizeof schema);
    CHECK(n > 0 && n < sizeof schema);
    json_value_t v;
    CHECK_EQ(json_parse(schema, n, &v), JSON_OK);
    CHECK_EQ((int)v.type, (int)JSON_ARRAY);
    CHECK_EQ((int)json_count(&v), tool_count());

    /* THE HEADROOM, AND WHY IT IS ASSERTED HERE RATHER THAN GUESSED.
     *
     * tools_load() refuses the ENTIRE array if it does not fit CHAT_TOOLS_BYTES,
     * leaving the model with no tools at all — so the interesting number is not
     * "does it fit" but "by how much". This binary links only the agent and vfs
     * families, so it cannot measure the real 45-tool total; the boot banner
     * does that, and chat.h's CHAT_REGISTRY_BYTES is where the banner's number
     * is written down. `make test-qemu` fails if the two ever disagree, which
     * is the guard this assertion lacked when its literal drifted 699 bytes
     * below the truth while this suite kept printing it as a measured fact.
     *
     * What CAN be asserted here, and is the thing that actually breaks, is the
     * SECOND bound: one request must hold the tool array, the system prompt and
     * a full history inside CHAT_REQ_BYTES, or the loop starts evicting live
     * exchanges on every send — the machine silently forgetting mid-job. The
     * `app` and `gui_open` descriptions were deliberately grown to make the app
     * capability discoverable, so this checks that spend against the real cliff
     * (chat.h documents the arithmetic) using the measured registry total. */
    const size_t registry_total = CHAT_REGISTRY_BYTES;   /* chat.h; see above */
    const size_t framing        = 1400;    /* request line + headers, chat.h  */
    size_t worst = registry_total + CHAT_HISTORY_BYTES +
                   strlen(chat_system_prompt()) + framing;
    printf("    (tools %zu + history %d + prompt %zu + framing %zu = %zu of "
           "CHAT_REQ_BYTES %d; CHAT_TOOLS_BYTES headroom %zu)\n",
           registry_total, CHAT_HISTORY_BYTES, strlen(chat_system_prompt()),
           framing, worst, CHAT_REQ_BYTES,
           (size_t)CHAT_TOOLS_BYTES - registry_total);
    CHECK(registry_total < CHAT_TOOLS_BYTES);
    CHECK(worst < CHAT_REQ_BYTES);
}

/* ====================================================================== */

/* Mirrors the kernel's boot filesystem (kernel/main.c's fs_init and self-test)
 * so these jobs run against the same shape the operator would meet. */
static void boot_fs(void) {
    vfs_init();
    ramfs_register();
    if (vfs_mount("/", "ramfs") != VFS_OK) { printf("mount failed\n"); exit(2); }
    vfs_mkdir("/etc");
    vfs_mkdir("/tmp");
}

int main(void) {
    console_init();
    boot_fs();

    RUN(test_plan_starts_empty);            /* before anything writes a plan */
    RUN(test_system_prompt_states_this_machine);
    RUN(test_multi_step_job_runs_to_completion);
    RUN(test_failure_then_recovery);
    RUN(test_wrong_tool_name_is_recoverable);
    RUN(test_answer_requires_reading_state_back);
    RUN(test_action_log_is_the_kernels_record);
    RUN(test_round_cap_leaves_the_job_recoverable);
    RUN(test_transient_http_error_is_retried);
    RUN(test_transport_failure_keeps_the_completed_work);
    RUN(test_plan_readback_and_states);
    RUN(test_action_log_survives_and_cannot_be_forged);
    RUN(test_wait_is_bounded_twice);
    RUN(test_hostile_input_never_breaks_the_machine);
    RUN(test_fuzz);
    RUN(test_registration_and_schema);

    return th_report("agency");
}
