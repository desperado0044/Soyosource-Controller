#include "ota.h"

#include <ElegantOTA.h>

#include "config.h"
#include "rs485.h"
#include "telnet_log.h"

static void onOTAStart() {
    g_notaus = true;
    rs485Pause(true); // setzt DE/RE bereits auf LOW (Empfang)
    ESP.wdtDisable();
    LOG("OTA: Update gestartet");
}

static void onOTAEnd(bool success) {
    LOG(success ? "OTA: Update erfolgreich" : "OTA: Update fehlgeschlagen");
    digitalWrite(RS485_DE_RE_PIN, HIGH);
    ESP.wdtEnable(0);
    ESP.restart();
}

void otaBegin(ESP8266WebServer &server) {
    ElegantOTA.begin(&server, "", config.ota_pass);
    ElegantOTA.onStart(onOTAStart);
    ElegantOTA.onEnd(onOTAEnd);
}

void otaLoop() {
    ElegantOTA.loop();
}
