#pragma once

// I2C-Pins fuer das SSD1306-OLED (128x64). Das sind die von der ESP8266-
// Arduino-Wire-Library ohnehin als Standard verwendeten GPIOs (D2/D1 auf dem
// NodeMCU-Board) -- an anderer Stelle im Projekt unbenutzt (RS485 sitzt auf
// D5/D6, die eingebaute LED auf GPIO2).
#define DISPLAY_SDA_PIN 4 // D2
#define DISPLAY_SCL_PIN 5 // D1

// Wird nur hier fuer den Splashscreen angezeigt, hat sonst keine Funktion --
// bei einem neuen Firmware-Stand einfach hochzaehlen, dann sieht man nach
// einem OTA-Update am Geraet selbst sofort, ob das Update angekommen ist.
#define DISPLAY_FW_VERSION "1.0"

void displayBegin();
void displayLoop();
