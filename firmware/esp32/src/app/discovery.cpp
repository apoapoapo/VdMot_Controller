/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/discovery.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "app/mqtt.h"
#include "app/network.h"
#include "app/settings.h"
#include "app/shared.h"

#ifndef FW_BUILD
#define FW_BUILD 0
#endif

namespace app
{

namespace
{

enum class Stage : uint8_t { Removed, Controller, Sources, Zones, Valves, Done };

constexpr uint8_t kControllerItems = 5;
constexpr uint8_t kSourceItems = 1;
constexpr uint8_t kZoneItems = 9;
constexpr uint8_t kValveItems = 4;
constexpr uint8_t kMaxPerPass = 6;

Stage g_stage = Stage::Done;
uint8_t g_idx = 0;
uint8_t g_sub = 0;

/* ids announced so far, to delete removed zones and sources */
char g_ann_zones[vdm::kMaxZones][vdm::kIdSize];
char g_ann_sources[vdm::kMaxSources][vdm::kIdSize];
char g_rm_zones[vdm::kMaxZones][vdm::kIdSize];
char g_rm_sources[vdm::kMaxSources][vdm::kIdSize];
uint8_t g_rm_zone_count = 0, g_rm_source_count = 0;

DynamicJsonDocument *g_doc = nullptr;
char g_topic[160];
char g_payload[1280];

/* one discovery message: component and object id; an empty payload
 * deletes the entity */
struct Msg {
    const char *component;
    char object[32];
};

void topicFor(const Msg &m)
{
    snprintf(g_topic, sizeof(g_topic), "%s/%s/%s/%s/config", config().mqtt.discovery_prefix,
             m.component, mqttClient(), m.object);
}

JsonObject base(const Msg &m, const char *name)
{
    g_doc->clear();
    JsonObject o = g_doc->to<JsonObject>();
    o["~"] = mqttBase();
    if (name != nullptr) {
        o["name"] = name;
    } else {
        o["name"] = (char *)nullptr; /* the device name */
    }
    char uid[64];
    snprintf(uid, sizeof(uid), "%s_%s", mqttClient(), m.object);
    o["uniq_id"] = uid;
    return o;
}

void controllerDevice(JsonObject o)
{
    JsonObject d = o.createNestedObject("dev");
    d.createNestedArray("ids").add(mqttClient());
    d["name"] = config().device.name;
    d["mf"] = "VdMot";
    d["mdl"] = "VdMot Controller v2";
    char v[24];
    snprintf(v, sizeof(v), "%d.%d.%d+%lu", FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH,
             static_cast<unsigned long>(FW_BUILD));
    d["sw"] = v;
    char url[40];
    snprintf(url, sizeof(url), "http://%s/", netStatus().ip.c_str());
    d["cu"] = url;
}

void zoneDevice(JsonObject o, const vdm::ZoneConfig &z)
{
    JsonObject d = o.createNestedObject("dev");
    char id[48];
    snprintf(id, sizeof(id), "%s_%s", mqttClient(), z.id);
    d.createNestedArray("ids").add(id);
    d["name"] = z.name;
    d["mf"] = "VdMot";
    d["mdl"] = "Zone";
    d["via_device"] = mqttClient();
}

/* ESP32 and STM32 must be online; optionally a third topic */
void availability(JsonObject o, const char *extra = nullptr, bool link = true)
{
    JsonArray a = o.createNestedArray("avty");
    a.createNestedObject()["t"] = "~/status";
    if (link) {
        a.createNestedObject()["t"] = "~/link";
    }
    if (extra != nullptr) {
        a.createNestedObject()["t"] = extra;
    }
    o["avty_mode"] = "all";
}

void temperature(JsonObject o)
{
    o["dev_cla"] = "temperature";
    o["unit_of_meas"] = "\xC2\xB0"
                        "C";
    o["stat_cla"] = "measurement";
}

/* --- builders: return false if `sub` does not exist ------------------- */

bool buildController(uint8_t sub, Msg &m)
{
    JsonObject o;
    switch (sub) {
    case 0:
        m.component = "binary_sensor";
        strcpy(m.object, "link");
        o = base(m, "STM32 link");
        o["stat_t"] = "~/link";
        o["pl_on"] = "online";
        o["pl_off"] = "offline";
        o["dev_cla"] = "connectivity";
        o["ent_cat"] = "diagnostic";
        availability(o, nullptr, false);
        break;
    case 1:
        m.component = "binary_sensor";
        strcpy(m.object, "failsafe");
        o = base(m, "Failsafe");
        o["stat_t"] = "~/state";
        o["val_tpl"] = "{{ 'ON' if value_json.failsafe else 'OFF' }}";
        o["dev_cla"] = "problem";
        availability(o);
        break;
    case 2:
        m.component = "binary_sensor";
        strcpy(m.object, "flow_guard");
        o = base(m, "Flow guard active");
        o["stat_t"] = "~/state";
        o["val_tpl"] = "{{ 'ON' if value_json.flow_guard else 'OFF' }}";
        o["ic"] = "mdi:pipe-valve";
        availability(o);
        break;
    case 3:
        m.component = "select";
        strcpy(m.object, "hvac_mode");
        o = base(m, "Heating / cooling");
        o["stat_t"] = "~/state";
        o["val_tpl"] = "{{ value_json.hvac_mode }}";
        o["cmd_t"] = "~/set";
        o["cmd_tpl"] = "{\"hvac_mode\":\"{{ value }}\"}";
        {
            JsonArray ops = o.createNestedArray("ops");
            ops.add("auto");
            ops.add("heat");
            ops.add("cool");
        }
        o["ic"] = "mdi:sun-snowflake-variant";
        availability(o, nullptr, false);
        break;
    case 4:
        m.component = "sensor";
        strcpy(m.object, "flow_total");
        o = base(m, "Total valve opening");
        o["stat_t"] = "~/state";
        o["val_tpl"] = "{{ value_json.flow_total }}";
        o["unit_of_meas"] = "%";
        o["stat_cla"] = "measurement";
        o["ent_cat"] = "diagnostic";
        availability(o);
        break;
    default:
        return false;
    }
    controllerDevice(o);
    return true;
}

bool buildSource(uint8_t idx, Msg &m)
{
    const vdm::SourceConfig &s = config().sources[idx];
    m.component = "sensor";
    snprintf(m.object, sizeof(m.object), "%s", s.id);
    JsonObject o = base(m, s.name);
    char t[48];
    snprintf(t, sizeof(t), "~/source/%s/state", s.id);
    o["stat_t"] = t;
    o["val_tpl"] = "{{ value_json.value }}";
    temperature(o);
    availability(o);
    controllerDevice(o);
    return true;
}

bool buildZone(uint8_t idx, uint8_t sub, Msg &m)
{
    const vdm::ZoneConfig &z = config().zones[idx];
    char st[48], set[48], clim[48];
    snprintf(st, sizeof(st), "~/zone/%s/state", z.id);
    snprintf(set, sizeof(set), "~/zone/%s/set", z.id);
    snprintf(clim, sizeof(clim), "~/zone/%s/climate", z.id);
    JsonObject o;
    switch (sub) {
    case 0:
        m.component = "climate";
        snprintf(m.object, sizeof(m.object), "%s_climate", z.id);
        o = base(m, nullptr);
        o["curr_temp_t"] = st;
        o["curr_temp_tpl"] = "{{ value_json.temperature }}";
        o["temp_stat_t"] = st;
        o["temp_stat_tpl"] = "{{ value_json.target }}";
        o["temp_cmd_t"] = set;
        o["temp_cmd_tpl"] = "{\"target\":{{ value }}}";
        o["mode_stat_t"] = st;
        o["mode_stat_tpl"] = "{{ 'auto' if value_json.enabled else 'off' }}";
        o["mode_cmd_t"] = set;
        o["mode_cmd_tpl"] = "{\"enabled\":\"{{ value }}\"}";
        {
            JsonArray modes = o.createNestedArray("modes");
            modes.add("auto");
            modes.add("off");
        }
        o["act_t"] = st;
        o["act_tpl"] = "{{ {'heating':'heating','cooling':'cooling','off':'off'}"
                       ".get(value_json.action, 'idle') }}";
        o["min_temp"] = 5;
        o["max_temp"] = 35;
        o["temp_step"] = 0.5;
        o["precision"] = 0.1;
        o["temp_unit"] = "C";
        availability(o, clim);
        break;
    case 1:
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "%s_temperature", z.id);
        o = base(m, "Temperature");
        o["stat_t"] = st;
        o["val_tpl"] = "{{ value_json.temperature }}";
        temperature(o);
        availability(o);
        break;
    case 2:
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "%s_output", z.id);
        o = base(m, "Valve output");
        o["stat_t"] = st;
        o["val_tpl"] = "{{ value_json.output }}";
        o["unit_of_meas"] = "%";
        o["stat_cla"] = "measurement";
        o["ic"] = "mdi:valve";
        availability(o);
        break;
    case 3:
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "%s_supply", z.id);
        o = base(m, "Supply temperature");
        o["stat_t"] = st;
        o["val_tpl"] = "{{ value_json.supply }}";
        temperature(o);
        o["ent_cat"] = "diagnostic";
        availability(o);
        break;
    case 4:
    case 5: {
        bool heat = sub == 4;
        m.component = "number";
        snprintf(m.object, sizeof(m.object), "%s_%s", z.id, heat ? "setpoint" : "cool_setpoint");
        o = base(m, heat ? "Heating setpoint" : "Cooling setpoint");
        o["stat_t"] = st;
        o["val_tpl"] = heat ? "{{ value_json.setpoint }}" : "{{ value_json.cool_setpoint }}";
        o["cmd_t"] = set;
        o["cmd_tpl"] = heat ? "{\"setpoint\":{{ value }}}" : "{\"cool_setpoint\":{{ value }}}";
        o["min"] = heat ? 5 : 15;
        o["max"] = heat ? 30 : 35;
        o["step"] = 0.5;
        o["mode"] = "box";
        o["dev_cla"] = "temperature";
        o["unit_of_meas"] = "\xC2\xB0"
                            "C";
        o["ent_cat"] = "config";
        availability(o);
        break;
    }
    case 6:
        m.component = "select";
        snprintf(m.object, sizeof(m.object), "%s_mode", z.id);
        o = base(m, "Mode");
        o["stat_t"] = st;
        o["val_tpl"] = "{{ value_json.mode }}";
        o["cmd_t"] = set;
        o["cmd_tpl"] = "{\"mode\":\"{{ value }}\"}";
        {
            JsonArray ops = o.createNestedArray("ops");
            ops.add("control");
            ops.add("manual");
        }
        o["ic"] = "mdi:tune-variant";
        availability(o);
        break;
    case 7:
        m.component = "binary_sensor";
        snprintf(m.object, sizeof(m.object), "%s_problem", z.id);
        o = base(m, "Temperature missing");
        o["stat_t"] = st;
        o["val_tpl"] = "{{ 'ON' if value_json.stale else 'OFF' }}";
        o["dev_cla"] = "problem";
        availability(o);
        break;
    case 8:
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "%s_state", z.id);
        o = base(m, "State");
        o["stat_t"] = st;
        o["val_tpl"] = "{{ value_json.action }}";
        o["ic"] = "mdi:information-outline";
        o["ent_cat"] = "diagnostic";
        availability(o);
        break;
    default:
        return false;
    }
    zoneDevice(o, z);
    return true;
}

