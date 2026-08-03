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
 * nueva la decodifica en el FB de atras. El swap espera vsync para alinearse con
 * el inicio del barrido -> sin tearing ni desplazamiento. El refresco de datos
 * tambien escribe al FB de atras y hace swap (true double buffering).
 *
 * Tareas: core 1 (loop) sondea el touch; core 0 (netTask) baja/decodifica/pinta
 * y envia el BME280.
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-7B (8MB PSRAM OPI, 16MB flash).
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "config.h"
#include "my_config.h"
#include "settings.h"
#include "status.h"
#include "rgb_lcd_port.h"
#include "ap_screen.h"
#include "bme280_sensor.h"
#include "portal.h"
#include "wifi_config.h"
#include "jpeg_render.h"
#include "touch_input.h"
#include "net.h"

// Páginas: constantes en config.h (NUM_TABS, PAGE_CONSOLA, MAX_PAGE_ID). La barra
// (que dibuja el servidor) muestra NUM_TABS pestañas: las 5 numeradas + "Consola".
// La consola (PAGE_CONSOLA) es full-screen SIN barra; estando en ella, un toque en
// cualquier parte regresa a la página 1.

// Barra de pestañas: franja inferior de la pantalla. Un toque en esta franja
// selecciona la pestaña segun la X (repartidas por igual). El servidor dibuja
// la barra en la misma zona. Un toque fuera de la franja se ignora.
static const int TABBAR_H    = 64;
// Zona TOCABLE de la barra (mas alta que la barra visible, para no fallar el
// toque): la franja inferior de 110 px cuenta como barra de pestañas.
static const int TAB_HIT_TOP = SCREEN_HEIGHT - 110;   // y >= 490 = barra

static const size_t FB_BYTES = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2;  // RGB565

// ── Spinner de carga ─────────────────────────────────────────────────────
static const int SPINNER_CX = SCREEN_WIDTH / 2;   // centro X
static const int SPINNER_CY = SCREEN_HEIGHT / 2;  // centro Y
static const int SPINNER_R1 = 30;                 // radio interno
static const int SPINNER_R2 = 50;                 // radio externo
static const int SPINNER_SEGMENTS = 8;            // número de barras

// Colores RGB565 para el degradado del spinner (de más brillante a más oscuro)
static const uint16_t SPINNER_COLORS[SPINNER_SEGMENTS] = {
    0xFFFF,  // blanco (barra activa)
    0xDEFB,  // gris muy claro
    0xBDF7,  // gris claro
    0x9CF3,  // gris medio-claro
    0x7BEF,  // gris medio
    0x5AEB,  // gris medio-oscuro
    0x39E7,  // gris oscuro
    0x18E3,  // gris muy oscuro
};

// Dibuja un pixel en el framebuffer (con clipping)
static inline void fb_pixel(uint16_t *fb, int x, int y, uint16_t color)
{
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT)
        fb[y * SCREEN_WIDTH + x] = color;
}

