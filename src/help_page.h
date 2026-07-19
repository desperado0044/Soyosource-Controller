#pragma once

#include <Arduino.h>

// Inhalt des "Help"-Tabs im Webinterface -- ausgelagert in eine eigene Datei,
// damit http_server.cpp (das schon den großen PAGE_HTML-String enthält) nicht
// noch weiter wächst. Wird nicht Teil von PAGE_HTML, sondern über einen
// eigenen Endpunkt (/help_content, siehe http_server.cpp) nachgeladen, sobald
// der Help-Tab das erste Mal geöffnet wird (siehe switchTab() im <script>
// von PAGE_HTML). Reines HTML-Fragment (nur die Karten), kein <html>/<body> --
// wird per innerHTML in den bereits vorhandenen #tab-help-Container
// eingesetzt.
static const char HELP_HTML[] PROGMEM = R"rawliteral(
<div class="card">
  <h2>Ersteinrichtung</h2>
  <p>Beim allerersten Start (oder nach einem Werksreset) öffnet das Gerät einen
  eigenen WLAN-Access-Point <code>SOYO-Setup</code> (Passwort <code>1234567890</code>,
  Adresse <code>10.0.0.1</code>). Damit verbinden, im "Netzwerkkonfiguration"-Tab
  die eigenen WLAN-Zugangsdaten eintragen und "WLAN speichern" klicken — das
  Gerät startet neu und verbindet sich danach mit dem eingetragenen WLAN.
  Anschließend erreichbar über <code>http://soyo.local</code> oder die vom
  Router vergebene IP. Zuletzt im "Gerätekonfig"-Tab unter "Betriebsmodus" den
  gewünschten Modus einstellen und speichern.</p>
</div>

<div class="card">
  <h2>Werksreset</h2>
  <p>GPIO0 (Flash-Taste am Board) beim Einschalten/Reset 5 Sekunden gedrückt
  halten (muss schon <b>vor</b> dem Einschalten gedrückt sein, nicht erst
  danach) → die Konfiguration wird gelöscht, das Gerät startet neu und öffnet
  wieder den Setup-Access-Point.</p>
</div>

<div class="card">
  <h2>Steuerung</h2>
  <p><b>Notaus</b>: setzt den Sollwert sofort auf 0W und sendet das umgehend
  an den Wechselrichter (nicht erst nach dem nächsten Regelzyklus). Solange
  Notaus aktiv ist, wird kein Messwert mehr verarbeitet — die Regelung pausiert
  komplett, unabhängig vom eingestellten Betriebsmodus.</p>
  <p><b>Notaus aufheben</b>: schaltet zurück in den Normalbetrieb. Der Sollwert
  wird ab dem nächsten Regelzyklus wieder ganz normal aus dem aktuellen
  Messwert berechnet, nicht schlagartig auf den alten Wert vor dem Notaus.</p>
  <p><b>Neustart</b>: startet den ESP sofort neu (Soft-Reset), ohne die
  Konfiguration zu verändern. Kurzzeitig nicht erreichbar, bis WLAN wieder
  verbunden ist.</p>
</div>

<div class="card">
  <h2>Betriebsmodi</h2>
  <div class="row"><span>Static</span><span>konstante Leistung aus "Statische Leistung (W)", kein externer Messwert nötig</span></div>
  <div class="row"><span>HttpInterface</span><span>externes System pusht Messwert per <code>GET /L1L2L3Auto?Value=x</code></span></div>
  <div class="row"><span>MqttSub</span><span>Messwert kommt per MQTT-Nachricht rein, reagiert sofort ohne Abfrageintervall</span></div>
  <div class="row"><span>Shelly Gen1/Gen2</span><span>Gerät fragt den Shelly-Stromzähler selbst per HTTP ab (Intervall einstellbar), Phasen L1/L2/L3 frei kombinierbar</span></div>
  <div class="row"><span>JSON HTTP Client</span><span>fragt eine beliebige selbst konfigurierbare JSON-URL ab, Wert über einen Punkt-getrennten Pfad ausgelesen</span></div>
  <p class="hint">Bei Shelly/JSON: nach 3 Fehlversuchen in Folge schaltet die
  Firmware auf den konfigurierten Fallback-Watt-Wert um, bis wieder 3 Anfragen
  in Folge erfolgreich waren.</p>
</div>

