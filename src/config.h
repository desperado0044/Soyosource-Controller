#pragma once

#include <Arduino.h>

// Betriebsmodi der Regelung
enum OperatingMode : uint8_t {
    MODE_STATIC          = 0, // konstanter Sollwert = static_watt
    MODE_HTTP_INTERFACE  = 1, // Messwert per GET /L1L2L3Auto?Value=x
    MODE_MQTT_SUB        = 2, // Messwert per MQTT-Subscribe
    MODE_SHELLY_GEN1     = 3, // Shelly Gen1 /status total_power
    MODE_SHELLY_GEN2     = 4, // Shelly Gen2 Pro /rpc/EM.GetStatus
    MODE_JSON_HTTP       = 5  // generischer JSON-HTTP-Client
};

// Alle Einstellungen des Geräts in einem einzigen Behälter (struct). Es gibt
// genau ein Config-Objekt im ganzen Projekt (siehe "extern Config config;"
// unten und dessen Definition in main.cpp) — jedes Modul, das eine Einstellung
// braucht, greift auf dasselbe Objekt zu.
//
// Texte werden bewusst als feste char-Arrays (z.B. char wifi_ssid[64])
// gespeichert statt als Arduino-String: die Größe steht damit von vornherein
// fest, was zum 1:1 passenden JSON-Format in /config.json passt und
// Speicherfragmentierung auf dem ESP8266 vermeidet (String wächst/schrumpft
// dynamisch auf dem Heap, was auf Dauer instabil werden kann).
struct Config {
    // WiFi
    char wifi_ssid[64];
    char wifi_pass[64];
    bool wifi_static;      // true = feste IP unten verwenden statt DHCP
    char wifi_ip[16];
    char wifi_gw[16];
    char wifi_mask[16];
    bool wifi_11n;          // erzwingt WLAN-Standard 802.11n (hilft bei manchen Routern/Störungen)

    // MQTT
    bool     mqtt_enabled;
    char     mqtt_broker[64];
    uint16_t mqtt_port;
    char     mqtt_user[32];
    char     mqtt_pass[32];
    char     mqtt_sub_topic[64];
    char     mqtt_pub_topic[64];

    // Betriebsmodus
    uint8_t mode;
    uint16_t static_watt; // Sollwert im Static-Modus (mode=0)

    // Shelly / JSON HTTP Client
    char shelly_ip[32];
    bool shelly_l1, shelly_l2, shelly_l3; // nur Gen2 wirksam
    uint16_t poll_interval_ms; // Abfrageintervall Shelly/JSON-HTTP, 400-2000ms

    // JSON HTTP Client
    char json_url[128];
    char json_path[64]; // Punkt-separiert, z.B. "StatusSNS.SML.DJ_TPWRCURR"

    // Regelung
    uint16_t max_power;      // Obergrenze für den Sollwert in Watt (Sicherheitslimit)
    uint8_t  soyo_count;     // Anzahl parallel angeschlossener Soyo-Geräte (1-12);
                             // der Messwert wird durch diese Zahl geteilt, bevor er
                             // auf den Sollwert aufaddiert wird (siehe main.cpp)
    int16_t  offset;         // wird zu jedem Messwert addiert, um z.B. einen leicht
                             // falsch kalibrierten Stromzähler auszugleichen
    uint16_t fallback_watt;  // Sollwert, wenn die Messwertquelle wiederholt ausfällt
    uint16_t rs485_send_interval_ms; // Wie oft ein neuer Sollwert an den Soyo gesendet
                             // wird, 1000-3000ms, Standard 2000ms. Ein Referenzcontroller
                             // (BavarianSuperGuy/KlausLi) mit identischer Hardware läuft
                             // empirisch bestätigt stabil bei ~2000ms -- dieser Bereich
                             // eignet sich für eigene Testreihen.

    // Nachtmodus: begrenzt die maximale Leistung in einem Zeitfenster, z.B. um
    // nachts leiser/schonender zu fahren
    bool     night_mode_enabled;
    uint8_t  night_start_h, night_start_m; // Beginn, z.B. 22:00
    uint8_t  night_end_h,   night_end_m;   // Ende, z.B. 06:00 (darf über Mitternacht gehen)
    uint16_t night_max_power;

    // OTA
    char ota_pass[32];
};

extern Config config;

// Füllt c mit den Werkseinstellungen.
void configSetDefaults(Config &c);
