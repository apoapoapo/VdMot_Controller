/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/sensor_manager.h"

#include <string.h>

namespace vdm
{

namespace
{

constexpr uint8_t kConvertT = 0x44;
constexpr uint8_t kReadScratchpad = 0xBE;
constexpr uint8_t kDs2438ConvertV = 0xB4;
constexpr uint8_t kDs2438WriteScratchpad = 0x4E;
constexpr uint8_t kDs2438CopyScratchpad = 0x48;
constexpr uint8_t kDs2438RecallMemory = 0xB8;
constexpr uint8_t kDs2438ConfigAd = 0x08;      /* 1: VDD, 0: VAD */
constexpr int16_t kDs18b20PowerOnRaw = 0x0550; /* 85 degC: no conversion happened */

void inc(uint16_t &c)
{
    if (c != UINT16_MAX) {
        c++;
    }
}

} // namespace

uint8_t SensorManager::kindOf(const Device &d, uint8_t i)
{
    if (d.rom[0] != onewire::kFamilyDs2438) {
        return VDM_SENSOR_TEMP_DS18B20;
    }
    static const uint8_t kinds[3] = {VDM_SENSOR_TEMP_DS2438, VDM_SENSOR_VAD_DS2438,
                                     VDM_SENSOR_VDD_DS2438};
    return kinds[i];
}

bool SensorManager::startScan()
{
    if (state_ == State::Scan || scan_requested_) {
        return false;
    }
    scan_requested_ = true;
    return true;
}

uint8_t SensorManager::entryCount() const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < count_; i++) {
        n = static_cast<uint8_t>(n + valuesOf(dev_[i]));
    }
    return n;
}

bool SensorManager::entry(uint8_t index, uint32_t now_ms, vdm_sensor_value_t &out) const
{
    for (uint8_t i = 0; i < count_; i++) {
        const Device &d = dev_[i];
        uint8_t n = valuesOf(d);
        if (index >= n) {
            index = static_cast<uint8_t>(index - n);
            continue;
        }
        const Value &v = d.v[index];
        memcpy(out.rom, d.rom, sizeof(out.rom));
        out.kind = kindOf(d, index);
        out.status = v.status;
        out.value = v.value;
        if (!v.valid) {
            out.age_s = 0xFFFF;
        } else {
            uint32_t age = (now_ms - v.valid_ms) / 1000u;
            out.age_s = age > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(age);
        }
        return true;
    }
    return false;
}

bool SensorManager::takeScanDone(vdm_evt_scan_done_t &evt)
{
    if (!scan_done_) {
        return false;
    }
    scan_done_ = false;
    evt = scan_evt_;
    return true;
}

bool SensorManager::takeCycleDone()
{
    bool d = cycle_done_;
    cycle_done_ = false;
    return d;
}

void SensorManager::fail(Value &v, uint8_t status)
{
    v.status = status;
    if (status == VDM_SSTAT_CRC_ERROR) {
        inc(crc_err_);
    }
}

void SensorManager::store(Value &v, int32_t value, uint32_t now_ms)
{
    v.status = VDM_SSTAT_OK;
    v.valid = true;
    v.valid_ms = now_ms;
    v.value = value;
}

bool SensorManager::select(const uint8_t (&rom)[8])
{
    if (port_.reset() != OneWirePort::Reset::Presence) {
        return false;
    }
    port_.writeByte(onewire::kMatchRom);
    for (uint8_t b : rom) {
        port_.writeByte(b);
    }
    return true;
}

