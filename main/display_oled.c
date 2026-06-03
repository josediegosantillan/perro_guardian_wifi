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

#define OLED_I2C_PORT        I2C_NUM_0
#define OLED_WIDTH           128
#define OLED_HEIGHT          64
#define OLED_PAGES           8
#define OLED_TASK_STACK_SIZE 4096
#define OLED_TASK_PRIORITY   2
#define OLED_UPDATE_MS       100
#define OLED_MARQUEE_STEP_PX 1
#define OLED_RETRY_MS        5000
#define OLED_DOG_INTERVAL_MS 30000
#define OLED_DOG_DURATION_MS 6000
#define OLED_DOG_FRAME_MS    500

static const char *TAG = "display_oled";
static TaskHandle_t s_oled_task;
static bool s_oled_ready;
static bool s_oled_disconnect_logged;
static uint8_t s_oled_address = APP_OLED_I2C_ADDRESS;
static int s_marquee_offset_px;

/* ── Framebuffer ────────────────────────────────────────────────────────── */

static uint8_t s_fb[OLED_PAGES][OLED_WIDTH];

/* ── Low-level I2C ──────────────────────────────────────────────────────── */

static esp_err_t oled_write(uint8_t control, const uint8_t *data, size_t len)
{
    uint8_t buf[17];
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 16) chunk = 16;
        buf[0] = control;
        memcpy(&buf[1], &data[off], chunk);
        esp_err_t e = i2c_master_write_to_device(
            OLED_I2C_PORT, s_oled_address, buf, chunk + 1, pdMS_TO_TICKS(100));
        if (e != ESP_OK) return e;
        off += chunk;
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
    esp_err_t err = oled_cmd((uint8_t)(0xB0 | (page & 0x07)));
    if (err != ESP_OK) return err;
    err = oled_cmd((uint8_t)(0x00 | (col & 0x0F)));
    if (err != ESP_OK) return err;
    err = oled_cmd((uint8_t)(0x10 | ((col >> 4) & 0x0F)));
    if (err != ESP_OK) return err;
    return ESP_OK;
}

static esp_err_t oled_clear_hw(void)
{
    uint8_t z[OLED_WIDTH] = {0};
    for (int p = 0; p < OLED_PAGES; p++) {
        esp_err_t err = oled_set_cursor(p, 0);
        if (err != ESP_OK) return err;
        err = oled_write(0x40, z, sizeof(z));
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* ── Framebuffer primitives ─────────────────────────────────────────────── */

static void fb_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

static void fb_pixel(int x, int y)
{
    if ((unsigned)x < OLED_WIDTH && (unsigned)y < OLED_HEIGHT)
        s_fb[y >> 3][x] |= (uint8_t)(1u << (y & 7));
}

static void fb_clear_pixel(int x, int y)
{
    if ((unsigned)x < OLED_WIDTH && (unsigned)y < OLED_HEIGHT)
        s_fb[y >> 3][x] &= ~(uint8_t)(1u << (y & 7));
}

static void fb_hline(int x0, int x1, int y)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) fb_pixel(x, y);
}

static void fb_vline(int x, int y0, int y1)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) fb_pixel(x, y);
}

static void fb_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        fb_pixel(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void fb_circle(int cx, int cy, int r)
{
    int x = 0, y = r, d = 3 - 2 * r;
    while (y >= x) {
        fb_pixel(cx+x,cy+y); fb_pixel(cx-x,cy+y);
        fb_pixel(cx+x,cy-y); fb_pixel(cx-x,cy-y);
        fb_pixel(cx+y,cy+x); fb_pixel(cx-y,cy+x);
        fb_pixel(cx+y,cy-x); fb_pixel(cx-y,cy-x);
        if (d > 0) { d += 4*(x-y)+10; y--; } else d += 4*x+6;
        x++;
    }
}

static void fb_fill_circle(int cx, int cy, int r)
{
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx+1)*(dx+1)+dy*dy <= r2) dx++;
        fb_hline(cx-dx, cx+dx, cy+dy);
    }
}

static void fb_fill_rect(int x, int y, int w, int h)
{
    for (int row = y; row < y+h; row++) fb_hline(x, x+w-1, row);
}

