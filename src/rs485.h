#pragma once

#include <Arduino.h>

#define RS485_DE_RE_PIN 14 // D5

// Operation-Status, Byte 4 der Status-Response.
enum SoyoOperationStatus : uint8_t {
    SOYO_OP_NORMAL           = 0x00,
    SOYO_OP_STARTUP          = 0x01,
    SOYO_OP_STANDBY          = 0x02,
    SOYO_OP_STARTUP_ABORTED  = 0x03,
    SOYO_OP_ERROR_BATTERY    = 0x04
};

struct SoyoStatus {
    bool valid;
    uint8_t operationStatus;
    float batteryVoltage;
    float batteryCurrent;
    float acVoltage;
    float frequency;
    float temperature;
    unsigned long lastUpdate;
};

extern int32_t   g_demand;        // aktuell effektiver (gerampter) Sollwert in Watt
extern bool      g_notaus;        // Notaus aktiv -> Demand sofort 0
extern bool      g_fallbackActive;
extern SoyoStatus g_soyoStatus;

void rs485Begin();
void rs485Loop();

// Setzt den gewünschten Zielsollwert. Begrenzung (max_power, Nachtmodus, ...)
// muss der Aufrufer bereits vorgenommen haben. Rampe (50W/Zyklus) und
// Sendelogik (nur bei Änderung >1W, alle 3000ms) übernimmt rs485Loop().
void rs485SetTargetDemand(int32_t watts);

// Für OTA: stoppt jegliches Senden und hält DE/RE auf Empfang (LOW).
void rs485Pause(bool pause);
