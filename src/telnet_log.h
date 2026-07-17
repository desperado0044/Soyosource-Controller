#pragma once

#include <Arduino.h>
#include <TelnetStream.h>

#define LOG_RINGBUFFER_SIZE 20
#define LOG_ENTRY_MAXLEN    80

void telnetLogBegin();
void telnetLogLoop();
void addToRingbuffer(const String &msg);
String getRingbufferJson();

// LOG(...) ist ein Makro (kein normaler Funktionsaufruf) und ist das einzige
// Logging-Werkzeug in diesem Projekt -- überall im Code steht LOG("...") statt
// Serial.print(...), weil der Hardware-UART für RS485 reserviert ist.
// "do { ... } while (0)" ist ein gängiger C/C++-Trick, damit sich das Makro
// nach dem Einsetzen exakt wie ein einzelner Funktionsaufruf verhält (auch
// z.B. innerhalb eines if ohne geschweifte Klammern) -- ohne den Trick könnte
// es je nach Aufrufstelle zu subtilen Bugs kommen.
#define LOG(msg) do { \
    TelnetStream.println(msg); \
    addToRingbuffer(String(msg)); \
} while (0)