static void fb_rect(int x, int y, int w, int h)
{
    fb_hline(x, x+w-1, y); fb_hline(x, x+w-1, y+h-1);
    fb_vline(x, y, y+h-1); fb_vline(x+w-1, y, y+h-1);
}

static void fb_ellipse(int cx, int cy, int rx, int ry)
{
    long a2 = (long)rx*rx, b2 = (long)ry*ry;
    long x = 0, y = ry, sigma = 2*b2 + a2*(1 - 2*ry);
    while (b2*x <= a2*y) {
        fb_pixel(cx+(int)x,cy+(int)y); fb_pixel(cx-(int)x,cy+(int)y);
        fb_pixel(cx+(int)x,cy-(int)y); fb_pixel(cx-(int)x,cy-(int)y);
        if (sigma >= 0) { sigma += 4*a2*(1-y); y--; }
        sigma += b2*(4*x+6); x++;
    }
    x = rx; y = 0;
    sigma = 2*a2 + b2*(1 - 2*rx);
    while (a2*y <= b2*x) {
        fb_pixel(cx+(int)x,cy+(int)y); fb_pixel(cx-(int)x,cy+(int)y);
        fb_pixel(cx+(int)x,cy-(int)y); fb_pixel(cx-(int)x,cy-(int)y);
        if (sigma >= 0) { sigma += 4*b2*(1-x); x--; }
        sigma += a2*(4*y+6); y++;
    }
}

static void fb_fill_ellipse(int cx, int cy, int rx, int ry)
{
    long a2 = (long)rx*rx, b2 = (long)ry*ry;
    for (int dy = -ry; dy <= ry; dy++) {
        long num = a2*(b2-(long)dy*dy);
        if (num < 0) continue;
        int dx = 0;
        while ((long)(dx+1)*(dx+1)*b2 <= num) dx++;
        fb_hline(cx-dx, cx+dx, cy+dy);
    }
}

static void oled_mark_disconnected(esp_err_t err)
{
    s_oled_ready = false;
    if (!s_oled_disconnect_logged) {
        s_oled_disconnect_logged = true;
        ESP_LOGW(TAG, "OLED desconectada o sin respuesta I2C: %s. Reintentando cada %d ms",
            esp_err_to_name(err),
            OLED_RETRY_MS);
    }
}

static esp_err_t fb_flush(void)
{
    if (!s_oled_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int p = 0; p < OLED_PAGES; p++) {
        esp_err_t err = oled_set_cursor(p, 0);
        if (err != ESP_OK) {
            oled_mark_disconnected(err);
            return err;
        }
        err = oled_write(0x40, s_fb[p], OLED_WIDTH);
        if (err != ESP_OK) {
            oled_mark_disconnected(err);
            return err;
        }
    }
    return ESP_OK;
}

/* ── Font ───────────────────────────────────────────────────────────────── */

typedef struct { char c; uint8_t col[5]; } font_char_t;

