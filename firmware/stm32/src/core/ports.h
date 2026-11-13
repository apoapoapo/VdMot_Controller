/*
 * Hardware ports used by the hardware-independent core.
 * The STM32 implementations live in src/board/, host tests provide fakes.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vdm
{

enum class Dir : uint8_t {
    Open,
    Close,
};

/* Motor drivers: L293 enables, direction line, MUX and valve PSU. */
class MotorPort
{
  public:
    virtual ~MotorPort() = default;
    /* Switches the valve power supply. */
    virtual void power(bool on) = 0;
    /* Selects channel (MUX) and direction. Only called while all motors are
     * disabled; the caller waits for the MUX to settle afterwards. */
    virtual void select(uint8_t channel, Dir dir) = 0;
    /* Enables the driver of the selected channel. */
    virtual void enable(uint8_t channel) = 0;
    /* Disables all drivers. Must be safe to call from interrupt context. */
    virtual void disableAll() = 0;
};

/* Small I2C EEPROM (24LC64: 8 KiB, 32-byte pages). */
class EepromPort
{
  public:
    static constexpr size_t kPageSize = 32;

    virtual ~EepromPort() = default;
    virtual size_t size() const = 0;
    virtual bool read(uint16_t addr, uint8_t *buf, size_t len) = 0;
    /* Starts a write of at most one page; must not cross a page boundary.
     * Returns false on a bus error. */
    virtual bool writePage(uint16_t addr, const uint8_t *buf, size_t len) = 0;
    /* True while the device is still busy with the internal write cycle. */
    virtual bool busy() = 0;
};

/* Byte sink towards the ESP32. */
class LinkPort
{
  public:
    virtual ~LinkPort() = default;
    virtual void write(const uint8_t *data, size_t len) = 0;
};

/* Platform diagnostics reported in GET_DIAG. */
struct PlatformDiag {
    uint16_t uart_hw_err;
    uint32_t loop_max_us;
    int16_t mcu_temp_c10;
};

/* System services. */
class SystemPort
{
  public:
    virtual ~SystemPort() = default;
    /* Waits until pending link output is sent, then performs a software
     * reset; does not return on the target. */
    virtual void reset() = 0;
    virtual PlatformDiag diag() = 0;
    virtual void clearDiag() = 0;
};

} // namespace vdm
