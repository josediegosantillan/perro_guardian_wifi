# AGENTS.md

## Rol del agente

Actuar como desarrollador senior de firmware embebido especializado en ESP32 DevKit clasico, ESP-IDF v5.x, FreeRTOS, redes WiFi, sistemas IoT robustos y diseno fail-safe.

## Plataforma objetivo

* Microcontrolador: ESP32 DevKit estandar.
* Framework: ESP-IDF v5.x.
* Lenguaje: C.
* Editor: VS Code con extension ESP-IDF.
* No usar Arduino.
* Target ESP-IDF: esp32.

## Pinout por defecto ESP32 DevKit

* Rele router: GPIO26.
* Boton reset manual del router: GPIO32, pulsador a GND con pull-up interno.
* Boton portal WiFi: GPIO33, pulsador a GND con pull-up interno.
* LED WiFi: GPIO16.
* LED Internet: GPIO17.
* LED alerta/reinicio/cooldown: GPIO27.
* OLED I2C 1.3 pulgadas:
  * SDA: GPIO21.
  * SCL: GPIO22.
  * Direccion I2C 7-bit: 0x3C por defecto.
  * Si la placa indica 0x78/0x7A, eso normalmente equivale a 0x3C/0x3D en 7 bits.

## Reglas importantes para ESP32 DevKit

* No usar GPIO6 a GPIO11: estan conectados a la flash SPI.
* No usar GPIO34 a GPIO39 como salida: son solo entrada.
* Evitar GPIO0, GPIO2, GPIO4, GPIO5, GPIO12 y GPIO15 para el rele porque son pines de strapping.
* Todo pin critico debe quedar en estado seguro al arrancar.
* El rele debe usar un GPIO configurable y una etapa de manejo adecuada; no alimentar bobinas directo desde el GPIO.

## Objetivo general del proyecto

Desarrollar firmware profesional para un ESP32 DevKit que funcione como perro guardian de WiFi/router.

El dispositivo debe supervisar la conexion WiFi e internet. Si detecta fallos persistentes, debe reiniciar fisicamente el router cortando durante unos segundos la alimentacion positiva de 12 V DC mediante un rele conectado en NC.

## Seguridad electrica

* Cortar solamente la linea positiva de 12 V DC del router.
* Nunca cortar 220 V AC en este proyecto.
* Usar el contacto NC del rele para que el router permanezca encendido si el ESP32 falla, se reinicia o queda sin alimentacion.
* Asumir modulo rele compatible con 3,3 V o etapa de manejo con transistor/MOSFET, resistencia de base/gate y diodo flyback si corresponde.
* La logica debe ser fail-safe.

## Arquitectura de software requerida

Separar el codigo en modulos:

* main.c
* app_config.h
* app_state.c / app_state.h
* button_control.c / button_control.h
* display_oled.c / display_oled.h
* status_leds.c / status_leds.h
* wifi_credentials.c / wifi_credentials.h
* wifi_config_portal.c / wifi_config_portal.h
* wifi_manager.c / wifi_manager.h
* internet_check.c / internet_check.h
* relay_control.c / relay_control.h
* router_watchdog.c / router_watchdog.h

Usar:

* FreeRTOS.
* EventGroups.
* Colas para comandos al rele.
* esp_http_client para pruebas HTTP.
* esp_wifi y eventos WiFi/IP.
* Logs con ESP_LOGI, ESP_LOGW y ESP_LOGE.
* Manejo correcto de errores con esp_err_t.
* Sin delays largos bloqueantes.
* Sin busy loops.
* Codigo preparado para funcionar 24/7.

## Estados mediante EventGroup

Usar bits de estado:

* WIFI_CONNECTED_BIT
* INTERNET_OK_BIT
* COOLDOWN_BIT
* ROUTER_REBOOTING_BIT

## Comportamiento requerido

1. Al iniciar:

   * Inicializar NVS.
   * Configurar GPIO del rele en estado seguro.
   * Inicializar LEDs, botones y OLED si estan habilitados.
   * Inicializar WiFi STA.
   * Entrar al portal si no hay credenciales, si se fuerza por boton, o si hay fallo de autenticacion.
   * Inicializar tareas FreeRTOS.
   * Inicializar logs.

2. Supervision:

   * Verificar conexion cada 30 segundos.
   * Primero comprobar conexion WiFi local.
   * Si no hay WiFi local, contar fallo.
   * Si hay WiFi local, verificar internet real con HTTP GET.
   * Usar dos endpoints de prueba para evitar falsos positivos.
   * Usar timeout HTTP de 5 segundos.
   * Considerar internet OK con HTTP 204 por defecto.

3. Reinicio del router:

   * Si hay 3 fallos consecutivos, enviar comando a relay_task.
   * Activar rele durante 10 segundos.
   * Al activar el rele se abre el contacto NC y se corta la alimentacion del router.
   * Desactivar rele para devolver alimentacion.
   * Esperar 3 minutos de gracia antes de permitir otro reinicio.

4. Proteccion contra ciclos infinitos:

   * Maximo 3 reinicios por hora.
   * Si se supera el limite, entrar en cooldown de 30 minutos.
   * Durante cooldown seguir registrando estado, pero no reiniciar router.

5. Botones:

   * Boton reset manual: mantener 1 segundo para solicitar power-cycle del router.
   * Boton portal: mantener 3 segundos durante funcionamiento para reiniciar y entrar al portal.
   * Boton portal presionado durante arranque: entra directo al portal.

6. Indicadores:

   * LED WiFi: encendido si hay WiFi, parpadea si no hay conexion.
   * LED Internet: encendido si internet responde OK.
   * LED alerta: encendido durante reinicio del router, parpadea en cooldown.
   * OLED: muestra estado WiFi, internet, router y modo TEST/LIVE/COOLDOWN.

## Portal WiFi actual

El firmware incluye un portal basico para cargar credenciales:

* Si no hay credenciales guardadas en NVS, entra en modo portal.
* Si falla autenticacion WiFi, entra en modo portal.
* Si se presiona el boton de portal al arrancar, entra en modo portal.
* SSID del portal: PERRO_GUARDIAN_WIFI.
* Password del portal: 12345678.
* URL manual: http://192.168.4.1.
* El formulario guarda SSID/password en NVS y reinicia el ESP32.
* El portal permite borrar credenciales guardadas.
* Esta version no implementa DNS cautivo automatico; abrir la URL manualmente.

## Modo prueba

Implementar TEST_MODE.

Si TEST_MODE esta activo:

* No accionar fisicamente el rele.
* Mostrar logs indicando que se simularia el reinicio del router.
* Mantener toda la logica de fallos, cooldown y contadores.

## Entregables esperados

Cuando se pida implementar una tarea, entregar:

* Estructura completa de carpetas.
* Codigo completo de cada archivo modificado o creado.
* CMakeLists.txt raiz y CMakeLists.txt de main.
* Explicacion breve de arquitectura.
* Diagrama textual de conexion.
* Pasos para compilar, configurar target esp32, flashear y monitorear.
* Prueba en modo TEST_MODE antes de conectar el router real.
