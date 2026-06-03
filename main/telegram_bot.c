#include "telegram_bot.h"

#include <ctype.h>
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
#define TELEGRAM_TASK_STACK_SIZE 12288
#define TELEGRAM_TASK_PRIORITY 3
#define TELEGRAM_POLL_MS 5000
#define TELEGRAM_HTTP_TIMEOUT_MS 6000
#define TELEGRAM_RESPONSE_MAX 4096
#define TELEGRAM_URL_MAX 1536
#define TELEGRAM_POST_BODY_MAX 1536
#define TELEGRAM_CALLBACK_ID_MAX 96

#define TELEGRAM_MAIN_MENU_MARKUP \
    "{\"inline_keyboard\":[" \
    "[{\"text\":\"📊 Estado\",\"callback_data\":\"status\"}]," \
    "[{\"text\":\"📶 WiFi\",\"callback_data\":\"wifi\"},{\"text\":\"🌐 Internet\",\"callback_data\":\"internet\"}]," \
    "[{\"text\":\"🔧 Portal WiFi\",\"callback_data\":\"portal_prompt\"},{\"text\":\"⏱ Cooldown\",\"callback_data\":\"cooldown\"}]," \
    "[{\"text\":\"🔄 Reset modem\",\"callback_data\":\"reset_prompt\"}]," \
    "[{\"text\":\"❓ Ayuda\",\"callback_data\":\"help\"},{\"text\":\"🔃 Actualizar\",\"callback_data\":\"menu\"}]" \
    "]}"

#define TELEGRAM_RESET_CONFIRM_MARKUP \
    "{\"inline_keyboard\":[" \
    "[{\"text\":\"✅ Confirmar reset modem\",\"callback_data\":\"reset_confirm\"}]," \
    "[{\"text\":\"❌ Cancelar\",\"callback_data\":\"menu\"}]" \
    "]}"

#define TELEGRAM_PORTAL_CONFIRM_MARKUP \
    "{\"inline_keyboard\":[" \
    "[{\"text\":\"✅ Confirmar portal WiFi\",\"callback_data\":\"portal_confirm\"}]," \
    "[{\"text\":\"❌ Cancelar\",\"callback_data\":\"menu\"}]" \
    "]}"

static const char *TAG = "telegram_bot";
static TaskHandle_t s_telegram_task;
static int s_update_offset;

typedef struct {
    char *data;
    int len;
    int max_len;
} http_buffer_t;

typedef enum {
    TELEGRAM_UPDATE_NONE = 0,
    TELEGRAM_UPDATE_MESSAGE,
    TELEGRAM_UPDATE_CALLBACK,
} telegram_update_type_t;

typedef struct {
    telegram_update_type_t type;
    char command[64];
    char callback_id[TELEGRAM_CALLBACK_ID_MAX];
} telegram_update_t;

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

static esp_err_t telegram_http_post_json(const char *url, const char *body, char *response, int response_len)
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

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

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

static void json_escape(const char *input, char *out, size_t out_len)
{
    size_t written = 0;

    if (out_len == 0) {
        return;
    }
    out[0] = '\0';

    for (const unsigned char *p = (const unsigned char *)input; *p != '\0' && written + 1 < out_len; p++) {
        if (*p == '"' || *p == '\\') {
            if (written + 2 >= out_len) {
                break;
            }
            out[written++] = '\\';
            out[written++] = (char)*p;
        } else if (*p == '\n') {
            if (written + 2 >= out_len) {
                break;
            }
            out[written++] = '\\';
            out[written++] = 'n';
        } else if (*p == '\r') {
            if (written + 2 >= out_len) {
                break;
            }
            out[written++] = '\\';
            out[written++] = 'r';
        } else if (*p == '\t') {
            if (written + 2 >= out_len) {
                break;
            }
            out[written++] = '\\';
            out[written++] = 't';
        } else if (*p >= 0x20) {
            out[written++] = (char)*p;
        }
    }
    out[written] = '\0';
}

