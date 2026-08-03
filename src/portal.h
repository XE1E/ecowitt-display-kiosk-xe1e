/**
 * portal.h - Portal de configuracion web del kiosco (sin WiFiManager).
 *
 * Un unico WebServer que corre en DOS situaciones:
 *
 *   1. Modo AP (portal cautivo). No hay red guardada, o no se pudo conectar:
 *      el ESP32 levanta el AP AP_NAME, un DNSServer que resuelve todo a
 *      192.168.4.1 y redirige las sondas de Android/iOS/Windows para que el
 *      portal salte solo. portal_run_ap() BLOQUEA hasta que se guarda una red
 *      (y reinicia) o expira el timeout.
 *
 *   2. Ya conectado a la LAN. El mismo servidor sigue escuchando en la IP del
 *      display, asi que la configuracion se puede cambiar en cualquier momento
 *      desde el navegador. Sin esto, una URL mal escrita dejaria el display
 *      inconfigurable. Hay que llamar portal_handle() desde loop().
 *
 * La pagina es HTML estatico en PROGMEM; los valores los rellena el JS desde
 * endpoints JSON (/api/status, /api/settings, /api/scan). Asi no se construyen
 * strings enormes en la RAM del ESP32.
 */

#ifndef PORTAL_H
#define PORTAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>

#include "config.h"
#include "settings.h"
#include "status.h"
#include "bme280_sensor.h"

static WebServer  _srv(80);
static DNSServer  _dns;
static bool _portal_up    = false;   // servidor arrancado
static bool _portal_is_ap = false;   // true = modo AP con portal cautivo
static bool _portal_saved = false;   // se guardo una red -> salir del AP y reiniciar

// Escaneo de redes: se cachea para no rescanear en cada recarga de la pagina
// (un scan tarda ~2-3 s y en modo AP tira momentaneamente al cliente).
static String   _scan_json;
static uint32_t _scan_at = 0;

// Callback opcional para pintar el brillo nuevo sin esperar el refresco.
typedef void (*PortalApplyCallback)(void);
static PortalApplyCallback _on_display_change = nullptr;

inline void portal_set_display_callback(PortalApplyCallback cb) { _on_display_change = cb; }

// ── Utilidades ───────────────────────────────────────────────────────────────

// Escapa una cadena para meterla en JSON (comillas, backslash y control).
static String _json_escape(const char *s)
{
    String out;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if ((uint8_t)c < 0x20) continue;
        else out += c;
    }
    return out;
}

static String _uptime_str()
{
    uint32_t s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u",
             (unsigned)(s / 86400), (unsigned)((s / 3600) % 24),
             (unsigned)((s / 60) % 60), (unsigned)(s % 60));
    return String(buf);
}

