/*
 * Bit-level 1-Wire bus simulator with DS18B20 and DS2438 devices.
 */
#pragma once

#include <string.h>

#include <vector>

#include "core/onewire.h"

namespace sim
{

struct OwDevice {
    uint8_t rom[8];
    bool present = true;
    bool corrupt = false; /* scratchpad CRC wrong */
    bool ignore_convert = false;
    int32_t temp_mc = 21500; /* m degC */
    uint16_t vad_mv = 1230;
    uint16_t vdd_mv = 4980;
    /* internal */
    int16_t scratch_raw = 0x0550; /* DS18B20 power-on value: 85 degC */
    int32_t ds_temp_mc = 0;
    uint8_t config = 0;
    uint16_t latched_mv = 0;
};

inline OwDevice makeDevice(uint8_t family, uint32_t serial)
{
    OwDevice d;
    d.rom[0] = family;
    for (int i = 0; i < 6; i++) {
        d.rom[1 + i] = static_cast<uint8_t>(serial >> (8 * (i % 4)));
    }
    d.rom[6] ^= static_cast<uint8_t>(serial * 7u);
    d.rom[7] = vdm::onewire::crc8(d.rom, 7);
    return d;
}

class FakeBus : public vdm::OneWirePort
{
  public:
    std::vector<OwDevice> devs;
    bool shorted = false;
    uint32_t slots = 0; /* bit slots since the counter was cleared */
    uint32_t resets = 0;

    Reset reset() override
    {
        resets++;
        slots += 8; /* a reset takes ~1 ms, about 8 bit slots */
        if (shorted) {
            return Reset::Short;
        }
        sel_.assign(devs.size(), false);
        bool any = false;
        for (size_t i = 0; i < devs.size(); i++) {
            sel_[i] = devs[i].present;
            any = any || devs[i].present;
        }
        mode_ = Mode::RomCmd;
        acc_ = 0;
        nbits_ = 0;
        out_.clear();
        out_bit_ = 0;
        return any ? Reset::Presence : Reset::NoPresence;
    }

    void writeBit(bool b) override
    {
        slots++;
        if (shorted) {
            return;
        }
        if (mode_ == Mode::Search) {
            for (size_t i = 0; i < devs.size(); i++) {
                if (sel_[i] && romBit(devs[i], search_pos_) != b) {
                    sel_[i] = false;
                }
            }
            search_pos_++;
            search_phase_ = 0;
            return;
        }
        if (b) {
            acc_ |= static_cast<uint8_t>(1u << nbits_);
        }
        if (++nbits_ == 8) {
            uint8_t v = acc_;
            acc_ = 0;
            nbits_ = 0;
            onByte(v);
        }
    }

    bool readBit() override
    {
        slots++;
        if (shorted) {
            return false;
        }
        if (mode_ == Mode::Search) {
            bool r = true; /* wired AND, released bus reads 1 */
            for (size_t i = 0; i < devs.size(); i++) {
                if (sel_[i]) {
                    bool bit = romBit(devs[i], search_pos_);
                    r = r && (search_phase_ == 0 ? bit : !bit);
                }
            }
            search_phase_++;
            return r;
        }
        if (out_bit_ / 8 >= out_.size()) {
            return true;
        }
        bool r = (out_[out_bit_ / 8] >> (out_bit_ % 8)) & 1u;
        out_bit_++;
        return r;
    }

  private:
    enum class Mode { RomCmd, Match, Search, Function, Page, Data, Ignore };

    static bool romBit(const OwDevice &d, int pos)
    {
        return (d.rom[pos / 8] >> (pos % 8)) & 1u;
    }

    OwDevice *selected()
    {
        OwDevice *one = nullptr;
        for (size_t i = 0; i < devs.size(); i++) {
            if (sel_[i]) {
                if (one != nullptr) {
                    return nullptr; /* more than one: no defined answer */
                }
                one = &devs[i];
            }
        }
        return one;
    }

    void send(const uint8_t *p, size_t n)
    {
        out_.assign(p, p + n);
        out_bit_ = 0;
    }

