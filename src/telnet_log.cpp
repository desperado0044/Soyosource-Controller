#include "telnet_log.h"
#include <ArduinoJson.h>

static String ringbuffer[LOG_RINGBUFFER_SIZE];
static uint8_t ringHead = 0;   // Index des ältesten Eintrags
static uint8_t ringCount = 0;

void telnetLogBegin() {
    TelnetStream.begin(23);
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
        ringCount++;
    } else {
        ringHead = (ringHead + 1) % LOG_RINGBUFFER_SIZE; // ältesten überschreiben
    }
    ringbuffer[writeIndex] = entry;
}

String getRingbufferJson() {
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.to<JsonArray>();

    for (uint8_t i = 0; i < ringCount; i++) {
        uint8_t idx = (ringHead + i) % LOG_RINGBUFFER_SIZE;
        arr.add(ringbuffer[idx]);
    }

    String out;
    serializeJson(doc, out);
    return out;
}
