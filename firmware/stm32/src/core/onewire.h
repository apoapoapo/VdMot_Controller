/*
 * 1-Wire bus access: port interface, CRC-8 and the ROM search algorithm
 * (Maxim application note 187).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vdm
{

class OneWirePort
{
  public:
    enum class Reset : uint8_t {
        Presence,   /* at least one device answered */
        NoPresence, /* bus fine, nobody there */
        Short,      /* line held low */
    };

    virtual ~OneWirePort() = default;
    virtual Reset reset() = 0;
    virtual void writeBit(bool bit) = 0;
    virtual bool readBit() = 0;

    void writeByte(uint8_t v)
    {
        for (int i = 0; i < 8; i++) {
            writeBit((v >> i) & 1u);
        }
    }
    uint8_t readByte()
    {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) {
            if (readBit()) {
                v |= static_cast<uint8_t>(1u << i);
            }
        }
        return v;
    }
};

namespace onewire
{

constexpr uint8_t kSearchRom = 0xF0;
constexpr uint8_t kMatchRom = 0x55;
constexpr uint8_t kSkipRom = 0xCC;
constexpr uint8_t kFamilyDs18b20 = 0x28;
constexpr uint8_t kFamilyDs2438 = 0x26;

/* Dallas/Maxim CRC-8 (polynomial x^8 + x^5 + x^4 + 1) */
uint8_t crc8(const uint8_t *data, size_t len);

/* Iterative ROM search; next() finds one device per call. */
class Search
{
  public:
    void restart();
    /* true: `rom` holds the next device; false: no more devices or bus error */
    bool next(OneWirePort &port, uint8_t (&rom)[8]);
    bool busError() const
    {
        return bus_error_;
    }

  private:
    uint8_t rom_[8] = {};
    int last_discrepancy_ = 0;
    bool last_device_ = false;
    bool bus_error_ = false;
};

} // namespace onewire
} // namespace vdm