// ── HTML de la pagina ────────────────────────────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="es"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ecowitt Display Kiosk</title>
<style>
:root{--bg:#11151c;--card:#1a1f2a;--line:#2b3342;--fg:#e6edf3;--mut:#8b98a8;--acc:#3b82f6;--ok:#22c55e;--bad:#ef4444}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,sans-serif}
.wrap{max-width:620px;margin:0 auto}
h1{font-size:20px;margin:0 0 4px}
.sub{color:var(--mut);font-size:13px;margin-bottom:18px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin-bottom:14px}
h2{font-size:15px;margin:0 0 12px;color:var(--acc);text-transform:uppercase;letter-spacing:.04em}
label{display:block;font-size:13px;color:var(--mut);margin:10px 0 4px}
input,select{width:100%;padding:9px;background:#0d1117;color:var(--fg);border:1px solid var(--line);border-radius:6px;font-size:15px}
input:focus,select:focus{outline:none;border-color:var(--acc)}
button{padding:9px 14px;background:var(--acc);color:#fff;border:0;border-radius:6px;font-size:14px;cursor:pointer}
button:active{opacity:.8}
button.sec{background:#33415a}
button.danger{background:var(--bad)}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:12px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 10px}
table{width:100%;border-collapse:collapse;font-size:13px}
td{padding:3px 0;border-bottom:1px solid var(--line)}
td:first-child{color:var(--mut);width:44%}
.slot{border:1px solid var(--line);border-radius:8px;padding:8px 10px;margin-bottom:10px}
.slot b{font-size:13px;color:var(--fg)}
.nets{margin-top:8px;max-height:210px;overflow-y:auto;border:1px solid var(--line);border-radius:6px;display:none}
.net{display:flex;justify-content:space-between;gap:8px;padding:8px 10px;cursor:pointer;border-bottom:1px solid var(--line)}
.net:hover{background:#222b39}
.net small{color:var(--mut);white-space:nowrap}
.msg{margin-top:10px;font-size:13px;min-height:18px}
.msg.ok{color:var(--ok)} .msg.bad{color:var(--bad)}
.hint{font-size:12px;color:var(--mut);margin-top:6px}
</style></head><body><div class="wrap">
<h1>Ecowitt Display Kiosk</h1>
<div class="sub" id="mode">Cargando...</div>

<section><h2>Estado</h2>
<table id="st"><tr><td>Cargando</td><td>...</td></tr></table>
</section>

<section><h2>Redes WiFi</h2>
<div class="row"><button class="sec" onclick="scan()" id="scanb">Escanear redes</button>
<span class="hint" id="scanmsg"></span></div>
<div class="nets" id="nets"></div>
<div class="hint">Toca una red de la lista para ponerla en el siguiente slot libre. Se intentan en orden 1 &rarr; 2 &rarr; 3.</div>
<div class="slot"><b>Red 1 (principal)</b>
<label>SSID</label><input id="ssid1" maxlength="32">
<label>Contrase&ntilde;a</label><input id="pass1" type="password" maxlength="64" placeholder="(sin cambios)">
</div>
<div class="slot"><b>Red 2 (respaldo)</b>
<label>SSID</label><input id="ssid2" maxlength="32">
<label>Contrase&ntilde;a</label><input id="pass2" type="password" maxlength="64" placeholder="(sin cambios)">
</div>
<div class="slot"><b>Red 3 (respaldo)</b>
<label>SSID</label><input id="ssid3" maxlength="32">
<label>Contrase&ntilde;a</label><input id="pass3" type="password" maxlength="64" placeholder="(sin cambios)">
</div>
<div class="row"><button onclick="saveWifi()">Guardar y reiniciar</button></div>
<div class="msg" id="wmsg"></div>
</section>

<section><h2>Servidor</h2>
<label>URL base (sin slash final)</label>
<input id="apiurl" maxlength="127" placeholder="http://192.168.1.100:8080">
<div class="hint">Usa HTTP por IP y puerto, no HTTPS: el handshake TLS agrega 1-2 s a cada peticion.</div>
<div class="row"><button class="sec" onclick="test()">Probar conexi&oacute;n</button>
<button onclick="saveServer()">Guardar y reiniciar</button></div>
<div class="msg" id="smsg"></div>
</section>

<section><h2>Pantalla</h2>
<label>Brillo: <span id="brv"></span></label>
<input id="bright" type="range" min="1" max="10" oninput="document.getElementById('brv').textContent=this.value*10+'%'">
<label>Intervalo de actualizaci&oacute;n</label>
<select id="updmin"></select>
<div class="hint">El brillo se aplica al pintar la imagen (el backlight de esta placa no tiene PWM). Se ve al siguiente refresco.</div>
<div class="row"><button onclick="saveDisplay()">Guardar</button></div>
<div class="msg" id="dmsg"></div>
</section>

<section><h2>Sensor BME280</h2>
<label>Env&iacute;o al servidor</label>
<select id="bmeen"><option value="1">Habilitado</option><option value="0">Deshabilitado</option></select>
<label>Intervalo de env&iacute;o (segundos, 60-3600)</label>
<input id="bmeint" type="number" min="60" max="3600" step="10">
<div class="hint">Offsets de calibraci&oacute;n: se SUMAN a la lectura cruda. Si el sensor marca 29.7 y el real es 25, el offset es -4.7.</div>
<div class="grid">
<div><label>Temp (&deg;C)</label><input id="offt" type="number" step="0.1"></div>
<div><label>Humedad (%)</label><input id="offh" type="number" step="0.1"></div>
</div>
<label>Presi&oacute;n (hPa)</label><input id="offp" type="number" step="0.1">
<div class="row"><button onclick="saveSensor()">Guardar</button></div>
<div class="msg" id="bmsg"></div>
</section>

<section><h2>Mantenimiento</h2>
<div class="row">
<button class="sec" onclick="doPost('/api/refresh','Refrescando...')">Forzar refresco</button>
<button class="sec" onclick="if(confirm('Reiniciar el display?'))doPost('/api/restart','Reiniciando...')">Reiniciar</button>
<button class="danger" onclick="if(confirm('Borrar TODA la configuracion? El display volvera al modo AP.'))doPost('/api/factory','Borrado. Reiniciando en modo AP...')">Borrar configuraci&oacute;n</button>
</div>
<div class="msg" id="mmsg"></div>
</section>

<script>
var $=function(i){return document.getElementById(i)};
function msg(id,t,ok){var e=$(id);e.textContent=t;e.className='msg'+(ok===true?' ok':ok===false?' bad':'')}

function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})}
function doPost(u,t){msg('mmsg',t);post(u,'').then(function(){}).catch(function(){})}

function loadStatus(){
  fetch('/api/status').then(function(r){return r.json()}).then(function(s){
    $('mode').textContent=s.ap?'Modo configuracion (AP '+s.ssid+') - '+s.ip:'Conectado a '+s.ssid+' - '+s.ip;
    var http=s.http===0?'--':(s.http===200?'200 OK':String(s.http));
    var rows=[['Red',s.ap?'(modo AP)':s.ssid+'  '+s.rssi+' dBm'],
      ['IP',s.ip],['MAC',s.mac],['Encendido hace',s.up],['Firmware',s.fw],
      ['Pagina mostrada',s.page?String(s.page):'--'],
      ['Ultimo GET',http+(s.ms?'  '+s.ms+' ms':'')+(s.bytes?'  '+s.bytes+' B':'')],
      ['POST BME280',s.post===0?'--':String(s.post)],
      ['BME280',s.bme?'presente':'no responde'],
      ['PSRAM libre',(s.psram/1024).toFixed(0)+' KB'],
      ['Heap libre',(s.heap/1024).toFixed(0)+' KB']];
    $('st').innerHTML=rows.map(function(r){return '<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>'}).join('');
  }).catch(function(){});
}

function loadSettings(){
  fetch('/api/settings').then(function(r){return r.json()}).then(function(c){
    $('ssid1').value=c.ssid1;$('ssid2').value=c.ssid2;$('ssid3').value=c.ssid3;
    $('apiurl').value=c.url;
    $('bright').value=c.bright;$('brv').textContent=c.bright*10+'%';
    var sel=$('updmin');sel.innerHTML='';
    for(var i=1;i<=15;i++){var o=document.createElement('option');o.value=i;
      o.text=i+(i==1?' minuto':' minutos');if(i==c.upd)o.selected=true;sel.appendChild(o)}
    $('bmeen').value=c.bme?'1':'0';$('bmeint').value=c.bmeint;
    $('offt').value=c.offt;$('offh').value=c.offh;$('offp').value=c.offp;
  }).catch(function(){});
}

function scan(){
  $('scanb').disabled=true;msg('scanmsg','Escaneando (2-3 s)...');
  fetch('/api/scan').then(function(r){return r.json()}).then(function(n){
    $('scanb').disabled=false;
    if(!n.length){msg('scanmsg','No se encontraron redes',false);return}
    msg('scanmsg',n.length+' redes');
    var d=$('nets');d.innerHTML='';d.style.display='block';
    n.forEach(function(x){
      var e=document.createElement('div');e.className='net';
      e.innerHTML='<span>'+(x.s||'(oculta)')+(x.e?' &#128274;':'')+'</span><small>'+x.r+' dBm</small>';
      e.onclick=function(){pick(x.s)};d.appendChild(e);
    });
  }).catch(function(){$('scanb').disabled=false;msg('scanmsg','Error al escanear',false)});
}

// Pone el SSID en el primer slot vacio (o en el 1 si los tres estan llenos).
function pick(s){
  var ids=['ssid1','ssid2','ssid3'];
  for(var i=0;i<3;i++){if(!$(ids[i]).value){$(ids[i]).value=s;$('pass'+(i+1)).focus();return}}
  $('ssid1').value=s;$('pass1').focus();
}

function saveWifi(){
  if(!$('ssid1').value){msg('wmsg','La red 1 no puede quedar vacia',false);return}
  var q=['ssid1','pass1','ssid2','pass2','ssid3','pass3'].map(function(i){
    return i+'='+encodeURIComponent($(i).value)}).join('&');
  msg('wmsg','Guardando y reiniciando... reconecta a tu red WiFi normal.',true);
  post('/api/save/wifi',q).catch(function(){});
}

function saveServer(){
  var u=$('apiurl').value.trim();
  if(!/^https?:\/\/.+/.test(u)){msg('smsg','La URL debe empezar con http:// o https://',false);return}
  msg('smsg','Guardando y reiniciando...',true);
  post('/api/save/server','url='+encodeURIComponent(u)).catch(function(){});
}

function test(){
  var u=$('apiurl').value.trim();
  if(!/^https?:\/\/.+/.test(u)){msg('smsg','La URL debe empezar con http:// o https://',false);return}
  msg('smsg','Probando...');
  fetch('/api/test?url='+encodeURIComponent(u)).then(function(r){return r.json()}).then(function(x){
    if(x.code===200)msg('smsg','OK: HTTP 200 en '+x.ms+' ms ('+x.bytes+' bytes)',true);
    else if(x.code===-2)msg('smsg','En modo AP no hay salida a la red: guarda el WiFi primero y prueba desde la LAN.',false);
    else msg('smsg','Fallo: HTTP '+x.code+' ('+x.ms+' ms)',false);
  }).catch(function(){msg('smsg','Sin respuesta del display',false)});
}

function saveDisplay(){
  var q='bright='+$('bright').value+'&upd='+$('updmin').value;
  post('/api/save/display',q).then(function(r){return r.text()}).then(function(){
    msg('dmsg','Guardado. Se aplica en el siguiente refresco.',true)
  }).catch(function(){msg('dmsg','Error al guardar',false)});
}

function saveSensor(){
  var q='en='+$('bmeen').value+'&int='+$('bmeint').value+
    '&offt='+$('offt').value+'&offh='+$('offh').value+'&offp='+$('offp').value;
  post('/api/save/sensor',q).then(function(r){return r.text()}).then(function(){
    msg('bmsg','Guardado y aplicado.',true)
  }).catch(function(){msg('bmsg','Error al guardar',false)});
}

loadStatus();loadSettings();setInterval(loadStatus,5000);
</script></div></body></html>)HTML";

