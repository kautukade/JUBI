/* app_calculator.h — a recorded session: the model builds a calculator.
 *
 * WHAT THIS IS
 *   Every string below is a Messages API response body of the shape
 *   api.anthropic.com returns, in the order it was returned. Queued into
 *   net/model_mock.c they replay the whole capability — chat_ask -> tool_use ->
 *   tool_dispatch -> app_launch -> a rejection the model reads -> a corrected
 *   document -> a window -> four clicks -> "15" — with no network, no API key and
 *   no QEMU. tests/host/test_app.c is the replay.
 *
 *   Without this, "the model built a calculator" is an anecdote. With it, it is a
 *   test that fails the build when the loop stops working.
 *
 * HONESTY ABOUT WHAT IS RECORDED
 *   This kernel is run without a key here (the key never enters the build), so a
 *   live model has never reached these tools. THE ASSISTANT TURNS BELOW ARE
 *   AUTHORED, NOT CAPTURED FROM A LIVE MODEL. What is real is everything on the
 *   kernel side of the seam: the turn loop, the tool dispatch, the document
 *   parser, the rejection text, the layout, the widget instantiation, the click
 *   dispatch through the same path a PS/2 mouse uses, and the arithmetic. The
 *   window is a malloc'd framebuffer, because a host test has no screen.
 *
 *   Read this as: "when a model does write this document, this is exactly what
 *   happens" — proved for the machinery, pending for the model.
 *
 * THE SESSION
 *   > hey I want a calculator
 *
 *   round 1  app action=format                 -> the format and a worked example
 *   round 2  app action=launch (CALC_DOC_BAD)  -> REFUSED at on[0].click:
 *                                                 no widget has the name or tag
 *                                                 "digits"; the reply lists the
 *                                                 names that DO exist
 *   round 3  app action=launch (CALC_DOC_GOOD) -> window id=1, 8 widgets
 *   round 4  gui_click x4: 7, +, 8, =          -> the display reads 15
 *   round 5  app action=state                  -> reads it back out of the app
 *   round 6  prose: "there it is, 7 + 8 = 15"
 *
 *   The bug in attempt 1 is the realistic kind: the handler selects the tag
 *   "digits" while the widgets declare "digit". Nothing in the format text says
 *   which one this document used — the model has to read the rejection, which is
 *   the entire argument for making rejections specific.
 *
 * EDITING
 *   The documents are JSON inside JSON inside a C string: the app document is a
 *   nested object in the tool_use "input", so a document's own `"` is written \"
 *   here and nothing needs double-escaping beyond that. Keep the two versions
 *   differing by exactly the one fix described above: the diff is the evidence
 *   that the loop converged for a reason.
 */
#ifndef APP_TRANSCRIPT_CALCULATOR_H
#define APP_TRANSCRIPT_CALCULATOR_H

/* ---- what the operator typed ---- */
#define AR_SENTENCE "hey I want a calculator"

/* ---- response envelopes ---- */
#define AR_MSG(blocks, stop)                                                   \
    "{\"id\":\"msg_01TranscriptAppCalculator\",\"type\":\"message\","          \
    "\"role\":\"assistant\",\"model\":\"claude-transcript\",\"content\":["     \
    blocks "],\"stop_reason\":\"" stop "\"}"

#define AR_TEXT(t)  "{\"type\":\"text\",\"text\":\"" t "\"}"
#define AR_USE(id, name, input)                                                \
    "{\"type\":\"tool_use\",\"id\":\"" id "\",\"name\":\"" name "\","          \
    "\"input\":" input "}"

/* ======================================================================= */
/* the document, as it was submitted                                       */
/* ======================================================================= */

/* A four-function calculator small enough to read here and real enough to be
 * wrong in an interesting way. Everything except the handler bindings is shared
 * between the two attempts. */
#define CALC_HEAD                                                              \
    "{\"title\":\"Calculator\",\"width\":180,\"height\":220,"                  \
    "\"grid\":{\"rows\":4,\"cols\":3,\"gap\":5,\"pad\":6},"                    \
    "\"vars\":{\"acc\":0,\"rhs\":0,\"res\":0,\"op\":\"\",\"fresh\":1},"        \
    "\"widgets\":["                                                            \
    "{\"kind\":\"field\",\"name\":\"display\",\"text\":\"0\","                 \
      "\"align\":\"right\",\"readonly\":true,"                                 \
      "\"row\":0,\"col\":0,\"colspan\":3},"                                    \
    "{\"kind\":\"button\",\"text\":\"7\",\"tag\":\"digit\",\"row\":1,\"col\":0},"\
    "{\"kind\":\"button\",\"text\":\"8\",\"tag\":\"digit\",\"row\":1,\"col\":1},"\
    "{\"kind\":\"button\",\"text\":\"+\",\"tag\":\"op\",\"row\":1,\"col\":2},"  \
    "{\"kind\":\"button\",\"text\":\"1\",\"tag\":\"digit\",\"row\":2,\"col\":0},"\
    "{\"kind\":\"button\",\"text\":\"C\",\"name\":\"clr\",\"row\":2,\"col\":1},"\
    "{\"kind\":\"button\",\"text\":\"-\",\"tag\":\"op\",\"row\":2,\"col\":2},"  \
    "{\"kind\":\"button\",\"text\":\"=\",\"name\":\"eq\",\"row\":3,\"col\":0,"  \
      "\"colspan\":3}"                                                         \
    "],\"on\":["

