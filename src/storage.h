#pragma once

#include "config.h"

bool storageBegin();
bool configFileExists();
bool loadConfig(Config &c);
bool saveConfig(const Config &c);

// Für Webinterface Config-Download/-Upload (gleiches Schema wie die Datei).
String configToJsonString(const Config &c);
bool configFromJsonString(Config &c, const String &json);

// Formatiert LittleFS und startet neu. Kehrt nicht zurück.
void factoryReset();
