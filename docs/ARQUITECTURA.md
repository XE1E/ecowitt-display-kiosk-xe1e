# Arquitectura

## Resumen

El proyecto se dividió en dos mitades:

- **Servidor** (repo `ecowitt-weather-server-xe1e`): renderiza las páginas del
  kiosco y las sirve como JPEG.
- **Firmware** (este repo): baja el JPEG y lo pinta; nada de UI local.

```
┌─────────────────────────────┐        HTTPS         ┌────────────────────────┐
│  Servidor (VPS)             │                       │  ESP32-S3 (display)     │
│                             │  GET /api/display.jpg │                         │
│  dashboard /kiosko?page=N   │ ───────?page=N──────► │  fetch → JPEGDEC →      │
│        │ (React)            │      (JPEG 1024×600)  │  framebuffer → swap     │
│        ▼                    │                       │        │                │
│  renderer (Chromium         │ ◄──POST /api/kiosk────│  GT911 tap → page++     │
│   headless) → screenshot    │        /local (JSON)  │  BME280 → POST          │
└─────────────────────────────┘                       └────────────────────────┘
```

## Lado servidor (ya desplegado)

- **`/kiosko?page=N`**: página React de tamaño fijo 1024×600. Marca
  `data-kiosk-ready="true"` cuando ya tiene datos (señal para capturar).
- **`renderer`**: contenedor Playwright/Chromium. Abre `/kiosko?page=N`, espera
  el `data-kiosk-ready`, hace screenshot JPEG 1024×600 y lo cachea ~45 s.
  Expuesto por nginx del dashboard en **`/api/display.jpg?page=N`**.
- **`/api/kiosk/local`**: `POST` guarda el BME280 del display (último + min/máx
  del día, reset a medianoche MX); `GET` lo devuelve para dibujar la página 2.

## Lado firmware (este repo)

### Panel RGB sin tearing
El driver `esp_lcd` nativo se configura con `num_fbs=2` (doble framebuffer) y
`bounce_buffer_size_px = H_RES*10`. El flujo:

1. `waveshare_get_frame_buffer()` devuelve los dos framebuffers en PSRAM.
2. Se decodifica el JPEG **directo** sobre el framebuffer **de atrás** (el que no
   se muestra), aplicando el brillo fila por fila en la misma pasada: no hay
   buffer intermedio. El flush de caché→PSRAM va **por trozos de 30 filas con una
   micro-pausa**, que es lo que reparte el tráfico de bus y evita dejar sin datos
   al DMA del panel.
3. `esp_lcd_panel_draw_bitmap(...)` con el puntero del framebuffer de atrás hace
   el **swap** (el driver reconoce el puntero como uno de sus FBs y conmuta sin
   copiar). Se alterna el índice en cada refresco.

El callback `on_bounce_frame_finish` libera un semáforo (`waveshare_wait_vsync`)
para alinear el swap con el fin de frame.

### Touch
Sin `esp_lcd_touch` (que exige el driver I2C nativo de IDF): se habla con el
GT911 directamente por `Wire`, igual que el BME280. El reset del GT911 va por el
expansor **CH422G** (IO1) y el pin **INT** (GPIO4) selecciona la dirección I2C
(INT bajo al soltar el reset → `0x5D`). Solo detectamos "un tap" (un dedo, con
debounce de 300 ms) para avanzar de página.

### Red
`WiFiClientSecure` con `setInsecure()` (no valida el certificado): el display es
de solo lectura en una red de confianza y no maneja secretos. El JPEG se baja a
un buffer en PSRAM (256 KB de margen para imágenes de ~50 KB).

## Validado en hardware (cómo quedó cada punto dudoso)

Todo lo que al principio estaba escrito "a ciegas" (siguiendo la documentación de
Waveshare, sin placa en el entorno de desarrollo) ya se probó y quedó así:

1. **Endianness del JPEG** (`jpeg_render.h`): `RGB565_LITTLE_ENDIAN` es la
   correcta para este panel; los colores salen bien.
2. **Semántica del swap** (`main.cpp` / `rgb_lcd_port.cpp`):
   `esp_lcd_panel_draw_bitmap` con el puntero de un framebuffer del driver
   conmuta sin copia. Hizo falta, además: **`CONFIG_LCD_RGB_RESTART_IN_VSYNC=n`**
   (con `=y` aparecían desplazamiento vertical y rayitas a la izquierda),
   **`CONFIG_SPIRAM_XIP_FROM_PSRAM=y`** (Flash y PSRAM comparten bus en el S3; la
   CPU buscando instrucciones en Flash dejaba al DMA sin bounce buffer), esperar
   el semáforo de vsync antes de cada swap, y copiar al framebuffer **por trozos
   con flush y micro-pausa** para no starvar al DMA. Detalle completo en
   [`DISPLAY_ISSUES.md`](DISPLAY_ISSUES.md).
3. **Reset/dirección del GT911** (`touch_input.h`): la secuencia por CH422G con
   INT bajo funciona y responde en `0x5D`; no hizo falta la dirección `0x14`.
   Sí hubo que detectar el **flanco** del toque puenteando los huecos en que el
   GT911 deja `bit7=0` durante un toque sostenido (`TOUCH_RELEASE_MS`).
4. **Timings del panel** (`rgb_lcd_port.cpp`): los back/front porch del ejemplo
   de Waveshare son correctos para este modelo. Lo que sí se bajó es el bounce
   buffer, a **3 líneas** (`H_RES*3`), que es lo que minimiza el desplazamiento.

## Regla de coherencia de la caché de framebuffers

`g_fbPage[]` dice qué página tiene cada framebuffer, y de ahí sale el "swap puro"
al volver a una página. Por eso **todo lo que pinte algo que no sea una página
(spinner, pantalla de info, pantalla de AP) tiene que invalidar esa entrada**
(`FB_INVALID`, o `FB_SPINNER` si es el spinner). Si no, el firmware cree que ese
framebuffer sigue teniendo una página válida y un cambio de pestaña lo presenta
tal cual: se queda mostrando el spinner hasta el siguiente refresco por intervalo
(hasta 15 min).

Los framebuffers los escribe **solo el netTask** (core 0), salvo el spinner
inmediato del toque, que se pinta desde `loop()` (core 1) para que el
oscurecimiento sea instantáneo. Ese caso se serializa con el mutex `g_fbmux`, y
el toque lo pide con *try-lock*: si el netTask está en medio de una carga, no
oscurece (la página nueva está a punto de aparecer de todos modos).

## Origen del código de hardware

Los ficheros de bajo nivel (`rgb_lcd_port.*`, `io_extension.*`, `i2c.h`,
`bme280_sensor.h`) se reutilizaron del repo anterior basado en LVGL
(`ESP32-S3-Ecowitt-Display`), adaptándolos: se eliminó toda dependencia de LVGL
y el stack de touch `esp_lcd_touch` se reescribió sobre `Wire`.
