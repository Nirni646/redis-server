# In-Memory Redis Server (C++)

A multi-client, in-memory key-value database written in C++17. It speaks the **Redis Serialization Protocol (RESP)** over **TCP sockets** (POSIX socket API), so the real `redis-cli` and Redis client libraries work against it.

**Tools & Technologies:** C++17, TCP Sockets, POSIX Socket API, RESP Protocol, STL, Linux

## Build and run

Requirements: Linux (or WSL on Windows), `g++` 7+ and `make`.

```bash
sudo apt update && sudo apt install -y build-essential   # only if g++/make are missing

unzip redis-server.zip && cd redis-server
make                    # builds ./my_redis_server
./my_redis_server       # listens on port 6379
```

Options (all optional): `./my_redis_server [port] [dump_file] [save_interval_seconds]`

```bash
./my_redis_server 6380                      # custom port
./my_redis_server 6379 mydata.my_rdb 60     # custom dump file, auto-save every 60s (0 = off)
```

Stop with **Ctrl+C**. The server finishes in-flight clients and writes a final dump before exiting.

## Connect to it

Open a second terminal.

```bash
# Option 1: redis-cli (sudo apt install redis-tools)
redis-cli -p 6379
127.0.0.1:6379> SET name nirni
OK
127.0.0.1:6379> GET name
"nirni"

# Option 2: netcat (inline commands, no install needed)
printf 'SET name nirni\r\nGET name\r\n' | nc -q1 localhost 6379

# Option 3: telnet
telnet localhost 6379
```

## Run the tests

```bash
make test        # starts the server, runs 69 checks over raw RESP, then shuts it down
```

Covers all command groups, binary-safe values, pipelining, fragmented packets, expiry, 20 concurrent clients, protocol errors, persistence across a restart and graceful SIGINT/SIGTERM shutdown (also run clean under AddressSanitizer, UBSan and ThreadSanitizer).

## Supported commands

| Group | Commands |
|---|---|
| Connection | `PING [msg]`, `ECHO`, `QUIT`, `COMMAND` |
| Keys | `DEL`, `EXISTS`, `KEYS` (`*` and `?` globs), `TYPE`, `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST`, `DBSIZE`, `FLUSHALL`, `FLUSHDB` |
| Strings | `SET key value [NX\|XX] [EX s\|PX ms]`, `GET`, `MSET`, `MGET`, `INCR`, `DECR`, `INCRBY`, `DECRBY`, `APPEND`, `STRLEN` |
| Lists | `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LRANGE`, `LINDEX` |
| Hashes | `HSET`, `HGET`, `HDEL`, `HGETALL`, `HLEN`, `HEXISTS` |
| Persistence | `SAVE` |

## Architecture

```
include/ + src/
  main.cpp                 parses args, loads the dump file, starts the server
  RedisServer              TCP listener, accept loop, one thread per client,
                           background save thread, SIGINT/SIGTERM handling
  RedisCommandHandler      RESP parser (arrays + inline) and command dispatch table
  RedisDatabase            Singleton, mutex-protected store: strings, lists, hashes, TTLs,
                           binary-safe file persistence
tests/test_server.py       integration tests (stdlib Python only)
```

- **Networking:** `socket` / `bind` / `listen` / `accept` / `recv` / `send`. Each connection gets its own thread and stays open for any number of commands (persistent communication). `TCP_NODELAY` is on.
- **RESP parsing:** incremental. It handles pipelined commands, commands split across packets, and enforces size limits. Malformed input gets a protocol error and the connection is closed.
- **Extensible commands:** each command is one function plus one line in the table in `RedisCommandHandler.cpp`, with arity checked centrally.
- **Concurrency:** one `std::mutex` guards the database so every command is atomic; no lost updates under parallel clients.
- **Expiry:** keys expire lazily on access and are skipped by `KEYS`, `DBSIZE` and dumps.
- **Persistence:** the database is dumped every 300 s (configurable), on `SAVE` and on shutdown. The dump is written to `<file>.tmp` and atomically renamed, so a crash mid-write never corrupts the previous dump. It is reloaded at startup.
- **Shutdown:** the signal handler only sets an atomic flag and calls `shutdown(2)` on the listening socket (async-signal-safe); the main thread then wakes clients, joins the save thread and does the final dump.

## Adding a command

```cpp
// RedisCommandHandler.cpp
std::string cmdGetDel(const Args& a){ /* a[0] is the command name */ }

// in commandTable():   arity > 0 = exact, < 0 = "at least"
{"GETDEL", {cmdGetDel, 2}},
```

## Notes / limitations

- Linux/WSL only (POSIX sockets). Single node, no replication or clustering.
- Thread-per-client is simple and fine for hundreds of connections; an `epoll` event loop would be the next step for thousands.
