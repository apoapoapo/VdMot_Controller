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
#include <esp_system.h>
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
    {
        static const char *const kReasons[] = {"unknown",    "power-on",  "external", "software",
                                               "crash",      "watchdog",  "watchdog", "watchdog",
                                               "deep sleep", "brown-out", "SDIO"};
        esp_reset_reason_t r = esp_reset_reason();
        const char *why = r < sizeof(kReasons) / sizeof(kReasons[0]) ? kReasons[r] : "?";
        bool bad = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                   r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
        app::espLog(bad ? VDM_LOG_WARN : VDM_LOG_INFO, "ESP32 started (reset: %s)", why);
    }
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
