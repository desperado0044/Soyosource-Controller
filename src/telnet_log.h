#pragma once

#include <Arduino.h>
#include <TelnetStream.h>

#define LOG_RINGBUFFER_SIZE 20
#define LOG_ENTRY_MAXLEN    80

void telnetLogBegin();
void telnetLogLoop();
void addToRingbuffer(const String &msg);
String getRingbufferJson();

#define LOG(msg) do { \
    TelnetStream.println(msg); \
    addToRingbuffer(String(msg)); \
} while (0)
