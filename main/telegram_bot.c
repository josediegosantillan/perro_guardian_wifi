#include "telegram_bot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_state.h"
#include "relay_control.h"
#include "wifi_config_portal.h"
#include "wifi_manager.h"

#define TELEGRAM_NAMESPACE "telegram"
#define TELEGRAM_TASK_STACK_SIZE 6144
#define TELEGRAM_TASK_PRIORITY 3
#define TELEGRAM_POLL_MS 10000
#define TELEGRAM_HTTP_TIMEOUT_MS 8000
#define TELEGRAM_RESPONSE_MAX 2048
#define TELEGRAM_URL_MAX 512

static const char *TAG = "telegram_bot";
static TaskHandle_t s_telegram_task;
static int s_update_offset;

typedef struct {
    char *data;
    int len;
    int max_len;
} http_buffer_t;

static bool config_ready(const telegram_bot_config_t *config)
{
    return config != NULL && config->enabled && config->bot_token_configured && config->chat_id[0] != '\0';
}

static esp_err_t nvs_get_bool(nvs_handle_t handle, const char *key, bool default_value, bool *out)
{
    uint8_t value = default_value ? 1 : 0;
    esp_err_t err = nvs_get_u8(handle, key, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    *out = value != 0;
    return ESP_OK;
}

esp_err_t telegram_bot_get_config(telegram_bot_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->notify_on_internet_fail = true;
    config->notify_on_router_reset = true;
    config->notify_on_cooldown = true;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TELEGRAM_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_get_bool(handle, "enabled", false, &config->enabled);
    nvs_get_bool(handle, "net_fail", true, &config->notify_on_internet_fail);
    nvs_get_bool(handle, "reset", true, &config->notify_on_router_reset);
    nvs_get_bool(handle, "cooldown", true, &config->notify_on_cooldown);
    nvs_get_bool(handle, "commands", false, &config->allow_commands);

    size_t len = sizeof(config->bot_token);
    err = nvs_get_str(handle, "token", config->bot_token, &len);
    config->bot_token_configured = err == ESP_OK && config->bot_token[0] != '\0';

    len = sizeof(config->chat_id);
    err = nvs_get_str(handle, "chat_id", config->chat_id, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No se pudo leer chat_id: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t telegram_bot_save_config(const telegram_bot_config_t *config, bool update_token)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TELEGRAM_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, "enabled", config->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "net_fail", config->notify_on_internet_fail ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "reset", config->notify_on_router_reset ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "cooldown", config->notify_on_cooldown ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "commands", config->allow_commands ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "chat_id", config->chat_id);
    }
    if (err == ESP_OK && update_token && config->bot_token[0] != '\0') {
        err = nvs_set_str(handle, "token", config->bot_token);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    http_buffer_t *buffer = (http_buffer_t *)evt->user_data;
    if (buffer == NULL || buffer->data == NULL || buffer->len >= buffer->max_len - 1) {
        return ESP_OK;
    }

    int copy_len = evt->data_len;
    if (buffer->len + copy_len >= buffer->max_len) {
        copy_len = buffer->max_len - buffer->len - 1;
    }
    memcpy(buffer->data + buffer->len, evt->data, copy_len);
    buffer->len += copy_len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static esp_err_t telegram_http_get(const char *url, char *response, int response_len)
{
    http_buffer_t buffer = {
        .data = response,
        .len = 0,
        .max_len = response_len,
    };
    if (response != NULL && response_len > 0) {
        response[0] = '\0';
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &buffer,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    return status >= 200 && status < 300 ? ESP_OK : ESP_FAIL;
}

static void url_encode(const char *input, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t written = 0;

    if (out_len == 0) {
        return;
    }
    out[0] = '\0';

    for (const unsigned char *p = (const unsigned char *)input; *p != '\0' && written + 1 < out_len; p++) {
        bool safe = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                    *p == '.' || *p == '~';
        if (safe) {
            out[written++] = (char)*p;
        } else {
            if (written + 3 >= out_len) {
                break;
            }
            out[written++] = '%';
            out[written++] = hex[*p >> 4];
            out[written++] = hex[*p & 0x0F];
        }
    }
    out[written] = '\0';
}

static esp_err_t telegram_send_message_with_config(const telegram_bot_config_t *config, const char *message)
{
    if (!config_ready(config) || message == NULL || message[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char encoded[256] = {0};
    url_encode(message, encoded, sizeof(encoded));

    char url[TELEGRAM_URL_MAX] = {0};
    int written = snprintf(
        url,
        sizeof(url),
        "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s",
        config->bot_token,
        config->chat_id,
        encoded);
    if (written < 0 || written >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char response[256] = {0};
    return telegram_http_get(url, response, sizeof(response));
}

esp_err_t telegram_bot_notify(const char *message)
{
    telegram_bot_config_t config;
    ESP_RETURN_ON_ERROR(telegram_bot_get_config(&config), TAG, "telegram config");
    return telegram_send_message_with_config(&config, message);
}

esp_err_t telegram_bot_send_test_message(const char *message)
{
    return telegram_bot_notify(message != NULL && message[0] != '\0' ? message : "Prueba desde PERRO_GUARDIAN_WIFI");
}

static bool json_string_after(const char *json, const char *key, char *out, size_t out_len)
{
    const char *pos = strstr(json, key);
    if (pos == NULL) {
        return false;
    }
    pos = strchr(pos, ':');
    if (pos == NULL) {
        return false;
    }
    pos = strchr(pos, '"');
    if (pos == NULL) {
        return false;
    }
    pos++;
    const char *end = strchr(pos, '"');
    if (end == NULL) {
        return false;
    }

    size_t len = (size_t)(end - pos);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, pos, len);
    out[len] = '\0';
    return true;
}

static int json_last_update_id(const char *json)
{
    int last_id = -1;
    const char *cursor = json;
    while ((cursor = strstr(cursor, "\"update_id\":")) != NULL) {
        cursor += strlen("\"update_id\":");
        last_id = atoi(cursor);
    }
    return last_id;
}

static bool chat_authorized(const char *json, const telegram_bot_config_t *config)
{
    const char *chat = strstr(json, "\"chat\":{\"id\":");
    if (chat == NULL) {
        chat = strstr(json, "\"chat\":{\"id\": ");
    }
    if (chat == NULL) {
        return false;
    }
    chat = strchr(chat, ':');
    if (chat == NULL) {
        return false;
    }
    chat++;
    while (*chat == ' ') {
        chat++;
    }
    return strncmp(chat, config->chat_id, strlen(config->chat_id)) == 0;
}

static void handle_command(const char *command, const telegram_bot_config_t *config)
{
    if (command == NULL || command[0] == '\0') {
        return;
    }

    if (strcmp(command, "/estado") == 0 || strcmp(command, "/ip") == 0 ||
        strcmp(command, "/wifi") == 0 || strcmp(command, "/internet") == 0) {
        char ssid[33] = {0};
        char ip[16] = {0};
        int rssi = 0;
        wifi_manager_get_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
        char message[256] = {0};
        snprintf(
            message,
            sizeof(message),
            "PERRO_GUARDIAN_WIFI\nWiFi: %s\nSSID: %s\nIP: %s\nRSSI: %d\nInternet: %s\nRouter: %s\nCooldown: %s",
            wifi_manager_is_connected() ? "OK" : "FAIL",
            ssid,
            ip,
            rssi,
            app_state_test_bits(APP_STATE_INTERNET_OK_BIT) ? "OK" : "FAIL",
            app_state_test_bits(APP_STATE_ROUTER_REBOOTING_BIT) ? "RESET" : "ON",
            app_state_test_bits(APP_STATE_COOLDOWN_BIT) ? "SI" : "NO");
        telegram_send_message_with_config(config, message);
        return;
    }

    if (strcmp(command, "/ayuda") == 0) {
        telegram_send_message_with_config(config, "/estado /ip /wifi /internet /reset_router /portal /cooldown /ayuda");
        return;
    }

    if (!config->allow_commands) {
        telegram_send_message_with_config(config, "Comandos remotos deshabilitados desde la web local");
        return;
    }

    if (strcmp(command, "/reset_router") == 0) {
        relay_control_request_power_cycle(APP_ROUTER_POWER_OFF_MS, APP_ROUTER_BOOT_WAIT_MS);
        telegram_send_message_with_config(config, "Reset de router solicitado");
    } else if (strcmp(command, "/portal") == 0) {
        wifi_config_portal_request_on_next_boot();
        telegram_send_message_with_config(config, "Portal programado. Reiniciando ESP32");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else if (strcmp(command, "/cooldown") == 0) {
        telegram_send_message_with_config(
            config,
            app_state_test_bits(APP_STATE_COOLDOWN_BIT) ? "Cooldown activo" : "Cooldown inactivo");
    }
}

static void telegram_poll_once(void)
{
    telegram_bot_config_t config;
    if (telegram_bot_get_config(&config) != ESP_OK || !config_ready(&config)) {
        return;
    }

    char url[TELEGRAM_URL_MAX] = {0};
    snprintf(
        url,
        sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?timeout=0&offset=%d",
        config.bot_token,
        s_update_offset);

    char response[TELEGRAM_RESPONSE_MAX] = {0};
    if (telegram_http_get(url, response, sizeof(response)) != ESP_OK) {
        ESP_LOGW(TAG, "No se pudieron consultar updates Telegram");
        return;
    }

    int last_id = json_last_update_id(response);
    if (last_id >= 0) {
        s_update_offset = last_id + 1;
    }

    if (!chat_authorized(response, &config)) {
        return;
    }

    char command[64] = {0};
    if (json_string_after(response, "\"text\"", command, sizeof(command))) {
        handle_command(command, &config);
    }
}

static void telegram_task(void *arg)
{
    (void)arg;

    bool prev_internet_ok = app_state_test_bits(APP_STATE_INTERNET_OK_BIT);
    bool prev_rebooting = app_state_test_bits(APP_STATE_ROUTER_REBOOTING_BIT);
    bool prev_cooldown = app_state_test_bits(APP_STATE_COOLDOWN_BIT);

    while (true) {
        telegram_bot_config_t config;
        if (telegram_bot_get_config(&config) == ESP_OK && config_ready(&config)) {
            bool internet_ok = app_state_test_bits(APP_STATE_INTERNET_OK_BIT);
            bool rebooting = app_state_test_bits(APP_STATE_ROUTER_REBOOTING_BIT);
            bool cooldown = app_state_test_bits(APP_STATE_COOLDOWN_BIT);

            if (config.notify_on_internet_fail && prev_internet_ok && !internet_ok) {
                telegram_send_message_with_config(&config, "Alerta: internet caido en PERRO_GUARDIAN_WIFI");
            }
            if (config.notify_on_router_reset && !prev_rebooting && rebooting) {
                telegram_send_message_with_config(&config, "Alerta: reinicio de router iniciado");
            }
            if (config.notify_on_cooldown && !prev_cooldown && cooldown) {
                telegram_send_message_with_config(&config, "Alerta: cooldown activo por limite de reinicios");
            }

            prev_internet_ok = internet_ok;
            prev_rebooting = rebooting;
            prev_cooldown = cooldown;

            telegram_poll_once();
        }

        vTaskDelay(pdMS_TO_TICKS(TELEGRAM_POLL_MS));
    }
}

esp_err_t telegram_bot_init(void)
{
    if (s_telegram_task != NULL) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(
        telegram_task, "telegram_task", TELEGRAM_TASK_STACK_SIZE, NULL, TELEGRAM_TASK_PRIORITY, &s_telegram_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
