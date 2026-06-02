# FluxServer

A from-scratch HTTP/1.1 server in C, designed as a study in how far you can
push a single-machine reactor with idiomatic Linux primitives — `epoll` +
edge-triggered I/O, a worker pool with per-slot queues and a lock-free
freelist, and `timerfd`-driven connection timeouts.

## Headline numbers

`wrk -t4 -c100 -d15s` on loopback, 4 worker threads, WSL2 / 12 logical cores:

| scenario                          |    QPS    |  P50    |
|-----------------------------------|----------:|--------:|
| `GET /stats` (JSON, no I/O)       | **296 k** |  269 µs |
| `GET /style.css`                  |   3.9 k   |  24.8 ms |
| `GET /` (357 B HTML)              |   3.5 k   |  27.2 ms |

Throughput peaks at 4 worker threads (CPU-bound on this 12-core box, not
mutex-bound — see [`docs/benchmark.md`](docs/benchmark.md) for the
threadpool evolution table and the negative-result detour that surfaced
this).

## Quick start

```bash
make                                        # release build
./flux-server --port 9000 --threads 4
curl http://localhost:9000/stats            # JSON metrics endpoint
curl http://localhost:9000/                 # 357 B index.html
```

## What's interesting

- **Reactor + worker model on one process.** Main thread runs `epoll_wait`
  on the listen fd, accepted fds, and a `timerfd`; events get dispatched
  to one of N worker threads via a per-slot FIFO. Workers handle I/O,
  parsing, and response generation. See [`src/server.c`](src/server.c)
  and [`src/threadpool.c`](src/threadpool.c).
- **Per-worker queue + least-loaded dispatch.** The natural first cut —
  per-worker queue with round-robin dispatch — actually *regressed*
  throughput by ~50 % at `--threads ≥ 8` under oversubscription, because
  a preempted worker's slot would fill while peers went idle. Switching
  to least-loaded dispatch (O(N) atomic-count scan, picks the shallowest
  queue) recovered the shared-queue's adaptive balancing without the
  global mutex. 2.2× over the shared FIFO at `threads=12`.
- **Lock-free SPSC freelist for `task_t`.** Each slot owns a freelist;
  `submit()` (reactor) pops, the slot's worker (single producer of
  pushes) pushes. Single-popper guarantees no ABA. Removes the
  `malloc`/`free` + glibc arena lock from the per-request hot path —
  visible as +17–27 % at low worker counts.
- **`EPOLLET` + `EPOLLONESHOT` + CAS-based connection lifecycle.** ET
  means each readiness fires exactly once and the worker must drain to
  `EAGAIN`. ONESHOT means the conn is parked while a worker holds it,
  preventing a second worker from picking up the same fd. Closing is
  enqueued via a CAS on the conn state, and only the reactor thread
  ever calls `close(fd)` — that's how the fd-UAF window (close-then-
  accept reuses the fd while another worker still holds the stale
  pointer) gets eliminated.
- **Incremental HTTP/1.1 parser.** Byte-by-byte state machine, no
  reliance on `\r\n` arriving in a single read. Unit-tested against
  partial / pipelined / malformed inputs. See
  [`src/http_parser.c`](src/http_parser.c).
- **Graceful shutdown with two-phase drain.** SIGTERM closes the listen
  fd, marks the server draining (forces `Connection: close`), and lets
  in-flight requests finish. After a 5-second grace period, remaining
  conns are force-closed. The event loop exits only when `conn_count()`
  reaches zero.

## Build profiles

| profile  | command           | use case                              |
|----------|-------------------|---------------------------------------|
| release  | `make`            | `-O2`, default                        |
| debug    | `make debug`      | `-O0 -g3`, for gdb                    |
| asan     | `make asan`       | AddressSanitizer + UBSan              |
| tsan     | `make tsan`       | ThreadSanitizer                       |
| ubsan    | `make ubsan`      | UBSan only                            |

`make test` (or `make test PROFILE=asan`) builds and runs the unit
suite under the matching profile.

## Tests

23 unit tests covering the buffer, HTTP parser, and timer-heap modules.
Pass under release **and** asan.

```bash
make test                       # release
make test PROFILE=asan          # under sanitizers
```

CI runs the full matrix on every push (3 profiles × {gcc, clang} = 6
jobs) — see [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Benchmarks

```bash
sudo apt install wrk            # if you don't have it
make
bench/scripts/run_bench.sh      # headline scenarios (~75 s)
bench/scripts/scale_threads.sh  # --threads sweep (~75 s)
```

Results land in `bench/results/`. The writeup with full analysis lives
in [`docs/benchmark.md`](docs/benchmark.md).

## Layout

```
include/       public headers (buf, conn, http, http_parser, threadpool, ...)
src/           implementation
tests/unit/    test_buf, test_http_parser, test_timer_heap
tests/        + tests/Makefile (driver)
bench/scripts/ run_bench.sh, scale_threads.sh
bench/results/ committed run outputs
docs/          benchmark writeup + analysis
examples/www/  default docroot (index.html, style.css)
.github/       CI workflow
Makefile       PROFILE=release|debug|asan|tsan|ubsan
```

## Configuration

```
--port <n>           listen port (default 9050)
--threads <n>        worker count (default 4 — see benchmark.md)
--timeout <secs>     idle connection timeout (default 60)
--docroot <path>     static file root (default examples/www)
--log-level <lvl>    debug | info | warn | error
--backlog <n>        listen(2) backlog
```

## Scope & non-goals

In: HTTP/1.1, keep-alive, pipelining, static files, JSON metrics, graceful
shutdown, connection timeouts.

Not in: TLS, HTTP/2, request logging (access log), `sendfile(2)` zero-copy,
SO_REUSEPORT per-worker reactors. Each was scoped out deliberately to keep
the reactor + threadpool the focus.
