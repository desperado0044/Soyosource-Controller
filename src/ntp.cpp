#include "ntp.h"

#include <Arduino.h>
#include <time.h>
#include "config.h"

void ntpBegin() {
    configTime(3600, 3600, "pool.ntp.org"); // MEZ/MESZ (grobe Sommerzeit-Näherung per Offset)
}

bool isNightMode() {
    if (!config.night_mode_enabled) {
        return false;
    }

    time_t now = time(nullptr);
    if (now < 100000) {
        // Zeit noch nicht per NTP synchronisiert
        return false;
    }

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

    // Fenster über Mitternacht, z.B. 22:00 - 06:00
    return nowMinutes >= startMinutes || nowMinutes < endMinutes;
}
