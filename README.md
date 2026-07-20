# Ecowitt Display Kiosk (XE1E)

Firmware para una pantalla dedicada del clima sobre **Waveshare ESP32-S3-Touch-LCD-7B**
(1024×600). Es el complemento del servidor
[ecowitt-weather-server-xe1e](https://github.com/XE1E/ecowitt-weather-server-xe1e).

## Filosofía: "el servidor renderiza, el ESP32 solo muestra"

En lugar de dibujar la interfaz en el ESP32 (LVGL, fuentes, iconos, layout…), el
**servidor** genera cada pantalla como un JPEG de 1024×600 y el ESP32 hace de
*display tonto*:

1. Baja el JPEG por WiFi: `GET /api/display.jpg?page=N`
2. Lo decodifica (JPEGDEC) sobre el **framebuffer de atrás** (RGB565)
3. Hace **swap** de framebuffer (doble buffer + bounce buffer) → **sin tearing**
4. Lee el **touch** (GT911): un *tap* cambia de página
5. Envía su **BME280** local al servidor (`POST /api/kiosk/local`); el servidor
   lo dibuja en la página 2

Ventajas: el diseño se edita en el servidor (React), no hay que recompilar el
firmware para cambiar la UI, y se elimina toda la complejidad de LVGL.

## Páginas

| Página | URL en el servidor        | Contenido                              |
|--------|---------------------------|----------------------------------------|
| 1      | `/kiosko?page=1`          | Estación: temp, tiles, pronóstico 6 h  |
| 2      | `/kiosko?page=2`          | Sensor local BME280 (este display)     |

Un *tap* en la pantalla avanza a la siguiente página (1 → 2 → 1…).

## Hardware

**Waveshare ESP32-S3-Touch-LCD-7B**
- ESP32-S3, PSRAM 8 MB (OPI), flash 16 MB
- LCD RGB 1024×600 16-bit @ 30 MHz
- Touch GT911 (I2C)
- Expansor CH422G (I2C) — reset del touch y backlight
- BME280 local (I2C)

I2C compartido en **GPIO 8 (SDA) / 9 (SCL)**. Pines del panel RGB en
[`src/rgb_lcd_port.h`](src/rgb_lcd_port.h) / [`src/config.h`](src/config.h).

## Compilar y flashear (PlatformIO)

```bash
# 1. Credenciales
cp my_config.h.template src/my_config.h
#    edita src/my_config.h: WIFI_SSID1/PASSWORD1 y API_BASE_URL

# 2. Compilar y subir
pio run -t upload

# 3. Monitor serie
pio device monitor
```

Ajusta la altitud de tu sitio en `src/config.h` (`BME280_ALTITUDE`, por defecto
2240 m para CDMX) para que la presión se corrija a nivel del mar.

## Estructura

```
src/
  main.cpp          Orquestación (fetch → decode → swap; touch; BME280)
  config.h          Pines de hardware + struct del sensor
  my_config.h       Credenciales (NO versionado; copiar del template)
  rgb_lcd_port.*    Driver del panel RGB (esp_lcd nativo, sin LVGL)
  jpeg_render.h     JPEGDEC → framebuffer RGB565
  touch_input.h     GT911 sobre Wire + detección de tap
  net.h             WiFi + GET del JPEG + POST del BME280
  bme280_sensor.h   Lectura del BME280 local
  io_extension.*    CH422G (reset touch, backlight)
  i2c.h             Wrapper I2C sobre Wire
docs/
  ARQUITECTURA.md   Detalle del diseño y decisiones
```

## Estado

Ver [`docs/ARQUITECTURA.md`](docs/ARQUITECTURA.md) para el detalle y los puntos
que faltan por validar en hardware real (endianness del JPEG, semántica exacta
del swap de framebuffer, secuencia de reset del GT911).

El lado servidor (renderer + endpoint `/api/kiosk/local`) ya está desplegado y
verificado en `https://clima.xe1e.net`.
