#include "mqtt_mgr.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "rs485.h"
#include "shelly.h"
#include "telnet_log.h"

// PubSubClient braucht eine TCP-Verbindung (WiFiClient), über die es dann das
// MQTT-Protokoll spricht. Beide werden nur einmal angelegt und danach in
// diesem File wiederverwendet (deshalb "static", d.h. nur hier sichtbar).
static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static unsigned long lastReconnectAttemptMillis = 0;
static const unsigned long RECONNECT_INTERVAL_MS = 5000;

static unsigned long lastPublishMillis = 0;
static const unsigned long PUBLISH_INTERVAL_MS = 3000;

// Wird von der PubSubClient-Bibliothek automatisch aufgerufen, sobald auf dem
// abonnierten Topic (config.mqtt_sub_topic) eine neue Nachricht ankommt. Man
// registriert diese Funktion einmal per setCallback() (siehe mqttBegin unten)
// und ruft sie selbst nie direkt auf.
// payload ist keine fertige Zeichenkette, sondern nur ein Roh-Byte-Puffer ohne
// Nullterminierung -- deshalb wird er hier erst in ein eigenes, garantiert
// nullterminiertes char-Array kopiert, bevor atof() ihn als Zahl lesen kann.
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
    (void)topic; // Topic wird ignoriert, wir haben nur ein einziges Sub-Topic abonniert

    if (config.mode != MODE_MQTT_SUB) return;

    char buf[32];
    // Payload auf Puffergröße begrenzen, falls eine überlange Nachricht ankommt
    unsigned int n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0'; // String-Ende markieren, sonst würde atof() über den Puffer hinauslesen

    float val = atof(buf);
    applyMeasurement(val);
    LOG(("MQTT: Messwert empfangen " + String(val)).c_str());
}

// Meldet einen Sensor bei Home Assistant an ("MQTT Discovery"). HA durchsucht
// dafür beim Start das Topic homeassistant/.../config nach solchen Nachrichten
// und legt daraus automatisch eine Entität an -- man muss den Sensor also NICHT
// mehr manuell in HA einrichten.
//
// Die Feldnamen (stat_t, val_tpl, uniq_id, pl_on/pl_off, dev_cla) sind keine
// eigene Abkürzung, sondern exakt die Kurzform, die Home Assistant für diese
// Nachricht erwartet (siehe HA-Doku "MQTT Discovery"). Es gibt auch die langen
// Namen (state_topic, value_template, ...), die funktionieren genauso, die
// Kurzform spart nur Bytes im ohnehin knappen ESP8266-RAM.
static void publishDiscoverySensor(const char *objectId, const char *name, const char *unit,
                                    const char *valueTemplate, const char *deviceClass,
                                    bool isBinary) {
    JsonDocument doc;
    doc["name"] = name;
    doc["uniq_id"] = String("soyo_") + objectId;     // eindeutige ID der Entität in HA
    doc["stat_t"] = config.mqtt_pub_topic;            // state_topic: wo HA den Wert abliest
    doc["val_tpl"] = valueTemplate;                   // value_template: Jinja-Ausdruck, der den Wert aus dem JSON-Payload holt
    if (unit != nullptr) doc["unit_of_meas"] = unit;  // unit_of_measurement, z.B. "W"
    if (deviceClass != nullptr) doc["dev_cla"] = deviceClass; // device_class: sagt HA, wie der Wert angezeigt werden soll (z.B. Signalstärke)
    if (isBinary) {
        // binary_sensor kennt nur zwei Zustände, dafür legt man den "Ein"/"Aus"-Text fest
        doc["pl_on"] = "ON";
        doc["pl_off"] = "OFF";
    }

    // "dev": fasst alle Sensoren zu einem gemeinsamen Gerät in HA zusammen,
    // statt dass jeder Sensor als eigenständiges Gerät auftaucht.
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = "soyo_controller";
    dev["name"] = "Soyosource Controller";
    dev["mf"] = "DIY";
    dev["mdl"] = "GTN1000/1200";

    String payload;
    serializeJson(doc, payload);

    // HA erwartet Discovery-Nachrichten unter diesem festen Topic-Schema:
    // homeassistant/<sensor|binary_sensor>/<beliebige_geräte-id>/<objekt-id>/config
    String topic = String("homeassistant/") + (isBinary ? "binary_sensor" : "sensor") +
                    "/soyo/" + objectId + "/config";
    // "true" am Ende = retained: die Nachricht bleibt beim Broker gespeichert,
    // damit HA sie auch findet, wenn es erst nach diesem Aufruf startet.
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
    // Jedes Gerät braucht am MQTT-Broker eine eindeutige Client-ID, sonst wirft
    // ein zweites Gerät mit derselben ID das erste laufend aus der Verbindung.
    // getChipId() ist pro ESP8266 einzigartig, deshalb eignet es sich dafür gut.
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

    JsonDocument doc;
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