// ── Handlers ─────────────────────────────────────────────────────────────────

static void _h_root()
{
    _srv.sendHeader("Cache-Control", "no-store");
    _srv.send_P(200, "text/html; charset=utf-8", PORTAL_HTML);
}

static void _h_status()
{
    String j = "{";
    j += "\"ap\":";     j += _portal_is_ap ? "true" : "false";
    j += ",\"ssid\":\""; j += _json_escape(_portal_is_ap ? AP_NAME : WiFi.SSID().c_str()); j += "\"";
    j += ",\"rssi\":";  j += String(WiFi.RSSI());
    j += ",\"ip\":\"";  j += (_portal_is_ap ? WiFi.softAPIP() : WiFi.localIP()).toString(); j += "\"";
    j += ",\"mac\":\""; j += WiFi.macAddress(); j += "\"";
    j += ",\"up\":\"";  j += _uptime_str(); j += "\"";
    j += ",\"fw\":\"";  j += FW_VERSION; j += "\"";
    j += ",\"page\":";  j += String(g_status.page);
    j += ",\"http\":";  j += String(g_status.last_http);
    j += ",\"ms\":";    j += String(g_status.last_ms);
    j += ",\"bytes\":"; j += String(g_status.last_bytes);
    j += ",\"post\":";  j += String(g_status.post_http);
    j += ",\"bme\":";   j += isBME280Available() ? "true" : "false";
    j += ",\"psram\":"; j += String(ESP.getFreePsram());
    j += ",\"heap\":";  j += String(ESP.getFreeHeap());
    j += "}";
    _srv.send(200, "application/json", j);
}

