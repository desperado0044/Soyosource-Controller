#include "display.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>

#include "config.h"
#include "rs485.h"
#include "shelly.h"
#include "wifi_mgr.h"
#include "ntp.h"

// "_F_" = voller Framebuffer (1024 Bytes fuer 128x64) statt seitenweisem
// Zeichnen -- auf dem ESP8266 (80KB RAM) kein Problem und deutlich einfacher
// zu benutzen, da man nicht selbst um mehrere Sende-Durchlaeufe herumbauen
// muss. "_HW_I2C" nutzt die Hardware-I2C-Peripherie (schneller als eine
// Software-Nachbildung); U8G2_R0 = keine Bilddrehung; kein eigener Reset-Pin
// vorgesehen (U8X8_PIN_NONE), das Modul haengt am selben Reset wie der ESP.
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Solange das Setup-Portal (WLAN-AP-Modus) laeuft, gibt es noch keinen
// echten Betrieb zu zeigen -- dafuer ein eigener, dauerhafter Infoscreen
// (SCREEN_CONFIG_PORTAL). Erst wenn das Geraet ins konfigurierte WLAN
// wechselt, startet die eigentliche Splash-Sequenz: erst das Wordmark
// waehrend des Verbindungsaufbaus, dann -- sobald verbunden -- 5 Sekunden
// lang die IP-Adresse, danach fuer den Rest der Laufzeit der Betriebsscreen.
enum DisplayScreen : uint8_t {
    SCREEN_CONFIG_PORTAL,
    SCREEN_SPLASH_BOOT,
    SCREEN_SPLASH_IP,
    SCREEN_BETRIEB
};

static const unsigned long IP_SPLASH_DURATION_MS = 5000;

static DisplayScreen screen = SCREEN_SPLASH_BOOT;
static unsigned long screenEnteredAt = 0;
static unsigned long lastRedrawMillis = 0;

// Kleines 4-Balken-WLAN-Symbol: verbunden -> alle Balken gefuellt,
// nicht verbunden/reconnecting -> nur Umriss. Bewusst als einfache Rechtecke
// statt als Icon-Font gezeichnet, damit keine zusaetzliche Font-Tabelle
// geraten/nachgeschlagen werden muss.
static void drawWifiIcon(int x, int y, bool connected) {
    const int barW = 3, gap = 1;
    const int heights[4] = {3, 5, 7, 9};
    for (int i = 0; i < 4; i++) {
        int bx = x + i * (barW + gap);
        int by = y + (9 - heights[i]);
        if (connected) {
            u8g2.drawBox(bx, by, barW, heights[i]);
        } else {
            u8g2.drawFrame(bx, by, barW, heights[i]);
        }
    }
}

// Zeigt die Richtung der Netz-Leistung: Spitze nach oben = Bezug (Haus zieht
// noch Strom vom Netz), Spitze nach unten = Einspeisung.
static void drawTrendArrow(int x, int y, bool bezug) {
    if (bezug) {
        u8g2.drawTriangle(x, y + 8, x + 4, y, x + 8, y + 8);
    } else {
        u8g2.drawTriangle(x, y, x + 4, y + 8, x + 8, y);
    }
}

static void drawSplashBoot() {
    u8g2.setFont(u8g2_font_helvB18_tr);
    u8g2.drawStr(16, 26, "SOYO");
    u8g2.drawHLine(16, 30, 60);

    // Build-Uhrzeit (__TIME__) statt einer eigenen, separat gepflegten
    // Versionsnummer -- die musste vorher von Hand hochgezaehlt werden und
    // wurde dabei vergessen, sodass hier lange eine veraltete Version stand,
    // obwohl neue Firmware laengst lief. __TIME__ aktualisiert sich
    // automatisch bei jedem Build dieser Datei und aendert sich garantiert
    // bei jedem neuen Upload -- GIT_VERSION (siehe http_server.cpp) waere
    // hier zu lang fuers 128px-Display (Font 6px/Zeichen) und aendert sich
    // ausserdem erst bei einem echten Commit, nicht bei jedem Testbuild.
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(16, 42, "FW " __TIME__);

    // Ganz simple "laufende Punkte"-Animation: Anzahl Punkte haengt vom
    // aktuellen millis()-Wert ab, kein eigener Animations-Zaehler noetig.
    String connecting = "Verbinde WLAN";
    int dotCount = (millis() / 400) % 4;
    for (int i = 0; i < dotCount; i++) connecting += '.';
    u8g2.drawStr(16, 58, connecting.c_str());
}

