#include "wifi_mgr.h"

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>

#include "config.h"
#include "telnet_log.h"

static const unsigned long CONNECT_TIMEOUT_MS = 15000;
static const unsigned long RECONNECT_INTERVAL_MS = 10000;
static const uint8_t MAX_FAILS_BEFORE_FORCE = 5;
// Wie lange auf dem Fallback-WLAN gelaufen wird, bevor kurz getestet wird, ob
// das primaere WLAN wieder erreichbar ist. Nicht zu kurz, da so ein Test die
// gerade funktionierende Fallback-Verbindung kurz unterbricht.
static const unsigned long PRIMARY_RECOVERY_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 Minuten

static WifiState state = WIFI_STATE_AP_MODE;
static DNSServer dnsServer;
static unsigned long lastAttemptMillis = 0;
static unsigned long lastPrimaryRecoveryMillis = 0;
static uint8_t failCount = 0;
// false = gerade am primaeren WLAN (oben in der Konfiguration) dran, true =
// am Fallback-WLAN. Das primaere WLAN wird immer bevorzugt -- siehe
// PRIMARY_RECOVERY_INTERVAL_MS oben.
static bool usingSecondary = false;

static bool hasSecondarySsid() {
    return strlen(config.wifi_ssid2) > 0;
}

// Startet einen eigenen WLAN-Access-Point ("SOYO-Setup"), zu dem man sich mit
// dem Handy verbindet, um das Gerät erstmalig einzurichten. Der DNSServer
// beantwortet dabei JEDE Namensauflösungsanfrage ("*") mit der eigenen IP
// 10.0.0.1 -- das ist der Trick hinter einem "Captive Portal": das Handy denkt,
// es müsste sich erst irgendwo einloggen, und öffnet von selbst eine
// Browser-Seite, die dann direkt auf unserem Webinterface landet.
static void startApMode() {
    WiFi.mode(WIFI_AP);
    IPAddress apIP(10, 0, 0, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    dnsServer.start(53, "*", apIP);
    state = WIFI_STATE_AP_MODE;
    LOG("WiFi: AP-Modus gestartet (SOYO-Setup, 10.0.0.1)");
}

// mDNS (soyo.local) muss nach JEDEM neuen Verbindungsaufbau erneut gestartet
// werden, nicht nur beim allerersten Mal: nach einem Verbindungsabbruch und
// -wiederaufbau (WIFI_STATE_STA_RECONNECTING -> WIFI_STATE_STA_CONNECTED)
// kann sich z.B. die IP-Adresse geändert haben, und der mDNS-Responder bleibt
// sonst an die alte, nicht mehr gültige Verbindung gebunden -- soyo.local
// wäre dann bis zum nächsten Neustart nicht mehr erreichbar.
static void startMdns() {
    if (MDNS.begin("soyo")) {
        LOG("mDNS: soyo.local aktiv");
    }
}

// Die konfigurierte feste IP gilt nur fuer das primaere WLAN -- das
// Fallback-Netz ist typischerweise ein ganz anderes Netzwerk (z.B. ein
// Handy-Hotspot) mit einem anderen Adressbereich, in dem diese feste IP gar
// nicht passen wuerde. Wird aber NUR aufgerufen, wenn ueberhaupt eine feste
// IP aktiv ist (wifi_static) -- in den meisten Setups (DHCP) macht diese
// Funktion also gar nichts, WiFi.config() wird dann bei jedem Reconnect-
// Versuch gar nicht erst angefasst. Wichtig: ein wiederholtes WiFi.config()
// bei jedem einzelnen Reconnect-Versuch (auch nur um DHCP zu "bestaetigen")
// hat sich auf dem ESP8266 als Absturzursache erwiesen (Neustart mitten im
// Betrieb bei laengeren Reconnect-Phasen) -- deshalb hier bewusst so
// zurueckhaltend wie moeglich.
static void applyIpConfigFor(bool secondary) {
    if (!config.wifi_static) return;
    if (!secondary) {
        IPAddress ip, gw, mask;
        ip.fromString(config.wifi_ip);
        gw.fromString(config.wifi_gw);
        mask.fromString(config.wifi_mask);
        WiFi.config(ip, gw, mask);
    } else {
        // Zurueck zum Fallback-Netz wechseln, waehrend eine feste IP fuer
        // das primaere Netz aktiv ist -- die feste IP passt dort nicht,
        // also zurueck auf normales DHCP.
        WiFi.config(0U, 0U, 0U);
    }
}

// Beginnt einen neuen Verbindungsversuch zum primaeren (secondary=false) oder
// zum Fallback-WLAN (secondary=true).
static void beginConnectAttempt(bool secondary) {
    usingSecondary = secondary;
    applyIpConfigFor(secondary);
    const char *ssid = secondary ? config.wifi_ssid2 : config.wifi_ssid;
    const char *pass = secondary ? config.wifi_pass2 : config.wifi_pass;
    WiFi.begin(ssid, pass);
    lastAttemptMillis = millis();
    LOG(("WiFi: Verbinde mit " + String(ssid) + (secondary ? " (Fallback)" : "")).c_str());
}

static void startStaConnect() {
    WiFi.mode(WIFI_STA);
    if (config.wifi_11n) {
        WiFi.setPhyMode(WIFI_PHY_MODE_11N);
    }
    state = WIFI_STATE_STA_CONNECTING;
    failCount = 0;
    beginConnectAttempt(false); // Boot versucht immer zuerst das primaere WLAN
}

void wifiMgrBegin() {
    WiFi.persistent(false);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);

    if (strlen(config.wifi_ssid) == 0) {
        startApMode();
    } else {
        startStaConnect();
    }
}

