#include "http_server.h"

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

#include "config.h"
#include "storage.h"
#include "rs485.h"
#include "shelly.h"
#include "wifi_mgr.h"
#include "telnet_log.h"

ESP8266WebServer server(80);

// Wenn ein Neustart nötig ist (z.B. nach dem Speichern einer neuen
// Konfiguration), darf man nicht sofort ESP.restart() aufrufen: dann würde
// die laufende HTTP-Antwort ("OK, gespeichert...") nie beim Browser ankommen,
// weil die Verbindung mitten drin abreißt. Stattdessen wird hier nur ein
// Zeitpunkt in der (nahen) Zukunft gemerkt; httpServerLoop() prüft bei jedem
// Durchlauf, ob dieser Zeitpunkt erreicht ist, und startet erst dann neu --
// das gibt der Antwort genug Zeit, noch rauszugehen. Auch das ist wieder das
// millis()-Zeitstempel-Muster statt eines blockierenden delay().
static bool restartPending = false;
static unsigned long restartAtMillis = 0;

static void scheduleRestart(unsigned long delayMs) {
    restartPending = true;
    restartAtMillis = millis() + delayMs;
}

// ---------------------------------------------------------------------------
// Webinterface (Dark-Mode, mobil-optimiert, Auto-Refresh per fetch("/status"))
//
// PROGMEM: Dieser HTML/CSS/JS-Text ist mehrere KB groß. Ohne PROGMEM würde er
// beim Start komplett in den ohnehin knappen RAM des ESP8266 kopiert werden.
// Mit PROGMEM bleibt er im (viel größeren) Flash-Speicher liegen und wird nur
// bei Bedarf ausgelesen (server.send_P(...) statt server.send(...), das "_P"
// steht für "aus PROGMEM lesen").
// R"rawliteral( ... )rawliteral" ist ein "raw string literal": alles zwischen
// den Klammern wird 1:1 als Text übernommen, ohne dass z.B. Anführungszeichen
// im HTML extra escaped werden müssten.
// ---------------------------------------------------------------------------
static const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>Soyosource Regler</title>
<style>
:root{--bg:#f5f5f7;--fg:#1c1c1e;--card:#ffffff;--border:#d0d0d5;--accent:#0a84ff;--red:#ff3b30;--green:#34c759;--yellow:#ffcc00;--muted:#8a8a8e;}
@media (prefers-color-scheme: dark){:root{--bg:#000000;--fg:#f2f2f7;--card:#1c1c1e;--border:#3a3a3c;--muted:#98989d;}}
*{box-sizing:border-box;}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font-family:-apple-system,"Segoe UI",Roboto,sans-serif;}
h1{font-size:1.3em;margin:0 0 14px 0;transition:opacity .3s;}
h2{font-size:1.05em;margin:0 0 8px 0;}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px;margin-bottom:14px;}
.row{display:flex;justify-content:space-between;gap:10px;padding:7px 0;border-bottom:1px solid var(--border);}
.row:last-child{border-bottom:none;}
.row.blink{animation:blink 1s step-start infinite;}
@keyframes blink{50%{opacity:.25;}}
.btn{display:inline-block;padding:12px 18px;border-radius:10px;border:none;font-size:1em;margin:4px 6px 4px 0;min-height:44px;color:#fff;cursor:pointer;}
.btn-red{background:var(--red);}
.btn-green{background:var(--green);}
.btn-blue{background:var(--accent);}
.btn-muted{background:var(--muted);}
.warning{background:var(--yellow);color:#2c2c00;padding:10px 12px;border-radius:10px;margin-bottom:14px;display:none;font-weight:600;}
.rssi-bars{display:inline-flex;align-items:flex-end;gap:2px;height:18px;}
.rssi-bars span{width:4px;background:var(--muted);border-radius:1px;display:inline-block;}
.rssi-bars span.on{background:var(--green);}
fieldset{border:1px solid var(--border);border-radius:10px;margin-bottom:14px;padding:10px 12px 14px;}
legend{padding:0 6px;font-weight:600;}
label{display:block;margin:10px 0 3px;font-size:.85em;color:var(--muted);}
input[type=text],input[type=password],input[type=number],input[type=time],select{
width:100%;padding:10px;border-radius:8px;border:1px solid var(--border);background:var(--bg);color:var(--fg);font-size:1em;}
.checkbox-row{display:flex;align-items:center;gap:8px;margin:10px 0;}
.checkbox-row input{width:20px;height:20px;}
.hint{font-size:.82em;color:var(--muted);margin-top:4px;}
a{color:var(--accent);}
#staticIpFields,#jsonSection,#nightFields,#staticSection{display:none;}
</style>
</head>
<body>

<h1 id="heartbeat">&#9679; ESP Herzschlag</h1>

<div id="apwarning" class="warning">AP-Setup-Modus aktiv &mdash; bitte WLAN unten konfigurieren und speichern.</div>
<div id="fallbackWarning" class="warning">Fallback aktiv &mdash; Messwertquelle antwortet nicht, Ausgabe auf Fallback-Watt.</div>

<div class="card">
  <h2>Status</h2>
  <div class="row"><span>Demand</span><span><b id="demand">-</b> W</span></div>
  <div class="row"><span>Netzwert</span><span><b id="netz">-</b> W</span></div>
  <div class="row"><span>Modus</span><span id="modeText">-</span></div>
  <div class="row"><span>Notaus</span><span id="notausState">-</span></div>
  <div class="row"><span>WLAN RSSI</span><span class="rssi-bars" id="rssiBars"></span></div>
  <div class="row"><span>Laufzeit</span><span id="uptime">-</span></div>
  <div class="row"><span>Freier Heap</span><span><span id="heap">-</span> B</span></div>
  <div class="row"><span>Messwert</span><span id="measurementAge">-</span></div>
  <div class="row blink" id="dcRow"><span>DC-Einspeisung</span><span id="dcState">-</span></div>
</div>

<div class="card" id="soyoStatus" style="display:none;">
  <h2>Soyo-Status (RS485)</h2>
  <div class="row"><span>Batteriespannung</span><span><span id="soyoBattV">-</span> V</span></div>
  <div class="row"><span>Batteriestrom</span><span><span id="soyoBattA">-</span> A</span></div>
  <div class="row"><span>AC-Spannung</span><span><span id="soyoAcV">-</span> V</span></div>
  <div class="row"><span>Frequenz</span><span><span id="soyoFreq">-</span> Hz</span></div>
  <div class="row"><span>Temperatur</span><span><span id="soyoTemp">-</span> &deg;C</span></div>
</div>

<div class="card">
  <h2>Steuerung</h2>
  <button class="btn btn-red" onclick="notaus(true)">Notaus</button>
  <button class="btn btn-green" onclick="notaus(false)">Notaus aufheben</button>
  <button class="btn btn-muted" onclick="restart()">Neustart</button>
</div>

<div class="card">
  <h2>Konfiguration</h2>
  <form id="cfgForm" onsubmit="return false;">

    <fieldset>
      <legend>WLAN</legend>
      <label>SSID</label>
      <input type="text" id="wifi_ssid">
      <label>Passwort</label>
      <input type="password" id="wifi_pass">
      <div class="checkbox-row"><input type="checkbox" id="wifi_static" onchange="onStaticToggle()"><label style="margin:0;">Statische IP</label></div>
      <div id="staticIpFields">
        <label>IP-Adresse</label>
        <input type="text" id="wifi_ip" placeholder="192.168.1.200">
        <label>Gateway</label>
        <input type="text" id="wifi_gw" placeholder="192.168.1.1">
        <label>Subnetzmaske</label>
        <input type="text" id="wifi_mask" placeholder="255.255.255.0">
      </div>
      <div class="checkbox-row"><input type="checkbox" id="wifi_11n"><label style="margin:0;">802.11n erzwingen</label></div>
    </fieldset>

    <fieldset>
      <legend>MQTT</legend>
      <div class="checkbox-row"><input type="checkbox" id="mqtt_enabled"><label style="margin:0;">MQTT aktiv</label></div>
      <label>Broker</label>
      <input type="text" id="mqtt_broker">
      <label>Port</label>
      <input type="number" id="mqtt_port" value="1883">
      <label>Benutzer</label>
      <input type="text" id="mqtt_user">
      <label>Passwort</label>
      <input type="password" id="mqtt_pass">
      <label>Sub-Topic (Messwert)</label>
      <input type="text" id="mqtt_sub_topic">
      <label>Pub-Topic (Status)</label>
      <input type="text" id="mqtt_pub_topic">
    </fieldset>

    <fieldset>
      <legend>Betriebsmodus</legend>
      <label>Modus</label>
      <select id="mode" onchange="onModeChange()">
        <option value="0">Static (konstante Leistung)</option>
        <option value="1">HttpInterface (/L1L2L3Auto?Value=x)</option>
        <option value="2">MqttSub</option>
        <option value="3">Shelly Gen1</option>
        <option value="4">Shelly Gen2 Pro</option>
        <option value="5">JSON HTTP Client</option>
      </select>

      <div id="staticSection">
        <label>Statische Leistung (W)</label>
        <input type="number" id="static_watt">
      </div>

      <label>Shelly IP</label>
      <input type="text" id="shelly_ip" placeholder="192.168.1.50">
      <div class="checkbox-row"><input type="checkbox" id="shelly_l1"><label style="margin:0;">L1</label></div>
      <div class="checkbox-row"><input type="checkbox" id="shelly_l2"><label style="margin:0;">L2</label></div>
      <div class="checkbox-row"><input type="checkbox" id="shelly_l3"><label style="margin:0;">L3</label></div>
      <div class="hint" id="shellyHint">Gen1: immer Gesamtleistung, Phasenauswahl nicht verfügbar.</div>

      <div id="jsonSection">
        <label>JSON-URL</label>
        <input type="text" id="json_url" placeholder="http://host/status">
        <label>JSON-Pfad (Punkt-separiert)</label>
        <input type="text" id="json_path" placeholder="StatusSNS.SML.DJ_TPWRCURR">
      </div>
    </fieldset>

    <fieldset>
      <legend>Regelung</legend>
      <label>Max. Leistung (W)</label>
      <input type="number" id="max_power">
      <label>Sollwertteiler (1-4)</label>
      <input type="number" id="soyo_count" min="1" max="4">
      <label>Nullpunkt-Offset (W)</label>
      <input type="number" id="offset">
      <label>Fallback-Watt</label>
      <input type="number" id="fallback_watt">
    </fieldset>

    <fieldset>
      <legend>Nachtmodus</legend>
      <div class="checkbox-row"><input type="checkbox" id="night_mode_enabled" onchange="onNightToggle()"><label style="margin:0;">Aktiv</label></div>
      <div id="nightFields">
        <label>Von</label>
        <input type="time" id="night_start">
        <label>Bis</label>
        <input type="time" id="night_end">
        <label>Max. Leistung nachts (W)</label>
        <input type="number" id="night_max_power">
      </div>
    </fieldset>

    <fieldset>
      <legend>OTA</legend>
      <label>OTA-Passwort</label>
      <input type="password" id="ota_pass">
    </fieldset>

    <button type="button" class="btn btn-blue" onclick="saveConfig()">Save Controller</button>
  </form>

  <a class="btn btn-muted" href="/config" download="config.json">Config Download</a>
  <br>
  <label>Config-Restore (JSON-Datei)</label>
  <input type="file" id="uploadFile" accept="application/json">
  <button class="btn btn-muted" onclick="uploadConfig()">Config Upload</button>
</div>

<div class="card">
  <a href="/update">Firmware-Update (OTA)</a>
</div>

<script>
const MODE_NAMES = ['Static','HttpInterface','MqttSub','Shelly Gen1','Shelly Gen2 Pro','JSON HTTP Client'];

function pad(n){return n.toString().padStart(2,'0');}

function fmtUptime(sec){
  const d=Math.floor(sec/86400), h=Math.floor((sec%86400)/3600), m=Math.floor((sec%3600)/60);
  let s='';
  if(d>0) s+=d+'d ';
  s+=h+'h '+m+'m';
  return s;
}

function rssiBars(rssi){
  let bars=0;
  if(rssi>-55) bars=4; else if(rssi>-65) bars=3; else if(rssi>-75) bars=2; else if(rssi>-85) bars=1;
  let html='';
  for(let i=1;i<=4;i++){
    html+='<span class="'+(i<=bars?'on':'')+'" style="height:'+(i*4+4)+'px"></span>';
  }
  return html;
}

function refreshStatus(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('demand').textContent=d.demand;
    document.getElementById('netz').textContent=Number(d.netz).toFixed(1);
    document.getElementById('modeText').textContent=MODE_NAMES[d.mode]||d.mode;
    document.getElementById('notausState').textContent=d.notaus?'AKTIV':'inaktiv';
    document.getElementById('uptime').textContent=fmtUptime(d.uptime);
    document.getElementById('heap').textContent=d.heap;
    document.getElementById('measurementAge').textContent='vor '+d.last_measurement_age+'s aktualisiert';
    document.getElementById('rssiBars').innerHTML=rssiBars(d.rssi);
    document.getElementById('dcState').textContent=(d.demand>0)?'aktiv':'aus';
    document.getElementById('dcRow').classList.toggle('blink', d.demand>0);
    document.getElementById('apwarning').style.display=d.ap_mode?'block':'none';
    document.getElementById('fallbackWarning').style.display=d.fallback?'block':'none';

    const soyoSec=document.getElementById('soyoStatus');
    if(d.soyo){
      soyoSec.style.display='block';
      document.getElementById('soyoBattV').textContent=Number(d.soyo.battery_v).toFixed(1);
      document.getElementById('soyoBattA').textContent=Number(d.soyo.battery_a).toFixed(1);
      document.getElementById('soyoAcV').textContent=Number(d.soyo.ac_v).toFixed(0);
      document.getElementById('soyoFreq').textContent=Number(d.soyo.freq).toFixed(1);
      document.getElementById('soyoTemp').textContent=Number(d.soyo.temp).toFixed(1);
    } else {
      soyoSec.style.display='none';
    }
  }).catch(()=>{});
}
setInterval(refreshStatus,2000);
refreshStatus();

setInterval(()=>{
  const hb=document.getElementById('heartbeat');
  hb.style.opacity=(hb.style.opacity=='0.25')?'1':'0.25';
},500);

function notaus(on){
  fetch(on?'/notaus':'/notaus_off').then(refreshStatus);
}

function restart(){
  if(confirm('ESP wirklich neu starten?')) fetch('/restart');
}

function onModeChange(){
  const mode=parseInt(document.getElementById('mode').value);
  document.getElementById('jsonSection').style.display=(mode==5)?'block':'none';
  document.getElementById('staticSection').style.display=(mode==0)?'block':'none';
  const l1=document.getElementById('shelly_l1'), l2=document.getElementById('shelly_l2'), l3=document.getElementById('shelly_l3');
  const hint=document.getElementById('shellyHint');
  if(mode==4){
    l1.disabled=false; l2.disabled=false; l3.disabled=false;
    hint.style.display='none';
  } else {
    l1.disabled=true; l2.disabled=true; l3.disabled=true;
    hint.style.display=(mode==3)?'block':'none';
  }
}

function onStaticToggle(){
  document.getElementById('staticIpFields').style.display=document.getElementById('wifi_static').checked?'block':'none';
}

function onNightToggle(){
  document.getElementById('nightFields').style.display=document.getElementById('night_mode_enabled').checked?'block':'none';
}

function loadConfigForm(){
  fetch('/config').then(r=>r.json()).then(c=>{
    document.getElementById('wifi_ssid').value=c.wifi_ssid||'';
    document.getElementById('wifi_pass').value=c.wifi_pass||'';
    document.getElementById('wifi_static').checked=!!c.wifi_static;
    document.getElementById('wifi_ip').value=c.wifi_ip||'';
    document.getElementById('wifi_gw').value=c.wifi_gw||'';
    document.getElementById('wifi_mask').value=c.wifi_mask||'';
    document.getElementById('wifi_11n').checked=!!c.wifi_11n;

    document.getElementById('mqtt_enabled').checked=!!c.mqtt_enabled;
    document.getElementById('mqtt_broker').value=c.mqtt_broker||'';
    document.getElementById('mqtt_port').value=c.mqtt_port||1883;
    document.getElementById('mqtt_user').value=c.mqtt_user||'';
    document.getElementById('mqtt_pass').value=c.mqtt_pass||'';
    document.getElementById('mqtt_sub_topic').value=c.mqtt_sub_topic||'';
    document.getElementById('mqtt_pub_topic').value=c.mqtt_pub_topic||'';

    document.getElementById('mode').value=c.mode||0;
    document.getElementById('static_watt').value=c.static_watt;

    document.getElementById('shelly_ip').value=c.shelly_ip||'';
    document.getElementById('shelly_l1').checked=!!c.shelly_l1;
    document.getElementById('shelly_l2').checked=!!c.shelly_l2;
    document.getElementById('shelly_l3').checked=!!c.shelly_l3;

    document.getElementById('json_url').value=c.json_url||'';
    document.getElementById('json_path').value=c.json_path||'';

    document.getElementById('max_power').value=c.max_power;
    document.getElementById('soyo_count').value=c.soyo_count;
    document.getElementById('offset').value=c.offset;
    document.getElementById('fallback_watt').value=c.fallback_watt;

    document.getElementById('night_mode_enabled').checked=!!c.night_mode_enabled;
    document.getElementById('night_start').value=pad(c.night_start_h)+':'+pad(c.night_start_m);
    document.getElementById('night_end').value=pad(c.night_end_h)+':'+pad(c.night_end_m);
    document.getElementById('night_max_power').value=c.night_max_power;

    document.getElementById('ota_pass').value=c.ota_pass||'';

    onModeChange();
    onStaticToggle();
    onNightToggle();
  });
}

function buildConfigObject(){
  function timeParts(id){
    const v=document.getElementById(id).value||'00:00';
    const p=v.split(':');
    return [parseInt(p[0])||0, parseInt(p[1])||0];
  }
  const ns=timeParts('night_start'), ne=timeParts('night_end');

  return {
    wifi_ssid: document.getElementById('wifi_ssid').value,
    wifi_pass: document.getElementById('wifi_pass').value,
    wifi_static: document.getElementById('wifi_static').checked,
    wifi_ip: document.getElementById('wifi_ip').value,
    wifi_gw: document.getElementById('wifi_gw').value,
    wifi_mask: document.getElementById('wifi_mask').value,
    wifi_11n: document.getElementById('wifi_11n').checked,

    mqtt_enabled: document.getElementById('mqtt_enabled').checked,
    mqtt_broker: document.getElementById('mqtt_broker').value,
    mqtt_port: parseInt(document.getElementById('mqtt_port').value)||1883,
    mqtt_user: document.getElementById('mqtt_user').value,
    mqtt_pass: document.getElementById('mqtt_pass').value,
    mqtt_sub_topic: document.getElementById('mqtt_sub_topic').value,
    mqtt_pub_topic: document.getElementById('mqtt_pub_topic').value,

    mode: parseInt(document.getElementById('mode').value),
    static_watt: parseInt(document.getElementById('static_watt').value)||0,

    shelly_ip: document.getElementById('shelly_ip').value,
    shelly_l1: document.getElementById('shelly_l1').checked,
    shelly_l2: document.getElementById('shelly_l2').checked,
    shelly_l3: document.getElementById('shelly_l3').checked,

    json_url: document.getElementById('json_url').value,
    json_path: document.getElementById('json_path').value,

    max_power: parseInt(document.getElementById('max_power').value)||1200,
    soyo_count: parseInt(document.getElementById('soyo_count').value)||1,
    offset: parseInt(document.getElementById('offset').value)||0,
    fallback_watt: parseInt(document.getElementById('fallback_watt').value)||0,

    night_mode_enabled: document.getElementById('night_mode_enabled').checked,
    night_start_h: ns[0], night_start_m: ns[1],
    night_end_h: ne[0], night_end_m: ne[1],
    night_max_power: parseInt(document.getElementById('night_max_power').value)||0,

    ota_pass: document.getElementById('ota_pass').value
  };
}

function saveConfig(){
  fetch('/config', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(buildConfigObject())})
    .then(()=>alert('Gespeichert, Neustart...'));
}