static esp_err_t telegram_send_message_markup_with_config(
    const telegram_bot_config_t *config, const char *message, const char *reply_markup)
{
    if (!config_ready(config) || message == NULL || message[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char *url = calloc(1, TELEGRAM_URL_MAX);
    char *escaped_message = calloc(1, 768);
    char *body = calloc(1, TELEGRAM_POST_BODY_MAX);
    char *response = calloc(1, 256);
    if (url == NULL || escaped_message == NULL || body == NULL || response == NULL) {
        free(url);
        free(escaped_message);
        free(body);
        free(response);
        return ESP_ERR_NO_MEM;
    }

    json_escape(message, escaped_message, 768);

    int url_written = snprintf(url, TELEGRAM_URL_MAX, "https://api.telegram.org/bot%s/sendMessage", config->bot_token);
    if (url_written < 0 || url_written >= TELEGRAM_URL_MAX) {
        free(url);
        free(escaped_message);
        free(body);
        free(response);
        return ESP_ERR_INVALID_SIZE;
    }

    int body_written = 0;
    if (reply_markup != NULL && reply_markup[0] != '\0') {
        body_written = snprintf(
            body,
            TELEGRAM_POST_BODY_MAX,
            "{\"chat_id\":\"%s\",\"text\":\"%s\",\"reply_markup\":%s}",
            config->chat_id,
            escaped_message,
            reply_markup);
    } else {
        body_written = snprintf(
            body,
            TELEGRAM_POST_BODY_MAX,
            "{\"chat_id\":\"%s\",\"text\":\"%s\"}",
            config->chat_id,
            escaped_message);
    }

    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (body_written >= 0 && body_written < TELEGRAM_POST_BODY_MAX) {
        err = telegram_http_post_json(url, body, response, 256);
    }

    free(url);
    free(escaped_message);
    free(body);
    free(response);
    return err;
}

static esp_err_t telegram_send_message_with_config(const telegram_bot_config_t *config, const char *message)
{
    return telegram_send_message_markup_with_config(config, message, NULL);
}

static esp_err_t telegram_answer_callback_with_config(
    const telegram_bot_config_t *config, const char *callback_id, const char *message)
{
    if (!config_ready(config) || callback_id == NULL || callback_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char encoded_message[128] = {0};
    url_encode(message != NULL ? message : "OK", encoded_message, sizeof(encoded_message));

    char url[TELEGRAM_URL_MAX] = {0};
    int written = snprintf(
        url,
        sizeof(url),
        "https://api.telegram.org/bot%s/answerCallbackQuery?callback_query_id=%s&text=%s",
        config->bot_token,
        callback_id,
        encoded_message);
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

static void normalize_command(char *command)
{
    if (command == NULL) {
        return;
    }

    char *space = strchr(command, ' ');
    if (space != NULL) {
        *space = '\0';
    }

    char *mention = strchr(command, '@');
    if (mention != NULL) {
        *mention = '\0';
    }
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

static bool read_chat_id_at(const char *chat, char *out, size_t out_len)
{
    if (chat == NULL || out == NULL || out_len == 0) {
        return false;
    }

    chat = strstr(chat, "\"id\"");
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

    size_t written = 0;
    if (*chat == '-' && written + 1 < out_len) {
        out[written++] = *chat++;
    }
    while (isdigit((unsigned char)*chat) && written + 1 < out_len) {
        out[written++] = *chat++;
    }
    out[written] = '\0';
    return written > 0;
}

static bool find_authorized_message(const char *json, const telegram_bot_config_t *config, telegram_update_t *update)
{
    const char *cursor = json;
    bool saw_chat = false;

    while ((cursor = strstr(cursor, "\"chat\":{\"id\"")) != NULL) {
        saw_chat = true;
        char found_chat_id[TELEGRAM_CHAT_ID_MAX_LEN] = {0};
        if (!read_chat_id_at(cursor, found_chat_id, sizeof(found_chat_id))) {
            cursor += strlen("\"chat\":{\"id\"");
            continue;
        }

        ESP_LOGI(TAG, "Telegram chat recibido: %s, esperado: %s", found_chat_id, config->chat_id);
        if (strcmp(found_chat_id, config->chat_id) == 0) {
            if (json_string_after(cursor, "\"text\"", update->command, sizeof(update->command))) {
                update->type = TELEGRAM_UPDATE_MESSAGE;
                return true;
            }
            ESP_LOGW(TAG, "Update Telegram autorizado pero sin texto de comando");
            return false;
        }

        cursor += strlen("\"chat\":{\"id\"");
    }

    if (saw_chat) {
        ESP_LOGW(TAG, "Mensaje Telegram ignorado: chat no autorizado");
    } else {
        ESP_LOGW(TAG, "Update Telegram sin campo chat.id");
    }
    return false;
}

static bool find_authorized_callback(const char *json, const telegram_bot_config_t *config, telegram_update_t *update)
{
    const char *cursor = json;
    bool saw_chat = false;

    while ((cursor = strstr(cursor, "\"callback_query\"")) != NULL) {
        const char *chat = strstr(cursor, "\"chat\":{");
        if (chat == NULL) {
            cursor += strlen("\"callback_query\"");
            continue;
        }

        saw_chat = true;
        char found_chat_id[TELEGRAM_CHAT_ID_MAX_LEN] = {0};
        if (!read_chat_id_at(chat, found_chat_id, sizeof(found_chat_id))) {
            cursor += strlen("\"callback_query\"");
            continue;
        }

        ESP_LOGI(TAG, "Telegram callback chat recibido: %s, esperado: %s", found_chat_id, config->chat_id);
        if (strcmp(found_chat_id, config->chat_id) == 0) {
            if (!json_string_after(cursor, "\"id\"", update->callback_id, sizeof(update->callback_id))) {
                ESP_LOGW(TAG, "Callback Telegram autorizado pero sin callback id");
                return false;
            }
            if (!json_string_after(cursor, "\"data\"", update->command, sizeof(update->command))) {
                ESP_LOGW(TAG, "Callback Telegram autorizado pero sin data");
                return false;
            }
            update->type = TELEGRAM_UPDATE_CALLBACK;
            return true;
        }

        cursor += strlen("\"callback_query\"");
    }

    if (saw_chat) {
        ESP_LOGW(TAG, "Callback Telegram ignorado: chat no autorizado");
    }
    return false;
}

static bool find_authorized_update(const char *json, const telegram_bot_config_t *config, telegram_update_t *update)
{
    if (json == NULL || config == NULL || update == NULL) {
        return false;
    }

    memset(update, 0, sizeof(*update));
    if (find_authorized_callback(json, config, update)) {
        return true;
    }
    return find_authorized_message(json, config, update);
}

static esp_err_t telegram_send_menu(const telegram_bot_config_t *config)
{
    return telegram_send_message_markup_with_config(
        config,
        "🐕 PERRO GUARDIAN WIFI\n📱 Panel de control\nUsa los botones para consultar estado o ejecutar acciones.",
        TELEGRAM_MAIN_MENU_MARKUP);
}

static esp_err_t telegram_send_status(const telegram_bot_config_t *config)
{
    char ssid[33] = {0};
    char ip[16] = {0};
    int rssi = 0;
    wifi_manager_get_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);

    char message[384] = {0};
    snprintf(
        message,
        sizeof(message),
        "🐕 PERRO GUARDIAN WIFI\n📶 WiFi: %s\n📡 SSID: %s\n🌐 IP: %s\n📊 RSSI: %d dBm\n🔌 Internet: %s\n🔄 Modem: %s\n⏱ Cooldown: %s\n🧪 Test mode: %s",
        wifi_manager_is_connected() ? "✅ OK" : "❌ FAIL",
        ssid[0] != '\0' ? ssid : "-",
        ip[0] != '\0' ? ip : "-",
        rssi,
        app_state_test_bits(APP_STATE_INTERNET_OK_BIT) ? "✅ OK" : "❌ FAIL",
        app_state_test_bits(APP_STATE_ROUTER_REBOOTING_BIT) ? "🔄 RESET" : "✅ ON",
        app_state_test_bits(APP_STATE_COOLDOWN_BIT) ? "⚠️ SI" : "✅ NO",
        APP_TEST_MODE ? "⚠️ SI" : "NO");

    return telegram_send_message_markup_with_config(config, message, TELEGRAM_MAIN_MENU_MARKUP);
}

static void handle_callback(const char *data, const char *callback_id, const telegram_bot_config_t *config)
{
    if (data == NULL || data[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "Callback Telegram recibido: %s", data);
    telegram_answer_callback_with_config(config, callback_id, "OK");

    if (strcmp(data, "menu") == 0) {
        telegram_send_menu(config);
    } else if (strcmp(data, "status") == 0 || strcmp(data, "wifi") == 0 || strcmp(data, "internet") == 0) {
        telegram_send_status(config);
    } else if (strcmp(data, "help") == 0) {
        telegram_send_message_markup_with_config(
            config,
            "📋 Comandos disponibles:\n/menu o /panel — abre el panel\n/estado — muestra estado del sistema\n/reset_router — reinicia el modem\n/portal — fuerza portal WiFi\n/cooldown — consulta estado cooldown\n/ayuda — muestra esta ayuda",
            TELEGRAM_MAIN_MENU_MARKUP);
    } else if (strcmp(data, "cooldown") == 0) {
        telegram_send_message_markup_with_config(
            config,
            app_state_test_bits(APP_STATE_COOLDOWN_BIT) ? "⚠️ Cooldown activo" : "✅ Cooldown inactivo",
            TELEGRAM_MAIN_MENU_MARKUP);
    } else if (strcmp(data, "reset_prompt") == 0) {
        if (!config->allow_commands) {
            telegram_send_message_markup_with_config(
                config, "🔒 Comandos remotos deshabilitados desde la web local", TELEGRAM_MAIN_MENU_MARKUP);
            return;
        }
        telegram_send_message_markup_with_config(
            config,
            "⚠️ Esto cortara la alimentacion del modem durante unos segundos. Confirmar?",
            TELEGRAM_RESET_CONFIRM_MARKUP);
    } else if (strcmp(data, "reset_confirm") == 0) {
        if (!config->allow_commands) {
            telegram_send_message_markup_with_config(
                config, "🔒 Comandos remotos deshabilitados desde la web local", TELEGRAM_MAIN_MENU_MARKUP);
            return;
        }
        relay_control_request_power_cycle(APP_ROUTER_POWER_OFF_MS, APP_ROUTER_BOOT_WAIT_MS);
        telegram_send_message_markup_with_config(config, "✅ Reset de modem solicitado", TELEGRAM_MAIN_MENU_MARKUP);
    } else if (strcmp(data, "portal_prompt") == 0) {
        if (!config->allow_commands) {
            telegram_send_message_markup_with_config(
                config, "🔒 Comandos remotos deshabilitados desde la web local", TELEGRAM_MAIN_MENU_MARKUP);
            return;
        }
        telegram_send_message_markup_with_config(
            config,
            "⚠️ El ESP32 reiniciara y abrira el portal WiFi. Confirmar?",
            TELEGRAM_PORTAL_CONFIRM_MARKUP);
    } else if (strcmp(data, "portal_confirm") == 0) {
        if (!config->allow_commands) {
            telegram_send_message_markup_with_config(
                config, "🔒 Comandos remotos deshabilitados desde la web local", TELEGRAM_MAIN_MENU_MARKUP);
            return;
        }
        wifi_config_portal_request_on_next_boot();
        telegram_send_message_with_config(config, "✅ Portal programado. Reiniciando ESP32...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        telegram_send_message_markup_with_config(config, "❓ Boton no reconocido. Usa /menu", TELEGRAM_MAIN_MENU_MARKUP);
    }
}

static void handle_command(const char *command, const telegram_bot_config_t *config)
{
    if (command == NULL || command[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "Comando Telegram recibido: %s", command);

    if (strcmp(command, "/menu") == 0 || strcmp(command, "/panel") == 0) {
        telegram_send_menu(config);
        return;
    }

    if (strcmp(command, "/estado") == 0 || strcmp(command, "/ip") == 0 ||
        strcmp(command, "/wifi") == 0 || strcmp(command, "/internet") == 0) {
        telegram_send_status(config);
        return;
    }

    if (strcmp(command, "/ayuda") == 0) {
        telegram_send_message_markup_with_config(
            config,
            "📋 Comandos disponibles:\n/menu /panel /estado /ip /wifi /internet /reset_router /portal /cooldown /ayuda",
            TELEGRAM_MAIN_MENU_MARKUP);
        return;
    }

    if (!config->allow_commands) {
        telegram_send_message_with_config(config, "🔒 Comandos remotos deshabilitados desde la web local");
        return;
    }

    if (strcmp(command, "/reset_router") == 0) {
        relay_control_request_power_cycle(APP_ROUTER_POWER_OFF_MS, APP_ROUTER_BOOT_WAIT_MS);
        telegram_send_message_with_config(config, "✅ Reset de router solicitado");
    } else if (strcmp(command, "/portal") == 0) {
        wifi_config_portal_request_on_next_boot();
        telegram_send_message_with_config(config, "✅ Portal programado. Reiniciando ESP32...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else if (strcmp(command, "/cooldown") == 0) {
        telegram_send_message_markup_with_config(
            config,
            app_state_test_bits(APP_STATE_COOLDOWN_BIT) ? "⚠️ Cooldown activo" : "✅ Cooldown inactivo",
            TELEGRAM_MAIN_MENU_MARKUP);
    } else {
        telegram_send_message_markup_with_config(config, "❓ Comando no reconocido. Usa /ayuda", TELEGRAM_MAIN_MENU_MARKUP);
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

    char *response = calloc(1, TELEGRAM_RESPONSE_MAX);
    if (response == NULL) {
        ESP_LOGE(TAG, "Sin memoria para respuesta Telegram");
        return;
    }

    if (telegram_http_get(url, response, TELEGRAM_RESPONSE_MAX) != ESP_OK) {
        ESP_LOGW(TAG, "No se pudieron consultar updates Telegram");
        free(response);
        return;
    }

    int last_id = json_last_update_id(response);
    if (last_id < 0) {
        free(response);
        return;
    }
    ESP_LOGI(TAG, "Telegram updates recibidos hasta id %d", last_id);
    if (last_id >= 0) {
        s_update_offset = last_id + 1;
    }

    telegram_update_t update;
    if (find_authorized_update(response, &config, &update)) {
        if (update.type == TELEGRAM_UPDATE_CALLBACK) {
            handle_callback(update.command, update.callback_id, &config);
        } else if (update.type == TELEGRAM_UPDATE_MESSAGE) {
            normalize_command(update.command);
            handle_command(update.command, &config);
        }
    }
    free(response);
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
                telegram_send_message_with_config(&config, "🔴 Alerta: internet caido en PERRO GUARDIAN WIFI");
            }
            if (config.notify_on_router_reset && !prev_rebooting && rebooting) {
                telegram_send_message_with_config(&config, "🔄 Alerta: reinicio de router iniciado");
            }
            if (config.notify_on_cooldown && !prev_cooldown && cooldown) {
                telegram_send_message_with_config(&config, "⏱ Alerta: cooldown activo por limite de reinicios");
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
