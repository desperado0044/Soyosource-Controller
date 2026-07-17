#include "led.h"

#include <Arduino.h>
#include "wifi_mgr.h"
#include "rs485.h"

enum LedPattern {
    LED_SLOW_BLINK,   // AP-Modus: 1s an, 1s aus
    LED_FAST_BLINK,   // Verbindend/Reconnecting: 100ms an/aus
    LED_SOLID_ON,     // Normal verbunden
    LED_DOUBLE_BLINK  // Fallback aktiv: 2x kurz blinken, dann Pause
};

static unsigned long lastChangeMillis = 0;
static bool ledOn = false;
static uint8_t doubleBlinkStep = 0; // 0..3: an,aus,an,aus, dann Pause

static void applyLed(bool on) {
    ledOn = on;
    digitalWrite(LED_PIN, on ? LOW : HIGH); // active LOW
}

void ledBegin() {
    pinMode(LED_PIN, OUTPUT);
    applyLed(false);
}

void ledLoop() {
    unsigned long now = millis();

    LedPattern pattern;
    if (wifiMgrIsApMode()) {
        pattern = LED_SLOW_BLINK;
    } else if (g_fallbackActive) {
        pattern = LED_DOUBLE_BLINK;
    } else if (wifiMgrGetState() == WIFI_STATE_STA_CONNECTING || wifiMgrGetState() == WIFI_STATE_STA_RECONNECTING) {
        pattern = LED_FAST_BLINK;
    } else {
        pattern = LED_SOLID_ON;
    }

    switch (pattern) {
        case LED_SOLID_ON:
            if (!ledOn) applyLed(true);
            break;

        case LED_SLOW_BLINK:
            if (now - lastChangeMillis >= 1000) {
                applyLed(!ledOn);
                lastChangeMillis = now;
            }
            break;

        case LED_FAST_BLINK:
            if (now - lastChangeMillis >= 100) {
                applyLed(!ledOn);
                lastChangeMillis = now;
            }
            break;

        case LED_DOUBLE_BLINK: {
            // Schrittfolge: an(100)-aus(100)-an(100)-aus(700 Pause)
            unsigned long stepDuration;
            switch (doubleBlinkStep) {
                case 0: stepDuration = 100; break;
                case 1: stepDuration = 100; break;
                case 2: stepDuration = 100; break;
                default: stepDuration = 700; break;
            }
            if (now - lastChangeMillis >= stepDuration) {
                lastChangeMillis = now;
                doubleBlinkStep = (doubleBlinkStep + 1) % 4;
                applyLed(doubleBlinkStep == 0 || doubleBlinkStep == 2);
            }
            break;
        }
    }
}