static const font_char_t s_font[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x08,0x08,0x08,0x08,0x08}},
    {'_', {0x40,0x40,0x40,0x40,0x40}},
    {':', {0x00,0x36,0x36,0x00,0x00}},
    {'.', {0x00,0x60,0x60,0x00,0x00}},
    {'/', {0x20,0x10,0x08,0x04,0x02}},
    {'0', {0x3e,0x51,0x49,0x45,0x3e}},
    {'1', {0x00,0x42,0x7f,0x40,0x00}},
    {'2', {0x42,0x61,0x51,0x49,0x46}},
    {'3', {0x21,0x41,0x45,0x4b,0x31}},
    {'4', {0x18,0x14,0x12,0x7f,0x10}},
    {'5', {0x27,0x45,0x45,0x45,0x39}},
    {'6', {0x3c,0x4a,0x49,0x49,0x30}},
    {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}},
    {'9', {0x06,0x49,0x49,0x29,0x1e}},
    {'A', {0x7e,0x11,0x11,0x11,0x7e}},
    {'B', {0x7f,0x49,0x49,0x49,0x36}},
    {'C', {0x3e,0x41,0x41,0x41,0x22}},
    {'D', {0x7f,0x41,0x41,0x22,0x1c}},
    {'E', {0x7f,0x49,0x49,0x49,0x41}},
    {'F', {0x7f,0x09,0x09,0x09,0x01}},
    {'G', {0x3e,0x41,0x49,0x49,0x7a}},
    {'H', {0x7f,0x08,0x08,0x08,0x7f}},
    {'I', {0x00,0x41,0x7f,0x41,0x00}},
    {'J', {0x20,0x40,0x41,0x3f,0x01}},
    {'K', {0x7f,0x08,0x14,0x22,0x41}},
    {'L', {0x7f,0x40,0x40,0x40,0x40}},
    {'M', {0x7f,0x02,0x0c,0x02,0x7f}},
    {'N', {0x7f,0x04,0x08,0x10,0x7f}},
    {'O', {0x3e,0x41,0x41,0x41,0x3e}},
    {'P', {0x7f,0x09,0x09,0x09,0x06}},
    {'Q', {0x3e,0x41,0x51,0x21,0x5e}},
    {'R', {0x7f,0x09,0x19,0x29,0x46}},
    {'S', {0x46,0x49,0x49,0x49,0x31}},
    {'T', {0x01,0x01,0x7f,0x01,0x01}},
    {'U', {0x3f,0x40,0x40,0x40,0x3f}},
    {'V', {0x1f,0x20,0x40,0x20,0x1f}},
    {'W', {0x7f,0x20,0x18,0x20,0x7f}},
    {'X', {0x63,0x14,0x08,0x14,0x63}},
    {'Y', {0x07,0x08,0x70,0x08,0x07}},
    {'Z', {0x61,0x51,0x49,0x45,0x43}},
};

static const uint8_t *font_for_char(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(s_font)/sizeof(s_font[0]); i++) {
        if (s_font[i].c == c) return s_font[i].col;
    }
    return s_font[0].col;
}

static void fb_text(int x, int y, const char *text)
{
    while (*text && x + 5 < OLED_WIDTH) {
        const uint8_t *g = font_for_char(*text++);
        for (int col = 0; col < 5; col++) {
            for (int bit = 0; bit < 8; bit++) {
                if (g[col] & (1u << bit)) fb_pixel(x+col, y+bit);
            }
        }
        x += 6;
    }
}

static int fb_text_width(const char *text)
{
    return (int)strlen(text) * 6;
}

static void fb_text_scaled(int x, int y, const char *text, int xscale, int yscale)
{
    if (xscale < 1) xscale = 1;
    if (yscale < 1) yscale = 1;

    while (*text && x + (5 * xscale) < OLED_WIDTH) {
        const uint8_t *g = font_for_char(*text++);
        for (int col = 0; col < 5; col++) {
            for (int bit = 0; bit < 8; bit++) {
                if ((g[col] & (1u << bit)) == 0) continue;
                for (int dx = 0; dx < xscale; dx++) {
                    for (int dy = 0; dy < yscale; dy++) {
                        fb_pixel(x + col * xscale + dx, y + bit * yscale + dy);
                    }
                }
            }
        }
        x += 6 * xscale;
    }
}

static void fb_text_tall(int x, int y, const char *text)
{
    fb_text_scaled(x, y, text, 1, 2);
}

/* ── Dog face ───────────────────────────────────────────────────────────── */
/*
 * Front-facing German Shepherd on full 128×64 display.
 * frame 0: alert, mouth closed
 * frame 1: panting (open mouth + tongue)
 * frame 2: squinting (relaxed blink)
 * frame 3: same as 0 (loop reset)
 */
