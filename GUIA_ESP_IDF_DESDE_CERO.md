# Crear un proyecto ESP-IDF desde cero

Guia rapida para crear, configurar, compilar y flashear un proyecto ESP-IDF en Windows usando PowerShell.

## 1. Abrir una terminal con ESP-IDF habilitado

Si PowerShell bloquea `export.ps1`, usar `ExecutionPolicy Bypass` solo para esa sesion:

```powershell
powershell -ExecutionPolicy Bypass
```

Dentro de esa terminal, cargar ESP-IDF:

```powershell
. 'C:\esp\v5.5.4\esp-idf\export.ps1'
```

Verificar que `idf.py` esta disponible:

```powershell
idf.py --version
```

## 2. Crear la carpeta del proyecto

Elegir una ubicacion de trabajo:

```powershell
cd C:\ESP32_PROYECTOS
mkdir mi_proyecto_esp32
cd mi_proyecto_esp32
```

## 3. Crear la estructura minima

Crear la carpeta `main`:

```powershell
mkdir main
```

Crear `CMakeLists.txt` en la raiz:

```cmake
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mi_proyecto_esp32)
```

Crear `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ".")
```

Crear `main/main.c`:

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "app";

void app_main(void)
{
    while (1) {
        ESP_LOGI(TAG, "Proyecto ESP-IDF funcionando");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

## 4. Seleccionar el chip

Para ESP32-C3:

```powershell
idf.py set-target esp32c3
```

Otros ejemplos:

```powershell
idf.py set-target esp32
idf.py set-target esp32s3
idf.py set-target esp32c6
```

## 5. Configurar opciones del proyecto

Abrir el menu de configuracion:

```powershell
idf.py menuconfig
```

Opciones comunes:

- Serial flasher config: puerto, baudrate y tamano de flash.
- Partition Table: tabla de particiones.
- Component config: WiFi, logs, FreeRTOS, NVS, etc.

## 6. Compilar

```powershell
idf.py build
```

Si compila bien, se genera el firmware dentro de `build/`.

## 7. Flashear

Con puerto automatico:

```powershell
idf.py flash
```

Con puerto especifico:

```powershell
idf.py -p COM3 flash
```

Cambiar `COM3` por el puerto real del dispositivo.

## 8. Ver el monitor serie

```powershell
idf.py monitor
```

Flashear y abrir monitor en un solo paso:

```powershell
idf.py -p COM3 flash monitor
```

Para salir del monitor:

```text
Ctrl + ]
```

## 9. Archivos recomendados para una base limpia

Agregar `.gitignore`:

```gitignore
build/
managed_components/
dependencies.lock
sdkconfig.old
```

Agregar `sdkconfig.defaults` para guardar configuraciones base sin depender del `sdkconfig` completo.

Ejemplo:

```text
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_MONITOR_BAUD=115200
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

## 10. Inicializar Git

```powershell
git init
git add .
git commit -m "Base ESP-IDF inicial"
```

## 11. Problemas frecuentes

### PowerShell bloquea export.ps1

Error:

```text
La ejecucion de scripts esta deshabilitada en este sistema
```

Solucion temporal:

```powershell
powershell -ExecutionPolicy Bypass
. 'C:\esp\v5.5.4\esp-idf\export.ps1'
```

Solucion para el usuario actual:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

### idf.py no se reconoce

Significa que el entorno de ESP-IDF no fue cargado. Ejecutar:

```powershell
. 'C:\esp\v5.5.4\esp-idf\export.ps1'
```

### CMake no encuentra project.cmake

Generalmente falta `IDF_PATH`. Cargar el entorno completo con `export.ps1`, no solo ejecutar `ninja` directamente.

```powershell
. 'C:\esp\v5.5.4\esp-idf\export.ps1'
idf.py build
```

## 12. Flujo recomendado diario

```powershell
cd C:\ESP32_PROYECTOS\mi_proyecto_esp32
. 'C:\esp\v5.5.4\esp-idf\export.ps1'
idf.py build
idf.py -p COM3 flash monitor
```

