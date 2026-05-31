#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>

#include "app_config.h"
#include "app_state.h"
#include "button_control.h"
#include "display_oled.h"
#include "relay_control.h"
#include "router_watchdog.h"
#include "status_leds.h"
#include "telegram_bot.h"
#include "web_server.h"
#include "wifi_config_portal.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static bool force_portal_requested(void)
{
    int force_portal_gpio = APP_FORCE_PORTAL_GPIO;

    if (force_portal_gpio < 0) {
        return false;
    }
    if (force_portal_gpio > 39) {
        ESP_LOGE(TAG, "GPIO de portal invalido para ESP32: %d", force_portal_gpio);
        return false;
    }

    if (force_portal_gpio == APP_RELAY_GPIO) {
        ESP_LOGE(TAG, "GPIO de portal no puede ser el mismo que el GPIO del rele (%d)", APP_RELAY_GPIO);
        return false;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << force_portal_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = APP_FORCE_PORTAL_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = APP_FORCE_PORTAL_ACTIVE_LEVEL == 1 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar GPIO de portal %d: %s", force_portal_gpio, esp_err_to_name(err));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    int level = gpio_get_level(force_portal_gpio);
    bool requested = level == APP_FORCE_PORTAL_ACTIVE_LEVEL;
    if (requested) {
        ESP_LOGW(TAG, "Portal WiFi forzado por GPIO %d", force_portal_gpio);
    }

    return requested;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(app_state_init());
    ESP_ERROR_CHECK(relay_control_init());
    ESP_ERROR_CHECK(status_leds_init());
    display_oled_init();

    bool force_portal = force_portal_requested() || wifi_config_portal_requested_on_next_boot();

    err = wifi_manager_start();
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_ERROR_CHECK(err);
    }

    if (force_portal) {
        ESP_LOGW(TAG, "Entrando a portal WiFi por solicitud manual");
        ESP_ERROR_CHECK(wifi_config_portal_run());
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Entrando a portal porque no hay credenciales WiFi");
        ESP_ERROR_CHECK(wifi_config_portal_run());
    }

    if (!wifi_manager_wait_connected(APP_WIFI_CONNECT_TIMEOUT_MS)) {
        if (wifi_manager_auth_failure_seen()) {
            ESP_LOGW(TAG, "Fallo de autenticacion inicial; entrando al portal para corregir credenciales");
            ESP_ERROR_CHECK(wifi_config_portal_run());
        }
        ESP_LOGW(
            TAG,
            "No se pudo conectar al WiFi inicial (reason=%d); se continua la vigilancia sin entrar al portal",
            wifi_manager_last_disconnect_reason());
    }

    ESP_ERROR_CHECK(button_control_init());
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(telegram_bot_init());
    router_watchdog_run();
}
