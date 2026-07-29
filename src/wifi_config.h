/**
 * wifi_config.h - Configuración WiFi con WiFiManager + NVS.
 *
 * Flujo:
 * 1. Intenta conectar a redes guardadas en NVS (hasta 3, con fallback)
 * 2. Si falla, abre portal cautivo con escaneo para las 3 redes
 * 3. Las credenciales se guardan en NVS y persisten tras reflash
 */

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>

static const char *NVS_NAMESPACE = "wificfg";
static const char *KEY_SSID1 = "ssid1";
static const char *KEY_PASS1 = "pass1";
static const char *KEY_SSID2 = "ssid2";
static const char *KEY_PASS2 = "pass2";
static const char *KEY_SSID3 = "ssid3";
static const char *KEY_PASS3 = "pass3";
static const char *KEY_API_URL = "api_url";

#ifndef DEFAULT_API_URL
#define DEFAULT_API_URL "http://192.168.1.100:8080"
#endif

#ifndef AP_NAME
#define AP_NAME "EcowittKiosk"
#endif

#ifndef PORTAL_TIMEOUT
#define PORTAL_TIMEOUT 180
#endif

struct WiFiNetwork {
    char ssid[33];
    char pass[65];
};

static WiFiNetwork _nets[3];
static char _api_url[128];
static Preferences _prefs;
static bool _config_loaded = false;

typedef void (*APModeCallback)(const char *ap_name, const char *ip);
static APModeCallback _ap_callback = nullptr;

inline void wifi_config_set_ap_callback(APModeCallback cb) {
    _ap_callback = cb;
}

static void _load_config()
{
    if (_config_loaded) return;
    _prefs.begin(NVS_NAMESPACE, true);
    strncpy(_nets[0].ssid, _prefs.getString(KEY_SSID1, "").c_str(), 32);
    strncpy(_nets[0].pass, _prefs.getString(KEY_PASS1, "").c_str(), 64);
    strncpy(_nets[1].ssid, _prefs.getString(KEY_SSID2, "").c_str(), 32);
    strncpy(_nets[1].pass, _prefs.getString(KEY_PASS2, "").c_str(), 64);
    strncpy(_nets[2].ssid, _prefs.getString(KEY_SSID3, "").c_str(), 32);
    strncpy(_nets[2].pass, _prefs.getString(KEY_PASS3, "").c_str(), 64);
    strncpy(_api_url, _prefs.getString(KEY_API_URL, DEFAULT_API_URL).c_str(), 127);
    _prefs.end();
    _config_loaded = true;
}

static void _save_config()
{
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.putString(KEY_SSID1, _nets[0].ssid);
    _prefs.putString(KEY_PASS1, _nets[0].pass);
    _prefs.putString(KEY_SSID2, _nets[1].ssid);
    _prefs.putString(KEY_PASS2, _nets[1].pass);
    _prefs.putString(KEY_SSID3, _nets[2].ssid);
    _prefs.putString(KEY_PASS3, _nets[2].pass);
    _prefs.putString(KEY_API_URL, _api_url);
    _prefs.end();
    Serial.println("[wifi] config guardada");
}

static bool _try_connect(uint32_t timeout_ms = 15000)
{
    WiFi.mode(WIFI_STA);
    for (int i = 0; i < 3; i++) {
        if (strlen(_nets[i].ssid) == 0) continue;
        Serial.printf("[wifi] red %d: '%s'\n", i + 1, _nets[i].ssid);
        WiFi.begin(_nets[i].ssid, _nets[i].pass);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
            delay(250);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[wifi] OK: %s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        WiFi.disconnect();
    }
    return false;
}

static bool _should_save = false;
static void _save_callback() { _should_save = true; }

