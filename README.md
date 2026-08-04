# Ecowitt Display Kiosk (XE1E)

Firmware para una pantalla dedicada del clima sobre **Waveshare ESP32-S3-Touch-LCD-7B**
(1024×600). Es el complemento del servidor
[ecowitt-weather-server-xe1e](https://github.com/XE1E/ecowitt-weather-server-xe1e).

## Filosofía: "el servidor renderiza, el ESP32 solo muestra"

En lugar de dibujar la interfaz en el ESP32 (LVGL, fuentes, iconos, layout…), el
**servidor** genera cada pantalla como un JPEG de 1024×600 y el ESP32 hace de
*display tonto*:

1. Baja el JPEG por WiFi: `GET /api/display.jpg?page=N`
2. Lo decodifica (JPEGDEC) **directo sobre el framebuffer de atrás** (RGB565),
   aplicando el **brillo** configurado en la misma pasada (sin buffer intermedio)
3. Hace **swap** de framebuffer alineado a **vsync** (doble buffer + bounce
   buffer) → **sin tearing ni desplazamiento**
4. Lee el **touch** (GT911) y mapea el toque a la pestaña correspondiente
5. Envía su **BME280** local al servidor (`POST /api/kiosk/local`); el servidor
   lo dibuja en la página 2

Los dos framebuffers del panel se usan además como **caché de las 2 últimas
páginas distintas**: volver a una que sigue en un framebuffer es un swap puro,
sin escribir PSRAM, así que la transición es perfectamente limpia. Al ir a una
página que no está en caché se muestra un **spinner** mientras se baja.

Ventajas: el diseño se edita en el servidor (React), no hay que recompilar el
firmware para cambiar la UI, y se elimina toda la complejidad de LVGL.

## Páginas

El servidor dibuja una **barra de pestañas** de 64 px en la franja inferior. El
firmware mapea la X del toque a la pestaña y salta a esa página; la franja
tocable es más alta que la barra visible (los últimos 110 px) para no fallar el
toque.

| Pestaña | Página          | URL del renderer        | Contenido                                                     |
|---------|-----------------|-------------------------|---------------------------------------------------------------|
| ☀️ Estación | 1           | `/kiosko?page=1`        | Temperatura grande, 6 tiles (humedad, presión, viento, lluvia, UV, IMECA) y pronóstico 6 h |
| 📍 Local    | 2           | `/kiosko?page=2`        | Sensor BME280 de **este** display, con mín/máx                 |
| 🏠 Sensores | 3           | `/kiosko?page=3`        | Sensores interior / jardín / estación remota (GW1100)          |
| 📅 7 días   | 4           | `/kiosko?page=4`        | Pronóstico a 7 días                                            |
| 📈 48 h     | 5           | `/kiosko?page=5`        | Resumen multivariable: temperatura, presión, lluvia, viento y humedad de las últimas 48 h |
| 🖥️ Consola  | `consola`   | `/kiosko?page=consola`  | Réplica de la consola física Ecowitt, **pantalla completa sin barra** |

La consola es especial: al no tener barra de pestañas, un toque en **cualquier
parte** regresa a la página 1. Su URL usa `?page=consola`, no un número.

> El orden y el número de pestañas tienen que coincidir entre el array `TABS`
> del dashboard y `NUM_TABS` / `PAGE_CONSOLA` en [`src/config.h`](src/config.h).
> Si se agrega una página hay que tocar ambos lados.

## Hardware

**Waveshare ESP32-S3-Touch-LCD-7B**
- ESP32-S3, PSRAM 8 MB (OPI), flash 16 MB
- LCD RGB 1024×600 16-bit @ 30 MHz
- Touch GT911 (I2C)
- Expansor CH422G (I2C) — reset del touch y backlight
- BME280 local (I2C)

I2C compartido en **GPIO 8 (SDA) / 9 (SCL)**. Pines del panel RGB en
[`src/rgb_lcd_port.h`](src/rgb_lcd_port.h) / [`src/config.h`](src/config.h).

El backlight cuelga del CH422G (IO2), que es un expansor **digital**: no tiene
PWM, así que no se puede atenuar por hardware. Por eso el brillo se aplica
escalando los píxeles al copiarlos al framebuffer.

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

Solo el **primer** flasheo necesita cable: después se puede actualizar por
**OTA** desde la página de configuración, subiendo
`.pio/build/esp32s3/firmware.bin` (o con `curl`):

```bash
pio run                                    # genera el .bin, sin subirlo
curl -F "fw=@.pio/build/esp32s3/firmware.bin" http://<IP-del-display>/api/ota
```

El `.bin` se escribe en la partición inactiva (`app0`/`app1`, 6.5 MB cada una).
Si la subida se corta, `otadata` no se conmuta y el display sigue arrancando con
el firmware anterior.

La altitud del sitio (para corregir la presión a nivel del mar) se ajusta desde
la página de configuración; `BME280_ALTITUDE` en `src/config.h` es solo el valor
del primer arranque (2240 m, CDMX).

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
- **BME280**: habilitado, intervalo de envío, **altitud del sitio** y **offsets de
  calibración** (temp/hum/presión) — también en vivo, así se calibra sin reflashear
- **Firmware (OTA)**: sube el `firmware.bin` desde el navegador, sin abrir el
  gabinete ni conectar el cable
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
  bme280_sensor.h   Lectura del BME280 local (altitud y offsets en runtime)
  io_extension.*    CH422G (reset touch, backlight)
  i2c.h             Wrapper I2C sobre Wire
docs/
  ARQUITECTURA.md      Detalle del diseño y decisiones
  DISPLAY_ISSUES.md    Problemas del panel RGB (tearing, rayitas) y su solución
  PLAN-CONSOLA-XE1E.md Plan de la página "consola"
stubs/              Header vacío de esp-dsp (JPEGDEC con -DNO_SIMD)
3d-prints/          Gabinete imprimible (STL + fotos)
```

## Impresión 3D

**Gabinete** para la pantalla Waveshare: archivo STL en [`3d-prints/enclosure/`](3d-prints/enclosure/) y en [Printables](https://www.printables.com/model/1306944-waveshare-esp32-s3-7inch-capacitive-touch-display).

## Estado

Firmware **v1.2.2**, funcionando en hardware. Validado en la placa: decodificado
y pintado del JPEG, swap de framebuffer sin tearing, reset y lectura del GT911,
navegación por pestañas, BME280, portal de configuración (modo AP y LAN),
escaneo de redes, brillo, offsets en vivo y actualización por OTA.

El lado servidor (renderer + endpoint `/api/kiosk/local`) está desplegado en
`https://clima.xe1e.net`.

Detalle del diseño y las decisiones en
[`docs/ARQUITECTURA.md`](docs/ARQUITECTURA.md); el historial de los problemas del
panel RGB (tearing, desplazamiento vertical, rayitas) y cómo se resolvieron, en
[`docs/DISPLAY_ISSUES.md`](docs/DISPLAY_ISSUES.md).
