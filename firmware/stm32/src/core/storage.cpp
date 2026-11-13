/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/storage.h"

#include <string.h>

#include "vdm/codec.h"
#include "vdm/crc16.h"

namespace vdm
{

namespace
{

constexpr uint8_t kChannelLen = 25;
constexpr uint8_t kSystemLen = VDM_SYSTEM_CONFIG_LEN + VDM_MOTOR_PARAMS_LEN + 2;
constexpr uint8_t kCountersLen = 8;
constexpr uint8_t kTargetsLen = 2 * VDM_MAX_CHANNELS;

/* EEPROM layout (24LC64, 8 KiB). 0x0000-0x00FF is left to the legacy
 * firmware's data, which is ignored. */
constexpr uint16_t kBase = 0x0100;

void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t *p, uint32_t v)
{
    put16(p, static_cast<uint16_t>(v));
    put16(p + 2, static_cast<uint16_t>(v >> 16));
}

uint16_t get16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t get32(const uint8_t *p)
{
    return get16(p) | (static_cast<uint32_t>(get16(p + 2)) << 16);
}

bool newer(uint8_t a, uint8_t b)
{
    return static_cast<int8_t>(a - b) > 0;
}

} // namespace

Storage::Slot Storage::slot(uint8_t rec)
{
    switch (rec) {
    case kRecSystem:
        return {kBase + 0x00, kBase + 0x40, 1, kSystemLen};
    case kRecCounters:
        return {kBase + 0x80, kBase + 0xA0, 2, kCountersLen};
    case kRecTargets:
        return {kBase + 0xC0, kBase + 0xE0, 3, kTargetsLen};
    default: {
        uint16_t a = static_cast<uint16_t>(kBase + 0x100 + (rec - kRecChannel0) * 0x40);
        return {a, static_cast<uint16_t>(a + 0x20), static_cast<uint8_t>(0x10 + rec - kRecChannel0),
                kChannelLen};
    }
    }
}

/* ---------------------------------------------------------------------- */
/* Loading                                                                 */
/* ---------------------------------------------------------------------- */

bool Storage::load(uint8_t rec, uint8_t *payload)
{
    Slot s = slot(rec);
    uint8_t buf[2][kMaxRecord];
    bool valid[2] = {false, false};
    const uint16_t addr[2] = {s.addr_a, s.addr_b};
    size_t total = kHeaderLen + s.len + 2u;

    for (int i = 0; i < 2; i++) {
        if (!eeprom_.read(addr[i], buf[i], total)) {
            errors_++;
            continue;
        }
        const uint8_t *b = buf[i];
        if (b[0] != kMagic || b[1] != s.type || b[3] != s.len) {
            continue;
        }
        uint16_t crc = vdm_crc16(b, kHeaderLen + s.len);
        valid[i] = crc == get16(&b[kHeaderLen + s.len]);
    }

    int pick = -1;
    if (valid[0] && valid[1]) {
        pick = newer(buf[1][2], buf[0][2]) ? 1 : 0;
    } else if (valid[0]) {
        pick = 0;
    } else if (valid[1]) {
        pick = 1;
    }
    if (pick < 0) {
        return false;
    }
    seq_[rec] = buf[pick][2];
    newest_is_b_[rec] = pick == 1;
    memcpy(data_[rec], &buf[pick][kHeaderLen], s.len);
    memcpy(payload, &buf[pick][kHeaderLen], s.len);
    return true;
}

bool Storage::loadSystem(SystemData &out)
{
    uint8_t p[kMaxPayload];
    if (!load(kRecSystem, p)) {
        return false;
    }
    SystemData d{};
    if (vdm_dec_system_config(p, VDM_SYSTEM_CONFIG_LEN, &d.sys, nullptr) != VDM_OK ||
        vdm_dec_motor_params(&p[VDM_SYSTEM_CONFIG_LEN], VDM_MOTOR_PARAMS_LEN, &d.motor, nullptr) !=
            VDM_OK) {
        return false;
    }
    d.enabled_mask = get16(&p[VDM_SYSTEM_CONFIG_LEN + VDM_MOTOR_PARAMS_LEN]) &
                     static_cast<uint16_t>(VDM_CHANNEL_MASK_ALL);
    out = d;
    return true;
}

bool Storage::loadCounters(Counters &out)
{
    uint8_t p[kMaxPayload];
    if (!load(kRecCounters, p)) {
        return false;
    }
    out.boot_count = get32(&p[0]);
    out.op_hours = get32(&p[4]);
    return true;
}

bool Storage::loadTargets(uint16_t (&targets)[kChannels])
{
    uint8_t p[kMaxPayload];
    if (!load(kRecTargets, p)) {
        return false;
    }
    for (uint8_t i = 0; i < kChannels; i++) {
        uint16_t t = get16(&p[2 * i]);
        targets[i] = t > VDM_POS_MAX ? VDM_POS_MAX : t;
    }
    return true;
}

