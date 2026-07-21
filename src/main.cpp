/**
 * Ecowitt Display Kiosk (XE1E) - firmware ESP32-S3.
 *
 * Enfoque "display tonto": el SERVIDOR renderiza cada pagina del kiosco como un
 * JPEG 1024x600 (/api/display.jpg?page=N) y el ESP32 solo la baja, la decodifica
 * y la pinta. Ver docs/ARQUITECTURA.md.
 *
 * Navegacion: el servidor dibuja una BARRA DE PESTAÑAS abajo (una por pagina).
 * El firmware mapea la coordenada del toque a la pestaña -> salta a esa pagina.
 *
 * Framebuffers: el panel tiene 2. Se usan como cache de las 2 ultimas paginas
 * distintas mostradas. Volver a una pagina que sigue en un FB = swap PURO (sin
 * escribir PSRAM -> sin contencion -> transicion 100% limpia). Ir a una pagina
 * nueva la decodifica en el FB de atras (breve brinca al escribir el frame, solo
 * en ese tap). El refresco de datos recarga el FB mostrado (brinca, poco seguido).
 *
 * Tareas: core 1 (loop) sondea el touch; core 0 (netTask) baja/decodifica/pinta
 * y envia el BME280.
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-7B (8MB PSRAM OPI, 16MB flash).
 */

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "config.h"
#include "my_config.h"
#include "rgb_lcd_port.h"
#include "jpeg_render.h"
#include "touch_input.h"
#include "net.h"
#include "bme280_sensor.h"

// Defaults por si my_config.h (copiado de una plantilla vieja) no los trae.
#ifndef BME280_TEMP_OFFSET
#define BME280_TEMP_OFFSET  0.0f
#endif
#ifndef BME280_HUM_OFFSET
#define BME280_HUM_OFFSET   0.0f
#endif
#ifndef BME280_PRESS_OFFSET
#define BME280_PRESS_OFFSET 0.0f
#endif

// Numero de paginas del kiosco. DEBE coincidir con las paginas que dibuja el
// servidor (KioskPage) y con el numero de pestañas de la barra.
static const int NUM_PAGES = 5;

// Barra de pestañas: franja inferior de la pantalla. Un toque en esta franja
// selecciona la pestaña segun la X (repartidas por igual). El servidor dibuja
// la barra en la misma zona. Un toque fuera de la franja se ignora.
static const int TABBAR_H   = 64;
static const int TABBAR_TOP = SCREEN_HEIGHT - TABBAR_H;   // y >= 536 = barra

static const size_t FB_BYTES = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2;  // RGB565

// ── Estado compartido ────────────────────────────────────────────────────
static uint16_t *g_fb[2]     = { nullptr, nullptr };  // los 2 framebuffers del panel
static int       g_fbPage[2] = { 0, 0 };              // que pagina tiene cada FB (0=ninguna)
static int       g_shownFb   = 0;                     // indice del FB mostrado
static uint16_t *g_scratch   = nullptr;               // buffer de decodificacion (offscreen)
static uint32_t  g_fetched[NUM_PAGES + 1] = { 0 };    // millis del ultimo fetch por pagina

static volatile int g_page  = 1;   // pagina deseada (la cambia el touch)
static volatile int g_shown = 0;   // pagina mostrada actualmente
static SemaphoreHandle_t g_wake;   // despierta al netTask (tap o arranque)

static LocalSensorData g_local;

// ── Baja + decodifica la pagina y la copia al framebuffer indicado ──────────
// La copia va en trozos con flush+micro-pausa para no saturar el bus PSRAM (si
// fbIdx es el FB mostrado, ahi ocurre el unico brinca; poco seguido).
static bool load_into(int fbIdx, int page)
{
    const uint8_t *jpg = nullptr;
    size_t jpg_len = 0;
    if (!net_fetch_display(page, &jpg, &jpg_len)) return false;
    if (!jpeg_decode_to_fb(jpg, jpg_len, g_scratch)) return false;

    const int CHUNK_ROWS = 30;
    const size_t row_bytes = (size_t)SCREEN_WIDTH * 2;
    uint8_t *dst = (uint8_t *)g_fb[fbIdx];
    uint8_t *src = (uint8_t *)g_scratch;
    for (int y = 0; y < SCREEN_HEIGHT; y += CHUNK_ROWS) {
        int rows = min(CHUNK_ROWS, SCREEN_HEIGHT - y);
        size_t off = (size_t)y * row_bytes, n = (size_t)rows * row_bytes;
        memcpy(dst + off, src + off, n);
        waveshare_fb_flush(dst + off, n);
        delayMicroseconds(200);
    }
    g_fbPage[fbIdx]  = page;
    g_fetched[page]  = millis();
    return true;
}

