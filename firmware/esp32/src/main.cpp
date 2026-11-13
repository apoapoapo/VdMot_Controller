/*
 * VdMot Controller v2 - ESP32 (WT32-ETH01) firmware
 *
 * Network, web interface and the link to the STM32 valve controller.
 * See docs/refactoring/PLAN.md (phase 2).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "app/link_task.h"
#include "app/network.h"
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
    app::startLinkTask();
    app::networkBegin();
    app::webBegin();
}

void loop()
{
    app::networkLoop();
    app::webLoop();
    esp_task_wdt_reset();
    delay(2);
}
