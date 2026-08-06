/**
 * Ecowitt Display Kiosk (XE1E) - firmware ESP32-S3.
 *
 * Enfoque "display tonto": el SERVIDOR renderiza cada pagina del kiosco como un
 * JPEG 1024x600 (/api/display.jpg?page=N) y el ESP32 solo la baja, la decodifica
 * y la pinta. Ver docs/ARQUITECTURA.md.
 *
 * Navegacion POR ZONAS: con cada JPEG llega la cabecera X-Kiosk-Nav con los
 * rectangulos tocables de esa pantalla y a donde lleva cada uno (src/nav.h). El
 * firmware no sabe que paginas existen: solo mira en que rectangulo cayo el toque, y
 * si no cayo en ninguno retrocede. Antes el numero de pestañas estaba cableado aqui
 * y tenia que coincidir con el dashboard, asi que cada pantalla nueva del servidor
 * obligaba a reflashear. Queda como RESPALDO el reparto por la X de la barra, por si
 * una respuesta llega sin mapa.
 *
 * Framebuffers: el panel tiene 2. Se usan como cache de las 2 ultimas paginas
 * distintas mostradas. Volver a una pagina que sigue en un FB = swap PURO (sin
 * escribir PSRAM -> sin contencion -> transicion 100% limpia). Ir a una pagina
 * nueva la decodifica en el FB de atras. El swap espera vsync para alinearse con
 * el inicio del barrido -> sin tearing ni desplazamiento. El refresco de datos
 * tambien escribe al FB de atras y hace swap (true double buffering).
 * g_fbKey[] es la contabilidad de esa cache: lo que pinte algo que no sea una
 * pagina (spinner, info) tiene que invalidar su entrada. Ver ARQUITECTURA.md.
 *
 * Tareas: core 1 (loop) sondea el touch; core 0 (netTask) baja/decodifica/pinta
 * y envia el BME280.
 *
 * Hardware: Waveshare ESP32-S3-Touch-LCD-7B (8MB PSRAM OPI, 16MB flash).
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
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
#include "nav.h"
#include "net.h"

// El display ARRANCA en PAGE_HOME (= "consola"), que es la cara principal de la
// estacion y ademas el indice: cada celda suya lleva al detalle de esa variable.

// Franja de la barra de pestañas, SOLO para el respaldo sin mapa de zonas: un toque
// ahi selecciona pestaña segun la X. Es mas alta que la barra dibujada (64 px) para
// no fallar el toque. Con mapa, las zonas que manda el servidor mandan sobre esto.
static const int TAB_HIT_TOP = SCREEN_HEIGHT - 110;   // y >= 490 = barra

static const size_t FB_BYTES = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2;  // RGB565

// ── Spinner de carga ─────────────────────────────────────────────────────
static const int SPINNER_CX = SCREEN_WIDTH / 2;   // centro X
static const int SPINNER_CY = SCREEN_HEIGHT / 2;  // centro Y
static const int SPINNER_R1 = 30;                 // radio interno
static const int SPINNER_R2 = 50;                 // radio externo
static const int SPINNER_SEGMENTS = 8;            // número de barras
// Medio lado de la caja que ocupa el spinner (radio externo + el grosor de la
// barra). Solo estas filas se repintan al animarlo, en vez de la pantalla entera.
static const int SPINNER_BOX = SPINNER_R2 + 6;

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
        float angle = (float)i * 2.0f * (float)M_PI / SPINNER_SEGMENTS - (float)M_PI / 2.0f;
        int x0 = SPINNER_CX + (int)(SPINNER_R1 * cosf(angle));
        int y0 = SPINNER_CY + (int)(SPINNER_R1 * sinf(angle));
        int x1 = SPINNER_CX + (int)(SPINNER_R2 * cosf(angle));
        int y1 = SPINNER_CY + (int)(SPINNER_R2 * sinf(angle));

        int color_idx = (i - active + SPINNER_SEGMENTS) % SPINNER_SEGMENTS;
        fb_thick_line(fb, x0, y0, x1, y1, SPINNER_COLORS[color_idx], 6);
    }
}

static int g_spinner_frame = 0;  // frame actual del spinner (solo bajo g_fbmux)

// ── Estado compartido ────────────────────────────────────────────────────
// Contenido de cada framebuffer: el HASH del slug que tiene listo para "swap puro",
// o uno de estos dos marcadores. TODO lo que pinte algo que no sea una pagina DEBE
// dejar la entrada en FB_INVALID/FB_SPINNER: si no, el swap puro presentaria ese
// contenido creyendo que es la pagina (y se quedaria asi hasta el refresco por
// intervalo).
//
// Antes era un id numerico de pagina; ahora las paginas son slugs y se guarda su
// hash (ver nav_hash, que evita chocar con estos dos valores).
static const int32_t FB_INVALID =  0;   // contenido que no corresponde a ninguna pagina
static const int32_t FB_SPINNER = -1;   // negro + spinner de carga

// g_fbKey/g_shownFb los escribe el netTask y, para el spinner del toque, el
// loop(): volatile + g_fbmux (el mutex da la exclusion; volatile evita que el
// compilador se quede con una copia en registro).
static uint16_t *g_fb[2]     = { nullptr, nullptr };  // los 2 framebuffers del panel
static volatile int32_t g_fbKey[2] = { FB_INVALID, FB_INVALID };  // que tiene cada FB
static volatile int g_shownFb   = 0;                  // indice del FB mostrado
// Instante del fetch y zonas de lo que hay en cada framebuffer.
//
// La antiguedad iba en una tabla indexada por numero de pagina; con slugs esa tabla
// no se puede dimensionar --el arbol lo decide el servidor-- y tampoco hace falta:
// lo unico que se refresca por intervalo es lo que hay en un framebuffer, y solo hay
// dos. Las zonas van igual, para que volver a una pagina cacheada (swap puro, sin
// descarga) recupere tambien sus rectangulos.
static uint32_t g_fbAt[2]    = { 0, 0 };
static NavMapa  g_fbNav[2];

static char g_page[NAV_SLUG_MAX]  = PAGE_HOME;  // pagina deseada (la cambia el touch)
static char g_shown[NAV_SLUG_MAX] = "";         // pagina mostrada actualmente
static NavPila g_pila;                          // por donde se llego hasta aqui
// Serializa g_page/g_shown/g_pila entre el loop (touch, core 1) y netTask (core 0).
// Con ids numericos bastaba `volatile`, que era atomico; una cadena no lo es.
static SemaphoreHandle_t g_navmux;
static volatile bool g_info_request = false;  // toque largo: mostrar pantalla de info
static volatile bool g_info_showing = false;  // pantalla de info en pantalla (la cierra un toque)
static SemaphoreHandle_t g_wake;   // despierta al netTask (tap o arranque)
static SemaphoreHandle_t g_i2c;    // serializa el bus I2C (touch core1 + BME core0)
static SemaphoreHandle_t g_fbmux;  // serializa quien dibuja en los framebuffers

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

// ── Baja + decodifica la pagina DIRECTO sobre el framebuffer indicado ───────
// Sin buffer intermedio: JPEGDEC escribe cada bloque en el framebuffer de atras y
// dim_copy le aplica el brillo en la misma pasada. Antes se decodificaba a un
// scratch en PSRAM y se copiaba: 1.2 MB de lectura + 1.2 MB de escritura extra por
// refresco, mas 1.2 MB de PSRAM reservados.
//
// El FB de atras NO se muestra, asi que verlo llenarse no es un problema; pero
// mientras se decodifica ya no tiene la pagina que tenia -> se invalida ANTES de
// tocarlo, y solo se vuelve a marcar si el decode termina bien.
//
// El flush cache->PSRAM se sigue haciendo por trozos con micro-pausa: es lo que
// reparte el trafico de bus para no dejar sin datos al DMA del panel (que es la
// causa de las rayitas, ver docs/DISPLAY_ISSUES.md).
static bool load_into(int fbIdx, const char *page)
{
    const uint8_t *jpg = nullptr;
    size_t jpg_len = 0;
    char nav[NAV_HDR_MAX];
    uint32_t t0 = millis();
    if (!net_fetch_display(page, &jpg, &jpg_len, nav, sizeof(nav))) return false;

    uint32_t t_dec = millis();
    g_fbKey[fbIdx] = FB_INVALID;
    if (!jpeg_decode_to_fb(jpg, jpg_len, g_fb[fbIdx], dim_copy)) return false;

    uint32_t t_paint = millis();
    const int CHUNK_ROWS = 30;
    for (int y = 0; y < SCREEN_HEIGHT; y += CHUNK_ROWS) {
        int rows = min(CHUNK_ROWS, SCREEN_HEIGHT - y);
        size_t off_px = (size_t)y * SCREEN_WIDTH;
        waveshare_fb_flush(g_fb[fbIdx] + off_px, (size_t)rows * SCREEN_WIDTH * 2);
        delayMicroseconds(200);
    }
    uint32_t t_end = millis();

    // Desglose de la carga (lo muestra el portal): asi se ve si el tiempo que dura
    // el spinner se va en la red, en el decode o en el flush al panel.
    g_status.decode_ms = t_paint - t_dec;
    g_status.paint_ms  = t_end - t_paint;
    g_status.load_ms   = t_end - t0;

    // Las zonas se guardan JUNTO a la imagen, no en una variable global suelta: al
    // volver a esta pagina con un swap puro no hay descarga, asi que sus
    // rectangulos tienen que venir de aqui o el toque quedaria resolviendose contra
    // el mapa de otra pantalla.
    nav_parse(nav, g_fbNav[fbIdx]);
    g_fbKey[fbIdx] = nav_hash(page);
    g_fbAt[fbIdx]  = millis();
    return true;
}

// ── Verifica si una página está en caché ────────────────────────────────────
static bool page_is_cached(const char *page)
{
    int32_t k = nav_hash(page);
    for (int i = 0; i < 2; i++)
        if (g_fbKey[i] == k) return true;
    return false;
}

// ── Pantalla negra con spinner: la pinta en el FB de atras y la presenta ─────
// Llamar SIEMPRE con g_fbmux tomado. Marca ese FB como FB_SPINNER: ya no tiene
// una pagina que se pueda presentar con un swap puro.
static void spinner_present()
{
    int back = g_shownFb ^ 1;
    memset(g_fb[back], 0, FB_BYTES);
    g_spinner_frame = 0;
    draw_spinner(g_fb[back], g_spinner_frame);
    waveshare_fb_flush(g_fb[back], FB_BYTES);
    g_fbKey[back] = FB_SPINNER;
    // Sin zonas: lo que se ve es un spinner, no una pagina, y sus botones ya no
    // estan ahi. El loop ignora los toques mientras esto este en pantalla.
    nav_clear(g_fbNav[back]);
    waveshare_wait_vsync(WAVESHARE_VSYNC_WAIT_MS);
    waveshare_swap_fb(g_fb[back]);
    g_shownFb = back;
}

// ── Avanza el spinner un frame, EN SITIO ────────────────────────────────────
// Repinta solo la caja del spinner sobre el FB que ya se esta mostrando (que es
// negro entero, asi que basta limpiar sus filas). Sin swap y sin tocar el FB de
// atras: ~220 KB de bus en vez de los 2.4 MB de repintar la pantalla completa en
// el otro framebuffer, y el FB de atras conserva la pagina anterior para el swap
// puro. Requiere que el FB mostrado sea FB_SPINNER.
static void spinner_animate()
{
    uint16_t *fb = g_fb[g_shownFb];
    int y0 = max(0, SPINNER_CY - SPINNER_BOX);
    int y1 = min(SCREEN_HEIGHT, SPINNER_CY + SPINNER_BOX);
    size_t off_px = (size_t)y0 * SCREEN_WIDTH;
    size_t n_px   = (size_t)(y1 - y0) * SCREEN_WIDTH;

    memset(fb + off_px, 0, n_px * 2);
    g_spinner_frame = (g_spinner_frame + 1) % SPINNER_SEGMENTS;
    draw_spinner(fb, g_spinner_frame);
    waveshare_fb_flush(fb + off_px, n_px * 2);
}

/**
 * Escribe la pagina mostrada bajo g_navmux.
 *
 * La escribe netTask (core 0) y la lee el loop del touch (core 1) para apilarla al
 * navegar: sin el mutex, el loop podria leerla a medias y meter un slug partido en
 * la pila de "atras". Anidar g_navmux dentro de g_fbmux es seguro porque el loop
 * nunca hace lo contrario: suelta g_navmux ANTES de pedir g_fbmux.
 */
