# Soyosource GTN1000/1200 Nulleinspeisungsregler

ESP8266 (NodeMCU v2) Firmware für einen Soyosource GTN1000/1200 Nulleinspeisungsregler.
Ersetzt die Firmware von [BavarianSuperGuy/Esp-Soyosource-Controller](https://github.com/KlausLi/Esp-Soyosource-Controller)
mit non-blocking, modularem Code.

## Über dieses Projekt

Dieses Projekt ist ein quelloffener Nachbau und eine Weiterentwicklung der Firmware
von [BavarianSuperGuy (KlausLi)](https://github.com/KlausLi/Esp-Soyosource-Controller),
dessen Arbeit die Grundlage für dieses Projekt bildet.

Der Quellcode seiner Firmware ist nicht verfügbar. Dieses Projekt implementiert
denselben Funktionsumfang vollständig neu mit folgenden Verbesserungen:

- **Non-blocking WiFi**: Kein blockierendes Reconnect — stabile Verbindung auch bei
  schwachem Signal (BSGs bekanntes Problem: ESP hängt beim Reconnect, WLAN bricht weg)
- **Kein versehentlicher Werksreset**: BSG nutzt Doppelreset-Mechanismus — kurzer
  Stromausfall kann Konfiguration löschen. Hier: nur GPIO0 beim Boot (5s halten)
- **Vollständig quelloffen**: Jede Zeile Code einsehbar, anpassbar, verbesserbar
- **Non-blocking HTTP**: Shelly-Polling blockiert nicht den WLAN-Stack
- **Fallback-Logik**: Bei Shelly-Ausfall konfigurierbarer Fallback-Watt statt unkontrolliertem Verhalten
- **Telnet-Logging**: Echtzeit-Diagnose ohne seriellen Adapter
- **HA Auto-Discovery**: Automatische Integration in Home Assistant
- **Nachtmodus**: Leistungsbegrenzung per Uhrzeit
- **Config-Backup/Restore**: Konfiguration exportieren und importieren

## Hardware

| Funktion        | GPIO      |
|------------------|-----------|
| RS485 TX         | GPIO1 (TX0) |
| RS485 RX         | GPIO3 (RX0) |
| RS485 DE/RE      | GPIO14 (D5) |
| Flash-Button     | GPIO0 (Werksreset: 5s beim Boot gedrückt halten) |
| LED (onboard)    | GPIO2, active LOW |

RS485-Modul TX/RX werden an den Hardware-UART (Serial) angeschlossen, dieser wird
ausschließlich für RS485 verwendet — kein Debug-Output über Serial.

## Build & Flash

```
pio run -t upload
pio run -t uploadfs   # LittleFS-Image (nur nötig falls Dateien in data/ liegen)
```

## Ersteinrichtung

1. Erstes Boot ohne `config.json` (oder leere `wifi_ssid`) → Gerät startet einen
   Access Point `SOYO-Setup` (Passwort `1234567890`, IP `10.0.0.1`). Captive
   Portal öffnet sich auf den meisten Smartphones automatisch.
2. Im Webinterface unter "Konfiguration" WLAN-Zugangsdaten und Betriebsmodus setzen,
   "Save Controller" klicken → Gerät speichert und startet neu, verbindet sich mit dem WLAN.
3. Danach erreichbar über `http://soyo.local` oder die vom Router vergebene IP.

## Werksreset

GPIO0 (Flash-Taste) beim Einschalten 5 Sekunden gedrückt halten → LittleFS wird
formatiert, Gerät startet neu und geht wieder in den AP-Setup-Modus.

## Betriebsmodi

- **Static**: Ausgang konstant auf `static_watt` (kein externer Messwert nötig).
- **HttpInterface**: externe Quelle pusht Messwert per `GET /L1L2L3Auto?Value=<watt>`.
- **MqttSub**: Messwert kommt per MQTT-Subscribe auf `mqtt_sub_topic`.
- **Shelly Gen1 / Gen2 Pro**: Firmware pollt den Shelly per HTTP (alle 3s).
- **JSON HTTP Client**: generischer Poll gegen eine beliebige JSON-URL mit
  Punkt-separiertem Pfad (z.B. `StatusSNS.SML.DJ_TPWRCURR`).

Bei den pollenden Modi (Shelly/JSON) schaltet die Firmware nach 3 Fehlversuchen in
einen Fallback-Zustand (`fallback_watt`) und kehrt nach 3 erfolgreichen Antworten
wieder in den Normalbetrieb zurück (mit Ramping in 50W-Schritten).

## RS485 Status-Response

Das Sende-Frame (Sollwert setzen) und die Status-Anfrage sind exakt nach Vorgabe
implementiert (8 Byte, 4800 8N1, Checksumme `(264 - byte[4] - byte[5]) & 0xFF`).

Die **Antwort** des Wechselrichters ist ein 15-Byte-Frame:

```
[0]      0x23 Header
[1]      0x01
[2]      0x01
[3]      0x00 Reserved
[4]      Operation-Status (0=Normal, 1=Startup, 2=Standby, 3=Startup aborted, 4=Error/Battery)
[5..6]   Batteriespannung, uint16 BE, x0.1V
[7..8]   Batteriestrom,    uint16 BE, x0.1A
[9..10]  AC-Spannung,      uint16 BE, x1V
[11]     AC-Frequenz,      uint8,     x0.5Hz
[12..13] Temperatur,       uint16 BE, (raw-300) x0.1°C
[14]     CRC (Algorithmus nicht bekannt, wird nicht geprüft)
```

Vor dem Parsen validiert [rs485.cpp](src/rs485.cpp): Frame ist exakt 15 Byte,
Byte 0 ist `0x23`, und Batteriespannung/AC-Spannung/Temperatur liegen unter den
Sanity-Grenzen (150V / 300V / 200°C) — sonst wird der Frame verworfen und
geloggt.

**Neuere Geräte (purple mainboard, Firmware STC8-2022-218) antworten grundsätzlich
nicht auf den Status-Request.** Das ist kein Fehler: `g_soyoStatus.valid` bleibt
einfach `false` und die "Soyo-Status"-Karte im Webinterface bleibt ausgeblendet.

## Telnet-Log

`telnet soyo.local` (Port 23) zeigt WiFi-Statuswechsel, RS485-Frames (Hex),
HTTP-/MQTT-Fehler, Demand-Änderungen, Fallback-Start/Ende und Heap-Stand live an.
Die letzten 20 Zeilen sind zusätzlich über `GET /log` als JSON abrufbar.

## OTA

`http://soyo.local/update`, abgesichert mit dem in der Konfiguration gesetzten
OTA-Passwort (Feld "OTA-Passwort", Nutzername leer). Während des Uploads wird
Notaus gesetzt und RS485 pausiert.

## Lizenz

MIT License — siehe [LICENSE](LICENSE).

Protokoll-Informationen (RS485-Frame-Format) basieren auf Community-Reverse-Engineering,
dokumentiert unter:
- https://github.com/syssi/esphome-soyosource-gtn-virtual-meter
- https://secondlifestorage.com/index.php?threads/limiter-inverter-with-rs485-load-setting.7631/
