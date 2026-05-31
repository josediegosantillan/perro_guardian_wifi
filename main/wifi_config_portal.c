#include "wifi_config_portal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_credentials.h"

#define PORTAL_AP_SSID "PERRO_GUARDIAN_WIFI"
#define PORTAL_AP_PASSWORD "12345678"
#define PORTAL_AP_CHANNEL 6
#define PORTAL_AP_MAX_CONNECTIONS 4
#define PORTAL_POST_BUFFER_SIZE 512
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

static bool append_char(char *out, size_t out_len, size_t *written, char value)
{
    if (*written + 1 >= out_len) {
        return false;
    }

    out[*written] = value;
    (*written)++;
    out[*written] = '\0';
    return true;
}

static bool url_decode_segment(const char *src, size_t src_len, char *out, size_t out_len)
{
    size_t written = 0;

    if (out_len == 0) {
        return false;
    }
    out[0] = '\0';

    for (size_t i = 0; i < src_len; i++) {
        char decoded = src[i];

        if (src[i] == '+') {
            decoded = ' ';
        } else if (src[i] == '%' && i + 2 < src_len &&
                   isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = {src[i + 1], src[i + 2], '\0'};
            decoded = (char)strtol(hex, NULL, 16);
            i += 2;
        }

        if (!append_char(out, out_len, &written, decoded)) {
            return false;
        }
    }

    return true;
}

static void html_escape(const char *src, char *out, size_t out_len)
{
    size_t written = 0;

    if (out_len == 0) {
        return;
    }
    out[0] = '\0';

    while (*src != '\0') {
        const char *replacement = NULL;

        switch (*src) {
        case '&':
            replacement = "&amp;";
            break;
        case '<':
            replacement = "&lt;";
            break;
        case '>':
            replacement = "&gt;";
            break;
        case '"':
            replacement = "&quot;";
            break;
        case '\'':
            replacement = "&#39;";
            break;
        default:
            break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (written + replacement_len >= out_len) {
                break;
            }
            memcpy(out + written, replacement, replacement_len);
            written += replacement_len;
        } else {
            if (written + 1 >= out_len) {
                break;
            }
            out[written++] = *src;
        }

        src++;
    }

    out[written] = '\0';
}

static bool form_value(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *cursor = body;

    while (cursor != NULL && *cursor != '\0') {
        const char *next = strchr(cursor, '&');
        size_t field_len = next == NULL ? strlen(cursor) : (size_t)(next - cursor);

        if (field_len > key_len + 1 && strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            size_t value_len = field_len - key_len - 1;
            return url_decode_segment(cursor + key_len + 1, value_len, out, out_len);
        }

        cursor = next == NULL ? NULL : next + 1;
    }

    return false;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char ssid[WIFI_CREDENTIALS_MAX_SSID_LEN + 1] = {0};
    char password[WIFI_CREDENTIALS_MAX_PASSWORD_LEN + 1] = {0};
    char escaped_ssid[(WIFI_CREDENTIALS_MAX_SSID_LEN * 6) + 1] = {0};
    char saved_hint[512] = {0};

    if (wifi_credentials_load(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        html_escape(ssid, escaped_ssid, sizeof(escaped_ssid));
        snprintf(
            saved_hint,
            sizeof(saved_hint),
            "<p>SSID guardado actualmente: <b>%s</b></p>"
            "<form method=\"post\" action=\"/erase\">"
            "<button class=\"danger\" type=\"submit\">Borrar credenciales</button>"
            "</form>",
            escaped_ssid);
    }

    char page[1800] = {0};
    int written = snprintf(
        page,
        sizeof(page),
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Router Watchdog</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;margin:24px;background:#f7f7f7;color:#222}"
        "main{max-width:420px;margin:auto;background:white;padding:20px;border:1px solid #ddd}"
        "label{display:block;margin-top:14px;font-weight:bold}"
        "input{width:100%%;box-sizing:border-box;padding:10px;margin-top:6px}"
        "button{margin-top:18px;width:100%%;padding:12px;background:#0b5fff;color:white;border:0;font-weight:bold}"
        ".danger{background:#a32121}"
        "</style></head><body><main>"
        "<h1>Router Watchdog</h1>"
        "<p>Carga la red WiFi que debe vigilar el ESP32.</p>"
        "%s"
        "<form method=\"post\" action=\"/save\">"
        "<label>SSID<input name=\"ssid\" maxlength=\"32\" value=\"%s\" required></label>"
        "<label>Password<input name=\"password\" type=\"password\" maxlength=\"64\"></label>"
        "<button type=\"submit\">Guardar y reiniciar</button>"
        "</form></main></body></html>",
        saved_hint,
        escaped_ssid);

    if (written < 0 || written >= (int)sizeof(page)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo generar pagina");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t erase_post_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_credentials_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudieron borrar credenciales: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo borrar");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Credenciales WiFi borradas desde portal");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "<html><body><h1>Borrado</h1><p>El ESP32 se reiniciara.</p></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[PORTAL_POST_BUFFER_SIZE] = {0};
    int remaining = req->content_len;
    int received_total = 0;

    if (remaining <= 0 || remaining >= PORTAL_POST_BUFFER_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Formulario invalido");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int received = httpd_req_recv(req, body + received_total, remaining);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo leer formulario");
            return ESP_FAIL;
        }
        received_total += received;
        remaining -= received;
    }

    char ssid[WIFI_CREDENTIALS_MAX_SSID_LEN + 1] = {0};
    char password[WIFI_CREDENTIALS_MAX_PASSWORD_LEN + 1] = {0};

    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID requerido o demasiado largo");
        return ESP_FAIL;
    }
    if (!form_value(body, "password", password, sizeof(password))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password demasiado largo");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_credentials_save(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudieron guardar credenciales: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo guardar");
        return ESP_FAIL;
    }

    char verify_ssid[WIFI_CREDENTIALS_MAX_SSID_LEN + 1] = {0};
    char verify_password[WIFI_CREDENTIALS_MAX_PASSWORD_LEN + 1] = {0};
    err = wifi_credentials_load(verify_ssid, sizeof(verify_ssid), verify_password, sizeof(verify_password));
    if (err != ESP_OK || strcmp(ssid, verify_ssid) != 0 || strcmp(password, verify_password) != 0) {
        ESP_LOGE(TAG, "Verificacion de credenciales en NVS fallo: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo verificar NVS");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Credenciales verificadas en NVS para SSID: %s", ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "<html><body><h1>Guardado</h1><p>El ESP32 se reiniciara.</p></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_web_server(httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(server, &config), TAG, "httpd_start fallo");

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(*server, &root), TAG, "registro / fallo");

    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(*server, &save), TAG, "registro /save fallo");

    httpd_uri_t erase = {
        .uri = "/erase",
        .method = HTTP_POST,
        .handler = erase_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(*server, &erase), TAG, "registro /erase fallo");
    return ESP_OK;
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

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(start_web_server(&server), TAG, "start_web_server fallo");

    ESP_LOGW(TAG, "Conectate a WiFi '%s' con password '%s'", PORTAL_AP_SSID, PORTAL_AP_PASSWORD);
    ESP_LOGW(TAG, "Abri http://192.168.4.1 para cargar las credenciales");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
