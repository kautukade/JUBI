/* sse.h — Server-Sent Events: the model's reply, one token at a time.
 *
 * PURPOSE
 *   The console is a fixed grid of text cells with no scrollback (128x48 on a
 *   framebuffer, 80x25 on the VGA fallback). When the transport buffers a whole
 *   HTTP response and then prints it, a paragraph appears in one frame; when it
 *   prints the model's tokens as they land on the wire, the machine looks like
 *   it is thinking out loud. This module is what turns the second thing into
 *   code: a resumable state machine that eats arbitrary TCP-sized chunks of a
 *   `text/event-stream` body and emits text deltas the instant they arrive.
 *
 *   It is also the correct architecture. Buffering 64 KiB of reply before the
 *   first character can be shown is a latency choice nobody made on purpose.
 *
 * THE HARD PART — resumability
 *   Events do NOT arrive whole. A single `data: {...}` line is routinely split
 *   across TCP segments at an arbitrary byte boundary: mid-JSON, mid-field,
 *   mid-UTF-8 sequence, between the CR and the LF of one line ending. So there
 *   is exactly one rule here: every layer must survive being fed one byte at a
 *   time and produce byte-identical results to being fed the whole stream at
 *   once. tests/host/test_sse.c asserts precisely that, for every case it has.
 *
 * THREE LAYERS, each testable on its own
 *
 *   1. sse_parser_t — framing. Bytes -> events. Implements the WHATWG
 *      event-stream line grammar: CRLF/LF/CR line endings, an optional leading
 *      UTF-8 BOM, `field: value` with one optional leading space, `:` comment
 *      (keepalive) lines, unknown fields ignored, a blank line dispatching the
 *      accumulated event, multiple `data:` lines joined with '\n'. Knows
 *      nothing about JSON or about Anthropic.
 *
 *   2. sse_msg_t — reassembly. Events -> one Messages-API response body, plus a
 *      live text callback. The kernel above the transport (net/chat.c) needs
 *      *whole* content blocks: it echoes the assistant turn back verbatim in
 *      the next request and it dispatches tool_use blocks whose "input" must be
 *      complete JSON. So this layer replays the stream back into exactly the
 *      document a non-streaming request would have returned:
 *
 *          message_start        -> {"id":..,"type":"message","role":..,
 *                                   "model":..,"content":[
 *          content_block_start  -> open a block
 *          content_block_delta  -> text_delta:       append text (and echo it)
 *                                  input_json_delta: append raw JSON fragment
 *          content_block_stop   -> close the block
 *          message_delta        -> remember stop_reason
 *          message_stop         -> ],"stop_reason":..,"stop_sequence":null}
 *
 *      Text is copied as the *raw JSON string span* it arrived in, so nothing
 *      is ever re-escaped and no byte is reinterpreted; the live echo is a
 *      separate json_str() decode. Everything above the transport is therefore
 *      unchanged by streaming — streaming is a property of the wire, not of the
 *      protocol.
 *
 *   3. sse_http_t — the HTTP response driver. Raw response bytes (status line,
 *      headers, body) -> a filled model_response_t. It sniffs the response:
 *      only `200` + `content-type: text/event-stream` runs layers 1 and 2; a
 *      401, an HTML error page, or a plain JSON body is buffered verbatim
 *      exactly as the non-streaming transport does. The sniff matches the field
 *      name only at the START of a header line and requires a three-digit
 *      status, so neither a header *value* that happens to contain
 *      "content-type:" nor a status line with a wrapping digit run can decide
 *      which path a response takes.
 *
 * SAFETY MODEL — this parses untrusted network data
 *   - No allocation, no recursion, no globals. Every buffer is a fixed-size
 *     member of a caller-owned struct with a compile-time bound.
 *   - Every input is a (pointer, length) pair; nothing is assumed
 *     NUL-terminated and embedded NUL bytes are just bytes.
 *   - Overlong lines, overlong events and overlong tool inputs are clamped and
 *     the affected event is DROPPED and counted, never truncated into
 *     something that parses differently than it read. Dropping is not silent:
 *     every such counter reaches the caller as `truncated`, because a drop
 *     between two input_json_delta fragments splices the surviving prefix and
 *     suffix into a tool input that is still well-formed and no longer says
 *     what the model said. See sse_msg_lost().
 *   - Malformed JSON in an event is counted and ignored; the stream carries on.
 *   - sse_msg_finish() reserves tail room up front, so the document it produces
 *     is valid JSON even when the reply overran the response buffer — the
 *     caller learns that from `truncated`, not from a parse failure.
 *   - json.c does every byte of parsing. There is no second JSON reader here.
 *
 * SIZE
 *   sse_http_t is roughly 20 KiB of fixed buffers. It must live in .bss or a
 *   caller's static, never on the 64 KiB kernel stack.
 *
 * BUILD
 *   Compiled into the kernel always; only *used* under -DFABLEOS_STREAM (see the
 *   FABLEOS_STREAM blocks in net/net.c). The default build sends no
 *   "stream":true and takes the buffered path, byte for byte as before.
 *
 * SCOPE, AND THE ONE THING STILL OWED
 *   Tool calls are NOT a special case here: input_json_delta fragments are
 *   accumulated and validated, so net/chat.c receives whole tool_use blocks and
 *   the agentic loop works unchanged under the flag. There is no "fall back to
 *   buffering when tools are in play" path, because there did not need to be.
 *
 *   What IS still owed is one line in net/chat.c. Under the flag the transport
 *   prints the model's prose as it streams; chat.c's print_text_block() then
 *   prints the same prose again from the reassembled body, so a chat reply
 *   appears twice. net_ask() (the boot self-test, which lives in net.c) already
 *   suppresses its own second print via sse_http_streamed(). chat.c needs the
 *   same guard and is outside this change's ownership, so it is reported rather
 *   than made. It is currently unobservable: with no API key supplied over fw_cfg the
 *   API answers 401, which is not an event stream, so the streaming path never
 *   produces text for chat.c to duplicate.
 *
 * WHAT IS PROVEN, AND WHAT IS NOT
 *   Everything in tests/host/test_sse.c is synthetic. The framing, the
 *   reassembly, the HTTP sniff and the byte-at-a-time equivalence are proven
 *   exhaustively there. LIVE streaming against api.anthropic.com is UNPROVEN
 *   and has never been run: the streaming path is only reachable under
 *   -DFABLEOS_STREAM, no build target sets it, and the keyless boot every test
 *   uses gets a 401, which is not an event stream. The 401 does exercise the
 *   content-type sniff and the verbatim buffering fallback end to end, which is
 *   the half of the path a keyless build can reach.
 *
 * PUBLIC API
 *   sse_parser_init / sse_parser_feed / sse_parser_finish   framing
 *   sse_parser_events / _dropped / _comments / _bad_lines   framing counters
 *   sse_msg_init / sse_msg_event / sse_msg_finish           reassembly
 *   sse_msg_text_bytes / _blocks / _stop_reason / _error    reassembly results
 *   sse_http_init / sse_http_feed / sse_http_finish         HTTP driver
 *   sse_http_streamed / sse_http_error                      HTTP driver results
 *   sse_strerror                                            SSE_* -> text
 *
 * DEPENDENCIES
 *   json.h and model.h only (both of which are themselves free of kernel.h,
 *   libc and lwIP), so net/sse.c compiles unchanged into the kernel and into
 *   tests/host/test_sse.c. Printing is a caller-supplied callback, which is why
 *   this file does not know that a console exists.
 *
 * FUTURE EXTENSION POINTS
 *   - `thinking` blocks: today any block type other than text/tool_use is
 *     echoed from its content_block_start and its deltas are ignored (see
 *     SSE_BLK_OTHER). Streaming thinking text is another text_delta case.
 *   - Reconnection: `id:` and `retry:` are parsed and retained but unused; a
 *     Last-Event-ID resume would use them.
 *   - Layer 1 is protocol-agnostic. Any other SSE endpoint reuses it whole.
 */
