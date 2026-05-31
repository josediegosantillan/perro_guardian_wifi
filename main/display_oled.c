#include "display_oled.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_state.h"
#include "wifi_manager.h"

#define OLED_I2C_PORT I2C_NUM_0
#define OLED_WIDTH 128
#define OLED_PAGES 8
#define OLED_TASK_STACK_SIZE 3072
#define OLED_TASK_PRIORITY 2
#define OLED_UPDATE_MS 1000
#define OLED_DOG_INTERVAL_MS 30000
#define OLED_DOG_DURATION_MS 5000

static const char *TAG = "display_oled";
static TaskHandle_t s_oled_task;
static bool s_oled_ready;
static uint8_t s_oled_address = APP_OLED_I2C_ADDRESS;

typedef struct {
    char c;
    uint8_t col[5];
} font_char_t;

static const font_char_t font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'_', {0x40, 0x40, 0x40, 0x40, 0x40}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}},
    {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
    {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
    {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
    {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
    {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
    {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
    {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
    {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
    {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
    {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3e, 0x41, 0x51, 0x21, 0x5e}},
    {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
    {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
    {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
    {'W', {0x7f, 0x20, 0x18, 0x20, 0x7f}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static const uint8_t *font_for_char(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }

    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++) {
        if (font[i].c == c) {
            return font[i].col;
        }
    }

    return font[0].col;
}

static esp_err_t oled_write(uint8_t control, const uint8_t *data, size_t len)
{
    uint8_t buffer[17] = {0};
    size_t offset = 0;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > sizeof(buffer) - 1) {
            chunk = sizeof(buffer) - 1;
        }

        buffer[0] = control;
        memcpy(&buffer[1], &data[offset], chunk);
        esp_err_t err = i2c_master_write_to_device(
            OLED_I2C_PORT, s_oled_address, buffer, chunk + 1, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return err;
        }

        offset += chunk;
    }

    return ESP_OK;
}

static esp_err_t oled_cmd(uint8_t cmd)
{
    return oled_write(0x00, &cmd, 1);
}

static esp_err_t oled_set_cursor(int page, int col)
{
    col += APP_OLED_COLUMN_OFFSET;
    ESP_RETURN_ON_ERROR(oled_cmd((uint8_t)(0xB0 | (page & 0x07))), TAG, "set page");
    ESP_RETURN_ON_ERROR(oled_cmd((uint8_t)(0x00 | (col & 0x0F))), TAG, "set col low");
    ESP_RETURN_ON_ERROR(oled_cmd((uint8_t)(0x10 | ((col >> 4) & 0x0F))), TAG, "set col high");
    return ESP_OK;
}

static esp_err_t oled_clear(void)
{
    uint8_t zeros[OLED_WIDTH] = {0};
    for (int page = 0; page < OLED_PAGES; page++) {
        ESP_RETURN_ON_ERROR(oled_set_cursor(page, 0), TAG, "cursor clear");
        ESP_RETURN_ON_ERROR(oled_write(0x40, zeros, sizeof(zeros)), TAG, "clear page");
    }

    return ESP_OK;
}

static void oled_write_line(int page, const char *text)
{
    uint8_t line[OLED_WIDTH] = {0};
    int col = 0;

    while (*text != '\0' && col + 6 <= OLED_WIDTH) {
        const uint8_t *glyph = font_for_char(*text);
        for (int i = 0; i < 5; i++) {
            line[col++] = glyph[i];
        }
        line[col++] = 0x00;
        text++;
    }

    if (oled_set_cursor(page, 0) == ESP_OK) {
        oled_write(0x40, line, sizeof(line));
    }
}

static void oled_write_separator(int page)
{
    uint8_t line[OLED_WIDTH] = {0};
    for (int i = 0; i < OLED_WIDTH; i += 2) {
        line[i] = 0x08;
    }

    if (oled_set_cursor(page, 0) == ESP_OK) {
        oled_write(0x40, line, sizeof(line));
    }
}

static void oled_write_centered(int page, const char *text)
{
    size_t len = strlen(text);
    int text_width = (int)len * 6;
    int start_col = text_width < OLED_WIDTH ? (OLED_WIDTH - text_width) / 2 : 0;
    uint8_t line[OLED_WIDTH] = {0};
    int col = start_col;

    while (*text != '\0' && col + 6 <= OLED_WIDTH) {
        const uint8_t *glyph = font_for_char(*text);
        for (int i = 0; i < 5; i++) {
            line[col++] = glyph[i];
        }
        line[col++] = 0x00;
        text++;
    }

    if (oled_set_cursor(page, 0) == ESP_OK) {
        oled_write(0x40, line, sizeof(line));
    }
}

static void oled_write_status_screen(bool wifi_ok, bool internet_ok, bool rebooting)
{
    char ip[16] = {0};
    char ssid[33] = {0};
    int rssi = 0;
    wifi_manager_get_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);

    oled_write_line(0, rebooting ? "MODEM: RESET" : "MODEM: ON");
    oled_write_separator(1);
    oled_write_line(2, wifi_ok ? "WIFI: OK" : "WIFI: FAIL");
    oled_write_separator(3);
    oled_write_line(4, internet_ok ? "INTERNET: OK" : "INTERNET: FAIL");
    oled_write_separator(5);
    oled_write_line(6, APP_TEST_MODE ? "MODO: TEST" : "MODO: LIVE");

    char ip_line[22] = {0};
    snprintf(ip_line, sizeof(ip_line), "IP: %s", ip[0] != '\0' ? ip : "0.0.0.0");
    oled_write_line(7, ip_line);
}

static void oled_write_dog_frame(bool tail_up)
{
    oled_clear();
    oled_write_centered(0, "PERRO GUARDIAN");
    oled_write_centered(1, "VIGILANDO WIFI");

    if (tail_up) {
        oled_write_centered(3, "     /");
        oled_write_centered(4, " ___/___");
    } else {
        oled_write_centered(3, "      \\");
        oled_write_centered(4, " ___   \\");
    }

    oled_write_centered(5, "/ O  O \\___");
    oled_write_centered(6, "\\__^__/   )");
    oled_write_centered(7, "  U  U---U");
}

static esp_err_t oled_init_at_address(uint8_t address)
{
    s_oled_address = address;

    const uint8_t init_cmds[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0xAD, 0x8B, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F,
        0xD9, 0x22, 0xDB, 0x35, 0xA4, 0xA6, 0xAF,
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        ESP_RETURN_ON_ERROR(oled_cmd(init_cmds[i]), TAG, "oled init");
    }

    return oled_clear();
}

static esp_err_t oled_hw_init(void)
{
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = APP_OLED_SDA_GPIO,
        .scl_io_num = APP_OLED_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(OLED_I2C_PORT, &i2c_config), TAG, "i2c_param_config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(OLED_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG, "i2c_driver_install");

    esp_err_t err = oled_init_at_address(APP_OLED_I2C_ADDRESS);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    uint8_t fallback = APP_OLED_I2C_ADDRESS == 0x3C ? 0x3D : 0x3C;
    ESP_LOGW(TAG, "OLED no respondio en 0x%02X, probando 0x%02X", APP_OLED_I2C_ADDRESS, fallback);
    return oled_init_at_address(fallback);
}

static void oled_task(void *arg)
{
    (void)arg;
    TickType_t last_dog = xTaskGetTickCount();
    bool dog_tail_up = false;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_dog) >= pdMS_TO_TICKS(OLED_DOG_INTERVAL_MS)) {
            TickType_t dog_until = now + pdMS_TO_TICKS(OLED_DOG_DURATION_MS);
            while (xTaskGetTickCount() < dog_until) {
                oled_write_dog_frame(dog_tail_up);
                dog_tail_up = !dog_tail_up;
                vTaskDelay(pdMS_TO_TICKS(450));
            }
            oled_clear();
            last_dog = xTaskGetTickCount();
        }

        EventBits_t bits = app_state_event_group() != NULL ? xEventGroupGetBits(app_state_event_group()) : 0;
        bool wifi_ok = (bits & APP_STATE_WIFI_CONNECTED_BIT) != 0;
        bool internet_ok = (bits & APP_STATE_INTERNET_OK_BIT) != 0;
        bool rebooting = (bits & APP_STATE_ROUTER_REBOOTING_BIT) != 0;

        oled_write_status_screen(wifi_ok, internet_ok, rebooting);

        vTaskDelay(pdMS_TO_TICKS(OLED_UPDATE_MS));
    }
}

esp_err_t display_oled_init(void)
{
    if (!APP_OLED_ENABLED) {
        return ESP_OK;
    }

    esp_err_t err = oled_hw_init();
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "OLED no disponible en 0x%02X/0x%02X: %s. Se continua sin pantalla",
            APP_OLED_I2C_ADDRESS,
            APP_OLED_I2C_ADDRESS == 0x3C ? 0x3D : 0x3C,
            esp_err_to_name(err));
        i2c_driver_delete(OLED_I2C_PORT);
        return ESP_OK;
    }

    s_oled_ready = true;
    ESP_LOGI(
        TAG,
        "OLED lista: SDA GPIO%d, SCL GPIO%d, addr 0x%02X",
        APP_OLED_SDA_GPIO,
        APP_OLED_SCL_GPIO,
        s_oled_address);

    if (s_oled_task == NULL) {
        BaseType_t created = xTaskCreate(oled_task, "oled_task", OLED_TASK_STACK_SIZE, NULL, OLED_TASK_PRIORITY, &s_oled_task);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    (void)s_oled_ready;
    return ESP_OK;
}