// Dauerhafter Infoscreen, solange das Geraet als eigener Access Point auf
// die Ersteinrichtung wartet (SSID/IP, unter der die Konfigurationsseite
// erreichbar ist). Kein Zeit-basierter Wechsel -- displayLoop() haelt diesen
// Screen aktiv, bis wifiMgrIsApMode() false wird.
static void drawConfigPortal() {
    // helvB12 ist als Proportionalschrift fuer "CONFIG PORTAL" zu breit und
    // ragt ueber die 128px hinaus (Buchstaben werden abgeschnitten) --
    // 7x13B ist schmaler (fest 7px/Zeichen) und passt zuverlaessig.
    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.drawStr(8, 16, "CONFIG PORTAL");
    u8g2.drawHLine(8, 20, 112);

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(8, 34, "WLAN:");
    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.drawStr(8, 48, WIFI_AP_SSID);

    u8g2.setFont(u8g2_font_6x10_tf);
    String ip = WiFi.softAPIP().toString();
    u8g2.drawStr(8, 62, ip.c_str());
}

static void drawSplashIp() {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(16, 22, "IP-ADRESSE");

    u8g2.setFont(u8g2_font_7x13B_tr);
    String ip = WiFi.localIP().toString();
    u8g2.drawStr(16, 40, ip.c_str());

    // Ablaufbalken: startet voll (96px) und schrumpft linear auf 0, bis die
    // 5 Sekunden um sind -- reine Fortschrittsanzeige, keine echte Logik.
    unsigned long elapsed = millis() - screenEnteredAt;
    long barWidth = 96 - (long)(elapsed * 96 / IP_SPLASH_DURATION_MS);
    if (barWidth < 0) barWidth = 0;
    u8g2.drawFrame(16, 50, 96, 4);
    u8g2.drawBox(16, 50, (int)barWidth, 4);
}

static void drawBetrieb() {
    // Statuszeile: WLAN-Symbol links, aktiver Modus rechts als invertierte
    // Pille (weisser Kasten, schwarze Schrift -- klassischer OLED-Trick fuer
    // einen "hervorgehobenen" Text ohne eigene Grafik).
    bool wifiOk = (wifiMgrGetState() == WIFI_STATE_STA_CONNECTED);
    drawWifiIcon(0, 0, wifiOk);

    const char *modeLabel;
    if (g_notaus) modeLabel = "NOTAUS";
    else if (g_fallbackActive) modeLabel = "FALLBACK";
    else if (isNightMode()) modeLabel = "NIGHT";
    else modeLabel = "AUTO";

    u8g2.setFont(u8g2_font_6x10_tf);
    int modeW = u8g2.getStrWidth(modeLabel);
    u8g2.drawBox(128 - modeW - 6, 0, modeW + 6, 10);
    u8g2.setDrawColor(0);
    u8g2.drawStr(128 - modeW - 3, 8, modeLabel);
    u8g2.setDrawColor(1);

    u8g2.drawHLine(0, 13, 128);

    // Netz-Leistung: die zentrale Regelgroesse, deshalb gross und mit
    // Richtungspfeil. Betrag ohne Vorzeichen, die Richtung steckt im Pfeil.
    float netz = g_netzwert;
    bool bezug = netz >= 0;
    char netzBuf[16];
    snprintf(netzBuf, sizeof(netzBuf), "%d W", (int)fabsf(netz));

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 26, "NETZ");
    drawTrendArrow(0, 30, bezug);
    u8g2.setFont(u8g2_font_logisoso20_tr);
    u8g2.drawStr(14, 48, netzBuf);

    // Soyo-Gesamtleistung: g_demand ist der Sollwert PRO Geraet (siehe
    // rs485.h), bei mehreren parallelen Soyos also mit soyo_count
    // multiplizieren, um die tatsaechliche Gesamt-Ausgangsleistung zu zeigen.
    int32_t soyoTotal = g_demand * (int32_t)config.soyo_count;
    char soyoBuf[16];
    snprintf(soyoBuf, sizeof(soyoBuf), "%ld W", (long)soyoTotal);

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 62, "SOYO");
    u8g2.setFont(u8g2_font_7x13B_tr);
    u8g2.drawStr(40, 62, soyoBuf);
}