static void oled_draw_dog(int frame)
{
    bool panting = (frame == 1);
    bool squint  = (frame == 2);

    fb_clear();

    /* ── head ellipse (cx=64, cy=37, rx=28, ry=21) ──────────────── */
    fb_ellipse(64, 37, 28, 21);
    fb_ellipse(64, 37, 27, 20);   /* double outline for thickness */

    /* ── left ear ─────────────────────────────────────────────────── */
    fb_line(36, 25, 22, 5);     /* outer edge */
    fb_line(22, 5, 46, 14);     /* inner edge */
    fb_line(46, 14, 40, 23);    /* base */
    /* natural rounded tip: 3-pixel arc just above the convergence point */
    fb_pixel(21, 4); fb_pixel(22, 3); fb_pixel(23, 4);
    /* inner fur */
    fb_line(38, 23, 28, 8);
    fb_line(28, 8, 42, 16);

    /* ── right ear ────────────────────────────────────────────────── */
    fb_line(92, 25, 106, 5);    /* outer edge */
    fb_line(106, 5, 82, 14);    /* inner edge */
    fb_line(82, 14, 88, 23);    /* base */
    /* natural rounded tip */
    fb_pixel(105, 4); fb_pixel(106, 3); fb_pixel(107, 4);
    /* inner fur */
    fb_line(90, 23, 100, 8);
    fb_line(100, 8, 86, 16);

    /* ── eyebrows (alert, slightly raised) ───────────────────────── */
    fb_line(44, 21, 57, 20);
    fb_line(44, 22, 57, 21);
    fb_line(71, 20, 84, 21);
    fb_line(71, 21, 84, 22);

    /* ── eyes ─────────────────────────────────────────────────────── */
    if (squint) {
        fb_hline(45, 56, 29); fb_hline(45, 56, 30);
        fb_line(45, 30, 50, 33); fb_line(50, 33, 56, 30);
        fb_hline(72, 83, 29); fb_hline(72, 83, 30);
        fb_line(72, 30, 77, 33); fb_line(77, 33, 83, 30);
    } else {
        /* outer eye circles */
        fb_circle(50, 29, 6);
        fb_circle(78, 29, 6);
        /* pupils */
        fb_fill_circle(50, 30, 3);
        fb_fill_circle(78, 30, 3);
        /* catch-light (small bright dot clears a pixel from pupil) */
        fb_clear_pixel(48, 28);
        fb_clear_pixel(76, 28);
    }

    /* ── muzzle region ────────────────────────────────────────────── */
    fb_ellipse(64, 46, 17, 10);

    /* ── nose (filled ellipse) ────────────────────────────────────── */
    fb_fill_ellipse(64, 40, 9, 5);
    /* nose bridge highlight */
    fb_hline(60, 64, 37);
    fb_hline(60, 64, 38);

    /* ── mouth ────────────────────────────────────────────────────── */
    if (panting) {
        fb_line(54, 50, 59, 54);
        fb_line(59, 54, 64, 56);
        fb_line(64, 56, 69, 54);
        fb_line(69, 54, 74, 50);
        /* tongue */
        fb_fill_ellipse(64, 59, 7, 4);
        fb_vline(64, 52, 62);     /* center line */
    } else {
        fb_line(54, 50, 59, 53);
        fb_line(59, 53, 64, 52);
        fb_line(64, 52, 69, 53);
        fb_line(69, 53, 74, 50);
        fb_pixel(64, 53);         /* center divot */
    }

    /* ── neck / chest ─────────────────────────────────────────────── */
    fb_line(40, 56, 36, 63);
    fb_line(88, 56, 92, 63);
    fb_hline(36, 92, 63);

    /* ── fur texture dots ─────────────────────────────────────────── */
    fb_pixel(58, 18); fb_pixel(64, 16); fb_pixel(70, 18);
    fb_pixel(38, 35); fb_pixel(40, 39);
    fb_pixel(88, 35); fb_pixel(86, 39);

    fb_flush();
}

/* ── Status screen ──────────────────────────────────────────────────────── */

/* Phone-style signal bars at (x, y_bottom), rssi in dBm */
static void draw_signal_bars(int x, int y_bottom, int rssi)
{
    int filled = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int bh = (i+1)*2 + 1;     /* heights: 3, 5, 7, 9 */
        int bx = x + i*4;
        int by = y_bottom - bh + 1;
        if (i < filled) {
            fb_fill_rect(bx, by, 3, bh);
        } else {
            fb_rect(bx, by, 3, bh);
        }
    }
}

/* Tiny globe icon (internet indicator) at pixel (x,y) */
static void draw_inet_icon(int x, int y, bool ok)
{
    fb_circle(x+4, y+4, 4);
    fb_vline(x+4, y, y+8);
    fb_hline(x, x+8, y+4);
    fb_hline(x+1, x+7, y+2);
    fb_hline(x+1, x+7, y+6);
    if (!ok) {
        /* X slash over globe */
        fb_line(x+1, y+1, x+7, y+7);
        fb_line(x+7, y+1, x+1, y+7);
    }
}

