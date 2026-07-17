# Soyosource GTN1000/1200 Nulleinspeisungsregler

ESP8266 (NodeMCU v2) Firmware für einen Soyosource GTN1000/1200 Nulleinspeisungsregler.
Inspiriert von [BavarianSuperGuy/Esp-Soyosource-Controller](https://github.com/KlausLi/Esp-Soyosource-Controller).

## Über dieses Projekt

Dieses Projekt ist eine eigenständige, vollständig neu geschriebene Firmware für den Soyosource GTN1000/1200, angeregt durch die Arbeit von
[BavarianSuperGuy (KlausLi)](https://github.com/KlausLi/Esp-Soyosource-Controller),
der mit seinem Esp-Soyosource-Controller die Idee eines ESP8266-basierten
Nulleinspeisungsreglers für dieses Wechselrichtermodell bekannt gemacht hat.

Eigenschaften dieser Firmware:

- **Non-blocking WiFi**: WLAN-Verbindungsaufbau und Reconnect laufen non-blocking
  über eine Zustandsmaschine, ohne den restlichen Betrieb zu blockieren.

- **Werksreset**: GPIO0 5 Sekunden beim Boot gedrückt halten (siehe Abschnitt
  "Werksreset" unten).

- **Vollständig quelloffen**: Jede Zeile Code einsehbar, anpassbar, verbesserbar.

- **Non-blocking HTTP**: Shelly-Polling blockiert nicht den WLAN-Stack.

- **Fallback-Logik**: Bei Shelly-Ausfall konfigurierbarer Fallback-Watt
  mit definiertem Verhalten und LED-Anzeige.

- **Telnet-Logging**: Echtzeit-Diagnose ohne seriellen Adapter (Port 23).

- **HA Auto-Discovery**: Automatische Integration in Home Assistant per MQTT.

- **Nachtmodus**: Leistungsbegrenzung per Uhrzeit (NTP-basiert).

- **Config-Backup/Restore**: Konfiguration als JSON exportieren und importieren.

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
- **Shelly Gen1 / Gen2 Pro**: Firmware pollt den Shelly per HTTP (alle 500ms).
- **JSON HTTP Client**: generischer Poll gegen eine beliebige JSON-URL mit
  Punkt-separiertem Pfad (z.B. `StatusSNS.SML.DJ_TPWRCURR`).

Bei den pollenden Modi (Shelly/JSON) schaltet die Firmware nach 3 Fehlversuchen in
einen Fallback-Zustand (`fallback_watt`) und kehrt nach 3 erfolgreichen Antworten
wieder in den Normalbetrieb zurück.

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

## Für Einsteiger: wiederkehrende Code-Muster

Der Code ist kommentiert, aber ein paar Muster tauchen in fast jeder Datei auf.
Wer sie einmal verstanden hat, kann den restlichen Kommentaren im Code leichter folgen.

- **`static` vor einer Funktion/Variable** bedeutet: nur in dieser einen `.cpp`-Datei
  sichtbar, sonst nirgendwo im Projekt. Das ist Absicht, kein Tippfehler — es
  verhindert, dass sich z.B. zwei `lastSendMillis` aus unterschiedlichen Dateien
  in die Quere kommen.
- **`extern`** in einer `.h`-Datei ist das Gegenteil: "diese Variable/Funktion gibt
  es, ihre eigentliche Definition steht in einer `.cpp`-Datei". So teilen sich z.B.
  `main.cpp` und `http_server.cpp` denselben `config`. Suche nach der Zeile ohne
  `extern` (meist ganz oben in der passenden `.cpp`), um die echte Definition zu finden.
- **Kein `delay()`, stattdessen `millis()`-Zeitstempel**: Ein `delay(1000)` würde den
  ESP8266 eine ganze Sekunde lang komplett blockieren — kein WLAN, kein Webserver,
  nichts. Stattdessen merkt sich der Code den Zeitpunkt der letzten Aktion
  (`lastXyzMillis = millis();`) und prüft bei jedem Schleifendurchlauf nur, ob
  genug Zeit vergangen ist (`if (millis() - lastXyzMillis >= INTERVAL) { ... }`).
  So kann `loop()` nebenbei weiter WLAN, Webserver usw. bedienen. Das ist das
  wichtigste Muster im ganzen Projekt und taucht in praktisch jeder Datei auf.
- **`struct Config` und `enum OperatingMode`**: Ein `struct` ist einfach ein Behälter
  für mehrere zusammengehörige Werte (hier: alle Einstellungen). Ein `enum` gibt
  Zahlen sprechende Namen — `MODE_STATIC` ist nur ein anderer Name für `0`, aber
  deutlich lesbarer als eine nackte `0` im Code.
- **Callback-Funktionen** (z.B. `mqttCallback`, `onOTAStart`): Diese Funktionen ruft
  man nicht selbst auf. Man "registriert" sie einmal bei einer Bibliothek
  (`setCallback(mqttCallback)`), und die Bibliothek ruft sie automatisch auf, sobald
  das jeweilige Ereignis eintritt (z.B. eine MQTT-Nachricht kommt an).
- **`StaticJsonDocument<1024> doc; doc["key"] = wert;`**: Das ist die ArduinoJson-
  Bibliothek. `doc` ist ein JSON-Objekt im Speicher, `<1024>` die maximale Größe in
  Byte. `serializeJson(doc, ziel)` wandelt es in echten JSON-Text um,
  `deserializeJson(doc, text)` liest JSON-Text wieder in `doc` ein.
- **Zwei Zahlen zu einer größeren zusammensetzen** (`(frame[5] << 8) | frame[6]`):
  RS485/MQTT/JSON übertragen oft 16-Bit-Zahlen als zwei einzelne Bytes. `<< 8`
  schiebt das erste Byte um 8 Bit nach links (macht daraus quasi die "Zehnerstelle"),
  `|` fügt das zweite Byte als "Einerstelle" dazu. Beispiel: Bytes `0x01, 0x2C`
  ergeben `0x012C` = 300.

## Lizenz

MIT License — siehe [LICENSE](LICENSE).

Protokoll-Informationen (RS485-Frame-Format) basieren auf Community-Reverse-Engineering,
dokumentiert unter:
- https://github.com/syssi/esphome-soyosource-gtn-virtual-meter
- https://secondlifestorage.com/index.php?threads/limiter-inverter-with-rs485-load-setting.7631/
