/* model.c — transport-independent half of the model protocol (see model.h).
 *
 * Dispatch plus request construction. No transport, no kernel services, no
 * libc — so this compiles into the kernel and into the host test binaries from
 * the same source, and the escaping the tests verify is literally the escaping
 * the kernel performs.
 */

#include "model.h"

/* Model id and the request shape are protocol details, kept next to the code
 * that emits them rather than in the transport. */
#define MODEL_DEFAULT_ID "claude-opus-4-8"

int model_send(model_transport_t *t,
               const char *req_body, size_t req_len,
               char *resp_buf, size_t resp_cap,
               model_response_t *out) {
    if (!out) return MODEL_EINVAL;

    out->http_status    = 0;
    out->status_line[0] = '\0';
    out->body           = resp_buf;
    out->body_len       = 0;
    out->truncated      = 0;

    if (!t || !t->send || !req_body || !resp_buf || resp_cap == 0)
        return MODEL_EINVAL;
    if (resp_cap) resp_buf[0] = '\0';

    return t->send(t, req_body, req_len, resp_buf, resp_cap, out);
}

const char *model_last_error(model_transport_t *t) {
    if (!t || !t->last_error) return "no transport";
    const char *e = t->last_error(t);
    return e ? e : "";
}

const char *model_strerror(int err) {
    switch (err) {
        case MODEL_OK:          return "ok";
        case MODEL_EINVAL:      return "invalid argument";
        case MODEL_ENOSPC:      return "buffer too small";
        case MODEL_ETRANSPORT:  return "transport failure";
        case MODEL_ETIMEOUT:    return "timed out";
        case MODEL_EPROTO:      return "malformed HTTP response";
        case MODEL_ESCRIPT:     return "mock: no response queued";
        default:                return "unknown error";
    }
}

/* An empty tools array is not the same as no tools: the API rejects
 * "tools":[]. Treat "[]" (and anything shorter) as "this machine has no tools
 * registered" and omit the key entirely. */
static int tools_present(const char *tools, size_t len) {
    if (!tools || len < 3) return 0;
    for (size_t i = 0; i < len; i++)
        if (tools[i] != '[' && tools[i] != ']' &&
            tools[i] != ' ' && tools[i] != '\n' && tools[i] != '\t')
            return 1;
    return 0;
}

int model_build(char *dst, size_t cap, const model_request_t *req) {
    if (!dst || cap == 0 || !req) return MODEL_EINVAL;
    if (req->message_count && !req->messages) return MODEL_EINVAL;

    const char *model = req->model ? req->model : MODEL_DEFAULT_ID;

    json_writer_t w;
    json_writer_init(&w, dst, cap);

    json_put(&w, "{\"model\":");
    json_put_string(&w, model);
    json_put(&w, ",\"max_tokens\":");
    json_put_uint(&w, req->max_tokens);

    if (req->system && req->system[0]) {
        json_put(&w, ",\"system\":");
        json_put_string(&w, req->system);
    }

    /* The tool schemas are JSON already (tools_build_schema wrote them with the
     * same writer) — insert verbatim, exactly like tool.c inserts a schema. */
    if (tools_present(req->tools, req->tools_len)) {
        json_put(&w, ",\"tools\":");
        json_put_n(&w, req->tools, req->tools_len);
    }

    json_put(&w, ",\"messages\":[");
    for (size_t i = 0; i < req->message_count; i++) {
        const model_msg_t *m = &req->messages[i];
        if (i) json_put_char(&w, ',');
        json_put(&w, "{\"role\":");
        json_put_string(&w, m->role ? m->role : "user");
        json_put(&w, ",\"content\":");
        if (m->raw) json_put_n(&w, m->content, m->content_len);
        else        json_put_string(&w, m->content ? m->content : "");
        json_put_char(&w, '}');
    }
    json_put(&w, "]}");

    if (!json_writer_ok(&w)) return MODEL_ENOSPC;

    /* Raw content blocks are inserted uninterpreted, which is the one way a
     * caller could put a malformed — or injected — body on the wire. Re-read
     * what we just wrote with the same parser that faces the network, and
     * refuse rather than send it. Escaped text can never fail this; a caller
     * bug with raw JSON always does. */
    json_value_t v;
    if (json_parse(dst, json_writer_len(&w), &v) != JSON_OK) return MODEL_EINVAL;

    return (int)json_writer_len(&w);
}

int model_build_request(char *dst, size_t cap, const char *model,
                        uint32_t max_tokens, const char *prompt) {
    if (!prompt) return MODEL_EINVAL;

    /* The one place user input enters, and it enters escaped. */
    model_msg_t     turn = { "user", prompt, 0, 0 };
    model_request_t req  = { model, max_tokens, (const char *)0,
                             (const char *)0, 0, &turn, 1 };
    return model_build(dst, cap, &req);
}
