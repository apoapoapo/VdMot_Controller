/*
 * MQTT (docs/design/esp32-control-and-integration.md, section 7).
 *
 * PubSubClient runs in its own task, because connecting blocks. Received
 * messages are queued and handled in the main loop task (control,
 * settings); the main loop builds the state messages and queues them for
 * the MQTT task.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stddef.h>

namespace app
{

void mqttBegin();
void mqttLoop();
/* the configuration changed (called by app/settings) */
void mqttReconfigure();

/* "disabled", "connecting", "connected" */
const char *mqttState();

} // namespace app