static void _h_settings()
{
    settings_load();
    // Las contraseñas NO se devuelven: el campo se deja vacio y si el usuario lo
    // deja asi al guardar, se conserva la que ya estaba en NVS.
    String j = "{";
    j += "\"ssid1\":\""; j += _json_escape(g_set.net[0].ssid); j += "\"";
    j += ",\"ssid2\":\""; j += _json_escape(g_set.net[1].ssid); j += "\"";
    j += ",\"ssid3\":\""; j += _json_escape(g_set.net[2].ssid); j += "\"";
    j += ",\"url\":\"";  j += _json_escape(g_set.api_url); j += "\"";
    j += ",\"bright\":"; j += String(g_set.brightness);
    j += ",\"upd\":";    j += String(g_set.update_min);
    j += ",\"bme\":";    j += g_set.bme_enabled ? "true" : "false";
    j += ",\"bmeint\":"; j += String(g_set.bme_interval);
    j += ",\"offt\":";   j += String(g_set.off_temp, 1);
    j += ",\"offh\":";   j += String(g_set.off_hum, 1);
    j += ",\"offp\":";   j += String(g_set.off_press, 1);
    j += "}";
    _srv.send(200, "application/json", j);
}

static void _h_scan()
{
    // Cache de 20 s: en modo AP cada scan corta momentaneamente al cliente.
    if (_scan_json.length() && millis() - _scan_at < 20000) {
        _srv.send(200, "application/json", _scan_json);
        return;
    }

    int n = WiFi.scanNetworks(false, false);
    String j = "[";
    for (int i = 0; i < n; i++) {
        if (i) j += ",";
        j += "{\"s\":\"";  j += _json_escape(WiFi.SSID(i).c_str()); j += "\"";
        j += ",\"r\":";    j += String(WiFi.RSSI(i));
        j += ",\"e\":";    j += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "false" : "true";
        j += "}";
    }
    j += "]";
    WiFi.scanDelete();

    _scan_json = j;
    _scan_at = millis();
    _srv.send(200, "application/json", j);
    Serial.printf("[portal] scan: %d redes\n", n);
}

