#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include <stdbool.h>

#include "esp_err.h"

/*
 * Configura el GPIO del rele en estado seguro (router encendido), crea la cola de
 * comandos y lanza la relay_task. La relay_task es la unica duenia del GPIO del rele.
 */
esp_err_t relay_control_init(void);

/*
 * Encola un power-cycle del router: corte de alimentacion durante off_time_ms y luego
 * espera boot_wait_ms para que el router arranque. La relay_task mantiene el bit
 * APP_STATE_ROUTER_REBOOTING_BIT activo durante toda la secuencia.
 *
 * En TEST_MODE la secuencia se simula (mismos tiempos, sin tocar el GPIO).
 * Devuelve ESP_ERR_INVALID_STATE si la cola no esta inicializada, o ESP_FAIL si esta llena.
 */
esp_err_t relay_control_request_power_cycle(int off_time_ms, int boot_wait_ms);

/* true si hay un power-cycle en curso (bit APP_STATE_ROUTER_REBOOTING_BIT activo). */
bool relay_control_is_rebooting(void);

#endif
