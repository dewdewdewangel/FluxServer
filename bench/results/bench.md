# FluxServer benchmark

- date: 2026-06-02T02:59:46Z
- host: `6.6.87.2-microsoft-standard-WSL2 x86_64 / nproc=12`
- duration: 15s per scenario
- server: flux-server (release), 4 worker threads, port 9050
- tool: wrk wrk debian/4.1.0-4build3 [epoll] Copyright (C) 2012 Will Glozer

| scenario        | wrk-t |  conn |     QPS    |  P50 lat | P99 lat | errors |
|-----------------|------:|------:|-----------:|---------:|--------:|--------|
| / (357B html)   |    1 |    10 |    3140.08 |   2.98ms | 409.44ms |  |
| / (357B html)   |    4 |   100 |    3544.70 |  27.16ms |  45.84ms |  |
| / (357B html)   |    8 |   400 |    3428.34 | 105.45ms | 635.17ms |  |
| /style.css      |    4 |   100 |    3914.05 |  24.78ms |  38.92ms |  |
| /stats (JSON)   |    4 |   100 |  295635.73 | 269.00us | 552.47ms |  |
