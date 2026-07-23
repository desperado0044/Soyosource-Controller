#include "shelly.h"

#include <ESP8266WiFi.h>
#include <asyncHTTPrequest.h>
#include <ArduinoJson.h>

#include "config.h"
#include "rs485.h"
#include "ota.h"
#include "telnet_log.h"

float         g_netzwert = 0;
unsigned long g_lastMeasurementMillis = 0;

// Abfrageintervall ist über config.poll_interval_ms einstellbar (400-2000ms,
// Webinterface -> Konfiguration -> Regelung), Grenzen werden beim Laden/
// Speichern in storage.cpp erzwungen. Bewusst unabhängig vom RS485-Sendezyklus
// (siehe rs485.cpp): Anders als früher blockiert der HTTP-Abruf loop() nicht
// mehr (siehe "request" unten, asyncHTTPrequest statt HTTPClient) -- das
// Intervall dient nur noch dazu, den Shelly/JSON-Server nicht unnötig oft zu
// belasten. Der zuletzt bekannte Messwert (g_netzwert) bleibt bis zum nächsten
// Poll gültig; main.cpp verarbeitet ihn nur einmal pro tatsächlich neuer
// Messung weiter, unabhängig davon wie oft runControlLoop() läuft.
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

// asyncHTTPrequest kennt nur ganze Sekunden als Timeout (anders als die
// frühere HTTPClient-basierte Lösung mit Millisekunden).
static const int HTTP_TIMEOUT_S = 1;

// Wie schon vorher bei httpClient (WiFiClient): EIN Anfrage-Objekt wird für
// alle Abfragen wiederverwendet statt pro Poll neu angelegt -- asyncHTTPrequest
// ist dafür ausgelegt (open()/send() beliebig oft nacheinander aufrufbar).
static asyncHTTPrequest request;

// Modus, für den GERADE eine Anfrage läuft (open() wurde aufgerufen, Antwort
// aber noch nicht da) -- der Callback in onReadyStateChange() feuert erst
// später, u.U. nachdem der Benutzer im Webinterface längst den Modus
// gewechselt hat. Damit die Antwort trotzdem mit dem richtigen Parser
// ausgewertet wird, merken wir uns hier, welcher Modus beim Start der
// Anfrage aktiv war -- nicht einfach config.mode im Callback nachschauen.
static uint8_t inFlightMode = 0;

// Für die Poll-Dauer-Diagnose (siehe onFetchDone) -- Zeitpunkt, zu dem die
// gerade laufende bzw. zuletzt abgeschlossene Anfrage gestartet wurde.
static unsigned long fetchStartMillis = 0;

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

// Baut die abzufragende URL für den übergebenen Modus. Leerer String heißt
// "nicht konfiguriert" (z.B. shelly_ip/json_url noch leer) -- shellyLoop()
// wertet das wie einen fehlgeschlagenen Abruf.
static String buildUrlForMode(uint8_t mode) {
    switch (mode) {
        case MODE_SHELLY_GEN1:
            if (strlen(config.shelly_ip) == 0) return "";
            return String("http://") + config.shelly_ip + "/status";
        case MODE_SHELLY_GEN2:
            if (strlen(config.shelly_ip) == 0) return "";
            return String("http://") + config.shelly_ip + "/rpc/EM.GetStatus?id=0";
        case MODE_JSON_HTTP:
            if (strlen(config.json_url) == 0) return "";
            return String(config.json_url);
        default:
            return "";
    }
}

// Wertet das schon als JSON geparste Antwort-Dokument passend zum Modus aus
// und liefert die Leistung in outWatt. Gibt false zurück, wenn die
// erwarteten Felder fehlen (falscher Modus/falsche Antwort/falscher Pfad).
static bool parseResponseForMode(uint8_t mode, JsonDocument &doc, float &outWatt) {
    switch (mode) {
        case MODE_SHELLY_GEN1: {
            bool allPhases = config.shelly_l1 && config.shelly_l2 && config.shelly_l3;
            if (allPhases && !doc["total_power"].isNull()) {
                // "Gesamt" nutzt lieber den vom Shelly selbst berechneten
                // total_power-Wert als drei Einzelwerte selbst aufzusummieren
                // -- vermeidet Rundungsabweichungen zwischen beiden Wegen.
                outWatt = doc["total_power"].as<float>();
                return true;
            }
            float sum = 0;
            bool  ok  = false;
            if (config.shelly_l1 && !doc["emeters"][0]["power"].isNull()) { sum += doc["emeters"][0]["power"].as<float>(); ok = true; }
            if (config.shelly_l2 && !doc["emeters"][1]["power"].isNull()) { sum += doc["emeters"][1]["power"].as<float>(); ok = true; }
            if (config.shelly_l3 && !doc["emeters"][2]["power"].isNull()) { sum += doc["emeters"][2]["power"].as<float>(); ok = true; }
            if (ok) outWatt = sum;
            return ok;
        }
        case MODE_SHELLY_GEN2: {
            float sum = 0;
            if (config.shelly_l1) sum += doc["a_act_power"] | 0.0f;
            if (config.shelly_l2) sum += doc["b_act_power"] | 0.0f;
            if (config.shelly_l3) sum += doc["c_act_power"] | 0.0f;
            outWatt = sum;
            return true;
        }
        case MODE_JSON_HTTP:
            return traverseJsonPath(doc.as<JsonVariantConst>(), config.json_path, outWatt);
        default:
            return false;
    }
}

