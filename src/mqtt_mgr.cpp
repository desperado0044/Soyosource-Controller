#include "mqtt_mgr.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "rs485.h"
#include "shelly.h"
#include "telnet_log.h"

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static unsigned long lastReconnectAttemptMillis = 0;
static const unsigned long RECONNECT_INTERVAL_MS = 5000;

static unsigned long lastPublishMillis = 0;
static const unsigned long PUBLISH_INTERVAL_MS = 3000;

static void mqttCallback(char *topic, byte *payload, unsigned int length) {
    (void)topic;
    if (config.mode != MODE_MQTT_SUB) return;

    char buf[32];
    unsigned int n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    float val = atof(buf);
    applyMeasurement(val);
    LOG(("MQTT: Messwert empfangen " + String(val)).c_str());
}

static void publishDiscoverySensor(const char *objectId, const char *name, const char *unit,
                                    const char *valueTemplate, const char *deviceClass,
                                    bool isBinary) {
    StaticJsonDocument<1024> doc;
    doc["name"] = name;
    doc["uniq_id"] = String("soyo_") + objectId;
    doc["stat_t"] = config.mqtt_pub_topic;
    doc["val_tpl"] = valueTemplate;
    if (unit != nullptr) doc["unit_of_meas"] = unit;
    if (deviceClass != nullptr) doc["dev_cla"] = deviceClass;
    if (isBinary) {
        doc["pl_on"] = "ON";
        doc["pl_off"] = "OFF";
    }

    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = "soyo_controller";
    dev["name"] = "Soyosource Controller";
    dev["mf"] = "DIY";
    dev["mdl"] = "GTN1000/1200";

    String payload;
    serializeJson(doc, payload);

    String topic = String("homeassistant/") + (isBinary ? "binary_sensor" : "sensor") +
                    "/soyo/" + objectId + "/config";
    mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

static void publishDiscovery() {
    publishDiscoverySensor("demand", "Soyo Demand", "W", "{{ value_json.demand }}", nullptr, false);
    publishDiscoverySensor("netz", "Soyo Netz", "W", "{{ value_json.netz }}", nullptr, false);
    publishDiscoverySensor("rssi", "Soyo RSSI", "dBm", "{{ value_json.rssi }}", "signal_strength", false);
    publishDiscoverySensor("uptime", "Soyo Uptime", "s", "{{ value_json.uptime }}", nullptr, false);
    publishDiscoverySensor("heap", "Soyo Heap", "B", "{{ value_json.heap }}", nullptr, false);
    publishDiscoverySensor("notaus", "Soyo Notaus", nullptr,
                            "{{ 'ON' if value_json.notaus else 'OFF' }}", nullptr, true);
    publishDiscoverySensor("fallback", "Soyo Fallback", nullptr,
                            "{{ 'ON' if value_json.fallback else 'OFF' }}", nullptr, true);
    LOG("MQTT: Home Assistant Discovery gesendet");
}

static void attemptConnect() {
    String clientId = "soyo-" + String(ESP.getChipId(), HEX);
    bool ok;
    if (strlen(config.mqtt_user) > 0) {
        ok = mqttClient.connect(clientId.c_str(), config.mqtt_user, config.mqtt_pass);
    } else {
        ok = mqttClient.connect(clientId.c_str());
    }

    if (ok) {
        LOG("MQTT: verbunden");
        mqttClient.subscribe(config.mqtt_sub_topic);
        publishDiscovery();
    } else {
        LOG(("MQTT: Verbindung fehlgeschlagen, rc=" + String(mqttClient.state())).c_str());
    }
}

static void publishPeriodic() {
    unsigned long now = millis();
    if (now - lastPublishMillis < PUBLISH_INTERVAL_MS) return;
    lastPublishMillis = now;

    StaticJsonDocument<512> doc;
    doc["demand"] = g_demand;
    doc["netz"] = g_netzwert;
    doc["rssi"] = WiFi.RSSI();
    doc["mode"] = config.mode;
    doc["notaus"] = g_notaus;
    doc["fallback"] = g_fallbackActive;
    doc["uptime"] = millis() / 1000;
    doc["heap"] = ESP.getFreeHeap();

    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(config.mqtt_pub_topic, payload.c_str());
}

void mqttBegin() {
    if (!config.mqtt_enabled) return;
    mqttClient.setServer(config.mqtt_broker, config.mqtt_port);
    mqttClient.setCallback(mqttCallback);
}

void mqttLoop() {
    if (!config.mqtt_enabled) return;
    if (WiFi.status() != WL_CONNECTED) return;

    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttemptMillis >= RECONNECT_INTERVAL_MS) {
            lastReconnectAttemptMillis = now;
            attemptConnect();
        }
        return;
    }

    mqttClient.loop();
    publishPeriodic();
}