bool buildValve(uint8_t ch, uint8_t sub, Msg &m)
{
    const vdm::Config &c = config();
    const vdm::ValveConfig &v = c.valves[ch];
    char st[32], set[32];
    snprintf(st, sizeof(st), "~/valve/%u/state", ch + 1u);
    snprintf(set, sizeof(set), "~/valve/%u/set", ch + 1u);
    JsonObject o;
    char name[64];
    switch (sub) {
    case 0:
        m.component = "valve";
        snprintf(m.object, sizeof(m.object), "v%u_valve", ch + 1u);
        o = base(m, v.name);
        o["command_topic"] = set;
        o["payload_open"] = "100";
        o["payload_close"] = "0";
        o["payload_stop"] = (char *)nullptr;
        o["set_position_topic"] = set;
        o["position_topic"] = st;
        o["value_template"] = "{{ value_json.position }}";
        o["reports_position"] = true;
        o["ic"] = "mdi:valve";
        break;
    case 1:
        m.component = "binary_sensor";
        snprintf(m.object, sizeof(m.object), "v%u_fault", ch + 1u);
        snprintf(name, sizeof(name), "%s fault", v.name);
        o = base(m, name);
        o["stat_t"] = st;
        o["val_tpl"] = "{{ 'ON' if value_json.fault or value_json.mode == 'fault' else 'OFF' }}";
        o["json_attr_t"] = st;
        o["dev_cla"] = "problem";
        break;
    case 2:
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "v%u_current", ch + 1u);
        snprintf(name, sizeof(name), "%s motor current", v.name);
        o = base(m, name);
        o["stat_t"] = st;
        o["val_tpl"] = "{{ value_json.current_ma }}";
        o["unit_of_meas"] = "mA";
        o["dev_cla"] = "current";
        o["stat_cla"] = "measurement";
        o["ent_cat"] = "diagnostic";
        o["en"] = false;
        break;
    case 3:
        m.component = "button";
        snprintf(m.object, sizeof(m.object), "v%u_calibrate", ch + 1u);
        snprintf(name, sizeof(name), "%s calibrate", v.name);
        o = base(m, name);
        o["cmd_t"] = set;
        o["pl_prs"] = "{\"calibrate\":true}";
        o["ent_cat"] = "config";
        o["en"] = false;
        break;
    default:
        return false;
    }
    availability(o);
    int zi = c.zoneIndex(v.zone);
    if (zi >= 0) {
        zoneDevice(o, c.zones[zi]);
    } else {
        controllerDevice(o);
    }
    return true;
}

