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
constexpr uint8_t kZoneItems = 9 + vdm::kMaxInfoSources; /* + further temperatures */
constexpr uint8_t kValveItems = 5;
constexpr uint8_t kMaxPerPass = 6;

Stage g_stage = Stage::Done;
bool g_remove = false; /* the current message deletes the entity */
uint8_t g_idx = 0;
uint8_t g_sub = 0;

/* ids announced so far, to delete removed zones and sources */
char g_ann_zones[vdm::kMaxZones][vdm::kIdSize];
char g_ann_sources[vdm::kMaxSources][vdm::kIdSize];
char g_rm_zones[vdm::kMaxZones][vdm::kIdSize];
char g_rm_sources[vdm::kMaxSources][vdm::kIdSize];
uint8_t g_rm_zone_count = 0, g_rm_source_count = 0;
uint16_t g_enabled = 0x0FFF; /* channels enabled on the STM32 */

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

/* Note: ArduinoJson 6 keeps a const char* as a pointer and copies a char*.
 * Everything that is not a literal is assigned as char*, since the document
 * is serialized after the builder returned. */
const char *haName(const char *short_name, const char *name)
{
    return short_name[0] != '\0' ? short_name : name;
}

/* "Position", "Position vorne": function plus the valve's short name (see
 * ValveConfig::short_name) */
void valveLabel(char *out, size_t len, const char *function, const char *suffix)
{
    if (suffix[0] != '\0') {
        snprintf(out, len, "%s %s", function, suffix);
    } else {
        snprintf(out, len, "%s", function);
    }
}

