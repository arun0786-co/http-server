/* Mutation fuzzer for the parser and path resolution.
 *
 *   fuzz ITERATIONS SEED
 *
 * Starts from valid requests, mutates them (flip, insert special bytes, delete, duplicate,
 * truncate) and checks properties that must hold for any input:
 *   - every pointer the parser returns lies inside the buffer, and consumed <= length;
 *   - an error carries one of the statuses the server sends;
 *   - more bytes never undo a decision: once a prefix parses as a complete request, every
 *     longer input gives the same request, and once a prefix is rejected, so is every longer
 *     input (the server reads in arbitrary chunks, so this is what makes chunking safe);
 *   - a resolved path starts with '/', has no ".", ".." or empty segments, and resolves to
 *     itself again.
 * Run it under the sanitizers (make sanitize) to catch memory errors as well.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "path.h"

static const char *seeds[] = {
    "GET / HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /index.html?q=%20x HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n",
    "HEAD /a/b/../c HTTP/1.0\r\n\r\n",
    "POST /submit HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\nhello world",
    "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
    "GET /%2e%2e/etc/passwd HTTP/1.1\r\nHost: a\r\n\r\n",
};
static const char special[] = {'\r', '\n', ':', ' ', '\t', '%', '.', '/', '\0', (char)0xff, '0', '9', '?', '#'};

static unsigned long long rng_state;
static unsigned rnd(unsigned n) {
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)((rng_state >> 33) % n);
}

static int failures = 0;
#define REQUIRE(cond, what)                                    \
    do {                                                       \
        if (!(cond)) {                                         \
            if (failures++ < 10) printf("  violated: %s\n", what); \
        }                                                      \
    } while (0)

static size_t mutate(char *buf, size_t len, size_t cap) {
    int rounds = 1 + (int)rnd(4);
    for (int r = 0; r < rounds; ++r) {
        unsigned op = rnd(5);
        if (len == 0) op = 1;
        size_t at = len ? rnd((unsigned)len) : 0;
        if (op == 0) buf[at] ^= (char)(1u << rnd(8));
        else if (op == 1 && len + 1 < cap) {
            memmove(buf + at + 1, buf + at, len - at);
            buf[at] = rnd(2) ? special[rnd(sizeof special)] : (char)rnd(256);
            ++len;
        } else if (op == 2) {
            size_t n = 1 + rnd(8);
            if (at + n > len) n = len - at;
            memmove(buf + at, buf + at + n, len - at - n);
            len -= n;
        } else if (op == 3) {
            size_t n = 1 + rnd(16);
            if (at + n > len) n = len - at;
            if (len + n < cap) {
                memmove(buf + at + n, buf + at, len - at);
                len += n;
            }
        } else {
            len = at;
        }
    }
    return len;
}

static int same(const hp_request *a, const hp_request *b) {
    return a->method == b->method && a->method_len == b->method_len && a->target == b->target &&
           a->target_len == b->target_len && a->minor_version == b->minor_version &&
           a->n_headers == b->n_headers && a->content_length == b->content_length && a->keep_alive == b->keep_alive;
}

static void check_request(const char *buf, size_t len) {
    hp_request whole, part;
    size_t used = 0, pused = 0;
    hp_status s = hp_parse(buf, len, &whole, &used);
    if (s == HP_DONE) {
        REQUIRE(used <= len, "consumed beyond the input");
        REQUIRE(whole.method >= buf && whole.method + whole.method_len <= buf + len, "method outside buffer");
        REQUIRE(whole.target >= buf && whole.target + whole.target_len <= buf + len, "target outside buffer");
        for (size_t i = 0; i < whole.n_headers; ++i)
            REQUIRE(whole.headers[i].value >= buf && whole.headers[i].value + whole.headers[i].value_len <= buf + len,
                    "header outside buffer");
    }
    if (s == HP_ERROR) {
        int e = whole.error_status;
        REQUIRE(e == 400 || e == 413 || e == 414 || e == 431 || e == 501 || e == 505, "unexpected error status");
    }
    /* monotonicity over every prefix */
    int decided = 0;
    hp_status first = HP_INCOMPLETE;
    hp_request at_first;
    size_t first_used = 0;
    for (size_t i = 0; i <= len; ++i) {
        hp_status p = hp_parse(buf, i, &part, &pused);
        if (!decided && p != HP_INCOMPLETE) {
            decided = 1;
            first = p;
            at_first = part;
            first_used = pused;
        } else if (decided) {
            REQUIRE(p == first, "a decision changed with more bytes");
            if (p == HP_DONE && first == HP_DONE)
                REQUIRE(pused == first_used && same(&part, &at_first), "a complete request changed with more bytes");
        }
    }
}

static void check_path(const char *t, size_t len) {
    char out[2048], again[2048];
    if (len == 0) return;
    if (hp_resolve(t, len, out, sizeof out) != 0) return;
    REQUIRE(out[0] == '/', "resolved path not absolute");
    REQUIRE(strstr(out, "//") == NULL, "empty segment in resolved path");
    REQUIRE(strstr(out, "/./") == NULL && strstr(out, "/../") == NULL, "dot segment left in path");
    size_t n = strlen(out);
    REQUIRE(!(n >= 3 && strcmp(out + n - 3, "/..") == 0) && !(n >= 2 && strcmp(out + n - 2, "/.") == 0),
            "trailing dot segment");
    /* the output is already canonical, unless it contains bytes that decode again */
    if (!strchr(out, '%') && !strchr(out, '?') && !strchr(out, '#')) {
        REQUIRE(hp_resolve(out, n, again, sizeof again) == 0 && strcmp(out, again) == 0, "resolution not idempotent");
    }
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 100000;
    rng_state = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;
    static char buf[4096];
    for (long it = 0; it < iters; ++it) {
        const char *seed = seeds[rnd(sizeof seeds / sizeof seeds[0])];
        size_t len = strlen(seed);
        memcpy(buf, seed, len);
        len = mutate(buf, len, sizeof buf);
        check_request(buf, len);
        /* the target as a path: from the first '/' to the next space */
        char *slash = memchr(buf, '/', len);
        if (slash) {
            size_t n = 0;
            while (slash + n < buf + len && slash[n] != ' ') ++n;
            check_path(slash, n);
        }
    }
    printf("%ld inputs, %d property violations\n", iters, failures);
    return failures != 0;
}
