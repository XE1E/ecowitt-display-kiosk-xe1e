/**
 * settings.h - Ajustes persistentes del kiosco (NVS / Preferences).
 *
 * TODO lo configurable vive aqui y se edita desde el portal web (portal.h):
 * 3 redes WiFi, URL del servidor, brillo, intervalo de refresco y el BME280
 * (habilitado, intervalo de envio y offsets de calibracion).
 *
 * my_config.h solo aporta los DEFAULTS del primer arranque; una vez guardado
 * algo en NVS, manda el NVS y sobrevive a los reflasheos.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <Preferences.h>
#include "my_config.h"

// ── Defaults (sobreescribibles desde my_config.h) ────────────────────────────
#ifndef DEFAULT_API_URL
#define DEFAULT_API_URL "http://192.168.1.100:8080"
#endif
#ifndef AP_NAME
#define AP_NAME "EcowittKiosk"
#endif
#ifndef PORTAL_TIMEOUT
#define PORTAL_TIMEOUT 180
#endif
#ifndef DEFAULT_BRIGHTNESS
#define DEFAULT_BRIGHTNESS 10        // 1..10 (10 = 100%)
#endif
#ifndef DEFAULT_UPDATE_MIN
#define DEFAULT_UPDATE_MIN 3         // 1..15 minutos
#endif
#ifndef BME280_ENABLED
#define BME280_ENABLED 1
#endif
#ifndef REMOTE_STATION_INTERVAL
#define REMOTE_STATION_INTERVAL 60   // segundos
#endif
#ifndef BME280_TEMP_OFFSET
#define BME280_TEMP_OFFSET 0.0f
#endif
#ifndef BME280_HUM_OFFSET
#define BME280_HUM_OFFSET 0.0f
#endif
#ifndef BME280_PRESS_OFFSET
#define BME280_PRESS_OFFSET 0.0f
#endif

// ── Namespace y claves NVS (max 15 caracteres por clave) ─────────────────────
static const char *NVS_NAMESPACE = "wificfg";

struct WiFiSlot {
    char ssid[33];
    char pass[65];
};

struct KioskSettings {
    WiFiSlot net[3];
    char     api_url[128];
    uint8_t  brightness;      // 1..10
    uint8_t  update_min;      // 1..15
    bool     bme_enabled;
    uint16_t bme_interval;    // segundos, 60..3600
    float    off_temp;        // grados C
    float    off_hum;         // %
    float    off_press;       // hPa
};

static KioskSettings g_set;
static bool _set_loaded = false;

// ── Clamps (el portal no es la unica via de entrada: tambien el NVS viejo) ────
static inline uint8_t _clamp_u8(long v, uint8_t lo, uint8_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (uint8_t)v;
}

static inline float _clamp_f(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline void settings_load()
{
    if (_set_loaded) return;

    Preferences p;
    p.begin(NVS_NAMESPACE, true);   // solo lectura

    strlcpy(g_set.net[0].ssid, p.getString("ssid1", "").c_str(), sizeof(g_set.net[0].ssid));
    strlcpy(g_set.net[0].pass, p.getString("pass1", "").c_str(), sizeof(g_set.net[0].pass));
    strlcpy(g_set.net[1].ssid, p.getString("ssid2", "").c_str(), sizeof(g_set.net[1].ssid));
    strlcpy(g_set.net[1].pass, p.getString("pass2", "").c_str(), sizeof(g_set.net[1].pass));
    strlcpy(g_set.net[2].ssid, p.getString("ssid3", "").c_str(), sizeof(g_set.net[2].ssid));
    strlcpy(g_set.net[2].pass, p.getString("pass3", "").c_str(), sizeof(g_set.net[2].pass));

    strlcpy(g_set.api_url, p.getString("api_url", DEFAULT_API_URL).c_str(), sizeof(g_set.api_url));
    if (strlen(g_set.api_url) == 0)
        strlcpy(g_set.api_url, DEFAULT_API_URL, sizeof(g_set.api_url));

    g_set.brightness   = _clamp_u8(p.getUChar("bright",  DEFAULT_BRIGHTNESS), 1, 10);
    g_set.update_min   = _clamp_u8(p.getUChar("upd_min",  DEFAULT_UPDATE_MIN), 1, 15);
    g_set.bme_enabled  = p.getBool("bme_en", BME280_ENABLED ? true : false);
    g_set.bme_interval = (uint16_t)constrain((long)p.getUShort("bme_int", REMOTE_STATION_INTERVAL), 60L, 3600L);
    // Los floats se consultan con isKey() primero: Preferences::getFloat() de una
    // clave inexistente FUNCIONA (devuelve el default) pero escupe un
    // "[E] nvs_get_blob len fail: off_t NOT_FOUND" en el serial que parece un
    // error real. Con isKey() el primer arranque queda limpio.
    g_set.off_temp  = _clamp_f(p.isKey("off_t") ? p.getFloat("off_t") : BME280_TEMP_OFFSET,  -50.0f, 50.0f);
    g_set.off_hum   = _clamp_f(p.isKey("off_h") ? p.getFloat("off_h") : BME280_HUM_OFFSET,   -50.0f, 50.0f);
    g_set.off_press = _clamp_f(p.isKey("off_p") ? p.getFloat("off_p") : BME280_PRESS_OFFSET, -200.0f, 200.0f);

    p.end();
    _set_loaded = true;
}

inline void settings_save_wifi()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString("ssid1", g_set.net[0].ssid);
    p.putString("pass1", g_set.net[0].pass);
    p.putString("ssid2", g_set.net[1].ssid);
    p.putString("pass2", g_set.net[1].pass);
    p.putString("ssid3", g_set.net[2].ssid);
    p.putString("pass3", g_set.net[2].pass);
    p.end();
    Serial.println("[set] redes WiFi guardadas");
}

inline void settings_save_server()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString("api_url", g_set.api_url);
    p.end();
    Serial.printf("[set] URL guardada: %s\n", g_set.api_url);
}

inline void settings_save_display()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putUChar("bright",  g_set.brightness);
    p.putUChar("upd_min", g_set.update_min);
    p.end();
    Serial.printf("[set] pantalla: brillo=%u intervalo=%u min\n",
                  g_set.brightness, g_set.update_min);
}

inline void settings_save_sensor()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putBool("bme_en", g_set.bme_enabled);
    p.putUShort("bme_int", g_set.bme_interval);
    p.putFloat("off_t", g_set.off_temp);
    p.putFloat("off_h", g_set.off_hum);
    p.putFloat("off_p", g_set.off_press);
    p.end();
    Serial.printf("[set] BME280: en=%d int=%us offs=%.1f/%.1f/%.1f\n",
                  g_set.bme_enabled, g_set.bme_interval,
                  g_set.off_temp, g_set.off_hum, g_set.off_press);
}

/** Borra TODA la configuracion del kiosco (vuelve a defaults -> modo AP). */
inline void settings_factory_reset()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.clear();
    p.end();
    _set_loaded = false;
    Serial.println("[set] configuracion borrada (factory reset)");
}

/** true si hay al menos una red guardada (si no, el arranque va directo al AP). */
inline bool settings_has_wifi()
{
    settings_load();
    for (int i = 0; i < 3; i++)
        if (strlen(g_set.net[i].ssid) > 0) return true;
    return false;
}

/** Intervalo de refresco en ms (el portal lo edita en minutos). */
inline uint32_t settings_update_interval_ms()
{
    return (uint32_t)g_set.update_min * 60000UL;
}

#endif // SETTINGS_H
