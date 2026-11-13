# tinyhttp: an event-driven HTTP/1.1 server in C

A single-threaded static file server in about 600 lines of C, with no dependencies:

- one event loop: kqueue on macOS and the BSDs, `poll()` everywhere else;
- non-blocking sockets;
- keep-alive and pipelining;
- a strict request parser, fuzzed;
- path resolution that cannot leave the document root;
- tests against both event loops, with and without sanitizers;
- a benchmark against nginx.

```
make                       # build/tinyhttp (kqueue) and build/tinyhttp-poll
./build/tinyhttp -p 8080 -r ./www -t 10
make test                  # parser tests, 200,000 fuzz inputs, end-to-end tests on both loops
make sanitize              # the same under AddressSanitizer and UndefinedBehaviorSanitizer
sh bench/run.sh            # against nginx; needs nginx and wrk
```

## How it works

- **`src/parser.c`** parses a request without state. Given everything received so far, it
  returns one of three answers:
  - a complete request (head, plus body if Content-Length says so);
  - "need more bytes";
  - an error and the status to send.

  Because it keeps nothing between calls, a request read in any chunking parses exactly as
  if it had arrived whole. The cost is re-reading the head while it arrives, capped by the
  16 KB head limit. It enforces these rules:
  - token methods, a target with no spaces or control bytes, HTTP/1.0 or 1.1 (else 505), and
    Host required for 1.1;
  - no obsolete header folding;
  - limits on the target (414), the head (431) and the body (413);
  - conflicting Content-Lengths are a 400;
  - any Transfer-Encoding gets a 501. Request smuggling depends on mixing Transfer-Encoding
    with Content-Length, so the mix can never be accepted.
- **`src/path.c`** turns the target into a path under the root:
  - it decodes `%XX`, drops the query and fragment, and resolves `.` and `..`;
  - a path that would climb above the root gets 403;
  - any control byte gets 400, raw or encoded.
- **`src/server.c`** runs the loop:
  - **Order:** a connection answers its requests one at a time, so pipelined responses come
    back in order.
  - **Keep-alive** follows the HTTP/1.0 and 1.1 defaults.
  - **Files** go out in 64 KB chunks with partial writes tracked.
  - **Buffers** start at 8 KB and grow only for a request that needs it.
  - **Cleanup:** idle connections are closed, and SIGPIPE is ignored.

## Testing

- **`tests/test_parser.c`** has 93 checks:
  - parsing, keep-alive rules, pipelining;
  - a request held back until its last byte;
  - every error status;
  - traversal attempts, including encoded dots, encoded NUL, encoded CR LF and raw NUL.
- **`tests/fuzz.c`** mutates valid requests and checks properties that must hold for any
  input:
  - pointers stay inside the buffer;
  - errors use only the statuses the server sends;
  - once a prefix of the input is complete or rejected, every longer input gives the same
    answer;
  - resolved paths are canonical and resolve to themselves.

  It runs 200,000 inputs in `make test` and 100,000 under the sanitizers.
- **`tests/test_server.py`** starts the real binary and checks 18 behaviours over sockets:
  - files, index pages, a 3 MB file on a reused connection;
  - 404, HEAD and 405 with Allow;
  - three pipelined requests answered in order, and Connection: close;
  - four traversal attempts against a file outside the root;
  - oversized headers, Transfer-Encoding;
  - a request sent one byte at a time;
  - HTTP/1.0, the idle timeout, and the server still healthy at the end.

  It runs against the kqueue build, the poll build, and the sanitizer build.

## A bug the fuzzer found

The first version of path resolution passed every hand-written test, but the fuzzer found
180 property violations in its first 200,000 inputs. A raw NUL byte inside a segment (for
example `/%2e%2e<NUL>/etc/passwd`) was copied into the output path. Read as a C string, the
path ends at the NUL, so `open()` would have seen `/..`: the parent of the document root.

The request parser already rejected raw control bytes, so a network client could not reach
it. Still, the path module now refuses control bytes itself, raw or encoded, and the tests
pin both cases. Defence in depth means neither layer trusts the other.

## Against nginx

Both servers ran with one worker on an Apple M4 Pro, serving the same files over keep-alive
connections. nginx had `access_log off` and `sendfile on`. The load came from wrk (1 thread,
10 seconds per row), and the machine was also training a model on its GPU. Neither server
returned an error or a non-2xx response.

| file | connections | tinyhttp requests/s | nginx requests/s | tinyhttp p99 | nginx p99 |
|---|---|---|---|---|---|
| 1 KB | 1 | 24,640 | 29,329 | 0.92 ms | 0.64 ms |
| 1 KB | 50 | **55,018** | 41,225 | 4.00 ms | 10.26 ms |
| 100 KB | 1 | 14,938 | 19,620 | 1.01 ms | 0.56 ms |
| 100 KB | 50 | 30,696 | **45,136** | 5.49 ms | 4.14 ms |

How to read it:

- **Small files under load:** tinyhttp is faster here, but this is not like-for-like. nginx
  sends ETag, Last-Modified and Accept-Ranges and supports far more than this server does.
- **Large files:** nginx is about 1.5 times faster, because `sendfile` passes the file to
  the socket without copying it into user memory. tinyhttp reads each chunk into a buffer
  and writes it out. `sendfile` is the obvious next change, and it would be a different call
  on macOS and Linux.
- **Single connections:** nginx has the lower latency.

Raw output: `results/bench.txt`.

## Limits

- **One thread.** No TLS, no HTTP/2.
- **No caching headers.** No ETag, Last-Modified, conditional or range requests.
- **No epoll backend.** Linux uses `poll()`, which rescans every connection each loop. That
  is fine for hundreds of connections, not tens of thousands. I wrote no epoll path, because
  I could not run it here, and I did not want to ship one untested.
- **Loopback only.** It listens on 127.0.0.1.