static void set_shown(const char *page)
{
    xSemaphoreTake(g_navmux, portMAX_DELAY);
    if (page) strlcpy(g_shown, page, sizeof(g_shown));
    else      g_shown[0] = '\0';
    xSemaphoreGive(g_navmux);
}

// ── Muestra una pagina ──────────────────────────────────────────────────────
// Llamar SIEMPRE con g_fbmux tomado.
static void show(const char *page)
{
    if (!page || !*page) return;
    int32_t key = nav_hash(page);

    // ¿Ya esta cargada en algun framebuffer? -> swap PURO (transicion limpia).
    // Tambien aqui hay que esperar el limite de frame: es un swap como cualquier
    // otro y, sin la espera, conmutaba en un punto arbitrario del barrido -> se
    // veia el "brinco" justo en la transicion mas rapida, la que no escribe nada.
    for (int i = 0; i < 2; i++) {
        if (g_fbKey[i] == key) {
            waveshare_wait_vsync(WAVESHARE_VSYNC_WAIT_MS);
            waveshare_swap_fb(g_fb[i]);
            g_shownFb = i;
            set_shown(page);
            return;
        }
    }

    // No cacheada: si el toque ya dejo el spinner en pantalla, solo se avanza un
    // frame (barato); si no, se pinta entero.
    if (g_fbKey[g_shownFb] == FB_SPINNER) spinner_animate();
    else                                  spinner_present();

    // Cargar la página en el FB de atrás y mostrar.
    int back = g_shownFb ^ 1;
    if (load_into(back, page)) {
        waveshare_wait_vsync(WAVESHARE_VSYNC_WAIT_MS);
        waveshare_swap_fb(g_fb[back]);
        g_shownFb = back;
        set_shown(page);
    } else {
        // Fallo la descarga: lo que se muestra es el spinner, no una pagina. Sin
        // esto g_shown seguiria apuntando a la pagina anterior y volver a tocar
        // SU zona no haria nada -> spinner congelado hasta el refresco.
        set_shown(nullptr);
    }
}

