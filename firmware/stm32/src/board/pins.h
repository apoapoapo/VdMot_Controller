/*
 * Pin assignment of the VdMot Controller board (revisions C1/C2/C3).
 * Source: hardware/schematic_c2-sample_V02.pdf and the legacy firmware
 * (software_stm32/include/hardware.h, Copyright (C) 2021 Lenti84, GPLv3).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <Arduino.h>

#include "vdm/link.h"

namespace board
{

/* L293 enables, one per channel pair (channel n uses enable n / 2) */
constexpr PinName kEnable[6] = {PA_5, PA_6, PA_7, PB_0, PA_15, PB_3};
constexpr PinName kDirection = PA_8;   /* low = open */
constexpr PinName kMux = PB_1;         /* selects the even/odd channel of a pair */
constexpr PinName kPowerEnable = PB_9; /* valve PSU, active low */
constexpr PinName kLed = PC_13;        /* active low */

constexpr uint32_t kRevPulse = PA4;   /* back-EMF commutation pulses */
constexpr uint32_t kCurrent = PA0;    /* motor current, analog */
constexpr uint32_t kCurrentRef = PA1; /* reference/2, analog */

constexpr PinName kOneWire = PB_10; /* 1-Wire bus, external 3k3 pull-up */

constexpr uint32_t kLinkRx = PA10; /* USART1 to the ESP32 */
constexpr uint32_t kLinkTx = PA9;
constexpr uint32_t kI2cScl = PB6;
constexpr uint32_t kI2cSda = PB7;

constexpr uint8_t kEepromAddress = 0x50; /* 24LC64, A0..A2 = GND */

/* MUX levels selecting the even/odd channel of a pair (C1 is inverted).
 * VDM_HW_REV expands to an enum value, so this must not be an #if. */
constexpr bool kIsC1 = VDM_HW_REV == VDM_HW_C1;
constexpr uint32_t kMuxEven = kIsC1 ? 1u : 0u;
constexpr uint32_t kMuxOdd = kIsC1 ? 0u : 1u;

/* current measurement: shunt 3.9 ohm, gain 1.5, 12-bit ADC, 3.3 V:
 * 138 uA per digit -> 0.1 mA units = digits * 138 / 100 */
constexpr int32_t kCurrentUaPerDigit = 138;

} // namespace board