// HTML/JS custom para el portal: 3 redes con dropdown de escaneo
static const char CUSTOM_HTML[] PROGMEM = R"rawliteral(
<style>
.net-group{background:#f8f8f8;padding:12px;margin:10px 0;border-radius:8px;border:1px solid #ddd}
.net-group h4{margin:0 0 8px 0;color:#333}
.net-row{display:flex;gap:8px;margin-bottom:8px}
.net-row select,.net-row input{flex:1;padding:8px;border:1px solid #ccc;border-radius:4px}
.net-row select{max-width:60%}
</style>
<script>
function populateSelects(){
  var nets=[];
  document.querySelectorAll('#wifi a').forEach(function(a){
    var s=a.innerText.trim();if(s&&nets.indexOf(s)<0)nets.push(s);
  });
  nets.sort();
  ['sel1','sel2','sel3'].forEach(function(id){
    var sel=document.getElementById(id);if(!sel)return;
    sel.innerHTML='<option value="">-- Seleccionar --</option>';
    nets.forEach(function(n){
      var o=document.createElement('option');o.value=n;o.text=n;sel.appendChild(o);
    });
  });
}
function onSelChange(selId,inputId){
  var sel=document.getElementById(selId);
  var inp=document.getElementById(inputId);
  if(sel&&inp&&sel.value)inp.value=sel.value;
}
document.addEventListener('DOMContentLoaded',function(){
  setTimeout(populateSelects,500);
  setTimeout(populateSelects,2000);
});
</script>
)rawliteral";

static const char PARAM_HTML[] PROGMEM = R"rawliteral(
<div class="net-group">
<h4>Red WiFi 1 (Principal)</h4>
<div class="net-row">
<select id="sel1" onchange="onSelChange('sel1','ssid1')"><option>Cargando...</option></select>
<input type="text" id="ssid1" name="ssid1" placeholder="SSID" maxlength="32">
</div>
<input type="password" id="pass1" name="pass1" placeholder="Password" maxlength="64" style="width:100%;padding:8px;margin-top:4px">
</div>
<div class="net-group">
<h4>Red WiFi 2 (Respaldo)</h4>
<div class="net-row">
<select id="sel2" onchange="onSelChange('sel2','ssid2')"><option>Cargando...</option></select>
<input type="text" id="ssid2" name="ssid2" placeholder="SSID (opcional)" maxlength="32">
</div>
<input type="password" id="pass2" name="pass2" placeholder="Password" maxlength="64" style="width:100%;padding:8px;margin-top:4px">
</div>
<div class="net-group">
<h4>Red WiFi 3 (Respaldo)</h4>
<div class="net-row">
<select id="sel3" onchange="onSelChange('sel3','ssid3')"><option>Cargando...</option></select>
<input type="text" id="ssid3" name="ssid3" placeholder="SSID (opcional)" maxlength="32">
</div>
<input type="password" id="pass3" name="pass3" placeholder="Password" maxlength="64" style="width:100%;padding:8px;margin-top:4px">
</div>
<div class="net-group">
<h4>Servidor</h4>
<input type="text" id="apiurl" name="apiurl" placeholder="URL del servidor" maxlength="127" style="width:100%;padding:8px">
</div>
)rawliteral";

inline void wifi_config_begin(bool force_portal = false)
{
    _load_config();

    if (!force_portal && _try_connect()) {
        return;
    }

    Serial.println("[wifi] portal AP...");
    if (_ap_callback) {
        _ap_callback(AP_NAME, "192.168.4.1");
    }

    WiFiManager wm;
    wm.setDebugOutput(true);
    wm.setSaveConfigCallback(_save_callback);
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT);

    // Ocultar los campos por defecto de WiFiManager
    wm.setShowStaticFields(false);
    wm.setShowDnsFields(false);
    wm.setShowInfoUpdate(false);
    wm.setShowInfoErase(false);

    // Inyectar CSS/JS custom
    wm.setCustomHeadElement(CUSTOM_HTML);

    // Parámetros custom con HTML propio
    WiFiManagerParameter p_custom(PARAM_HTML);
    wm.addParameter(&p_custom);

    // Parámetros ocultos para capturar los valores
    WiFiManagerParameter p_ssid1("ssid1", "", _nets[0].ssid, 32);
    WiFiManagerParameter p_pass1("pass1", "", _nets[0].pass, 64);
    WiFiManagerParameter p_ssid2("ssid2", "", _nets[1].ssid, 32);
    WiFiManagerParameter p_pass2("pass2", "", _nets[1].pass, 64);
    WiFiManagerParameter p_ssid3("ssid3", "", _nets[2].ssid, 32);
    WiFiManagerParameter p_pass3("pass3", "", _nets[2].pass, 64);
    WiFiManagerParameter p_url("apiurl", "", _api_url, 127);

    wm.addParameter(&p_ssid1);
    wm.addParameter(&p_pass1);
    wm.addParameter(&p_ssid2);
    wm.addParameter(&p_pass2);
    wm.addParameter(&p_ssid3);
    wm.addParameter(&p_pass3);
    wm.addParameter(&p_url);

    bool connected = wm.startConfigPortal(AP_NAME);

    if (_should_save || connected) {
        strncpy(_nets[0].ssid, p_ssid1.getValue(), 32);
        strncpy(_nets[0].pass, p_pass1.getValue(), 64);
        strncpy(_nets[1].ssid, p_ssid2.getValue(), 32);
        strncpy(_nets[1].pass, p_pass2.getValue(), 64);
        strncpy(_nets[2].ssid, p_ssid3.getValue(), 32);
        strncpy(_nets[2].pass, p_pass3.getValue(), 64);
        strncpy(_api_url, p_url.getValue(), 127);

        if (strlen(_api_url) == 0) {
            strncpy(_api_url, DEFAULT_API_URL, 127);
        }

        // Si el usuario seleccionó una red del escaneo nativo de WM
        if (strlen(_nets[0].ssid) == 0 && WiFi.SSID().length() > 0) {
            strncpy(_nets[0].ssid, WiFi.SSID().c_str(), 32);
            strncpy(_nets[0].pass, WiFi.psk().c_str(), 64);
        }

        _save_config();
    }

    if (!connected) {
        Serial.println("[wifi] sin conexion, reiniciando...");
        delay(2000);
        ESP.restart();
    }

    Serial.printf("[wifi] conectado: %s\n", WiFi.localIP().toString().c_str());
}

inline bool wifi_config_reconnect(uint32_t timeout_ms = 15000)
{
    if (WiFi.status() == WL_CONNECTED) return true;
    _load_config();
    return _try_connect(timeout_ms);
}

inline const char *wifi_config_get_api_url()
{
    _load_config();
    return _api_url;
}

inline void wifi_config_reset()
{
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.clear();
    _prefs.end();
    _config_loaded = false;
    memset(_nets, 0, sizeof(_nets));
    memset(_api_url, 0, sizeof(_api_url));
    Serial.println("[wifi] config borrada");
}

inline bool wifi_config_has_saved()
{
    _load_config();
    return strlen(_nets[0].ssid) > 0;
}

#endif
