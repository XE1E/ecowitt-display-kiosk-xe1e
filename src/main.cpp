/**
 * Ecowitt Display Kiosk (XE1E) - firmware ESP32-S3.
 *
 * Enfoque "display tonto": el SERVIDOR renderiza cada pagina del kiosco como un
 * JPEG 1024x600 (/api/display.jpg?page=N) y el ESP32 solo:
 *   1. Baja el JPEG por WiFi.
 *   2. Lo decodifica sobre el framebuffer de atras (JPEGDEC -> RGB565).
 *   3. Hace swap del framebuffer (doble buffer + bounce buffer) -> sin tearing.
 *   4. Lee el touch (GT911): un tap cambia de pagina.
 *   5. Envia su BME280 local al servidor (POST /api/kiosk/local) cada N seg;
 *      el servidor lo dibuja en la pagina 2.
 *
 * Sin LVGL. Ver docs/ARQUITECTURA.md.
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-7B (8MB PSRAM OPI, 16MB flash).
 */

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "my_config.h"
#include "rgb_lcd_port.h"
#include "jpeg_render.h"
#include "touch_input.h"
#include "net.h"
#include "bme280_sensor.h"

// ── Estado ──────────────────────────────────────────────────────────────────
static uint16_t *g_fb[2] = { nullptr, nullptr };  // los dos framebuffers del panel
static int  g_back = 0;         // indice del framebuffer que NO se esta mostrando
static int  g_page = 1;         // pagina actual del kiosco
static const int NUM_PAGES = 2; // paginas disponibles en el servidor

static uint32_t g_last_img = 0;     // ultimo refresco de imagen
static uint32_t g_last_bme = 0;     // ultimo POST del BME280
static bool     g_need_refresh = true;

static LocalSensorData g_local;

// ── Refresco de imagen ────────────────────────────────────────────────────
// Baja el JPEG de la pagina actual, lo decodifica en el framebuffer de atras y
// hace swap. Si algo falla, deja la imagen anterior.
static void refresh_image()
{
    const uint8_t *jpg = nullptr;
    size_t jpg_len = 0;
    if (!net_fetch_display(g_page, &jpg, &jpg_len)) return;

    uint16_t *back = g_fb[g_back];
    if (!jpeg_decode_to_fb(jpg, jpg_len, back)) return;

    // Swap: mostrar el buffer recien pintado. Con num_fbs=2, pasar un puntero
    // que ES uno de los framebuffers hace que el driver conmute a el (sin copia).
    waveshare_wait_vsync(100);
    wavesahre_rgb_lcd_display((uint8_t *)back);

    g_back ^= 1;                 // el que se mostraba pasa a ser el de atras
    g_last_img = millis();
    g_need_refresh = false;
}

// ── BME280: leer y enviar ───────────────────────────────────────────────────
static void refresh_bme()
{
#if BME280_ENABLED
    if (!isBME280Available()) return;
    if (readBME280(g_local) && g_local.valid) {
        net_post_local(g_local.temperature, g_local.humidity, g_local.pressure);
    }
    g_last_bme = millis();
#endif
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] Ecowitt Display Kiosk (XE1E)");

    // I2C compartido (CH422G, GT911, BME280) en 8/9.
    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);

    // Panel RGB + framebuffers.
    waveshare_esp32_s3_rgb_lcd_init();
    waveshare_get_frame_buffer((void **)&g_fb[0], (void **)&g_fb[1]);
    // Limpiar ambos framebuffers a negro para no mostrar basura al arranque.
    for (int i = 0; i < 2; i++) {
        if (g_fb[i]) memset(g_fb[i], 0, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2);
    }
    g_back = 0;

    // Touch.
    touch_input_begin();

    // BME280 local.
#if BME280_ENABLED
    initBME280(BME280_ADDRESS);
#endif

    // Red + buffer de imagen en PSRAM.
    net_begin();
    net_connect_wifi();

    // Primer render inmediato.
    refresh_image();
    refresh_bme();
}

void loop()
{
    // Touch: un tap avanza de pagina y fuerza refresco inmediato.
    if (touch_input_tapped()) {
        g_page = (g_page % NUM_PAGES) + 1;   // 1 -> 2 -> 1 ...
        Serial.printf("[touch] pagina -> %d\n", g_page);
        g_need_refresh = true;
    }

    uint32_t now = millis();

    // Refresco periodico de la imagen (o inmediato tras un tap).
    if (g_need_refresh || now - g_last_img >= UPDATE_INTERVAL_MS) {
        refresh_image();
    }

    // Envio periodico del BME280.
#if BME280_ENABLED
    if (now - g_last_bme >= (uint32_t)REMOTE_STATION_INTERVAL * 1000UL) {
        refresh_bme();
    }
#endif

    delay(20);   // ~50 Hz de sondeo de touch
}