/** Prueba la URL pedida (sin guardarla) y reporta codigo, latencia y tamaño. */
static void _h_test()
{
    String base = _srv.arg("url");
    if (base.length() == 0) {
        _srv.send(400, "application/json", "{\"code\":-1,\"ms\":0,\"bytes\":0}");
        return;
    }
    if (_portal_is_ap) {
        // En modo AP no hay salida a la LAN todavia.
        _srv.send(200, "application/json", "{\"code\":-2,\"ms\":0,\"bytes\":0}");
        return;
    }

    WiFiClient client;
    HTTPClient http;
    String url = base + "/api/display.jpg?page=1";
    uint32_t t0 = millis();
    int code = -1, len = 0;
    if (http.begin(client, url)) {
        http.setTimeout(10000);
        code = http.GET();
        len = http.getSize();
        http.end();
    }
    uint32_t ms = millis() - t0;

    char j[96];
    snprintf(j, sizeof(j), "{\"code\":%d,\"ms\":%u,\"bytes\":%d}", code, (unsigned)ms, len);
    _srv.send(200, "application/json", j);
    Serial.printf("[portal] test %s -> %d (%u ms)\n", url.c_str(), code, (unsigned)ms);
}

// Responde y reinicia. El navegador ya recibio el 200 antes del reset.
static void _reply_and_restart(const char *txt)
{
    _srv.send(200, "text/plain", txt);
    _srv.client().flush();
    delay(600);
    ESP.restart();
}

