#ifndef FLUX_CONFIG_H
#define FLUX_CONFIG_H

#include "logger.h"

typedef struct {
    int          port;
    int          threads;
    int          timeout_sec;
    int          backlog;
    const char  *docroot;
    log_level_t  log_level;
} flux_config_t;

void flux_config_defaults(flux_config_t *cfg);

/* Returns 0 on success, 1 if help/version was shown (caller should exit 0),
 * -1 on parse error (already logged to stderr). */
int  flux_config_parse(int argc, char **argv, flux_config_t *cfg);

void flux_config_print(const flux_config_t *cfg);

#endif