static void oled_draw_status(bool wifi_ok, bool internet_ok, bool rebooting)
{
    char ip[16]   = {0};
    char ssid[33] = {0};
    int  rssi     = 0;
    wifi_manager_get_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);

    fb_clear();

    /* ── Row 0 (y=0-7): inverted title bar ───────────────────────── */
    char marquee[64] = {0};
    snprintf(marquee, sizeof(marquee), "PERRO_GUARDIAN_WIFI     IP:%s     ", ip[0] ? ip : "0.0.0.0");
    int marquee_width = fb_text_width(marquee);
    if (marquee_width <= 0) marquee_width = OLED_WIDTH;
    for (int x = -s_marquee_offset_px; x < OLED_WIDTH; x += marquee_width) {
        fb_text(x, 0, marquee);
    }
    s_marquee_offset_px = (s_marquee_offset_px + OLED_MARQUEE_STEP_PX) % marquee_width;

    /* ── Separator y=8 ───────────────────────────────────────────── */
    fb_hline(0, 127, 9);

    /* ── Row 1 (y=10): WiFi ──────────────────────────────────────── */
    if (wifi_ok) {
        char short_ssid[10] = {0};
        strncpy(short_ssid, ssid[0] ? ssid : "???", 9);
        char wline[20] = {0};
        snprintf(wline, sizeof(wline), "WIFI:%s", short_ssid);
        fb_text_tall(2, 12, wline);
        draw_signal_bars(110, 25, rssi);
    } else {
        fb_text_tall(2, 12, "WIFI:OFF");
        /* X bars */
        for (int i = 0; i < 4; i++) fb_rect(110+i*4, 18+(3-i)*2, 3, (i+1)*2+1);
    }

    /* ── Separator y=19 ──────────────────────────────────────────── */
    fb_hline(0, 127, 28);

    /* ── Row 2 (y=21): Internet ──────────────────────────────────── */
    draw_inet_icon(114, 34, internet_ok);
    fb_text_tall(2, 31, internet_ok ? "INTERNET:OK" : "INTERNET:FAIL");

    /* ── Separator y=31 ──────────────────────────────────────────── */
    fb_hline(0, 127, 47);

    /* ── Row 3 (y=33): Modem / Mode ──────────────────────────────── */
    if (rebooting) {
        fb_text_tall(2, 48, "REINICIO");
    } else {
        fb_text_tall(2, 48, "MODEM:OK");
        fb_text(82, 54, APP_TEST_MODE ? "TEST" : "LIVE");
    }

    /* ── Separator y=43 ──────────────────────────────────────────── */
    /* La IP queda chica para que entren todas las lineas principales. */

    /* ── Row 4 (y=45): IP address ────────────────────────────────── */
    /* ── Separator y=55 ──────────────────────────────────────────── */

    /* ── Side borders ────────────────────────────────────────────── */
    fb_vline(0,   9, 63);
    fb_vline(127, 9, 63);

    fb_flush();
}

/* ── Hardware init ──────────────────────────────────────────────────────── */

static esp_err_t oled_init_at_address(uint8_t address)
{
    s_oled_address = address;
    const uint8_t cmds[] = {
        0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,
        0xAD,0x8B,0xA1,0xC8,0xDA,0x12,0x81,0x7F,
        0xD9,0x22,0xDB,0x35,0xA4,0xA6,0xAF,
    };
    for (size_t i = 0; i < sizeof(cmds); i++) {
        esp_err_t err = oled_cmd(cmds[i]);
        if (err != ESP_OK) return err;
    }
    return oled_clear_hw();
}

