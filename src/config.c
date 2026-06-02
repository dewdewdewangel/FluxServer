#include "config.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logger.h"

#define FLUX_VERSION "0.1"

void flux_config_defaults(flux_config_t *cfg) {
    cfg->port        = 9000;
    cfg->threads     = 4;
    cfg->timeout_sec = 60;
    cfg->backlog     = 1024;
    cfg->docroot     = "examples/www";
    cfg->log_level   = LOG_INFO;
}

static int parse_log_level(const char *s, log_level_t *out) {
    if      (!strcasecmp(s, "debug")) *out = LOG_DEBUG;
    else if (!strcasecmp(s, "info"))  *out = LOG_INFO;
    else if (!strcasecmp(s, "warn"))  *out = LOG_WARN;
    else if (!strcasecmp(s, "error")) *out = LOG_ERROR;
    else return -1;
    return 0;
}

static void usage(FILE *out, const char *prog) {
    fprintf(out,
        "Usage: %s [options]\n"
        "\n"
        "  -p, --port PORT          listen port             (default 9000)\n"
        "  -t, --threads N          worker thread count     (default 4)\n"
        "      --timeout SEC        idle conn timeout       (default 60)\n"
        "      --backlog N          listen() backlog        (default 1024)\n"
        "      --docroot PATH       static-file root        (default examples/www)\n"
        "  -l, --log-level LVL      debug|info|warn|error   (default info)\n"
        "  -h, --help               show this help\n"
        "  -V, --version            show version\n",
        prog);
}

static int positive_int(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == 0 || *end != 0 || v <= 0 || v > 1000000) return -1;
    *out = (int)v;
    return 0;
}

int flux_config_parse(int argc, char **argv, flux_config_t *cfg) {
    flux_config_defaults(cfg);

    static const struct option longopts[] = {
        {"port",      required_argument, 0, 'p'},
        {"threads",   required_argument, 0, 't'},
        {"timeout",   required_argument, 0,  1 },
        {"backlog",   required_argument, 0,  2 },
        {"docroot",   required_argument, 0,  3 },
        {"log-level", required_argument, 0, 'l'},
        {"help",      no_argument,       0, 'h'},
        {"version",   no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "p:t:l:hV", longopts, NULL)) != -1) {
        switch (c) {
        case 'p':
            if (positive_int(optarg, &cfg->port) < 0 || cfg->port > 65535) {
                fprintf(stderr, "invalid --port: %s\n", optarg); return -1;
            }
            break;
        case 't':
            if (positive_int(optarg, &cfg->threads) < 0 || cfg->threads > 1024) {
                fprintf(stderr, "invalid --threads: %s\n", optarg); return -1;
            }
            break;
        case 1:
            if (positive_int(optarg, &cfg->timeout_sec) < 0) {
                fprintf(stderr, "invalid --timeout: %s\n", optarg); return -1;
            }
            break;
        case 2:
            if (positive_int(optarg, &cfg->backlog) < 0) {
                fprintf(stderr, "invalid --backlog: %s\n", optarg); return -1;
            }
            break;
        case 3:
            cfg->docroot = optarg;
            break;
        case 'l':
            if (parse_log_level(optarg, &cfg->log_level) < 0) {
                fprintf(stderr, "invalid --log-level: %s\n", optarg); return -1;
            }
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 1;
        case 'V':
            printf("flux-server %s\n", FLUX_VERSION);
            return 1;
        default:
            usage(stderr, argv[0]);
            return -1;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "unexpected positional argument: %s\n", argv[optind]);
        return -1;
    }
    return 0;
}

void flux_config_print(const flux_config_t *cfg) {
    fprintf(stderr, "flux-server config: port=%d threads=%d timeout=%ds "
                    "backlog=%d docroot=%s\n",
            cfg->port, cfg->threads, cfg->timeout_sec, cfg->backlog,
            cfg->docroot);
}
