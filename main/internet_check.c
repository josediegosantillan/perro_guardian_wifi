#include "internet_check.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "app_config.h"
#include "app_state.h"

static const char *TAG = "internet_check";

static bool status_code_is_online(int status)
{
    if (APP_STRICT_HTTP_204) {
        return status == 204;
    }

    return status >= 200 && status < 300;
}

static bool check_http_target(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return false;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = APP_INTERNET_CHECK_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "No se pudo crear el cliente HTTP para %s", url);
        return false;
    }

    bool online = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status_code_is_online(status)) {
            ESP_LOGI(TAG, "Internet OK via %s (HTTP %d)", url, status);
            online = true;
        } else {
            ESP_LOGW(TAG, "Respuesta inesperada de %s (HTTP %d)", url, status);
        }
    } else {
        ESP_LOGW(TAG, "Sin respuesta de %s: %s", url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return online;
}

bool internet_check_is_online(void)
{
    const char *targets[] = {
        APP_INTERNET_CHECK_URL_1,
        APP_INTERNET_CHECK_URL_2,
    };

    bool online = false;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        /* Alimenta el Task Watchdog: cada perform puede tardar hasta el timeout HTTP. */
        esp_task_wdt_reset();
        if (check_http_target(targets[i])) {
            online = true;
            break;
        }
    }

    if (online) {
        app_state_set_bits(APP_STATE_INTERNET_OK_BIT);
    } else {
        app_state_clear_bits(APP_STATE_INTERNET_OK_BIT);
        ESP_LOGW(TAG, "Todos los chequeos de Internet fallaron");
    }

    return online;
}
