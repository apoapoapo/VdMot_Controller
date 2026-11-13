/*
 * LinkServer - STM32 side of the ESP32 link: frame reception, request
 * dispatch, protection against double execution and event output.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

#include "core/ports.h"
#include "vdm/frame.h"

namespace vdm
{

class RequestHandler
{
  public:
    virtual ~RequestHandler() = default;
    /* Handles one request and writes the complete response payload
     * (status byte first) to `resp`. Returns its length. */
    virtual size_t handleRequest(uint8_t msg, const uint8_t *payload, size_t len, uint8_t *resp,
                                 size_t cap) = 0;
};

class LinkServer
{
  public:
    LinkServer(LinkPort &port, RequestHandler &handler) : port_(port), handler_(handler)
    {
        vdm_rx_init(&rx_);
    }

    /* Feeds one received byte. Returns true if it completed a valid request. */
    bool onByte(uint8_t byte);
    /* Sends an event frame. */
    bool sendEvent(uint8_t msg, const uint8_t *payload, size_t len);
    /* Forgets the cached last response (HELLO with reset_seq). */
    void resetSequence()
    {
        cache_valid_ = false;
    }

    const vdm_rx_stats_t &rxStats() const
    {
        return rx_.stats;
    }
    uint16_t duplicates() const
    {
        return duplicates_;
    }
    void clearStats();

  private:
    void sendResponse(uint8_t seq, uint8_t msg, const uint8_t *payload, size_t len);

    LinkPort &port_;
    RequestHandler &handler_;
    vdm_rx_t rx_;

    bool cache_valid_ = false;
    uint8_t cache_seq_ = 0;
    uint8_t cache_msg_ = 0;
    uint8_t cache_len_ = 0;
    uint8_t cache_[VDM_PAYLOAD_MAX] = {};

    uint8_t evt_seq_ = 0;
    uint16_t duplicates_ = 0;
};

} // namespace vdm