// Dibuja una línea gruesa desde (x0,y0) a (x1,y1)
static void fb_thick_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t color, int thickness)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    int t = thickness / 2;

    for (;;) {
        for (int tx = -t; tx <= t; tx++)
            for (int ty = -t; ty <= t; ty++)
                fb_pixel(fb, x0 + tx, y0 + ty, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Dibuja el spinner con la barra 'active' iluminada
static void draw_spinner(uint16_t *fb, int active)
{
    for (int i = 0; i < SPINNER_SEGMENTS; i++) {
        float angle = (float)i * 2.0f * 3.14159f / SPINNER_SEGMENTS - 3.14159f / 2.0f;
        int x0 = SPINNER_CX + (int)(SPINNER_R1 * cosf(angle));
        int y0 = SPINNER_CY + (int)(SPINNER_R1 * sinf(angle));
        int x1 = SPINNER_CX + (int)(SPINNER_R2 * cosf(angle));
        int y1 = SPINNER_CY + (int)(SPINNER_R2 * sinf(angle));

        int color_idx = (i - active + SPINNER_SEGMENTS) % SPINNER_SEGMENTS;
        fb_thick_line(fb, x0, y0, x1, y1, SPINNER_COLORS[color_idx], 6);
    }
}

static volatile int g_spinner_frame = 0;  // frame actual del spinner

// ── Estado compartido ────────────────────────────────────────────────────
static uint16_t *g_fb[2]     = { nullptr, nullptr };  // los 2 framebuffers del panel
static int       g_fbPage[2] = { 0, 0 };              // que pagina tiene cada FB (0=ninguna)
static int       g_shownFb   = 0;                     // indice del FB mostrado
static uint16_t *g_scratch   = nullptr;               // buffer de decodificacion (offscreen)
static uint32_t  g_fetched[MAX_PAGE_ID + 1] = { 0 };  // millis del ultimo fetch por pagina (1..MAX_PAGE_ID)

static volatile int g_page  = 1;   // pagina deseada (la cambia el touch)
static volatile int g_shown = 0;   // pagina mostrada actualmente
static volatile bool g_blackout = false;  // true si ya se oscureció desde touch
static volatile bool g_info_request = false;  // toque largo: mostrar pantalla de info
static SemaphoreHandle_t g_wake;   // despierta al netTask (tap o arranque)
static SemaphoreHandle_t g_i2c;    // serializa el bus I2C (touch core1 + BME core0)

static LocalSensorData g_local;

// ── Brillo: atenuacion por software ─────────────────────────────────────────
// El backlight de esta placa cuelga del expansor CH422G (IO2), que es digital:
// solo enciende/apaga, no hay PWM. Asi que el brillo se aplica escalando los
// pixeles al copiarlos al framebuffer, con dos LUTs (los canales de RGB565 son
// de 5 y 6 bits). En el nivel 10 no se toca nada: memcpy directo, costo cero.
static uint8_t  g_dim_level = 10;          // 1..10 (10 = 100%)
static uint16_t g_dim5[32], g_dim6[64];

static void build_dim_luts(uint8_t level)
{
    for (int i = 0; i < 32; i++) g_dim5[i] = (uint16_t)((i * level + 5) / 10);
    for (int i = 0; i < 64; i++) g_dim6[i] = (uint16_t)((i * level + 5) / 10);
    g_dim_level = level;
}

// Copia `px` pixeles aplicando el brillo actual.
static inline void dim_copy(uint16_t *dst, const uint16_t *src, size_t px)
{
    if (g_dim_level >= 10) { memcpy(dst, src, px * 2); return; }
    for (size_t i = 0; i < px; i++) {
        uint16_t p = src[i];
        dst[i] = (uint16_t)((g_dim5[(p >> 11) & 0x1F] << 11) |
                            (g_dim6[(p >>  5) & 0x3F] <<  5) |
                             g_dim5[ p        & 0x1F]);
    }
}

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
    const size_t row_px = (size_t)SCREEN_WIDTH;

    for (int y = 0; y < SCREEN_HEIGHT; y += CHUNK_ROWS) {
        int rows = min(CHUNK_ROWS, SCREEN_HEIGHT - y);
        size_t off_px = (size_t)y * row_px, n_px = (size_t)rows * row_px;
        dim_copy(g_fb[fbIdx] + off_px, g_scratch + off_px, n_px);
        waveshare_fb_flush(g_fb[fbIdx] + off_px, n_px * 2);
        delayMicroseconds(200);
    }
    g_fbPage[fbIdx]  = page;
    g_fetched[page]  = millis();
    return true;
}

// ── Verifica si una página está en caché ────────────────────────────────────
static bool page_is_cached(int page)
{
    for (int i = 0; i < 2; i++)
        if (g_fbPage[i] == page) return true;
    return false;
}

// ── Muestra pantalla negra con spinner (llamar desde touch) ─────────────────
static void blackout_with_spinner()
{
    int back = g_shownFb ^ 1;
    memset(g_fb[back], 0, FB_BYTES);
    g_spinner_frame = 0;
    draw_spinner(g_fb[back], g_spinner_frame);
    waveshare_fb_flush(g_fb[back], FB_BYTES);
    waveshare_wait_vsync(50);
    waveshare_swap_fb(g_fb[back]);
    g_shownFb = back;
    g_blackout = true;
}

// ── Avanza el spinner un frame ──────────────────────────────────────────────
static void spinner_tick()
{
    int back = g_shownFb ^ 1;
    memset(g_fb[back], 0, FB_BYTES);
    g_spinner_frame = (g_spinner_frame + 1) % SPINNER_SEGMENTS;
    draw_spinner(g_fb[back], g_spinner_frame);
    waveshare_fb_flush(g_fb[back], FB_BYTES);
    waveshare_wait_vsync(50);
    waveshare_swap_fb(g_fb[back]);
    g_shownFb = back;
}

// ── Muestra una pagina ──────────────────────────────────────────────────────
static void show(int page)
{
    if (page < 1 || page > MAX_PAGE_ID) return;

    // ¿Ya esta cargada en algun framebuffer? -> swap PURO (transicion limpia).
    for (int i = 0; i < 2; i++) {
        if (g_fbPage[i] == page) {
            waveshare_swap_fb(g_fb[i]);
            g_shownFb = i;
            g_shown = page;
            g_blackout = false;
            return;
        }
    }

    // No cacheada: mostrar spinner si no se hizo desde touch, luego cargar.
    if (!g_blackout) {
        int back = g_shownFb ^ 1;
        memset(g_fb[back], 0, FB_BYTES);
        g_spinner_frame = 0;
        draw_spinner(g_fb[back], g_spinner_frame);
        waveshare_fb_flush(g_fb[back], FB_BYTES);
        waveshare_wait_vsync(50);
        waveshare_swap_fb(g_fb[back]);
        g_shownFb = back;
    }
    g_blackout = false;

    // Animar spinner mientras carga
    spinner_tick();

    // Cargar la página en el FB de atrás y mostrar
    int back = g_shownFb ^ 1;
    if (load_into(back, page)) {
        waveshare_wait_vsync(50);
        waveshare_swap_fb(g_fb[back]);
        g_shownFb = back;
        g_shown = page;
    }
}

// ── Cambio de ajustes de pantalla desde el portal web ───────────────────────
// Brillo o intervalo nuevos (y el boton "forzar refresco"): las paginas en
// cache se pintaron con el brillo anterior, asi que se invalidan y se recargan.
static void on_display_settings_changed()
{
    build_dim_luts(g_set.brightness);
    for (int i = 0; i < 2; i++) g_fbPage[i] = 0;
    for (int i = 0; i <= MAX_PAGE_ID; i++) g_fetched[i] = 0;
    g_shown = 0;                       // fuerza a netTask a recargar g_page
    xSemaphoreGive(g_wake);
}

// ── Task de red (core 0) ────────────────────────────────────────────────────
static void netTask(void *)
{
    net_begin();
    // WiFi ya conectado por wifi_config_begin() en setup().

    uint32_t last_bme = 0;

    for (;;) {
        xSemaphoreTake(g_wake, pdMS_TO_TICKS(500));

        // OTA en curso: no bajar ni pintar nada. Escribir la flash mientras este
        // core mueve un JPEG por el bus de PSRAM solo alarga la subida.
        if (g_ota_active) {
            delay(200);
            continue;
        }

        // Toque largo: pantalla de info con la IP y la URL del portal. Se dibuja
        // AQUI (core 0) y no en el loop del touch, para que solo una tarea toque
        // los framebuffers. Se queda 15 s o hasta que se toque una pestaña.
        if (g_info_request) {
            g_info_request = false;
            int back = g_shownFb ^ 1;
            info_screen_show(g_fb[back], WiFi.SSID().c_str(),
                             WiFi.localIP().toString().c_str(), WiFi.RSSI());
            g_fbPage[back] = 0;          // ese FB ya no tiene una pagina valida
            g_shownFb = back;
            uint32_t t0 = millis();
            while (millis() - t0 < 15000 && g_page == g_shown) delay(50);
            g_shown = 0;                 // forzar recarga de la pagina al salir
            continue;
        }

        int page = g_page;

        // Cambio de pagina (tap en una pestaña).
        if (page != g_shown) show(page);

        // Refresco de datos de la pagina mostrada, si esta vieja: escribe al FB
        // de atras y hace swap (true double buffering -> sin tearing ni rayitas).
        // El intervalo es configurable en el portal (1..15 min).
        uint32_t now = millis();
        if (g_shown >= 1 &&
            (g_fetched[g_shown] == 0 || now - g_fetched[g_shown] >= settings_update_interval_ms())) {
            int back = g_shownFb ^ 1;
            if (load_into(back, g_shown)) {
                waveshare_wait_vsync(50);
                waveshare_swap_fb(g_fb[back]);
                g_shownFb = back;
            }
        }
        g_status.page = g_shown;

        // BME280: leer y enviar cada g_set.bme_interval segundos. La LECTURA toma
        // el mutex de I2C (el bus lo comparte con el touch del core 1). Tanto el
        // habilitado como el intervalo se cambian en caliente desde el portal.
        if (g_set.bme_enabled && isBME280Available() &&
            (last_bme == 0 || now - last_bme >= (uint32_t)g_set.bme_interval * 1000UL)) {
            bool ok;
            xSemaphoreTake(g_i2c, portMAX_DELAY);
            ok = readBME280(g_local) && g_local.valid;
            xSemaphoreGive(g_i2c);
            if (ok) net_post_local(g_local.temperature, g_local.humidity, g_local.pressure);
            last_bme = now;
        }
    }
}

// Callback para mostrar pantalla de modo AP.
static void on_ap_mode(const char *ap_name, const char *ip)
{
    Serial.printf("[wifi] modo AP: %s en %s\n", ap_name, ip);
    if (g_fb[0]) {
        ap_screen_show(g_fb[0], ap_name, ip);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] Ecowitt Display Kiosk (XE1E)");

    // I2C compartido (CH422G, GT911, BME280) en 8/9.
    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);

    // Panel RGB + sus dos framebuffers (antes de WiFi para mostrar pantalla de AP).
    waveshare_esp32_s3_rgb_lcd_init();
    waveshare_get_frame_buffer((void **)&g_fb[0], (void **)&g_fb[1]);
    for (int i = 0; i < 2; i++)
        if (g_fb[i]) memset(g_fb[i], 0, FB_BYTES);
    g_shownFb = 0;

    // Ajustes guardados (NVS) + LUTs de brillo, antes de pintar cualquier página.
    settings_load();
    build_dim_luts(g_set.brightness);

    // Registrar callback para mostrar pantalla cuando entre en modo AP.
    wifi_config_set_ap_callback(on_ap_mode);

    // WiFi: intenta conectar a las redes guardadas, o abre el portal de
    // configuración (modo AP). Bloquea hasta tener conexión; si entra en modo AP
    // llama al callback para pintar las instrucciones en la pantalla. Al volver,
    // el portal ya escucha en la IP de la LAN.
    wifi_config_begin();

    // Buffer de decodificacion (offscreen) en PSRAM.
    g_scratch = (uint16_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!g_scratch) Serial.println("[boot] ERROR: sin PSRAM para el buffer de decode");

    // Touch + BME280. El sensor se inicializa siempre (asi el portal puede
    // reportar si responde); que se ENVIE al servidor depende de g_set.bme_enabled.
    touch_input_begin();
    initBME280(BME280_I2C_ADDR);
    setBME280Altitude((float)g_set.altitude);
    setBME280TemperatureOffset(g_set.off_temp);
    setBME280HumidityOffset(g_set.off_hum);
    setBME280PressureOffset(g_set.off_press);

    // Mutex de I2C + task de red en el core 0; el loop() (touch) corre en el core 1.
    g_i2c  = xSemaphoreCreateMutex();
    g_wake = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);

    // El portal puede cambiar brillo/intervalo en caliente. Se registra DESPUES
    // de crear g_wake porque el callback lo usa para despertar a netTask.
    portal_set_display_callback(on_display_settings_changed);

    xSemaphoreGive(g_wake);   // primer render inmediato
}

