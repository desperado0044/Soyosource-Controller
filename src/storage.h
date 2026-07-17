#pragma once

#include "config.h"

bool storageBegin();
bool configFileExists();
bool loadConfig(Config &c);
bool saveConfig(const Config &c);

// Für Webinterface Config-Download (gleiches Schema wie die Datei).
String configToJsonString(const Config &c);

// Für Webinterface Config-Upload/Teil-Speicherung: merged die in json enthaltenen
// Felder in c hinein (nicht enthaltene Felder in c bleiben unverändert, siehe
// docToConfig in storage.cpp). outNeedsRestart wird auf true gesetzt, wenn ein
// Feld dabei war, das erst nach einem Neustart wirksam wird (WLAN/MQTT/OTA).
bool configMergeFromJsonString(Config &c, const String &json, bool &outNeedsRestart);

// Formatiert LittleFS und startet neu. Kehrt nicht zurück.
void factoryReset();
