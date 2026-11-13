/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/controller.h"

#include <string.h>

#include "vdm/codec.h"

namespace vdm
{

Controller::Controller(const BoardInfo &info, ValveManager &valves, SensorManager &sensors,
                       Storage &storage, SystemPort &system, LinkPort &link_port)
    : info_(info), valves_(valves), sensors_(sensors), storage_(storage), system_(system),
      link_(link_port, *this), sys_(vdm_system_config_default), motor_(vdm_motor_params_default)
{
}

/* ---------------------------------------------------------------------- */
/* Startup                                                                 */
/* ---------------------------------------------------------------------- */

void Controller::begin(uint32_t now_ms)
{
    now_ms_ = now_ms;
    last_tick_ms_ = now_ms;
    last_request_ms_ = now_ms;

    uint16_t enabled = VDM_CHANNEL_MASK_ALL;
    SystemData sd;
    if (storage_.loadSystem(sd)) {
        sys_ = sd.sys;
        motor_ = sd.motor;
        enabled = sd.enabled_mask;
    }

    if (!storage_.loadCounters(counters_)) {
        counters_ = Counters{};
    }
    counters_.boot_count++;
    storage_.saveCounters(counters_);

    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        ChannelPersist p;
        if (storage_.loadChannel(ch, p)) {
            valves_.restore(ch, p);
        }
    }

    uint16_t stored[VDM_MAX_CHANNELS];
    bool have_targets = storage_.loadTargets(stored);
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        uint16_t t;
        if (sys_.startup_pos != VDM_POS_HOLD) {
            t = sys_.startup_pos;
        } else if (have_targets) {
            t = stored[ch];
        } else {
            /* first start without any stored target: open, like the failsafe */
            t = sys_.failsafe_pos != VDM_POS_HOLD ? sys_.failsafe_pos : VDM_POS_MAX;
        }
        valves_.setTarget(ch, t);
        /* the valves were most probably left at their stored target */
        valves_.setPositionHint(ch, have_targets ? stored[ch] : t);
        saved_targets_[ch] = have_targets ? stored[ch] : t;
    }

    valves_.setOperatingHours(counters_.op_hours);
    applyConfig();
    valves_.setEnabledMask(enabled);
}

void Controller::applyConfig()
{
    valves_.setMotorParams(motor_);
    valves_.setRecalibration(sys_.recal_after_moves, sys_.recal_after_days);
    sensors_.setInterval(sys_.sensor_interval_s);
}

bool Controller::encodeSensors(uint8_t offset, uint8_t *buf, size_t cap, size_t *len)
{
    vdm_sensors_t s{};
    s.total = sensors_.entryCount();
    s.offset = offset;
    while (s.count < VDM_SENSORS_PER_MSG &&
           sensors_.entry(static_cast<uint8_t>(offset + s.count), now_ms_, s.sensors[s.count])) {
        s.count++;
    }
    return vdm_enc_sensors(&s, buf, cap, len);
}

void Controller::saveSystem()
{
    SystemData d{sys_, motor_, valves_.enabledMask()};
    storage_.saveSystem(d);
}

/* ---------------------------------------------------------------------- */
/* Main loop                                                               */
/* ---------------------------------------------------------------------- */

void Controller::onLinkByte(uint8_t byte, uint32_t now_ms)
{
    now_ms_ = now_ms;
    if (link_.onByte(byte)) {
        last_request_ms_ = now_ms;
        /* normally ended in handleRequest(); a retransmitted request is
         * answered from the cache without calling it */
        if (failsafe_active_) {
            endFailsafe();
        }
    }
}

void Controller::endFailsafe()
{
    /* back to the targets from before the failsafe: the ESP32 may have
     * restarted meanwhile and no longer know them */
    failsafe_active_ = false;
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        valves_.setTarget(ch, pre_failsafe_targets_[ch], false);
    }
    link_timeout_evt_ = true;
}

void Controller::tick(uint32_t now_ms)
{
    now_ms_ = now_ms;

    if (reset_pending_) {
        valves_.stop(VDM_CHANNEL_MASK_ALL);
        storage_.flush();
        system_.reset();
        reset_pending_ = false;
        return;
    }

    /* operating hours */
    hour_acc_ms_ += now_ms - last_tick_ms_;
    last_tick_ms_ = now_ms;
    if (hour_acc_ms_ >= kHourMs) {
        hour_acc_ms_ -= kHourMs;
        counters_.op_hours++;
        valves_.setOperatingHours(counters_.op_hours);
        storage_.saveCounters(counters_);
    }

    /* failsafe */
    if (!failsafe_active_ && sys_.link_timeout_s != 0 &&
        now_ms - last_request_ms_ >= sys_.link_timeout_s * 1000UL) {
        failsafe_active_ = true;
        for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
            pre_failsafe_targets_[ch] = valves_.target(ch);
            uint16_t t = sys_.failsafe_pos != VDM_POS_HOLD ? sys_.failsafe_pos : valves_.target(ch);
            valves_.setTarget(ch, t, true);
        }
    }

    valves_.tick(now_ms);
    sensors_.tick(now_ms);
    storage_.tick();
    sendEvents(now_ms);
    persist(now_ms);
}

