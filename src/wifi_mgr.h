#pragma once

#include <Arduino.h>

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
