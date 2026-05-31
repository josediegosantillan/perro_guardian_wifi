# Prompt para Google AI Studio: Web servida por ESP32 + Telegram

Quiero que generes la especificacion y el codigo base para una pagina web embebida en un ESP32 llamado `PERRO_GUARDIAN_WIFI`.

La pagina debe ser servida directamente por el ESP32 mediante HTTP. No quiero una app Android, no quiero un HTML guardado en el celular y no quiero backend externo. El usuario debe entrar desde el navegador del celular o PC a la IP del ESP32.

Tambien quiero soporte opcional de Telegram para alertas y comandos remotos autorizados.

## Objetivo

Crear una web local servida por el ESP32 para:

* Ver estado del watchdog.
* Ver estado WiFi e internet.
* Configurar credenciales WiFi.
* Configurar IP fija o DHCP.
* Configurar parametros del watchdog.
* Ejecutar acciones manuales.
* Ver logs/eventos.
* Configurar Telegram.

La web debe funcionar en:

```text
Modo portal: http://192.168.4.1
Modo normal: http://IP_DEL_ESP32
```

Portal WiFi actual:

```text
SSID SoftAP: PERRO_GUARDIAN_WIFI
Password SoftAP: 12345678
URL manual: http://192.168.4.1
```

## Contexto del firmware

El firmware es ESP-IDF v5.x en C para ESP32 DevKit clasico.

El ESP32 supervisa WiFi e internet. Si detecta fallos persistentes, reinicia fisicamente el router cortando unos segundos la linea positiva de 12 V DC mediante un rele conectado en contacto NC.

El firmware actual ya tiene:

* WiFi STA.
* Portal WiFi manual por SoftAP.
* Credenciales guardadas en NVS.
* Boton fisico para reset manual del router.
* Boton fisico para forzar portal WiFi.
* LEDs de estado.
* OLED I2C.
* Modo TEST_MODE.
* Contadores.
* Cooldown para evitar ciclos infinitos.

## Requisitos de la web embebida

La web debe ser simple, liviana y apta para servir desde `esp_http_server`.

Preferencias:

* Un solo HTML embebido como string C o archivo compilado en firmware.
* CSS y JavaScript incluidos en la misma pagina.
* Sin CDN.
* Sin frameworks.
* Sin imagenes externas.
* Sin dependencias de internet.
* Mobile-first.
* Debe funcionar en celulares de 360 px de ancho.

## Secciones de la web

Usar tabs o botones superiores:

1. Dashboard.
2. Acciones.
3. Configuracion.
4. WiFi.
5. IP.
6. Telegram.
7. Logs.

## Dashboard

Debe mostrar:

* WiFi conectado/desconectado.
* SSID.
* IP del ESP32.
* RSSI.
* Internet OK/fallo.
* Router reiniciando/no.
* Rele activo/inactivo.
* Cooldown activo/no.
* TEST_MODE activo/no.
* Uptime.
* Fallos consecutivos.
* Reinicios totales.
* Hits de limite/cooldown.
* Ultimo motivo.

Debe refrescar estado cada 3 segundos llamando:

```http
GET /api/status
```

Respuesta esperada:

```json
{
  "wifiConnected": true,
  "ssid": "MiRouter",
  "ip": "192.168.1.80",
  "rssiDbm": -61,
  "internetOk": true,
  "cooldown": false,
  "routerRebooting": false,
  "relayActive": false,
  "testMode": true,
  "uptimeSec": 4312,
  "consecutiveFailures": 0,
  "totalReboots": 2,
  "limitHits": 0,
  "lastReason": "internet_ok",
  "portalAvailable": false
}
```

## Acciones

Botones:

* Reiniciar router.
* Forzar portal WiFi.
* Borrar credenciales WiFi.
* Reiniciar ESP32.
* Limpiar contadores.

Todos los comandos usan:

```http
POST /api/command
Content-Type: application/json
```

Ejemplos:

```json
{"cmd":"router_power_cycle"}
```

```json
{"cmd":"force_wifi_portal"}
```

```json
{"cmd":"erase_wifi_credentials"}
```

```json
{"cmd":"esp_restart"}
```

```json
{"cmd":"clear_stats"}
```

Respuesta:

```json
{
  "ok": true,
  "cmd": "router_power_cycle",
  "message": "Comando aceptado"
}
```

Cada accion peligrosa debe pedir confirmacion.

## Configuracion watchdog

Leer:

```http
GET /api/config
```

Guardar:

```http
POST /api/config
Content-Type: application/json
```

Campos:

```json
{
  "wifiConnectTimeoutMs": 20000,
  "watchdogCheckIntervalMs": 30000,
  "internetCheckTimeoutMs": 5000,
  "maxConsecutiveFailures": 3,
  "routerPowerOffMs": 10000,
  "routerBootWaitMs": 180000,
  "powerCycleMaxPerWindow": 3,
  "powerCycleWindowMs": 3600000,
  "cooldownAfterLimitMs": 1800000,
  "strictHttp204": true,
  "testMode": true,
  "internetCheckUrl1": "http://clients3.google.com/generate_204",
  "internetCheckUrl2": "http://cp.cloudflare.com/generate_204"
}
```

Validar rangos:

| Campo | Min | Max | Default |
| --- | ---: | ---: | ---: |
| wifiConnectTimeoutMs | 1000 | 300000 | 20000 |
| watchdogCheckIntervalMs | 1000 | 3600000 | 30000 |
| internetCheckTimeoutMs | 500 | 60000 | 5000 |
| maxConsecutiveFailures | 1 | 100 | 3 |
| routerPowerOffMs | 1000 | 300000 | 10000 |
| routerBootWaitMs | 1000 | 1800000 | 180000 |
| powerCycleMaxPerWindow | 1 | 100 | 3 |
| powerCycleWindowMs | 60000 | 86400000 | 3600000 |
| cooldownAfterLimitMs | 60000 | 86400000 | 1800000 |

