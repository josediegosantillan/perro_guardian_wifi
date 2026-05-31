#include "wifi_credentials.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define WIFI_CREDENTIALS_NAMESPACE "wifi_creds"
#define WIFI_CREDENTIALS_SSID_KEY "ssid"
#define WIFI_CREDENTIALS_PASSWORD_KEY "password"

static const char *TAG = "wifi_credentials";

esp_err_t wifi_credentials_load(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CREDENTIALS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t stored_ssid_len = ssid_len;
    err = nvs_get_str(handle, WIFI_CREDENTIALS_SSID_KEY, ssid, &stored_ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    size_t stored_password_len = password_len;
    err = nvs_get_str(handle, WIFI_CREDENTIALS_PASSWORD_KEY, password, &stored_password_len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    if (ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Credenciales WiFi cargadas desde NVS para SSID: %s", ssid);
    return ESP_OK;
}

esp_err_t wifi_credentials_save(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > WIFI_CREDENTIALS_MAX_SSID_LEN ||
        strlen(password) > WIFI_CREDENTIALS_MAX_PASSWORD_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CREDENTIALS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo abrir NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, WIFI_CREDENTIALS_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_CREDENTIALS_PASSWORD_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Credenciales WiFi guardadas para SSID: %s", ssid);
    } else {
        ESP_LOGE(TAG, "No se pudieron guardar credenciales en NVS: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_credentials_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CREDENTIALS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}
