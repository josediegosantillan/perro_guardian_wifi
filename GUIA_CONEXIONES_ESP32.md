# Guia de conexiones ESP32 DevKit

Esta guia corresponde al firmware `perro_guardian_wifi` configurado para un ESP32 DevKit estandar con ESP-IDF.

## Pinout usado por el firmware

| Funcion | GPIO ESP32 | Conexion externa | Nota |
| --- | ---: | --- | --- |
| Rele router | GPIO26 | Entrada IN del modulo rele | Salida digital. No conectar bobina directa al GPIO. |
| Boton reset manual | GPIO32 | Pulsador entre GPIO32 y GND | Usa pull-up interno. Mantener 1 segundo. |
| Boton portal WiFi | GPIO33 | Pulsador entre GPIO33 y GND | Usa pull-up interno. Mantener 3 segundos. |
| LED WiFi | GPIO16 | LED con resistencia a GND | Encendido si hay WiFi, parpadea si no. |
| LED Internet | GPIO17 | LED con resistencia a GND | Encendido si internet responde OK. |
| LED alerta | GPIO27 | LED con resistencia a GND | Encendido durante reset, parpadea en cooldown. |
| OLED SDA | GPIO21 | Pin SDA de la pantalla OLED | Bus I2C. |
| OLED SCL | GPIO22 | Pin SCL de la pantalla OLED | Bus I2C. |
| GND comun | GND | GND de todos los modulos | Obligatorio para senales estables. |

## Conexion de alimentacion

```text
Fuente 5 V estable -> pin 5V/VIN del ESP32 DevKit
GND fuente         -> GND del ESP32
```

Si usas un modulo rele de 5 V:

```text
5 V fuente -> VCC/JD-VCC del modulo rele, segun el modulo
GND fuente -> GND del modulo rele
GND rele   -> GND ESP32
GPIO26     -> IN del modulo rele
```

Importante: el GPIO del ESP32 entrega senal, no potencia. La bobina del rele debe estar alimentada por el modulo rele o por una etapa con transistor/MOSFET y diodo flyback.

## Conexion del rele al router

El proyecto esta pensado para cortar solamente la linea positiva de 12 V DC del router.

```text
Fuente 12 V +  -> COM del rele
NC del rele    -> positivo + del router
Fuente 12 V -  -> negativo - del router
```

Usar contacto `NC` hace que el router quede encendido por defecto. Si el ESP32 se apaga, falla o se reinicia, el rele vuelve al estado seguro y el router conserva alimentacion.

No cortar 220 V AC con este proyecto.

## Botones

Los dos botones van conectados igual: un lado al GPIO y el otro lado a GND.

### Reset manual del router

```text
GPIO32 -> pulsador -> GND
```

Mantener presionado 1 segundo. El firmware solicita un ciclo de apagado/encendido del router.

### Portal WiFi

```text
GPIO33 -> pulsador -> GND
```

Mantener presionado 3 segundos. El ESP32 reinicia y entra al portal WiFi.

Tambien se puede mantener presionado al arrancar para forzar el portal.

## LEDs de estado

Cada LED debe llevar resistencia serie, por ejemplo 220 ohm a 1 kohm.

```text
GPIO16 -> resistencia -> anodo LED WiFi     catodo -> GND
GPIO17 -> resistencia -> anodo LED Internet catodo -> GND
GPIO27 -> resistencia -> anodo LED Alerta   catodo -> GND
```

El firmware asume LED activo en alto:

```text
GPIO en 1 -> LED encendido
GPIO en 0 -> LED apagado
```

## Pantalla OLED I2C 1.3 pulgadas

Conexion recomendada:

```text
OLED VCC -> 3V3 del ESP32
OLED GND -> GND del ESP32
OLED SDA -> GPIO21
OLED SCL -> GPIO22
```

La direccion I2C por defecto es `0x3C`. Muchas pantallas muestran `0x78 / 0x7A` en la placa; eso normalmente equivale a `0x3C / 0x3D` en direccion I2C de 7 bits. El firmware intenta detectar ambas.

Si la OLED no responde, el firmware sigue funcionando sin reiniciarse.

## Diagrama general

```text
                 ESP32 DevKit
              +----------------+
              |            3V3 |---- VCC OLED
              |            GND |---- GND comun
              |         GPIO21 |---- SDA OLED
              |         GPIO22 |---- SCL OLED
              |         GPIO26 |---- IN modulo rele
              |         GPIO32 |---- Boton reset manual ---- GND
              |         GPIO33 |---- Boton portal WiFi  ---- GND
              |         GPIO16 |---- R ---- LED WiFi ---- GND
              |         GPIO17 |---- R ---- LED NET  ---- GND
              |         GPIO27 |---- R ---- LED ALERT ---- GND
              +----------------+

Fuente 12 V + ---- COM rele
NC rele ----------- + router
Fuente 12 V - ----- - router
```

## Pines que conviene evitar en ESP32 DevKit

* GPIO6 a GPIO11: usados por la flash SPI.
* GPIO34 a GPIO39: solo entrada, no sirven para rele ni LEDs.
* GPIO0, GPIO2, GPIO4, GPIO5, GPIO12 y GPIO15: pines de strapping; evitarlos para el rele.

## Prueba segura recomendada

1. Compilar con `TEST_MODE` activado.
2. Conectar solo ESP32, botones, LEDs y OLED.
3. Verificar logs por monitor serie.
4. Probar boton portal y boton reset manual.
5. Conectar el modulo rele sin conectar el router real.
6. Medir continuidad entre `COM` y `NC`.
7. Recien despues conectar la linea positiva de 12 V del router al contacto NC.

## Comandos utiles

Desde una terminal ESP-IDF:

```powershell
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

Portal WiFi manual:

```text
SSID: PERRO_GUARDIAN_WIFI
Password: 12345678
URL: http://192.168.4.1
```
