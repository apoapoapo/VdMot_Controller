/*
 * VdMot Controller v2 - ESP32 (WT32-ETH01) firmware
 *
 * Network, web interface, room control and the link to the STM32 valve
 * controller. See docs/refactoring/PLAN.md (phase 2) and
 * docs/design/esp32-control-and-integration.md.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "app/control.h"
#include "app/link_task.h"
#include "app/mqtt.h"
#include "app/network.h"
#include "app/settings.h"
#include "app/shared.h"
#include "app/web.h"

void setup()
{
    Serial.begin(115200);
    Serial.println("VdMot Controller v2");

    esp_task_wdt_init(15, true);
    esp_task_wdt_add(nullptr);

    if (!LittleFS.begin(false)) {
        Serial.println("LittleFS: formatting");
        LittleFS.begin(true);
    }
    app::sharedInit();
    app::settingsBegin();
    app::controlBegin();
    app::mqttBegin();
    app::startLinkTask();
    app::networkBegin();
    app::webBegin();
}

void loop()
{
    app::networkLoop();
    app::webLoop();
    app::controlLoop();
    app::mqttLoop();
    app::settingsLoop();
    esp_task_wdt_reset();
    delay(2);
}