#ifndef SSE_H
#define SSE_H

#include <stdint.h>
#include <stddef.h>

#include "json.h"
#include "model.h"

/* ---- result codes (errno-style negatives; 0 = success) ---- */
#define SSE_OK          0
#define SSE_EINVAL    -22   /* bad arguments                                  */
#define SSE_ENOSPC    -28   /* the reassembled body did not fit               */
#define SSE_EPROTO    -72   /* the response was not an intelligible stream    */
#define SSE_ESTREAM   -73   /* the stream itself carried an `error` event     */

/* ---- fixed bounds (all overridable at build time) ----
 * Sized for the Anthropic Messages stream, where the largest event is
 * message_start at a couple of hundred bytes. The limits exist to bound the
 * struct, not because a real event ever approaches them. */
#ifndef SSE_LINE_MAX
#define SSE_LINE_MAX        4096   /* one `field: value` line                 */
#endif
#ifndef SSE_DATA_MAX
#define SSE_DATA_MAX        4096   /* all data lines of one event, joined     */
#endif
#ifndef SSE_NAME_MAX
#define SSE_NAME_MAX          64   /* the `event:` name                       */
#endif
#ifndef SSE_ID_MAX
#define SSE_ID_MAX            64   /* the `id:` value                         */
#endif
#ifndef SSE_TOOL_INPUT_MAX
#define SSE_TOOL_INPUT_MAX  4096   /* one tool_use "input" object             */
#endif
#ifndef SSE_HDR_MAX
#define SSE_HDR_MAX         2048   /* status line + response headers          */
#endif
#define SSE_ERR_MAX          160   /* a stream error message, clamped         */
#define SSE_STOP_MAX          32   /* "stop_reason", clamped                  */

