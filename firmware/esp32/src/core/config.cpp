/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace vdm
{

namespace
{

void copyStr(char *dst, size_t size, const char *src)
{
    if (size == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Reads fields of one JSON object; the first error is kept. */
class Reader
{
  public:
    Reader(JsonObjectConst o, const char *where, char *err, size_t err_len)
        : o_(o), where_(where), err_(err), err_len_(err_len)
    {
    }

    bool ok() const
    {
        return ok_;
    }

    void str(const char *key, char *dst, size_t size)
    {
        JsonVariantConst v = o_[key];
        if (v.isNull()) {
            return;
        }
        if (!v.is<const char *>()) {
            fail(key, "must be text");
            return;
        }
        const char *s = v.as<const char *>();
        if (strlen(s) >= size) {
            fail(key, "is too long");
            return;
        }
        copyStr(dst, size, s);
    }

    void boolean(const char *key, bool &dst)
    {
        JsonVariantConst v = o_[key];
        if (v.isNull()) {
            return;
        }
        if (!v.is<bool>()) {
            fail(key, "must be true or false");
            return;
        }
        dst = v.as<bool>();
    }

    template <typename T> void integer(const char *key, T &dst, long min, long max)
    {
        JsonVariantConst v = o_[key];
        if (v.isNull()) {
            return;
        }
        if (!v.is<long>() && !v.is<double>()) {
            fail(key, "must be a number");
            return;
        }
        double d = v.as<double>();
        if (d != floor(d) || d < min || d > max) {
            range(key, min, max);
            return;
        }
        dst = static_cast<T>(static_cast<long>(d));
    }

    void real(const char *key, float &dst, float min, float max)
    {
        JsonVariantConst v = o_[key];
        if (v.isNull()) {
            return;
        }
        if (!v.is<double>() && !v.is<long>()) {
            fail(key, "must be a number");
            return;
        }
        double d = v.as<double>();
        if (!(d >= min && d <= max)) {
            char b[48];
            snprintf(b, sizeof(b), "must be %g..%g", static_cast<double>(min),
                     static_cast<double>(max));
            fail(key, b);
            return;
        }
        dst = static_cast<float>(d);
    }

    void ip(const char *key, uint32_t &dst)
    {
        JsonVariantConst v = o_[key];
        if (v.isNull()) {
            return;
        }
        const char *s = v.as<const char *>();
        if (s == nullptr) {
            fail(key, "must be an IPv4 address");
            return;
        }
        if (s[0] == '\0') {
            dst = 0;
            return;
        }
        if (!parseIp(s, dst)) {
            fail(key, "must be an IPv4 address");
        }
    }

    /* enum from a list of names */
    template <typename T> void choice(const char *key, T &dst, const char *const *names, size_t n)
    {
        JsonVariantConst v = o_[key];
        if (v.isNull()) {
            return;
        }
        const char *s = v.as<const char *>();
        for (size_t i = 0; s != nullptr && i < n; i++) {
            if (strcmp(s, names[i]) == 0) {
                dst = static_cast<T>(i);
                return;
            }
        }
        fail(key, "has an unknown value");
    }

    void fail(const char *key, const char *what)
    {
        if (ok_) {
            snprintf(err_, err_len_, "%s.%s %s", where_, key, what);
            ok_ = false;
        }
    }

  private:
    void range(const char *key, long min, long max)
    {
        char b[48];
        snprintf(b, sizeof(b), "must be %ld..%ld", min, max);
        fail(key, b);
    }

    JsonObjectConst o_;
    const char *where_;
    char *err_;
    size_t err_len_;
    bool ok_ = true;
};

const char *const kNetModes[] = {"auto", "ethernet", "wifi"};
const char *const kSourceTypes[] = {"onewire", "mqtt"};
const char *const kZoneModes[] = {"control", "manual"};
const char *const kProfiles[] = {"floor", "radiator", "custom"};

void setErr(char *err, size_t len, const char *fmt, const char *a = "", const char *b = "")
{
    snprintf(err, len, fmt, a, b);
}

bool validId(const char *id, char prefix)
{
    if (id[0] != prefix || id[1] == '\0') {
        return false;
    }
    for (const char *p = id + 1; *p; p++) {
        if (!isdigit(static_cast<unsigned char>(*p))) {
            return false;
        }
    }
    return true;
}

uint16_t idNumber(const char *id)
{
    return static_cast<uint16_t>(atoi(id + 1));
}

/* "<key>_set" for a secret that is not sent */
void putSecret(JsonObject o, const char *key, const char *value, bool secrets)
{
    if (secrets) {
        o[key] = value;
    } else {
        char k[32];
        snprintf(k, sizeof(k), "%s_set", key);
        o[k] = value[0] != '\0';
    }
}

void putIp(JsonObject o, const char *key, uint32_t ip)
{
    char b[16];
    formatIp(ip, b, sizeof(b));
    o[key] = b;
}

/* float values with a fixed number of decimals, so 21.5 stays 21.5 */
float round2(float v)
{
    return roundf(v * 100.0f) / 100.0f;
}

bool hasId(JsonArrayConst list, const char *id)
{
    for (JsonObjectConst o : list) {
        const char *x = o["id"];
        if (x != nullptr && strcmp(x, id) == 0) {
            return true;
        }
    }
    return false;
}

/* new entries of "sources" / "zones" (without id) get the next free id */
bool assignIds(JsonObject root, const char *name)
{
    char prefix;
    const char *counter;
    if (strcmp(name, "sources") == 0) {
        prefix = 's';
        counter = "next_source_id";
    } else if (strcmp(name, "zones") == 0) {
        prefix = 'z';
        counter = "next_zone_id";
    } else {
        return true;
    }
    uint16_t next = root[counter] | 1;
    for (JsonVariant e : root[name].as<JsonArray>()) {
        JsonObject o = e.as<JsonObject>();
        const char *id = o["id"];
        if (o.isNull() || (id != nullptr && id[0] != '\0')) {
            continue;
        }
        char b[kIdSize];
        snprintf(b, sizeof(b), "%c%u", prefix, next++);
        if (!o["id"].set(b)) {
            return false;
        }
    }
    return root[counter].set(next);
}

/* references to removed sources or zones are cleared */
bool dropReferences(JsonObject root, const char *name)
{
    if (strcmp(name, "zones") == 0) {
        JsonArrayConst zones = root["zones"];
        for (JsonVariant v : root["valves"].as<JsonArray>()) {
            const char *z = v["zone"];
            if (z != nullptr && z[0] != '\0' && !hasId(zones, z)) {
                v["zone"] = "";
            }
        }
    } else if (strcmp(name, "sources") == 0) {
        JsonArrayConst sources = root["sources"];
        for (JsonVariant zv : root["zones"].as<JsonArray>()) {
            const char *cs = zv["control_source"];
            if (cs != nullptr && cs[0] != '\0' && !hasId(sources, cs)) {
                zv["control_source"] = "";
            }
            JsonArray info = zv["info_sources"];
            for (size_t i = info.size(); i-- > 0;) {
                const char *id = info[i];
                if (id != nullptr && !hasId(sources, id)) {
                    info.remove(i);
                }
            }
        }
    }
    return true;
}

} // namespace

Config::Config()
{
    for (uint8_t i = 0; i < VDM_MAX_CHANNELS; i++) {
        snprintf(valves[i].name, sizeof(valves[i].name), "Channel %u", i + 1u);
    }
}

const Config &configDefaults()
{
    static const Config *d = new Config(); /* heap: too large for static RAM */
    return *d;
}

int Config::sourceIndex(const char *id) const
{
    if (id == nullptr || id[0] == '\0') {
        return -1;
    }
    for (uint8_t i = 0; i < source_count; i++) {
        if (strcmp(sources[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int Config::zoneIndex(const char *id) const
{
    if (id == nullptr || id[0] == '\0') {
        return -1;
    }
    for (uint8_t i = 0; i < zone_count; i++) {
        if (strcmp(zones[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

PiParams piParams(const ZoneConfig &z)
{
    switch (z.profile) {
    case Profile::Floor:
        return {2.0f, 3600, 300};
    case Profile::Radiator:
        return {1.5f, 900, 60};
    case Profile::Custom:
        break;
    }
    return {z.xp, z.ti_s, z.sample_s};
}

const char *zoneModeName(ZoneMode m)
{
    return kZoneModes[static_cast<uint8_t>(m)];
}

const char *profileName(Profile p)
{
    return kProfiles[static_cast<uint8_t>(p)];
}

/* ---------------------------------------------------------------------- */

void configToJson(const Config &c, JsonObject o, bool secrets)
{
    o["version"] = c.version;

    JsonObject d = o.createNestedObject("device");
    d["name"] = c.device.name;
    d["web_user"] = c.device.web_user;
    putSecret(d, "web_pwd", c.device.web_pwd, secrets);
    d["tz"] = c.device.tz;
    d["ntp"] = c.device.ntp;

    JsonObject n = o.createNestedObject("network");
    n["mode"] = kNetModes[static_cast<uint8_t>(c.network.mode)];
    n["dhcp"] = c.network.dhcp;
    putIp(n, "ip", c.network.ip);
    putIp(n, "mask", c.network.mask);
    putIp(n, "gateway", c.network.gateway);
    putIp(n, "dns", c.network.dns);
    n["ssid"] = c.network.ssid;
    putSecret(n, "wifi_pwd", c.network.wifi_pwd, secrets);

    JsonObject m = o.createNestedObject("mqtt");
    m["enabled"] = c.mqtt.enabled;
    m["host"] = c.mqtt.host;
    m["port"] = c.mqtt.port;
    m["user"] = c.mqtt.user;
    putSecret(m, "password", c.mqtt.password, secrets);
    m["client_id"] = c.mqtt.client_id;
    m["base_topic"] = c.mqtt.base_topic;
    m["discovery"] = c.mqtt.discovery;
    m["discovery_prefix"] = c.mqtt.discovery_prefix;

    JsonObject f = o.createNestedObject("flow");
    f["fallback_pct"] = c.flow.fallback_pct;
    f["min_total_pct"] = c.flow.min_total_pct;

    JsonArray sa = o.createNestedArray("sources");
    for (uint8_t i = 0; i < c.source_count; i++) {
        const SourceConfig &s = c.sources[i];
        JsonObject so = sa.createNestedObject();
        so["id"] = s.id;
        so["name"] = s.name;
        so["type"] = kSourceTypes[static_cast<uint8_t>(s.type)];
        if (s.type == SourceType::OneWire) {
            so["rom"] = s.rom;
        } else {
            so["topic"] = s.topic;
            so["field"] = s.field;
        }
        so["offset"] = round2(s.offset);
        so["max_age_s"] = s.max_age_s;
    }

    JsonArray za = o.createNestedArray("zones");
    for (uint8_t i = 0; i < c.zone_count; i++) {
        const ZoneConfig &z = c.zones[i];
        JsonObject zo = za.createNestedObject();
        zo["id"] = z.id;
        zo["name"] = z.name;
        zo["mode"] = kZoneModes[static_cast<uint8_t>(z.mode)];
        zo["setpoint"] = round2(z.setpoint);
        zo["heating"] = z.heating;
        zo["control_source"] = z.control_source;
        JsonArray info = zo.createNestedArray("info_sources");
        for (const auto &id : z.info_sources) {
            if (id[0] != '\0') {
                info.add(id);
            }
        }
        zo["profile"] = kProfiles[static_cast<uint8_t>(z.profile)];
        zo["xp"] = round2(z.xp);
        zo["ti_s"] = z.ti_s;
        zo["sample_s"] = z.sample_s;
        zo["fallback_pct"] = z.fallback_pct;
    }

    JsonArray va = o.createNestedArray("valves");
    for (const ValveConfig &v : c.valves) {
        JsonObject vo = va.createNestedObject();
        vo["name"] = v.name;
        vo["zone"] = v.zone;
        vo["min_pct"] = v.min_pct;
        vo["max_pct"] = v.max_pct;
        vo["factor_pct"] = v.factor_pct;
        vo["flow_priority"] = v.flow_priority;
    }

    o["next_source_id"] = c.next_source_id;
    o["next_zone_id"] = c.next_zone_id;
}

bool configFromJson(JsonObjectConst o, Config &out, char *err, size_t err_len)
{
    static Config &c = *new Config(); /* large: neither stack nor static RAM */
    c = configDefaults();
    err[0] = '\0';

    if (o.isNull()) {
        setErr(err, err_len, "configuration must be a JSON object");
        return false;
    }

    Reader top(o, "config", err, err_len);
    top.integer("version", c.version, 1, 65535);
    top.integer("next_source_id", c.next_source_id, 1, 65535);
    top.integer("next_zone_id", c.next_zone_id, 1, 65535);
    if (!top.ok()) {
        return false;
    }

    {
        Reader r(o["device"], "device", err, err_len);
        r.str("name", c.device.name, sizeof(c.device.name));
        r.str("web_user", c.device.web_user, sizeof(c.device.web_user));
        r.str("web_pwd", c.device.web_pwd, sizeof(c.device.web_pwd));
        r.str("tz", c.device.tz, sizeof(c.device.tz));
        r.str("ntp", c.device.ntp, sizeof(c.device.ntp));
        if (!r.ok()) {
            return false;
        }
    }
    {
        Reader r(o["network"], "network", err, err_len);
        r.choice("mode", c.network.mode, kNetModes, 3);
        r.boolean("dhcp", c.network.dhcp);
        r.ip("ip", c.network.ip);
        r.ip("mask", c.network.mask);
        r.ip("gateway", c.network.gateway);
        r.ip("dns", c.network.dns);
        r.str("ssid", c.network.ssid, sizeof(c.network.ssid));
        r.str("wifi_pwd", c.network.wifi_pwd, sizeof(c.network.wifi_pwd));
        if (!r.ok()) {
            return false;
        }
    }
    {
        Reader r(o["mqtt"], "mqtt", err, err_len);
        r.boolean("enabled", c.mqtt.enabled);
        r.str("host", c.mqtt.host, sizeof(c.mqtt.host));
        r.integer("port", c.mqtt.port, 1, 65535);
        r.str("user", c.mqtt.user, sizeof(c.mqtt.user));
        r.str("password", c.mqtt.password, sizeof(c.mqtt.password));
        r.str("client_id", c.mqtt.client_id, sizeof(c.mqtt.client_id));
        r.str("base_topic", c.mqtt.base_topic, sizeof(c.mqtt.base_topic));
        r.boolean("discovery", c.mqtt.discovery);
        r.str("discovery_prefix", c.mqtt.discovery_prefix, sizeof(c.mqtt.discovery_prefix));
        if (!r.ok()) {
            return false;
        }
    }
    {
        Reader r(o["flow"], "flow", err, err_len);
        r.integer("fallback_pct", c.flow.fallback_pct, 0, 100);
        r.integer("min_total_pct", c.flow.min_total_pct, 0, 100 * VDM_MAX_CHANNELS);
        if (!r.ok()) {
            return false;
        }
    }

    JsonArrayConst sa = o["sources"];
    if (!o["sources"].isNull() && sa.isNull()) {
        setErr(err, err_len, "sources must be a list");
        return false;
    }
    if (sa.size() > kMaxSources) {
        snprintf(err, err_len, "at most %u temperature sources", kMaxSources);
        return false;
    }
    for (JsonObjectConst so : sa) {
        SourceConfig &s = c.sources[c.source_count++];
        Reader r(so, "source", err, err_len);
        r.str("id", s.id, sizeof(s.id));
        r.str("name", s.name, sizeof(s.name));
        r.choice("type", s.type, kSourceTypes, 2);
        s.max_age_s = s.type == SourceType::Mqtt ? 3600 : 300;
        r.str("rom", s.rom, sizeof(s.rom));
        r.str("topic", s.topic, sizeof(s.topic));
        r.str("field", s.field, sizeof(s.field));
        r.real("offset", s.offset, -10.0f, 10.0f);
        r.integer("max_age_s", s.max_age_s, 0, 7L * 24 * 3600);
        if (!r.ok()) {
            return false;
        }
    }

    JsonArrayConst za = o["zones"];
    if (!o["zones"].isNull() && za.isNull()) {
        setErr(err, err_len, "zones must be a list");
        return false;
    }
    if (za.size() > kMaxZones) {
        snprintf(err, err_len, "at most %u zones", kMaxZones);
        return false;
    }
    for (JsonObjectConst zo : za) {
        ZoneConfig &z = c.zones[c.zone_count++];
        Reader r(zo, "zone", err, err_len);
        r.str("id", z.id, sizeof(z.id));
        r.str("name", z.name, sizeof(z.name));
        r.choice("mode", z.mode, kZoneModes, 2);
        r.real("setpoint", z.setpoint, 5.0f, 30.0f);
        r.boolean("heating", z.heating);
        r.str("control_source", z.control_source, sizeof(z.control_source));
        r.choice("profile", z.profile, kProfiles, 3);
        r.real("xp", z.xp, 0.2f, 20.0f);
        r.integer("ti_s", z.ti_s, 0, 24L * 3600);
        r.integer("sample_s", z.sample_s, 10, 3600);
        r.integer("fallback_pct", z.fallback_pct, -1, 100);
        JsonVariantConst iv = zo["info_sources"];
        JsonArrayConst info = iv;
        if (!iv.isNull() && info.isNull()) {
            r.fail("info_sources", "must be a list");
        } else if (info.size() > kMaxInfoSources) {
            r.fail("info_sources", "has too many entries");
        } else {
            uint8_t k = 0;
            for (JsonVariantConst id : info) {
                const char *s = id.as<const char *>();
                if (s == nullptr || strlen(s) >= kIdSize) {
                    r.fail("info_sources", "contains an invalid id");
                    break;
                }
                copyStr(z.info_sources[k++], kIdSize, s);
            }
        }
        if (!r.ok()) {
            return false;
        }
    }

    JsonVariantConst vv = o["valves"];
    JsonArrayConst va = vv;
    if (!vv.isNull() && (va.isNull() || va.size() > VDM_MAX_CHANNELS)) {
        snprintf(err, err_len, "valves must be a list of at most %u entries", VDM_MAX_CHANNELS);
        return false;
    }
    uint8_t ch = 0;
    for (JsonObjectConst vo : va) {
        ValveConfig &v = c.valves[ch++];
        Reader r(vo, "valve", err, err_len);
        r.str("name", v.name, sizeof(v.name));
        r.str("zone", v.zone, sizeof(v.zone));
        r.integer("min_pct", v.min_pct, 0, 100);
        r.integer("max_pct", v.max_pct, 0, 100);
        r.integer("factor_pct", v.factor_pct, 10, 250);
        r.boolean("flow_priority", v.flow_priority);
        if (!r.ok()) {
            return false;
        }
    }

    if (!configValidate(c, err, err_len)) {
        return false;
    }
    out = c;
    return true;
}

bool configValidate(const Config &c, char *err, size_t err_len)
{
    if (c.device.name[0] == '\0') {
        setErr(err, err_len, "device.name must not be empty");
        return false;
    }
    if (!c.network.dhcp && (c.network.ip == 0 || c.network.mask == 0)) {
        setErr(err, err_len, "network: a static address needs ip and mask");
        return false;
    }
    if (c.mqtt.enabled && c.mqtt.host[0] == '\0') {
        setErr(err, err_len, "mqtt.host is required when MQTT is enabled");
        return false;
    }

    for (uint8_t i = 0; i < c.source_count; i++) {
        const SourceConfig &s = c.sources[i];
        if (!validId(s.id, 's')) {
            setErr(err, err_len, "source id '%s' is invalid (s<number>)", s.id);
            return false;
        }
        if (idNumber(s.id) >= c.next_source_id) {
            setErr(err, err_len, "source id '%s' is not below next_source_id", s.id);
            return false;
        }
        if (c.sourceIndex(s.id) != i) {
            setErr(err, err_len, "source id '%s' is used twice", s.id);
            return false;
        }
        if (s.name[0] == '\0') {
            setErr(err, err_len, "source '%s' needs a name", s.id);
            return false;
        }
        uint8_t rom[8];
        if (s.type == SourceType::OneWire && !parseRom(s.rom, rom)) {
            setErr(err, err_len, "source '%s': invalid 1-Wire ROM id '%s'", s.name, s.rom);
            return false;
        }
        if (s.type == SourceType::Mqtt && s.topic[0] == '\0') {
            setErr(err, err_len, "source '%s' needs an MQTT topic", s.name);
            return false;
        }
    }

    for (uint8_t i = 0; i < c.zone_count; i++) {
        const ZoneConfig &z = c.zones[i];
        if (!validId(z.id, 'z')) {
            setErr(err, err_len, "zone id '%s' is invalid (z<number>)", z.id);
            return false;
        }
        if (idNumber(z.id) >= c.next_zone_id) {
            setErr(err, err_len, "zone id '%s' is not below next_zone_id", z.id);
            return false;
        }
        if (c.zoneIndex(z.id) != i) {
            setErr(err, err_len, "zone id '%s' is used twice", z.id);
            return false;
        }
        if (z.name[0] == '\0') {
            setErr(err, err_len, "zone '%s' needs a name", z.id);
            return false;
        }
        if (z.control_source[0] != '\0' && c.sourceIndex(z.control_source) < 0) {
            setErr(err, err_len, "zone '%s': unknown control source '%s'", z.name,
                   z.control_source);
            return false;
        }
        for (const auto &id : z.info_sources) {
            if (id[0] != '\0' && c.sourceIndex(id) < 0) {
                setErr(err, err_len, "zone '%s': unknown info source '%s'", z.name, id);
                return false;
            }
        }
        if (z.profile == Profile::Custom && z.ti_s != 0 && z.ti_s < z.sample_s) {
            setErr(err, err_len, "zone '%s': integral time must not be below the sample time",
                   z.name);
            return false;
        }
    }

    for (const ValveConfig &v : c.valves) {
        if (v.zone[0] != '\0' && c.zoneIndex(v.zone) < 0) {
            setErr(err, err_len, "valve '%s': unknown zone '%s'", v.name, v.zone);
            return false;
        }
        if (v.min_pct > v.max_pct) {
            setErr(err, err_len, "valve '%s': minimum position above the maximum", v.name);
            return false;
        }
    }
    return true;
}

bool configApplySection(const Config &base, const char *name, JsonVariantConst section, Config &out,
                        char *err, size_t err_len)
{
    static DynamicJsonDocument doc(kConfigJsonCapacity);
    doc.clear();
    JsonObject root = doc.to<JsonObject>();
    configToJson(base, root, true);

    static const char *const kObjects[] = {"device", "network", "mqtt", "flow"};
    static const char *const kArrays[] = {"sources", "zones", "valves"};
    bool known = false;
    for (const char *k : kObjects) {
        if (strcmp(name, k) == 0) {
            JsonObjectConst in = section;
            if (in.isNull()) {
                snprintf(err, err_len, "%s must be a JSON object", name);
                return false;
            }
            JsonObject target = root[name];
            for (JsonPairConst p : in) {
                if (!target[p.key()].set(p.value())) {
                    snprintf(err, err_len, "configuration too large");
                    return false;
                }
            }
            known = true;
        }
    }
    for (const char *k : kArrays) {
        if (strcmp(name, k) == 0) {
            if (!section.is<JsonArrayConst>()) {
                snprintf(err, err_len, "%s must be a list", name);
                return false;
            }
            if (!root[name].set(section)) {
                snprintf(err, err_len, "configuration too large");
                return false;
            }
            if (!assignIds(root, name) || !dropReferences(root, name)) {
                snprintf(err, err_len, "configuration too large");
                return false;
            }
            known = true;
        }
    }
    if (!known) {
        snprintf(err, err_len, "unknown section '%s'", name);
        return false;
    }
    if (doc.overflowed()) {
        snprintf(err, err_len, "configuration too large");
        return false;
    }
    return configFromJson(root, out, err, err_len);
}

/* ---------------------------------------------------------------------- */

bool parseIp(const char *s, uint32_t &out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isdigit(static_cast<unsigned char>(*s))) {
            return false;
        }
        unsigned n = 0;
        int digits = 0;
        while (isdigit(static_cast<unsigned char>(*s))) {
            n = n * 10u + static_cast<unsigned>(*s++ - '0');
            if (++digits > 3) {
                return false;
            }
        }
        if (n > 255) {
            return false;
        }
        v |= static_cast<uint32_t>(n) << (8 * i);
        if (i < 3 && *s++ != '.') {
            return false;
        }
    }
    if (*s != '\0') {
        return false;
    }
    out = v;
    return true;
}

void formatIp(uint32_t ip, char *buf, size_t len)
{
    if (ip == 0) {
        copyStr(buf, len, "");
        return;
    }
    snprintf(buf, len, "%u.%u.%u.%u", static_cast<unsigned>(ip & 0xFFu),
             static_cast<unsigned>((ip >> 8) & 0xFFu), static_cast<unsigned>((ip >> 16) & 0xFFu),
             static_cast<unsigned>(ip >> 24));
}

bool parseRom(const char *s, uint8_t rom[8])
{
    int n = 0;
    while (*s != '\0' && n < 8) {
        if (!isxdigit(static_cast<unsigned char>(s[0])) ||
            !isxdigit(static_cast<unsigned char>(s[1]))) {
            return false;
        }
        char hex[3] = {s[0], s[1], '\0'};
        rom[n++] = static_cast<uint8_t>(strtoul(hex, nullptr, 16));
        s += 2;
        if (*s == '-' || *s == ':' || *s == ' ') {
            s++;
        }
    }
    return n == 8 && *s == '\0';
}

void formatRom(const uint8_t rom[8], char *buf, size_t len)
{
    snprintf(buf, len, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", rom[0], rom[1], rom[2], rom[3],
             rom[4], rom[5], rom[6], rom[7]);
}

void hostnameFrom(const char *name, char *out, size_t len)
{
    size_t w = 0;
    for (const char *p = name; *p != '\0' && w + 1 < len && w < 32; p++) {
        char ch = *p;
        if (isalnum(static_cast<unsigned char>(ch))) {
            out[w++] = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        } else if ((ch == ' ' || ch == '_' || ch == '-') && w > 0 && out[w - 1] != '-') {
            out[w++] = '-';
        }
    }
    while (w > 0 && out[w - 1] == '-') {
        w--;
    }
    out[w] = '\0';
    if (w == 0) {
        copyStr(out, len, "vdmot");
    }
}

} // namespace vdm
