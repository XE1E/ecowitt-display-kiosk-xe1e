/**
 * net.h - HTTP para el display kiosco.
 *
 *  - GET del JPEG del display: /api/display.jpg?page=N  -> buffer en PSRAM.
 *  - POST del BME280 local:    /api/kiosk/local          (JSON).
 *
 * La conexión WiFi la maneja wifi_config.h (WiFiManager + NVS).
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
 * Descarga /api/display.jpg?page=N al buffer interno.
 * @param page  numero de pagina (1..N)
 * @param out   se apunta al buffer interno con el JPEG
 * @param out_len  longitud descargada
 * @return true si HTTP 200 y cabe en el buffer.
 */
inline bool net_fetch_display(int page, const uint8_t **out, size_t *out_len)
{
    if (!_img_buf) return false;
    if (WiFi.status() != WL_CONNECTED && !wifi_config_reconnect()) return false;

    WiFiClient client;

    HTTPClient http;
    // La consola es una página especial full-screen: URL ?page=consola. El resto
    // son numéricas (?page=N).
    String pageParam = (page == PAGE_CONSOLA) ? String("consola") : String(page);
    String url = String(wifi_config_get_api_url()) + "/api/display.jpg?page=" + pageParam;
    if (!http.begin(client, url)) {
        Serial.println("[net] http.begin fallo");
        return false;
    }
    http.setTimeout(15000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[net] GET %s -> %d\n", url.c_str(), code);
        http.end();
        return false;
    }

    int len = http.getSize();               // puede ser -1 (chunked)
    WiFiClient *stream = http.getStreamPtr();
    size_t total = 0;
    uint32_t last = millis();
    while (http.connected() && (len < 0 || total < (size_t)len)) {
        size_t avail = stream->available();
        if (avail) {
            if (total + avail > IMG_BUF_MAX) {
                Serial.println("[net] imagen mas grande que el buffer");
                http.end();
                return false;
            }
            int r = stream->readBytes(_img_buf + total, avail);
            total += r;
            last = millis();
        } else if (millis() - last > 5000) {
            break;                           // timeout de lectura
        } else {
            delay(2);
        }
    }
    http.end();

    if (total == 0) return false;
    *out = _img_buf;
    *out_len = total;
    Serial.printf("[net] display.jpg page=%d: %u bytes\n", page, (unsigned)total);
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
    if (code != HTTP_CODE_OK) {
        Serial.printf("[net] POST kiosk/local -> %d\n", code);
        return false;
    }
    return true;
}

#endif // NET_H
