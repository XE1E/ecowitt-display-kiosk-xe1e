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
# 1. Configuración (opcional: solo defaults del primer arranque; el WiFi y la
#    URL se configuran después desde la página web del display)
cp my_config.h.template src/my_config.h

# 2. Compilar y subir
pio run -t upload

# 3. Monitor serie
pio device monitor
```

Ajusta la altitud de tu sitio en `src/config.h` (`BME280_ALTITUDE`, por defecto
2240 m para CDMX) para que la presión se corrija a nivel del mar.

## Configuración (página web del display)

Todo se configura desde una página web que sirve el propio ESP32
([`src/portal.h`](src/portal.h)) — no hace falta app ni recompilar. Hay dos
formas de llegar a ella:

| Situación | Cómo entrar |
|---|---|
| Sin red guardada, o ninguna responde | El display abre el AP **"EcowittKiosk"** (sin contraseña). Al conectarte, el **portal cautivo se abre solo**; si no, ve a `http://192.168.4.1/` |
| Ya conectado a tu red | `http://<IP-del-display>/` desde cualquier navegador de la LAN |

Para ver la IP sin entrar al router: **toque largo (~2.5 s) fuera de la barra de
pestañas** — muestra IP, SSID, señal y versión durante 15 s.

Qué se puede configurar:

- **Estado / diagnóstico** — SSID, RSSI, IP, MAC, uptime, firmware, PSRAM/heap,
  código y latencia del último GET, estado del BME280
- **3 redes WiFi** con **escaneo** (toca una red de la lista y se llena el slot).
  Se intentan en orden 1 → 2 → 3
- **URL del servidor**, con botón **Probar conexión** (código HTTP y ms) antes de
  guardar
- **Brillo** (1–10) e **intervalo de refresco** (1–15 min) — se aplican sin
  reiniciar
- **BME280**: habilitado, intervalo de envío y **offsets de calibración**
  (temp/hum/presión) — también en vivo, así se calibra sin reflashear
- **Mantenimiento**: forzar refresco, reiniciar y borrar configuración

Lo que cambia WiFi o URL reinicia el display; el resto se aplica al instante.

Todo se guarda en **NVS** y persiste tras reinicios y reflasheos: una vez
guardado algo, `src/my_config.h` ya no manda (solo aporta los defaults del primer
arranque). Para volver a cero: **Borrar configuración** en el portal, o
`pio run -t erase` antes de flashear.

> El primer arranque siempre entra en modo AP y espera indefinidamente: sin red
> guardada no hay a dónde conectarse. Si hay redes guardadas pero ninguna
> responde, el portal expira a los `PORTAL_TIMEOUT` segundos y reintenta.

## Estructura

```
src/
  main.cpp          Orquestación (fetch → decode → swap; touch; BME280)
  config.h          Pines de hardware + struct del sensor
  my_config.h       Defaults del primer arranque (NO versionado; copiar del template)
  settings.h        Ajustes persistentes en NVS (redes, URL, brillo, sensor)
  portal.h          Página web de configuración + portal cautivo (AP y LAN)
  status.h          Estado de ejecución para el bloque de diagnóstico
  wifi_config.h     Conexión WiFi (3 redes con fallback) y salto al portal
  ap_screen.h       Pantallas locales: modo AP e info (IP + URL del portal)
  rgb_lcd_port.*    Driver del panel RGB (esp_lcd nativo, sin LVGL)
  jpeg_render.h     JPEGDEC → framebuffer RGB565
  touch_input.h     GT911 sobre Wire + detección de tap
  net.h             HTTP: GET del JPEG + POST del BME280
  bme280_sensor.h   Lectura del BME280 local
  io_extension.*    CH422G (reset touch, backlight)
  i2c.h             Wrapper I2C sobre Wire
docs/
  ARQUITECTURA.md   Detalle del diseño y decisiones
```

## Impresión 3D

**Gabinete** para la pantalla Waveshare: archivo STL en [`3d-prints/enclosure/`](3d-prints/enclosure/) y en [Printables](https://www.printables.com/model/1306944-waveshare-esp32-s3-7inch-capacitive-touch-display).

## Estado

Ver [`docs/ARQUITECTURA.md`](docs/ARQUITECTURA.md) para el detalle y los puntos
que faltan por validar en hardware real (endianness del JPEG, semántica exacta
del swap de framebuffer, secuencia de reset del GT911).

El lado servidor (renderer + endpoint `/api/kiosk/local`) ya está desplegado y
verificado en `https://clima.xe1e.net`.
