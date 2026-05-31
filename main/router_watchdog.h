#ifndef ROUTER_WATCHDOG_H
#define ROUTER_WATCHDOG_H

#include "esp_err.h"

typedef struct {
    int consecutive_failures;
    unsigned long total_reboots;
    unsigned long limit_hits;
    char last_reason[32];
} router_watchdog_status_t;

void router_watchdog_run(void);
esp_err_t router_watchdog_get_status(router_watchdog_status_t *status);
esp_err_t router_watchdog_clear_stats(void);

#endif
