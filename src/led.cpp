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

// Die eingebaute LED des NodeMCU ist "active LOW" verdrahtet: Der Pin muss auf
// LOW gezogen werden, damit die LED leuchtet, und auf HIGH, damit sie aus ist
// -- genau umgekehrt zu dem, was man intuitiv erwarten würde. Damit man sich
// im restlichen Code (ledLoop unten) nicht ständig merken muss "LOW=an",
// kapselt applyLed() das: von außen ruft man einfach applyLed(true) für "an".
static void applyLed(bool on) {
    ledOn = on;
    digitalWrite(LED_PIN, on ? LOW : HIGH);
}

void ledBegin() {
    pinMode(LED_PIN, OUTPUT);
    applyLed(false);
}

// Wird bei jedem Schleifendurchlauf aus der Haupt-loop() (main.cpp) aufgerufen.
// Erst wird entschieden, WELCHES Blinkmuster gerade gilt (Prioritätsreihenfolge:
// AP-Setup > Fallback > WLAN-Problem > normal verbunden), danach wird nur für
// dieses eine Muster geprüft, ob genug Zeit für den nächsten An/Aus-Wechsel
// vergangen ist. Wie überall in diesem Projekt: kein delay(), nur millis()-
// Zeitstempel, damit die LED "nebenbei" blinkt, während loop() weiterläuft.
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
            // doubleBlinkStep zählt 0,1,2,3,0,1,2,3,... durch (der %4-Operator
            // sorgt dafür, dass nach 3 wieder bei 0 begonnen wird). Jeder Schritt
            // hat seine eigene Dauer und An/Aus-Zustand -- zusammen ergibt das
            // die Sequenz an(100)-aus(100)-an(100)-aus(700, Pause).
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
