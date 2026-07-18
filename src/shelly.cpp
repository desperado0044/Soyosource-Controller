#include "shelly.h"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "rs485.h"
#include "telnet_log.h"

float         g_netzwert = 0;
unsigned long g_lastMeasurementMillis = 0;

// Abfrageintervall ist über config.poll_interval_ms einstellbar (400-2000ms,
// Webinterface -> Konfiguration -> Regelung), Grenzen werden beim Laden/
// Speichern in storage.cpp erzwungen. Bewusst unabhängig vom RS485-Sendezyklus
// (500ms, siehe rs485.cpp): Der HTTP-Abruf blockiert loop() für ~100-450ms
// (live gemessen), läuft er zu oft, verdrängt er das RS485-Senden und den
// Webserver zu stark. Der zuletzt bekannte Messwert (g_netzwert) bleibt bis
// zum nächsten Poll gültig; main.cpp verarbeitet ihn nur einmal pro tatsächlich
// neuer Messung weiter, unabhängig davon wie oft runControlLoop() läuft.
static unsigned long lastPollMillis = 0;

// Fallback-Mechanismus: Ein einzelner fehlgeschlagener HTTP-Abruf (z.B. weil
// der Shelly gerade neu startet) soll noch nicht gleich den Regler in einen
// Sicherheitsmodus schicken. Erst wenn FALLBACK_TRIGGER_FAILS Abfragen HINTER-
// EINANDER fehlschlagen, wird g_fallbackActive gesetzt (siehe enterFallback).
// Genauso wird erst nach mehreren erfolgreichen Antworten in Folge wieder in
// den Normalbetrieb zurückgeschaltet (siehe leaveFallbackIfReady) -- das
// verhindert ein "Flackern" zwischen Normal- und Fallback-Zustand bei einer
// wackligen Verbindung.
static uint8_t failCount = 0;
static uint8_t successCount = 0;
static const uint8_t FALLBACK_TRIGGER_FAILS = 3;
static const uint8_t FALLBACK_CLEAR_SUCCESSES = 3;

static const uint16_t HTTP_TIMEOUT_MS = 1500;

// Bei 500ms Poll-Intervall fällt der TCP-Verbindungsaufbau (SYN/SYN-ACK/ACK)
// pro Abfrage spürbar ins Gewicht -- er blockiert den ESP8266 zusätzlich zur
// eigentlichen Anfrage und lässt bei jedem Poll den Webserver kurz "hängen"
// (unschön bemerkbar im Webinterface). httpClient bleibt daher über alle
// Abfragen hinweg bestehen (kein lokales WiFiClient pro Aufruf mehr), und
// setReuse(true) unten sorgt dafür, dass HTTPClient die bestehende TCP-
// Verbindung zum selben Host wiederverwendet statt jedes Mal neu aufzubauen
// -- sofern die Gegenstelle (Shelly) HTTP Keep-Alive unterstützt.
static WiFiClient httpClient;

void shellyBegin() {
    // Kein weiteres Setup nötig, httpClient oben lebt für die gesamte Laufzeit.
}