/* Bytes held back in the response buffer so the closing
 * `"}],"stop_reason":"...","stop_sequence":null}` always fits, even when the
 * reply overran. This is why an over-long stream still yields valid JSON. */
#define SSE_TAIL_RESERVE     128

/* ====================================================================== */
/* layer 1 — framing                                                       */
/* ====================================================================== */

/* One dispatched event. `type` and `data` are NUL-terminated for convenience
 * AND carry an explicit length, because `data` may contain NUL bytes that a
 * hostile server put there. Both pointers are only valid for the duration of
 * the callback — they point into the parser's own buffers. */
typedef struct sse_event {
    const char *type;        /* event name; "message" when none was given     */
    const char *data;        /* data lines joined with '\n', trailing '\n' cut*/
    size_t      data_len;
    const char *id;          /* the most recent `id:` value, "" if none       */
} sse_event_t;

typedef void (*sse_event_fn)(void *ctx, const sse_event_t *ev);

typedef struct sse_parser {
    /* line assembly */
    char     line[SSE_LINE_MAX];
    size_t   line_len;
    uint8_t  line_over;      /* this line did not fit                         */
    uint8_t  after_cr;       /* last byte was CR; swallow a following LF      */
    uint8_t  bom;            /* 0..2 = BOM bytes matched, 3 = past the BOM    */

    /* event assembly */
    char     name[SSE_NAME_MAX];
    char     id[SSE_ID_MAX];
    char     data[SSE_DATA_MAX];
    size_t   data_len;
    uint8_t  ev_bad;         /* something overflowed: drop at dispatch        */

    /* counters — cheap ground truth for tests and for a boot banner */
    uint32_t n_events;
    uint32_t n_dropped;
    uint32_t n_comments;
    uint32_t n_bad_lines;
    uint32_t n_bytes;

    sse_event_fn on_event;
    void        *ctx;
} sse_parser_t;

/* `fn` may be NULL (the parser then only counts). */
void sse_parser_init(sse_parser_t *p, sse_event_fn fn, void *ctx);

/* Feed any number of bytes, at any boundary. Safe with n == 0 or bytes NULL. */
void sse_parser_feed(sse_parser_t *p, const char *bytes, size_t n);

/* End of stream. Leniently completes an unterminated final line and dispatches
 * a pending event if one has data — servers do close without the final blank
 * line, and losing the last delta to pedantry helps nobody. */
void sse_parser_finish(sse_parser_t *p);

uint32_t sse_parser_events(const sse_parser_t *p);     /* dispatched          */
uint32_t sse_parser_dropped(const sse_parser_t *p);    /* over-limit, dropped */
uint32_t sse_parser_comments(const sse_parser_t *p);   /* `:` keepalives      */
uint32_t sse_parser_bad_lines(const sse_parser_t *p);  /* lines that overran  */
uint32_t sse_parser_bytes(const sse_parser_t *p);      /* body bytes consumed */

/* ====================================================================== */
/* layer 2 — Anthropic message reassembly                                  */
/* ====================================================================== */

/* Called for every text_delta, with the DECODED UTF-8 text. Not
 * NUL-terminated-safe to assume beyond `len`; may be called many times per
 * event and zero times for a turn that is nothing but tool calls. */
typedef void (*sse_text_fn)(void *ctx, const char *text, size_t len);

enum { SSE_BLK_NONE = 0, SSE_BLK_TEXT, SSE_BLK_TOOL, SSE_BLK_OTHER };

typedef struct sse_msg {
    json_writer_t w;             /* writes the reassembled body               */
    sse_text_fn   on_text;
    void         *ctx;

    uint8_t  started;            /* the top-level object is open              */
    uint8_t  block;              /* SSE_BLK_* currently open                  */
    uint8_t  done;               /* message_stop (or [DONE]) seen             */
    uint8_t  closed;             /* sse_msg_finish() has run                  */
    uint8_t  clipped;            /* ran out of response buffer                */
    uint8_t  frozen;             /* clipped: structure output has stopped     */
    uint8_t  tool_over;          /* current tool input overran its buffer     */
    uint8_t  tool_lost;          /* STICKY: some tool input was never complete*/

    uint32_t blocks;             /* content blocks emitted                    */
    uint32_t text_bytes;         /* decoded text bytes echoed                 */
    uint32_t deltas;             /* deltas applied                            */
    uint32_t bad_events;         /* events whose data was not valid JSON      */
    uint32_t unknown_events;     /* well-formed events of an unknown type     */
    uint32_t stray_deltas;       /* deltas with no block open                 */

    char     stop[SSE_STOP_MAX];
    int      err;                /* SSE_* sticky error                        */
    char     err_msg[SSE_ERR_MAX];

    size_t   tool_len;
    char     tool[SSE_TOOL_INPUT_MAX];
    char     scratch[SSE_DATA_MAX + 1];
} sse_msg_t;

