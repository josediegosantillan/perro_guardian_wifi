#include "button_control.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "relay_control.h"
#include "wifi_config_portal.h"

#define BUTTON_TASK_STACK_SIZE 3072
#define BUTTON_TASK_PRIORITY 3
#define BUTTON_POLL_MS 50
#define MANUAL_RESET_HOLD_MS 1000
#define PORTAL_HOLD_MS 3000

static const char *TAG = "button_control";
static TaskHandle_t s_button_task;

static bool gpio_is_valid_input(int gpio)
{
    return gpio >= 0 && gpio <= 39;
}

static esp_err_t configure_button_gpio(int gpio, int active_level)
{
    if (!gpio_is_valid_input(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_level == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_level == 1 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&config);
}

static bool button_pressed(int gpio, int active_level)
{
    if (gpio < 0) {
        return false;
    }

    return gpio_get_level(gpio) == active_level;
}

static void button_task(void *arg)
{
    (void)arg;

    int manual_hold_ms = 0;
    int portal_hold_ms = 0;
    bool manual_fired = false;
    bool portal_fired = false;

    while (true) {
        if (button_pressed(APP_MANUAL_RESET_BUTTON_GPIO, APP_MANUAL_RESET_BUTTON_ACTIVE_LEVEL)) {
            manual_hold_ms += BUTTON_POLL_MS;
            if (!manual_fired && manual_hold_ms >= MANUAL_RESET_HOLD_MS) {
                manual_fired = true;
                ESP_LOGW(TAG, "Boton reset manual: solicitando reinicio fisico del router");
                esp_err_t err = relay_control_request_power_cycle(APP_ROUTER_POWER_OFF_MS, APP_ROUTER_BOOT_WAIT_MS);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "No se pudo solicitar reset manual: %s", esp_err_to_name(err));
                }
            }
        } else {
            manual_hold_ms = 0;
            manual_fired = false;
        }

        if (button_pressed(APP_FORCE_PORTAL_GPIO, APP_FORCE_PORTAL_ACTIVE_LEVEL)) {
            portal_hold_ms += BUTTON_POLL_MS;
            if (!portal_fired && portal_hold_ms >= PORTAL_HOLD_MS) {
                portal_fired = true;
                ESP_LOGW(TAG, "Boton portal: se entrara al portal WiFi tras reiniciar");
                esp_err_t err = wifi_config_portal_request_on_next_boot();
                if (err == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(250));
                    esp_restart();
                }
                ESP_LOGE(TAG, "No se pudo guardar solicitud de portal: %s", esp_err_to_name(err));
            }
        } else {
            portal_hold_ms = 0;
            portal_fired = false;
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_control_init(void)
{
    if (APP_MANUAL_RESET_BUTTON_GPIO >= 0) {
        ESP_RETURN_ON_ERROR(
            configure_button_gpio(APP_MANUAL_RESET_BUTTON_GPIO, APP_MANUAL_RESET_BUTTON_ACTIVE_LEVEL),
            TAG,
            "No se pudo configurar boton reset manual");
        ESP_LOGI(TAG, "Boton reset manual en GPIO%d", APP_MANUAL_RESET_BUTTON_GPIO);
    }

    if (APP_FORCE_PORTAL_GPIO >= 0) {
        ESP_RETURN_ON_ERROR(
            configure_button_gpio(APP_FORCE_PORTAL_GPIO, APP_FORCE_PORTAL_ACTIVE_LEVEL),
            TAG,
            "No se pudo configurar boton portal");
        ESP_LOGI(TAG, "Boton portal WiFi en GPIO%d", APP_FORCE_PORTAL_GPIO);
    }

    if (s_button_task == NULL) {
        BaseType_t created = xTaskCreate(
            button_task, "button_task", BUTTON_TASK_STACK_SIZE, NULL, BUTTON_TASK_PRIORITY, &s_button_task);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