/* The digit handler, whose selector is the only difference between the two
 * attempts. */
#define CALC_DIGITS(sel)                                                       \
    "{\"click\":\"" sel "\",\"do\":["                                          \
    "{\"if\":\"fresh\",\"then\":[{\"set\":\"display\",\"to\":\"''\"},"          \
                                "{\"set\":\"fresh\",\"to\":\"0\"}]},"          \
    "{\"if\":\"digits(display) >= 9\",\"then\":[{\"stop\":true}]},"            \
    "{\"if\":\"display == '0'\",\"then\":[{\"set\":\"display\",\"to\":\"''\"}]},"\
    "{\"set\":\"display\",\"to\":\"cat(display, key)\"}]}"

#define CALC_TAIL                                                              \
    ",{\"click\":[\"op\",\"eq\"],\"do\":["                                     \
    "{\"set\":\"rhs\",\"to\":\"num(display)\"},"                               \
    "{\"if\":\"op == ''\",\"then\":[{\"set\":\"res\",\"to\":\"rhs\"}]},"        \
    "{\"if\":\"op == '+'\",\"then\":[{\"set\":\"res\",\"to\":\"acc + rhs\"}]},"\
    "{\"if\":\"op == '-'\",\"then\":[{\"set\":\"res\",\"to\":\"acc - rhs\"}]},"\
    "{\"set\":\"acc\",\"to\":\"res\"},"                                        \
    "{\"set\":\"display\",\"to\":\"text(res)\"},"                              \
    "{\"if\":\"key == '='\",\"then\":[{\"set\":\"op\",\"to\":\"''\"}],"         \
                          "\"else\":[{\"set\":\"op\",\"to\":\"key\"}]},"        \
    "{\"set\":\"fresh\",\"to\":\"1\"}]}"                                       \
    ",{\"click\":\"clr\",\"do\":["                                             \
    "{\"set\":\"acc\",\"to\":\"0\"},{\"set\":\"op\",\"to\":\"''\"},"            \
    "{\"set\":\"fresh\",\"to\":\"1\"},{\"set\":\"display\",\"to\":\"'0'\"}]}"  \
    "]}"

/* Attempt 1: the widgets are tagged "digit" and the handler selects "digits".
 * Nothing is launched. */
#define CALC_DOC_BAD   CALC_HEAD CALC_DIGITS("digits") CALC_TAIL

/* Attempt 2: the same document with the selector corrected. The only change. */
#define CALC_DOC_GOOD  CALC_HEAD CALC_DIGITS("digit")  CALC_TAIL

/* ======================================================================= */
/* the session                                                             */
/* ======================================================================= */

#define AR_ROUND1                                                              \
    AR_MSG(AR_TEXT("I can build you one. Let me check the document format "     \
                   "this machine expects.")                                    \
           ","                                                                 \
           AR_USE("toolu_ar_1", "app", "{\"action\":\"format\"}"),             \
           "tool_use")

#define AR_ROUND2                                                              \
    AR_MSG(AR_TEXT("A display and a keypad, with one handler per key group.")   \
           ","                                                                 \
           AR_USE("toolu_ar_2", "app",                                         \
                  "{\"action\":\"launch\",\"x\":60,\"y\":120,\"document\":"     \
                  CALC_DOC_BAD "}"),                                           \
           "tool_use")

#define AR_ROUND3                                                              \
    AR_MSG(AR_TEXT("I tagged the keys \\\"digit\\\" and then selected "         \
                   "\\\"digits\\\". Fixing that one word.")                    \
           ","                                                                 \
           AR_USE("toolu_ar_3", "app",                                         \
                  "{\"action\":\"launch\",\"x\":60,\"y\":120,\"document\":"     \
                  CALC_DOC_GOOD "}"),                                          \
           "tool_use")

/* Four presses in one turn: the loop allows up to CHAT_MAX_TOOL_CALLS blocks per
 * assistant message, and this is what a model doing arithmetic on its own
 * calculator actually looks like. */
#define AR_ROUND4                                                              \
    AR_MSG(AR_TEXT("It is on screen. Let me prove the keys work: 7 + 8.")       \
           ","                                                                 \
           AR_USE("toolu_ar_4a", "gui_click", "{\"id\":1,\"label\":\"7\"}")     \
           ","                                                                 \
           AR_USE("toolu_ar_4b", "gui_click", "{\"id\":1,\"label\":\"+\"}")     \
           ","                                                                 \
           AR_USE("toolu_ar_4c", "gui_click", "{\"id\":1,\"label\":\"8\"}")     \
           ","                                                                 \
           AR_USE("toolu_ar_4d", "gui_click", "{\"id\":1,\"label\":\"=\"}"),    \
           "tool_use")

#define AR_ROUND5                                                              \
    AR_MSG(AR_USE("toolu_ar_5", "app", "{\"action\":\"state\",\"id\":1}"),      \
           "tool_use")

#define AR_ROUND6                                                              \
    AR_MSG(AR_TEXT("Your calculator is open and working: I pressed 7, +, 8, = "  \
                   "and its display reads 15. Click it with the mouse, or tell " \
                   "me what to work out."),                                     \
           "end_turn")

#endif /* APP_TRANSCRIPT_CALCULATOR_H */
