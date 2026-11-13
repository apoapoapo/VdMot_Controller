/*
 * VdMot Controller v2 - STM32 firmware
 *
 * Valve actuator controller: drives up to 12 DC valve motors (one at a
 * time), learns their stroke by counting back-EMF pulses and talks to the
 * ESP32 using the link protocol in docs/protocol/stm32-esp32-link.md.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <Arduino.h>
#include <IWatchdog.h>

#include "board/board.h"
#include "board/pins.h"
#include "core/controller.h"

#ifndef VDM_MCU
#error "VDM_MCU must be defined by the build environment"
#endif
#ifndef VDM_HW_REV
#error "VDM_HW_REV must be defined by the build environment"
#endif
#ifndef FW_BUILD
#define FW_BUILD 0
#endif

namespace
{

constexpr uint32_t kWatchdogUs = 2000000; /* 2 s */
constexpr uint32_t kBootWindowMs = 2000;

board::Motor motor;
board::Eeprom eeprom;
board::Link link_port;
board::System system_port;
board::OneWireBus onewire_bus;
vdm::MotorRun motor_run(motor);
vdm::ValveManager valves(motor, motor_run);
vdm::SensorManager sensors(onewire_bus);
vdm::Storage storage(eeprom);
vdm::BoardInfo info;
vdm::Controller *controller = nullptr;

} // namespace

void setup()
{
    board::safeOutputs();
    uint8_t reset_cause = board::readResetCause();

    pinMode(pinNametoDigitalPin(board::kLed), OUTPUT);
    /* firmware update request from the ESP32? (does not return then) */
    board::bootWindow(kBootWindowMs);

    eeprom.begin();
    onewire_bus.begin();
    link_port.begin();

    info.fw_major = FW_VERSION_MAJOR;
    info.fw_minor = FW_VERSION_MINOR;
    info.fw_patch = FW_VERSION_PATCH;
    info.fw_build = FW_BUILD;
    info.hw_rev = VDM_HW_REV;
    info.mcu = VDM_MCU;
    board::readUid(info.mcu_uid);
    info.capabilities = VDM_CAP_DS18B20 | VDM_CAP_DS2438;
    info.reset_cause = reset_cause;

    static vdm::Controller ctl(info, valves, sensors, storage, system_port, link_port);
    controller = &ctl;
    controller->begin(millis());

    board::startSampling(motor_run);
    IWatchdog.begin(kWatchdogUs);
}

void loop()
{
    uint32_t start = micros();
    uint32_t now = millis();

    int c;
    while ((c = link_port.read()) >= 0) {
        controller->onLinkByte(static_cast<uint8_t>(c), now);
    }
    controller->tick(now);

    /* LED: slow blink in normal operation, fast blink while the motor runs
     * or the failsafe is active */
    uint32_t period = (valves.motorBusy() || controller->failsafeActive()) ? 100 : 500;
    digitalWriteFast(board::kLed, (now / period) % 2 ? HIGH : LOW);

    IWatchdog.reload();
    system_port.loopTime(micros() - start);
}
