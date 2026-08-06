/**
 * net.h - HTTP para el display kiosco.
 *
 *  - GET del JPEG del display: /api/display.jpg?page=N  -> buffer en PSRAM.
 *  - POST del BME280 local:    /api/kiosk/local          (JSON).
 *
 * La conexión WiFi la maneja wifi_config.h (portal propio + NVS). El resultado
 * de la última petición se publica en g_status (status.h) para el bloque
 * "Estado" del portal web.
 *
 * Se baja por HTTP (no HTTPS): el handshake TLS en el ESP32 tarda ~1-2s por
 * peticion y hacia lentisimo el cambio de pagina. La imagen es publica y no hay
 * secretos, asi que HTTP directo (puerto 8080 por IP) es ideal: ~40ms.
 */

#ifndef NET_H
#define NET_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "status.h"
#include "wifi_config.h"

// Buffer en PSRAM para el JPEG descargado (se reserva 1 vez en net_begin).
static uint8_t *_img_buf = nullptr;
static const size_t IMG_BUF_MAX = 256 * 1024;   // margen de sobra para ~50KB

inline void net_begin()
{
    if (!_img_buf) {
        _img_buf = (uint8_t *)heap_caps_malloc(IMG_BUF_MAX, MALLOC_CAP_SPIRAM);
        if (!_img_buf) Serial.println("[net] ERROR: sin PSRAM para el buffer de imagen");
    }
}

/**
 * Descarga /api/display.jpg?page=<slug> al buffer interno.
 *
 * @param page     slug de la pagina ("consola", "det-rain-7d", "3"...)
 * @param out      se apunta al buffer interno con el JPEG
 * @param out_len  longitud descargada
 * @param nav      (opcional) recibe la cabecera X-Kiosk-Nav, o cadena vacia si el
 *                 servidor no la manda
 * @param nav_max  tamaño del buffer de `nav`
 * @return true si HTTP 200 y cabe en el buffer.
 *
 * La pagina es una CADENA y no un numero desde que el servidor manda el arbol de
 * navegacion: los slugs son "det-wind-7d" o "stats-mes". Mantener aqui una tabla de
 * ids seria volver a tener dos listas que sincronizar entre los dos repos, que es
 * justo lo que se quito.
 */
inline bool net_fetch_display(const char *page, const uint8_t **out, size_t *out_len,
                              char *nav = nullptr, size_t nav_max = 0)
{
    if (nav && nav_max) nav[0] = '\0';
    if (!_img_buf) return false;
    if (WiFi.status() != WL_CONNECTED && !wifi_config_reconnect()) {
        g_status.last_http = -1;      // sin WiFi
        g_status.last_ms = 0;
        return false;
    }

    uint32_t t0 = millis();
    WiFiClient client;

    HTTPClient http;
    String url = String(wifi_config_get_api_url()) + "/api/display.jpg?page=" + String(page);
    if (!http.begin(client, url)) {
        Serial.println("[net] http.begin fallo");
        g_status.last_http = -2;      // URL invalida
        g_status.last_ms = 0;
        return false;
    }
    http.setTimeout(15000);

    // HTTPClient DESCARTA las cabeceras de respuesta salvo las que se pidan antes
    // del GET. Aqui llega el mapa de zonas tactiles de esta pagina.
    static const char *CABECERAS[] = { "X-Kiosk-Nav" };
    http.collectHeaders(CABECERAS, 1);

    int code = http.GET();
    g_status.last_http = code;
    if (code != HTTP_CODE_OK) {
        Serial.printf("[net] GET %s -> %d\n", url.c_str(), code);
        http.end();
        g_status.last_ms = millis() - t0;
        return false;
    }

    // El mapa de zonas se copia ANTES de leer el cuerpo: http.end() libera las
    // cabeceras y despues ya no se puede consultar.
    if (nav && nav_max) {
        String h = http.header("X-Kiosk-Nav");
        strlcpy(nav, h.c_str(), nav_max);
        if (h.length() >= nav_max) {
            // Truncada: mejor quedarse sin zonas que con media lista, porque la
            // ultima quedaria a medias y podria mandar a una pagina inexistente.
            Serial.printf("[net] X-Kiosk-Nav de %u bytes no cabe en %u\n",
                          (unsigned)h.length(), (unsigned)nav_max);
            nav[0] = '\0';
        }
    }

    int len = http.getSize();               // puede ser -1 (chunked)
    WiFiClient *stream = http.getStreamPtr();
    size_t total = 0;
    bool   overflow = false;
    uint32_t last = millis();
    while (http.connected() && (len < 0 || total < (size_t)len)) {
        size_t avail = stream->available();
        if (avail) {
            if (total + avail > IMG_BUF_MAX) {
                Serial.println("[net] imagen mas grande que el buffer");
                overflow = true;
                break;
            }
            int r = stream->readBytes(_img_buf + total, avail);
            total += r;
            last = millis();
        } else if (millis() - last > 5000) {
            Serial.println("[net] timeout leyendo el cuerpo");
            break;                           // timeout de lectura
        } else {
            delay(2);
        }
    }
    http.end();

    g_status.last_ms    = millis() - t0;
    g_status.last_bytes = total;
    if (overflow || total == 0) return false;
    // Descarga incompleta (se corto la conexion o expiro el timeout de lectura):
    // el JPEG truncado o decodifica a medias o falla. Se descarta y se reintenta,
    // asi nunca se pinta media pagina.
    if (len > 0 && total != (size_t)len) {
        Serial.printf("[net] descarga incompleta: %u de %d bytes\n", (unsigned)total, len);
        return false;
    }
    g_status.last_ok_at = millis();
    *out = _img_buf;
    *out_len = total;
    Serial.printf("[net] display.jpg page=%s: %u bytes%s\n", page, (unsigned)total,
                  (nav && nav[0]) ? " (con zonas)" : "");
    return true;
}

/**
 * POST del BME280 local a /api/kiosk/local.
 */
inline bool net_post_local(float temperature, float humidity, float pressure)
{
    if (WiFi.status() != WL_CONNECTED && !wifi_config_reconnect()) return false;

    WiFiClient client;

    HTTPClient http;
    String url = String(wifi_config_get_api_url()) + "/api/kiosk/local";
    if (!http.begin(client, url)) return false;
    http.addHeader("Content-Type", "application/json");

    char body[128];
    snprintf(body, sizeof(body),
             "{\"temperature\":%.1f,\"humidity\":%.1f,\"pressure\":%.1f}",
             temperature, humidity, pressure);

    int code = http.POST((uint8_t *)body, strlen(body));
    http.end();
    g_status.post_http = code;
    if (code != HTTP_CODE_OK) {
        Serial.printf("[net] POST kiosk/local -> %d\n", code);
        return false;
    }
    return true;
}

#endif // NET_H
