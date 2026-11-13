"""The server end to end, over real sockets.

    python3 tests/test_server.py build/tinyhttp

Starts the binary on a free port with a temporary document root and checks what a client
sees: files, errors, keep-alive, pipelining, traversal attempts, slow clients and timeouts.
Exits non-zero if anything fails.
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

failures = []


def check(cond, what):
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        failures.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Conn:
    """A client connection with its own receive buffer, so pipelined responses split correctly."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""

    def send(self, data):
        self.s.sendall(data)

    def _fill(self):
        chunk = self.s.recv(65536)
        if not chunk:
            raise ConnectionError("closed by the server")
        self.buf += chunk

    def response(self, head=False):
        """status, headers, body. A HEAD response has no body, whatever Content-Length says."""
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        raw, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = raw.decode("latin-1").split("\r\n")
        status = int(lines[0].split()[1])
        headers = {k.lower(): v.strip() for k, v in (line.split(":", 1) for line in lines[1:])}
        n = 0 if head else int(headers.get("content-length", 0))
        while len(self.buf) < n:
            self._fill()
        body, self.buf = self.buf[:n], self.buf[n:]
        return status, headers, body

    def closed(self):
        try:
            return self.s.recv(1) == b""
        except (ConnectionResetError, socket.timeout):
            return True


def main(binary):
    root = tempfile.mkdtemp()
    with open(os.path.join(root, "index.html"), "w") as f:
        f.write("<h1>home</h1>")
    os.mkdir(os.path.join(root, "docs"))
    with open(os.path.join(root, "docs", "index.html"), "w") as f:
        f.write("docs")
    big = os.urandom(3_000_000)
    with open(os.path.join(root, "big.bin"), "wb") as f:
        f.write(big)
    secret = os.path.join(os.path.dirname(root), "secret.txt")
    with open(secret, "w") as f:
        f.write("do not serve")
    port = free_port()
    proc = subprocess.Popen([binary, "-p", str(port), "-r", root, "-t", "2"], stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.05)

        c = Conn(port)
        c.send(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
        st, h, body = c.response()
        check(st == 200 and body == b"<h1>home</h1>" and h["content-type"].startswith("text/html"), "GET / serves index.html")
        c.send(b"GET /docs HTTP/1.1\r\nHost: t\r\n\r\n")
        st, h, body = c.response()
        check(st == 200 and body == b"docs", "a directory serves its index.html")
        c.send(b"GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n")
        st, h, body = c.response()
        check(st == 200 and body == big, "a 3 MB file arrives intact on the same connection")
        c.send(b"GET /missing HTTP/1.1\r\nHost: t\r\n\r\n")
        st, h, body = c.response()
        check(st == 404 and h["connection"] == "keep-alive", "404 keeps the connection")
        c.send(b"HEAD / HTTP/1.1\r\nHost: t\r\n\r\n")
        st, h, body = c.response(head=True)
        check(st == 200 and h["content-length"] == "13" and body == b"", "HEAD sends headers only")
        c.send(b"DELETE / HTTP/1.1\r\nHost: t\r\n\r\n")
        st, h, body = c.response()
        check(st == 405 and h.get("allow") == "GET, HEAD", "other methods get 405 with Allow")

        c = Conn(port)  # pipelining: three requests in one write, answered in order
        c.send(b"GET /docs/ HTTP/1.1\r\nHost: t\r\n\r\nGET /missing HTTP/1.1\r\nHost: t\r\n\r\n"
               b"GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
        a, b_, last = c.response(), c.response(), c.response()
        check((a[0], b_[0], last[0]) == (200, 404, 200) and a[2] == b"docs", "pipelined requests answered in order")
        check(last[1]["connection"] == "close" and c.closed(), "Connection: close closes after the response")

        for target, want in [(b"/../secret.txt", 403), (b"/%2e%2e/secret.txt", 403), (b"/docs/../../secret.txt", 403),
                             (b"/a%00b", 400)]:
            c = Conn(port)
            c.send(b"GET " + target + b" HTTP/1.1\r\nHost: t\r\n\r\n")
            st, h, body = c.response()
            check(st == want and b"do not serve" not in body, f"{target.decode()} gets {want}")

        c = Conn(port)
        c.send(b"GET / HTTP/1.1\r\nHost: t\r\n" + b"X-Pad: " + b"a" * 20000 + b"\r\n\r\n")
        st, h, body = c.response()
        check(st == 431 and c.closed(), "oversized headers get 431 and a close")

        c = Conn(port)
        c.send(b"GET / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n")
        check(c.response()[0] == 501, "Transfer-Encoding gets 501")

        c = Conn(port)  # a request dribbled in one byte at a time
        for byte in b"GET / HTTP/1.1\r\nHost: t\r\n\r\n":
            c.send(bytes([byte]))
            time.sleep(0.002)
        check(c.response()[0] == 200, "a request sent one byte at a time is answered")

        c = Conn(port)
        c.send(b"GET / HTTP/1.0\r\n\r\n")
        st, h, body = c.response()
        check(st == 200 and c.closed(), "HTTP/1.0 without keep-alive closes")

        c = Conn(port)  # idle timeout is 2 seconds in this test
        c.s.settimeout(6)
        t0 = time.time()
        closed = c.closed()
        check(closed and 1.5 < time.time() - t0 < 5, "an idle connection is closed after the timeout")

        c = Conn(port)  # still serving after all of that
        c.send(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
        check(c.response()[0] == 200, "server still healthy at the end")
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        os.remove(secret)
    print(f"{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