static esp_err_t oled_hw_init(void)
{
    i2c_config_t cfg = {
        .mode           = I2C_MODE_MASTER,
        .sda_io_num     = APP_OLED_SDA_GPIO,
        .scl_io_num     = APP_OLED_SCL_GPIO,
        .sda_pullup_en  = GPIO_PULLUP_ENABLE,
        .scl_pullup_en  = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(OLED_I2C_PORT, &cfg),           TAG, "i2c_param");
    ESP_RETURN_ON_ERROR(i2c_driver_install(OLED_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG, "i2c_install");

    esp_err_t err = oled_init_at_address(APP_OLED_I2C_ADDRESS);
    if (err == ESP_OK) return ESP_OK;

    uint8_t fallback = APP_OLED_I2C_ADDRESS == 0x3C ? 0x3D : 0x3C;
    ESP_LOGW(TAG, "OLED no respondio en 0x%02X, probando 0x%02X", APP_OLED_I2C_ADDRESS, fallback);
    return oled_init_at_address(fallback);
}

static esp_err_t oled_recover(void)
{
    esp_err_t err = oled_init_at_address(s_oled_address);
    if (err != ESP_OK) {
        err = oled_init_at_address(APP_OLED_I2C_ADDRESS);
    }
    if (err != ESP_OK) {
        uint8_t fallback = APP_OLED_I2C_ADDRESS == 0x3C ? 0x3D : 0x3C;
        err = oled_init_at_address(fallback);
    }

    if (err == ESP_OK) {
        s_oled_ready = true;
        s_oled_disconnect_logged = false;
        ESP_LOGI(TAG, "OLED recuperada en addr 0x%02X", s_oled_address);
    }
    return err;
}

/* ── OLED task ──────────────────────────────────────────────────────────── */

static void oled_task(void *arg)
{
    (void)arg;
    TickType_t last_dog = xTaskGetTickCount();
    TickType_t next_oled_retry = 0;
    int dog_frame = 0;

    while (true) {
        TickType_t now = xTaskGetTickCount();

        if (!s_oled_ready) {
            if (now >= next_oled_retry) {
                oled_recover();
                next_oled_retry = xTaskGetTickCount() + pdMS_TO_TICKS(OLED_RETRY_MS);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if ((now - last_dog) >= pdMS_TO_TICKS(OLED_DOG_INTERVAL_MS)) {
            TickType_t dog_until = now + pdMS_TO_TICKS(OLED_DOG_DURATION_MS);
            dog_frame = 0;
            while (s_oled_ready && xTaskGetTickCount() < dog_until) {
                oled_draw_dog(dog_frame & 3);
                dog_frame++;
                vTaskDelay(pdMS_TO_TICKS(OLED_DOG_FRAME_MS));
            }
            if (s_oled_ready) {
                esp_err_t err = oled_clear_hw();
                if (err != ESP_OK) {
                    oled_mark_disconnected(err);
                }
            }
            last_dog = xTaskGetTickCount();
        }

        EventBits_t bits = app_state_event_group() ?
            xEventGroupGetBits(app_state_event_group()) : 0;
        bool wifi_ok     = (bits & APP_STATE_WIFI_CONNECTED_BIT)  != 0;
        bool internet_ok = (bits & APP_STATE_INTERNET_OK_BIT)     != 0;
        bool rebooting   = (bits & APP_STATE_ROUTER_REBOOTING_BIT)!= 0;

        oled_draw_status(wifi_ok, internet_ok, rebooting);
        vTaskDelay(pdMS_TO_TICKS(OLED_UPDATE_MS));
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t display_oled_init(void)
{
    if (!APP_OLED_ENABLED) return ESP_OK;

    esp_err_t err = oled_hw_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
            "OLED no disponible en 0x%02X/0x%02X: %s. Continuando sin pantalla.",
            APP_OLED_I2C_ADDRESS,
            APP_OLED_I2C_ADDRESS == 0x3C ? 0x3D : 0x3C,
            esp_err_to_name(err));
        i2c_driver_delete(OLED_I2C_PORT);
        return ESP_OK;
    }

    s_oled_ready = true;
    ESP_LOGI(TAG, "OLED lista: SDA GPIO%d, SCL GPIO%d, addr 0x%02X",
        APP_OLED_SDA_GPIO, APP_OLED_SCL_GPIO, s_oled_address);

    if (!s_oled_task) {
        BaseType_t ok = xTaskCreate(
            oled_task, "oled_task",
            OLED_TASK_STACK_SIZE, NULL,
            OLED_TASK_PRIORITY, &s_oled_task);
        if (ok != pdPASS) return ESP_ERR_NO_MEM;
    }

    (void)s_oled_ready;
    return ESP_OK;
}
