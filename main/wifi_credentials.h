#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#include <stddef.h>

#include "esp_err.h"

#define WIFI_CREDENTIALS_MAX_SSID_LEN 32
#define WIFI_CREDENTIALS_MAX_PASSWORD_LEN 64

esp_err_t wifi_credentials_load(char *ssid, size_t ssid_len, char *password, size_t password_len);
esp_err_t wifi_credentials_save(const char *ssid, const char *password);
esp_err_t wifi_credentials_erase(void);

#endif
