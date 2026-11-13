/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/link_client.h"

#include <stdio.h>
#include <string.h>

#include "vdm/codec.h"

namespace vdm
{

LinkClient::LinkClient(ByteSink &out) : out_(out)
{
    vdm_rx_init(&rx_);
}

/* ---------------------------------------------------------------------- */
/* Commands                                                                */
/* ---------------------------------------------------------------------- */

bool LinkClient::enqueue(uint8_t msg, const uint8_t *payload, size_t len)
{
    if (q_count_ == kQueueSize || len > sizeof(Request::payload)) {
        return false;
    }
    Request &r = queue_[(q_head_ + q_count_) % kQueueSize];
    r.msg = msg;
    r.len = static_cast<uint8_t>(len);
    if (len > 0) {
        memcpy(r.payload, payload, len);
    }
    q_count_++;
    return true;
}

bool LinkClient::enqueueMask(uint8_t msg, uint16_t mask)
{
    uint8_t b[2];
    size_t n = 0;
    return vdm_enc_mask(mask, b, sizeof(b), &n) && enqueue(msg, b, n);
}

bool LinkClient::setTarget(uint8_t channel, uint16_t target)
{
    if (channel >= VDM_MAX_CHANNELS || target > VDM_POS_MAX) {
        return false;
    }
    uint16_t bit = static_cast<uint16_t>(1u << channel);
    desired_[channel] = target;
    desired_valid_ |= bit;
    desired_dirty_ |= bit;
    return true;
}

bool LinkClient::calibrate(uint16_t mask)
{
    return enqueueMask(VDM_MSG_CALIBRATE, mask);
}

bool LinkClient::detect(uint16_t mask)
{
    return enqueueMask(VDM_MSG_DETECT, mask);
}

bool LinkClient::stop(uint16_t mask)
{
    /* the STM32 sets the target to the reached position: forget ours */
    desired_valid_ &= static_cast<uint16_t>(~mask);
    desired_dirty_ &= static_cast<uint16_t>(~mask);
    return enqueueMask(VDM_MSG_STOP, mask);
}

bool LinkClient::setChannels(uint16_t mask)
{
    return enqueueMask(VDM_MSG_SET_CHANNELS, mask);
}

bool LinkClient::sensorScan()
{
    return enqueue(VDM_MSG_SENSOR_SCAN, nullptr, 0);
}

bool LinkClient::setSystemConfig(const vdm_system_config_t &c)
{
    uint8_t b[VDM_SYSTEM_CONFIG_LEN];
    size_t n = 0;
    return vdm_enc_system_config(&c, b, sizeof(b), &n) && enqueue(VDM_MSG_SET_SYSTEM_CONFIG, b, n);
}

bool LinkClient::setMotorParams(const vdm_motor_params_t &m)
{
    uint8_t b[VDM_MOTOR_PARAMS_LEN];
    size_t n = 0;
    return vdm_enc_motor_params(&m, b, sizeof(b), &n) && enqueue(VDM_MSG_SET_MOTOR_PARAMS, b, n);
}

bool LinkClient::resetStm32()
{
    uint8_t b[VDM_RESET_LEN];
    size_t n = 0;
    return vdm_enc_reset(b, sizeof(b), &n) && enqueue(VDM_MSG_RESET, b, n);
}

bool LinkClient::takeHardResetRequest()
{
    bool r = hard_reset_request_;
    hard_reset_request_ = false;
    return r;
}

void LinkClient::suspend(uint32_t now_ms)
{
    pending_ = false;
    setState(LinkState::Updating, now_ms);
}

void LinkClient::resume(uint32_t now_ms)
{
    vdm_rx_init(&rx_);
    last_contact_ms_ = now_ms;
    setState(LinkState::Disconnected, now_ms);
}

/* ---------------------------------------------------------------------- */
/* Sending                                                                 */
/* ---------------------------------------------------------------------- */

void LinkClient::send(const Request &r, uint32_t now_ms)
{
    uint8_t wire[VDM_WIRE_MAX];
    size_t n = 0;
    if (!vdm_frame_encode(VDM_KIND_REQUEST, cur_seq_, r.msg, r.payload, r.len, wire, sizeof(wire),
                          &n)) {
        pending_ = false;
        return;
    }
    out_.write(wire, n);
    sent_ms_ = now_ms;
}

bool LinkClient::pollDue(uint32_t now_ms, Request &r)
{
    r.len = 0;
    if (fetch_config_) {
        fetch_config_ = false;
        r.msg = VDM_MSG_GET_SYSTEM_CONFIG;
        return true;
    }
    if (!model_.motor_valid) {
        r.msg = VDM_MSG_GET_MOTOR_PARAMS;
        return true;
    }
    if (desired_dirty_ != 0) {
        vdm_set_targets_t t{};
        for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
            if ((desired_dirty_ >> ch) & 1u) {
                t.targets[t.count++] = {ch, desired_[ch]};
            }
        }
        size_t n = 0;
        if (vdm_enc_set_targets(&t, r.payload, sizeof(r.payload), &n)) {
            r.msg = VDM_MSG_SET_TARGETS;
            r.len = static_cast<uint8_t>(n);
            targets_in_flight_ = desired_dirty_;
            desired_dirty_ = 0;
            return true;
        }
        desired_dirty_ = 0;
    }
    if (static_cast<int32_t>(now_ms - next_states_ms_) >= 0) {
        next_states_ms_ = now_ms + kPollStatesMs;
        r.msg = VDM_MSG_GET_VALVE_STATES;
        return true;
    }
    if (sensor_fetch_ >= 0) {
        r.msg = VDM_MSG_GET_SENSORS;
        r.payload[0] = static_cast<uint8_t>(sensor_fetch_);
        r.len = 1;
        sensor_fetch_ = -1;
        return true;
    }
    if (static_cast<int32_t>(now_ms - next_diag_ms_) >= 0) {
        next_diag_ms_ = now_ms + kPollDiagMs;
        r.msg = VDM_MSG_GET_DIAG;
        return true;
    }
    if (static_cast<int32_t>(now_ms - next_detail_ms_) >= 0) {
        next_detail_ms_ = now_ms + kPollDetailMs;
        r.msg = VDM_MSG_GET_VALVE_DETAIL;
        r.payload[0] = detail_idx_;
        r.len = 1;
        detail_idx_ = static_cast<uint8_t>((detail_idx_ + 1u) % VDM_MAX_CHANNELS);
        return true;
    }
    return false;
}