    void onByte(uint8_t v)
    {
        switch (mode_) {
        case Mode::RomCmd:
            if (v == vdm::onewire::kSearchRom) {
                mode_ = Mode::Search;
                search_pos_ = 0;
                search_phase_ = 0;
            } else if (v == vdm::onewire::kSkipRom) {
                mode_ = Mode::Function;
            } else if (v == vdm::onewire::kMatchRom) {
                mode_ = Mode::Match;
                match_n_ = 0;
            } else {
                mode_ = Mode::Ignore;
            }
            break;
        case Mode::Match:
            match_[match_n_++] = v;
            if (match_n_ == 8) {
                for (size_t i = 0; i < devs.size(); i++) {
                    sel_[i] = sel_[i] && memcmp(devs[i].rom, match_, 8) == 0;
                }
                mode_ = Mode::Function;
            }
            break;
        case Mode::Function:
            function(v);
            break;
        case Mode::Page:
            page(v);
            break;
        case Mode::Data: {
            OwDevice *d = selected();
            if (d != nullptr && data_n_++ == 0) {
                d->config = v;
            }
            break;
        }
        default:
            break;
        }
    }

    void function(uint8_t cmd)
    {
        func_ = cmd;
        if (cmd == 0x44) {
            for (size_t i = 0; i < devs.size(); i++) {
                OwDevice &d = devs[i];
                if (!sel_[i] || d.ignore_convert) {
                    continue;
                }
                d.scratch_raw = static_cast<int16_t>(d.temp_mc * 16 / 1000);
                d.ds_temp_mc = d.temp_mc;
            }
            mode_ = Mode::Ignore;
            return;
        }
        OwDevice *d = selected();
        if (d == nullptr) {
            mode_ = Mode::Ignore;
            return;
        }
        if (d->rom[0] == vdm::onewire::kFamilyDs18b20 && cmd == 0xBE) {
            uint8_t sp[9] = {static_cast<uint8_t>(d->scratch_raw),
                             static_cast<uint8_t>(d->scratch_raw >> 8),
                             0x4B,
                             0x46,
                             0x7F,
                             0xFF,
                             0x0C,
                             0x10,
                             0};
            sp[8] = vdm::onewire::crc8(sp, 8) ^ (d->corrupt ? 0x5A : 0);
            send(sp, 9);
            mode_ = Mode::Ignore;
            return;
        }
        if (d->rom[0] == vdm::onewire::kFamilyDs2438) {
            if (cmd == 0xB4) {
                d->latched_mv = (d->config & 0x08) ? d->vdd_mv : d->vad_mv;
                mode_ = Mode::Ignore;
            } else {
                mode_ = Mode::Page; /* BE, 4E, 48, B8 are followed by a page number */
            }
            return;
        }
        mode_ = Mode::Ignore;
    }

    void page(uint8_t pg)
    {
        OwDevice *d = selected();
        if (d == nullptr || pg != 0) {
            mode_ = Mode::Ignore;
            return;
        }
        if (func_ == 0x4E) {
            mode_ = Mode::Data;
            data_n_ = 0;
            return;
        }
        if (func_ == 0xBE) {
            int16_t traw = static_cast<int16_t>((d->ds_temp_mc * 100 / 3125) << 3);
            uint16_t v = static_cast<uint16_t>(d->latched_mv / 10);
            uint8_t sp[9] = {d->config,
                             static_cast<uint8_t>(traw),
                             static_cast<uint8_t>(traw >> 8),
                             static_cast<uint8_t>(v),
                             static_cast<uint8_t>(v >> 8),
                             0,
                             0,
                             0,
                             0};
            sp[8] = vdm::onewire::crc8(sp, 8) ^ (d->corrupt ? 0x5A : 0);
            send(sp, 9);
        }
        mode_ = Mode::Ignore;
    }

    std::vector<bool> sel_;
    Mode mode_ = Mode::Ignore;
    uint8_t acc_ = 0;
    int nbits_ = 0;
    int search_pos_ = 0;
    int search_phase_ = 0;
    uint8_t match_[8] = {};
    int match_n_ = 0;
    uint8_t func_ = 0;
    int data_n_ = 0;
    std::vector<uint8_t> out_;
    size_t out_bit_ = 0;
};

} // namespace sim
