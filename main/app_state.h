#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* Bits de estado global del sistema (EventGroup compartido). */
#define APP_STATE_WIFI_CONNECTED_BIT BIT0
#define APP_STATE_INTERNET_OK_BIT BIT1
#define APP_STATE_COOLDOWN_BIT BIT2
#define APP_STATE_ROUTER_REBOOTING_BIT BIT3

/* Inicializa el EventGroup compartido. Idempotente. */
esp_err_t app_state_init(void);

/* Acceso directo al EventGroup para esperas (xEventGroupWaitBits). */
EventGroupHandle_t app_state_event_group(void);

/* Helpers de set/clear/get sobre los bits anteriores. */
void app_state_set_bits(EventBits_t bits);
void app_state_clear_bits(EventBits_t bits);
bool app_state_test_bits(EventBits_t bits);

#endif
