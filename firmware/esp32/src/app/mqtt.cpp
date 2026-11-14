/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/mqtt.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <math.h>
#include <string.h>

#include "app/control.h"
#include "app/discovery.h"
#include "app/network.h"
#include "app/settings.h"
#include "app/shared.h"
#include "vdm/crc16.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif

namespace app
{

namespace
{

constexpr uint32_t kPublishCheckMs = 2000;
constexpr uint32_t kRepublishMs = 5UL * 60UL * 1000UL; /* everything, even unchanged */
constexpr size_t kTopicMax = 160;
constexpr size_t kPayloadMax = 512;

constexpr uint32_t kReconnectMs = 10000;

struct InMsg {
    char topic[kTopicMax];
    char payload[kPayloadMax];
    uint16_t len;
};

/* to the MQTT task: a message to publish or a topic to subscribe */
struct OutMsg {
    bool subscribe;
    bool retain;
    char topic[kTopicMax];
    char payload[1280];
};

QueueHandle_t g_in = nullptr;
QueueHandle_t g_out = nullptr;
SemaphoreHandle_t g_lock = nullptr;
volatile bool g_connected = false;
volatile bool g_just_connected = false;
volatile bool g_restart = false;
bool g_started = false;

/* settings of the running connection, guarded by g_lock */
vdm::MqttConfig g_running;
char g_base[96] = "";
char g_client_id[vdm::kNameSize] = "";
char g_lwt_topic[128] = "";

bool g_resubscribe = false;
uint32_t g_last_check = 0;
uint32_t g_last_full = 0;
vdm::Model &g_model = *new vdm::Model();
OutMsg &g_msg = *new OutMsg(); /* built in the main loop */

/* change detection: CRC of the last payload per state topic */
enum : uint8_t { kSlotLink, kSlotState, kSlotZone = 2 };
constexpr uint16_t kSlotZoneClimate = kSlotZone + vdm::kMaxZones;
constexpr uint16_t kSlotValve = kSlotZoneClimate + vdm::kMaxZones;
constexpr uint16_t kSlotSource = kSlotValve + VDM_MAX_CHANNELS;
constexpr uint16_t kSlots = kSlotSource + vdm::kMaxSources;
uint16_t g_crc[kSlots];
bool g_sent[kSlots];

const char *const kModes[] = {"disabled", "unknown",     "not connected", "idle", "opening",
                              "closing",  "calibrating", "queued",        "fault"};
const char *const kFaults[] = {"",          "open circuit",  "blocked",
                               "timeout",   "overcurrent",   "calibration failed",
                               "no pulses", "count mismatch"};

void onMessage(char *topic, uint8_t *payload, unsigned int len)
{
    static InMsg m; /* MQTT task only */
    size_t tl = strlen(topic);
    if (tl >= kTopicMax || len >= kPayloadMax) {
        return;
    }
    memcpy(m.topic, topic, tl + 1);
    memcpy(m.payload, payload, len);
    m.payload[len] = '\0';
    m.len = static_cast<uint16_t>(len);
    xQueueSend(g_in, &m, 0);
}

/* The MQTT task: connects (blocking is fine here), receives and sends. */
void mqttTask(void *)
{
    static WiFiClient net;
    static PubSubClient mc(net);
    static OutMsg out;
    static vdm::MqttConfig cfg;
    static char client_id[vdm::kNameSize], lwt[128];
    mc.setBufferSize(1536);
    mc.setCallback(onMessage);
    mc.setSocketTimeout(5);
    mc.setKeepAlive(30);
    uint32_t last_try = 0;
    bool first = true;

    for (;;) {
        if (g_restart) {
            g_restart = false;
            if (mc.connected()) {
                mc.publish(lwt, "offline", true); /* a clean disconnect sends no will */
                mc.disconnect();
            }
            g_connected = false;
            first = true;
            xQueueReset(g_out);
        }
        xSemaphoreTake(g_lock, portMAX_DELAY);
        bool enabled = g_started;
        cfg = g_running;
        strlcpy(client_id, g_client_id, sizeof(client_id));
        strlcpy(lwt, g_lwt_topic, sizeof(lwt));
        xSemaphoreGive(g_lock);

        if (!enabled || !networkUp()) {
            g_connected = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!mc.connected()) {
            g_connected = false;
            if (first || millis() - last_try >= kReconnectMs) {
                first = false;
                last_try = millis();
                net.setTimeout(5);
                mc.setServer(cfg.host, cfg.port);
                bool ok =
                    mc.connect(client_id, cfg.user[0] ? cfg.user : nullptr,
                               cfg.password[0] ? cfg.password : nullptr, lwt, 1, true, "offline");
                if (ok) {
                    mc.publish(lwt, "online", true);
                    g_connected = true;
                    g_just_connected = true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        mc.loop();
        for (int k = 0; k < 12 && xQueueReceive(g_out, &out, 0) == pdTRUE; k++) {
            if (out.subscribe) {
                mc.subscribe(out.topic);
            } else if (!mc.publish(out.topic, out.payload, out.retain)) {
                espLog(VDM_LOG_WARN, "MQTT: publishing %s failed (%u bytes)", out.topic,
                       static_cast<unsigned>(strlen(out.payload)));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void stopClient()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_started = false;
    xSemaphoreGive(g_lock);
    g_restart = true;
}

void startClient()
{
    const vdm::MqttConfig &m = config().mqtt;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_running = m;
    mqttClientId(g_client_id, sizeof(g_client_id));
    mqttBaseTopic(g_base, sizeof(g_base));
    snprintf(g_lwt_topic, sizeof(g_lwt_topic), "%s/status", g_base);
    g_started = m.enabled && m.host[0] != '\0';
    xSemaphoreGive(g_lock);
    g_restart = true;
    if (g_started) {
        espLog(VDM_LOG_INFO, "MQTT: connecting to %s:%u as %s, base topic %s", m.host, m.port,
               g_client_id, g_base);
    }
}

bool enqueue(bool subscribe, const char *topic, const char *payload, bool retain,
             uint32_t wait_ms = 20)
{
    g_msg.subscribe = subscribe;
    g_msg.retain = retain;
    strlcpy(g_msg.topic, topic, sizeof(g_msg.topic));
    strlcpy(g_msg.payload, payload, sizeof(g_msg.payload));
    return xQueueSend(g_out, &g_msg, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

/* publishes if the payload changed (or `force`); retried later if the
 * queue to the MQTT task is full */
void publishSlot(uint16_t slot, const char *topic, const char *payload, bool force)
{
    uint16_t crc = vdm_crc16(reinterpret_cast<const uint8_t *>(payload), strlen(payload));
    if (!force && g_sent[slot] && g_crc[slot] == crc) {
        return;
    }
    if (!enqueue(false, topic, payload, true)) {
        return;
    }
    g_crc[slot] = crc;
    g_sent[slot] = true;
}

float r1(float v)
{
    return roundf(v * 10.0f) / 10.0f;
}

float r2(float v)
{
    return roundf(v * 100.0f) / 100.0f;
}

/* false if a subscription could not be queued (retried later) */
bool subscribeAll()
{
    const vdm::Config &c = config();
    char t[kTopicMax];
    bool ok = true;
    snprintf(t, sizeof(t), "%s/zone/+/set", g_base);
    ok = enqueue(true, t, "", false, 500) && ok;
    snprintf(t, sizeof(t), "%s/valve/+/set", g_base);
    ok = enqueue(true, t, "", false, 500) && ok;
    snprintf(t, sizeof(t), "%s/set", g_base);
    ok = enqueue(true, t, "", false, 500) && ok;
    if (c.mqtt.discovery) {
        snprintf(t, sizeof(t), "%s/status", c.mqtt.discovery_prefix);
        ok = enqueue(true, t, "", false, 500) && ok;
    }
    for (uint8_t i = 0; i < c.source_count; i++) {
        if (c.sources[i].type == vdm::SourceType::Mqtt) {
            ok = enqueue(true, engine().mqttTopic(i, t, sizeof(t)), "", false, 500) && ok;
        }
    }
    return ok;
}

void publishStates(bool force)
{
    const vdm::Config &c = config();
    const vdm::ControlEngine &e = engine();
    char err[8];
    readModel(g_model, err, sizeof(err));
    if (g_model.valves_valid) {
        uint16_t mask = 0;
        for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
            if (g_model.valves[ch].mode != VDM_MODE_DISABLED) {
                mask |= static_cast<uint16_t>(1u << ch);
            }
        }
        discoverySetEnabled(mask);
    }
    char topic[kTopicMax];
    char out[kPayloadMax];

    snprintf(topic, sizeof(topic), "%s/link", g_base);
    publishSlot(kSlotLink, topic, g_model.state == vdm::LinkState::Connected ? "online" : "offline",
                force);

    {
        StaticJsonDocument<384> d;
        bool failsafe = false;
        for (const auto &v : g_model.valves) {
            failsafe = failsafe || (v.flags & VDM_VFLAG_FAILSAFE);
        }
        d["failsafe"] = failsafe;
        static const char *const kHvac[] = {"auto", "heat", "cool"};
        d["hvac_mode"] = kHvac[static_cast<uint8_t>(c.hvac.mode)];
        d["flow_guard"] = e.flow().active;
        d["flow_insufficient"] = e.flow().insufficient;
        d["flow_total"] = r1(e.flow().total / 10.0f);
        d["flow_added"] = r1(e.flow().added / 10.0f);
        char v[24];
        snprintf(v, sizeof(v), "%d.%d.%d+%lu", FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH,
                 static_cast<unsigned long>(FW_BUILD));
        d["esp_version"] = v;
        if (g_model.hello_valid) {
            snprintf(v, sizeof(v), "%u.%u.%u+%lu", g_model.hello.fw_major, g_model.hello.fw_minor,
                     g_model.hello.fw_patch, static_cast<unsigned long>(g_model.hello.fw_build));
            d["stm_version"] = v;
        }
        serializeJson(d, out, sizeof(out));
        snprintf(topic, sizeof(topic), "%s/state", g_base);
        publishSlot(kSlotState, topic, out, force);
    }

    for (uint8_t i = 0; i < c.zone_count; i++) {
        const vdm::ZoneConfig &zc = c.zones[i];
        const vdm::ZoneStatus &z = e.zone(i);
        /* missing values are null, so templates in Home Assistant give "unknown" */
        StaticJsonDocument<512> d;
        d["name"] = zc.name;
        d["mode"] = vdm::zoneModeName(zc.mode);
        d["enabled"] = zc.enabled;
        d["setpoint"] = r1(zc.setpoint);
        d["cool_setpoint"] = r1(zc.cool_setpoint);
        /* the setpoint in use: cooling setpoint while cold is supplied */
        d["target"] = r1(z.medium == vdm::Medium::Cool ? zc.cool_setpoint : zc.setpoint);
        d["temperature"] = (char *)nullptr;
        if (z.has_temperature) {
            d["temperature"] = r2(z.temperature);
        }
        d["supply"] = (char *)nullptr;
        if (z.has_supply) {
            d["supply"] = r1(z.supply);
        }
        d["source"] = vdm::sourceStateName(z.source);
        d["stale"] = z.source == vdm::SourceState::Stale || z.source == vdm::SourceState::Missing;
        d["medium"] = vdm::mediumName(z.medium);
        d["output"] = (char *)nullptr;
        if (z.has_output) {
            d["output"] = r1(z.output);
        }
        d["action"] = vdm::zoneActionName(z.action);
        serializeJson(d, out, sizeof(out));
        snprintf(topic, sizeof(topic), "%s/zone/%s/state", g_base, zc.id);
        publishSlot(kSlotZone + i, topic, out, force);
        snprintf(topic, sizeof(topic), "%s/zone/%s/climate", g_base, zc.id);
        publishSlot(kSlotZoneClimate + i, topic,
                    zc.mode == vdm::ZoneMode::Control ? "online" : "offline", force);
    }

    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        const vdm_valve_state_t &s = g_model.valves[ch];
        const vdm::ValveStatus &v = e.valve(ch);
        StaticJsonDocument<448> d;
        d["name"] = c.valves[ch].name;
        d["zone"] = c.valves[ch].zone;
        d["position"] = (char *)nullptr;
        d["mode"] = "unknown";
        d["fault"] = "";
        if (g_model.valves_valid) {
            if (s.actual <= 1000) {
                d["position"] = r1(s.actual / 10.0f);
            }
            d["target"] = r1(s.target / 10.0f);
            d["mode"] = s.mode <= VDM_MODE_LAST ? kModes[s.mode] : "?";
            d["fault"] = s.fault <= VDM_FAULT_LAST ? kFaults[s.fault] : "?";
            d["current_ma"] = r1(s.current_ma10 / 10.0f);
        }
        if (v.has_target) {
            d["requested"] = r1(v.requested / 10.0f);
            d["effective"] = r1(v.effective / 10.0f);
        }
        d["controlled"] = v.controlled;
        serializeJson(d, out, sizeof(out));
        snprintf(topic, sizeof(topic), "%s/valve/%u/state", g_base, ch + 1u);
        publishSlot(kSlotValve + ch, topic, out, force);
    }

    for (uint8_t i = 0; i < c.source_count; i++) {
        const vdm::SourceStatus &s = e.source(i);
        StaticJsonDocument<192> d;
        d["name"] = c.sources[i].name;
        d["value"] = (char *)nullptr;
        if (s.has_value) {
            d["value"] = r2(s.value);
            d["age"] = s.age_s;
        }
        d["status"] = vdm::sourceStateName(s.state);
        serializeJson(d, out, sizeof(out));
        snprintf(topic, sizeof(topic), "%s/source/%s/state", g_base, c.sources[i].id);
        publishSlot(kSlotSource + i, topic, out, force);
    }
}

bool parseBool(JsonVariantConst v, bool &out)
{
    if (v.is<bool>()) {
        out = v.as<bool>();
        return true;
    }
    const char *s = v.as<const char *>();
    if (s == nullptr) {
        return false;
    }
    if (!strcasecmp(s, "on") || !strcasecmp(s, "heat") || !strcasecmp(s, "cool") ||
        !strcasecmp(s, "auto") || !strcmp(s, "1") || !strcasecmp(s, "true")) {
        out = true;
        return true;
    }
    if (!strcasecmp(s, "off") || !strcmp(s, "0") || !strcasecmp(s, "false")) {
        out = false;
        return true;
    }
    return false;
}

void zoneCommand(const char *zid, const char *payload, size_t len)
{
    int zi = config().zoneIndex(zid);
    if (zi < 0) {
        espLog(VDM_LOG_WARN, "MQTT: unknown zone '%s'", zid);
        return;
    }
    StaticJsonDocument<256> d;
    if (deserializeJson(d, payload, len) || !d.is<JsonObject>()) {
        espLog(VDM_LOG_WARN, "MQTT: zone %s: JSON object expected", zid);
        return;
    }
    vdm::ZoneConfig z = config().zones[zi];
    bool ok = true;
    if (!d["setpoint"].isNull()) {
        float v = d["setpoint"].as<float>();
        ok = ok && v >= 5.0f && v <= 30.0f;
        z.setpoint = roundf(v * 10.0f) / 10.0f;
    }
    if (!d["target"].isNull()) {
        /* the setpoint in use (Home Assistant climate) */
        float v = d["target"].as<float>();
        bool cool = engine().zone(static_cast<uint8_t>(zi)).medium == vdm::Medium::Cool;
        ok = ok && v >= (cool ? 15.0f : 5.0f) && v <= (cool ? 35.0f : 30.0f);
        (cool ? z.cool_setpoint : z.setpoint) = roundf(v * 10.0f) / 10.0f;
    }
    if (!d["cool_setpoint"].isNull()) {
        float v = d["cool_setpoint"].as<float>();
        ok = ok && v >= 15.0f && v <= 35.0f;
        z.cool_setpoint = roundf(v * 10.0f) / 10.0f;
    }
    for (const char *k : {"enabled", "heating"}) {
        if (!d[k].isNull()) {
            ok = ok && parseBool(d[k], z.enabled);
        }
    }
    if (!d["mode"].isNull()) {
        const char *m = d["mode"].as<const char *>();
        if (m != nullptr && !strcmp(m, "control")) {
            z.mode = vdm::ZoneMode::Control;
        } else if (m != nullptr && !strcmp(m, "manual")) {
            z.mode = vdm::ZoneMode::Manual;
        } else {
            ok = false;
        }
    }
    if (!ok) {
        espLog(VDM_LOG_WARN, "MQTT: zone %s: invalid command %.*s", zid, static_cast<int>(len),
               payload);
        return;
    }
    settingsEdit().zones[zi] = z;
    settingsChanged();
    controlKick();
    g_last_check = 0; /* publish the new state soon */
}

void valveCommand(const char *num, const char *payload, size_t len)
{
    long n = strtol(num, nullptr, 10);
    if (n < 1 || n > VDM_MAX_CHANNELS) {
        return;
    }
    uint8_t ch = static_cast<uint8_t>(n - 1);
    StaticJsonDocument<128> d;
    float pos = NAN;
    if (!deserializeJson(d, payload, len) && d.is<JsonObject>()) {
        if (d["calibrate"].as<bool>()) {
            Command c{};
            c.type = CommandType::Calibrate;
            c.value = static_cast<uint16_t>(1u << ch);
            sendCommand(c);
            return;
        }
        if (!d["position"].isNull()) {
            pos = d["position"].as<float>();
        }
    } else if (d.is<float>() || d.is<long>()) {
        pos = d.as<float>(); /* plain number */
    }
    if (!(pos >= 0.0f && pos <= 100.0f)) {
        espLog(VDM_LOG_WARN, "MQTT: valve %ld: invalid command", n);
        return;
    }
    if (controlSetTarget(ch, static_cast<uint16_t>(lroundf(pos * 10.0f))) ==
        vdm::ControlEngine::Result::Controlled) {
        espLog(VDM_LOG_WARN, "MQTT: valve %ld belongs to a zone in mode control", n);
    }
    g_last_check = 0;
}

/* <base>/set: {"hvac_mode":"cool"} */
void systemCommand(const char *payload, size_t len)
{
    StaticJsonDocument<128> d;
    if (deserializeJson(d, payload, len) || !d.is<JsonObject>()) {
        return;
    }
    const char *m = d["hvac_mode"];
    if (m != nullptr) {
        static const char *const kHvac[] = {"auto", "heat", "cool"};
        for (uint8_t i = 0; i < 3; i++) {
            if (strcmp(m, kHvac[i]) == 0) {
                settingsEdit().hvac.mode = static_cast<vdm::HvacMode>(i);
                settingsChanged();
                controlKick();
                g_last_check = 0;
                return;
            }
        }
        espLog(VDM_LOG_WARN, "MQTT: unknown hvac_mode '%s'", m);
    }
}

void handle(const InMsg &m)
{
    size_t bl = strlen(g_base);
    char ha_status[48];
    snprintf(ha_status, sizeof(ha_status), "%s/status", config().mqtt.discovery_prefix);
    if (strcmp(m.topic, ha_status) == 0) {
        if (strcmp(m.payload, "online") == 0) {
            discoveryStart(); /* Home Assistant restarted */
        }
        return;
    }
    if (strncmp(m.topic, g_base, bl) == 0 && strcmp(m.topic + bl, "/set") == 0) {
        systemCommand(m.payload, m.len);
        return;
    }
    if (strncmp(m.topic, g_base, bl) == 0 && m.topic[bl] == '/') {
        const char *rest = m.topic + bl + 1;
        char id[16];
        if (sscanf(rest, "zone/%15[^/]/set", id) == 1 && strstr(rest, "/set") != nullptr) {
            zoneCommand(id, m.payload, m.len);
            return;
        }
        if (sscanf(rest, "valve/%15[^/]/set", id) == 1 && strstr(rest, "/set") != nullptr) {
            valveCommand(id, m.payload, m.len);
            return;
        }
    }
    controlMqtt(m.topic, m.payload, m.len);
}

} // namespace

void mqttBegin()
{
    g_in = xQueueCreate(8, sizeof(InMsg));
    g_out = xQueueCreate(8, sizeof(OutMsg));
    g_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(mqttTask, "mqtt", 6144, nullptr, 2, nullptr, 0);
}

void mqttReconfigure()
{
    const vdm::MqttConfig &m = config().mqtt;
    char base[96];
    mqttBaseTopic(base, sizeof(base));
    bool changed = m.enabled != g_running.enabled || strcmp(m.host, g_running.host) != 0 ||
                   m.port != g_running.port || strcmp(m.user, g_running.user) != 0 ||
                   strcmp(m.password, g_running.password) != 0 ||
                   strcmp(m.client_id, g_running.client_id) != 0 || strcmp(base, g_base) != 0;
    if (changed) {
        stopClient();
        if (m.enabled) {
            espLog(VDM_LOG_INFO, "MQTT: settings changed, reconnecting");
        }
    }
    g_resubscribe = true; /* sources may have changed */
    discoveryStart();     /* names, zones, valves may have changed */
    for (bool &s : g_sent) {
        s = false; /* names or zones may have changed */
    }
}

void mqttLoop()
{
    if (g_in == nullptr) {
        return;
    }
    if (!g_started && config().mqtt.enabled && config().mqtt.host[0] != '\0' && networkUp()) {
        startClient();
    }

    static InMsg m;
    for (int k = 0; k < 4 && xQueueReceive(g_in, &m, 0) == pdTRUE; k++) {
        handle(m);
    }

    if (!g_connected) {
        return;
    }
    uint32_t now = millis();
    if (g_just_connected) {
        g_just_connected = false;
        g_resubscribe = !subscribeAll();
        discoveryStart();
        for (bool &s : g_sent) {
            s = false;
        }
        espLog(VDM_LOG_INFO, "MQTT: connected");
    }
    if (g_resubscribe) {
        g_resubscribe = !subscribeAll();
    }
    discoveryStep();
    if (now - g_last_check >= kPublishCheckMs || g_last_check == 0) {
        g_last_check = now;
        bool full = now - g_last_full >= kRepublishMs;
        if (full) {
            g_last_full = now;
        }
        publishStates(full);
    }
}

bool mqttPublish(const char *topic, const char *payload, bool retain)
{
    return g_connected && enqueue(false, topic, payload, retain, 0);
}

const char *mqttBase()
{
    return g_base;
}

const char *mqttClient()
{
    return g_client_id;
}

const char *mqttState()
{
    if (!config().mqtt.enabled) {
        return "disabled";
    }
    return g_connected ? "connected" : "connecting";
}

} // namespace app
