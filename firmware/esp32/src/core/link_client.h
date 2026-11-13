/*
 * LinkClient - ESP32 side of the link to the STM32 (protocol v1).
 *
 * Single-threaded: onByte(), tick() and the command functions are called
 * from the link task only.
 *
 *  - handshake: HELLO every second until the STM32 answers
 *  - stop-and-wait: one outstanding request, 100 ms timeout, up to 3
 *    retries with the same sequence number, then the link counts as down
 *  - polling: valve states every 10 s, diagnostics every 30 s, one valve
 *    detail every 5 s; sensor pages after scans
 *  - events update the model directly; a gap in the event sequence
 *    triggers an immediate full status poll
 *  - targets set by the user are kept and resent after a reconnect
 *  - if the STM32 stays silent for 30 s a hard reset is requested
 *    (at most every 5 minutes)
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/model.h"
#include "vdm/frame.h"

namespace vdm
{

class ByteSink
{
  public:
    virtual ~ByteSink() = default;
    virtual void write(const uint8_t *data, size_t len) = 0;
};

class LinkClient
{
  public:
    static constexpr uint32_t kTimeoutMs = 100;
    static constexpr uint8_t kMaxRetries = 3;
    static constexpr uint32_t kHelloMs = 1000;
    static constexpr uint32_t kPollStatesMs = 10000;
    static constexpr uint32_t kPollDiagMs = 30000;
    static constexpr uint32_t kPollDetailMs = 5000;
    static constexpr uint32_t kHardResetAfterMs = 30000;
    static constexpr uint32_t kHardResetGapMs = 5UL * 60UL * 1000UL;
    static constexpr uint8_t kQueueSize = 16;

    explicit LinkClient(ByteSink &out);

    void onByte(uint8_t byte, uint32_t now_ms);
    void tick(uint32_t now_ms);

    /* --- commands (queued, false if the queue is full or the input is
     * invalid) ------------------------------------------------------- */
    bool setTarget(uint8_t channel, uint16_t target);
    bool calibrate(uint16_t mask);
    bool detect(uint16_t mask);
    bool stop(uint16_t mask);
    bool setChannels(uint16_t mask);
    bool sensorScan();
    bool setSystemConfig(const vdm_system_config_t &c);
    bool setMotorParams(const vdm_motor_params_t &m);
    bool resetStm32();

    /* The STM32 firmware is being updated: stop talking to it. */
    void suspend(uint32_t now_ms);
    void resume(uint32_t now_ms);

    /* true once when the app should pulse NRST */
    bool takeHardResetRequest();

    const Model &model() const
    {
        return model_;
    }
    /* last error answer of a command, empty if none */
    const char *lastError() const
    {
        return last_error_;
    }

  private:
    struct Request {
        uint8_t msg;
        uint8_t len;
        uint8_t payload[40];
    };

    bool enqueue(uint8_t msg, const uint8_t *payload, size_t len);
    bool enqueueMask(uint8_t msg, uint16_t mask);
    void send(const Request &r, uint32_t now_ms);
    void startNext(uint32_t now_ms);
    bool pollDue(uint32_t now_ms, Request &r);
    void onFrame(const vdm_frame_t &f, uint32_t now_ms);
    void onResponse(const vdm_frame_t &f, uint32_t now_ms);
    void onEvent(const vdm_frame_t &f, uint32_t now_ms);
    void onConnected(uint32_t now_ms);
    void linkDown(uint32_t now_ms);
    void setState(LinkState s, uint32_t now_ms);
    void storeSensors(const vdm_sensors_t &s);
    void setError(const char *what, uint8_t status, uint8_t detail);

    ByteSink &out_;
    vdm_rx_t rx_;
    Model model_;

    Request queue_[kQueueSize];
    uint8_t q_head_ = 0;
    uint8_t q_count_ = 0;

    bool pending_ = false;
    Request cur_{};
    uint8_t cur_seq_ = 0;
    uint32_t sent_ms_ = 0;
    uint8_t retries_ = 0;
    uint8_t next_seq_ = 1;

    uint32_t last_hello_ms_ = 0;
    bool hello_sent_ = false;
    uint32_t last_contact_ms_ = 0;
    uint32_t last_hard_reset_ms_ = 0;
    bool hard_reset_done_ = false;
    bool hard_reset_request_ = false;

    uint32_t next_states_ms_ = 0;
    uint32_t next_diag_ms_ = 0;
    uint32_t next_detail_ms_ = 0;
    uint8_t detail_idx_ = 0;
    int16_t sensor_fetch_ = -1; /* next GET_SENSORS offset, -1: none */
    bool fetch_config_ = false;
    bool have_evt_seq_ = false;
    uint8_t last_evt_seq_ = 0;

    uint16_t desired_[VDM_MAX_CHANNELS] = {};
    uint16_t desired_valid_ = 0;
    uint16_t desired_dirty_ = 0;
    uint16_t targets_in_flight_ = 0;

    char last_error_[64] = {};
};

} // namespace vdm