<div class="card">
  <h2>Konfiguration: Regelung</h2>
  <div class="row"><span>Max. Leistung (W)</span><span>harte Obergrenze für den Sollwert, wird nie überschritten</span></div>
  <div class="row"><span>Anzahl der Soyos am Bus (1-12)</span><span>Sollwert wird durch diese Zahl geteilt, wenn mehrere Soyo-Geräte parallel am selben RS485-Bus hängen</span></div>
  <div class="row"><span>Nullpunkt-Offset (W)</span><span>wird zu jedem Messwert addiert, gleicht z.B. eine leicht falsch kalibrierte Messquelle aus</span></div>
  <div class="row"><span>Fallback-Watt</span><span>Sollwert, wenn die Messwertquelle wiederholt nicht antwortet</span></div>
  <div class="row"><span>Poll-Intervall (ms)</span><span>wie oft Shelly/JSON abgefragt werden (400-2000ms)</span></div>
  <div class="row"><span>RS485-Sendeintervall (ms)</span><span>wie oft ein neuer Sollwert an den Soyo gesendet wird (1000-3000ms, Standard 1100ms); zu häufige oder zu seltene Updates können den Soyo unvorhersehbar reagieren lassen, siehe README</span></div>
  <p class="hint">Änderungen hier wirken sofort, ohne Neustart.</p>
</div>

<div class="card">
  <h2>Konfiguration: Nachtmodus</h2>
  <p>Optional aktivierbar, begrenzt die maximale Leistung in einem
  Zeitfenster (z.B. 22:00-06:00) zusätzlich zur normalen Max.-Leistung-Grenze
  — z.B. um nachts leiser/schonender zu fahren. Das Zeitfenster darf über
  Mitternacht gehen. Wirkt ebenfalls sofort, ohne Neustart; die Uhrzeit kommt
  per NTP aus dem Internet (nur verfügbar, sobald das Gerät im WLAN ist).</p>
</div>

<div class="card">
  <h2>Speichern: Neustart oder sofort?</h2>
  <p>Jeder Konfigurationsblock hat einen eigenen Save-Button, es gibt keinen
  globalen "Alles speichern" mehr. Nicht mitgeschickte Felder anderer Blöcke
  bleiben dabei unangetastet.</p>
  <div class="row"><span>WLAN, MQTT, OTA-Passwort</span><span>brauchen einen Neustart (Verbindungen werden nur beim Boot aufgebaut)</span></div>
  <div class="row"><span>Betriebsmodus, Regelung, Nachtmodus</span><span>wirken sofort, kein Neustart nötig</span></div>
</div>

<div class="card">
  <h2>Statuspunkte (Hauptseite)</h2>
  <div class="row"><span><span class="status-dot on"></span> ESP</span><span>Webinterface antwortet gerade</span></div>
  <div class="row"><span><span class="status-dot on"></span> Soyo</span><span>RS485-Antwort vom Wechselrichter da (manche Geräte antworten grundsätzlich nie, das ist normal)</span></div>
  <div class="row"><span><span class="status-dot on"></span> Messwertquelle</span><span>nur bei Shelly/JSON-Modus, rot bei Fallback</span></div>
</div>

<div class="card">
  <h2>HTTP-Endpunkte</h2>
  <div class="row"><span><code>GET /status</code></span><span>Live-Status als JSON</span></div>
  <div class="row"><span><code>GET /L1L2L3Auto?Value=x</code></span><span>Messwert setzen (HttpInterface-Modus)</span></div>
  <div class="row"><span><code>GET /notaus</code> / <code>/notaus_off</code></span><span>Notaus setzen/aufheben</span></div>
  <div class="row"><span><code>GET /config</code></span><span>Konfiguration als JSON herunterladen</span></div>
  <div class="row"><span><code>GET /log</code></span><span>letzte 20 Log-Zeilen als JSON</span></div>
  <div class="row"><span><code>GET /update</code></span><span>Firmware-Update-Seite (ElegantOTA)</span></div>
  <p class="hint">Live-Diagnose zusätzlich per Telnet: <code>telnet soyo.local</code> (Port 23).</p>
</div>

<div class="card">
  <h2>Display (optional)</h2>
  <p>Ein angeschlossenes SSD1306-OLED zeigt Live-Betrieb ohne Handy/Laptop:
  im Setup-Modus SSID/IP des Config-Portals, danach kurz Firmware-Version
  und die zugewiesene IP, dauerhaft danach Netz-/Soyo-Leistung, WLAN-Status
  und aktiver Modus. Rein informativ, keine eigenen Einstellungen im
  Webinterface — Details siehe README.</p>
</div>

<div class="card">
  <h2>Vollständige Dokumentation</h2>
  <p>Für Details (RS485-Protokoll, Hardware-Verkabelung, Fallback-Logik,
  Code-Aufbau) siehe die README im Projekt-Repository:</p>
  <a class="btn btn-blue" href="https://github.com/desperado0044/Soyosource-Controller" target="_blank" rel="noopener">Zum GitHub-Repository</a>
</div>
)rawliteral";
