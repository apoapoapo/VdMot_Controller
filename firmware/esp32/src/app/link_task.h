/*
 * Link task: owns the UART to the STM32, the LinkClient and the STM32
 * firmware update.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

namespace app
{

void startLinkTask();

} // namespace app