void LinkClient::startNext(uint32_t now_ms)
{
    Request r{};
    if (model_.state == LinkState::Disconnected || model_.state == LinkState::Incompatible) {
        if (hello_sent_ && now_ms - last_hello_ms_ < kHelloMs) {
            return;
        }
        hello_sent_ = true;
        last_hello_ms_ = now_ms;
        vdm_hello_req_t h{VDM_PROTO_VERSION, VDM_HELLO_FLAG_RESET_SEQ};
        size_t n = 0;
        vdm_enc_hello_req(&h, r.payload, sizeof(r.payload), &n);
        r.msg = VDM_MSG_HELLO;
        r.len = static_cast<uint8_t>(n);
    } else if (q_count_ > 0) {
        r = queue_[q_head_];
        q_head_ = static_cast<uint8_t>((q_head_ + 1u) % kQueueSize);
        q_count_--;
    } else if (!pollDue(now_ms, r)) {
        return;
    }

    cur_ = r;
    cur_seq_ = next_seq_;
    next_seq_ = static_cast<uint8_t>(next_seq_ + 1u);
    if (next_seq_ == 0) {
        next_seq_ = 1;
    }
    retries_ = 0;
    pending_ = true;
    model_.stats.requests++;
    send(cur_, now_ms);
}

void LinkClient::tick(uint32_t now_ms)
{
    if (model_.state == LinkState::Updating) {
        return;
    }

    if (pending_ && now_ms - sent_ms_ >= kTimeoutMs) {
        if (cur_.msg == VDM_MSG_HELLO) {
            pending_ = false; /* HELLO is repeated by the handshake anyway */
        } else if (retries_ < kMaxRetries) {
            retries_++;
            model_.stats.retries++;
            send(cur_, now_ms);
        } else {
            pending_ = false;
            model_.stats.timeouts++;
            if (cur_.msg == VDM_MSG_SET_TARGETS) {
                desired_dirty_ |= targets_in_flight_;
            }
            linkDown(now_ms);
        }
    }

    if (model_.state != LinkState::Connected && now_ms - last_contact_ms_ >= kHardResetAfterMs &&
        (!hard_reset_done_ || now_ms - last_hard_reset_ms_ >= kHardResetGapMs)) {
        hard_reset_done_ = true;
        last_hard_reset_ms_ = now_ms;
        hard_reset_request_ = true;
        model_.stats.hard_resets++;
    }

    if (!pending_) {
        startNext(now_ms);
    }
}

