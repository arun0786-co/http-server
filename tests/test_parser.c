/* Unit tests for the request parser and path resolution. */
#include <stdio.h>
#include <string.h>

#include "parser.h"
#include "path.h"

static int failures = 0, checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);    \
        }                                                                        \
    } while (0)

static hp_status parse(const char *s, hp_request *r, size_t *used) { return hp_parse(s, strlen(s), r, used); }

static int status_of(const char *s) {
    hp_request r;
    size_t used = 0;
    return parse(s, &r, &used) == HP_ERROR ? r.error_status : 0;
}

static void test_simple_get(void) {
    const char *s = "GET /index.html?x=1 HTTP/1.1\r\nHost: a\r\nUser-Agent: t\r\n\r\n";
    hp_request r;
    size_t used = 0;
    CHECK(parse(s, &r, &used) == HP_DONE);
    CHECK(used == strlen(s));
    CHECK(r.method_len == 3 && memcmp(r.method, "GET", 3) == 0);
    CHECK(r.target_len == 15 && memcmp(r.target, "/index.html?x=1", 15) == 0);
    CHECK(r.minor_version == 1 && r.keep_alive == 1 && r.n_headers == 2);
    const hp_header *h = hp_find(&r, "user-agent");
    CHECK(h && h->value_len == 1 && h->value[0] == 't');
}

static void test_keep_alive_rules(void) {
    hp_request r;
    size_t used;
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", &r, &used) == HP_DONE && !r.keep_alive);
    CHECK(parse("GET / HTTP/1.0\r\n\r\n", &r, &used) == HP_DONE && !r.keep_alive);
    CHECK(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &r, &used) == HP_DONE && r.keep_alive);
}

static void test_incomplete_until_the_last_byte(void) {
    const char *s = "POST /f HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    size_t n = strlen(s);
    hp_request r;
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) CHECK(hp_parse(s, i, &r, &used) == HP_INCOMPLETE);
    CHECK(hp_parse(s, n, &r, &used) == HP_DONE && used == n && r.content_length == 5);
}

static void test_pipelined_requests(void) {
    const char *s = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    hp_request r;
    size_t used = 0;
    CHECK(parse(s, &r, &used) == HP_DONE && r.target[1] == 'a');
    size_t first = used;
    CHECK(hp_parse(s + first, strlen(s) - first, &r, &used) == HP_DONE && r.target[1] == 'b');
}

static void test_errors(void) {
    CHECK(status_of("GET /\r\n\r\n") == 400);                                         /* no version */
    CHECK(status_of("GET / HTTP/2.0\r\nHost: a\r\n\r\n") == 505);
    CHECK(status_of("GET / HTTP/1.1\r\n\r\n") == 400);                               /* no Host */
    CHECK(status_of("G(T / HTTP/1.1\r\nHost: a\r\n\r\n") == 400);                    /* bad method */
    CHECK(status_of("GET /a b HTTP/1.1\r\nHost: a\r\n\r\n") == 400);                 /* space in target */
    CHECK(status_of("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n") == 400);         /* obs-fold */
    CHECK(status_of("GET / HTTP/1.1\r\nHost a\r\n\r\n") == 400);                     /* no colon */
    CHECK(status_of("GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n") == 501);
    CHECK(status_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n") == 400);
    CHECK(status_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1x\r\n\r\n") == 400);
    CHECK(status_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999\r\n\r\n") == 413);
    static char big[HP_MAX_TARGET + 64];
    memset(big, 'a', sizeof big - 1);
    memcpy(big, "GET /", 5);
    CHECK(status_of(big) == 414);  /* request line never ends within the limit */
    static char many[HP_MAX_HEAD + 512];
    strcpy(many, "GET / HTTP/1.1\r\nHost: a\r\n");
    while (strlen(many) < HP_MAX_HEAD) strcat(many, "X-Pad: 0123456789012345678901234567890123456789\r\n");
    strcat(many, "\r\n");
    CHECK(status_of(many) == 431);
}

static void test_leading_blank_lines_are_skipped(void) {
    hp_request r;
    size_t used;
    CHECK(parse("\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", &r, &used) == HP_DONE);
}

static const char *resolve(const char *t, int *status) {
    static char out[512];
    *status = hp_resolve(t, strlen(t), out, sizeof out);
    return out;
}

static void test_paths(void) {
    int st;
    CHECK(strcmp(resolve("/", &st), "/") == 0 && st == 0);
    CHECK(strcmp(resolve("/a/./b//c?q=1#f", &st), "/a/b/c") == 0 && st == 0);
    CHECK(strcmp(resolve("/a/b/../c", &st), "/a/c") == 0 && st == 0);
    CHECK(strcmp(resolve("/caf%C3%A9.txt", &st), "/caf\xC3\xA9.txt") == 0 && st == 0);
    resolve("/../etc/passwd", &st);
    CHECK(st == 403);
    resolve("/a/%2e%2e/%2E%2E/etc/passwd", &st);  /* encoded dots */
    CHECK(st == 403);
    resolve("/a%00b", &st);
    CHECK(st == 400);
    resolve("/a%zz", &st);
    CHECK(st == 400);
    resolve("/a%2", &st);
    CHECK(st == 400);
    /* a raw NUL would truncate the path when opened: "/..\0x" is "/.." to open() */
    char raw[] = {'/', '.', '.', '\0', '/', 'x'};
    char out[64];
    CHECK(hp_resolve(raw, sizeof raw, out, sizeof out) == 400);
    char raw2[] = {'/', '%', '2', 'e', '%', '2', 'e', '\0', '/', 'e'};
    CHECK(hp_resolve(raw2, sizeof raw2, out, sizeof out) == 400);
    resolve("/a%0d%0aSet-Cookie:x", &st);  /* encoded CR LF */
    CHECK(st == 400);
    resolve("relative", &st);
    CHECK(st == 400);
    resolve("/a\\..\\b", &st);
    CHECK(st == 400);
}

int main(void) {
    test_simple_get();
    test_keep_alive_rules();
    test_incomplete_until_the_last_byte();
    test_pipelined_requests();
    test_errors();
    test_leading_blank_lines_are_skipped();
    test_paths();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
