/* json.h — a small, pure JSON reader/writer for the model protocol.
 *
 * PURPOSE
 *   Turn bytes that arrived from the network into values the kernel can act on,
 *   and turn kernel data into a well-formed request body — without trusting the
 *   input for one instant. Everything the kernel learns from Anthropic (the
 *   assistant's text, a block's "type", "stop_reason", and soon a tool_use
 *   block's "id"/"name"/"input") is read through this module.
 *
 * ARCHITECTURE
 *   Two halves that never share state:
 *     - Reader: a zero-copy cursor. json_parse() validates a whole document once
 *       and hands back a json_value_t, which is nothing but {type, start, len} —
 *       a bounded span into the caller's buffer. Navigation (json_get, json_at,
 *       json_member) re-walks that span with the same bounds-checked scanner, so
 *       there is no tree, no allocation, and no pointer that can outlive its
 *       buffer by accident.
 *     - Writer: json_writer_t, a fixed-capacity, overflow-flagging appender.
 *       json_put_string() is the only sanctioned way to interpolate a string
 *       into a request; it escapes so a quote or a newline in a prompt cannot
 *       corrupt (or inject into) the JSON.
 *
 * RESPONSIBILITIES
 *   - Validate: reject malformed, truncated, and over-nested documents.
 *   - Navigate: objects by key, arrays by index, objects by member index.
 *   - Decode: unescape JSON strings (including \uXXXX and surrogate pairs) into
 *     UTF-8, into a caller-supplied buffer, never past its end.
 *   - Encode: escape strings safely when building a request.
 *
 * PUBLIC API
 *   json_parse / json_parse_str          — validate and locate the root value.
 *   json_get / json_at / json_member     — navigate objects and arrays.
 *   json_count                           — element / member count.
 *   json_str / json_get_str / json_str_eq— decode strings, bounds-checked.
 *   json_int / json_bool                 — decode scalars.
 *   json_escape / json_writer_*          — build request bodies safely.
 *   json_msg_*                           — thin sugar for the Anthropic
 *                                          Messages response shape.
 *
 * DEPENDENCIES
 *   None. Not kernel.h, not string.h, not the heap, not the port/ shims — only
 *   <stdint.h>/<stddef.h>. That is deliberate: the same object file compiles
 *   freestanding into the kernel and natively for tests/host/test_json.c, so the
 *   parser that faces the network is the parser the tests hammer.
 *
 * SAFETY MODEL
 *   Every function takes a length or a capacity and honours it. Recursion is
 *   bounded by JSON_MAX_DEPTH (a hostile "[[[[[..." cannot blow the 64 KiB
 *   kernel stack). Input need not be NUL-terminated. Output is always
 *   NUL-terminated when capacity allows, and truncation is reported, never
 *   silent.
 *
 * FUTURE EXTENSION POINTS
 *   The reader already walks content[] generically, which is what the tool-use
 *   loop needs: a block's "input" object is available as a raw span
 *   ({start,len}) that can be echoed back verbatim in a tool_result, or walked
 *   member by member to bind kernel call arguments. Streaming (SSE) would slot
 *   in as a separate framing layer feeding whole JSON documents to json_parse().
 */
#ifndef JSON_H
#define JSON_H

#include <stdint.h>
#include <stddef.h>

/* ---- error codes (errno-style negatives; 0 = success) ---- */
#define JSON_OK        0
#define JSON_ENOENT   -2    /* key or index not present            */
#define JSON_ETYPE   -21    /* value is not of the requested type  */
#define JSON_EINVAL  -22    /* malformed or truncated JSON         */
#define JSON_ENOSPC  -28    /* destination buffer too small        */
#define JSON_EDEPTH  -40    /* nesting deeper than JSON_MAX_DEPTH  */

/* Nesting limit. Recursion is the only stack cost in the parser and each frame
 * is tiny, so 32 is far below the kernel's 64 KiB stack while being far above
 * anything the Messages API produces. */
#define JSON_MAX_DEPTH 32