static void _h_save_wifi()
{
    settings_load();
    const char *sk[3] = { "ssid1", "ssid2", "ssid3" };
    const char *pk[3] = { "pass1", "pass2", "pass3" };

    for (int i = 0; i < 3; i++) {
        if (_srv.hasArg(sk[i]))
            strlcpy(g_set.net[i].ssid, _srv.arg(sk[i]).c_str(), sizeof(g_set.net[i].ssid));
        // Password vacia = "sin cambios", SALVO que el SSID quedara vacio (slot
        // borrado): ahi tambien se limpia la password.
        if (strlen(g_set.net[i].ssid) == 0) {
            g_set.net[i].pass[0] = '\0';
        } else if (_srv.hasArg(pk[i]) && _srv.arg(pk[i]).length() > 0) {
            strlcpy(g_set.net[i].pass, _srv.arg(pk[i]).c_str(), sizeof(g_set.net[i].pass));
        }
    }

    settings_save_wifi();
    _portal_saved = true;
    _reply_and_restart("Guardado. Reiniciando...");
}

static void _h_save_server()
{
    settings_load();
    String u = _srv.arg("url");
    u.trim();
    while (u.endsWith("/")) u.remove(u.length() - 1);
    if (u.length() == 0) {
        _srv.send(400, "text/plain", "URL vacia");
        return;
    }
    strlcpy(g_set.api_url, u.c_str(), sizeof(g_set.api_url));
    settings_save_server();
    _reply_and_restart("Guardado. Reiniciando...");
}

static void _h_save_display()
{
    settings_load();
    g_set.brightness = _clamp_u8(_srv.arg("bright").toInt(), 1, 10);
    g_set.update_min = _clamp_u8(_srv.arg("upd").toInt(), 1, 15);
    settings_save_display();
    _srv.send(200, "text/plain", "OK");
    // Aplica en vivo (rehace las LUTs de brillo e invalida la cache de paginas).
    if (_on_display_change) _on_display_change();
}

static void _h_save_sensor()
{
    settings_load();
    g_set.bme_enabled  = _srv.arg("en").toInt() != 0;
    g_set.bme_interval = (uint16_t)constrain(_srv.arg("int").toInt(), 60L, 3600L);
    g_set.off_temp     = _clamp_f(_srv.arg("offt").toFloat(),  -50.0f, 50.0f);
    g_set.off_hum      = _clamp_f(_srv.arg("offh").toFloat(),   -50.0f, 50.0f);
    g_set.off_press    = _clamp_f(_srv.arg("offp").toFloat(), -200.0f, 200.0f);
    settings_save_sensor();

    // Los offsets se aplican al vuelo, sin reiniciar (asi se calibra en vivo).
    setBME280TemperatureOffset(g_set.off_temp);
    setBME280HumidityOffset(g_set.off_hum);
    setBME280PressureOffset(g_set.off_press);

    _srv.send(200, "text/plain", "OK");
}

static void _h_restart()
{
    _reply_and_restart("Reiniciando...");
}

