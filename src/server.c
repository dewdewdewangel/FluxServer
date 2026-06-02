#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "conn.h"
#include "http.h"
#include "logger.h"
#include "metrics.h"
#include "signal_handler.h"
#include "threadpool.h"
#include "timer_heap.h"

#define MAX_EVENTS 1024

static struct timespec deadline_from_now(int seconds) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += seconds;
    return t;
}

static void arm_timerfd(int timerfd, timer_heap_t *th) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    struct timespec next;
    if (timer_heap_peek(th, &next) == 0) {
        its.it_value = next;
    }
    /* TFD_TIMER_ABSTIME so the deadline is on the same MONOTONIC clock as the heap. */
    if (timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &its, NULL) < 0)
        LOG_ERROR("timerfd_settime: %s", strerror(errno));
}

static void on_timeout(conn_t *c, void *ud) {
    (void)ud;
    if (atomic_load(&c->in_worker)) return; /* worker holds it; let it refresh */
    LOG_INFO("connection fd=%d timed out", c->fd);
    metrics_inc(&g_metrics.timeouts);
    conn_close_async(c);
}

/* Used during the force-close phase of graceful shutdown. */
static int force_close_cb(conn_t *c, void *ud) {
    (void)ud;
    if (atomic_load(&c->state) == CONN_ACTIVE)
        conn_close_async(c);
    return 0;
}

int main(int argc, char **argv) {
    flux_config_t cfg;
    int cfg_rc = flux_config_parse(argc, argv, &cfg);
    if (cfg_rc == 1) return 0;
    if (cfg_rc < 0) return 2;

    log_init(cfg.log_level);
    metrics_init();
    http_set_docroot(cfg.docroot);
    setup_signal_handlers();

    /* SOCK_NONBLOCK saves an fcntl; SOCK_CLOEXEC keeps the fd out of any
     * future fork()+exec() (defence in depth — we don't fork today). */
    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd < 0) { LOG_ERROR("socket: %s", strerror(errno)); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons((uint16_t)cfg.port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("bind: %s", strerror(errno)); exit(1);
    }
    if (listen(server_fd, cfg.backlog) < 0) {
        LOG_ERROR("listen: %s", strerror(errno)); exit(1);
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) { LOG_ERROR("epoll_create1: %s", strerror(errno)); exit(1); }

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) { LOG_ERROR("timerfd_create: %s", strerror(errno)); exit(1); }

    timer_heap_t *th = timer_heap_create(FLUX_MAX_CONN);
    if (!th) { LOG_ERROR("timer_heap_create failed"); exit(1); }

    struct epoll_event event;
    event.events  = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        LOG_ERROR("epoll_ctl server_fd: %s", strerror(errno)); exit(1);
    }
    event.data.fd = timerfd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timerfd, &event) < 0) {
        LOG_ERROR("epoll_ctl timerfd: %s", strerror(errno)); exit(1);
    }

    threadpool_t *pool = threadpool_create(cfg.threads);
    if (!pool) { LOG_ERROR("threadpool_create failed"); exit(1); }

    LOG_INFO("server listening on port %d, threads=%d, timeout=%ds, docroot=%s",
             cfg.port, cfg.threads, cfg.timeout_sec, cfg.docroot);

    struct epoll_event events[MAX_EVENTS];

    /*
     * Graceful shutdown plan:
     *   1. SIGTERM/SIGINT  -> server_running = 0
     *   2. First iteration with server_running == 0:
     *        - close + un-register the listening fd  (no new clients)
     *        - http_set_draining(1)                  (force Connection: close)
     *        - record shutdown_deadline = now + 5 s
     *   3. Keep running the event loop so in-flight requests drain naturally:
     *        - workers finish, write responses
     *        - keep-alive clients see "Connection: close", FIN, leave
     *        - conn_count() decreases as conns reap
     *   4. Past the deadline, force-close any laggers via conn_close_async.
     *   5. Loop exits once server_running == 0 && conn_count() == 0.
     */
    int             shutting_down = 0;
    struct timespec shutdown_deadline = {0};

    while (server_running || conn_count() > 0) {
        /* Short timeout during drain so we can re-check the deadline. */
        int wait_ms = shutting_down ? 100 : -1;
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, wait_ms);

        conn_drain_closed(epoll_fd, th);

        if (!server_running && !shutting_down) {
            LOG_INFO("shutdown initiated, draining %zu connections",
                     conn_count());
            if (server_fd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, NULL);
                close(server_fd);
                server_fd = -1;
            }
            http_set_draining(1);
            clock_gettime(CLOCK_MONOTONIC, &shutdown_deadline);
            shutdown_deadline.tv_sec += 5; /* 5-second grace period */
            shutting_down = 1;
        }

        if (shutting_down) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec >= shutdown_deadline.tv_sec) {
                LOG_INFO("grace period expired, force-closing %zu connections",
                         conn_count());
                conn_foreach(force_close_cb, NULL);
                /* On the next pass conn_drain_closed will reap them. */
            }
        }

        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                for (;;) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    /* accept4 fuses accept + fcntl(O_NONBLOCK) + FD_CLOEXEC
                     * into a single syscall. */
                    int client_fd = accept4(server_fd,
                                            (struct sockaddr *)&client_addr,
                                            &client_len,
                                            SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        LOG_ERROR("accept4: %s", strerror(errno));
                        break;
                    }

                    char ipbuf[INET_ADDRSTRLEN];
                    /* inet_ntop is reentrant; inet_ntoa returns a static buffer. */
                    inet_ntop(AF_INET, &client_addr.sin_addr, ipbuf, sizeof(ipbuf));
                    LOG_INFO("client connected fd=%d %s:%d",
                             client_fd, ipbuf, ntohs(client_addr.sin_port));

                    conn_t *c = conn_create(client_fd, &client_addr);
                    if (!c) {
                        LOG_ERROR("conn_create failed fd=%d", client_fd);
                        close(client_fd);
                        continue;
                    }

                    event.events  = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    event.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0) {
                        LOG_ERROR("epoll_ctl client_fd: %s", strerror(errno));
                        close(client_fd);
                        conn_destroy(c);
                        continue;
                    }
                    timer_heap_push(th, c, deadline_from_now(cfg.timeout_sec));
                }
            } else if (fd == timerfd) {
                uint64_t exp;
                ssize_t r = read(timerfd, &exp, sizeof(exp));
                (void)r;

                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                timer_heap_pop_expired(th, now, on_timeout, NULL);
            } else {
                conn_t *c = conn_get(fd);
                if (!c) {
                    LOG_WARN("event on unregistered fd=%d", fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }
                if (atomic_load(&c->state) == CONN_CLOSING) continue;

                clock_gettime(CLOCK_MONOTONIC, &c->last_active);
                timer_heap_update(th, c, deadline_from_now(cfg.timeout_sec));
                atomic_store(&c->in_worker, 1);
                threadpool_submit(pool, c, epoll_fd);
            }
        }

        arm_timerfd(timerfd, th);
    }

    LOG_INFO("event loop exited (conn_count=%zu); cleaning up", conn_count());
    threadpool_destroy(pool);
    /* One last drain after workers stopped — picks up any conns enqueued
     * by the very last handle_client invocations. */
    conn_drain_closed(epoll_fd, th);
    timer_heap_destroy(th);
    close(timerfd);
    if (server_fd >= 0) close(server_fd);
    close(epoll_fd);
    LOG_INFO("server stopped");
    return 0;
}