void displayBegin() {
    Wire.begin(DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    // Ohne diese Zeile laeuft I2C nur mit dem 100kHz-Standardtakt: eine
    // komplette Framebuffer-Uebertragung (1024 Byte) blockiert dann grob
    // 60-100ms pro Redraw. 400kHz ("Fast Mode") ist Standard-Spezifikation,
    // jedes SSD1306-Modul unterstuetzt das, und viertelt die Blockierzeit --
    // haelt loop() insgesamt reaktionsschneller fuer RS485/Shelly/MQTT.
    Wire.setClock(400000);
    u8g2.begin();
    screen = SCREEN_SPLASH_BOOT;
    screenEnteredAt = millis();
}

void displayLoop() {
    // Screen-Uebergaenge unabhaengig vom Redraw-Timing pruefen, damit ein
    // Wechsel (z.B. sobald WLAN verbunden ist) nicht durch das Redraw-
    // Intervall verzoegert wird.
    //
    // Solange der AP-Setup-Modus laeuft, wird SCREEN_CONFIG_PORTAL bei jedem
    // Durchlauf erneut erzwungen (kein Wechsel moeglich, bis die
    // Einrichtung abgeschlossen ist). Erst wenn wifiMgrIsApMode() false
    // wird -- das Geraet also ins konfigurierte WLAN wechselt -- startet
    // die normale Splash-Sequenz von vorn.
    if (wifiMgrIsApMode()) {
        screen = SCREEN_CONFIG_PORTAL;
    } else if (screen == SCREEN_CONFIG_PORTAL) {
        screen = SCREEN_SPLASH_BOOT;
        screenEnteredAt = millis();
    } else if (screen == SCREEN_SPLASH_BOOT) {
        if (wifiMgrGetState() == WIFI_STATE_STA_CONNECTED) {
            screen = SCREEN_SPLASH_IP;
            screenEnteredAt = millis();
        }
    } else if (screen == SCREEN_SPLASH_IP) {
        if (millis() - screenEnteredAt >= IP_SPLASH_DURATION_MS) {
            screen = SCREEN_BETRIEB;
        }
    }

    // Redraw an config.poll_interval_ms gekoppelt statt an eine eigene feste
    // Konstante: die angezeigten Werte aendern sich ohnehin nicht schneller
    // als der Messwert selbst nachkommt, und bleibt automatisch konsistent,
    // falls poll_interval_ms mal angepasst wird.
    unsigned long now = millis();
    if (now - lastRedrawMillis < config.poll_interval_ms) return;
    lastRedrawMillis = now;

    u8g2.clearBuffer();
    switch (screen) {
        case SCREEN_CONFIG_PORTAL: drawConfigPortal(); break;
        case SCREEN_SPLASH_BOOT:   drawSplashBoot();   break;
        case SCREEN_SPLASH_IP:     drawSplashIp();     break;
        case SCREEN_BETRIEB:       drawBetrieb();      break;
    }
    u8g2.sendBuffer();
}
