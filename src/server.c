/* A single-threaded, event-driven HTTP/1.1 static file server.
 *
 *   tinyhttp [-p PORT] [-r ROOT] [-t IDLE_SECONDS]
 *
 * One loop waits on every socket at once (kqueue on macOS and the BSDs; poll() elsewhere, or
 * when built with -DUSE_POLL). Sockets are non-blocking. A connection reads until it holds a
 * whole request, answers it, and only then looks at the next one in its buffer, so pipelined
 * requests are answered in order. Keep-alive follows HTTP/1.1 (on unless "Connection: close")
 * and HTTP/1.0 (off unless "Connection: keep-alive"). Idle connections are closed.
 */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "parser.h"
#include "path.h"

#if !defined(USE_POLL) && (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__))
#define USE_KQUEUE 1
#include <sys/event.h>
#else
#include <poll.h>
#endif

#define MAX_CONNS 65536
#define IN_CAP (HP_MAX_HEAD + HP_MAX_BODY + 4096)

typedef struct {
    int fd;
    char *in;
    size_t in_len, in_cap;  /* starts small; doubles up to IN_CAP when a request needs it */
    char head[1024];      /* response status line and headers */
    size_t head_len, head_sent;
    int file_fd;
    off_t file_off, file_left;
    int responding;       /* a response is being sent */
    int close_after;      /* close once the response is sent */
    int want_write;       /* waiting for the socket to accept more */
    time_t last;
} conn;

static conn *conns[MAX_CONNS];
static const char *root = ".";
static int idle_seconds = 10;
static volatile sig_atomic_t stopping = 0;
static char file_buf[65536];
#ifdef USE_KQUEUE
static int kq;
#endif

static void on_signal(int sig) {
    (void)sig;
    stopping = 1;
}

static const char *reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Internal Server Error";
    }
}

static const char *mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const char *map[][2] = {{".html", "text/html; charset=utf-8"}, {".css", "text/css"},
                                   {".js", "text/javascript"}, {".json", "application/json"},
                                   {".txt", "text/plain; charset=utf-8"}, {".png", "image/png"},
                                   {".jpg", "image/jpeg"}, {".svg", "image/svg+xml"}, {".pdf", "application/pdf"}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (strcmp(dot, map[i][0]) == 0) return map[i][1];
    return "application/octet-stream";
}

static const char *http_date(void) {
    static char buf[64];
    static time_t cached = 0;
    time_t now = time(NULL);
    if (now != cached) {
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
        cached = now;
    }
    return buf;
}

static void want_write(conn *c, int on) {
    if (c->want_write == on) return;
    c->want_write = on;
#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, c->fd, EVFILT_WRITE, on ? EV_ADD | EV_ENABLE : EV_DELETE, 0, 0, NULL);
    kevent(kq, &ev, 1, NULL, 0, NULL);
#endif
}

static void close_conn(conn *c) {
    if (c->file_fd >= 0) close(c->file_fd);
    close(c->fd);  /* also removes it from the kqueue */
    conns[c->fd] = NULL;
    free(c->in);
    free(c);
}

/* Fill c->head with a response; `body` is a short message for errors, or NULL for a file. */
static void respond(conn *c, int status, const char *type, off_t length, int keep_alive, const char *extra) {
    int n = snprintf(c->head, sizeof c->head,
                     "HTTP/1.1 %d %s\r\nServer: tinyhttp\r\nDate: %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %lld\r\nConnection: %s\r\n%s\r\n",
                     status, reason(status), http_date(), type, (long long)length,
                     keep_alive ? "keep-alive" : "close", extra ? extra : "");
    c->head_len = (size_t)n;
    c->head_sent = 0;
    c->responding = 1;
    c->close_after = !keep_alive;
}

static void respond_error(conn *c, int status, int keep_alive, int head_only) {
    char body[128];
    int n = snprintf(body, sizeof body, "%d %s\n", status, reason(status));
    respond(c, status, "text/plain; charset=utf-8", n, keep_alive, status == 405 ? "Allow: GET, HEAD\r\n" : NULL);
    if (!head_only && c->head_len + (size_t)n < sizeof c->head) {
        memcpy(c->head + c->head_len, body, (size_t)n);
        c->head_len += (size_t)n;
    }
}

static void serve(conn *c, const hp_request *r) {
    int head_only = r->method_len == 4 && memcmp(r->method, "HEAD", 4) == 0;
    int get = r->method_len == 3 && memcmp(r->method, "GET", 3) == 0;
    if (!get && !head_only) {
        respond_error(c, 405, r->keep_alive, 0);
        return;
    }
    char rel[4096], full[8192];
    int status = hp_resolve(r->target, r->target_len, rel, sizeof rel);
    if (status) {
        respond_error(c, status, r->keep_alive, head_only);
        return;
    }
    struct stat st;
    snprintf(full, sizeof full, "%s%s", root, rel);
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        strncat(full, rel[1] ? "/index.html" : "index.html", sizeof full - strlen(full) - 1);
    }
    int fd = open(full, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        respond_error(c, errno == EACCES ? 403 : 404, r->keep_alive, head_only);
        return;
    }
    respond(c, 200, mime(full), st.st_size, r->keep_alive, NULL);
    if (head_only) {
        close(fd);
    } else {
        c->file_fd = fd;
        c->file_off = 0;
        c->file_left = st.st_size;
    }
}