/* objects of a removed zone / source, for the empty configuration */
bool removedObject(uint8_t idx, uint8_t sub, Msg &m)
{
    if (idx < g_rm_source_count) {
        if (sub > 0) {
            return false;
        }
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "%s", g_rm_sources[idx]);
        return true;
    }
    idx = static_cast<uint8_t>(idx - g_rm_source_count);
    if (idx >= g_rm_zone_count) {
        return false;
    }
    static const char *const kComp[] = {"climate", "sensor", "sensor",        "sensor", "number",
                                        "number",  "select", "binary_sensor", "sensor"};
    static const char *const kObj[] = {"climate", "temperature", "output",
                                       "supply",  "setpoint",    "cool_setpoint",
                                       "mode",    "problem",     "state"};
    if (sub >= kZoneItems) {
        return false;
    }
    m.component = kComp[sub];
    snprintf(m.object, sizeof(m.object), "%s_%s", g_rm_zones[idx], kObj[sub]);
    return true;
}

void collectRemoved()
{
    const vdm::Config &c = config();
    g_rm_zone_count = 0;
    for (auto &id : g_ann_zones) {
        if (id[0] != '\0' && c.zoneIndex(id) < 0) {
            strcpy(g_rm_zones[g_rm_zone_count++], id);
        }
        id[0] = '\0';
    }
    for (uint8_t i = 0; i < c.zone_count; i++) {
        strcpy(g_ann_zones[i], c.zones[i].id);
    }
    g_rm_source_count = 0;
    for (auto &id : g_ann_sources) {
        if (id[0] != '\0' && c.sourceIndex(id) < 0) {
            strcpy(g_rm_sources[g_rm_source_count++], id);
        }
        id[0] = '\0';
    }
    for (uint8_t i = 0; i < c.source_count; i++) {
        strcpy(g_ann_sources[i], c.sources[i].id);
    }
}