// ── Cambio de ajustes de pantalla desde el portal web ───────────────────────
// Brillo o intervalo nuevos (y el boton "forzar refresco"): las paginas en
// cache se pintaron con el brillo anterior, asi que se invalidan y se recargan.
static void on_display_settings_changed()
{
    build_dim_luts(g_set.brightness);
    for (int i = 0; i < 2; i++) { g_fbKey[i] = FB_INVALID; g_fbAt[i] = 0; nav_clear(g_fbNav[i]); }
    set_shown(nullptr);                // fuerza a netTask a recargar g_page
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
        // los framebuffers. Se queda 15 s o hasta que se toque la pantalla.
        if (g_info_request) {
            g_info_request = false;
            xSemaphoreTake(g_fbmux, portMAX_DELAY);
            int back = g_shownFb ^ 1;
            info_screen_show(g_fb[back], WiFi.SSID().c_str(),
                             WiFi.localIP().toString().c_str(), WiFi.RSSI());
            g_fbKey[back] = FB_INVALID;    // ese FB ya no tiene una pagina valida
            nav_clear(g_fbNav[back]);      // ni sus zonas
            g_shownFb = back;
            xSemaphoreGive(g_fbmux);

            g_info_showing = true;         // cualquier toque la baja (lo hace loop())
            uint32_t t0 = millis();
            while (millis() - t0 < 15000 && g_info_showing && !strcmp(g_page, g_shown))
                delay(50);
            g_info_showing = false;
            set_shown(nullptr);            // forzar recarga de la pagina al salir
            continue;
        }

        xSemaphoreTake(g_fbmux, portMAX_DELAY);

        // Copia bajo el mutex: el loop puede cambiar g_page a media frase, y una
        // cadena no se lee de un tiron como se leia el int de antes.
        char page[NAV_SLUG_MAX];
        xSemaphoreTake(g_navmux, portMAX_DELAY);
        strlcpy(page, g_page, sizeof(page));
        xSemaphoreGive(g_navmux);

        // Cambio de pagina (tap en una zona).
        if (strcmp(page, g_shown)) show(page);

        // Refresco de datos de la pagina mostrada, si esta vieja: escribe al FB
        // de atras y hace swap (true double buffering -> sin tearing ni rayitas).
        // El intervalo es configurable en el portal (1..15 min).
        uint32_t now = millis();
        if (g_shown[0] &&
            (g_fbAt[g_shownFb] == 0 || now - g_fbAt[g_shownFb] >= settings_update_interval_ms())) {
            int back = g_shownFb ^ 1;
            if (load_into(back, g_shown)) {
                waveshare_wait_vsync(WAVESHARE_VSYNC_WAIT_MS);
                waveshare_swap_fb(g_fb[back]);
                g_shownFb = back;
            }
        }
        strlcpy(g_status.page, g_shown, sizeof(g_status.page));

        xSemaphoreGive(g_fbmux);

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

    // Touch + BME280. El sensor se inicializa siempre (asi el portal puede
    // reportar si responde); que se ENVIE al servidor depende de g_set.bme_enabled.
    touch_input_begin();
    initBME280(BME280_I2C_ADDR);
    setBME280Altitude((float)g_set.altitude);
    setBME280TemperatureOffset(g_set.off_temp);
    setBME280HumidityOffset(g_set.off_hum);
    setBME280PressureOffset(g_set.off_press);

    // Mutex de I2C y de framebuffers + task de red en el core 0; el loop() (touch)
    // corre en el core 1. Los dos se crean ANTES del netTask: ambos los usa.
    g_i2c   = xSemaphoreCreateMutex();
    g_fbmux = xSemaphoreCreateMutex();
    g_navmux = xSemaphoreCreateMutex();
    g_wake  = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);

    // El portal puede cambiar brillo/intervalo en caliente. Se registra DESPUES
    // de crear g_wake porque el callback lo usa para despertar a netTask.
    portal_set_display_callback(on_display_settings_changed);

    xSemaphoreGive(g_wake);   // primer render inmediato
}