void SensorManager::readDs18b20(Device &d, uint32_t now_ms)
{
    Value &v = d.v[0];
    if (!select(d.rom)) {
        inc(bus_err_);
        fail(v, VDM_SSTAT_UNREACHABLE);
        return;
    }
    port_.writeByte(kReadScratchpad);
    uint8_t sp[9];
    bool all_ones = true;
    for (uint8_t &b : sp) {
        b = port_.readByte();
        all_ones = all_ones && b == 0xFF;
    }
    if (all_ones) {
        fail(v, VDM_SSTAT_UNREACHABLE);
        return;
    }
    if (onewire::crc8(sp, 8) != sp[8]) {
        fail(v, VDM_SSTAT_CRC_ERROR);
        return;
    }
    int16_t raw = static_cast<int16_t>(sp[0] | (sp[1] << 8));
    if (raw == kDs18b20PowerOnRaw) {
        fail(v, VDM_SSTAT_OUT_OF_RANGE);
        return;
    }
    store(v, static_cast<int32_t>(raw) * 125 / 2, now_ms); /* 1/16 degC -> m degC */
}

/* Returns true when all sub-steps of the device are done. */
bool SensorManager::stepDs2438(Device &d, uint32_t now_ms)
{
    switch (ds_step_) {
    case Ds2438Step::ConfigVad:
    case Ds2438Step::ConfigVdd: {
        bool vdd = ds_step_ == Ds2438Step::ConfigVdd;
        if (!select(d.rom)) {
            inc(bus_err_);
            fail(d.v[0], VDM_SSTAT_UNREACHABLE);
            fail(d.v[1], VDM_SSTAT_UNREACHABLE);
            fail(d.v[2], VDM_SSTAT_UNREACHABLE);
            return true;
        }
        port_.writeByte(kDs2438WriteScratchpad);
        port_.writeByte(0x00);
        port_.writeByte(vdd ? kDs2438ConfigAd : 0x00);
        if (select(d.rom)) {
            port_.writeByte(kDs2438CopyScratchpad);
            port_.writeByte(0x00);
        }
        ds_step_ = vdd ? Ds2438Step::ConvertVdd : Ds2438Step::ConvertVad;
        return false;
    }

    case Ds2438Step::ConvertVad:
    case Ds2438Step::ConvertVdd:
        if (select(d.rom)) {
            port_.writeByte(kDs2438ConvertV);
        }
        wait_until_ms_ = now_ms + kDs2438ConvertMs;
        ds_step_ =
            ds_step_ == Ds2438Step::ConvertVad ? Ds2438Step::RecallVad : Ds2438Step::RecallVdd;
        return false;

    case Ds2438Step::RecallVad:
    case Ds2438Step::RecallVdd:
        if (static_cast<int32_t>(now_ms - wait_until_ms_) < 0) {
            return false;
        }
        recall_ok_ = select(d.rom);
        if (recall_ok_) {
            port_.writeByte(kDs2438RecallMemory);
            port_.writeByte(0x00);
        }
        ds_step_ = ds_step_ == Ds2438Step::RecallVad ? Ds2438Step::ReadVad : Ds2438Step::ReadVdd;
        return false;

    case Ds2438Step::ReadVad:
    case Ds2438Step::ReadVdd: {
        bool vdd = ds_step_ == Ds2438Step::ReadVdd;
        Value &volt = d.v[vdd ? 2 : 1];
        uint8_t page[9];
        bool ok = recall_ok_ && select(d.rom);
        if (ok) {
            port_.writeByte(kReadScratchpad);
            port_.writeByte(0x00);
            for (uint8_t &b : page) {
                b = port_.readByte();
            }
        } else {
            inc(bus_err_);
        }
        if (!ok) {
            fail(volt, VDM_SSTAT_UNREACHABLE);
            if (!vdd) {
                fail(d.v[0], VDM_SSTAT_UNREACHABLE);
            }
        } else if (onewire::crc8(page, 8) != page[8]) {
            fail(volt, VDM_SSTAT_CRC_ERROR);
            if (!vdd) {
                d.v[0].status = VDM_SSTAT_CRC_ERROR;
            }
        } else {
            uint16_t mv = static_cast<uint16_t>(((page[4] & 0x03u) << 8 | page[3]) * 10u);
            store(volt, mv, now_ms);
            if (!vdd) {
                /* temperature from the broadcast Convert T, 1/256 degC in
                 * 13 bits left aligned (0.03125 degC per LSB) */
                int16_t raw = static_cast<int16_t>(page[1] | (page[2] << 8));
                store(d.v[0], static_cast<int32_t>(raw >> 3) * 3125 / 100, now_ms);
            }
        }
        if (vdd) {
            return true;
        }
        ds_step_ = Ds2438Step::ConfigVdd;
        return false;
    }
    }
    return true;
}

