/* Simulated STM32 ROM bootloader (AN3155 subset) behind a BootPort. */
#pragma once

#include <string.h>

#include <deque>
#include <vector>

#include "core/stm32_flasher.h"

namespace sim
{

class FakeBootloader : public vdm::BootPort
{
  public:
    std::vector<uint8_t> flash;
    uint16_t chip_id = 0x431;
    uint32_t accept_baud = 0; /* 0: any */
    bool silent = false;      /* no bootloader at all */
    bool boot0_wired = true;  /* BOOT0 reaches the STM32 */
    bool app_present = true;  /* an application with a DEADBEEF boot window runs */
    bool start_in_bootloader = false;
    bool reset_wired = true; /* NRST reaches the STM32 */
    int go_count = 0;
    int software_entries = 0;
    int nack_writes = 0;       /* NACK the next n write data phases */
    bool corrupt_read = false; /* flip a bit in read data */
    bool boot0 = false;
    bool reset = false;
    bool in_bootloader = false;
    int app_starts = 0;
    uint32_t sectors_erased = 0; /* bit mask */
    uint32_t baud = 0;
    bool parity = false;

    explicit FakeBootloader(size_t size = 512 * 1024) : flash(size, 0x00) {}

    void begin(uint32_t b, bool even) override
    {
        baud = b;
        parity = even;
    }
    void write(const uint8_t *d, size_t n) override
    {
        for (size_t i = 0; i < n; i++)
            onByte(d[i]);
    }
    int read(uint32_t) override
    {
        if (out_.empty())
            return -1;
        int v = out_.front();
        out_.pop_front();
        return v;
    }
    void drain() override
    {
        out_.clear();
    }
    void setBoot0(bool h) override
    {
        boot0 = h;
    }
    void setReset(bool a) override
    {
        if (reset_wired && reset && !a) { /* released */
            restart();
        }
        reset = a;
    }
    /* the RESET button of the BlackPill */
    void pressReset()
    {
        restart();
    }
    void restart()
    {
        bool b0 = boot0 && boot0_wired;
        in_bootloader = b0 && !silent;
        state_ = St::Sync;
        out_.clear();
        app_window_ = !b0 && app_present;
        app_match_ = 0;
        if (!b0)
            app_starts++;
    }
    void init()
    {
        if (start_in_bootloader) {
            in_bootloader = true;
            state_ = St::Sync;
        }
    }
    void delayMs(uint32_t) override {}

  private:
    enum class St { Sync, Cmd, CmdC, Addr, Len, Data, ReadLen, EraseHdr, EraseList };
    static constexpr uint8_t kAck = 0x79, kNack = 0x1F;

    static uint32_t sectorStart(uint16_t s)
    {
        if (s <= 4)
            return s * 16384u;
        return 131072u + (s - 5u) * 131072u;
    }
    static uint32_t sectorSize(uint16_t s)
    {
        return s < 4 ? 16384u : (s == 4 ? 65536u : 131072u);
    }

    void ack(bool ok)
    {
        out_.push_back(ok ? kAck : kNack);
    }