bool Storage::loadChannel(uint8_t ch, ChannelPersist &out)
{
    uint8_t p[kMaxPayload];
    if (ch >= kChannels || !load(static_cast<uint8_t>(kRecChannel0 + ch), p)) {
        return false;
    }
    ChannelPersist c;
    c.open_counts = get32(&p[0]);
    c.close_counts = get32(&p[4]);
    c.mean_open_ma10 = get16(&p[8]);
    c.mean_close_ma10 = get16(&p[10]);
    c.calib_op_hours = get32(&p[12]);
    c.moves_since_calib = get32(&p[16]);
    c.moves_total = get32(&p[20]);
    c.calibrated = p[24] ? 1 : 0;
    if (c.calibrated && (c.open_counts == 0 || c.close_counts == 0)) {
        c.calibrated = 0;
    }
    out = c;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Saving                                                                  */
/* ---------------------------------------------------------------------- */

void Storage::queue(uint8_t rec, const uint8_t *payload)
{
    memcpy(data_[rec], payload, slot(rec).len);
    pending_ |= 1UL << rec;
}

void Storage::saveSystem(const SystemData &d)
{
    uint8_t p[kMaxPayload] = {};
    size_t n = 0;
    if (!vdm_enc_system_config(&d.sys, p, VDM_SYSTEM_CONFIG_LEN, &n) ||
        !vdm_enc_motor_params(&d.motor, &p[VDM_SYSTEM_CONFIG_LEN], VDM_MOTOR_PARAMS_LEN, &n)) {
        return; /* invalid configs are never stored */
    }
    put16(&p[VDM_SYSTEM_CONFIG_LEN + VDM_MOTOR_PARAMS_LEN], d.enabled_mask);
    queue(kRecSystem, p);
}

void Storage::saveCounters(const Counters &c)
{
    uint8_t p[kMaxPayload] = {};
    put32(&p[0], c.boot_count);
    put32(&p[4], c.op_hours);
    queue(kRecCounters, p);
}

void Storage::saveTargets(const uint16_t (&targets)[kChannels])
{
    uint8_t p[kMaxPayload] = {};
    for (uint8_t i = 0; i < kChannels; i++) {
        put16(&p[2 * i], targets[i]);
    }
    queue(kRecTargets, p);
}

void Storage::saveChannel(uint8_t ch, const ChannelPersist &c)
{
    if (ch >= kChannels) {
        return;
    }
    uint8_t p[kMaxPayload] = {};
    put32(&p[0], c.open_counts);
    put32(&p[4], c.close_counts);
    put16(&p[8], c.mean_open_ma10);
    put16(&p[10], c.mean_close_ma10);
    put32(&p[12], c.calib_op_hours);
    put32(&p[16], c.moves_since_calib);
    put32(&p[20], c.moves_total);
    p[24] = c.calibrated;
    queue(static_cast<uint8_t>(kRecChannel0 + ch), p);
}

void Storage::startNext()
{
    for (uint8_t rec = 0; rec < kRecCount; rec++) {
        if (!(pending_ & (1UL << rec))) {
            continue;
        }
        pending_ &= ~(1UL << rec);
        Slot s = slot(rec);
        uint8_t seq = static_cast<uint8_t>(seq_[rec] + 1u);
        buf_[0] = kMagic;
        buf_[1] = s.type;
        buf_[2] = seq;
        buf_[3] = s.len;
        memcpy(&buf_[kHeaderLen], data_[rec], s.len);
        uint16_t crc = vdm_crc16(buf_, kHeaderLen + s.len);
        put16(&buf_[kHeaderLen + s.len], crc);
        buf_len_ = static_cast<uint8_t>(kHeaderLen + s.len + 2u);
        buf_off_ = 0;
        buf_addr_ = newest_is_b_[rec] ? s.addr_a : s.addr_b;
        active_ = static_cast<int8_t>(rec);
        return;
    }
}

void Storage::tick()
{
    if (active_ < 0) {
        if (pending_ == 0) {
            return;
        }
        startNext();
    }
    if (eeprom_.busy()) {
        return;
    }
    if (buf_off_ < buf_len_) {
        uint16_t addr = static_cast<uint16_t>(buf_addr_ + buf_off_);
        size_t room = EepromPort::kPageSize - (addr % EepromPort::kPageSize);
        size_t n = buf_len_ - buf_off_;
        if (n > room) {
            n = room;
        }
        if (!eeprom_.writePage(addr, &buf_[buf_off_], n)) {
            /* give up on this copy; the other slot still holds valid data */
            errors_++;
            active_ = -1;
            return;
        }
        buf_off_ = static_cast<uint8_t>(buf_off_ + n);
        return;
    }
    /* last page written and the write cycle has finished */
    uint8_t rec = static_cast<uint8_t>(active_);
    seq_[rec] = buf_[2];
    newest_is_b_[rec] = buf_addr_ == slot(rec).addr_b;
    writes_++;
    active_ = -1;
}

void Storage::flush()
{
    /* bounded: every record needs at most a few page writes */
    for (uint32_t guard = 0; guard < 100000u && !idle(); guard++) {
        tick();
    }
}

} // namespace vdm
