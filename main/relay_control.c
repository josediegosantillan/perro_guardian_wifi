#include "relay_control.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_state.h"

#define RELAY_TASK_STACK_SIZE 3072
#define RELAY_TASK_PRIORITY 5
#define RELAY_QUEUE_LENGTH 2

static const char *TAG = "relay_control";

typedef struct {
    int off_time_ms;
    int boot_wait_ms;
} relay_command_t;

static QueueHandle_t s_relay_queue;
static TaskHandle_t s_relay_task;

static int inactive_level(void)
{
    return APP_RELAY_ACTIVE_LEVEL ? 0 : 1;
}

static esp_err_t validate_relay_gpio(void)
{
    if (APP_RELAY_GPIO < 0 || APP_RELAY_GPIO > 39) {
        ESP_LOGE(TAG, "GPIO de rele invalido para ESP32: %d", APP_RELAY_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    if (APP_RELAY_GPIO >= 34 && APP_RELAY_GPIO <= 39) {
        ESP_LOGE(TAG, "GPIO%d es solo entrada en ESP32; no sirve para el rele", APP_RELAY_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    if (APP_RELAY_GPIO >= 6 && APP_RELAY_GPIO <= 11) {
        ESP_LOGE(TAG, "GPIO%d esta asociado a la flash SPI; no usarlo para el rele", APP_RELAY_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    if (APP_RELAY_GPIO == 0 || APP_RELAY_GPIO == 2 || APP_RELAY_GPIO == 4 ||
        APP_RELAY_GPIO == 5 || APP_RELAY_GPIO == 12 || APP_RELAY_GPIO == 15) {
        ESP_LOGE(TAG, "GPIO%d es pin de strapping en ESP32; no usarlo para el rele", APP_RELAY_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void relay_set_router_on(void)
{
    if (APP_TEST_MODE) {
        ESP_LOGW(TAG, "[TEST_MODE] Se simularia ENCENDER el router (sin tocar GPIO)");
        return;
    }
    gpio_set_level(APP_RELAY_GPIO, inactive_level());
    ESP_LOGI(TAG, "Router encendido");
}

static void relay_set_router_off(void)
{
    if (APP_TEST_MODE) {
        ESP_LOGW(TAG, "[TEST_MODE] Se simularia CORTAR la alimentacion del router (sin tocar GPIO)");
        return;
    }
    gpio_set_level(APP_RELAY_GPIO, APP_RELAY_ACTIVE_LEVEL);
    ESP_LOGW(TAG, "Router apagado (rele activo)");
}

/* Espera troceada para alimentar el Task Watchdog durante esperas largas. */
static void relay_delay_feeding_wdt(int total_ms)
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

static void relay_task(void *arg)
{
    (void)arg;

    /* Suscripcion al Task Watchdog; si no esta inicializado el TWDT, se ignora. */
    esp_task_wdt_add(NULL);

    relay_command_t cmd;
    while (true) {
        if (xQueueReceive(s_relay_queue, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }

        app_state_set_bits(APP_STATE_ROUTER_REBOOTING_BIT);
        ESP_LOGW(TAG, "Power-cycle: corte %d ms, espera de arranque %d ms", cmd.off_time_ms, cmd.boot_wait_ms);

        relay_set_router_off();
        relay_delay_feeding_wdt(cmd.off_time_ms);
        relay_set_router_on();

        ESP_LOGI(TAG, "Esperando %d ms para el arranque del router", cmd.boot_wait_ms);
        relay_delay_feeding_wdt(cmd.boot_wait_ms);

        app_state_clear_bits(APP_STATE_ROUTER_REBOOTING_BIT);
    }
}

esp_err_t relay_control_init(void)
{
    esp_err_t err = validate_relay_gpio();
    if (err != ESP_OK) {
        return err;
    }

    if (APP_TEST_MODE) {
        ESP_LOGW(TAG, "TEST_MODE activo: no se configura ni acciona GPIO%d", APP_RELAY_GPIO);
    } else {
        gpio_set_level(APP_RELAY_GPIO, inactive_level());

        gpio_config_t config = {
            .pin_bit_mask = 1ULL << APP_RELAY_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        err = gpio_config(&config);
        if (err != ESP_OK) {
            return err;
        }

        /* Estado seguro inmediato: router encendido (nivel inactivo). */
        gpio_set_level(APP_RELAY_GPIO, inactive_level());
    }
    ESP_LOGI(TAG, "Rele listo en GPIO %d (TEST_MODE=%d)", APP_RELAY_GPIO, APP_TEST_MODE);

    if (s_relay_queue == NULL) {
        s_relay_queue = xQueueCreate(RELAY_QUEUE_LENGTH, sizeof(relay_command_t));
        if (s_relay_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_relay_task == NULL) {
        BaseType_t created = xTaskCreate(
            relay_task, "relay_task", RELAY_TASK_STACK_SIZE, NULL, RELAY_TASK_PRIORITY, &s_relay_task);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t relay_control_request_power_cycle(int off_time_ms, int boot_wait_ms)
{
    if (s_relay_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    relay_command_t cmd = {
        .off_time_ms = off_time_ms,
        .boot_wait_ms = boot_wait_ms,
    };

    if (xQueueSend(s_relay_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola de rele llena, se descarta el comando");
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool relay_control_is_rebooting(void)
{
    return app_state_test_bits(APP_STATE_ROUTER_REBOOTING_BIT);
}
