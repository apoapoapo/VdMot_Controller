/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/onewire.h"

#include <string.h>

namespace vdm
{
namespace onewire
{

uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int k = 0; k < 8; k++) {
            uint8_t mix = (crc ^ b) & 0x01u;
            crc = static_cast<uint8_t>(crc >> 1);
            if (mix) {
                crc ^= 0x8Cu;
            }
            b = static_cast<uint8_t>(b >> 1);
        }
    }
    return crc;
}

void Search::restart()
{
    memset(rom_, 0, sizeof(rom_));
    last_discrepancy_ = 0;
    last_device_ = false;
    bus_error_ = false;
}

bool Search::next(OneWirePort &port, uint8_t (&rom)[8])
{
    if (last_device_) {
        return false;
    }
    OneWirePort::Reset r = port.reset();
    if (r != OneWirePort::Reset::Presence) {
        bus_error_ = r == OneWirePort::Reset::Short;
        last_device_ = true;
        return false;
    }
    port.writeByte(kSearchRom);

    int last_zero = 0;
    for (int bit = 1; bit <= 64; bit++) {
        int byte = (bit - 1) / 8;
        uint8_t mask = static_cast<uint8_t>(1u << ((bit - 1) % 8));
        bool id = port.readBit();
        bool cmp = port.readBit();
        if (id && cmp) {
            /* nobody answered: devices vanished during the search */
            last_device_ = true;
            return false;
        }
        bool dir;
        if (id != cmp) {
            dir = id;
        } else if (bit < last_discrepancy_) {
            dir = (rom_[byte] & mask) != 0;
        } else {
            dir = bit == last_discrepancy_;
        }
        if (!dir && id == cmp) {
            last_zero = bit;
        }
        if (dir) {
            rom_[byte] |= mask;
        } else {
            rom_[byte] = static_cast<uint8_t>(rom_[byte] & ~mask);
        }
        port.writeBit(dir);
    }
    last_discrepancy_ = last_zero;
    if (last_discrepancy_ == 0) {
        last_device_ = true;
    }
    if (crc8(rom_, 7) != rom_[7] || rom_[0] == 0) {
        bus_error_ = true;
        last_device_ = true;
        return false;
    }
    memcpy(rom, rom_, sizeof(rom_));
    return true;
}

} // namespace onewire
} // namespace vdm
