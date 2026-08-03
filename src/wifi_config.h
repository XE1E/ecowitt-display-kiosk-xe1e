/**
 * wifi_config.h - Conexion WiFi del kiosco.
 *
 * Flujo de arranque:
 *   1. Se borran las credenciales que esp_wifi guarda por su cuenta (ver abajo).
 *   2. Si NO hay ninguna red en nuestro NVS -> portal AP, sin timeout.
 *   3. Si hay, se intentan en orden 1 -> 2 -> 3. Si ninguna responde -> portal
 *      AP con timeout (al expirar reinicia y vuelve a intentar la red).
 *   4. Conectado: queda el servidor de configuracion escuchando en la LAN.
 *
 * IMPORTANTE (la "red fantasma"): Arduino-ESP32 tiene WiFi.persistent(true) por
 * defecto, asi que cada WiFi.begin() graba el SSID/password en el NVS propio de
 * esp_wifi (namespace nvs.net80211), aparte del nuestro. Al arrancar el STA,
 * esp_wifi se reconecta SOLO a lo que tenga ahi guardado, ignorando nuestros 3
 * slots: el display terminaba asociado a una red que ninguna pantalla permitia
 * editar. Por eso lo primero que hace wifi_config_begin() es borrar ese
 * namespace y poner WiFi.persistent(false) para que no se vuelva a grabar nada.
 */

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#include "settings.h"
#include "portal.h"

typedef void (*APModeCallback)(const char *ap_name, const char *ip);
static APModeCallback _ap_callback = nullptr;

inline void wifi_config_set_ap_callback(APModeCallback cb) { _ap_callback = cb; }

/** Intenta las 3 redes guardadas en orden. Salta los slots vacios. */
static bool _try_connect(uint32_t timeout_ms = 15000)
{
    settings_load();
    WiFi.mode(WIFI_STA);

    for (int i = 0; i < 3; i++) {
        if (strlen(g_set.net[i].ssid) == 0) continue;
        Serial.printf("[wifi] red %d: '%s'\n", i + 1, g_set.net[i].ssid);
        WiFi.begin(g_set.net[i].ssid, g_set.net[i].pass);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms)
            delay(250);

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[wifi] OK '%s' -> %s (%d dBm)\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
            return true;
        }
        Serial.printf("[wifi] red %d fallo\n", i + 1);
        WiFi.disconnect();
    }
    return false;
}

/** Levanta el portal AP avisando primero a la pantalla. No retorna. */
static void _go_to_portal(bool wait_forever)
{
    if (_ap_callback) _ap_callback(AP_NAME, "192.168.4.1");
    portal_run_ap(wait_forever);
}

/**
 * Conecta o abre el portal. BLOQUEA hasta tener conexion (o reinicia).
 * Al volver, el servidor de configuracion ya escucha en la IP de la LAN.
 */
inline void wifi_config_begin()
{
    settings_load();

    // Borrar la configuracion persistente de esp_wifi (ver cabecera del archivo).
    // Se hace ANTES de inicializar el WiFi, atacando directo su namespace de NVS.
    //
    // Los dos caminos "de libro" NO funcionan aqui, ambos comprobados en la placa:
    //   - WiFi.disconnect(false, true): en el core 3.x el STA todavia no esta
    //     arrancado en este punto, falla con "STA not started" y el borrado no
    //     ocurre (devuelve false, sin mas aviso).
    //   - esp_wifi_restore(): borra, pero deja el driver en modo NULL, asi que
    //     todos los WiFi.begin() posteriores mueren con "Failed to start STA".
    Preferences np;
    if (np.begin("nvs.net80211", false)) {
        np.clear();
        np.end();
        Serial.println("[wifi] credenciales internas de esp_wifi borradas");
    }

    // De aqui en adelante nada se graba: manda unicamente nuestro NVS.
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);
    delay(100);

    if (!settings_has_wifi()) {
        Serial.println("[wifi] sin redes guardadas -> portal de configuracion");
        _go_to_portal(true);        // primer arranque: esperar indefinidamente
    }

    if (!_try_connect()) {
        Serial.println("[wifi] ninguna red guardada respondio -> portal");
        _go_to_portal(false);       // reinicia al expirar el timeout
    }

    WiFi.setAutoReconnect(true);    // reconexion automatica si el AP se cae
    portal_begin_lan();
}

/** Reconexion en caliente (la llama net.h antes de cada peticion). */
inline bool wifi_config_reconnect(uint32_t timeout_ms = 15000)
{
    if (WiFi.status() == WL_CONNECTED) return true;
    return _try_connect(timeout_ms);
}

inline const char *wifi_config_get_api_url()
{
    settings_load();
    return g_set.api_url;
}

#endif // WIFI_CONFIG_H
