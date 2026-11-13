#include "parser.h"

#include <string.h>
#include <strings.h>

static int is_tchar(unsigned char c) {
    /* RFC 9110 token characters */
    if (c >= '0' && c <= '9') return 1;
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return 1;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int is_vchar_or_ws(unsigned char c) { return c == '\t' || (c >= 0x20 && c != 0x7f); }

static hp_status fail(hp_request *req, int status) {
    req->error_status = status;
    return HP_ERROR;
}

/* Position of the CRLF ending the line that starts at `from`, or -1 if not there yet. */
static long line_end(const char *buf, size_t len, size_t from) {
    for (size_t i = from; i + 1 < len; ++i)
        if (buf[i] == '\r' && buf[i + 1] == '\n') return (long)i;
    return -1;
}

const hp_header *hp_find(const hp_request *req, const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < req->n_headers; ++i)
        if (req->headers[i].name_len == n && strncasecmp(req->headers[i].name, name, n) == 0) return &req->headers[i];
    return NULL;
}

static int value_is(const hp_header *h, const char *word) {
    size_t n = strlen(word);
    return h && h->value_len == n && strncasecmp(h->value, word, n) == 0;
}

hp_status hp_parse(const char *buf, size_t len, hp_request *req, size_t *consumed) {
    memset(req, 0, sizeof *req);
    size_t pos = 0;
    /* tolerate empty lines before a request (RFC 9112 section 2.2) */
    while (pos + 1 < len && buf[pos] == '\r' && buf[pos + 1] == '\n') pos += 2;

    long eol = line_end(buf, len, pos);
    if (eol < 0) {
        if (len - pos > HP_MAX_TARGET + 32) return fail(req, 414);
        return HP_INCOMPLETE;
    }
    /* request line: METHOD SP TARGET SP HTTP/1.x */
    size_t i = pos, end = (size_t)eol;
    req->method = buf + i;
    while (i < end && is_tchar((unsigned char)buf[i])) ++i;
    req->method_len = i - pos;
    if (req->method_len == 0 || req->method_len > 16 || i >= end || buf[i] != ' ') return fail(req, 400);
    ++i;
    req->target = buf + i;
    size_t t0 = i;
    while (i < end && buf[i] != ' ') {
        unsigned char c = (unsigned char)buf[i];
        if (c <= 0x20 || c == 0x7f) return fail(req, 400);
        ++i;
    }
    req->target_len = i - t0;
    if (req->target_len == 0) return fail(req, 400);
    if (req->target_len > HP_MAX_TARGET) return fail(req, 414);
    if (i >= end || buf[i] != ' ') return fail(req, 400);
    ++i;
    if (end - i != 8 || memcmp(buf + i, "HTTP/", 5) != 0 || buf[i + 6] != '.' ||
        buf[i + 5] < '0' || buf[i + 5] > '9' || buf[i + 7] < '0' || buf[i + 7] > '9')
        return fail(req, 400);
    if (buf[i + 5] != '1') return fail(req, 505);
    req->minor_version = buf[i + 7] - '0';
    if (req->minor_version > 1) return fail(req, 505);

    /* headers, until an empty line */
    pos = end + 2;
    for (;;) {
        eol = line_end(buf, len, pos);
        if (eol < 0) return len > HP_MAX_HEAD ? fail(req, 431) : HP_INCOMPLETE;
        if ((size_t)eol + 2 > HP_MAX_HEAD) return fail(req, 431);
        end = (size_t)eol;
        if (end == pos) {
            pos += 2;
            break;
        }
        if (buf[pos] == ' ' || buf[pos] == '\t') return fail(req, 400); /* obsolete line folding */
        if (req->n_headers == HP_MAX_HEADERS) return fail(req, 431);
        hp_header *h = &req->headers[req->n_headers++];
        size_t j = pos;
        while (j < end && is_tchar((unsigned char)buf[j])) ++j;
        if (j == pos || j >= end || buf[j] != ':') return fail(req, 400);
        h->name = buf + pos;
        h->name_len = j - pos;
        ++j;
        while (j < end && (buf[j] == ' ' || buf[j] == '\t')) ++j;
        size_t v1 = end;
        while (v1 > j && (buf[v1 - 1] == ' ' || buf[v1 - 1] == '\t')) --v1;
        for (size_t k = j; k < v1; ++k)
            if (!is_vchar_or_ws((unsigned char)buf[k])) return fail(req, 400);
        h->value = buf + j;
        h->value_len = v1 - j;
        pos = end + 2;
    }

    /* message framing: Transfer-Encoding is refused outright, which also rules out the
       Content-Length plus Transfer-Encoding mix that request smuggling relies on */
    if (hp_find(req, "Transfer-Encoding")) return fail(req, 501);
    int seen_cl = 0;
    for (size_t k = 0; k < req->n_headers; ++k) {
        const hp_header *h = &req->headers[k];
        if (h->name_len != 14 || strncasecmp(h->name, "Content-Length", 14) != 0) continue;
        if (h->value_len == 0 || h->value_len > 9) return fail(req, h->value_len > 9 ? 413 : 400);
        size_t v = 0;
        for (size_t c = 0; c < h->value_len; ++c) {
            if (h->value[c] < '0' || h->value[c] > '9') return fail(req, 400);
            v = v * 10 + (size_t)(h->value[c] - '0');
        }
        if (seen_cl && v != req->content_length) return fail(req, 400); /* conflicting lengths */
        req->content_length = v;
        seen_cl = 1;
    }
    if (req->content_length > HP_MAX_BODY) return fail(req, 413);
    if (req->minor_version == 1 && !hp_find(req, "Host")) return fail(req, 400);

    const hp_header *conn = hp_find(req, "Connection");
    req->keep_alive = req->minor_version == 1 ? !value_is(conn, "close") : value_is(conn, "keep-alive");

    if (len - pos < req->content_length) return HP_INCOMPLETE;
    *consumed = pos + req->content_length;
    return HP_DONE;
}
