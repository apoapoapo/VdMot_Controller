/*
 * Settings - owner of the configuration (/config.json on LittleFS).
 *
 * Used from the main loop task only (web, network, control, later MQTT),
 * so no locking is needed.
 *
 * On the very first start (no configuration file) the network settings,
 * the station name and the web login are taken over once from the legacy
 * 1.x NVS, so the device stays reachable. The legacy NVS is not changed.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include "core/config.h"

namespace app
{

void settingsBegin();
void settingsLoop();

const vdm::Config &config();

/* Replaces the configuration, saves it and reconfigures the controller.
 * Returns false if saving failed (the new configuration is still active). */
bool settingsCommit(const vdm::Config &c);

/* Small runtime changes (setpoint, mode, heating): saved after a short
 * delay, so a series of changes costs one flash write. */
vdm::Config &settingsEdit();
void settingsChanged();

/* how the configuration was loaded, for the status page */
const char *settingsLoadInfo();

/* host name derived from the device name */
const char *hostname();

} // namespace app
