/*
 * STM32 implementations of the core ports.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include "core/motor_run.h"
#include "core/onewire.h"
#include "core/ports.h"

namespace board
{

/* Puts every motor-related output into its safe state. Called first thing
 * after reset. */
void safeOutputs();

class Motor : public vdm::MotorPort
{
  public:
    void power(bool on) override;
    void select(uint8_t channel, vdm::Dir dir) override;
    void enable(uint8_t channel) override;
    void disableAll() override;
};

class Eeprom : public vdm::EepromPort
{
  public:
    void begin();
    size_t size() const override
    {
        return 8192;
    }
    bool read(uint16_t addr, uint8_t *buf, size_t len) override;
    bool writePage(uint16_t addr, const uint8_t *buf, size_t len) override;
    bool busy() override;
};

/* Bit-banged 1-Wire master (standard speed, Maxim AN126 timing). */
class OneWireBus : public vdm::OneWirePort
{
  public:
    void begin();
    Reset reset() override;
    void writeBit(bool bit) override;
    bool readBit() override;
};

class Link : public vdm::LinkPort
{
  public:
    void begin();
    void write(const uint8_t *data, size_t len) override;
    int read();
};

class System : public vdm::SystemPort
{
  public:
    void reset() override;
    vdm::PlatformDiag diag() override;
    void clearDiag() override;
    void loopTime(uint32_t us);

  private:
    uint32_t loop_max_us_ = 0;
};

/* Starts the 1 kHz current sampling and the pulse interrupt for `run`. */
void startSampling(vdm::MotorRun &run);

/* Boot window for firmware updates: listens on the ESP32 UART (115200 8E1)
 * for "DEADBEEF", answers "BEEFIT" and jumps to the ROM bootloader. Same
 * handshake as the legacy 1.x firmware, needed because BOOT0 is not wired
 * to the ESP32 on the C2/C3 boards. Returns after `window_ms` otherwise. */
void bootWindow(uint32_t window_ms);

/* Reset cause (vdm_reset_cause_t); reads and clears the RCC flags. */
uint8_t readResetCause();
void readUid(uint8_t (&uid)[12]);

} // namespace board
