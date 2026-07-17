#pragma once

void ntpBegin();

// true, wenn Nachtmodus aktiviert ist und die aktuelle Uhrzeit im
// konfigurierten Fenster (night_start..night_end, ggf. über Mitternacht) liegt.
bool isNightMode();
