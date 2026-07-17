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

static unsigned long lastPollMillis = 0;
static const unsigned long POLL_INTERVAL_MS = 3000;

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

void shellyBegin() {
    // Kein Setup nötig, HTTPClient wird pro Abfrage neu angelegt.
}

// Fragt einen Shelly-Gen1-Stromzähler (Shelly EM/3EM alter Generation) über
// dessen HTTP-API ab und liefert die aktuelle Gesamtleistung in outWatt.
// Gibt true zurück, wenn die Abfrage geklappt hat, sonst false. Das gemeinsame
// Grundgerüst (URL bauen, HTTPClient starten, GET, JSON parsen, http.end())
// wiederholt sich in fetchShellyGen2()/fetchJsonHttp() unten fast identisch.
static bool fetchShellyGen1(float &outWatt) {
    if (strlen(config.shelly_ip) == 0) return false;

    WiFiClient client;
    HTTPClient http;
    String url = String("http://") + config.shelly_ip + "/status";

    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        LOG("Shelly Gen1: begin() fehlgeschlagen");
        return false;
    }

    int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        StaticJsonDocument<1024> doc;
        DeserializationError err = deserializeJson(doc, http.getString());
        if (!err && !doc["total_power"].isNull()) {
            outWatt = doc["total_power"].as<float>();
            ok = true;
        } else {
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

    WiFiClient client;
    HTTPClient http;
    String url = String("http://") + config.shelly_ip + "/rpc/EM.GetStatus?id=0";

    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        LOG("Shelly Gen2: begin() fehlgeschlagen");
        return false;
    }

    int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        StaticJsonDocument<1024> doc;
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

    WiFiClient client;
    HTTPClient http;

    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, config.json_url)) {
        LOG("JsonHttp: begin() fehlgeschlagen");
        return false;
    }

    int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        StaticJsonDocument<1024> doc;
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
        LOG("Fallback beendet, Ramping zurück zum Sollwert");
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
    if (now - lastPollMillis < POLL_INTERVAL_MS) {
        return;
    }
    lastPollMillis = now;

    float watt = 0;
    bool ok = false;

    switch (config.mode) {
        case MODE_SHELLY_GEN1: ok = fetchShellyGen1(watt); break;
        case MODE_SHELLY_GEN2: ok = fetchShellyGen2(watt); break;
        case MODE_JSON_HTTP:   ok = fetchJsonHttp(watt);   break;
        default: break;
    }

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
