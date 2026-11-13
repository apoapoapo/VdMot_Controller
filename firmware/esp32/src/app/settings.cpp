/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/settings.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_mac.h>

#include "app/shared.h"

#include "app/control.h"
#include "core/config_store.h"

namespace app
{

namespace
{

constexpr uint32_t kSaveDelayMs = 10000;

class LittleFsAdapter : public vdm::FileSystem
{
  public:
    bool read(const char *path, std::string &out) override
    {
        File f = LittleFS.open(path, "r");
        if (!f) {
            return false;
        }
        size_t n = f.size();
        out.assign(n, '\0');
        bool ok = n == 0 || f.read(reinterpret_cast<uint8_t *>(&out[0]), n) == n;
        f.close();
        return ok;
    }
    bool write(const char *path, const std::string &data) override
    {
        File f = LittleFS.open(path, "w");
        if (!f) {
            return false;
        }
        bool ok =
            f.write(reinterpret_cast<const uint8_t *>(data.data()), data.size()) == data.size();
        f.close();
        return ok;
    }
    bool rename(const char *from, const char *to) override
    {
        return LittleFS.rename(from, to);
    }
    bool remove(const char *path) override
    {
        return LittleFS.remove(path);
    }
    bool exists(const char *path) override
    {
        return LittleFS.exists(path);
    }
};

LittleFsAdapter g_fs;
vdm::ConfigStore g_store(g_fs);
vdm::Config &g_cfg = *new vdm::Config(); /* heap: too large for static RAM */
bool g_dirty = false;
uint32_t g_dirty_ms = 0;
char g_info[128] = "";
char g_hostname[33] = "vdmot";

void copy(char *dst, size_t size, Preferences &p, const char *key)
{
    if (p.isKey(key)) {
        p.getString(key, dst, size);
    }
}

/* network settings, station name and web login of the legacy firmware */
bool takeLegacy(vdm::Config &c)
{
    Preferences p;
    bool found = false;
    if (p.begin("netCfg", true)) { /* read-only */
        found = true;
        uint8_t mode = p.getUChar("ethwifi", 0);
        c.network.mode = mode == 1   ? vdm::NetMode::Ethernet
                         : mode == 2 ? vdm::NetMode::Wifi
                                     : vdm::NetMode::Auto;
        c.network.dhcp = p.getUChar("dhcp", 1) != 0;
        c.network.ip = p.getULong("staticIp", 0);
        c.network.mask = p.getULong("mask", 0);
        c.network.gateway = p.getULong("gw", 0);
        c.network.dns = p.getULong("dnsIp", 0);
        if (c.network.ip == 0 || c.network.mask == 0) {
            c.network.dhcp = true;
        }
        copy(c.network.ssid, sizeof(c.network.ssid), p, "ssid");
        copy(c.network.wifi_pwd, sizeof(c.network.wifi_pwd), p, "pwd");
        copy(c.device.web_user, sizeof(c.device.web_user), p, "userName");
        copy(c.device.web_pwd, sizeof(c.device.web_pwd), p, "userPwd");
        p.end();
    }
    if (p.begin("sysCfg", true)) {
        char name[vdm::kNameSize] = "";
        copy(name, sizeof(name), p, "stName");
        if (name[0] != '\0') {
            strlcpy(c.device.name, name, sizeof(c.device.name));
        }
        p.end();
    }
    return found;
}

void updateHostname()
{
    vdm::hostnameFrom(g_cfg.device.name, g_hostname, sizeof(g_hostname));
}

bool saveNow()
{
    g_dirty = false;
    bool ok = g_store.save(g_cfg);
    if (!ok) {
        Serial.println("config: save failed");
    }
    return ok;
}

} // namespace

void settingsBegin()
{
    vdm::LoadResult r = g_store.load(g_cfg);
    snprintf(g_info, sizeof(g_info), "%s%s%s", vdm::loadResultName(r),
             g_store.lastError()[0] ? ": " : "", g_store.lastError());
    if (r == vdm::LoadResult::Invalid) {
        /* keep the broken file for diagnostics */
        LittleFS.remove("/config.bad");
        LittleFS.rename("/config.json", "/config.bad");
    }
    if (r == vdm::LoadResult::Defaults || r == vdm::LoadResult::Invalid) {
        char err[96];
        vdm::Config &c = g_cfg;
        if (takeLegacy(c) && vdm::configValidate(c, err, sizeof(err))) {
            strlcat(g_info, ", network settings taken over from the 1.x firmware", sizeof(g_info));
        } else {
            c.reset();
        }
        saveNow();
    }
    updateHostname();
    Serial.printf("config: %s\n", g_info);
}

void settingsLoop()
{
    if (g_dirty && millis() - g_dirty_ms >= kSaveDelayMs) {
        saveNow();
    }
}

const vdm::Config &config()
{
    return g_cfg;
}

bool settingsCommit(const vdm::Config &c)
{
    /* saved first: a configuration that cannot be stored is not used */
    if (!g_store.save(c)) {
        espLog(VDM_LOG_ERROR, "configuration could not be saved, change discarded");
        return false;
    }
    g_dirty = false;
    g_cfg = c;
    updateHostname();
    controlReconfigure();
    return true;
}

void mqttBaseTopic(char *out, size_t len)
{
    const vdm::MqttConfig &m = g_cfg.mqtt;
    if (m.base_topic[0] != '\0') {
        strlcpy(out, m.base_topic, len);
        return;
    }
    char id[vdm::kNameSize];
    mqttClientId(id, sizeof(id));
    snprintf(out, len, "vdmot/%s", id);
}

void mqttClientId(char *out, size_t len)
{
    if (g_cfg.mqtt.client_id[0] != '\0') {
        strlcpy(out, g_cfg.mqtt.client_id, len);
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(out, len, "vdmot-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

vdm::Config &settingsEdit()
{
    return g_cfg;
}

void settingsChanged()
{
    if (!g_dirty) {
        g_dirty = true;
        g_dirty_ms = millis();
    }
    controlReconfigure();
}

const char *settingsLoadInfo()
{
    return g_info;
}

const char *hostname()
{
    return g_hostname;
}

} // namespace app