/* ---------------------------------------------------------------------- */
/* Receiving                                                               */
/* ---------------------------------------------------------------------- */

void LinkClient::onByte(uint8_t byte, uint32_t now_ms)
{
    if (model_.state == LinkState::Updating) {
        return;
    }
    vdm_frame_t f;
    if (vdm_rx_feed(&rx_, byte, &f)) {
        last_contact_ms_ = now_ms;
        onFrame(f, now_ms);
    }
}

void LinkClient::onFrame(const vdm_frame_t &f, uint32_t now_ms)
{
    if (f.kind == VDM_KIND_RESPONSE) {
        if (pending_ && f.seq == cur_seq_ && f.msg == cur_.msg) {
            pending_ = false;
            onResponse(f, now_ms);
        }
    } else if (f.kind == VDM_KIND_EVENT) {
        onEvent(f, now_ms);
    }
}

void LinkClient::setState(LinkState s, uint32_t now_ms)
{
    if (model_.state != s) {
        model_.state = s;
        model_.state_since_ms = now_ms;
    }
}

void LinkClient::linkDown(uint32_t now_ms)
{
    if (model_.state == LinkState::Connected) {
        setState(LinkState::Disconnected, now_ms);
        hello_sent_ = false;
        static const char kMsg[] = "link: STM32 does not answer";
        model_.addLog(now_ms, VDM_LOG_WARN, kMsg, sizeof(kMsg) - 1);
    }
}

void LinkClient::onConnected(uint32_t now_ms)
{
    setState(LinkState::Connected, now_ms);
    model_.stats.link_ups++;
    hard_reset_done_ = false;
    have_evt_seq_ = false;
    fetch_config_ = true;
    model_.motor_valid = false;
    next_states_ms_ = now_ms;
    next_diag_ms_ = now_ms;
    next_detail_ms_ = now_ms;
    sensor_fetch_ = 0;
    desired_dirty_ |= desired_valid_; /* resend what the user asked for */
}

void LinkClient::setError(const char *what, uint8_t status, uint8_t detail)
{
    snprintf(last_error_, sizeof(last_error_), "%s: status %u, detail %u", what, status, detail);
}

void LinkClient::storeSensors(const vdm_sensors_t &s)
{
    model_.sensor_total = s.total > Model::kMaxSensors ? Model::kMaxSensors : s.total;
    for (uint8_t i = 0; i < s.count; i++) {
        uint16_t idx = static_cast<uint16_t>(s.offset + i);
        if (idx < Model::kMaxSensors) {
            model_.sensors[idx] = s.sensors[i];
        }
    }
}