JsonObject base(const Msg &m, const char *name)
{
    g_doc->clear();
    JsonObject o = g_doc->to<JsonObject>();
    o["~"] = mqttBase();
    if (name != nullptr) {
        o["name"] = const_cast<char *>(name); /* copied: may be a local buffer */
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
    d["name"] = const_cast<char *>(config().device.name);
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
    char name[2 * vdm::kNameSize];
    snprintf(name, sizeof(name), "%s %s", config().device.name, z.name);
    d["name"] = name;
    d["mf"] = "VdMot";
    d["mdl"] = "VdMot zone";
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
        a.createNestedObject()["t"] = const_cast<char *>(extra); /* copied */
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

/* used by a zone (control or further temperature): shown there instead */
bool usedByZone(const char *id)
{
    const vdm::Config &c = config();
    for (uint8_t i = 0; i < c.zone_count; i++) {
        for (const auto &x : c.zones[i].control_sources) {
            if (strcmp(x, id) == 0) {
                return true;
            }
        }
        for (const auto &x : c.zones[i].info_sources) {
            if (strcmp(x, id) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool buildSource(uint8_t idx, Msg &m)
{
    const vdm::SourceConfig &s = config().sources[idx];
    m.component = "sensor";
    snprintf(m.object, sizeof(m.object), "%s", s.id);
    if (usedByZone(s.id)) {
        g_remove = true; /* the zone shows it */
        return true;
    }
    JsonObject o = base(m, haName(s.short_name, s.name));
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
        o = base(m, "Controller output");
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
    default: {
        /* further temperatures of the zone */
        uint8_t k = static_cast<uint8_t>(sub - 9);
        if (k >= vdm::kMaxInfoSources) {
            return false;
        }
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "%s_info%u", z.id, k + 1u);
        int si = config().sourceIndex(z.info_sources[k]);
        if (si < 0) {
            g_remove = true; /* not (or no longer) used */
            return true;
        }
        const vdm::SourceConfig &s = config().sources[si];
        o = base(m, haName(s.short_name, s.name));
        char t[48];
        snprintf(t, sizeof(t), "~/source/%s/state", s.id);
        o["stat_t"] = t;
        o["val_tpl"] = "{{ value_json.value }}";
        temperature(o);
        availability(o);
        break;
    }
    }
    zoneDevice(o, z);
    return true;
}

bool buildValve(uint8_t ch, uint8_t sub, Msg &m)
{
    const vdm::Config &c = config();
    if (!((g_enabled >> ch) & 1u)) {
        /* channel disabled on the STM32: no entities */
        static const char *const kVComp[] = {"valve", "binary_sensor", "sensor", "button",
                                             "sensor"};
        static const char *const kVObj[] = {"valve", "fault", "current", "calibrate", "position"};
        if (sub >= kValveItems) {
            return false;
        }
        m.component = kVComp[sub];
        snprintf(m.object, sizeof(m.object), "v%u_%s", ch + 1u, kVObj[sub]);
        g_remove = true;
        return true;
    }
    const vdm::ValveConfig &v = c.valves[ch];
    int vz = c.zoneIndex(v.zone);
    uint8_t in_zone = 0;
    for (const vdm::ValveConfig &x : c.valves) {
        in_zone = static_cast<uint8_t>(in_zone + (vz >= 0 && strcmp(x.zone, v.zone) == 0));
    }
    const char *suffix = v.short_name[0] != '\0'   ? v.short_name
                         : (vz < 0 || in_zone > 1) ? v.name
                                                   : "";
    char st[32], set[32];
    snprintf(st, sizeof(st), "~/valve/%u/state", ch + 1u);
    snprintf(set, sizeof(set), "~/valve/%u/set", ch + 1u);
    JsonObject o;
    char name[64];
    switch (sub) {
    case 0:
        m.component = "valve";
        snprintf(m.object, sizeof(m.object), "v%u_valve", ch + 1u);
        valveLabel(name, sizeof(name), "Valve", suffix);
        o = base(m, name);
        /* position (actual) and opening/closing; commands are the target in % */
        o["cmd_t"] = set;
        o["stat_t"] = st;
        o["val_tpl"] = "{% if value_json.position is none %}None{% else %}"
                       "{{ {'position': value_json.position, 'state': value_json.mode} | tojson }}"
                       "{% endif %}";
        o["reports_position"] = true;
        o["json_attr_t"] = st;
        o["ic"] = "mdi:valve";
        break;
    case 1:
        m.component = "binary_sensor";
        snprintf(m.object, sizeof(m.object), "v%u_fault", ch + 1u);
        valveLabel(name, sizeof(name), "Fault", suffix);
        o = base(m, name);
        o["stat_t"] = st;
        o["val_tpl"] = "{{ 'ON' if value_json.fault or value_json.mode == 'fault' else 'OFF' }}";
        o["json_attr_t"] = st;
        o["dev_cla"] = "problem";
        break;
    case 2:
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "v%u_current", ch + 1u);
        valveLabel(name, sizeof(name), "Motor current", suffix);
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
        valveLabel(name, sizeof(name), "Calibrate", suffix);
        o = base(m, name);
        o["cmd_t"] = set;
        o["pl_prs"] = "{\"calibrate\":true}";
        o["ent_cat"] = "config";
        o["en"] = false;
        break;
    case 4:
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "v%u_position", ch + 1u);
        valveLabel(name, sizeof(name), "Position", suffix);
        o = base(m, name);
        o["stat_t"] = st;
        o["val_tpl"] = "{{ value_json.position }}";
        o["unit_of_meas"] = "%";
        o["stat_cla"] = "measurement";
        o["ic"] = "mdi:valve";
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
    if (sub >= 9) {
        m.component = "sensor";
        snprintf(m.object, sizeof(m.object), "%s_info%u", g_rm_zones[idx], sub - 8u);
    } else {
        m.component = kComp[sub];
        snprintf(m.object, sizeof(m.object), "%s_%s", g_rm_zones[idx], kObj[sub]);
    }
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

bool buildAt(const vdm::Config &c, Msg &m);

/* builds the message at the cursor; false: cursor moved, try again */
bool build(Msg &m, bool &remove)
{
    g_remove = false;
    bool ok = buildAt(config(), m);
    remove = g_remove;
    return ok;
}

bool buildAt(const vdm::Config &c, Msg &m)
{
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
        g_remove = true;
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

void discoverySetEnabled(uint16_t mask)
{
    if (mask != g_enabled) {
        g_enabled = mask;
        discoveryStart();
    }
}

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
