#include "rs485.h"

#include "telnet_log.h"

int32_t    g_demand = 0;
bool       g_notaus = false;
bool       g_fallbackActive = false;
SoyoStatus g_soyoStatus = {false, SOYO_OP_NORMAL, 0, 0, 0, 0, 0, 0};

static int32_t targetDemand = 0;
static int32_t lastSentDemand = -1; // erzwingt den ersten Sendevorgang
static bool    paused = false;

static unsigned long lastSendMillis = 0;
static const unsigned long SEND_INTERVAL_MS = 3000;

static unsigned long lastStatusRequestMillis = 0;
static const unsigned long STATUS_REQUEST_INTERVAL_MS = 10000;

enum TxPhase { TX_IDLE, TX_HOLD_DE };
static TxPhase txPhase = TX_IDLE;
static unsigned long txHoldStartMillis = 0;
static const unsigned long TX_HOLD_MS = 2;

// Antwort-Frame (15 Byte, Header 0x23) ist länger als die Sende-Frames (8 Byte,
// Header 0x24) und beginnt mit einem anderen Header-Byte.
static const uint8_t RX_FRAME_LEN = 15;
static const uint8_t RX_FRAME_HEADER = 0x23;
static uint8_t rxBuffer[RX_FRAME_LEN];
static uint8_t rxIndex = 0;

// Sanity-Grenzen für die Status-Response, um Fehl-Synchronisation auf dem
// Bus (verschobene/verstümmelte Frames) zu erkennen und zu verwerfen.
static const float SANITY_MAX_BATTERY_V = 150.0f;
static const float SANITY_MAX_AC_V = 300.0f;
static const float SANITY_MAX_TEMP_C = 200.0f;

static void setDE(bool high) {
    digitalWrite(RS485_DE_RE_PIN, high ? HIGH : LOW);
}

static void logFrameHex(const char *prefix, const uint8_t *frame, uint8_t len) {
    String hex;
    for (uint8_t i = 0; i < len; i++) {
        if (frame[i] < 0x10) hex += "0";
        hex += String(frame[i], HEX);
        hex += " ";
    }
    LOG((String(prefix) + hex).c_str());
}

static void sendFrame(const uint8_t *frame, uint8_t len) {
    setDE(true);
    Serial.write(frame, len);
    txPhase = TX_HOLD_DE;
    txHoldStartMillis = millis();
    logFrameHex("RS485 TX: ", frame, len);
}

static void sendDemandFrame(uint16_t watt) {
    uint8_t frame[8];
    frame[0] = 0x24;
    frame[1] = 0x56;
    frame[2] = 0x00;
    frame[3] = 0x21;
    frame[4] = (watt >> 8) & 0xFF;
    frame[5] = watt & 0xFF;
    frame[6] = 0x80;
    frame[7] = (uint8_t)((264 - frame[4] - frame[5]) & 0xFF);
    sendFrame(frame, 8);
}

