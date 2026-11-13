/* HTTP/1.1 request parsing. Pure: no I/O, no allocation, pointers into the caller's buffer.
 *
 * hp_parse looks at everything received so far and either finds one complete request (head,
 * plus body if Content-Length says there is one), says more bytes are needed, or rejects the
 * request with the status to answer. It keeps no state between calls, so feeding a request
 * one byte at a time gives exactly the same result as feeding it all at once (the tests check
 * this); the cost is re-reading the head while it arrives, bounded by HP_MAX_HEAD.
 */
#ifndef HP_PARSER_H
#define HP_PARSER_H

#include <stddef.h>

#define HP_MAX_HEADERS 64
#define HP_MAX_HEAD 16384        /* request line plus headers */
#define HP_MAX_TARGET 8192
#define HP_MAX_BODY (1u << 20)   /* bodies are read and discarded; static files take none */

typedef enum { HP_ERROR = -1, HP_INCOMPLETE = 0, HP_DONE = 1 } hp_status;

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} hp_header;

typedef struct {
    const char *method;
    size_t method_len;
    const char *target;
    size_t target_len;
    int minor_version;   /* HTTP/1.0 or HTTP/1.1 */
    hp_header headers[HP_MAX_HEADERS];
    size_t n_headers;
    size_t content_length;
    int keep_alive;
    int error_status;    /* on HP_ERROR: 400, 413, 414, 431, 501 or 505 */
} hp_request;

hp_status hp_parse(const char *buf, size_t len, hp_request *req, size_t *consumed);

/* Case-insensitive header lookup; NULL if absent. */
const hp_header *hp_find(const hp_request *req, const char *name);

#endif
