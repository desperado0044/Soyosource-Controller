#include "wifi_mgr.h"

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>

#include "config.h"
#include "telnet_log.h"

static const unsigned long CONNECT_TIMEOUT_MS = 15000;
static const unsigned long RECONNECT_INTERVAL_MS = 10000;
static const uint8_t MAX_FAILS_BEFORE_FORCE = 5;

static WifiState state = WIFI_STATE_AP_MODE;
static DNSServer dnsServer;
static unsigned long lastAttemptMillis = 0;
static uint8_t failCount = 0;

static void startApMode() {
    WiFi.mode(WIFI_AP);
    IPAddress apIP(10, 0, 0, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    dnsServer.start(53, "*", apIP);
    state = WIFI_STATE_AP_MODE;
    LOG("WiFi: AP-Modus gestartet (SOYO-Setup, 10.0.0.1)");
}

static void startStaConnect() {
    WiFi.mode(WIFI_STA);
    if (config.wifi_11n) {
        WiFi.setPhyMode(WIFI_PHY_MODE_11N);
    }
    if (config.wifi_static) {
        IPAddress ip, gw, mask;
        ip.fromString(config.wifi_ip);
        gw.fromString(config.wifi_gw);
        mask.fromString(config.wifi_mask);
        WiFi.config(ip, gw, mask);
    }
    WiFi.begin(config.wifi_ssid, config.wifi_pass);
    state = WIFI_STATE_STA_CONNECTING;
    lastAttemptMillis = millis();
    failCount = 0;
    LOG(("WiFi: Verbinde mit " + String(config.wifi_ssid)).c_str());
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

void wifiMgrLoop() {
    if (state == WIFI_STATE_AP_MODE) {
        dnsServer.processNextRequest();
        return;
    }

    MDNS.update();
    unsigned long now = millis();

    switch (state) {
        case WIFI_STATE_STA_CONNECTING:
            if (WiFi.status() == WL_CONNECTED) {
                state = WIFI_STATE_STA_CONNECTED;
                failCount = 0;
                LOG(("WiFi: verbunden, IP " + WiFi.localIP().toString()).c_str());
                if (MDNS.begin("soyo")) {
                    LOG("mDNS: soyo.local aktiv");
                }
            } else if (now - lastAttemptMillis >= CONNECT_TIMEOUT_MS) {
                failCount++;
                LOG(("WiFi: Verbindungsversuch fehlgeschlagen (" + String(failCount) + "/5)").c_str());
                if (failCount >= MAX_FAILS_BEFORE_FORCE) {
                    LOG("WiFi: Erzwinge vollständigen Reconnect");
                    WiFi.disconnect();
                    failCount = 0;
                }
                WiFi.begin(config.wifi_ssid, config.wifi_pass);
                lastAttemptMillis = now;
            }
            break;

        case WIFI_STATE_STA_CONNECTED:
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
                LOG(("WiFi: wiederverbunden, IP " + WiFi.localIP().toString()).c_str());
            } else if (now - lastAttemptMillis >= RECONNECT_INTERVAL_MS) {
                lastAttemptMillis = now;
                failCount++;
                LOG(("WiFi: Reconnect-Versuch (" + String(failCount) + "/5)").c_str());
                if (failCount >= MAX_FAILS_BEFORE_FORCE) {
                    LOG("WiFi: Erzwinge vollständigen Reconnect");
                    WiFi.disconnect();
                    failCount = 0;
                }
                WiFi.begin(config.wifi_ssid, config.wifi_pass);
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
