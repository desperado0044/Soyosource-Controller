#pragma once

#include <Arduino.h>

extern float         g_netzwert;              // letzter Messwert in Watt, inkl. offset
extern unsigned long g_lastMeasurementMillis;

void shellyBegin();

// Pollt je nach config.mode Shelly Gen1 / Gen2 / generischen JSON-HTTP-Client
// alle 1000ms, aktualisiert g_netzwert und steuert die Fallback-Logik.
// Wirkt nur in den Modi MODE_SHELLY_GEN1, MODE_SHELLY_GEN2, MODE_JSON_HTTP.
void shellyLoop();

// Von HttpInterface- (/L1L2L3Auto) und MQTT-Modus aufgerufen, um einen extern
// gelieferten Rohmesswert (ohne Offset) einzuspeisen.
void applyMeasurement(float rawWatt);
