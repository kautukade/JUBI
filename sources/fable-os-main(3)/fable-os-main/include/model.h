/* model.h — the transport seam between the kernel and the model.
 *
 * PURPOSE
 *   Separate *what the kernel says to the model* from *how the bytes get there*.
 *   One side builds and interprets JSON; the other side moves a request body to
 *   api.anthropic.com and brings a response body back. Everything above this
 *   line — today net_ask(), tomorrow the tool-use loop that lets the model call
 *   real kernel functions — is written against model_transport_t and cannot tell
 *   whether it is talking to TLS over an e1000 or to a canned script in a host
 *   unit test. That is the whole point: a loop that dispatches ring-0 calls must
 *   be testable without a network, an API key, or a QEMU boot.
 *
 * ARCHITECTURE
 *   model_transport_t is a vtable with exactly one interesting method, send():
 *   hand it a fully-formed request body, get back an HTTP status, a body, and a
 *   length in a caller-supplied buffer. No callbacks, no ownership transfer, no
 *   allocation — the caller owns every byte, which keeps the interface usable
 *   from an interrupt-free kernel context and trivial to fake.
 *
 *     net/net.c        -> model_tls_transport()   real mbedTLS/lwIP round-trip
 *     net/model_mock.c -> model_mock_transport()  scripted responses for tests
 *
 *   Request construction lives here too (model_build_request), because escaping
 *   a prompt into JSON is protocol logic, not transport logic, and it must be
 *   exercised by the same host tests that hammer the parser.
 *
 * RESPONSIBILITIES
 *   - Define the request/response contract (model_response_t).
 *   - Dispatch through a transport with argument validation and a uniform error
 *     space, so no caller has to special-case a transport.
 *   - Build Messages API request bodies with correct escaping.
 *   - NOT: parsing responses (that is json.h) and NOT owning buffers.
 *
 * PUBLIC API
 *   model_send            — run one request/response exchange over a transport.
 *   model_build           — build a Messages API body from a tools array and a
 *                           multi-turn history (what the tool-use loop needs).
 *   model_build_request   — the one-shot form: a single user turn, no tools.
 *   model_strerror        — human-readable form of a MODEL_* error.
 *   model_tls_transport   — the real transport (kernel only; needs lwIP).
 *   model_mock_transport  — the scripted transport, plus model_mock_* controls.
 *
 * DEPENDENCIES
 *   json.h and <stdint.h>/<stddef.h>. Deliberately not kernel.h, not lwIP, not
 *   mbedTLS: net/model.c and net/model_mock.c compile on the host as well as
 *   freestanding. Only net/net.c, which *implements* the TLS transport, pulls in
 *   the network stack.
 *
 * FUTURE EXTENSION POINTS
 *   - The tool-use loop calls model_send() in a loop, appending tool_result
 *     content blocks between turns; nothing in this header changes.
 *   - A streaming transport can be added by giving model_transport_t a second,
 *     optional method — existing callers keep using send().
 *   - A replay transport (log real traffic, replay it in tests) is the same
 *     shape as the mock.
 */
#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>
#include <stddef.h>

#include "json.h"

/* ---- error codes (0 = an exchange completed; the HTTP status may still be an
 * error like 401 — that is the caller's business, not the transport's) ---- */
#define MODEL_OK           0
#define MODEL_EINVAL      -22   /* bad arguments                              */
#define MODEL_ENOSPC      -28   /* request did not fit the buffer given       */
#define MODEL_ETRANSPORT  -70   /* connect / TLS / socket failure             */
#define MODEL_ETIMEOUT    -71   /* no complete response before the deadline   */
#define MODEL_EPROTO      -72   /* response was not intelligible HTTP         */
#define MODEL_ESCRIPT     -73   /* mock transport: nothing left queued        */

#define MODEL_STATUS_LINE_MAX 96

/* What one exchange produced. `body` points into the caller's buffer and is
 * NUL-terminated; `body_len` excludes that NUL. */
typedef struct model_response {
    int    http_status;                            /* 200, 401, ... 0 if none */
    char   status_line[MODEL_STATUS_LINE_MAX];     /* verbatim, e.g.
                                                    * "HTTP/1.1 401 Unauthorized" */
    char  *body;
    size_t body_len;
    int    truncated;      /* body was longer than the buffer provided */
} model_response_t;

/* A transport. `priv` is the implementation's own state; callers never touch it.
 *
 * send() contract:
 *   - `req_body` / `req_len` describe a complete request body (JSON). The
 *     transport adds whatever framing it needs (HTTP headers, TLS) itself.
 *   - `resp_buf` / `resp_cap` are the caller's response buffer. The transport
 *     may use the whole buffer as scratch, but on success leaves the response
 *     *body* at the front, NUL-terminated, with out->body == resp_buf.
 *   - Returns MODEL_OK when a response was received (even a 4xx/5xx), or a
 *     negative MODEL_* code. On error, *out is zeroed except status_line[0].
 *   - Must not block forever: a transport is responsible for its own deadline
 *     and reports MODEL_ETIMEOUT.
 * last_error() returns a short, static, human-readable reason for the most
 * recent failure — never NULL. */
typedef struct model_transport {
    const char *name;
    int (*send)(struct model_transport *t,
                const char *req_body, size_t req_len,
                char *resp_buf, size_t resp_cap,
                model_response_t *out);
    const char *(*last_error)(struct model_transport *t);
    void *priv;
} model_transport_t;