typedef enum {
    JSON_INVALID = 0,
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

/* A located value: a bounded span of the caller's buffer, not a copy. For a
 * string, the span includes the surrounding quotes and the escapes as written;
 * use json_str() to decode it. The span is only valid while the buffer is. */
typedef struct json_value {
    json_type_t type;
    const char *start;
    size_t      len;
} json_value_t;

/* ---- parsing ---- */

/* Validate `text[0..len)` as one JSON document (trailing whitespace allowed) and
 * locate the root value. The buffer need not be NUL-terminated. */
int json_parse(const char *text, size_t len, json_value_t *out);

/* Same, for a NUL-terminated buffer. */
int json_parse_str(const char *text, json_value_t *out);

/* ---- navigation ---- */

/* Members of an object / elements of an array; 0 for any other type. */
size_t json_count(const json_value_t *v);

/* Look up `key` in an object. JSON_ENOENT if absent, JSON_ETYPE if not object. */
int json_get(const json_value_t *obj, const char *key, json_value_t *out);

/* Element `index` of an array. */
int json_at(const json_value_t *arr, size_t index, json_value_t *out);

/* Member `index` of an object, by position. Either out pointer may be NULL.
 * Lets a caller walk an arbitrary object (e.g. a tool_use "input"). */
int json_member(const json_value_t *obj, size_t index,
                json_value_t *key_out, json_value_t *val_out);

/* ---- decoding ---- */

/* Decode a JSON string into `dst` as UTF-8, always NUL-terminating when cap > 0.
 * `*out_len` (optional) receives the byte length written, excluding the NUL.
 * Returns JSON_ENOSPC if the value did not fit — `dst` then holds a valid,
 * NUL-terminated prefix. Invalid or unpaired surrogates decode to U+FFFD. */
int json_str(const json_value_t *v, char *dst, size_t cap, size_t *out_len);

/* json_get() + json_str() in one step. */
int json_get_str(const json_value_t *obj, const char *key, char *dst, size_t cap);

/* 1 if `v` is a string whose decoded value equals the NUL-terminated `s`. Does
 * not allocate and does not need a buffer, so it is the cheap way to test a
 * block's "type". */
int json_str_eq(const json_value_t *v, const char *s);

/* Integers. A fractional part is truncated toward zero; exponents are rejected
 * with JSON_EINVAL (the Messages API only sends integers). Overflow is
 * JSON_EINVAL rather than a wrapped value. */
int json_int(const json_value_t *v, int64_t *out);

/* true -> 1, false -> 0. JSON_ETYPE for anything else. */
int json_bool(const json_value_t *v, int *out);

/* ---- encoding ---- */

/* Escape `src` as a JSON string *body* (no surrounding quotes) into `dst`.
 * Returns the length the escaped form needs, excluding the NUL — snprintf-style,
 * so a return >= cap means it was truncated. `dst` is always NUL-terminated when
 * cap > 0. Bytes >= 0x80 pass through (the input is assumed to be UTF-8). */
size_t json_escape(char *dst, size_t cap, const char *src);
size_t json_escape_n(char *dst, size_t cap, const char *src, size_t n);

/* A fixed-capacity appender. Once it overflows it stops writing and stays
 * overflowed, so exactly one check at the end is enough. `buf` is kept
 * NUL-terminated throughout. */
typedef struct json_writer {
    char  *buf;
    size_t cap;
    size_t len;
    int    overflow;
} json_writer_t;

void   json_writer_init(json_writer_t *w, char *buf, size_t cap);
void   json_put(json_writer_t *w, const char *s);            /* raw, no escaping */
void   json_put_n(json_writer_t *w, const char *s, size_t n);
void   json_put_char(json_writer_t *w, char c);
void   json_put_uint(json_writer_t *w, uint64_t v);
void   json_put_int(json_writer_t *w, int64_t v);
void   json_put_string(json_writer_t *w, const char *s);     /* "quoted+escaped" */
int    json_writer_ok(const json_writer_t *w);               /* 0 once overflowed */
size_t json_writer_len(const json_writer_t *w);

/* ---- Anthropic Messages response sugar ----
 * Thin wrappers over the generic API above; nothing here knows about transport.
 * A caller that needs tool_use blocks walks content[] itself:
 *     json_msg_content(&root, &content);
 *     for (i = 0; i < json_count(&content); i++) {
 *         json_at(&content, i, &blk);
 *         json_get(&blk, "type", &t);
 *         if (json_str_eq(&t, "tool_use")) { ... json_get(&blk, "input", &in); }
 *     }
 */

/* Locate the content[] array of a Messages response. */
int json_msg_content(const json_value_t *root, json_value_t *out);

/* Concatenate the decoded text of every content[] block of type "text".
 * JSON_ENOENT if the response has no text blocks at all (e.g. an API error
 * body, or a turn that is nothing but tool_use). */
int json_msg_text(const json_value_t *root, char *dst, size_t cap, size_t *out_len);

/* "stop_reason" as a string; a JSON null yields "" and JSON_OK. */
int json_msg_stop_reason(const json_value_t *root, char *dst, size_t cap);

#endif /* JSON_H */