static void _h_factory()
{
    settings_factory_reset();
    // Tambien borra las credenciales que esp_wifi guarda por su cuenta, para que
    // el proximo arranque no se reconecte solo a la red anterior.
    WiFi.disconnect(false, true);
    _reply_and_restart("Configuracion borrada. Reiniciando en modo AP...");
}

// Forzar refresco: lo atiende main.cpp via callback (invalida la cache).
static void _h_refresh()
{
    _srv.send(200, "text/plain", "OK");
    if (_on_display_change) _on_display_change();
}

// Sondas de deteccion de portal cautivo: cualquier 302 hace que el sistema
// operativo abra la pagina. En modo STA, un 404 normal.
static void _h_not_found()
{
    if (_portal_is_ap) {
        _srv.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        _srv.send(302, "text/plain", "");
    } else {
        _srv.send(404, "text/plain", "no encontrado");
    }
}

static void _register_handlers()
{
    _srv.on("/",                  HTTP_GET,  _h_root);
    _srv.on("/api/status",        HTTP_GET,  _h_status);
    _srv.on("/api/settings",      HTTP_GET,  _h_settings);
    _srv.on("/api/scan",          HTTP_GET,  _h_scan);
    _srv.on("/api/test",          HTTP_GET,  _h_test);
    _srv.on("/api/save/wifi",     HTTP_POST, _h_save_wifi);
    _srv.on("/api/save/server",   HTTP_POST, _h_save_server);
    _srv.on("/api/save/display",  HTTP_POST, _h_save_display);
    _srv.on("/api/save/sensor",   HTTP_POST, _h_save_sensor);
    _srv.on("/api/restart",       HTTP_POST, _h_restart);
    _srv.on("/api/factory",       HTTP_POST, _h_factory);
    _srv.on("/api/refresh",       HTTP_POST, _h_refresh);
    _srv.onNotFound(_h_not_found);
}

// ── API publica ──────────────────────────────────────────────────────────────

/** Arranca el servidor sobre la conexion STA ya establecida (no bloquea). */
inline void portal_begin_lan()
{
    if (_portal_up) return;
    _portal_is_ap = false;
    _register_handlers();
    _srv.begin();
    _portal_up = true;
    Serial.printf("[portal] configuracion en http://%s/\n", WiFi.localIP().toString().c_str());
}

/** Atender peticiones. Llamar desde loop(). */
inline void portal_handle()
{
    if (!_portal_up) return;
    if (_portal_is_ap) _dns.processNextRequest();
    _srv.handleClient();
}

/**
 * Modo AP con portal cautivo. BLOQUEA.
 *
 * @param wait_forever true en el primer arranque (sin red guardada): no tiene
 *        sentido salirse por timeout porque no hay a donde conectarse. Si es
 *        false, a los PORTAL_TIMEOUT segundos reinicia para reintentar la red.
 * @return no retorna si se guarda configuracion (reinicia).
 */
inline void portal_run_ap(bool wait_forever)
{
    settings_load();

    WiFi.mode(WIFI_AP_STA);          // AP_STA: el AP sirve la pagina y el STA escanea
    WiFi.softAP(AP_NAME);
    delay(300);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[portal] AP '%s' en http://%s/\n", AP_NAME, ip.toString().c_str());

    _portal_is_ap = true;
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", ip);         // todo dominio -> nosotros = portal cautivo
    _register_handlers();
    _srv.begin();
    _portal_up = true;

    uint32_t t0 = millis();
    uint32_t limit = (uint32_t)PORTAL_TIMEOUT * 1000UL;
    for (;;) {
        _dns.processNextRequest();
        _srv.handleClient();
        if (_portal_saved) break;    // (no deberia llegar: _h_save_wifi reinicia)
        if (!wait_forever && PORTAL_TIMEOUT > 0 && millis() - t0 > limit) {
            Serial.println("[portal] timeout del portal, reiniciando");
            delay(200);
            ESP.restart();
        }
        delay(2);
    }
}

#endif // PORTAL_H