function uploadConfig(){
  const f=document.getElementById('uploadFile').files[0];
  if(!f){ alert('Keine Datei gewählt'); return; }
  const reader=new FileReader();
  reader.onload=function(){
    fetch('/config_upload', {method:'POST', headers:{'Content-Type':'application/json'}, body: reader.result})
      .then(()=>alert('Config wiederhergestellt, Neustart...'));
  };
  reader.readAsText(f);
}

loadConfigForm();
</script>
</body>
</html>
)rawliteral";

// Liefert die komplette Webseite von oben aus. send_P (statt send) liest
// dabei direkt aus dem PROGMEM-Flash-Speicher.
static void handleRoot() {
    server.send_P(200, "text/html", PAGE_HTML);
}

// Liefert die aktuellen Live-Werte als JSON. Das ist genau die URL, die die
// JavaScript-Funktion refreshStatus() oben in der Webseite alle 2 Sekunden
// per fetch('/status') abruft, um die Anzeige zu aktualisieren -- ohne dass
// die Seite dafür neu geladen werden muss.
static void handleStatus() {
    StaticJsonDocument<1024> doc;
    doc["demand"] = g_demand;
    doc["netz"] = g_netzwert;
    doc["rssi"] = WiFi.RSSI();
    doc["mode"] = config.mode;
    doc["notaus"] = g_notaus;
    doc["fallback"] = g_fallbackActive;
    doc["uptime"] = millis() / 1000;
    doc["heap"] = ESP.getFreeHeap();
    doc["ap_mode"] = wifiMgrIsApMode();
    doc["wifi_state"] = (int)wifiMgrGetState();
    doc["last_measurement_age"] = (millis() - g_lastMeasurementMillis) / 1000;

    if (g_soyoStatus.valid) {
        JsonObject soyo = doc["soyo"].to<JsonObject>();
        soyo["battery_v"] = g_soyoStatus.batteryVoltage;
        soyo["battery_a"] = g_soyoStatus.batteryCurrent;
        soyo["ac_v"] = g_soyoStatus.acVoltage;
        soyo["freq"] = g_soyoStatus.frequency;
        soyo["temp"] = g_soyoStatus.temperature;
        soyo["age"] = (millis() - g_soyoStatus.lastUpdate) / 1000;
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// Endpunkt für den Betriebsmodus "HttpInterface" (mode=1): ein externes
// System (z.B. Home Assistant, Node-RED, ein eigenes Script) ruft periodisch
// GET /L1L2L3Auto?Value=<zahl> auf, um der Firmware den aktuellen Netz-
// Messwert zu "pushen". Der Name der URL ist historisch (aus der
// Ursprungs-Firmware übernommen) und bewusst unverändert, damit bestehende
// Integrationen, die schon gegen diese URL sprechen, weiter funktionieren.
static void handleL1L2L3Auto() {
    if (!server.hasArg("Value")) {
        server.send(400, "text/plain", "Missing Value");
        return;
    }
    float val = server.arg("Value").toFloat();
    applyMeasurement(val);
    server.send(200, "text/plain", "OK");
}

static void handleNotaus() {
    g_notaus = true;
    LOG("Notaus: aktiviert (Web)");
    server.send(200, "text/plain", "OK");
}

static void handleNotausOff() {
    g_notaus = false;
    LOG("Notaus: deaktiviert (Web)");
    server.send(200, "text/plain", "OK");
}

static void handleRestart() {
    server.send(200, "text/plain", "Restarting...");
    LOG("Neustart via Webinterface angefordert");
    scheduleRestart(300);
}

static void handleConfigGet() {
    String json = configToJsonString(config);
    server.sendHeader("Content-Disposition", "attachment; filename=config.json");
    server.send(200, "application/json", json);
}

// Gemeinsame Logik für "Config speichern" (POST /config, von der Web-Maske)
// und "Config wiederherstellen" (POST /config_upload, aus einer hochgeladenen
// Datei) -- beide schicken denselben JSON-Text als POST-Body, server.arg("plain")
// ist bei ESP8266WebServer der Weg, an genau diesen rohen Body heranzukommen.
static void applyConfigBodyAndRestart() {
    String body = server.arg("plain");
    Config newConfig;
    if (configFromJsonString(newConfig, body)) {
        config = newConfig;
        saveConfig(config);
        server.send(200, "text/plain", "OK, restarting...");
        LOG("Config: gespeichert, Neustart");
        scheduleRestart(300);
    } else {
        server.send(400, "text/plain", "Invalid JSON");
    }
}

static void handleConfigPost() {
    applyConfigBodyAndRestart();
}

static void handleConfigUpload() {
    applyConfigBodyAndRestart();
}

static void handleLog() {
    server.send(200, "application/json", getRingbufferJson());
}

// server.on(pfad, methode, handlerFunktion) trägt eine "Route" in den
// Webserver ein: kommt eine Anfrage mit passendem Pfad UND passender
// HTTP-Methode (GET/POST) rein, wird automatisch die angegebene Funktion
// aufgerufen. "/config" kommt bewusst zweimal vor (GET und POST) -- das sind
// zwei unterschiedliche Handler für denselben Pfad, je nach Methode.
void httpServerBegin() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/L1L2L3Auto", HTTP_GET, handleL1L2L3Auto);
    server.on("/notaus", HTTP_GET, handleNotaus);
    server.on("/notaus_off", HTTP_GET, handleNotausOff);
    server.on("/restart", HTTP_GET, handleRestart);
    server.on("/config", HTTP_GET, handleConfigGet);
    server.on("/config", HTTP_POST, handleConfigPost);
    server.on("/config_upload", HTTP_POST, handleConfigUpload);
    server.on("/log", HTTP_GET, handleLog);
    server.begin();
    LOG("HTTP-Server gestartet");
}

// Wird bei jedem Schleifendurchlauf aus der Haupt-loop() (main.cpp) aufgerufen.
// server.handleClient() schaut nach, ob gerade eine Anfrage reingekommen ist,
// und ruft bei Bedarf den passenden, oben registrierten Handler auf -- non-
// blocking, d.h. ist gerade keine Anfrage da, kehrt die Funktion sofort zurück.
void httpServerLoop() {
    server.handleClient();
    if (restartPending && millis() >= restartAtMillis) {
        ESP.restart();
    }
}
