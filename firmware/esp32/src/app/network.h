/*
 * Network: Ethernet (WT32-ETH01, LAN8720), WiFi station as fallback and a
 * setup access point if neither works.
 *
 * The settings come from the configuration (app/settings); changes take
 * effect after a restart.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <Arduino.h>

namespace app
{

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
/* an interface has an IP address (not the setup access point) */
bool networkUp();
NetStatus netStatus();

} // namespace app
