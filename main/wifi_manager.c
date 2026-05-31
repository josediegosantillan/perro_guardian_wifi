#include "wifi_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "app_config.h"
#include "app_state.h"
#include "wifi_credentials.h"

#define WIFI_RECONNECT_BASE_DELAY_MS 1000
#define WIFI_RECONNECT_MAX_DELAY_MS 60000

static const char *TAG = "wifi_manager";
static bool wifi_started;
static bool wifi_initialized;
static esp_timer_handle_t reconnect_timer;
static int reconnect_attempts;
static int last_disconnect_reason;
static bool auth_failure_seen;

static const char *disconnect_reason_name(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP_RSSI_THRESHOLD";
    default:
        return "UNKNOWN";
    }
}

static bool disconnect_reason_is_auth_failure(int reason)
{
    return reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD;
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Reintentando conexion WiFi (intento %d)", reconnect_attempts);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (reconnect_timer == NULL) {
        esp_wifi_connect();
        return;
    }

    /* Backoff exponencial con tope: base * 2^n, limitado a WIFI_RECONNECT_MAX_DELAY_MS. */
    int delay_ms = WIFI_RECONNECT_BASE_DELAY_MS;
    for (int i = 0; i < reconnect_attempts && delay_ms < WIFI_RECONNECT_MAX_DELAY_MS; i++) {
        delay_ms *= 2;
    }
    if (delay_ms > WIFI_RECONNECT_MAX_DELAY_MS) {
        delay_ms = WIFI_RECONNECT_MAX_DELAY_MS;
    }
    if (reconnect_attempts < 100) {
        reconnect_attempts++;
    }

    ESP_LOGW(TAG, "WiFi desconectado, reintento en %d ms", delay_ms);
    esp_timer_stop(reconnect_timer);
    esp_timer_start_once(reconnect_timer, (uint64_t)delay_ms * 1000);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        reconnect_attempts = 0;
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        last_disconnect_reason = event != NULL ? event->reason : 0;
        app_state_clear_bits(APP_STATE_WIFI_CONNECTED_BIT);
        ESP_LOGW(
            TAG,
            "WiFi desconectado: reason=%d (%s)",
            last_disconnect_reason,
            disconnect_reason_name(last_disconnect_reason));

        if (last_disconnect_reason == WIFI_REASON_NO_AP_FOUND) {
            ESP_LOGW(TAG, "No se encontro el SSID configurado. Confirmar que sea red 2.4 GHz y que el router este encendido");
        } else if (disconnect_reason_is_auth_failure(last_disconnect_reason)) {
            auth_failure_seen = true;
            ESP_LOGE(TAG, "Fallo de autenticacion WiFi. Revisar password/seguridad o reconfigurar desde el portal");
        }

        schedule_reconnect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        reconnect_attempts = 0;
        auth_failure_seen = false;
        app_state_set_bits(APP_STATE_WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi conectado, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_start(void)
{
    if (wifi_started) {
        return ESP_OK;
    }

    if (!wifi_initialized) {
        ESP_RETURN_ON_ERROR(app_state_init(), TAG, "app_state_init fallo");

        const esp_timer_create_args_t timer_args = {
            .callback = reconnect_timer_cb,
            .name = "wifi_reconnect",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &reconnect_timer), TAG, "esp_timer_create fallo");

        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init fallo");
        ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default fallo");
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "esp_wifi_init fallo");

        ESP_RETURN_ON_ERROR(
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL),
            TAG,
            "registro WIFI_EVENT fallo");
        ESP_RETURN_ON_ERROR(
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
            TAG,
            "registro IP_EVENT fallo");

        wifi_initialized = true;
    }

    char ssid[WIFI_CREDENTIALS_MAX_SSID_LEN + 1] = {0};
    char password[WIFI_CREDENTIALS_MAX_PASSWORD_LEN + 1] = {0};
    esp_err_t err = wifi_credentials_load(ssid, sizeof(ssid), password, sizeof(password));

    if (err != ESP_OK) {
        if (strcmp(APP_WIFI_SSID, "CAMBIAR_SSID") == 0 || APP_WIFI_SSID[0] == '\0') {
            ESP_LOGW(TAG, "No hay credenciales WiFi guardadas");
            return ESP_ERR_NOT_FOUND;
        }

        strlcpy(ssid, APP_WIFI_SSID, sizeof(ssid));
        strlcpy(password, APP_WIFI_PASSWORD, sizeof(password));
        ESP_LOGW(TAG, "Usando credenciales WiFi de menuconfig para SSID: %s", ssid);
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    /* Umbral permisivo: acepta redes abiertas, WPA2 y WPA3. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode fallo");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config fallo");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start fallo");

    wifi_started = true;
    last_disconnect_reason = 0;
    auth_failure_seen = false;
    ESP_LOGI(TAG, "WiFi iniciado para SSID: %s", ssid);
    return ESP_OK;
}

bool wifi_manager_wait_connected(int timeout_ms)
{
    EventGroupHandle_t group = app_state_event_group();
    if (group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        group,
        APP_STATE_WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    return (bits & APP_STATE_WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    return app_state_test_bits(APP_STATE_WIFI_CONNECTED_BIT);
}

int wifi_manager_last_disconnect_reason(void)
{
    return last_disconnect_reason;
}

bool wifi_manager_last_disconnect_was_auth_failure(void)
{
    return disconnect_reason_is_auth_failure(last_disconnect_reason);
}

bool wifi_manager_auth_failure_seen(void)
{
    return auth_failure_seen;
}
