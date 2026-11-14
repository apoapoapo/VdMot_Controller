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

/* Saves the configuration and, if that worked, makes it the active one and
 * reconfigures the controller. Returns false (nothing changed) otherwise. */
bool settingsCommit(const vdm::Config &c);

/* Small runtime changes (setpoint, mode, heating): saved after a short
 * delay, so a series of changes costs one flash write. */
vdm::Config &settingsEdit();
void settingsChanged();

/* how the configuration was loaded, for the status page */
const char *settingsLoadInfo();

/* host name derived from the device name */
const char *hostname();

/* MQTT client id (configured or "vdmot-" + the last 3 MAC bytes) and the
 * base topic (configured or "vdmot/<client id>") */
void mqttClientId(char *out, size_t len);
void mqttBaseTopic(char *out, size_t len);

} // namespace app