// Wird bei jedem Schleifendurchlauf aus der Haupt-loop() (main.cpp) aufgerufen.
// Statt auf ein Verbindungs-Event zu warten, wird hier bei jedem Aufruf einfach
// kurz nachgeschaut, was WiFi.status() gerade sagt ("Polling") -- einfacher zu
// verstehen als Callback-basierte Events und für dieses Projekt ausreichend
// schnell.
void wifiMgrLoop() {
    if (state == WIFI_STATE_AP_MODE) {
        dnsServer.processNextRequest(); // beantwortet DNS-Anfragen für das Captive Portal
        return;
    }

    MDNS.update(); // hält "soyo.local" im lokalen Netz auffindbar
    unsigned long now = millis();

    switch (state) {
        case WIFI_STATE_STA_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                state = WIFI_STATE_STA_CONNECTED;
                failCount = 0;
                lastPrimaryRecoveryMillis = now;
                LOG(("WiFi: verbunden, IP " + WiFi.localIP().toString() +
                     (usingSecondary ? " (Fallback)" : "")).c_str());
                startMdns();
            } else if (now - lastAttemptMillis >= CONNECT_TIMEOUT_MS) {
                // Nach CONNECT_TIMEOUT_MS ohne Erfolg: einfach nochmal versuchen,
                // aber -- falls ein Fallback-WLAN konfiguriert ist -- dabei
                // jedes Mal auf das jeweils andere WLAN wechseln, statt stur
                // immer wieder dasselbe zu probieren. failCount zaehlt dabei
                // Fehlversuche insgesamt (unabhaengig vom WLAN) weiter; erst
                // nach mehreren Fehlversuchen in Folge (MAX_FAILS_BEFORE_FORCE)
                // wird zusätzlich ein WiFi.disconnect() davorgeschaltet, das den
                // WLAN-Stack des ESP8266 komplett zurücksetzt -- das hilft in
                // hartnäckigen Fällen, ist aber etwas "brachialer" als ein
                // normales erneutes WiFi.begin().
                failCount++;
                LOG(("WiFi: Verbindungsversuch fehlgeschlagen (" + String(failCount) + "/5)").c_str());
                if (failCount >= MAX_FAILS_BEFORE_FORCE) {
                    LOG("WiFi: Erzwinge vollständigen Reconnect");
                    WiFi.disconnect();
                    failCount = 0;
                }
                beginConnectAttempt(hasSecondarySsid() ? !usingSecondary : usingSecondary);
            }
            break;

        case WIFI_STATE_STA_CONNECTED:
            // Bewusst KEIN aktiver periodischer Test, ob das primaere WLAN
            // wieder erreichbar ist, waehrend stabil auf dem Fallback-WLAN
            // gelaufen wird -- das wuerde eine bereits funktionierende,
            // aktive Verbindung unterbrechen, nur um testweise umzuschalten.
            // Genau das hat sich als Absturzursache erwiesen (Neustart mitten
            // im Betrieb). Stattdessen wird das primaere WLAN einfach beim
            // naechsten echten Verbindungsverlust wieder mitversucht (siehe
            // die Alternier-Logik oben in STA_CONNECTING/STA_RECONNECTING) --
            // dann ist ohnehin schon nichts mehr verbunden, es gibt also
            // nichts zu unterbrechen.
            if (WiFi.status() != WL_CONNECTED) {
                state = WIFI_STATE_STA_RECONNECTING;
                lastAttemptMillis = now;
                failCount = 0;
                LOG("WiFi: Verbindung verloren, Reconnecting");
            }
            break;

        case WIFI_STATE_STA_RECONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                state = WIFI_STATE_STA_CONNECTED;
                failCount = 0;
                lastPrimaryRecoveryMillis = now;
                LOG(("WiFi: wiederverbunden, IP " + WiFi.localIP().toString() +
                     (usingSecondary ? " (Fallback)" : "")).c_str());
                startMdns();
            } else if (now - lastAttemptMillis >= RECONNECT_INTERVAL_MS) {
                lastAttemptMillis = now;
                failCount++;
                LOG(("WiFi: Reconnect-Versuch (" + String(failCount) + "/5)").c_str());
                if (failCount >= MAX_FAILS_BEFORE_FORCE) {
                    LOG("WiFi: Erzwinge vollständigen Reconnect");
                    WiFi.disconnect();
                    failCount = 0;
                }
                beginConnectAttempt(hasSecondarySsid() ? !usingSecondary : usingSecondary);
            }
            break;

        default:
            break;
    }
}

WifiState wifiMgrGetState() {
    return state;
}

bool wifiMgrIsApMode() {
    return state == WIFI_STATE_AP_MODE;
}
