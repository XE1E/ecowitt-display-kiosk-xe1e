/**
 * status.h - Estado de ejecucion del kiosco (solo para diagnostico).
 *
 * Lo escriben net.h (resultado del ultimo GET/POST) y main.cpp (pagina actual);
 * lo lee el bloque "Estado" del portal (portal.h). Todo volatile porque se
 * escribe en el core 0 (netTask) y se lee en el core 1 (loop/portal).
 */

#ifndef STATUS_H
#define STATUS_H

#include <Arduino.h>

struct KioskStatus {
    volatile int      last_http;      // codigo HTTP del ultimo GET (0 = todavia nada, <0 = error de cliente)
    volatile uint32_t last_ms;        // cuanto tardo el ultimo GET, en ms
    volatile uint32_t last_bytes;     // tamaño del ultimo JPEG bajado
    volatile uint32_t last_ok_at;     // millis del ultimo GET correcto
    volatile int      post_http;      // codigo HTTP del ultimo POST del BME280
    // Slug de la pagina mostrada ("consola", "det-rain-7d"...). Era un int cuando
    // las paginas se numeraban; ahora las nombra el servidor. No es volatile: una
    // cadena no se copia atomicamente y de todos modos esto es solo diagnostico
    // --lo peor que puede pasar es que el portal lea un nombre a medio escribir--.
    char              page[24];
    // Desglose de la ultima carga de pagina, para saber donde se va el tiempo que
    // dura el spinner: red (last_ms) + decode del JPEG + flush al panel.
    volatile uint32_t decode_ms;      // JPEGDEC sobre el framebuffer
    volatile uint32_t paint_ms;       // flush cache->PSRAM por trozos
    volatile uint32_t load_ms;        // total: fetch + decode + flush
};

static KioskStatus g_status = { 0, 0, 0, 0, 0, "", 0, 0, 0 };

// Actualizacion OTA en curso: lo pone portal.h mientras recibe el .bin y lo lee
// netTask para no bajar ni pintar paginas durante la escritura de la flash.
static volatile bool g_ota_active = false;

#endif // STATUS_H