void Controller::sendEvents(uint32_t now_ms)
{
    uint8_t buf[VDM_PAYLOAD_MAX];
    size_t n = 0;

    if (!hello_done_) {
        if (!boot_evt_sent_ || now_ms - last_boot_evt_ms_ >= kBootEventMs) {
            boot_evt_sent_ = true;
            last_boot_evt_ms_ = now_ms;
            vdm_hello_t h = hello();
            if (vdm_enc_hello(&h, buf, sizeof(buf), &n)) {
                link_.sendEvent(VDM_EVT_BOOT, buf, n);
            }
        }
        /* keep collecting changes, they are sent after the handshake */
        evt_pending_ |= valves_.takeChanged();
        vdm_evt_scan_done_t ignored;
        sensors_.takeScanDone(ignored);
        sensors_.takeCycleDone();
        return;
    }

    if (link_timeout_evt_) {
        link_timeout_evt_ = false;
        if (vdm_enc_u16(sys_.failsafe_pos, buf, sizeof(buf), &n)) {
            link_.sendEvent(VDM_EVT_LINK_TIMEOUT, buf, n);
        }
    }

    vdm_evt_scan_done_t scan;
    if (sensors_.takeScanDone(scan) && vdm_enc_evt_scan_done(&scan, buf, sizeof(buf), &n)) {
        link_.sendEvent(VDM_EVT_SENSOR_SCAN_DONE, buf, n);
    }
    if (sensors_.takeCycleDone()) {
        uint8_t total = sensors_.entryCount();
        for (uint16_t off = 0; off < total;
             off = static_cast<uint16_t>(off + VDM_SENSORS_PER_MSG)) {
            if (encodeSensors(static_cast<uint8_t>(off), buf, sizeof(buf), &n)) {
                link_.sendEvent(VDM_EVT_SENSORS, buf, n);
            }
        }
    }

    vdm_evt_calibration_t cal;
    while (valves_.popCalibrationEvent(cal)) {
        if (vdm_enc_evt_calibration(&cal, buf, sizeof(buf), &n)) {
            link_.sendEvent(VDM_EVT_CALIBRATION, buf, n);
        }
    }

    evt_pending_ |= valves_.takeChanged();
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS && evt_pending_ != 0; ch++) {
        uint16_t bit = static_cast<uint16_t>(1u << ch);
        if (!(evt_pending_ & bit) || now_ms - last_evt_ms_[ch] < kValveEventMinMs) {
            continue;
        }
        evt_pending_ &= static_cast<uint16_t>(~bit);
        last_evt_ms_[ch] = now_ms;
        vdm_valve_state_t s = valves_.state(ch);
        if (vdm_enc_valve_state(&s, buf, sizeof(buf), &n)) {
            link_.sendEvent(VDM_EVT_VALVE, buf, n);
        }
    }
}

void Controller::persist(uint32_t now_ms)
{
    /* targets: lazily, failsafe targets are never stored */
    if (!failsafe_active_) {
        bool differ = false;
        for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
            if (valves_.target(ch) != saved_targets_[ch]) {
                differ = true;
                break;
            }
        }
        if (differ && !targets_dirty_) {
            targets_dirty_ = true;
            targets_dirty_since_ = now_ms;
        }
        if (targets_dirty_ && now_ms - targets_dirty_since_ >= kLazyWriteMs) {
            targets_dirty_ = false;
            for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
                saved_targets_[ch] = valves_.target(ch);
            }
            storage_.saveTargets(saved_targets_);
        }
    }

    /* channel records: calibrations at once, statistics lazily */
    uint16_t urgent = 0;
    uint16_t dirty = valves_.takePersistDirty(&urgent);
    if (dirty != 0 && channels_dirty_ == 0) {
        channels_dirty_since_ = now_ms;
    }
    channels_dirty_ |= dirty;
    bool lazy_due = channels_dirty_ != 0 && now_ms - channels_dirty_since_ >= kLazyWriteMs;
    uint16_t write = lazy_due ? channels_dirty_ : urgent;
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        if ((write >> ch) & 1u) {
            storage_.saveChannel(ch, valves_.persist(ch));
        }
    }
    channels_dirty_ &= static_cast<uint16_t>(~write);
}

/* ---------------------------------------------------------------------- */
/* Requests                                                                */
/* ---------------------------------------------------------------------- */

