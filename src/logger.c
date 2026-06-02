#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include "logger.h"

static log_level_t g_min_level = LOG_DEBUG;

static const char *level_str[] = {
    "DEBUG",
    "INFO ",
    "WARN ",
    "ERROR"
};

void log_init(log_level_t min_level) {
    g_min_level = min_level;
}

void log_write(log_level_t level, const char *file, int line, const char *fmt, ...) {
    if (level < g_min_level) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm t;
    localtime_r(&tv.tv_sec, &t);

    va_list args;
    va_start(args, fmt);

    flockfile(stdout);
    fprintf(stdout, "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] [%s] [%s:%d] ",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
            t.tm_hour, t.tm_min, t.tm_sec,
            tv.tv_usec / 1000,
            level_str[level],
            file, line);
    vfprintf(stdout, fmt, args);
    putchar('\n');
    fflush(stdout);
    funlockfile(stdout);

    va_end(args);
}
