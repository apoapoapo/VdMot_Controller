/*
 * Web server: status page, REST API (JSON), STM32 and ESP32 firmware
 * upload. Runs in the Arduino loop task.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

namespace app
{

void webBegin();
void webLoop();

} // namespace app