void LinkClient::onResponse(const vdm_frame_t &f, uint32_t now_ms)
{
    uint8_t status = 0;
    uint8_t detail = 0;
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    if (vdm_dec_response(f.payload, f.len, &status, &detail, &body, &body_len) != VDM_OK) {
        return;
    }

    if (status == VDM_ERR_NOT_READY && f.msg != VDM_MSG_HELLO) {
        /* the STM32 restarted without us noticing the boot event */
        if (f.msg == VDM_MSG_SET_TARGETS) {
            desired_dirty_ |= targets_in_flight_;
        }
        setState(LinkState::Disconnected, now_ms);
        hello_sent_ = false;
        return;
    }

    switch (f.msg) {
    case VDM_MSG_HELLO: {
        vdm_hello_t h;
        if (status != VDM_OK || vdm_dec_hello(body, body_len, &h) != VDM_OK) {
            return;
        }
        model_.hello = h;
        model_.hello_valid = true;
        if (h.proto_version != VDM_PROTO_VERSION) {
            setState(LinkState::Incompatible, now_ms);
            return;
        }
        onConnected(now_ms);
        return;
    }
    case VDM_MSG_GET_VALVE_STATES: {
        vdm_valve_states_t s;
        if (status == VDM_OK && vdm_dec_valve_states(body, body_len, &s, nullptr) == VDM_OK) {
            for (uint8_t i = 0; i < s.count; i++) {
                model_.valves[s.valves[i].channel] = s.valves[i];
            }
            model_.valves_valid = true;
        }
        return;
    }
    case VDM_MSG_GET_VALVE_DETAIL: {
        vdm_valve_detail_t d;
        if (status == VDM_OK && vdm_dec_valve_detail(body, body_len, &d, nullptr) == VDM_OK) {
            model_.details[d.state.channel] = d;
            model_.detail_valid[d.state.channel] = true;
        }
        return;
    }
    case VDM_MSG_GET_SENSORS: {
        vdm_sensors_t s;
        if (status == VDM_OK && vdm_dec_sensors(body, body_len, &s, nullptr) == VDM_OK) {
            storeSensors(s);
            model_.sensors_ms = now_ms;
            uint16_t next = static_cast<uint16_t>(s.offset + s.count);
            if (s.count > 0 && next < s.total) {
                sensor_fetch_ = static_cast<int16_t>(next);
            }
        }
        return;
    }
    case VDM_MSG_GET_SYSTEM_CONFIG:
        if (status == VDM_OK &&
            vdm_dec_system_config(body, body_len, &model_.sys, nullptr) == VDM_OK) {
            model_.sys_valid = true;
        }
        return;
    case VDM_MSG_GET_MOTOR_PARAMS:
        if (status == VDM_OK &&
            vdm_dec_motor_params(body, body_len, &model_.motor, nullptr) == VDM_OK) {
            model_.motor_valid = true;
        }
        return;
    case VDM_MSG_GET_DIAG:
        if (status == VDM_OK && vdm_dec_diag(body, body_len, &model_.diag) == VDM_OK) {
            model_.diag_valid = true;
        }
        return;
    case VDM_MSG_SET_TARGETS:
        if (status == VDM_ERR_CHANNEL || status == VDM_ERR_RANGE) {
            /* drop the offending target, resend the others */
            uint16_t sent = targets_in_flight_;
            uint8_t idx = 0;
            for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
                if ((sent >> ch) & 1u) {
                    if (idx == detail) {
                        desired_valid_ &= static_cast<uint16_t>(~(1u << ch));
                        sent &= static_cast<uint16_t>(~(1u << ch));
                    }
                    idx++;
                }
            }
            desired_dirty_ |= sent;
            setError("set target", status, detail);
        } else if (status != VDM_OK) {
            setError("set target", status, detail);
        }
        targets_in_flight_ = 0;
        return;
    case VDM_MSG_SET_SYSTEM_CONFIG:
    case VDM_MSG_SET_MOTOR_PARAMS:
        if (status == VDM_OK) {
            fetch_config_ = true;
            model_.motor_valid = false;
        } else {
            setError(f.msg == VDM_MSG_SET_SYSTEM_CONFIG ? "system config" : "motor params", status,
                     detail);
        }
        return;
    case VDM_MSG_SET_CHANNELS:
    case VDM_MSG_CALIBRATE:
    case VDM_MSG_DETECT:
    case VDM_MSG_STOP:
    case VDM_MSG_SENSOR_SCAN:
    case VDM_MSG_RESET:
        if (status != VDM_OK) {
            setError("command", status, detail);
        } else if (f.msg != VDM_MSG_RESET) {
            next_states_ms_ = now_ms; /* show the effect soon */
        }
        return;
    default:
        return;
    }
}