/* `out`/`cap` receive the reassembled Messages response body. cap must be at
 * least SSE_TAIL_RESERVE + 64 or every append is refused (the result is still
 * a valid, empty message). `on_text` may be NULL. */
void sse_msg_init(sse_msg_t *m, char *out, size_t cap, sse_text_fn fn, void *ctx);

/* Apply one framed event. Never fails: anything unintelligible is counted. */
void sse_msg_event(sse_msg_t *m, const sse_event_t *ev);

/* Close the document. Writes the closers for whatever is still open, so the
 * result is valid JSON even for a stream that was cut mid-block. Returns
 * SSE_OK, SSE_ESTREAM (the stream reported an error), SSE_ENOSPC (nothing at
 * all fitted) or SSE_EPROTO. `*out_len` receives the body length. */
int sse_msg_finish(sse_msg_t *m, size_t *out_len);

int         sse_msg_complete(const sse_msg_t *m);     /* message_stop seen    */
int         sse_msg_clipped(const sse_msg_t *m);      /* body was cut short   */

/* Nonzero when something the model sent is not in the reassembled document:
 * a malformed event, a delta with no block open, or a tool input that overran
 * its buffer. Sticky — close_block() clears the per-block flag, this does not.
 * Distinct from _clipped (we ran out of room) and from _complete (the stream
 * ended early): this is loss in the MIDDLE, which is the dangerous one, because
 * the surviving prefix and suffix of a tool input can splice into well-formed
 * JSON that says something the model never said. */
int         sse_msg_lost(const sse_msg_t *m);
uint32_t    sse_msg_blocks(const sse_msg_t *m);
uint32_t    sse_msg_text_bytes(const sse_msg_t *m);
/* The RAW JSON string body of stop_reason — escapes exactly as they arrived,
 * no surrounding quotes — because that is what goes back out into the document.
 * For every value the API actually sends ("end_turn", "tool_use", ...) raw and
 * decoded are the same bytes. A value too long for SSE_STOP_MAX is dropped
 * whole and this returns "": half an escape sequence is not a shorter reason. */
const char *sse_msg_stop_reason(const sse_msg_t *m);  /* "" if none           */
const char *sse_msg_error(const sse_msg_t *m);        /* "" if none           */

/* ====================================================================== */
/* layer 3 — the HTTP response driver                                      */
/* ====================================================================== */

typedef struct sse_http {
    char     hdr[SSE_HDR_MAX];
    size_t   hdr_len;
    uint8_t  hdr_over;           /* headers longer than SSE_HDR_MAX           */
    uint8_t  eoh;               /* CRLFCRLF scan state                        */
    uint8_t  in_body;
    uint8_t  streaming;          /* body is being reassembled, not buffered   */

    int      status;
    char     status_line[MODEL_STATUS_LINE_MAX];

    char    *buf;                /* the caller's response buffer              */
    size_t   cap;
    size_t   len;                /* raw bytes buffered (non-stream path)      */
    uint8_t  overflow;

    sse_text_fn on_text;
    void       *ctx;

    sse_parser_t p;
    sse_msg_t    m;
} sse_http_t;

/* `buf`/`cap` is the caller's response buffer; it holds either the verbatim
 * body (non-SSE responses) or the reassembled one (SSE responses). */
void sse_http_init(sse_http_t *h, char *buf, size_t cap, sse_text_fn fn, void *ctx);

/* Feed raw response bytes — status line, headers and body, in any chunking. */
void sse_http_feed(sse_http_t *h, const char *bytes, size_t n);

/* End of connection: fill *out. Returns SSE_OK or a negative SSE_*. */
int sse_http_finish(sse_http_t *h, model_response_t *out);

int         sse_http_streamed(const sse_http_t *h);  /* the SSE path ran      */
const char *sse_http_error(const sse_http_t *h);     /* "" if none            */

/* Static description of an SSE_* code (never NULL). */
const char *sse_strerror(int err);

#endif /* SSE_H */