## WiFi

Debe permitir:

* Escanear redes.
* Elegir SSID.
* Escribir password.
* Guardar credenciales.
* Borrar credenciales.

Escanear:

```http
GET /api/wifi/scan
```

Respuesta:

```json
{
  "networks": [
    {"ssid":"MiRouter","rssi":-55,"auth":"WPA2"},
    {"ssid":"Vecino","rssi":-78,"auth":"WPA2"}
  ]
}
```

Guardar credenciales:

```http
POST /api/wifi/credentials
Content-Type: application/json
```

```json
{
  "ssid": "MiRouter",
  "password": "password_wifi"
}
```

Respuesta:

```json
{
  "ok": true,
  "message": "Credenciales guardadas. El ESP32 se reiniciara."
}
```

Advertir:

```text
Al guardar credenciales, el ESP32 puede reiniciarse y cambiar de IP.
```

## IP fija o DHCP

Debe permitir configurar:

* DHCP automatico.
* IP fija.
* Gateway.
* Mascara.
* DNS.

Endpoint:

```http
GET /api/ip-config
POST /api/ip-config
```

JSON:

```json
{
  "staticIpEnabled": false,
  "staticIp": "192.168.1.50",
  "gateway": "192.168.1.1",
  "netmask": "255.255.255.0",
  "dns": "8.8.8.8"
}
```

Validar IPv4 simple.

Advertir:

```text
Si configuras una IP fija que ya usa otro equipo, puede haber conflicto de red.
```

## Telegram

Telegram es opcional y requiere internet.

La web debe mostrar esta advertencia:

```text
Telegram requiere que el ESP32 tenga internet. Si el router esta sin internet, Telegram no funcionara.
```

El ESP32 podria usar un bot de Telegram para:

* Enviar alerta cuando falla internet.
* Enviar alerta cuando reinicia el router.
* Enviar alerta cuando entra en cooldown.
* Consultar estado por mensaje.
* Ejecutar comandos autorizados.

Leer:

```http
GET /api/telegram/config
```

Respuesta:

```json
{
  "enabled": false,
  "botTokenConfigured": false,
  "chatId": "",
  "notifyOnInternetFail": true,
  "notifyOnRouterReset": true,
  "notifyOnCooldown": true,
  "allowCommands": false
}
```

Guardar:

```http
POST /api/telegram/config
Content-Type: application/json
```

Body:

```json
{
  "enabled": true,
  "botToken": "123456789:AA...",
  "chatId": "123456789",
  "notifyOnInternetFail": true,
  "notifyOnRouterReset": true,
  "notifyOnCooldown": true,
  "allowCommands": false
}
```

Probar:

```http
POST /api/telegram/test
Content-Type: application/json
```

```json
{
  "message": "Prueba desde PERRO_GUARDIAN_WIFI"
}
```

Comandos sugeridos para el bot:

```text
/estado
/ip
/wifi
/internet
/reset_router
/portal
/cooldown
/ayuda
```

Seguridad:

* `allowCommands` debe venir apagado por defecto.
* Si se activa, aceptar comandos solo del `chatId` configurado.
* No imprimir el token completo en la web despues de guardarlo.
* Mostrar solo `botTokenConfigured: true/false`.

## Logs

Endpoint:

```http
GET /api/logs
```

Respuesta:

```json
{
  "logs": [
    {
      "timeSec": 4312,
      "level": "WARN",
      "tag": "wifi_manager",
      "message": "WiFi desconectado"
    }
  ]
}
```

La web debe:

* Mostrar hora/uptime.
* Nivel.
* Tag.
* Mensaje.
* Colorear WARN/ERROR.
* Tener boton actualizar.
* Tener boton limpiar vista local.

## Estilo visual

Debe ser una herramienta tecnica limpia:

* Mobile-first.
* Fondo claro.
* Tarjetas compactas.
* Botones grandes.
* Estados con colores:
  * Verde: OK.
  * Rojo: fallo.
  * Amarillo/naranja: advertencia/reinicio.
  * Azul: informacion.
* No hacer landing page.
* No usar imagenes externas.
* No usar iconos externos.

## Seguridad web local

Implementar una proteccion simple para acciones criticas:

* PIN local opcional guardado en NVS.
* Si no hay PIN configurado, permitir solo configuracion inicial desde portal.
* Para acciones peligrosas pedir PIN en la web.

Acciones peligrosas:

* Reiniciar router.
* Borrar credenciales WiFi.
* Reiniciar ESP32.
* Habilitar comandos Telegram.
* Cambiar IP fija.

## Entregables esperados

Generar:

* HTML/CSS/JS embebible.
* Endpoints HTTP esperados.
* Esqueleto C para ESP-IDF con `esp_http_server`.
* Estructura sugerida de modulo:

```text
web_server.c
web_server.h
telegram_bot.c
telegram_bot.h
app_settings.c
app_settings.h
```

No usar Arduino.
No usar librerias externas innecesarias.

## Criterios de aceptacion

La solucion debe permitir:

1. Entrar por `http://192.168.4.1` en modo portal.
2. Entrar por `http://IP_DEL_ESP32` en modo normal.
3. Ver dashboard.
4. Guardar WiFi.
5. Configurar IP fija/DHCP.
6. Cambiar parametros del watchdog.
7. Ejecutar acciones manuales con confirmacion.
8. Configurar Telegram.
9. Enviar mensaje de prueba por Telegram.
10. Ver logs.

## Nota final

Priorizar robustez y simplicidad. La web embebida es el panel principal. Telegram es un canal secundario para alertas y comandos remotos autorizados.
