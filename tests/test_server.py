#!/usr/bin/env python3
"""Integration tests: starts the server, talks raw RESP over TCP, checks replies.
Run from the project root:  make test   (or: python3 tests/test_server.py)
Uses only the Python standard library."""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "my_redis_server")


def encode(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        a = a if isinstance(a, bytes) else str(a).encode()
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""

    def _line(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _read(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind in (b"+", b"-"):
            return rest.decode() if kind == b"+" else Exception(rest.decode())
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n == -1:
                return None
            while len(self.buf) < n + 2:
                self.buf += self.sock.recv(4096)
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            return data.decode()
        if kind == b"*":
            return [self._read() for _ in range(int(rest))]
        raise ValueError(line)

    def cmd(self, *args):
        self.sock.sendall(encode(*args))
        return self._read()

    def raw(self, data):
        self.sock.sendall(data)
        return self._read()

    def close(self):
        self.sock.close()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start(port, dump, interval=0):
    p = subprocess.Popen([BINARY, str(port), dump, str(interval)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


passed = failed = 0


def check(name, got, want):
    global passed, failed
    if isinstance(got, Exception):
        got = "ERR:" + str(got)
    if got == want:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name}: got {got!r}, want {want!r}")


def main():
    if not os.path.exists(BINARY):
        sys.exit("Build first: make")
    tmp = tempfile.mkdtemp()
    dump = os.path.join(tmp, "dump.my_rdb")
    port = free_port()
    server = start(port, dump)
    c = Client(port)

    # basics
    check("ping", c.cmd("PING"), "PONG")
    check("ping msg", c.cmd("PING", "hi"), "hi")
    check("echo", c.cmd("echo", "hello world"), "hello world")
    check("set", c.cmd("SET", "name", "nirni"), "OK")
    check("get", c.cmd("GET", "name"), "nirni")
    check("get missing", c.cmd("GET", "nope"), None)
    check("binary safe value", c.cmd("SET", "b", b"a\r\nb\x00c"), "OK")
    check("binary safe get", c.cmd("GET", "b"), "a\r\nb\x00c")
    check("set nx fails", c.cmd("SET", "name", "x", "NX"), None)
    check("set xx ok", c.cmd("SET", "name", "x", "XX"), "OK")
    check("exists", c.cmd("EXISTS", "name", "b", "nope"), 2)
    check("del", c.cmd("DEL", "name", "b", "nope"), 2)
    check("dbsize", c.cmd("DBSIZE"), 0)

    # numbers
    check("incr", c.cmd("INCR", "n"), 1)
    check("incrby", c.cmd("INCRBY", "n", 10), 11)
    check("decrby", c.cmd("DECRBY", "n", 1), 10)
    c.cmd("SET", "s", "abc")
    check("incr non-int", c.cmd("INCR", "s"), "ERR:ERR value is not an integer or out of range")
    check("append", c.cmd("APPEND", "s", "def"), 6)
    check("strlen", c.cmd("STRLEN", "s"), 6)
    check("mset", c.cmd("MSET", "a", 1, "b", 2), "OK")
    check("mget", c.cmd("MGET", "a", "b", "zzz"), ["1", "2", None])

    # lists
    check("rpush", c.cmd("RPUSH", "l", "a", "b", "c"), 3)
    check("lpush", c.cmd("LPUSH", "l", "z"), 4)
    check("lrange all", c.cmd("LRANGE", "l", 0, -1), ["z", "a", "b", "c"])
    check("lrange neg", c.cmd("LRANGE", "l", -2, -1), ["b", "c"])
    check("lindex", c.cmd("LINDEX", "l", 1), "a")
    check("llen", c.cmd("LLEN", "l"), 4)
    check("lpop", c.cmd("LPOP", "l"), "z")
    check("rpop", c.cmd("RPOP", "l"), "c")
    check("type list", c.cmd("TYPE", "l"), "list")

    # hashes
    check("hset", c.cmd("HSET", "h", "f1", "v1", "f2", "v2"), 2)
    check("hget", c.cmd("HGET", "h", "f1"), "v1")
    check("hlen", c.cmd("HLEN", "h"), 2)
    check("hexists", c.cmd("HEXISTS", "h", "f2"), 1)
    check("hgetall", sorted(c.cmd("HGETALL", "h")), ["f1", "f2", "v1", "v2"])
    check("hdel", c.cmd("HDEL", "h", "f1"), 1)

    # errors
    check("wrongtype", c.cmd("LPUSH", "s", "x"), "ERR:WRONGTYPE Operation against a key holding the wrong kind of value")
    check("unknown", c.cmd("NOPE"), "ERR:ERR unknown command 'NOPE'")
    check("arity", c.cmd("GET"), "ERR:ERR wrong number of arguments for 'get' command")

    # keys / expiry
    c.cmd("FLUSHALL")
    c.cmd("MSET", "user:1", "a", "user:2", "b", "other", "c")
    check("keys glob", sorted(c.cmd("KEYS", "user:*")), ["user:1", "user:2"])
    check("keys ?", c.cmd("KEYS", "othe?"), ["other"])
    check("ttl none", c.cmd("TTL", "other"), -1)
    check("ttl missing", c.cmd("TTL", "ghost"), -2)
    check("set px", c.cmd("SET", "temp", "v", "PX", 150), "OK")
    check("pttl set", 0 < c.cmd("PTTL", "temp") <= 150, True)
    time.sleep(0.25)
    check("expired", c.cmd("GET", "temp"), None)
    check("expire", c.cmd("EXPIRE", "other", 100), 1)
    check("ttl", 98 <= c.cmd("TTL", "other") <= 100, True)
    check("persist", c.cmd("PERSIST", "other"), 1)

    # pipelining + fragmented input + inline commands
    c.sock.sendall(encode("SET", "p", 1) + encode("INCR", "p") + encode("GET", "p"))
    check("pipe 1", c._read(), "OK")
    check("pipe 2", c._read(), 2)
    check("pipe 3", c._read(), "2")
    msg = encode("SET", "frag", "value")
    c.sock.sendall(msg[:7]); time.sleep(0.05); c.sock.sendall(msg[7:])
    check("fragmented", c._read(), "OK")
    check("inline", c.raw(b"GET frag\r\n"), "value")
    check("inline ping", c.raw(b"ping\n"), "PONG")

    # concurrency: 20 clients x 200 INCRs on the same key must not lose updates
    c.cmd("SET", "counter", 0)
    def worker():
        w = Client(port)
        for _ in range(200):
            w.cmd("INCR", "counter")
        w.close()
    threads = [threading.Thread(target=worker) for _ in range(20)]
    [t.start() for t in threads]; [t.join() for t in threads]
    check("concurrent incr", c.cmd("GET", "counter"), "4000")

    # large value
    big = "x" * 1_000_000
    check("big set", c.cmd("SET", "big", big), "OK")
    check("big get", c.cmd("GET", "big") == big, True)

    # protocol error closes the connection
    bad = Client(port)
    check("protocol error", bad.raw(b"*abc\r\n"), "ERR:ERR Protocol error: invalid multibulk length")
    bad.close()

    # persistence: SAVE, SIGINT (final dump), restart, data must be back
    c.cmd("FLUSHALL")
    c.cmd("SET", "k", "v")
    c.cmd("RPUSH", "list", "a", "b")
    c.cmd("HSET", "hash", "f", "v")
    c.cmd("SET", "ttlkey", "t", "EX", 1000)
    c.cmd("SET", "bin", b"line1\nline2 \x00")
    check("save", c.cmd("SAVE"), "OK")
    check("quit", c.cmd("QUIT"), "OK")
    c.close()
    server.send_signal(signal.SIGINT)
    check("graceful exit code", server.wait(timeout=10), 0)

    server = start(port, dump)
    c = Client(port)
    check("restored string", c.cmd("GET", "k"), "v")
    check("restored list", c.cmd("LRANGE", "list", 0, -1), ["a", "b"])
    check("restored hash", c.cmd("HGET", "hash", "f"), "v")
    check("restored ttl", 900 <= c.cmd("TTL", "ttlkey") <= 1000, True)
    check("restored binary", c.cmd("GET", "bin"), "line1\nline2 \x00")
    check("restored dbsize", c.cmd("DBSIZE"), 5)
    c.close()
    server.send_signal(signal.SIGTERM)
    check("sigterm exit", server.wait(timeout=10), 0)

    print(f"{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