// Toque largo: millis del inicio de un toque fuera de la barra (0 = ninguno).
static uint32_t g_press_start = 0;
static const uint32_t LONG_PRESS_MS = 2500;

void loop()
{
    // Sondeo del touch (bajo el mutex de I2C). Un toque en la franja inferior
    // (barra de pestañas) salta a la pagina correspondiente.
    uint16_t tx = 0, ty = 0;
    xSemaphoreTake(g_i2c, portMAX_DELAY);
    bool tapped = touch_input_tapped(&tx, &ty);
    xSemaphoreGive(g_i2c);

    if (tapped) {
        int new_page = 0;

        if (g_shown == PAGE_CONSOLA) {
            // Consola (full-screen, sin barra): un toque en CUALQUIER parte regresa
            // a la página 1 (principal).
            Serial.printf("[touch] consola x=%u y=%u -> pagina 1\n", tx, ty);
            new_page = 1;
        } else if (ty >= TAB_HIT_TOP) {
            // Barra de NUM_TABS pestañas: las 5 numeradas + "Consola" (la última).
            int idx = (int)((uint32_t)tx * NUM_TABS / SCREEN_WIDTH);   // 0..NUM_TABS-1
            if (idx < 0) idx = 0;
            if (idx >= NUM_TABS) idx = NUM_TABS - 1;
            new_page = (idx == NUM_TABS - 1) ? PAGE_CONSOLA : (idx + 1);
            Serial.printf("[touch] tab x=%u y=%u -> pagina %d\n", tx, ty, new_page);
        } else {
            // Fuera de la barra: no cambia de página, pero si se sostiene se
            // convierte en toque largo (ver abajo).
            Serial.printf("[touch] x=%u y=%u (fuera de la barra)\n", tx, ty);
            g_press_start = millis();
        }

        // Si hay cambio de página, oscurecer inmediatamente si no está en caché
        if (new_page > 0 && new_page != g_page) {
            if (!page_is_cached(new_page)) {
                blackout_with_spinner();  // spinner instantáneo desde el touch
            }
            g_page = new_page;
            xSemaphoreGive(g_wake);
        }
    }

    // Toque largo (>= 2.5 s) fuera de la barra: pantalla con la IP y la URL del
    // portal. Sin esto no hay forma de averiguar la IP del display (y por tanto
    // de llegar a la configuración) sin entrar al router.
    if (g_press_start) {
        if (!touch_input_down()) {
            g_press_start = 0;                      // se soltó antes: era un tap
        } else if (millis() - g_press_start >= LONG_PRESS_MS) {
            g_press_start = 0;
            Serial.println("[touch] toque largo -> pantalla de info");
            g_info_request = true;
            xSemaphoreGive(g_wake);
        }
    }

    // Atender la página de configuración (puerto 80 en la IP de la LAN).
    portal_handle();
    delay(5);
}