void SensorManager::finishScan(uint32_t now_ms)
{
    /* keep the values of devices that are still present */
    for (uint8_t i = 0; i < found_count_; i++) {
        for (uint8_t k = 0; k < count_; k++) {
            if (memcmp(found_[i].rom, dev_[k].rom, 8) == 0) {
                memcpy(found_[i].v, dev_[k].v, sizeof(found_[i].v));
                break;
            }
        }
    }
    memcpy(dev_, found_, sizeof(Device) * found_count_);
    count_ = found_count_;

    uint8_t ds18 = 0;
    uint8_t ds2438 = 0;
    for (uint8_t i = 0; i < count_; i++) {
        if (dev_[i].rom[0] == onewire::kFamilyDs2438) {
            ds2438++;
        } else {
            ds18++;
        }
    }
    scan_evt_ = {count_, ds18, ds2438, static_cast<uint8_t>(search_.busError() ? 0 : 1)};
    scan_done_ = true;
    next_cycle_ms_ = now_ms; /* measure right away */
}

void SensorManager::tick(uint32_t now_ms)
{
    switch (state_) {
    case State::Idle:
        if (scan_requested_) {
            scan_requested_ = false;
            search_.restart();
            found_count_ = 0;
            state_ = State::Scan;
        } else if (static_cast<int32_t>(now_ms - next_cycle_ms_) >= 0) {
            next_cycle_ms_ = now_ms + interval_ms_;
            state_ = count_ > 0 ? State::Convert : State::Idle;
        }
        break;

    case State::Scan: {
        uint8_t rom[8];
        if (found_count_ < kMaxDevices && search_.next(port_, rom)) {
            uint8_t fam = rom[0];
            if (fam == onewire::kFamilyDs18b20 || fam == onewire::kFamilyDs2438) {
                Device &d = found_[found_count_++];
                memcpy(d.rom, rom, 8);
                for (Value &v : d.v) {
                    v = Value{};
                }
            }
            break;
        }
        if (search_.busError()) {
            inc(bus_err_);
        }
        finishScan(now_ms);
        state_ = State::Idle;
        break;
    }

    case State::Convert:
        if (port_.reset() != OneWirePort::Reset::Presence) {
            inc(bus_err_);
            for (uint8_t i = 0; i < count_; i++) {
                for (uint8_t k = 0; k < valuesOf(dev_[i]); k++) {
                    fail(dev_[i].v[k], VDM_SSTAT_UNREACHABLE);
                }
            }
            state_ = State::Done;
            break;
        }
        port_.writeByte(onewire::kSkipRom);
        port_.writeByte(kConvertT);
        wait_until_ms_ = now_ms + kConvertMs;
        state_ = State::Wait;
        break;

    case State::Wait:
        if (static_cast<int32_t>(now_ms - wait_until_ms_) >= 0) {
            read_idx_ = 0;
            ds_step_ = Ds2438Step::ConfigVad;
            state_ = State::Read;
        }
        break;

    case State::Read:
        if (read_idx_ >= count_) {
            state_ = State::Done;
            break;
        }
        if (dev_[read_idx_].rom[0] == onewire::kFamilyDs2438) {
            if (!stepDs2438(dev_[read_idx_], now_ms)) {
                break;
            }
            ds_step_ = Ds2438Step::ConfigVad;
        } else {
            readDs18b20(dev_[read_idx_], now_ms);
        }
        read_idx_++;
        break;

    case State::Done:
        cycle_done_ = true;
        state_ = State::Idle;
        break;
    }
}

} // namespace vdm
