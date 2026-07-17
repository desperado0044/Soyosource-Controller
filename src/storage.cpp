#include "storage.h"

#include <LittleFS.h>
#include <ArduinoJson.h>

#include "telnet_log.h"

static const char *CONFIG_PATH = "/config.json";

void configSetDefaults(Config &c) {
    memset(&c, 0, sizeof(Config));

    c.wifi_static = false;
    strlcpy(c.wifi_mask, "255.255.255.0", sizeof(c.wifi_mask));
    c.wifi_11n = false;

    c.mqtt_enabled = false;
    c.mqtt_port = 1883;
    strlcpy(c.mqtt_sub_topic, "Soyosource/L1L2L3", sizeof(c.mqtt_sub_topic));
    strlcpy(c.mqtt_pub_topic, "Soyosource/status", sizeof(c.mqtt_pub_topic));

    c.mode = MODE_STATIC;
    c.static_watt = 0;

    c.shelly_l1 = true;
    c.shelly_l2 = true;
    c.shelly_l3 = true;

    c.max_power = 1200;
    c.soyo_count = 1;
    c.offset = 0;
    c.fallback_watt = 0;

    c.night_mode_enabled = false;
    c.night_start_h = 22;
    c.night_start_m = 0;
    c.night_end_h = 6;
    c.night_end_m = 0;
    c.night_max_power = 300;
}

static void configToDoc(const Config &c, JsonDocument &doc) {
    doc["wifi_ssid"] = c.wifi_ssid;
    doc["wifi_pass"] = c.wifi_pass;
    doc["wifi_static"] = c.wifi_static;
    doc["wifi_ip"] = c.wifi_ip;
    doc["wifi_gw"] = c.wifi_gw;
    doc["wifi_mask"] = c.wifi_mask;
    doc["wifi_11n"] = c.wifi_11n;

    doc["mqtt_enabled"] = c.mqtt_enabled;
    doc["mqtt_broker"] = c.mqtt_broker;
    doc["mqtt_port"] = c.mqtt_port;
    doc["mqtt_user"] = c.mqtt_user;
    doc["mqtt_pass"] = c.mqtt_pass;
    doc["mqtt_sub_topic"] = c.mqtt_sub_topic;
    doc["mqtt_pub_topic"] = c.mqtt_pub_topic;

    doc["mode"] = c.mode;
    doc["static_watt"] = c.static_watt;

    doc["shelly_ip"] = c.shelly_ip;
    doc["shelly_l1"] = c.shelly_l1;
    doc["shelly_l2"] = c.shelly_l2;
    doc["shelly_l3"] = c.shelly_l3;

    doc["json_url"] = c.json_url;
    doc["json_path"] = c.json_path;

    doc["max_power"] = c.max_power;
    doc["soyo_count"] = c.soyo_count;
    doc["offset"] = c.offset;
    doc["fallback_watt"] = c.fallback_watt;

    doc["night_mode_enabled"] = c.night_mode_enabled;
    doc["night_start_h"] = c.night_start_h;
    doc["night_start_m"] = c.night_start_m;
    doc["night_end_h"] = c.night_end_h;
    doc["night_end_m"] = c.night_end_m;
    doc["night_max_power"] = c.night_max_power;

    doc["ota_pass"] = c.ota_pass;
}