void next()
{
    g_sub++;
}

void nextIndex()
{
    g_idx++;
    g_sub = 0;
}

void nextStage()
{
    g_stage = static_cast<Stage>(static_cast<uint8_t>(g_stage) + 1);
    g_idx = 0;
    g_sub = 0;
}

/* builds the message at the cursor; false: cursor moved, try again */
bool build(Msg &m, bool &remove)
{
    const vdm::Config &c = config();
    remove = false;
    switch (g_stage) {
    case Stage::Removed:
        if (g_idx >= g_rm_source_count + g_rm_zone_count) {
            nextStage();
            return false;
        }
        if (!removedObject(g_idx, g_sub, m)) {
            nextIndex();
            return false;
        }
        remove = true;
        return true;
    case Stage::Controller:
        if (g_sub >= kControllerItems) {
            nextStage();
            return false;
        }
        return buildController(g_sub, m);
    case Stage::Sources:
        if (g_idx >= c.source_count) {
            nextStage();
            return false;
        }
        if (g_sub >= kSourceItems) {
            nextIndex();
            return false;
        }
        return buildSource(g_idx, m);
    case Stage::Zones:
        if (g_idx >= c.zone_count) {
            nextStage();
            return false;
        }
        if (g_sub >= kZoneItems) {
            nextIndex();
            return false;
        }
        return buildZone(g_idx, g_sub, m);
    case Stage::Valves:
        if (g_idx >= VDM_MAX_CHANNELS) {
            nextStage();
            return false;
        }
        if (g_sub >= kValveItems) {
            nextIndex();
            return false;
        }
        return buildValve(g_idx, g_sub, m);
    case Stage::Done:
        break;
    }
    return false;
}

} // namespace

void discoveryStart()
{
    if (!config().mqtt.discovery) {
        g_stage = Stage::Done;
        return;
    }
    collectRemoved();
    g_stage = Stage::Removed;
    g_idx = 0;
    g_sub = 0;
}

void discoveryStep()
{
    if (g_stage == Stage::Done) {
        return;
    }
    if (g_doc == nullptr) {
        g_doc = new DynamicJsonDocument(2048);
    }
    uint8_t sent = 0;
    for (int guard = 0; guard < 64 && sent < kMaxPerPass && g_stage != Stage::Done; guard++) {
        Msg m{};
        bool remove;
        if (!build(m, remove)) {
            continue;
        }
        topicFor(m);
        if (remove) {
            g_payload[0] = '\0';
        } else if (g_doc->overflowed() || measureJson(*g_doc) >= sizeof(g_payload)) {
            espLog(VDM_LOG_ERROR, "HA discovery: %s too large", m.object);
            next();
            continue;
        }
        if (!remove) {
            serializeJson(*g_doc, g_payload, sizeof(g_payload));
        }
        if (!mqttPublish(g_topic, g_payload, true)) {
            return; /* queue full: same message in the next pass */
        }
        sent++;
        next();
    }
    if (g_stage == Stage::Done) {
        espLog(VDM_LOG_INFO, "HA discovery sent");
    }
}

} // namespace app
