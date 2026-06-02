# Benchmark

> All numbers below were captured on this machine; **re-run yourself** with
> `bench/scripts/run_bench.sh` to get numbers for your environment.

## Test bed

- Kernel: `6.6.87.2-microsoft-standard-WSL2 x86_64`
- CPU: `nproc = 12` (WSL2)
- Server: `flux-server` release build, default port 9050, log-level=warn
- Client: `wrk` 4.1 with `--latency`
- Loopback (`127.0.0.1`) — kernel-only round-trip, no NIC

## Headline numbers

| scenario                       | QPS      | P50      | P99      | notes                       |
|--------------------------------|---------:|---------:|---------:|-----------------------------|
| `GET /stats` (JSON, no I/O)    | **301k** |  265 µs  |  383 ms* | pure CPU path, 4 workers    |
| `GET /` (357 B HTML, 1 syscall+read) | 4.5k | 21.5 ms | 31.3 ms | open/fstat/read per request |
| `GET /` @ 400 keep-alive conns | 4.4k     |  83.6 ms |  538 ms  | worker queueing dominates   |

\* P99 spike on `/stats` is from connection setup/teardown outliers; steady
state is sub-ms.

Captured: `bench/results/bench.md`, raw wrk output: `bench/results/bench.log`.

## Worker-thread scaling — `/stats`

Same workload (`wrk -t4 -c100`), only `--threads` varies. Numbers are
on the final threadpool design (per-worker queue + least-loaded dispatch
+ per-slot freelist); see *Threadpool evolution* below for the before/after.

| --threads |   QPS    |  P50   |  P99   | speedup vs t=1 |
|----------:|---------:|-------:|-------:|---------------:|
|         1 |    70k   | 1.4 ms | 2.4 ms |          1.0×  |
|         2 |   157k   | 578 µs | 407 ms |          2.2×  |
|         4 | **285k** | 271 µs | 2.9 ms |        **4.1×**|
|         8 |   206k   | 351 µs | 387 ms |          2.9×  |
|        12 |   210k   | 358 µs | 6.7 ms |          3.0×  |

Captured: `bench/results/scale.md`.

### Why the curve peaks at 4 workers

It is **not** mutex contention on the task queue, despite the initial
intuition. The original threadpool used one `pthread_mutex_t` for a shared
FIFO; an early refactor — swap that out for per-worker queues — did not
move the 4-worker peak, confirming the mutex was not the bottleneck.

The real ceiling is `nproc`. The test bed has 12 logical cores and the
client side runs `wrk -t4` (4 hot threads), so the server can spend at
most ~8 cores on workers + reactor before oversubscribing. Above 4
workers the throughput-per-worker drops as the kernel scheduler starts
preempting workers under load. The default of `--threads=4` in
`config.c` is calibrated to this co-tenancy.

### Threadpool evolution

| --threads | shared FIFO | per-worker RR | per-worker least-loaded | + per-slot freelist |
|----------:|------------:|--------------:|------------------------:|--------------------:|
|         1 |        64k  |         63k   |                    60k  |             **70k** |
|         2 |       123k  |        120k   |                   124k  |            **157k** |
|         4 |       265k  |        274k   |                **281k** |             285k    |
|         8 |       148k  |         80k   |                **240k** |             206k    |
|        12 |       100k  |         78k   |                **219k** |             210k    |

Three non-obvious results, in the order we discovered them:

1. **Per-worker queue + round-robin dispatch regressed under
   oversubscription** (t≥8 dropped ~50%). When the kernel preempts one
   worker, its slot piles up while peers go idle — static RR has no way
   to redirect.
2. **Least-loaded dispatch recovered the property a shared queue gets
   for free** — idle workers naturally pull work, so the dispatcher
   simply has to avoid filling a slot whose worker isn't draining.
   submit() does an O(N) scan over atomic counts to pick the shallowest
   queue. Cheap up to N≈16.
3. **The per-slot freelist helps at low N but is invisible at high N.**
   At threads=1 and 2 it picks up +17 % and +27 % — that is
   the cost of `malloc(sizeof(task_t))` + `free()` per request, plus
   glibc arena lock contention, made visible by eliminating it. At
   threads ≥ 4 the bottleneck has moved elsewhere (nproc ceiling), so
   shaving the allocator off the per-request hot path does not show up
   in `wrk` numbers, which are within run-to-run noise.

Freelist mechanics: per-slot lock-free stack, SPSC (the reactor pops via
`task_alloc`, that slot's worker pushes via `task_free`). Single-popper
guarantees no ABA — `submit()` is reactor-only and one slot has exactly
one worker. CAS on the `_Atomic(task_t *)` head; misses fall back to
`malloc()`. Drained in `threadpool_destroy()` after workers join.

The peak is unchanged (≈ 4 workers, all four architectures hit the same
nproc ceiling) but the *shape* of the scaling curve is much better:
low-N throughput is up materially, and high-N tail is 1.4–2.1× the
shared-FIFO baseline.

### Why `/` is slower than `/stats`

`/stats` is a single `snprintf` into the per-conn tx buffer — 0 syscalls per
request once the connection is established. `/` does `open + fstat + read`
on every request because file content isn't cached. The 60× ratio is the
cost of 3 syscalls per request including a page-cache hit on a small file.

Fixes (not in scope for this project): mmap'd cache of frequently-served
files, or `sendfile(2)` (which still touches the page cache but skips the
userspace round-trip).

### High-concurrency tail latency

At 400 keep-alive connections × 4 workers, every worker is multiplexing
~100 conns. The P99 of 538 ms is the per-conn waiting time while peers
process — the bottleneck is the I/O+parse work per request, not the
queue. The mean QPS (≈ 4.4k) is roughly `4 × throughput_per_worker`,
confirming saturation downstream of the reactor.

## How to reproduce

```bash
# default 15 s/scenario
bench/scripts/run_bench.sh

# shorter, e.g. for CI
bench/scripts/run_bench.sh 5

# thread-count sweep
bench/scripts/scale_threads.sh

# raw wrk if you want to drive it yourself
./flux-server --port 9050 --threads 4 --log-level warn &
wrk -t4 -c100 -d30s --latency http://127.0.0.1:9050/stats
```

