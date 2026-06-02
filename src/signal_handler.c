#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stddef.h>
#include "signal_handler.h"

volatile sig_atomic_t server_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    server_running = 0;
}

void setup_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}
