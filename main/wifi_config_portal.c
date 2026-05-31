#include "wifi_config_portal.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "web_server.h"

#define PORTAL_AP_SSID "PERRO_GUARDIAN_WIFI"
#define PORTAL_AP_PASSWORD "12345678"
#define PORTAL_AP_CHANNEL 6
#define PORTAL_AP_MAX_CONNECTIONS 4
#define PORTAL_NVS_NAMESPACE "portal"
#define PORTAL_NVS_FORCE_KEY "force_once"

static const char *TAG = "wifi_config_portal";

esp_err_t wifi_config_portal_request_on_next_boot(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PORTAL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, PORTAL_NVS_FORCE_KEY, 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool wifi_config_portal_requested_on_next_boot(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PORTAL_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t force_once = 0;
    err = nvs_get_u8(handle, PORTAL_NVS_FORCE_KEY, &force_once);
    if (err == ESP_OK && force_once == 1) {
        ESP_LOGW(TAG, "Portal WiFi solicitado desde NVS");
        nvs_erase_key(handle, PORTAL_NVS_FORCE_KEY);
        nvs_commit(handle);
        nvs_close(handle);
        return true;
    }

    nvs_close(handle);
    return false;
}

esp_err_t wifi_config_portal_run(void)
{
    ESP_LOGW(TAG, "Iniciando portal de configuracion WiFi");

    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, PORTAL_AP_SSID, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, PORTAL_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(PORTAL_AP_SSID);
    ap_config.ap.channel = PORTAL_AP_CHANNEL;
    ap_config.ap.max_connection = PORTAL_AP_MAX_CONNECTIONS;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "esp_wifi_set_mode AP fallo");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "esp_wifi_set_config AP fallo");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start AP fallo");

    ESP_RETURN_ON_ERROR(web_server_start(), TAG, "web_server_start fallo");

    ESP_LOGW(TAG, "Conectate a WiFi '%s' con password '%s'", PORTAL_AP_SSID, PORTAL_AP_PASSWORD);
    ESP_LOGW(TAG, "Abri http://192.168.4.1 para cargar las credenciales");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
