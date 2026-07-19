#pragma once

#include <ESP8266WebServer.h>

// true zwischen onOTAStart() und onOTAEnd()/Timeout -- andere Module (siehe
// shellyLoop() in shelly.cpp) prüfen das, um während eines laufenden Uploads
// keine neue HTTP-Anfrage mehr zu starten, statt dem ESP8266 zusätzlich
// zum Flash-Schreiben noch Netzwerk-Traffic aufzubürden.
extern bool g_otaActive;

void otaBegin(ESP8266WebServer &server);
void otaLoop();
