// Der ESP8266 hat (in diesem Projekt) keinen per USB-Kabel angeschlossenen PC,
// über den man klassisch per Serial-Monitor mitlesen könnte -- der Hardware-
// UART ist ja für RS485 reserviert (siehe rs485.h). Stattdessen kann man sich
// per Telnet (z.B. `telnet soyo.local`) auf Port 23 mit dem Gerät verbinden
// und bekommt dort dieselben Log-Zeilen live zu sehen, die sonst auf dem
// Serial-Monitor stehen würden. Das LOG(...)-Makro (siehe telnet_log.h) ist
// der einzige Weg, wie im Projekt geloggt wird.
#include "telnet_log.h"
#include <ArduinoJson.h>

// Zusätzlich zum Telnet-Live-Log merkt sich die Firmware die letzten
// LOG_RINGBUFFER_SIZE Zeilen auch im RAM, damit man sie über /log auch ohne
// aktive Telnet-Verbindung nachträglich abrufen kann (z.B. im Webinterface).
// Das ist ein "Ringpuffer" (auch "Rundpuffer"/"Circular Buffer"): statt bei
// vollem Puffer alles nach vorne zu verschieben (teuer), wird einfach der
// älteste Eintrag durch den neuen überschrieben. ringHead zeigt auf den
// Index des ältesten noch gültigen Eintrags, ringCount auf die Anzahl der
// aktuell belegten Plätze. Der %-Operator (Rest der Division) sorgt dafür,
// dass der Index nach dem letzten Platz wieder bei 0 weiterzählt, statt über
// das Array hinauszulaufen -- so entsteht der "Ring".
static String ringbuffer[LOG_RINGBUFFER_SIZE];
static uint8_t ringHead = 0;   // Index des ältesten Eintrags
static uint8_t ringCount = 0;  // wie viele Plätze aktuell belegt sind (max. LOG_RINGBUFFER_SIZE)

void telnetLogBegin() {
    TelnetStream.begin(23); // 23 ist der Standard-Port für Telnet
}

void telnetLogLoop() {
    // TelnetStreamClass benötigt kein Polling: Verbindungsannahme geschieht
    // lazy in read()/available(), die von LOG() bzw. println() ausgelöst werden.
}

void addToRingbuffer(const String &msg) {
    String entry = msg;
    if (entry.length() > LOG_ENTRY_MAXLEN) {
        entry = entry.substring(0, LOG_ENTRY_MAXLEN);
    }

    uint8_t writeIndex = (ringHead + ringCount) % LOG_RINGBUFFER_SIZE;
    if (ringCount < LOG_RINGBUFFER_SIZE) {
        ringCount++; // Puffer war noch nicht voll: ein Platz mehr belegt
    } else {
        ringHead = (ringHead + 1) % LOG_RINGBUFFER_SIZE; // Puffer war voll: ältesten Eintrag "vergessen"
    }
    ringbuffer[writeIndex] = entry;
}

String getRingbufferJson() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (uint8_t i = 0; i < ringCount; i++) {
        uint8_t idx = (ringHead + i) % LOG_RINGBUFFER_SIZE;
        arr.add(ringbuffer[idx]);
    }

    String out;
    serializeJson(doc, out);
    return out;
}