void LinkClient::onEvent(const vdm_frame_t &f, uint32_t now_ms)
{
    model_.stats.events++;
    if (f.msg == VDM_EVT_BOOT) {
        vdm_hello_t h;
        if (vdm_dec_hello(f.payload, f.len, &h) == VDM_OK) {
            model_.hello = h;
            model_.hello_valid = true;
        }
        if (model_.state == LinkState::Connected) {
            model_.stats.stm_reboots++;
            static const char kMsg[] = "link: STM32 restarted";
            model_.addLog(now_ms, VDM_LOG_WARN, kMsg, sizeof(kMsg) - 1);
        }
        if (model_.state != LinkState::Incompatible) {
            setState(LinkState::Disconnected, now_ms);
            hello_sent_ = false; /* answer with HELLO right away */
        }
        have_evt_seq_ = false;
        return;
    }
    if (model_.state != LinkState::Connected) {
        return;
    }

    if (have_evt_seq_) {
        uint8_t expected = static_cast<uint8_t>(last_evt_seq_ + 1u);
        if (expected == 0) {
            expected = 1;
        }
        if (f.seq != expected) {
            model_.stats.evt_lost++;
            next_states_ms_ = now_ms;
        }
    }
    have_evt_seq_ = true;
    last_evt_seq_ = f.seq;

    switch (f.msg) {
    case VDM_EVT_VALVE: {
        vdm_valve_state_t s;
        if (vdm_dec_valve_state(f.payload, f.len, &s, nullptr) == VDM_OK) {
            model_.valves[s.channel] = s;
        }
        return;
    }
    case VDM_EVT_CALIBRATION: {
        vdm_evt_calibration_t c;
        if (vdm_dec_evt_calibration(f.payload, f.len, &c, nullptr) == VDM_OK) {
            model_.calibration[c.channel] = c;
            model_.calibration_valid[c.channel] = true;
        }
        return;
    }
    case VDM_EVT_SENSORS: {
        vdm_sensors_t s;
        if (vdm_dec_sensors(f.payload, f.len, &s, nullptr) == VDM_OK) {
            storeSensors(s);
            model_.sensors_ms = now_ms;
        }
        return;
    }
    case VDM_EVT_SENSOR_SCAN_DONE:
        if (vdm_dec_evt_scan_done(f.payload, f.len, &model_.scan) == VDM_OK) {
            model_.scan_valid = true;
            sensor_fetch_ = 0;
        }
        return;
    case VDM_EVT_LINK_TIMEOUT: {
        static const char kMsg[] = "STM32 was in failsafe (link timeout)";
        model_.addLog(now_ms, VDM_LOG_WARN, kMsg, sizeof(kMsg) - 1);
        return;
    }
    case VDM_EVT_LOG: {
        vdm_evt_log_t l;
        if (vdm_dec_evt_log(f.payload, f.len, &l) == VDM_OK) {
            model_.addLog(now_ms, l.level, l.text, l.len);
        }
        return;
    }
    default:
        return;
    }
}

} // namespace vdm