// ── Muestra una pagina ──────────────────────────────────────────────────────
static void show(int page)
{
    if (page < 1 || page > NUM_PAGES) return;

    // ¿Ya esta cargada en algun framebuffer? -> swap PURO (transicion limpia).
    for (int i = 0; i < 2; i++) {
        if (g_fbPage[i] == page) {
            waveshare_swap_fb(g_fb[i]);
            g_shownFb = i;
            g_shown = page;
            return;
        }
    }
    // No cacheada: cargarla en el FB de atras y conmutar.
    int back = g_shownFb ^ 1;
    if (load_into(back, page)) {
        waveshare_swap_fb(g_fb[back]);
        g_shownFb = back;
        g_shown = page;
    }
}

// ── Task de red (core 0) ────────────────────────────────────────────────────
static void netTask(void *)
{
    net_begin();
    net_connect_wifi();

    uint32_t last_bme = 0;

    for (;;) {
        xSemaphoreTake(g_wake, pdMS_TO_TICKS(500));

        int page = g_page;

        // Cambio de pagina (tap en una pestaña).
        if (page != g_shown) show(page);

        // Refresco de datos de la pagina mostrada, si esta vieja: recarga su
        // propio FB (actualiza en vivo; breve brinca, poco seguido).
        uint32_t now = millis();
        if (g_shown >= 1 &&
            (g_fetched[g_shown] == 0 || now - g_fetched[g_shown] >= UPDATE_INTERVAL_MS)) {
            for (int i = 0; i < 2; i++) {
                if (g_fbPage[i] == g_shown) { load_into(i, g_shown); break; }
            }
        }

        // BME280: leer y enviar cada REMOTE_STATION_INTERVAL segundos.
#if BME280_ENABLED
        if (isBME280Available() &&
            (last_bme == 0 || now - last_bme >= (uint32_t)REMOTE_STATION_INTERVAL * 1000UL)) {
            if (readBME280(g_local) && g_local.valid) {
                net_post_local(g_local.temperature, g_local.humidity, g_local.pressure);
            }
            last_bme = now;
        }
#endif
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] Ecowitt Display Kiosk (XE1E)");

    // I2C compartido (CH422G, GT911, BME280) en 8/9.
    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);

    // Panel RGB + sus dos framebuffers.
    waveshare_esp32_s3_rgb_lcd_init();
    waveshare_get_frame_buffer((void **)&g_fb[0], (void **)&g_fb[1]);
    for (int i = 0; i < 2; i++)
        if (g_fb[i]) memset(g_fb[i], 0, FB_BYTES);
    g_shownFb = 0;

    // Buffer de decodificacion (offscreen) en PSRAM.
    g_scratch = (uint16_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!g_scratch) Serial.println("[boot] ERROR: sin PSRAM para el buffer de decode");

    // Touch + BME280.
    touch_input_begin();
#if BME280_ENABLED
    initBME280(BME280_I2C_ADDR);
    setBME280TemperatureOffset(BME280_TEMP_OFFSET);
    setBME280HumidityOffset(BME280_HUM_OFFSET);
    setBME280PressureOffset(BME280_PRESS_OFFSET);
#endif

    // Task de red en el core 0; el loop() (touch) corre en el core 1.
    g_wake = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
    xSemaphoreGive(g_wake);   // primer render inmediato
}

void loop()
{
    // Sondeo rapido del touch. Un toque en la barra de pestañas (franja inferior)
    // salta a la pagina correspondiente; fuera de la barra se ignora.
    uint16_t tx = 0, ty = 0;
    if (touch_input_tapped(&tx, &ty)) {
        if (ty >= TABBAR_TOP) {
            int idx = (int)((uint32_t)tx * NUM_PAGES / SCREEN_WIDTH);   // 0..N-1
            if (idx < 0) idx = 0;
            if (idx >= NUM_PAGES) idx = NUM_PAGES - 1;
            int p = idx + 1;
            Serial.printf("[touch] tab x=%u y=%u -> pagina %d\n", tx, ty, p);
            if (p != g_page) { g_page = p; xSemaphoreGive(g_wake); }
        } else {
            Serial.printf("[touch] x=%u y=%u (fuera de la barra, ignorado)\n", tx, ty);
        }
    }
    delay(10);
}
