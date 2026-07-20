/**
 * Ecowitt Display Kiosk (XE1E) - firmware ESP32-S3.
 *
 * Enfoque "display tonto": el SERVIDOR renderiza cada pagina del kiosco como un
 * JPEG 1024x600 (/api/display.jpg?page=N) y el ESP32 solo la baja, la decodifica
 * y la pinta. Ver docs/ARQUITECTURA.md.
 *
 * Arquitectura de tareas (para que el touch responda al instante):
 *   - Core 1 (loop): sondea el touch cada ~10 ms. Un tap cambia de pagina y
 *     despierta al task de red.
 *   - Core 0 (netTask): baja + decodifica + pinta la imagen; envia el BME280.
 *
 * Cada pagina se decodifica en su propio buffer en PSRAM (cache), asi que al
 * tocar se pinta al instante (sin esperar la red). El update de imagen se hace
 * con esp_lcd_panel_draw_bitmap desde un buffer aparte: el driver hace el
 * doble-buffer sincronizado a vsync -> sin tearing ni "brinco".
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

static const int NUM_PAGES = 2;
static const size_t FB_BYTES = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2;  // RGB565

// ── Estado compartido ────────────────────────────────────────────────────
static uint16_t *g_pagebuf[NUM_PAGES + 1] = { nullptr };  // decodificado por pagina (1..N)
static bool      g_ready[NUM_PAGES + 1]   = { false };     // ¿esa pagina ya tiene imagen?
static uint32_t  g_fetched[NUM_PAGES + 1] = { 0 };         // millis del ultimo fetch por pagina

static volatile int g_page = 1;        // pagina deseada (la cambia el touch)
static volatile int g_shown = 0;       // pagina actualmente pintada
static SemaphoreHandle_t g_wake;       // despierta al netTask (tap o arranque)

static LocalSensorData g_local;

// ── Pinta una pagina ya decodificada ──────────────────────────────────────
static void draw_page(int page)
{
    if (page < 1 || page > NUM_PAGES || !g_ready[page]) return;
    // Doble buffer: draw_bitmap copia al framebuffer de atras y el driver hace
    // el swap en vsync (CONFIG_LCD_RGB_RESTART_IN_VSYNC=y). Transicion limpia.
    wavesahre_rgb_lcd_display((uint8_t *)g_pagebuf[page]);
    g_shown = page;
}

// ── Baja + decodifica una pagina en su buffer ──────────────────────────────
static bool fetch_page(int page)
{
    const uint8_t *jpg = nullptr;
    size_t jpg_len = 0;
    if (!net_fetch_display(page, &jpg, &jpg_len)) return false;
    if (!jpeg_decode_to_fb(jpg, jpg_len, g_pagebuf[page])) return false;
    g_ready[page]   = true;
    g_fetched[page] = millis();
    return true;
}

// ── Task de red (core 0): fetch + decode + draw + BME280 ────────────────────
static void netTask(void *)
{
    net_begin();
    net_connect_wifi();

    uint32_t last_bme = 0;

    for (;;) {
        // Espera un tap (semaforo) o hasta 500 ms para el refresco periodico.
        xSemaphoreTake(g_wake, pdMS_TO_TICKS(500));

        int page = g_page;   // pagina deseada actual

        // 1) Cambio de pagina: si ya esta en cache, pintar al instante.
        if (page != g_shown && g_ready[page]) {
            draw_page(page);
        }

        // 2) Refresco de datos: si la pagina no tiene imagen o esta vieja.
        uint32_t now = millis();
        bool stale = !g_ready[page] ||
                     (now - g_fetched[page] >= UPDATE_INTERVAL_MS);
        if (stale) {
            if (fetch_page(page) && page == g_page) {
                draw_page(page);   // solo si sigue siendo la pagina deseada
            }
        }

        // 2b) Pre-cargar en cache las paginas que aun no tienen imagen, para
        //     que el primer cambio de pagina tambien sea instantaneo.
        for (int p = 1; p <= NUM_PAGES; p++) {
            if (!g_ready[p]) fetch_page(p);
        }

        // 3) BME280: leer y enviar cada REMOTE_STATION_INTERVAL segundos.
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

    // Panel RGB (el driver mantiene su propio doble framebuffer).
    waveshare_esp32_s3_rgb_lcd_init();

    // Un buffer de decodificacion por pagina (cache) en PSRAM.
    for (int p = 1; p <= NUM_PAGES; p++) {
        g_pagebuf[p] = (uint16_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
        if (g_pagebuf[p]) memset(g_pagebuf[p], 0, FB_BYTES);
        else Serial.printf("[boot] ERROR: sin PSRAM para pagina %d\n", p);
    }

    // Touch + BME280.
    touch_input_begin();
#if BME280_ENABLED
    initBME280(BME280_I2C_ADDR);
#endif

    // Task de red en el core 0; el loop() (touch) corre en el core 1.
    g_wake = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
    xSemaphoreGive(g_wake);   // primer render inmediato
}

void loop()
{
    // Sondeo rapido del touch. Un tap avanza de pagina y despierta al netTask,
    // que pinta la pagina cacheada al instante.
    if (touch_input_tapped()) {
        g_page = (g_page % NUM_PAGES) + 1;   // 1 -> 2 -> 1 ...
        Serial.printf("[touch] pagina -> %d\n", g_page);
        xSemaphoreGive(g_wake);
    }
    delay(10);
}