/* Send as much of the response as the socket takes. 1 when the response is finished. */
static int flush(conn *c) {
    while (c->head_sent < c->head_len) {
        ssize_t n = write(c->fd, c->head + c->head_sent, c->head_len - c->head_sent);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        c->head_sent += (size_t)n;
    }
    while (c->file_left > 0) {
        size_t want = c->file_left < (off_t)sizeof file_buf ? (size_t)c->file_left : sizeof file_buf;
        ssize_t got = pread(c->file_fd, file_buf, want, c->file_off);
        if (got <= 0) return -1;
        ssize_t n = write(c->fd, file_buf, (size_t)got);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        c->file_off += n;  /* a short write is re-read from the file next time */
        c->file_left -= n;
    }
    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    c->responding = 0;
    return 1;
}

/* Answer whatever complete requests are buffered, one at a time. 0 to keep the connection. */
static int progress(conn *c) {
    for (;;) {
        if (c->responding) {
            int r = flush(c);
            if (r < 0) return -1;
            if (r == 0) {
                want_write(c, 1);
                return 0;
            }
            want_write(c, 0);
            if (c->close_after) return -1;
        }
        hp_request req;
        size_t used = 0;
        hp_status s = hp_parse(c->in, c->in_len, &req, &used);
        if (s == HP_INCOMPLETE) return 0;
        if (s == HP_ERROR) {
            respond_error(c, req.error_status, 0, 0);
            c->in_len = 0;
            continue;
        }
        serve(c, &req);
        memmove(c->in, c->in + used, c->in_len - used);
        c->in_len -= used;
    }
}

static void on_readable(conn *c) {
    for (;;) {
        if (c->in_len == c->in_cap) {
            if (c->in_cap == IN_CAP) break;  /* the parser rejects anything this large */
            size_t cap = c->in_cap * 2 < IN_CAP ? c->in_cap * 2 : IN_CAP;
            char *bigger = realloc(c->in, cap);
            if (!bigger) {
                close_conn(c);
                return;
            }
            c->in = bigger;
            c->in_cap = cap;
        }
        ssize_t n = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (n == 0) {
            close_conn(c);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn(c);
            return;
        }
        c->in_len += (size_t)n;
        c->last = time(NULL);
    }
    if (progress(c) < 0) close_conn(c);
}

static void on_writable(conn *c) {
    c->last = time(NULL);
    if (progress(c) < 0) close_conn(c);
}

static void accept_all(int lfd) {
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) return;
        if (fd >= MAX_CONNS) {
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        conn *c = calloc(1, sizeof *c);
        c->in_cap = 8192;
        c->in = malloc(c->in_cap);
        if (!c->in) {
            free(c);
            close(fd);
            continue;
        }
        c->fd = fd;
        c->file_fd = -1;
        c->last = time(NULL);
        conns[fd] = c;
#ifdef USE_KQUEUE
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        kevent(kq, &ev, 1, NULL, 0, NULL);
#endif
    }
}

static void close_idle(void) {
    time_t now = time(NULL);
    for (int fd = 0; fd < MAX_CONNS; ++fd)
        if (conns[fd] && now - conns[fd]->last > idle_seconds) close_conn(conns[fd]);
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 1024) != 0) {
        perror("listen");
        exit(1);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

int main(int argc, char **argv) {
    int port = 8080;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "-p") == 0) port = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "-r") == 0) root = argv[i + 1];
        else if (strcmp(argv[i], "-t") == 0) idle_seconds = atoi(argv[i + 1]);
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    int lfd = listen_on(port);
    fprintf(stderr, "tinyhttp: serving %s on 127.0.0.1:%d (%s)\n", root, port,
#ifdef USE_KQUEUE
            "kqueue"
#else
            "poll"
#endif
    );
    time_t last_sweep = time(NULL);
#ifdef USE_KQUEUE
    kq = kqueue();
    struct kevent ev, events[256];
    EV_SET(&ev, lfd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(kq, &ev, 1, NULL, 0, NULL);
    while (!stopping) {
        struct timespec ts = {1, 0};
        int n = kevent(kq, NULL, 0, events, 256, &ts);
        for (int i = 0; i < n; ++i) {
            int fd = (int)events[i].ident;
            if (fd == lfd) {
                accept_all(lfd);
                continue;
            }
            conn *c = conns[fd];
            if (!c) continue;
            if (events[i].filter == EVFILT_READ) on_readable(c);
            else if (events[i].filter == EVFILT_WRITE) on_writable(c);
        }
#else
    static struct pollfd pfd[MAX_CONNS];
    while (!stopping) {
        int n = 0;
        pfd[n++] = (struct pollfd){lfd, POLLIN, 0};
        for (int fd = 0; fd < MAX_CONNS; ++fd)
            if (conns[fd]) pfd[n++] = (struct pollfd){fd, (short)(conns[fd]->want_write ? POLLOUT : POLLIN), 0};
        if (poll(pfd, (nfds_t)n, 1000) > 0) {
            for (int i = 0; i < n; ++i) {
                if (!pfd[i].revents) continue;
                if (pfd[i].fd == lfd) {
                    accept_all(lfd);
                    continue;
                }
                conn *c = conns[pfd[i].fd];
                if (!c) continue;
                if (pfd[i].revents & POLLOUT) on_writable(c);
                else on_readable(c);
            }
        }
#endif
        if (time(NULL) != last_sweep) {
            close_idle();
            last_sweep = time(NULL);
        }
    }
    fprintf(stderr, "tinyhttp: stopped\n");
    return 0;
}