// Toque largo: millis del inicio de un toque fuera de la barra (0 = ninguno).
static uint32_t g_press_start = 0;
static const uint32_t LONG_PRESS_MS = 2500;

// Ultimo toque, para la vuelta automatica a la home por inactividad.
static uint32_t g_last_touch = 0;

void loop()
{
    // Sondeo del touch (bajo el mutex de I2C). Un toque en la franja inferior
    // (barra de pestañas) salta a la pagina correspondiente.
    uint16_t tx = 0, ty = 0;
    xSemaphoreTake(g_i2c, portMAX_DELAY);
    bool tapped = touch_input_tapped(&tx, &ty);
    xSemaphoreGive(g_i2c);

    if (tapped) {
        char destino[NAV_SLUG_MAX] = "";
        g_last_touch = millis();

        // Pantalla de info en pantalla: cualquier toque la baja (es lo que dice
        // su pie) y no hace nada mas.
        if (g_info_showing) { g_info_showing = false; return; }

        // Cargando: lo que se ve es el spinner, no una pagina, asi que sus botones
        // ya no estan donde el dedo cree. Se ignora el toque en vez de resolverlo
        // contra un mapa que no corresponde a lo que hay en pantalla.
        if (g_fbKey[g_shownFb] == FB_SPINNER) return;

        // El mapa de la pagina que se esta VIENDO, no el de la ultima descargada:
        // al volver atras con un swap puro no hay descarga.
        const NavMapa &mapa = g_fbNav[g_shownFb];
        bool con_zonas = mapa.n > 0;

        xSemaphoreTake(g_navmux, portMAX_DELAY);
        if (con_zonas) {
            const char *hit = nav_hit(mapa, tx, ty);
            if (hit) {
                // Toque en un boton: se apila de donde se viene para poder volver.
                nav_push(g_pila, g_shown);
                strlcpy(destino, hit, sizeof(destino));
                Serial.printf("[touch] x=%u y=%u -> %s\n", tx, ty, destino);
            } else {
                // Fuera de toda zona = ATRAS. La pila dice por donde se llego; si
                // esta vacia (arranque directo en una subpagina, o ya en la home)
                // se usa el `back` que declara la propia pagina.
                const char *prev = nav_pop(g_pila);
                if (prev) strlcpy(destino, prev, sizeof(destino));
                else if (mapa.back[0]) strlcpy(destino, mapa.back, sizeof(destino));
                Serial.printf("[touch] x=%u y=%u -> atras: %s\n", tx, ty,
                              destino[0] ? destino : "(nada)");
                // Sin destino no hay nada que hacer, pero puede ser toque largo.
                if (!destino[0]) g_press_start = millis();
            }
        } else if (ty >= TAB_HIT_TOP) {
            // RESPALDO sin mapa de zonas: reparto por la X de la barra de pestañas,
            // como se hacia antes de que el servidor mandara la navegacion. Sirve
            // para no quedarse con un display muerto si un despliegue va a medias o
            // si alguna pantalla no publica zonas.
            int idx = (int)((uint32_t)tx * NUM_TABS / SCREEN_WIDTH);   // 0..NUM_TABS-1
            if (idx < 0) idx = 0;
            if (idx >= NUM_TABS) idx = NUM_TABS - 1;
            if (idx == NUM_TABS - 1) strlcpy(destino, "consola", sizeof(destino));
            else                     snprintf(destino, sizeof(destino), "%d", idx + 1);
            Serial.printf("[touch] tab x=%u y=%u -> %s (sin mapa)\n", tx, ty, destino);
        } else {
            Serial.printf("[touch] x=%u y=%u (sin mapa, fuera de la barra)\n", tx, ty);
            g_press_start = millis();
        }

        bool cambia = destino[0] && strcmp(destino, g_page);
        if (cambia) strlcpy(g_page, destino, sizeof(g_page));
        xSemaphoreGive(g_navmux);

        // Si hay cambio de página, oscurecer inmediatamente si no está en caché.
        // El spinner se pinta AQUI (core 1) para que el toque responda al
        // instante; es la unica excepcion a "solo netTask dibuja", asi que va bajo
        // g_fbmux. Con try-lock: si el netTask esta en medio de una carga no se
        // oscurece (la pagina nueva esta a punto de aparecer igual).
        if (cambia) {
            if (!page_is_cached(destino) && xSemaphoreTake(g_fbmux, 0) == pdTRUE) {
                spinner_present();
                xSemaphoreGive(g_fbmux);
            }
            xSemaphoreGive(g_wake);
        }
    }

    // Vuelta automatica a la home tras un rato sin tocar nada. Un display de pared
    // acaba siempre enseñando la consola: si alguien deja abierto un historico de
    // hace tres meses, lo que se ve desde el sofa deja de ser el clima de ahora.
    // Se limpia la pila: no tendria sentido "volver atras" a donde estabas hace una
    // hora. Con 0 minutos no vuelve nunca (se queda donde lo dejaron).
    if (g_set.idle_home_min && g_last_touch &&
        millis() - g_last_touch >= (uint32_t)g_set.idle_home_min * 60000UL) {
        g_last_touch = 0;
        xSemaphoreTake(g_navmux, portMAX_DELAY);
        bool vuelve = strcmp(g_page, PAGE_HOME) != 0;
        if (vuelve) {
            strlcpy(g_page, PAGE_HOME, sizeof(g_page));
            g_pila.n = 0;
        }
        xSemaphoreGive(g_navmux);
        if (vuelve) {
            Serial.printf("[nav] %u min sin tocar -> %s\n", g_set.idle_home_min, PAGE_HOME);
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
