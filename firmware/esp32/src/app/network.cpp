/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/network.h"

#include "app/settings.h"

#include <ESPmDNS.h>
#include <ETH.h>
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

volatile bool g_eth_ip = false;
volatile bool g_wifi_ip = false;
bool g_wifi_started = false;
bool g_ap_started = false;
bool g_mdns_started = false;
uint32_t g_start_ms = 0;
String g_ap_ssid;
vdm::NetworkConfig g_net; /* settings at the start; changes need a restart */

void onEvent(arduino_event_id_t event)
{
    switch (event) {
    case ARDUINO_EVENT_ETH_START:
        ETH.setHostname(hostname());
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
    if (!g_net.dhcp) {
        ETH.config(IPAddress(g_net.ip), IPAddress(g_net.gateway), IPAddress(g_net.mask),
                   IPAddress(g_net.dns));
    }
}

void startWifi()
{
    if (g_wifi_started || g_net.ssid[0] == '\0') {
        return;
    }
    g_wifi_started = true;
    WiFi.mode(g_ap_started ? WIFI_AP_STA : WIFI_STA);
    WiFi.setHostname(hostname());
    if (!g_net.dhcp) {
        WiFi.config(IPAddress(g_net.ip), IPAddress(g_net.gateway), IPAddress(g_net.mask),
                    IPAddress(g_net.dns));
    }
    WiFi.begin(g_net.ssid, g_net.wifi_pwd);
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
    g_net = config().network;
    WiFi.onEvent(onEvent);
    g_start_ms = millis();
    if (g_net.mode != vdm::NetMode::Wifi) {
        startEthernet();
    } else {
        startWifi();
    }
    configTzTime(config().device.tz, config().device.ntp);
}

void networkLoop()
{
    uint32_t t = millis() - g_start_ms;
    bool up = g_eth_ip || g_wifi_ip;
    if (!up && g_net.mode == vdm::NetMode::Auto && t >= kWifiFallbackMs) {
        startWifi();
    }
    if (!up && t >= kApFallbackMs) {
        startAp();
    }
    if ((up || g_ap_started) && !g_mdns_started) {
        g_mdns_started = MDNS.begin(hostname());
        if (g_mdns_started) {
            MDNS.addService("http", "tcp", 80);
        }
    }
}

bool networkUp()
{
    return g_eth_ip || g_wifi_ip;
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
