// Der ESP8266 hat keine eigene batteriegepufferte Echtzeituhr -- nach jedem
// Neustart "weiß" er zunächst nicht, welche Uhrzeit gerade ist. NTP (Network
// Time Protocol) holt sich die aktuelle Uhrzeit von einem Zeitserver im
// Internet. Danach liefert die ganz normale C-Funktion time() die korrekte
// Uhrzeit, ganz ohne dass man sich noch um NTP kümmern müsste.
#include "ntp.h"

#include <Arduino.h>
#include <time.h>
#include "config.h"

// Die beiden Zahlen-Parameter sind Zeitzonen-Offsets in Sekunden: 3600 (=1h)
// für die normale Zeitzone (MEZ), nochmal 3600 als Sommerzeit-Zuschlag. Damit
// stimmt die Uhrzeit ganzjährig ungefähr für Deutschland/Mitteleuropa; eine
// exakte automatische Sommerzeit-Umstellung wäre komplizierter und ist für
// den Nachtmodus (auf 1 Minute genau reicht) nicht nötig.
void ntpBegin() {
    configTime(3600, 3600, "pool.ntp.org");
}

// true, wenn der Nachtmodus aktiv ist UND die aktuelle Uhrzeit gerade
// innerhalb des konfigurierten Zeitfensters liegt.
bool isNightMode() {
    if (!config.night_mode_enabled) {
        return false;
    }

    time_t now = time(nullptr);
    if (now < 100000) {
        // Sehr kleiner Zeitwert = "1. Januar 1970, kurz nach Mitternacht" =
        // die typische Ausgangszeit, solange noch keine NTP-Antwort da war.
        // In dem Fall lieber "kein Nachtmodus" annehmen, als mit einer
        // offensichtlich falschen Uhrzeit zu rechnen.
        return false;
    }

    // Uhrzeit in "Minuten seit Mitternacht" umrechnen (z.B. 22:30 -> 1350).
    // Das macht den Vergleich von Uhrzeiten unten zu einfacher Zahlen-
    // Arithmetik, statt Stunden und Minuten einzeln vergleichen zu müssen.
    struct tm timeInfo;
    localtime_r(&now, &timeInfo);
    uint16_t nowMinutes = timeInfo.tm_hour * 60 + timeInfo.tm_min;
    uint16_t startMinutes = config.night_start_h * 60 + config.night_start_m;
    uint16_t endMinutes = config.night_end_h * 60 + config.night_end_m;

    if (startMinutes == endMinutes) {
        return false;
    }

    if (startMinutes < endMinutes) {
        // Fenster innerhalb desselben Tages, z.B. 08:00 - 18:00
        return nowMinutes >= startMinutes && nowMinutes < endMinutes;
    }

    // Fenster geht über Mitternacht, z.B. 22:00 - 06:00: "nachts" ist dann
    // entweder noch VOR Mitternacht ab 22:00, ODER schon NACH Mitternacht vor
    // 06:00 -- deshalb "oder" (||) statt "und" wie im Fall oberhalb.
    return nowMinutes >= startMinutes || nowMinutes < endMinutes;
}
