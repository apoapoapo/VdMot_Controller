/*
 * Control - runs the ControlEngine in the main loop task: feeds it with the
 * link model, sends the effective valve targets to the link task, keeps
 * the STM32 failsafe position in line with the fallback position and keeps
 * the controller state in RTC RAM over a restart.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include "core/control.h"

namespace app
{

void controlBegin();
void controlLoop();
/* the configuration changed (called by app/settings) */
void controlReconfigure();

const vdm::ControlEngine &engine();

/* external valve target (web, later MQTT/KNX); evaluated at once */
vdm::ControlEngine::Result controlSetTarget(uint8_t ch, uint16_t permille);

/* evaluates and sends right away instead of waiting for the next tick */
void controlKick();

} // namespace app
