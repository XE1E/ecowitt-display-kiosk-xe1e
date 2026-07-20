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
2. Se decodifica el JPEG sobre el framebuffer **de atrás** (el que no se muestra).
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

## Pendiente de validar en hardware real

Estas partes están escritas siguiendo la documentación y el código de ejemplo de
Waveshare, pero **no se han podido probar en la placa** (no hay hardware en el
entorno de desarrollo). Al primer flasheo, revisar:

1. **Endianness del JPEG** (`jpeg_render.h`): se usa `RGB565_LITTLE_ENDIAN`. Si
   los colores salen con rojo/azul invertidos, cambiar a `RGB565_BIG_ENDIAN`.
2. **Semántica del swap** (`main.cpp` / `rgb_lcd_port.cpp`): confirmar que
   `esp_lcd_panel_draw_bitmap` con el puntero de un framebuffer completo conmuta
   sin copia y sin tearing. Si hubiera parpadeo, puede requerir
   `CONFIG_LCD_RGB_RESTART_IN_VSYNC` o ajustar el uso del semáforo de vsync.
3. **Reset/dirección del GT911** (`touch_input.h`): si el GT911 no responde,
   probar la dirección de respaldo `0x14` y revisar los tiempos de la secuencia
   de reset vía CH422G.
4. **Timings del panel** (`rgb_lcd_port.cpp`): los back/front porch vienen del
   ejemplo de Waveshare para este modelo; si la imagen sale desplazada, ajustar.

## Origen del código de hardware

Los ficheros de bajo nivel (`rgb_lcd_port.*`, `io_extension.*`, `i2c.h`,
`bme280_sensor.h`) se reutilizaron del repo anterior basado en LVGL
(`ESP32-S3-Ecowitt-Display`), adaptándolos: se eliminó toda dependencia de LVGL
y el stack de touch `esp_lcd_touch` se reescribió sobre `Wire`.