vdm_hello_t Controller::hello() const
{
    vdm_hello_t h{};
    h.proto_version = VDM_PROTO_VERSION;
    h.fw_major = info_.fw_major;
    h.fw_minor = info_.fw_minor;
    h.fw_patch = info_.fw_patch;
    h.fw_build = info_.fw_build;
    h.hw_rev = info_.hw_rev;
    h.mcu = info_.mcu;
    memcpy(h.mcu_uid, info_.mcu_uid, sizeof(h.mcu_uid));
    h.channel_count = VDM_MAX_CHANNELS;
    h.capabilities = info_.capabilities;
    h.reset_cause = info_.reset_cause;
    h.boot_count = counters_.boot_count;
    h.config_crc = vdm_config_crc(&sys_, &motor_);
    return h;
}

size_t Controller::ok(uint8_t *resp, size_t cap, const uint8_t *body, size_t body_len)
{
    size_t n = 0;
    return vdm_enc_response(VDM_OK, 0, body, body_len, resp, cap, &n) ? n : 0;
}

size_t Controller::error(uint8_t *resp, size_t cap, uint8_t status, uint8_t detail)
{
    size_t n = 0;
    return vdm_enc_response(status, detail, nullptr, 0, resp, cap, &n) ? n : 0;
}

size_t Controller::okConfigCrc(uint8_t *resp, size_t cap)
{
    uint8_t body[2];
    size_t n = 0;
    vdm_enc_u16(vdm_config_crc(&sys_, &motor_), body, sizeof(body), &n);
    return ok(resp, cap, body, n);
}

