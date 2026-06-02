#include "metrics.h"

#include <string.h>

metrics_t g_metrics;

void metrics_init(void) {
    memset(&g_metrics, 0, sizeof(g_metrics));
    clock_gettime(CLOCK_MONOTONIC, &g_metrics.started_at);
}

void metrics_record_response(int status) {
    if      (status >= 200 && status < 300) metrics_inc(&g_metrics.responses_2xx);
    else if (status >= 400 && status < 500) metrics_inc(&g_metrics.responses_4xx);
    else if (status >= 500 && status < 600) metrics_inc(&g_metrics.responses_5xx);
}

uint64_t metrics_uptime_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)(now.tv_sec - g_metrics.started_at.tv_sec);
}
