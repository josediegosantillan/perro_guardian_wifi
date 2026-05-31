#include "router_watchdog.h"

#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_state.h"
#include "internet_check.h"
#include "relay_control.h"
#include "wifi_manager.h"

#define WATCHDOG_TASK_STACK_SIZE 4096
#define WATCHDOG_TASK_PRIORITY 4
#define WATCHDOG_STATS_NAMESPACE "watchdog"
#define WATCHDOG_STATS_TOTAL_REBOOTS_KEY "total_reboots"
#define WATCHDOG_STATS_LIMIT_HITS_KEY "limit_hits"
#define WATCHDOG_STATS_LAST_REASON_KEY "last_reason"

static const char *TAG = "router_watchdog";
static volatile int s_consecutive_failures;

static void stats_increment_u32(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WATCHDOG_STATS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo abrir NVS de metricas: %s", esp_err_to_name(err));
        return;
    }

    uint32_t value = 0;
    err = nvs_get_u32(handle, key, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No se pudo leer metrica %s: %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    value++;
    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Metrica %s=%lu", key, (unsigned long)value);
    } else {
        ESP_LOGW(TAG, "No se pudo guardar metrica %s: %s", key, esp_err_to_name(err));
    }
}

static void stats_set_last_reason(const char *reason)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WATCHDOG_STATS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo abrir NVS para ultimo motivo: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(handle, WATCHDOG_STATS_LAST_REASON_KEY, reason);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo guardar ultimo motivo: %s", esp_err_to_name(err));
    }
}

static void stats_record_power_cycle(const char *reason)
{
    stats_increment_u32(WATCHDOG_STATS_TOTAL_REBOOTS_KEY);
    stats_set_last_reason(reason);
}

static bool power_cycle_allowed(TickType_t now, TickType_t *window_start, int *cycles_in_window)
{
    TickType_t window_ticks = pdMS_TO_TICKS(APP_POWER_CYCLE_WINDOW_MS);

    if (*window_start == 0 || now - *window_start >= window_ticks) {
        *window_start = now;
        *cycles_in_window = 0;
    }

    if (*cycles_in_window >= APP_POWER_CYCLE_MAX_PER_WINDOW) {
        return false;
    }

    (*cycles_in_window)++;
    return true;
}

static bool cooldown_active(TickType_t now, TickType_t cooldown_until)
{
    return cooldown_until != 0 && now < cooldown_until;
}

static unsigned long cooldown_remaining_ms(TickType_t now, TickType_t cooldown_until)
{
    if (!cooldown_active(now, cooldown_until)) {
        return 0;
    }

    return (unsigned long)((cooldown_until - now) * portTICK_PERIOD_MS);
}

/* Espera troceada alimentando el Task Watchdog en cada paso. */
static void watchdog_delay(int total_ms)
{
    const int step_ms = 1000;
    int remaining = total_ms;

    while (remaining > 0) {
        int chunk = remaining < step_ms ? remaining : step_ms;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        esp_task_wdt_reset();
        remaining -= chunk;
    }
}

/* Espera la conexion WiFi en tramos de 1 s para no disparar el Task Watchdog. */
static bool wait_wifi_connected(int timeout_ms)
{
    int remaining = timeout_ms;

    while (remaining > 0) {
        int chunk = remaining < 1000 ? remaining : 1000;
        if (wifi_manager_wait_connected(chunk)) {
            return true;
        }
        esp_task_wdt_reset();
        remaining -= chunk;
    }

    return wifi_manager_is_connected();
}