size_t Controller::handleRequest(uint8_t msg, const uint8_t *p, size_t len, uint8_t *resp,
                                 size_t cap)
{
    uint8_t body[VDM_PAYLOAD_MAX];
    size_t n = 0;
    uint8_t bad = 0;
    vdm_status_t st;

    /* the link is back: end the failsafe before the request is executed,
     * so that a new target in this very request wins */
    if (failsafe_active_) {
        endFailsafe();
    }

    if (msg != VDM_MSG_HELLO && !hello_done_) {
        return error(resp, cap, VDM_ERR_NOT_READY);
    }

    switch (msg) {
    case VDM_MSG_HELLO: {
        vdm_hello_req_t req;
        if (vdm_dec_hello_req(p, len, &req) != VDM_OK) {
            return error(resp, cap, VDM_ERR_BAD_LEN);
        }
        if (req.flags & VDM_HELLO_FLAG_RESET_SEQ) {
            link_.resetSequence();
        }
        hello_done_ = true;
        /* report every channel once after the handshake */
        evt_pending_ |= VDM_CHANNEL_MASK_ALL;
        vdm_hello_t h = hello();
        vdm_enc_hello(&h, body, sizeof(body), &n);
        return ok(resp, cap, body, n);
    }

    case VDM_MSG_PING:
        if (len > VDM_PING_MAX) {
            return error(resp, cap, VDM_ERR_BAD_LEN);
        }
        return ok(resp, cap, p, len);

    case VDM_MSG_RESET:
        st = vdm_dec_reset(p, len);
        if (st != VDM_OK) {
            return error(resp, cap, st);
        }
        reset_pending_ = true;
        return ok(resp, cap);

    case VDM_MSG_SET_SYSTEM_CONFIG: {
        vdm_system_config_t c;
        st = vdm_dec_system_config(p, len, &c, &bad);
        if (st != VDM_OK) {
            return error(resp, cap, st, bad);
        }
        sys_ = c;
        applyConfig();
        saveSystem();
        return okConfigCrc(resp, cap);
    }

    case VDM_MSG_GET_SYSTEM_CONFIG:
        if (len != 0) {
            return error(resp, cap, VDM_ERR_BAD_LEN);
        }
        vdm_enc_system_config(&sys_, body, sizeof(body), &n);
        return ok(resp, cap, body, n);

    case VDM_MSG_GET_VALVE_STATES: {
        if (len != 0) {
            return error(resp, cap, VDM_ERR_BAD_LEN);
        }
        vdm_valve_states_t s{};
        s.count = VDM_MAX_CHANNELS;
        for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
            s.valves[ch] = valves_.state(ch);
        }
        vdm_enc_valve_states(&s, body, sizeof(body), &n);
        return ok(resp, cap, body, n);
    }

    case VDM_MSG_SET_TARGETS: {
        vdm_set_targets_t t;
        st = vdm_dec_set_targets(p, len, &t, &bad);
        if (st != VDM_OK) {
            return error(resp, cap, st, bad);
        }
        for (uint8_t i = 0; i < t.count; i++) {
            if (!((valves_.enabledMask() >> t.targets[i].channel) & 1u)) {
                return error(resp, cap, VDM_ERR_CHANNEL, i);
            }
        }
        for (uint8_t i = 0; i < t.count; i++) {
            valves_.setTarget(t.targets[i].channel, t.targets[i].target);
        }
        return ok(resp, cap);
    }

    case VDM_MSG_CALIBRATE:
    case VDM_MSG_DETECT:
    case VDM_MSG_STOP: {
        uint16_t mask;
        st = vdm_dec_mask(p, len, &mask);
        if (st != VDM_OK) {
            return error(resp, cap, st);
        }
        if (msg != VDM_MSG_STOP) {
            uint16_t off = static_cast<uint16_t>(mask & ~valves_.enabledMask());
            if (off != 0) {
                uint8_t ch = 0;
                while (!((off >> ch) & 1u)) {
                    ch++;
                }
                return error(resp, cap, VDM_ERR_CHANNEL, ch);
            }
        }
        if (msg == VDM_MSG_CALIBRATE) {
            valves_.calibrate(mask);
        } else if (msg == VDM_MSG_DETECT) {
            valves_.detect(mask);
        } else {
            valves_.stop(mask);
        }
        return ok(resp, cap);
    }

    case VDM_MSG_SET_CHANNELS: {
        uint16_t mask;
        st = vdm_dec_mask(p, len, &mask);
        if (st != VDM_OK) {
            return error(resp, cap, st);
        }
        valves_.setEnabledMask(mask);
        saveSystem();
        return ok(resp, cap);
    }

    case VDM_MSG_GET_VALVE_DETAIL: {
        uint8_t ch;
        st = vdm_dec_channel(p, len, &ch);
        if (st != VDM_OK) {
            return error(resp, cap, st);
        }
        vdm_valve_detail_t d = valves_.detail(ch);
        vdm_enc_valve_detail(&d, body, sizeof(body), &n);
        return ok(resp, cap, body, n);
    }

    case VDM_MSG_SET_MOTOR_PARAMS: {
        vdm_motor_params_t m;
        st = vdm_dec_motor_params(p, len, &m, &bad);
        if (st != VDM_OK) {
            return error(resp, cap, st, bad);
        }
        motor_ = m;
        applyConfig();
        saveSystem();
        return okConfigCrc(resp, cap);
    }

    case VDM_MSG_GET_MOTOR_PARAMS:
        if (len != 0) {
            return error(resp, cap, VDM_ERR_BAD_LEN);
        }
        vdm_enc_motor_params(&motor_, body, sizeof(body), &n);
        return ok(resp, cap, body, n);

    case VDM_MSG_SENSOR_SCAN:
        if (len != 0) {
            return error(resp, cap, VDM_ERR_BAD_LEN);
        }
        if (!sensors_.startScan()) {
            return error(resp, cap, VDM_ERR_BUSY);
        }
        return ok(resp, cap);

    case VDM_MSG_GET_SENSORS: {
        uint8_t offset;
        st = vdm_dec_offset(p, len, &offset);
        if (st != VDM_OK) {
            return error(resp, cap, st);
        }
        if (!encodeSensors(offset, body, sizeof(body), &n)) {
            return error(resp, cap, VDM_ERR_INTERNAL);
        }
        return ok(resp, cap, body, n);
    }

    case VDM_MSG_GET_DIAG: {
        if (len != 0) {
            return error(resp, cap, VDM_ERR_BAD_LEN);
        }
        const vdm_rx_stats_t &rx = link_.rxStats();
        PlatformDiag pd = system_.diag();
        vdm_diag_t d{};
        d.uptime_s = now_ms_ / 1000u;
        d.rx_frames_ok = rx.frames_ok;
        d.rx_crc_err = rx.crc_err;
        d.rx_malformed = rx.malformed;
        d.rx_overflow = rx.overflow;
        d.rx_bad_version = rx.bad_version;
        d.rx_duplicate = link_.duplicates();
        d.uart_hw_err = pd.uart_hw_err;
        d.ow_crc_err = sensors_.crcErrors();
        d.ow_bus_err = sensors_.busErrors();
        d.eeprom_writes = storage_.writes();
        d.eeprom_err = storage_.errors();
        d.loop_max_us = pd.loop_max_us;
        d.mcu_temp_c10 = pd.mcu_temp_c10;
        vdm_enc_diag(&d, body, sizeof(body), &n);
        return ok(resp, cap, body, n);
    }

    case VDM_MSG_CLEAR_DIAG:
        if (len != 0) {
            return error(resp, cap, VDM_ERR_BAD_LEN);
        }
        link_.clearStats();
        sensors_.clearStats();
        system_.clearDiag();
        return ok(resp, cap);

    default:
        return error(resp, cap, VDM_ERR_UNKNOWN_MSG);
    }
}

} // namespace vdm