static void docToConfig(JsonDocument &doc, Config &c) {
    strlcpy(c.wifi_ssid, doc["wifi_ssid"] | "", sizeof(c.wifi_ssid));
    strlcpy(c.wifi_pass, doc["wifi_pass"] | "", sizeof(c.wifi_pass));
    c.wifi_static = doc["wifi_static"] | false;
    strlcpy(c.wifi_ip, doc["wifi_ip"] | "", sizeof(c.wifi_ip));
    strlcpy(c.wifi_gw, doc["wifi_gw"] | "", sizeof(c.wifi_gw));
    strlcpy(c.wifi_mask, doc["wifi_mask"] | "255.255.255.0", sizeof(c.wifi_mask));
    c.wifi_11n = doc["wifi_11n"] | false;

    c.mqtt_enabled = doc["mqtt_enabled"] | false;
    strlcpy(c.mqtt_broker, doc["mqtt_broker"] | "", sizeof(c.mqtt_broker));
    c.mqtt_port = doc["mqtt_port"] | 1883;
    strlcpy(c.mqtt_user, doc["mqtt_user"] | "", sizeof(c.mqtt_user));
    strlcpy(c.mqtt_pass, doc["mqtt_pass"] | "", sizeof(c.mqtt_pass));
    strlcpy(c.mqtt_sub_topic, doc["mqtt_sub_topic"] | "Soyosource/L1L2L3", sizeof(c.mqtt_sub_topic));
    strlcpy(c.mqtt_pub_topic, doc["mqtt_pub_topic"] | "Soyosource/status", sizeof(c.mqtt_pub_topic));

    c.mode = doc["mode"] | (uint8_t)MODE_STATIC;
    c.static_watt = doc["static_watt"] | 0;

    strlcpy(c.shelly_ip, doc["shelly_ip"] | "", sizeof(c.shelly_ip));
    c.shelly_l1 = doc["shelly_l1"] | true;
    c.shelly_l2 = doc["shelly_l2"] | true;
    c.shelly_l3 = doc["shelly_l3"] | true;

    strlcpy(c.json_url, doc["json_url"] | "", sizeof(c.json_url));
    strlcpy(c.json_path, doc["json_path"] | "", sizeof(c.json_path));

    c.max_power = doc["max_power"] | 1200;
    c.soyo_count = doc["soyo_count"] | 1;
    c.offset = doc["offset"] | 0;
    c.fallback_watt = doc["fallback_watt"] | 0;

    c.night_mode_enabled = doc["night_mode_enabled"] | false;
    c.night_start_h = doc["night_start_h"] | 22;
    c.night_start_m = doc["night_start_m"] | 0;
    c.night_end_h = doc["night_end_h"] | 6;
    c.night_end_m = doc["night_end_m"] | 0;
    c.night_max_power = doc["night_max_power"] | 300;

    strlcpy(c.ota_pass, doc["ota_pass"] | "", sizeof(c.ota_pass));

    if (c.soyo_count < 1) c.soyo_count = 1;
    if (c.soyo_count > 4) c.soyo_count = 4;
}

bool storageBegin() {
    if (!LittleFS.begin()) {
        LOG("LittleFS: Mount fehlgeschlagen, formatiere neu");
        LittleFS.format();
        return LittleFS.begin();
    }
    return true;
}

bool configFileExists() {
    return LittleFS.exists(CONFIG_PATH);
}

bool loadConfig(Config &c) {
    if (!configFileExists()) {
        LOG("Config: /config.json nicht vorhanden");
        return false;
    }

    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        LOG("Config: Öffnen fehlgeschlagen");
        return false;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        LOG(("Config: JSON-Fehler " + String(err.c_str())).c_str());
        return false;
    }

    docToConfig(doc, c);
    return true;
}

bool saveConfig(const Config &c) {
    StaticJsonDocument<1024> doc;
    configToDoc(c, doc);

    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) {
        LOG("Config: Schreiben fehlgeschlagen (open)");
        return false;
    }

    bool ok = serializeJson(doc, f) > 0;
    f.close();

    if (!ok) {
        LOG("Config: Serialisierung fehlgeschlagen");
    }
    return ok;
}

String configToJsonString(const Config &c) {
    StaticJsonDocument<1024> doc;
    configToDoc(c, doc);
    String out;
    serializeJson(doc, out);
    return out;
}

bool configFromJsonString(Config &c, const String &json) {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        LOG(("Config: Upload JSON-Fehler " + String(err.c_str())).c_str());
        return false;
    }
    docToConfig(doc, c);
    return true;
}

void factoryReset() {
    LOG("Werksreset: formatiere LittleFS");
    LittleFS.format();
    ESP.restart();
}