// Fragt einen Shelly-Gen1-Stromzähler (Shelly EM/3EM alter Generation) über
// dessen HTTP-API ab und liefert die aktuelle Leistung in outWatt -- je nach
// Konfiguration entweder die Gesamtleistung (Feld "total_power") oder eine
// einzelne Phase (Feld "power" in doc["emeters"][0/1/2], Index 0=L1, 1=L2,
// 2=L3). Gibt true zurück, wenn die Abfrage geklappt hat, sonst false. Das
// gemeinsame Grundgerüst (URL bauen, HTTPClient starten, GET, JSON parsen,
// http.end()) wiederholt sich in fetchShellyGen2()/fetchJsonHttp() unten fast
// identisch.
static bool fetchShellyGen1(float &outWatt) {
    if (strlen(config.shelly_ip) == 0) return false;

    HTTPClient http;
    String url = String("http://") + config.shelly_ip + "/status";

    http.setReuse(true);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(httpClient, url)) {
        LOG("Shelly Gen1: begin() fehlgeschlagen");
        return false;
    }

    int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getString());
        if (!err) {
            bool allPhases = config.shelly_l1 && config.shelly_l2 && config.shelly_l3;
            if (allPhases && !doc["total_power"].isNull()) {
                // "Gesamt" nutzt lieber den vom Shelly selbst berechneten
                // total_power-Wert als drei Einzelwerte selbst aufzusummieren
                // -- vermeidet Rundungsabweichungen zwischen beiden Wegen.
                outWatt = doc["total_power"].as<float>();
                ok = true;
            } else {
                float sum = 0;
                if (config.shelly_l1 && !doc["emeters"][0]["power"].isNull()) { sum += doc["emeters"][0]["power"].as<float>(); ok = true; }
                if (config.shelly_l2 && !doc["emeters"][1]["power"].isNull()) { sum += doc["emeters"][1]["power"].as<float>(); ok = true; }
                if (config.shelly_l3 && !doc["emeters"][2]["power"].isNull()) { sum += doc["emeters"][2]["power"].as<float>(); ok = true; }
                if (ok) outWatt = sum;
            }
        }
        if (!ok) {
            LOG("Shelly Gen1: JSON-Fehler");
        }
    } else {
        LOG(("Shelly Gen1: HTTP-Fehler " + String(code)).c_str());
    }

    // http.end() gibt die HTTP-Verbindung wieder frei. Das muss auf JEDEM
    // Rückweg passieren, auch bei einem Fehler -- sonst bleiben nach und nach
    // Verbindungen offen, bis dem ESP8266 der Speicher ausgeht.
    http.end();
    return ok;
}

static bool fetchShellyGen2(float &outWatt) {
    if (strlen(config.shelly_ip) == 0) return false;

    HTTPClient http;
    String url = String("http://") + config.shelly_ip + "/rpc/EM.GetStatus?id=0";

    http.setReuse(true);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(httpClient, url)) {
        LOG("Shelly Gen2: begin() fehlgeschlagen");
        return false;
    }

    int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getString());
        if (!err) {
            float sum = 0;
            if (config.shelly_l1) sum += doc["a_act_power"] | 0.0f;
            if (config.shelly_l2) sum += doc["b_act_power"] | 0.0f;
            if (config.shelly_l3) sum += doc["c_act_power"] | 0.0f;
            outWatt = sum;
            ok = true;
        } else {
            LOG("Shelly Gen2: JSON-Fehler");
        }
    } else {
        LOG(("Shelly Gen2: HTTP-Fehler " + String(code)).c_str());
    }

    http.end();
    return ok;
}

// Liest einen verschachtelten Wert aus einem JSON-Dokument, dessen Pfad als
// Punkt-getrennter Text übergeben wird, z.B. "StatusSNS.SML.DJ_TPWRCURR" für
// { "StatusSNS": { "SML": { "DJ_TPWRCURR": 123 } } }.
// Funktionsweise: der Pfad wird an jedem "." in einzelne Schlüssel zerlegt
// (z.B. "StatusSNS", "SML", "DJ_TPWRCURR"), und bei jedem Schlüssel steigt
// "cur" eine Ebene tiefer ins JSON hinein (cur = cur[key]). Ist irgendwo auf
// dem Weg der Schlüssel nicht vorhanden, bricht die Funktion mit false ab.
static bool traverseJsonPath(JsonVariantConst root, const char *path, float &outValue) {
    JsonVariantConst cur = root;
    String pathStr(path);
    int start = 0; // Position, ab der im Pfad-Text der nächste Schlüssel beginnt

    while (start < (int)pathStr.length()) {
        int dot = pathStr.indexOf('.', start); // Position des nächsten "."; -1 wenn keiner mehr da ist
        String key = (dot == -1) ? pathStr.substring(start) : pathStr.substring(start, dot);
        cur = cur[key];
        if (cur.isNull()) return false; // Schlüssel existiert nicht -> Pfad ist falsch/passt nicht
        if (dot == -1) break; // letzter Schlüssel im Pfad erreicht
        start = dot + 1;
    }

    outValue = cur.as<float>();
    return true;
}

