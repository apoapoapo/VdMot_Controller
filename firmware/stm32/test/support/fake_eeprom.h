/* In-memory 24LC64 with page-write rules, busy time and failure injection. */
#pragma once

#include <string.h>
#include <unity.h>

#include "core/ports.h"

namespace sim
{

class FakeEeprom : public vdm::EepromPort
{
  public:
    uint8_t mem[8192];
    uint32_t page_writes = 0;
    int busy_polls = 0;         /* busy() answers true this many times per write */
    int32_t fail_after = -1;    /* make every write after this many page writes fail */
    int32_t power_loss_at = -1; /* write only the first half of this page write */
    uint32_t violations = 0;

    FakeEeprom()
    {
        memset(mem, 0xFF, sizeof(mem));
    }

    size_t size() const override
    {
        return sizeof(mem);
    }

    bool read(uint16_t addr, uint8_t *buf, size_t len) override
    {
        if (addr + len > sizeof(mem)) {
            return false;
        }
        memcpy(buf, &mem[addr], len);
        return true;
    }

    bool writePage(uint16_t addr, const uint8_t *buf, size_t len) override
    {
        if (busy_left_ > 0 || len == 0 || len > kPageSize ||
            (addr / kPageSize) != ((addr + len - 1) / kPageSize)) {
            violations++;
            return false;
        }
        if (fail_after >= 0 && static_cast<int32_t>(page_writes) >= fail_after) {
            return false;
        }
        if (power_loss_at >= 0 && static_cast<int32_t>(page_writes) == power_loss_at) {
            memcpy(&mem[addr], buf, len / 2);
            frozen_ = true; /* nothing gets written any more */
            page_writes++;
            return true;
        }
        if (!frozen_) {
            memcpy(&mem[addr], buf, len);
        }
        page_writes++;
        busy_left_ = busy_polls;
        return true;
    }

    bool busy() override
    {
        if (busy_left_ > 0) {
            busy_left_--;
            return true;
        }
        return false;
    }

    void powerCycle()
    {
        frozen_ = false;
        busy_left_ = 0;
        power_loss_at = -1;
    }

  private:
    int busy_left_ = 0;
    bool frozen_ = false;
};

} // namespace sim