static void sendStatusRequestFrame() {
    static const uint8_t frame[8] = {0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    sendFrame(frame, 8);
}

static void handleTxTiming() {
    if (txPhase == TX_HOLD_DE && millis() - txHoldStartMillis >= TX_HOLD_MS) {
        setDE(false);
        txPhase = TX_IDLE;
    }
}

// Status-Response-Layout (15 Byte):
//   [0]     0x23 Header
//   [1]     0x01
//   [2]     0x01
//   [3]     0x00 Reserved
//   [4]     Operation-Status (siehe SoyoOperationStatus)
//   [5..6]  Batteriespannung  uint16 BE, x0.1V
//   [7..8]  Batteriestrom     uint16 BE, x0.1A
//   [9..10] AC-Spannung       uint16 BE, x1V
//   [11]    AC-Frequenz       uint8,     x0.5Hz
//   [12..13] Temperatur       uint16 BE, (raw-300) x0.1°C
//   [14]    CRC (Algorithmus nicht bekannt, wird nicht geprüft)
//
// Hinweis: Neuere Geräte (purple mainboard, FW STC8-2022-218) antworten
// grundsätzlich nicht auf den Status-Request. Das ist kein Fehler -- es
// kommt einfach keine Response an, g_soyoStatus.valid bleibt false und die
// Soyo-Statuszeile im Webinterface bleibt leer/ausgeblendet.
static void parseStatusResponse(const uint8_t *frame) {
    if (frame[0] != RX_FRAME_HEADER) {
        LOG("RS485: Status-Response verworfen (falscher Header)");
        return;
    }

    uint16_t battRaw = (frame[5] << 8) | frame[6];
    uint16_t currRaw = (frame[7] << 8) | frame[8];
    uint16_t acRaw = (frame[9] << 8) | frame[10];
    uint16_t tempRaw = (frame[12] << 8) | frame[13];

    float batteryVoltage = battRaw * 0.1f;
    float acVoltage = acRaw * 1.0f;
    float temperature = ((int32_t)tempRaw - 300) * 0.1f;

    if (batteryVoltage >= SANITY_MAX_BATTERY_V ||
        acVoltage >= SANITY_MAX_AC_V ||
        temperature >= SANITY_MAX_TEMP_C) {
        LOG("RS485: Status-Response verworfen (Sanity-Check fehlgeschlagen)");
        return;
    }

    g_soyoStatus.operationStatus = frame[4];
    g_soyoStatus.batteryVoltage = batteryVoltage;
    g_soyoStatus.batteryCurrent = currRaw * 0.1f;
    g_soyoStatus.acVoltage = acVoltage;
    g_soyoStatus.frequency = frame[11] * 0.5f;
    g_soyoStatus.temperature = temperature;
    g_soyoStatus.valid = true;
    g_soyoStatus.lastUpdate = millis();

    LOG(("RS485: Status-Response empfangen (Status=" + String(g_soyoStatus.operationStatus) + ")").c_str());
}

static void applyDemandRamp() {
    if (g_notaus) {
        targetDemand = 0;
        g_demand = 0;
    } else {
        int32_t diff = targetDemand - g_demand;
        if (diff > 100) {
            g_demand += 50;
        } else if (diff < -100) {
            g_demand -= 50;
        } else {
            g_demand = targetDemand;
        }
    }

    if (lastSentDemand < 0 || abs(g_demand - lastSentDemand) > 1) {
        sendDemandFrame((uint16_t)g_demand);
        LOG(("RS485: Demand " + String(lastSentDemand) + "W -> " + String(g_demand) + "W").c_str());
        lastSentDemand = g_demand;
    }
}

void rs485Begin() {
    pinMode(RS485_DE_RE_PIN, OUTPUT);
    setDE(false);
    Serial.begin(4800, SERIAL_8N1);
}

void rs485Loop() {
    handleTxTiming();

    while (Serial.available()) {
        uint8_t b = Serial.read();
        if (rxIndex == 0 && b != RX_FRAME_HEADER) {
            continue; // auf Frame-Start (0x23) warten
        }
        rxBuffer[rxIndex++] = b;
        if (rxIndex >= RX_FRAME_LEN) {
            parseStatusResponse(rxBuffer);
            rxIndex = 0;
        }
    }

    if (paused) {
        return;
    }

    unsigned long now = millis();

    if (txPhase == TX_IDLE && now - lastStatusRequestMillis >= STATUS_REQUEST_INTERVAL_MS) {
        lastStatusRequestMillis = now;
        sendStatusRequestFrame();
        return;
    }

    if (now - lastSendMillis >= SEND_INTERVAL_MS) {
        lastSendMillis = now;
        applyDemandRamp();
    }
}

void rs485SetTargetDemand(int32_t watts) {
    if (watts < 0) watts = 0;
    targetDemand = watts;
}

void rs485Pause(bool pause) {
    paused = pause;
    if (pause) {
        setDE(false);
        txPhase = TX_IDLE;
        LOG("RS485: pausiert (OTA)");
    } else {
        LOG("RS485: fortgesetzt");
    }
}
