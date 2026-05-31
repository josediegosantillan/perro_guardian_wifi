#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t wifi_manager_start(void);
bool wifi_manager_wait_connected(int timeout_ms);
bool wifi_manager_is_connected(void);
int wifi_manager_last_disconnect_reason(void);
bool wifi_manager_last_disconnect_was_auth_failure(void);
bool wifi_manager_auth_failure_seen(void);
esp_err_t wifi_manager_get_status(char *ssid, size_t ssid_len, char *ip, size_t ip_len, int *rssi);

#endif
