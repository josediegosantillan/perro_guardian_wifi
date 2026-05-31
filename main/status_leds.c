#include "status_leds.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_state.h"

#define LED_TASK_STACK_SIZE 2048
#define LED_TASK_PRIORITY 2
#define LED_UPDATE_MS 250

static const char *TAG = "status_leds";
static TaskHandle_t s_led_task;

static bool gpio_can_output(int gpio)
{
    if (gpio < 0) {
        return false;
    }

    if (gpio >= 34 && gpio <= 39) {
        return false;
    }

    if (gpio >= 6 && gpio <= 11) {
        return false;
    }

    return gpio <= 33;
}

static int led_inactive_level(void)
{
    return APP_STATUS_LED_ACTIVE_LEVEL ? 0 : 1;
}

static void set_led(int gpio, bool on)
{
    if (gpio < 0) {
        return;
    }

    gpio_set_level(gpio, on ? APP_STATUS_LED_ACTIVE_LEVEL : led_inactive_level());
}

static esp_err_t configure_led(int gpio, const char *name)
{
    if (gpio < 0) {
        return ESP_OK;
    }

    if (!gpio_can_output(gpio)) {
        ESP_LOGE(TAG, "GPIO%d no sirve como salida para LED %s", gpio, name);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_set_level(gpio, led_inactive_level());

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) {
        set_led(gpio, false);
        ESP_LOGI(TAG, "LED %s en GPIO%d", name, gpio);
    }

    return err;
}

static void led_task(void *arg)
{
    (void)arg;
    bool blink = false;
    int tick = 0;

    while (true) {
        EventBits_t bits = app_state_event_group() != NULL ? xEventGroupGetBits(app_state_event_group()) : 0;
        bool wifi_ok = (bits & APP_STATE_WIFI_CONNECTED_BIT) != 0;
        bool internet_ok = (bits & APP_STATE_INTERNET_OK_BIT) != 0;
        bool cooldown = (bits & APP_STATE_COOLDOWN_BIT) != 0;
        bool rebooting = (bits & APP_STATE_ROUTER_REBOOTING_BIT) != 0;

        if ((tick % 2) == 0) {
            blink = !blink;
        }

        set_led(APP_STATUS_LED_WIFI_GPIO, wifi_ok || blink);
        set_led(APP_STATUS_LED_INTERNET_GPIO, internet_ok);

        if (rebooting) {
            set_led(APP_STATUS_LED_ALERT_GPIO, true);
        } else if (cooldown) {
            set_led(APP_STATUS_LED_ALERT_GPIO, blink);
        } else {
            set_led(APP_STATUS_LED_ALERT_GPIO, false);
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(LED_UPDATE_MS));
    }
}

esp_err_t status_leds_init(void)
{
    esp_err_t err = configure_led(APP_STATUS_LED_WIFI_GPIO, "WiFi");
    if (err != ESP_OK) {
        return err;
    }
    err = configure_led(APP_STATUS_LED_INTERNET_GPIO, "Internet");
    if (err != ESP_OK) {
        return err;
    }
    err = configure_led(APP_STATUS_LED_ALERT_GPIO, "Alerta");
    if (err != ESP_OK) {
        return err;
    }

    if (s_led_task == NULL) {
        BaseType_t created = xTaskCreate(led_task, "led_task", LED_TASK_STACK_SIZE, NULL, LED_TASK_PRIORITY, &s_led_task);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
