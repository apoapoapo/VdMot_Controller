/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/network.h"

#include <ESPmDNS.h>
#include <ETH.h>
#include <Preferences.h>
#include <WiFi.h>

namespace app
{

namespace
{

/* WT32-ETH01 */
constexpr int kEthAddr = 1;
constexpr int kEthPower = 16;
constexpr int kEthMdc = 23;
constexpr int kEthMdio = 18;

constexpr uint32_t kWifiFallbackMs = 10000; /* auto mode: no Ethernet IP yet */
constexpr uint32_t kApFallbackMs = 30000;   /* nothing works: open the setup AP */
constexpr const char *kApPassword = "vdmot-setup";

NetSettings g_settings;
volatile bool g_eth_ip = false;
volatile bool g_wifi_ip = false;
bool g_wifi_started = false;
bool g_ap_started = false;
bool g_mdns_started = false;
uint32_t g_start_ms = 0;
String g_ap_ssid;

void sanitizeHostname(char *h)
{
    size_t w = 0;
    for (size_t r = 0; h[r] != '\0' && w < 32; r++) {
        char c = h[r];
        if (isalnum(static_cast<unsigned char>(c)) || c == '-') {
            h[w++] = static_cast<char>(tolower(c));
        } else if (c == ' ' || c == '_') {
            h[w++] = '-';
        }
    }
    h[w] = '\0';
    if (w == 0) {
        strlcpy(h, "vdmot", 33);
    }
}

void loadLegacySettings()
{
    Preferences p;
    if (p.begin("netCfg", true)) { /* read-only */
        g_settings.mode = p.getUChar("ethwifi", 0);
        g_settings.dhcp = p.getUChar("dhcp", 1) != 0;
        g_settings.ip = p.getULong("staticIp", 0);
        g_settings.mask = p.getULong("mask", 0);
        g_settings.gateway = p.getULong("gw", 0);
        g_settings.dns = p.getULong("dnsIp", 0);
        if (p.isKey("ssid")) {
            p.getString("ssid", g_settings.ssid, sizeof(g_settings.ssid));
        }
        if (p.isKey("pwd")) {
            p.getString("pwd", g_settings.wifi_pwd, sizeof(g_settings.wifi_pwd));
        }
        if (p.isKey("userName")) {
            p.getString("userName", g_settings.web_user, sizeof(g_settings.web_user));
        }
        if (p.isKey("userPwd")) {
            p.getString("userPwd", g_settings.web_pwd, sizeof(g_settings.web_pwd));
        }
        p.end();
    }
    if (p.begin("sysCfg", true)) {
        if (p.isKey("stName")) {
            p.getString("stName", g_settings.hostname, sizeof(g_settings.hostname));
        }
        p.end();
    }
    sanitizeHostname(g_settings.hostname);
    if (g_settings.mode > 2) {
        g_settings.mode = 0;
    }
}

void onEvent(arduino_event_id_t event)
{
    switch (event) {
    case ARDUINO_EVENT_ETH_START:
        ETH.setHostname(g_settings.hostname);
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
        g_eth_ip = true;
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
        g_eth_ip = false;
        break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        g_wifi_ip = true;
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        g_wifi_ip = false;
        break;
    default:
        break;
    }
}

void startEthernet()
{
    ETH.begin(kEthAddr, kEthPower, kEthMdc, kEthMdio, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
    if (!g_settings.dhcp && g_settings.ip != 0) {
        ETH.config(IPAddress(g_settings.ip), IPAddress(g_settings.gateway),
                   IPAddress(g_settings.mask), IPAddress(g_settings.dns));
    }
}

void startWifi()
{
    if (g_wifi_started || g_settings.ssid[0] == '\0') {
        return;
    }
    g_wifi_started = true;
    WiFi.mode(g_ap_started ? WIFI_AP_STA : WIFI_STA);
    WiFi.setHostname(g_settings.hostname);
    if (!g_settings.dhcp && g_settings.ip != 0) {
        WiFi.config(IPAddress(g_settings.ip), IPAddress(g_settings.gateway),
                    IPAddress(g_settings.mask), IPAddress(g_settings.dns));
    }
    WiFi.begin(g_settings.ssid, g_settings.wifi_pwd);
}

void startAp()
{
    if (g_ap_started) {
        return;
    }
    g_ap_started = true;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char ssid[24];
    snprintf(ssid, sizeof(ssid), "VdMot-%02X%02X", mac[4], mac[5]);
    g_ap_ssid = ssid;
    WiFi.mode(g_wifi_started ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(ssid, kApPassword);
}

} // namespace

void networkBegin()
{
    loadLegacySettings();
    WiFi.onEvent(onEvent);
    g_start_ms = millis();
    if (g_settings.mode != 2) {
        startEthernet();
    }
    if (g_settings.mode == 2) {
        startWifi();
    }
}

void networkLoop()
{
    uint32_t t = millis() - g_start_ms;
    bool up = g_eth_ip || g_wifi_ip;
    if (!up && g_settings.mode == 0 && t >= kWifiFallbackMs) {
        startWifi();
    }
    if (!up && t >= kApFallbackMs) {
        startAp();
    }
    if ((up || g_ap_started) && !g_mdns_started) {
        g_mdns_started = MDNS.begin(g_settings.hostname);
        if (g_mdns_started) {
            MDNS.addService("http", "tcp", 80);
        }
    }
}

const NetSettings &netSettings()
{
    return g_settings;
}

NetStatus netStatus()
{
    NetStatus s{};
    s.ap_active = g_ap_started;
    s.ap_ssid = g_ap_ssid;
    s.rssi = 0;
    if (g_eth_ip) {
        s.interface = "ethernet";
        s.ip = ETH.localIP().toString();
        s.mac = ETH.macAddress();
    } else if (g_wifi_ip) {
        s.interface = "wifi";
        s.ip = WiFi.localIP().toString();
        s.mac = WiFi.macAddress();
        s.rssi = WiFi.RSSI();
    } else if (g_ap_started) {
        s.interface = "ap";
        s.ip = WiFi.softAPIP().toString();
        s.mac = WiFi.softAPmacAddress();
    } else {
        s.interface = "none";
        s.mac = ETH.macAddress();
    }
    return s;
}

} // namespace app
