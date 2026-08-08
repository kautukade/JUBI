/* model_mock.c — a scripted model_transport_t (see model.h).
 *
 * Queue the responses the model "would" send, run the code under test, then
 * inspect the request bodies it produced. Nothing here touches the network, the
 * heap, or the clock, so a test of the tool-use loop is deterministic and runs
 * in microseconds:
 *
 *     model_mock_reset();
 *     model_mock_queue(200, tool_use_json);      // turn 1: model calls a tool
 *     model_mock_queue(200, final_text_json);    // turn 2: model answers
 *     run_the_loop(model_mock_transport());
 *     CHECK_CONTAINS(model_mock_request(1), "tool_result");
 *
 * Storage is static and fixed-size on purpose: no allocation means the mock can
 * also be linked into the kernel later (an offline demo mode) without depending
 * on the heap. It is not linked into the kernel today.
 */

#include "model.h"

typedef struct {
    int         err;        /* MODEL_OK, or the failure to simulate */
    int         status;
    const char *body;       /* not copied: caller keeps it alive */
    const char *reason;     /* for error entries */
} mock_entry_t;

static mock_entry_t q[MODEL_MOCK_MAX_QUEUE];
static size_t       q_len;      /* queued */
static size_t       q_pos;      /* consumed */

static char   reqs[MODEL_MOCK_MAX_QUEUE][MODEL_MOCK_REQ_CAP];
static int    reqs_trunc[MODEL_MOCK_MAX_QUEUE];
static size_t req_count;

static const char *last_err = "";

/* ---- local helpers (no libc) ---- */

static size_t zlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void put_status_line(char *dst, size_t cap, int status) {
    static const char prefix[] = "HTTP/1.1 ";
    size_t off = 0;
    for (size_t i = 0; i < sizeof prefix - 1 && off + 1 < cap; i++)
        dst[off++] = prefix[i];

    unsigned v = (unsigned)(status < 0 ? 0 : status);
    char     t[10];
    int      n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n && off + 1 < cap) dst[off++] = t[--n];

    const char *reason = (status == 200) ? " OK"
                       : (status == 401) ? " Unauthorized"
                       : (status == 429) ? " Too Many Requests"
                       : (status == 500) ? " Internal Server Error"
                                         : " Mock";
    for (size_t i = 0; reason[i] && off + 1 < cap; i++) dst[off++] = reason[i];
    if (cap) dst[off] = '\0';
}

/* ---- transport implementation ---- */

static void record_request(const char *body, size_t len) {
    size_t slot = req_count;
    req_count++;
    if (slot >= MODEL_MOCK_MAX_QUEUE) return;

    size_t n = len;
    int    truncated = 0;
    if (n >= MODEL_MOCK_REQ_CAP) { n = MODEL_MOCK_REQ_CAP - 1; truncated = 1; }
    for (size_t i = 0; i < n; i++) reqs[slot][i] = body[i];
    reqs[slot][n] = '\0';
    reqs_trunc[slot] = truncated;
}

static int mock_send(model_transport_t *t,
                     const char *req_body, size_t req_len,
                     char *resp_buf, size_t resp_cap,
                     model_response_t *out) {
    (void)t;
    record_request(req_body, req_len);

    if (q_pos >= q_len) {
        last_err = "mock: response queue empty";
        return MODEL_ESCRIPT;
    }

    mock_entry_t *e = &q[q_pos++];
    if (e->err != MODEL_OK) {
        last_err = e->reason ? e->reason : model_strerror(e->err);
        return e->err;
    }

    const char *body = e->body ? e->body : "";
    size_t      n    = zlen(body);

    out->http_status = e->status;
    put_status_line(out->status_line, sizeof out->status_line, e->status);

    if (n >= resp_cap) { n = resp_cap - 1; out->truncated = 1; }
    for (size_t i = 0; i < n; i++) resp_buf[i] = body[i];
    resp_buf[n] = '\0';

    out->body     = resp_buf;
    out->body_len = n;

    last_err = "";
    return MODEL_OK;
}

static const char *mock_last_error(model_transport_t *t) {
    (void)t;
    return last_err;
}

static model_transport_t mock_transport = {
    "mock", mock_send, mock_last_error, (void *)0
};

model_transport_t *model_mock_transport(void) { return &mock_transport; }

/* ---- scripting controls ---- */

void model_mock_reset(void) {
    q_len = q_pos = 0;
    req_count = 0;
    last_err = "";
    for (size_t i = 0; i < MODEL_MOCK_MAX_QUEUE; i++) {
        reqs[i][0] = '\0';
        reqs_trunc[i] = 0;
    }
}

int model_mock_queue(int http_status, const char *body) {
    if (q_len >= MODEL_MOCK_MAX_QUEUE) return MODEL_ENOSPC;
    q[q_len].err    = MODEL_OK;
    q[q_len].status = http_status;
    q[q_len].body   = body;
    q[q_len].reason = (const char *)0;
    q_len++;
    return MODEL_OK;
}

int model_mock_queue_error(int err, const char *reason) {
    if (q_len >= MODEL_MOCK_MAX_QUEUE) return MODEL_ENOSPC;
    if (err == MODEL_OK) return MODEL_EINVAL;
    q[q_len].err    = err;
    q[q_len].status = 0;
    q[q_len].body   = (const char *)0;
    q[q_len].reason = reason;
    q_len++;
    return MODEL_OK;
}

size_t model_mock_request_count(void) { return req_count; }

const char *model_mock_request(size_t index) {
    if (index >= req_count || index >= MODEL_MOCK_MAX_QUEUE) return (const char *)0;
    return reqs[index];
}

const char *model_mock_last_request(void) {
    if (req_count == 0) return (const char *)0;
    return model_mock_request(req_count - 1);
}

int model_mock_request_truncated(size_t index) {
    if (index >= req_count || index >= MODEL_MOCK_MAX_QUEUE) return 0;
    return reqs_trunc[index];
}

size_t model_mock_pending(void) { return q_len - q_pos; }
