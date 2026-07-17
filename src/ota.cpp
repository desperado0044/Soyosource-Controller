// OTA = "Over-The-Air"-Update: neue Firmware wird per WLAN über die Webseite
// /update hochgeladen, statt den ESP8266 per USB-Kabel neu flashen zu müssen.
// Das übernimmt komplett die ElegantOTA-Bibliothek; dieses File hängt sich nur
// mit zwei Callbacks (onOTAStart/onOTAEnd) in deren Ablauf ein, um während des
// Uploads den Reglerbetrieb sicher stillzulegen.
#include "ota.h"

#include <ElegantOTA.h>

#include "config.h"
#include "rs485.h"
#include "telnet_log.h"

// Wird von ElegantOTA automatisch aufgerufen, sobald ein Firmware-Upload
// beginnt. Während des Uploads braucht der ESP8266 seinen Speicher und seine
// Rechenzeit für den Empfang der neuen Firmware -- deshalb wird hier alles
// andere sicherheitshalber stillgelegt: Notaus setzen (Sollwert geht auf 0),
// RS485 pausieren (kein Senden/Empfangen mehr) und den Software-Watchdog
// abschalten (sonst würde er den langsamen Upload fälschlich als "hängen
// geblieben" werten und den ESP mitten im Update neu starten).
static void onOTAStart() {
    g_notaus = true;
    rs485Pause(true); // setzt DE/RE bereits auf LOW (Empfang)
    ESP.wdtDisable();
    LOG("OTA: Update gestartet");
}

// Wird aufgerufen, sobald der Upload fertig ist (erfolgreich oder nicht).
// Der ESP8266 startet danach in jedem Fall neu -- bei Erfolg bootet er mit
// der neuen Firmware, bei Misserfolg einfach wieder mit der alten.
static void onOTAEnd(bool success) {
    LOG(success ? "OTA: Update erfolgreich" : "OTA: Update fehlgeschlagen");
    digitalWrite(RS485_DE_RE_PIN, HIGH);
    ESP.wdtEnable(0);
    ESP.restart();
}

// Hängt ElegantOTA an denselben Webserver (siehe http_server.cpp), der auch
// die restlichen Seiten ausliefert -- daher braucht diese Funktion eine
// Referenz auf genau dieses server-Objekt.
void otaBegin(ESP8266WebServer &server) {
    ElegantOTA.begin(&server, "", config.ota_pass);
    ElegantOTA.onStart(onOTAStart);
    ElegantOTA.onEnd(onOTAEnd);
}

void otaLoop() {
    ElegantOTA.loop();
}
