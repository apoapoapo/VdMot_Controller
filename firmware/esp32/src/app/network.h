/*
 * Network: Ethernet (WT32-ETH01, LAN8720), WiFi station as fallback and a
 * setup access point if neither works.
 *
 * The settings are read from the legacy 1.x firmware's NVS namespaces
 * ("netCfg", "sysCfg") without modifying them, so switching back to the
 * legacy firmware keeps working.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <Arduino.h>

namespace app
{

struct NetSettings {
    uint8_t mode = 0; /* 0 auto, 1 Ethernet, 2 WiFi (legacy values) */
    bool dhcp = true;
    uint32_t ip = 0, mask = 0, gateway = 0, dns = 0;
    char ssid[65] = "";
    char wifi_pwd[65] = "";
    char hostname[33] = "vdmot";
    char web_user[33] = "";
    char web_pwd[33] = "";
};

struct NetStatus {
    const char *interface; /* "ethernet", "wifi", "ap", "none" */
    String ip;
    String mac;
    int rssi;
    bool ap_active;
    String ap_ssid;
};

void networkBegin();
void networkLoop();
const NetSettings &netSettings();
NetStatus netStatus();

} // namespace app
