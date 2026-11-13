/*
 * SensorManager - non-blocking 1-Wire measurement.
 *
 * Each tick() performs at most one short bus transaction (<= ~10 ms):
 *
 *   scan     one ROM search step per tick
 *   convert  Skip ROM + Convert T (all devices at once), wait 750 ms
 *   read     DS18B20: Match ROM + Read Scratchpad, one device per tick
 *            DS2438:  VAD and VDD conversion + page 0 read, in sub-steps
 *
 * Sensors are identified by their ROM id only. A DS18B20 provides one
 * value (temperature), a DS2438 three (temperature, VAD, VDD).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

#include "core/onewire.h"
#include "vdm/link.h"

namespace vdm
{

class SensorManager
{
  public:
    static constexpr uint8_t kMaxDevices = 32;
    static constexpr uint32_t kConvertMs = 750; /* DS18B20, 12 bit */
    static constexpr uint32_t kDs2438ConvertMs = 10;
    /* longest bus transaction per tick, in bit slots (~65 us each) */
    static constexpr uint32_t kMaxSlotsPerTick = 220;

    explicit SensorManager(OneWirePort &port) : port_(port) {}

    void setInterval(uint16_t seconds)
    {
        interval_ms_ = seconds * 1000UL;
    }
    /* Starts a new bus search; false if a search is already running. */
    bool startScan();
    bool scanning() const
    {
        return state_ == State::Scan;
    }

    void tick(uint32_t now_ms);

    /* Value list (entries), stable until the next scan. */
    uint8_t entryCount() const;
    bool entry(uint8_t index, uint32_t now_ms, vdm_sensor_value_t &out) const;

    /* Events: true once after a finished scan / measurement cycle. */
    bool takeScanDone(vdm_evt_scan_done_t &evt);
    bool takeCycleDone();

    uint16_t crcErrors() const
    {
        return crc_err_;
    }
    uint16_t busErrors() const
    {
        return bus_err_;
    }
    void clearStats()
    {
        crc_err_ = bus_err_ = 0;
    }

  private:
    enum class State : uint8_t { Idle, Scan, Convert, Wait, Read, Done };
    enum class Ds2438Step : uint8_t {
        ConfigVad,
        ConvertVad,
        RecallVad,
        ReadVad,
        ConfigVdd,
        ConvertVdd,
        RecallVdd,
        ReadVdd,
    };

    struct Value {
        uint8_t status = VDM_SSTAT_NOT_MEASURED;
        bool valid = false;
        uint32_t valid_ms = 0;
        int32_t value = 0;
    };

    struct Device {
        uint8_t rom[8];
        Value v[3]; /* DS18B20: [0] temperature; DS2438: [0] temp, [1] VAD, [2] VDD */
    };

    static uint8_t valuesOf(const Device &d)
    {
        return d.rom[0] == onewire::kFamilyDs2438 ? 3 : 1;
    }
    static uint8_t kindOf(const Device &d, uint8_t i);

    bool select(const uint8_t (&rom)[8]);
    void readDs18b20(Device &d, uint32_t now_ms);
    bool stepDs2438(Device &d, uint32_t now_ms);
    void fail(Value &v, uint8_t status);
    void store(Value &v, int32_t value, uint32_t now_ms);
    void finishScan(uint32_t now_ms);

    OneWirePort &port_;
    State state_ = State::Idle;
    uint32_t interval_ms_ = 30000;
    uint32_t next_cycle_ms_ = 0;
    uint32_t wait_until_ms_ = 0;
    bool scan_requested_ = true; /* first scan right after start */

    Device dev_[kMaxDevices];
    uint8_t count_ = 0;

    onewire::Search search_;
    Device found_[kMaxDevices];
    uint8_t found_count_ = 0;

    uint8_t read_idx_ = 0;
    Ds2438Step ds_step_ = Ds2438Step::ConfigVad;
    bool recall_ok_ = false;

    bool scan_done_ = false;
    vdm_evt_scan_done_t scan_evt_{};
    bool cycle_done_ = false;
    uint16_t crc_err_ = 0;
    uint16_t bus_err_ = 0;
};

} // namespace vdm
