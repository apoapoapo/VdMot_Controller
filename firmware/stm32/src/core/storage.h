/*
 * Storage - persistent records in the I2C EEPROM.
 *
 * Every record exists in two slots (A/B). A write always goes to the slot
 * that does not hold the newest valid copy, so a power loss during a write
 * never destroys the previous data. Each copy carries a header, a sequence
 * number and a CRC-16:
 *
 *   magic 0x56 | type | seq | len | payload[len] | crc_lo | crc_hi
 *
 * Writes are queued and executed page by page from tick(), without waiting
 * for the EEPROM's internal write cycle.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

#include "core/ports.h"
#include "core/valve_manager.h"
#include "vdm/link.h"

namespace vdm
{

struct SystemData {
    vdm_system_config_t sys;
    vdm_motor_params_t motor;
    uint16_t enabled_mask;
};

struct Counters {
    uint32_t boot_count;
    uint32_t op_hours;
};

class Storage
{
  public:
    static constexpr uint8_t kChannels = VDM_MAX_CHANNELS;

    explicit Storage(EepromPort &eeprom) : eeprom_(eeprom) {}

    /* Loading reads both slots and returns the newest valid copy.
     * false: no valid record (the output is left untouched). */
    bool loadSystem(SystemData &out);
    bool loadCounters(Counters &out);
    bool loadTargets(uint16_t (&targets)[kChannels]);
    bool loadChannel(uint8_t ch, ChannelPersist &out);

    /* Queue a write; a newer save of the same record replaces a pending one. */
    void saveSystem(const SystemData &d);
    void saveCounters(const Counters &c);
    void saveTargets(const uint16_t (&targets)[kChannels]);
    void saveChannel(uint8_t ch, const ChannelPersist &p);

    /* Executes pending writes; call from the main loop. */
    void tick();
    /* Writes everything pending, waiting for the EEPROM (before a reset). */
    void flush();
    bool idle() const
    {
        return pending_ == 0 && active_ < 0;
    }

    uint32_t writes() const
    {
        return writes_;
    }
    uint16_t errors() const
    {
        return errors_;
    }

  private:
    enum : uint8_t {
        kRecSystem = 0,
        kRecCounters = 1,
        kRecTargets = 2,
        kRecChannel0 = 3,
        kRecCount = kRecChannel0 + kChannels,
    };
    static constexpr uint8_t kMagic = 0x56;
    static constexpr uint8_t kHeaderLen = 4;
    static constexpr uint8_t kMaxPayload = 32;
    static constexpr uint8_t kMaxRecord = kHeaderLen + kMaxPayload + 2;

    struct Slot {
        uint16_t addr_a;
        uint16_t addr_b;
        uint8_t type;
        uint8_t len;
    };
    static Slot slot(uint8_t rec);

    bool load(uint8_t rec, uint8_t *payload);
    void queue(uint8_t rec, const uint8_t *payload);
    void startNext();

    EepromPort &eeprom_;

    uint8_t data_[kRecCount][kMaxPayload] = {};
    uint8_t seq_[kRecCount] = {};
    bool newest_is_b_[kRecCount] = {};
    uint32_t pending_ = 0;

    int8_t active_ = -1;
    uint8_t buf_[kMaxRecord] = {};
    uint8_t buf_len_ = 0;
    uint8_t buf_off_ = 0;
    uint16_t buf_addr_ = 0;

    uint32_t writes_ = 0;
    uint16_t errors_ = 0;
};

} // namespace vdm
