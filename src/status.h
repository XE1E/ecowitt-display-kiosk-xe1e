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
    volatile int      page;           // pagina mostrada ahora
};

static KioskStatus g_status = { 0, 0, 0, 0, 0, 0 };

#endif // STATUS_H
