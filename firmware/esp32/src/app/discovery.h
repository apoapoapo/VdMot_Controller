/*
 * Home Assistant MQTT discovery (docs/design/esp32-control-and-integration.md,
 * section 8).
 *
 * One device for the controller and one per zone (linked with via_device).
 * The configuration messages are retained and sent step by step from the
 * main loop, as many as the queue to the MQTT task takes per pass.
 * Entities of zones or sources that were removed while running are
 * deleted (empty configuration).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

namespace app
{

/* (re)announces everything; called on connect, on a configuration change
 * and when Home Assistant comes online */
void discoveryStart();
/* sends the next messages; called from the MQTT loop while connected */
void discoveryStep();

} // namespace app