/* Run one exchange. Validates arguments, zeroes *out, then dispatches. This is
 * the only call the tool-use loop needs to know about. */
int model_send(model_transport_t *t,
               const char *req_body, size_t req_len,
               char *resp_buf, size_t resp_cap,
               model_response_t *out);

/* Short reason for the last failure on `t` (never NULL, safe on NULL input). */
const char *model_last_error(model_transport_t *t);

/* Static description of a MODEL_* code (never NULL). */
const char *model_strerror(int err);

/* ---- request construction --------------------------------------------
 * Two entry points over one implementation, so the escaping the host tests
 * hammer is literally the escaping every request gets.
 *
 *   model_build_request  one user turn, no tools — the boot self-test.
 *   model_build          a tools array plus a whole conversation — the loop.
 */

/* One message of a conversation.
 *
 * `raw` decides how `content` is treated, and the distinction is the whole
 * safety story of this struct:
 *
 *   raw == 0  `content` is TEXT. It is escaped with json_put_string(), so a
 *             quote, a newline or an outright `"},{"role":"assistant"` in
 *             whatever the operator typed lands inside the JSON string and
 *             cannot become structure. `content_len` is ignored (NUL-terminated).
 *
 *   raw == 1  `content` is a JSON value already — the assistant's content[]
 *             array echoed back verbatim, or a tool_result array built with
 *             json.h. It is inserted uninterpreted, so the CALLER owns its
 *             well-formedness. model_build() re-parses the finished body and
 *             refuses to return one that is not valid JSON, so a caller
 *             mistake here is a MODEL_EINVAL, never a corrupt request on the
 *             wire. Never pass untrusted text with raw == 1. */
typedef struct model_msg {
    const char *role;          /* "user" | "assistant"                       */
    const char *content;
    size_t      content_len;   /* used only when raw                         */
    int         raw;
} model_msg_t;

/* Everything one Messages API request carries. */
typedef struct model_request {
    const char        *model;          /* NULL -> the compiled-in default     */
    uint32_t           max_tokens;
    const char        *system;         /* NULL/"" -> omitted; escaped         */
    const char        *tools;          /* raw JSON array from tools_build_schema();
                                        * NULL or "[]" -> the key is omitted  */
    size_t             tools_len;
    const model_msg_t *messages;
    size_t             message_count;
} model_request_t;

/* Build a Messages API request body into `dst`. Returns the body length, or
 * MODEL_ENOSPC (dst too small — dst is left a valid NUL-terminated prefix) or
 * MODEL_EINVAL (bad arguments, or raw content that produced invalid JSON). */
int model_build(char *dst, size_t cap, const model_request_t *req);

/* Build a single-user-turn Messages API request body into `dst`.
 * `prompt` is escaped, so quotes, newlines, backslashes and control characters
 * in whatever the user typed can never break out of the JSON string.
 * Returns the body length on success, or MODEL_ENOSPC / MODEL_EINVAL. */
int model_build_request(char *dst, size_t cap, const char *model,
                        uint32_t max_tokens, const char *prompt);

/* ---- transports ---- */

/* The real thing: TLS to api.anthropic.com. Implemented in net/net.c, so it is
 * only linked into the kernel. Returns NULL before net_init() has succeeded. */
model_transport_t *model_tls_transport(void);

/* ---- mock transport (net/model_mock.c) ----
 * A scriptable stand-in: queue the responses the model "would" give, run the
 * code under test, then inspect the request bodies it produced. This is what
 * makes the tool-use loop testable — queue a tool_use response, assert the
 * kernel dispatched the call, queue the follow-up.
 *
 *     model_mock_reset();
 *     model_mock_queue(200, "{\"content\":[{\"type\":\"tool_use\", ...}]}");
 *     model_mock_queue(200, "{\"content\":[{\"type\":\"text\",...}]}");
 *     ... run the loop against model_mock_transport() ...
 *     CHECK_CONTAINS(model_mock_request(1), "tool_result");
 */
#define MODEL_MOCK_MAX_QUEUE   16
/* Recorded requests must be inspectable *whole*: a tool-use loop's follow-up
 * body carries the system prompt, every tool schema and the entire history, so
 * a 4 KiB record would clip away the tool_result blocks a test needs to see. */
#define MODEL_MOCK_REQ_CAP   32768

model_transport_t *model_mock_transport(void);

/* Drop every queued response and every recorded request. */
void model_mock_reset(void);

/* Queue a canned response. `body` is NOT copied — pass a literal or a buffer
 * that outlives the test. Returns 0, or MODEL_ENOSPC if the queue is full. */
int model_mock_queue(int http_status, const char *body);

/* Queue a transport failure (e.g. MODEL_ETIMEOUT) with an optional reason. */
int model_mock_queue_error(int err, const char *reason);

/* How many requests were sent, and their bodies (NULL if out of range; bodies
 * beyond MODEL_MOCK_MAX_QUEUE are counted but not retained). Request bodies are
 * truncated to MODEL_MOCK_REQ_CAP-1 bytes; model_mock_request_truncated() says
 * whether that happened. */
size_t      model_mock_request_count(void);
const char *model_mock_request(size_t index);
const char *model_mock_last_request(void);
int         model_mock_request_truncated(size_t index);

/* Responses queued but not yet consumed — assert 0 to prove a test used them. */
size_t model_mock_pending(void);

#endif /* MODEL_H */