    void onByte(uint8_t b)
    {
        if (!in_bootloader) {
            /* application boot window: 115200 8E1, "DEADBEEF" -> "BEEFIT" */
            static const char kReq[] = "DEADBEEF";
            if (!app_window_ || baud != 115200 || !parity || silent)
                return;
            app_match_ = b == (uint8_t)kReq[app_match_] ? app_match_ + 1 : (b == 'D' ? 1 : 0);
            if (app_match_ == 8) {
                for (const char *p = "BEEFIT\r\n"; *p; p++)
                    out_.push_back((uint8_t)*p);
                app_window_ = false;
                in_bootloader = true;
                state_ = St::Sync;
                software_entries++;
            }
            return;
        }
        switch (state_) {
        case St::Sync:
            if (b == 0x7F && parity && (accept_baud == 0 || accept_baud == baud)) {
                ack(true);
                state_ = St::Cmd;
            }
            break;
        case St::Cmd:
            cmd_ = b;
            state_ = St::CmdC;
            break;
        case St::CmdC:
            if ((uint8_t)~cmd_ != b) {
                ack(false);
                state_ = St::Cmd;
                break;
            }
            ack(true);
            buf_.clear();
            if (cmd_ == 0x02) {
                out_.push_back(1);
                out_.push_back((uint8_t)(chip_id >> 8));
                out_.push_back((uint8_t)chip_id);
                ack(true);
                state_ = St::Cmd;
            } else if (cmd_ == 0x31 || cmd_ == 0x11 || cmd_ == 0x21) {
                state_ = St::Addr;
            } else if (cmd_ == 0x44) {
                state_ = St::EraseHdr;
            } else {
                state_ = St::Cmd;
            }
            break;
        case St::Addr:
            buf_.push_back(b);
            if (buf_.size() == 5) {
                uint8_t x = buf_[0] ^ buf_[1] ^ buf_[2] ^ buf_[3];
                addr_ = (uint32_t)buf_[0] << 24 | buf_[1] << 16 | buf_[2] << 8 | buf_[3];
                bool ok =
                    x == buf_[4] && addr_ >= 0x08000000u && addr_ < 0x08000000u + flash.size();
                ack(ok);
                buf_.clear();
                if (ok && cmd_ == 0x21) { /* GO: start the application */
                    go_count++;
                    in_bootloader = false;
                    app_starts++;
                    state_ = St::Sync;
                    break;
                }
                state_ = !ok ? St::Cmd : (cmd_ == 0x31 ? St::Len : St::ReadLen);
            }
            break;
        case St::Len:
            len_ = b + 1u;
            buf_.assign(1, b);
            state_ = St::Data;
            break;
        case St::Data:
            buf_.push_back(b);
            if (buf_.size() == len_ + 2) {
                uint8_t x = 0;
                for (size_t i = 0; i + 1 < buf_.size(); i++)
                    x ^= buf_[i];
                bool ok = x == buf_.back() && len_ % 4 == 0;
                if (ok && nack_writes > 0) {
                    nack_writes--;
                    ok = false;
                }
                if (ok) {
                    uint32_t off = addr_ - 0x08000000u;
                    for (size_t i = 0; i < len_; i++)
                        flash[off + i] &= buf_[1 + i];
                }
                ack(ok);
                state_ = St::Cmd;
            }
            break;
        case St::ReadLen:
            buf_.push_back(b);
            if (buf_.size() == 2) {
                bool ok = (uint8_t)~buf_[0] == buf_[1];
                ack(ok);
                if (ok) {
                    uint32_t off = addr_ - 0x08000000u;
                    for (size_t i = 0; i <= buf_[0]; i++) {
                        uint8_t v = flash[off + i];
                        if (corrupt_read && i == 3)
                            v ^= 0x10;
                        out_.push_back(v);
                    }
                }
                state_ = St::Cmd;
            }
            break;
        case St::EraseHdr:
            buf_.push_back(b);
            if (buf_.size() == 2) {
                n_sectors_ = ((buf_[0] << 8) | buf_[1]) + 1u;
                state_ = St::EraseList;
            }
            break;
        case St::EraseList:
            buf_.push_back(b);
            if (buf_.size() == 2 + 2 * n_sectors_ + 1) {
                uint8_t x = 0;
                for (size_t i = 0; i + 1 < buf_.size(); i++)
                    x ^= buf_[i];
                bool ok = x == buf_.back();
                for (size_t k = 0; ok && k < n_sectors_; k++) {
                    uint16_t s = (uint16_t)(buf_[2 + 2 * k] << 8 | buf_[3 + 2 * k]);
                    if (sectorStart(s) + sectorSize(s) > flash.size()) {
                        ok = false;
                        break;
                    }
                    memset(&flash[sectorStart(s)], 0xFF, sectorSize(s));
                    sectors_erased |= 1u << s;
                }
                ack(ok);
                state_ = St::Cmd;
            }
            break;
        }
    }

    bool app_window_ = false;
    size_t app_match_ = 0;
    St state_ = St::Sync;
    uint8_t cmd_ = 0;
    std::vector<uint8_t> buf_;
    uint32_t addr_ = 0;
    size_t len_ = 0;
    size_t n_sectors_ = 0;
    std::deque<uint8_t> out_;
};

struct MemImage : vdm::ImageSource {
    std::vector<uint8_t> data;
    size_t size() const override
    {
        return data.size();
    }
    bool read(size_t off, uint8_t *buf, size_t len) override
    {
        if (off + len > data.size())
            return false;
        memcpy(buf, &data[off], len);
        return true;
    }
};

/* puts a firmware info block (vdm/fw_info.h) at `offset` */
inline void addFwInfo(MemImage &img, size_t offset, uint16_t a, uint16_t b)
{
    const uint8_t info[16] = {'V',        'D',
                              'M',        'O',
                              'T',        'F',
                              'W',        '2',
                              (uint8_t)a, (uint8_t)(a >> 8),
                              (uint8_t)b, (uint8_t)(b >> 8),
                              2,          1,
                              0,          0};
    memcpy(&img.data[offset], info, sizeof(info));
}

inline MemImage makeImage(size_t size, uint32_t seed = 1)
{
    MemImage img;
    img.data.resize(size);
    uint32_t x = seed;
    for (size_t i = 0; i < size; i++) {
        x = x * 1103515245u + 12345u;
        img.data[i] = (uint8_t)(x >> 16);
    }
    const uint32_t sp = 0x20020000u, pc = 0x08000199u;
    memcpy(&img.data[0], &sp, 4);
    memcpy(&img.data[4], &pc, 4);
    return img;
}

} // namespace sim
