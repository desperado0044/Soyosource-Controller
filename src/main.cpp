#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "config.h"
#include "storage.h"
#include "telnet_log.h"
#include "wifi_mgr.h"
#include "led.h"
#include "ntp.h"
#include "rs485.h"
#include "shelly.h"
#include "mqtt_mgr.h"
#include "http_server.h"
#include "ota.h"

Config config;

static unsigned long lastHeapLogMillis = 0;
static const unsigned long HEAP_LOG_INTERVAL_MS = 30000;
static const uint32_t HEAP_MIN_BYTES = 4096;
static const unsigned long MEASUREMENT_STALE_MS = 60000;

// GPIO0 (Flash-Button) 5s beim Boot gedrückt halten -> Werksreset.
// Einziger erlaubter blockierender Wartepunkt im gesamten Projekt.
static void checkFactoryReset() {
    pinMode(0, INPUT_PULLUP);
    if (digitalRead(0) != LOW) {
        return;
    }

    unsigned long start = millis();
    while (millis() - start < 5000) {
        if (digitalRead(0) != LOW) {
            return; // Taste vorzeitig losgelassen
        }
        yield();
    }

    if (digitalRead(0) == LOW) {
        factoryReset(); // formatiert LittleFS und startet neu, kehrt nicht zurück
    }
}

// Regellogik: messwert (inkl. Offset, in g_netzwert) -> neuer Sollwert.
// Static-Modus hält konstant static_watt, Fallback-Zustand fallback_watt.
// Toleranzband und Nachtmodus-Begrenzung gelten nur für die reguläre
// messwertbasierte Regelung.
static void runControlLoop() {
    if (g_notaus) {
        rs485SetTargetDemand(0);
        return;
    }

    int32_t target;

    if (config.mode == MODE_STATIC) {
        target = config.static_watt;
        g_lastMeasurementMillis = millis(); // kein externer Messwert im Static-Modus nötig
    } else if (g_fallbackActive) {
        target = config.fallback_watt;
    } else {
        float netz = g_netzwert;
        if (netz <= -20.0f || netz >= 5.0f) {
            target = g_demand + (int32_t)(netz / config.soyo_count);
        } else {
            target = g_demand; // Toleranzband: Sollwert unverändert
        }
        target = constrain(target, 0, (int32_t)config.max_power);
    }

    if (isNightMode()) {
        target = constrain(target, 0, (int32_t)config.night_max_power);
    }

    rs485SetTargetDemand(target);
}

static void runWatchdog() {
    ESP.wdtFeed();

    unsigned long now = millis();

    if (now - lastHeapLogMillis >= HEAP_LOG_INTERVAL_MS) {
        lastHeapLogMillis = now;
        LOG(("Heap frei: " + String(ESP.getFreeHeap()) + " Bytes").c_str());
    }

    if (ESP.getFreeHeap() < HEAP_MIN_BYTES) {
        LOG("Watchdog: Heap kritisch niedrig, Neustart");
        ESP.restart();
    }

    // Static-Modus liefert nie einen externen Messwert, AP-Modus wartet auf
    // Erstkonfiguration -- beides wäre sonst ein Reboot-Loop.
    bool measurementRelevant = (config.mode != MODE_STATIC) && !wifiMgrIsApMode();
    if (measurementRelevant && now - g_lastMeasurementMillis > MEASUREMENT_STALE_MS) {
        LOG("Watchdog: Kein gültiger Messwert seit >60s, Neustart");
        ESP.restart();
    }
}

void setup() {
    checkFactoryReset();

    storageBegin();
    configSetDefaults(config);
    loadConfig(config);

    telnetLogBegin();
    LOG("Boot: Soyosource-Controller startet");

    ledBegin();
    rs485Begin();
    wifiMgrBegin();
    ntpBegin();
    shellyBegin();
    mqttBegin();
    httpServerBegin();
    otaBegin(server);

    g_lastMeasurementMillis = millis();

    ESP.wdtEnable(0);
}

void loop() {
    wifiMgrLoop();
    ledLoop();
    telnetLogLoop();
    rs485Loop();
    shellyLoop();
    mqttLoop();
    httpServerLoop();
    otaLoop();

    runControlLoop();
    runWatchdog();
}