static void watchdog_task(void *arg)
{
    (void)arg;

    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "Esperando %d ms antes de iniciar vigilancia", APP_STARTUP_GRACE_MS);
    watchdog_delay(APP_STARTUP_GRACE_MS);

    int failures = 0;
    int cycles_in_window = 0;
    TickType_t cycle_window_start = 0;
    TickType_t cooldown_until = 0;
    const char *last_failure_reason = "sin_fallo";

    while (true) {
        esp_task_wdt_reset();

        /* Mientras el relay_task ejecuta un power-cycle, no contar fallos. */
        if (relay_control_is_rebooting()) {
            watchdog_delay(APP_WATCHDOG_CHECK_INTERVAL_MS);
            continue;
        }

        ESP_LOGI(TAG, "Chequeando conectividad");

        if (!wait_wifi_connected(APP_WIFI_CONNECT_TIMEOUT_MS)) {
            failures++;
            s_consecutive_failures = failures;
            last_failure_reason = "wifi_local";
            ESP_LOGW(TAG, "WiFi no conectado (%d/%d)", failures, APP_WATCHDOG_MAX_FAILURES);
        } else if (!internet_check_is_online()) {
            failures++;
            s_consecutive_failures = failures;
            last_failure_reason = "internet";
            ESP_LOGW(TAG, "Internet caido (%d/%d)", failures, APP_WATCHDOG_MAX_FAILURES);
        } else {
            failures = 0;
            s_consecutive_failures = failures;
            last_failure_reason = "sin_fallo";
        }

        TickType_t now = xTaskGetTickCount();
        if (!cooldown_active(now, cooldown_until) && app_state_test_bits(APP_STATE_COOLDOWN_BIT)) {
            app_state_clear_bits(APP_STATE_COOLDOWN_BIT);
            ESP_LOGW(TAG, "Cooldown finalizado; se habilitan nuevos reinicios si la ventana lo permite");
        }

        if (failures >= APP_WATCHDOG_MAX_FAILURES) {
            if (cooldown_active(now, cooldown_until)) {
                app_state_set_bits(APP_STATE_COOLDOWN_BIT);
                ESP_LOGE(
                    TAG,
                    "En cooldown por limite de reinicios, faltan aprox. %lu ms; no se reinicia",
                    cooldown_remaining_ms(now, cooldown_until));
                watchdog_delay(APP_WATCHDOG_CHECK_INTERVAL_MS);
                continue;
            }

            if (!power_cycle_allowed(now, &cycle_window_start, &cycles_in_window)) {
                app_state_set_bits(APP_STATE_COOLDOWN_BIT);
                cooldown_until = now + pdMS_TO_TICKS(APP_COOLDOWN_AFTER_LIMIT_MS);
                stats_increment_u32(WATCHDOG_STATS_LIMIT_HITS_KEY);
                ESP_LOGE(
                    TAG,
                    "Limite de reinicios alcanzado (%d cada %d ms). Cooldown por %d ms",
                    APP_POWER_CYCLE_MAX_PER_WINDOW,
                    APP_POWER_CYCLE_WINDOW_MS,
                    APP_COOLDOWN_AFTER_LIMIT_MS);
                watchdog_delay(APP_WATCHDOG_CHECK_INTERVAL_MS);
                continue;
            }

            app_state_clear_bits(APP_STATE_COOLDOWN_BIT);
            ESP_LOGW(
                TAG,
                "Solicitando reinicio del router por falta de conectividad (%d/%d en ventana)",
                cycles_in_window,
                APP_POWER_CYCLE_MAX_PER_WINDOW);

            esp_err_t err = relay_control_request_power_cycle(APP_ROUTER_POWER_OFF_MS, APP_ROUTER_BOOT_WAIT_MS);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "No se pudo encolar el power-cycle: %s", esp_err_to_name(err));
            } else {
                stats_record_power_cycle(last_failure_reason);
            }
            failures = 0;
            s_consecutive_failures = failures;
            /* La relay_task fija ROUTER_REBOOTING_BIT; el proximo ciclo esperara a que termine. */
            watchdog_delay(APP_WATCHDOG_CHECK_INTERVAL_MS);
        } else {
            watchdog_delay(APP_WATCHDOG_CHECK_INTERVAL_MS);
        }
    }
}

void router_watchdog_run(void)
{
    ESP_ERROR_CHECK(app_state_init());
    ESP_ERROR_CHECK(relay_control_init());

    BaseType_t created = xTaskCreate(
        watchdog_task, "watchdog_task", WATCHDOG_TASK_STACK_SIZE, NULL, WATCHDOG_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear watchdog_task");
    }
}

static uint32_t stats_get_u32(nvs_handle_t handle, const char *key)
{
    uint32_t value = 0;
    esp_err_t err = nvs_get_u32(handle, key, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No se pudo leer metrica %s: %s", key, esp_err_to_name(err));
    }
    return value;
}

esp_err_t router_watchdog_get_status(router_watchdog_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    status->consecutive_failures = s_consecutive_failures;
    strlcpy(status->last_reason, "sin_datos", sizeof(status->last_reason));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WATCHDOG_STATS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    status->total_reboots = stats_get_u32(handle, WATCHDOG_STATS_TOTAL_REBOOTS_KEY);
    status->limit_hits = stats_get_u32(handle, WATCHDOG_STATS_LIMIT_HITS_KEY);

    size_t reason_len = sizeof(status->last_reason);
    err = nvs_get_str(handle, WATCHDOG_STATS_LAST_REASON_KEY, status->last_reason, &reason_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No se pudo leer ultimo motivo: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t router_watchdog_clear_stats(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WATCHDOG_STATS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
