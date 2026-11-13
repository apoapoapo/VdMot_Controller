/*
 * Config - the ESP32 configuration (docs/design/esp32-control-and-integration.md,
 * section 2).
 *
 * Plain structures with fixed sizes, JSON (de)serialization and
 * validation. Loading always starts from the defaults, so missing fields
 * keep their default value and unknown fields are ignored; a document that
 * fails validation is rejected as a whole.
 *
 * Positions are given in percent here; the link protocol uses 0.1 %.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "vdm/link.h"

namespace vdm
{

constexpr uint16_t kConfigVersion = 1;
constexpr uint8_t kMaxSources = 32;
constexpr uint8_t kMaxZones = VDM_MAX_CHANNELS;
constexpr uint8_t kMaxInfoSources = 4;
constexpr size_t kIdSize = 8;    /* "s123" + NUL */
constexpr size_t kNameSize = 33; /* 32 characters */

enum class NetMode : uint8_t { Auto, Ethernet, Wifi };
enum class SourceType : uint8_t { OneWire, Mqtt };
enum class ZoneMode : uint8_t { Control, Manual };
enum class Profile : uint8_t { Floor, Radiator, Custom };

struct DeviceConfig {
    char name[kNameSize] = "VdMot";
    char web_user[kNameSize] = "";
    char web_pwd[65] = "";
    char tz[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
    char ntp[64] = "pool.ntp.org";
};

struct NetworkConfig {
    NetMode mode = NetMode::Auto;
    bool dhcp = true;
    /* IPv4 addresses, first octet in the lowest byte (as IPAddress(uint32_t)) */
    uint32_t ip = 0, mask = 0, gateway = 0, dns = 0;
    char ssid[kNameSize] = "";
    char wifi_pwd[65] = "";
};

struct MqttConfig {
    bool enabled = false;
    char host[64] = "";
    uint16_t port = 1883;
    char user[kNameSize] = "";
    char password[65] = "";
    char client_id[kNameSize] = ""; /* empty: derived from the MAC address */
    char base_topic[64] = "";       /* empty: vdmot/<client id> */
    bool discovery = true;
    char discovery_prefix[kNameSize] = "homeassistant";
};

struct FlowConfig {
    uint8_t fallback_pct = 100; /* also written to the STM32 as failsafe position */
    uint16_t min_total_pct = 0; /* sum of the target positions in percentage points, 0 = off */
};

struct SourceConfig {
    char id[kIdSize] = "";
    char name[kNameSize] = "";
    SourceType type = SourceType::OneWire;
    char rom[24] = "";    /* 1-Wire: "28-FF-..." */
    char topic[128] = ""; /* MQTT */
    char field[32] = "";  /* MQTT: JSON field, empty = plain number */
    float offset = 0.0f;  /* K */
    uint32_t max_age_s = 300;
};

struct ZoneConfig {
    char id[kIdSize] = "";
    char name[kNameSize] = "";
    ZoneMode mode = ZoneMode::Control;
    float setpoint = 20.0f;
    bool heating = true;
    char control_source[kIdSize] = "";
    char info_sources[kMaxInfoSources][kIdSize] = {};
    Profile profile = Profile::Radiator;
    float xp = 1.5f;          /* proportional band in K (profile custom) */
    uint32_t ti_s = 900;      /* integral time (profile custom) */
    uint32_t sample_s = 60;   /* sample time (profile custom) */
    int8_t fallback_pct = -1; /* position while the source is stale, -1: flow.fallback_pct */
};

struct ValveConfig {
    char name[kNameSize] = "";
    char zone[kIdSize] = ""; /* empty: controlled directly (like a zone in manual mode) */
    uint8_t min_pct = 0;
    uint8_t max_pct = 100;
    uint8_t factor_pct = 100;
    bool flow_priority = false;
};

struct Config {
    uint16_t version = kConfigVersion;
    DeviceConfig device;
    NetworkConfig network;
    MqttConfig mqtt;
    FlowConfig flow;
    uint8_t source_count = 0;
    SourceConfig sources[kMaxSources];
    uint8_t zone_count = 0;
    ZoneConfig zones[kMaxZones];
    ValveConfig valves[VDM_MAX_CHANNELS];
    uint16_t next_source_id = 1; /* ids are never reused */
    uint16_t next_zone_id = 1;

    Config();

    int sourceIndex(const char *id) const;
    int zoneIndex(const char *id) const;
};

/* The defaults. Config is large (~10 kB): keep instances off the stack and
 * preferably on the heap. */
const Config &configDefaults();

/* Effective controller parameters of a zone (profile or custom values). */
struct PiParams {
    float xp; /* K */
    uint32_t ti_s;
    uint32_t sample_s;
};
PiParams piParams(const ZoneConfig &z);

/* Size of a JsonDocument that holds any configuration. */
constexpr size_t kConfigJsonCapacity = 24576;

/* Writes the configuration into `obj`. Without `secrets` the passwords are
 * left out and replaced by "<field>_set": true/false. */
void configToJson(const Config &c, JsonObject obj, bool secrets);

/* Reads and validates a configuration. `out` is only changed on success.
 * `err` receives a short English message on failure. */
bool configFromJson(JsonObjectConst obj, Config &out, char *err, size_t err_len);

/* Validates the cross references and value ranges of a configuration. */
bool configValidate(const Config &c, char *err, size_t err_len);

/* Replaces one section of `base` with `section` (JSON) and validates the
 * result. Object sections are merged key by key, so fields that are not
 * sent (e.g. passwords) keep their value; array sections (sources, zones,
 * valves) are replaced. New sources and zones (without "id") get the next
 * free id; references to removed sources or zones are cleared. */
bool configApplySection(const Config &base, const char *name, JsonVariantConst section, Config &out,
                        char *err, size_t err_len);

/* "192.168.1.2" <-> uint32_t (first octet in the lowest byte) */
bool parseIp(const char *s, uint32_t &out);
void formatIp(uint32_t ip, char *buf, size_t len);

/* "28-FF-01-..." (8 hex bytes, any separator) -> bytes */
bool parseRom(const char *s, uint8_t rom[8]);
void formatRom(const uint8_t rom[8], char *buf, size_t len);

/* Host name from a display name: lower case, [a-z0-9-], at most 32 chars. */
void hostnameFrom(const char *name, char *out, size_t len);

const char *zoneModeName(ZoneMode m);
const char *profileName(Profile p);

} // namespace vdm