static bool fetchJsonHttp(float &outWatt) {
    if (strlen(config.json_url) == 0) return false;

    HTTPClient http;

    http.setReuse(true);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(httpClient, config.json_url)) {
        LOG("JsonHttp: begin() fehlgeschlagen");
        return false;
    }

    int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getString());
        if (!err) {
            ok = traverseJsonPath(doc.as<JsonVariantConst>(), config.json_path, outWatt);
            if (!ok) LOG("JsonHttp: Pfad nicht gefunden");
        } else {
            LOG("JsonHttp: JSON-Fehler");
        }
    } else {
        LOG(("JsonHttp: HTTP-Fehler " + String(code)).c_str());
    }

    http.end();
    return ok;
}

// Bewusst kein Median/keine Ausreißer-Filterung auf den Rohwert: Live-
// Messungen an einem Referenzcontroller mit identischer Hardware (siehe
// Kommentar in main.cpp) zeigen, dass dessen Sollwert-Sprünge exakt dem
// jeweils aktuellsten Rohmesswert entsprechen (Ø-Fehler <0.5W über 17
// beobachtete Sprünge) -- ein zusätzlicher Glättungsfilter würde von diesem
// bestätigten Referenzverhalten abweichen, nicht näher daran sein.
void applyMeasurement(float rawWatt) {
    g_netzwert = rawWatt + config.offset;
    g_lastMeasurementMillis = millis();
}

static void enterFallback() {
    if (!g_fallbackActive) {
        g_fallbackActive = true;
        LOG(("Fallback aktiv, Demand=" + String(config.fallback_watt) + "W").c_str());
    }
    successCount = 0;
}

static void leaveFallbackIfReady() {
    if (g_fallbackActive && successCount >= FALLBACK_CLEAR_SUCCESSES) {
        g_fallbackActive = false;
        failCount = 0;
        LOG("Fallback beendet, zurück zum berechneten Sollwert");
    }
}

void shellyLoop() {
    if (config.mode != MODE_SHELLY_GEN1 && config.mode != MODE_SHELLY_GEN2 && config.mode != MODE_JSON_HTTP) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    unsigned long now = millis();
    if (now - lastPollMillis < config.poll_interval_ms) {
        return;
    }
    lastPollMillis = now;

    float watt = 0;
    bool ok = false;

    // Diagnose: misst, wie lange der komplette HTTP-Abruf (inkl. TCP-Verbindung,
    // Warten auf Antwort, JSON-Parsen) tatsächlich dauert -- genau diese Zeit
    // blockiert den ESP8266 komplett, da loop() währenddessen nichts anderes
    // tun kann (siehe README, Abschnitt "Für Einsteiger").
    unsigned long fetchStartMillis = millis();

    switch (config.mode) {
        case MODE_SHELLY_GEN1: ok = fetchShellyGen1(watt); break;
        case MODE_SHELLY_GEN2: ok = fetchShellyGen2(watt); break;
        case MODE_JSON_HTTP:   ok = fetchJsonHttp(watt);   break;
        default: break;
    }

    LOG(("Poll-Dauer: " + String(millis() - fetchStartMillis) + "ms (" + (ok ? "OK" : "Fehler") + ")").c_str());

    if (ok) {
        applyMeasurement(watt);
        failCount = 0;
        successCount++;
        leaveFallbackIfReady();
    } else {
        successCount = 0;
        failCount++;
        if (failCount >= FALLBACK_TRIGGER_FAILS) {
            enterFallback();
        }
    }
}