// Bewusst kein Median/keine Ausreißer-Filterung auf den Rohwert: Live-
// Messungen an einem Referenzcontroller mit identischer Hardware (siehe
// Kommentar in main.cpp) zeigen, dass dessen Sollwert-Sprünge exakt dem
// jeweils aktuellsten Rohmesswert entsprechen (Ø-Fehler <0.5W über 17
// beobachtete Sprünge) -- ein zusätzlicher Glättungsfilter würde von diesem
// bestätigten Referenzverhalten abweichen, nicht näher daran sein.
void applyMeasurement(float rawWatt) {
    g_netzwert = rawWatt + config.calibration_offset_w;
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

// Gemeinsamer Abschluss eines Poll-Versuchs -- wird sowohl aufgerufen, wenn
// open() schon direkt fehlschlägt (z.B. leere URL), als auch später
// asynchron aus dem readyStateChange-Callback, sobald die Antwort (oder ein
// Fehler/Timeout) da ist. Aktualisiert Messwert bzw. Fail-/Fallback-Zähler
// und schreibt die Poll-Dauer-Diagnosezeile.
static void onFetchDone(bool ok, float watt) {
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
    inFlightMode = 0;
}

// true zwischen request.open() und der Auswertung der fertigen Antwort in
// shellyLoop() -- siehe dort.
static bool requestPending = false;

// Wertet eine fertige (readyState==4) Antwort aus: HTTP-Code prüfen, JSON
// parsen, passend zu inFlightMode auswerten. WICHTIG: das darf NUR aus dem
// normalen loop()-Kontext heraus laufen (siehe shellyLoop(), Polling von
// request.readyState()), NICHT aus asyncHTTPrequest's onReadyStateChange-
// Callback -- der läuft auf ESP8266 innerhalb des ESPAsyncTCP/LWIP-Callback-
// Stacks, der deutlich kleiner ist als der normale loop()-Stack. deserialize-
// Json() (rekursiver Abstieg) hat dort real einen Stack-Overflow und damit
// einen harten Absturz/Reset ausgelöst -- reproduzierbar beobachtet beim
// ersten Testlauf dieser Umstellung (Log bricht mitten im Zyklus ab, kein
// Fehler-Log, einfach Reset; siehe auch das Beispiel in der Library-eigenen
// README, das genau deshalb im loop() pollt statt im Callback zu arbeiten).
static void processFinishedRequest() {
    int   code = request.responseHTTPcode(); // 200 bei Erfolg, sonst HTTP-Code oder (negativ) interner Fehler/Timeout
    float watt = 0;
    bool  ok   = false;

    if (code == 200) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, request.responseText());
        if (!err) {
            ok = parseResponseForMode(inFlightMode, doc, watt);
            if (!ok) LOG("Shelly/JsonHttp: Antwort passt nicht zum Modus/Pfad");
        } else {
            LOG("Shelly/JsonHttp: JSON-Fehler");
        }
    } else {
        LOG(("Shelly/JsonHttp: HTTP-Fehler " + String(code)).c_str());
    }

    onFetchDone(ok, watt);
}

void shellyBegin() {
    request.setTimeout(HTTP_TIMEOUT_S);
    // Bewusst KEIN onReadyStateChange()-Callback -- siehe Kommentar bei
    // processFinishedRequest() oben. shellyLoop() pollt stattdessen
    // request.readyState() selbst.
}

void shellyLoop() {
    if (config.mode != MODE_SHELLY_GEN1 && config.mode != MODE_SHELLY_GEN2 && config.mode != MODE_JSON_HTTP) {
        return;
    }
    // Während eines laufenden OTA-Uploads keine neue Anfrage starten -- der
    // ESP8266 braucht seinen Speicher und seine Rechenzeit dann fürs
    // Flash-Schreiben (siehe g_otaActive in ota.cpp).
    if (g_otaActive) {
        return;
    }

    // Läuft noch eine Anfrage, deren Antwort wir noch nicht ausgewertet
    // haben? readyState bleibt nach Abschluss auf 4 stehen, bis der nächste
    // open() aufgerufen wird -- requestPending sorgt dafür, dass wir genau
    // einmal auswerten statt bei jedem loop()-Durchlauf erneut.
    if (requestPending) {
        if (request.readyState() != 4 /* readyStateDone */) {
            return; // noch nicht fertig, beim nächsten Mal wieder schauen
        }
        processFinishedRequest();
        requestPending = false;
        return; // nächste Anfrage erst wieder nach Ablauf von poll_interval_ms
    }

    unsigned long now = millis();
    if (now - lastPollMillis < config.poll_interval_ms) {
        return;
    }
    lastPollMillis = now;
    fetchStartMillis = now;

    // Fehlendes WLAN zaehlt genauso als fehlgeschlagener Poll-Versuch wie ein
    // HTTP-Fehler unten -- sonst wuerde eine laengere WLAN-Unterbrechung nie
    // den Fallback-Sollwert (config.fallback_watt, siehe enterFallback())
    // ausloesen: der Regler wuerde stattdessen unbegrenzt am zuletzt
    // berechneten Sollwert festhalten (siehe runControlLoop() in main.cpp),
    // bis nach MEASUREMENT_STALE_MS (60s) der separate Watchdog eingreift und
    // den ESP komplett neu startet -- sicherheitsrelevant, da der Regler bis
    // dahin blind auf einem veralteten Sollwert verharrt.
    if (WiFi.status() != WL_CONNECTED) {
        LOG("Shelly/JsonHttp: WLAN nicht verbunden, zaehlt als fehlgeschlagener Poll");
        onFetchDone(false, 0);
        return;
    }

    inFlightMode = config.mode;

    String url = buildUrlForMode(config.mode);
    if (url.length() == 0 || !request.open("GET", url.c_str())) {
        LOG("Shelly/JsonHttp: open() fehlgeschlagen (URL leer/ungültig?)");
        onFetchDone(false, 0);
        return;
    }
    request.send();
    requestPending = true;
}
