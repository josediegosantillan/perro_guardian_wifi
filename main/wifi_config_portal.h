#ifndef WIFI_CONFIG_PORTAL_H
#define WIFI_CONFIG_PORTAL_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t wifi_config_portal_run(void);
esp_err_t wifi_config_portal_request_on_next_boot(void);
bool wifi_config_portal_requested_on_next_boot(void);

#endif
