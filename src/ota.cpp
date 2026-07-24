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

// Wird auf true gesetzt, sobald ein Upload beginnt, und erst in onOTAEnd()
// wieder zurückgesetzt. Zusammen mit otaStartMillis/OTA_TIMEOUT_MS unten
// federt das einen realen, heute beobachteten Fehlerfall ab: ElegantOTA ruft
// onOTAStart() schon beim reinen Start-Request auf (GET /ota/start), bevor
// der eigentliche Upload (POST /ota/upload) überhaupt läuft. Kommt dieser
// Upload aus irgendeinem Grund nie an oder bricht die Verbindung vorher ab
// (z.B. Netzwerkaussetzer), wird onOTAEnd() NIE aufgerufen -- ohne die
// Zeitgrenze hier bliebe RS485 dann für immer pausiert (Notaus aktiv, kein
// einziges Frame mehr raus, keine RS485-Anzeige mehr am Wechselrichter), bis
// irgendwer manuell neu startet.
bool                 g_otaActive = false;
static unsigned long otaStartMillis = 0;
static const unsigned long OTA_TIMEOUT_MS = 60000; // 1 Minute

// Wird von ElegantOTA automatisch aufgerufen, sobald ein Firmware-Upload
// beginnt. Während des Uploads braucht der ESP8266 seinen Speicher und seine
// Rechenzeit für den Empfang der neuen Firmware -- deshalb wird hier alles
// andere sicherheitshalber stillgelegt: Notaus setzen (Sollwert geht auf 0),
// RS485 pausieren (kein Senden/Empfangen mehr) und den Software-Watchdog
// abschalten (sonst würde er den langsamen Upload fälschlich als "hängen
// geblieben" werten und den ESP mitten im Update neu starten).
static void onOTAStart() {
    // ElegantOTA ruft das bei JEDEM GET /ota/start auf, auch ohne dass danach
    // ein echter Upload folgt (z.B. ein im Browser offen gelassener/neu
    // geladener /update-Tab). War schon ein Versuch aktiv, darf ein weiterer
    // Aufruf den Timer unten NICHT neu starten -- sonst würde jeder erneute
    // Aufruf innerhalb der Timeout-Frist den Schutz immer wieder aufschieben,
    // und Notaus/RS485-Pause könnten im schlimmsten Fall unbegrenzt lange
    // aktiv bleiben, ohne dass je ein echter Upload passiert (real auf einem
    // Gerät beobachtet: mehrere Stunden Notaus, ohne dass es je manuell
    // gesetzt wurde). otaStartMillis wird deshalb nur beim allerersten Aufruf
    // gesetzt, der Timeout bleibt so eine echte Obergrenze.
    if (!g_otaActive) {
        otaStartMillis = millis();
    }
    g_notaus = true;
    rs485Pause(true); // setzt DE/RE bereits auf LOW (Empfang)
    ESP.wdtDisable();
    g_otaActive = true;
    LOG("OTA: Update gestartet");
}

// Wird aufgerufen, sobald der Upload fertig ist (erfolgreich oder nicht).
// Der ESP8266 startet danach in jedem Fall neu -- bei Erfolg bootet er mit
// der neuen Firmware, bei Misserfolg einfach wieder mit der alten.
static void onOTAEnd(bool success) {
    g_otaActive = false;
    LOG(success ? "OTA: Update erfolgreich" : "OTA: Update fehlgeschlagen");
    digitalWrite(RS485_DE_RE_PIN, HIGH);
    digitalWrite(RS485_RE_PIN, HIGH);
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

    // Siehe Kommentar bei otaActive oben: onOTAStart() ohne folgenden,
    // erfolgreich abgeschlossenen Upload würde die Regelung sonst für immer
    // lahmlegen. Kein Neustart nötig -- einfach den Zustand von vor dem
    // OTA-Start wiederherstellen, als wäre nie einer versucht worden.
    if (g_otaActive && millis() - otaStartMillis > OTA_TIMEOUT_MS) {
        g_otaActive = false;
        LOG("OTA: Timeout -- kein Upload angekommen, Regelung wird wieder freigegeben");
        g_notaus = false;
        rs485Pause(false);
        ESP.wdtEnable(0);
    }
}
