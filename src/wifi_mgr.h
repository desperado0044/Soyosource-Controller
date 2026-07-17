#pragma once

#include <Arduino.h>

// Der WLAN-Verbindungsaufbau passiert nicht in einem Rutsch, sondern über
// mehrere loop()-Durchläufe verteilt (kein delay()/while(), das würde die
// ganze Firmware blockieren). Deshalb merkt sich wifi_mgr.cpp, in welcher
// "Phase" es gerade ist:
//   AP_MODE            -> Gerät ist eigener Access Point (Ersteinrichtung),
//                          kein Router-WLAN verbunden
//   STA_CONNECTING     -> versucht sich gerade zum konfigurierten WLAN zu
//                          verbinden (Erstverbindung nach dem Boot)
//   STA_CONNECTED      -> normal verbunden
//   STA_RECONNECTING   -> Verbindung war da, ist weg, versucht wiederzukommen
enum WifiState : uint8_t {
    WIFI_STATE_AP_MODE,
    WIFI_STATE_STA_CONNECTING,
    WIFI_STATE_STA_CONNECTED,
    WIFI_STATE_STA_RECONNECTING
};

#define WIFI_AP_SSID "SOYO-Setup"
#define WIFI_AP_PASS "1234567890"

void wifiMgrBegin();
void wifiMgrLoop();
WifiState wifiMgrGetState();
bool wifiMgrIsApMode();
