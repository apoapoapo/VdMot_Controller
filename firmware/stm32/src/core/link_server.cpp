/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/link_server.h"

#include <string.h>

namespace vdm
{

void LinkServer::sendResponse(uint8_t seq, uint8_t msg, const uint8_t *payload, size_t len)
{
    uint8_t wire[VDM_WIRE_MAX];
    size_t n = 0;
    if (vdm_frame_encode(VDM_KIND_RESPONSE, seq, msg, payload, len, wire, sizeof(wire), &n)) {
        port_.write(wire, n);
    }
}

bool LinkServer::onByte(uint8_t byte)
{
    vdm_frame_t f;
    if (!vdm_rx_feed(&rx_, byte, &f) || f.kind != VDM_KIND_REQUEST) {
        return false;
    }

    if (cache_valid_ && f.seq == cache_seq_ && f.msg == cache_msg_) {
        /* retransmission: answer again, do not execute twice */
        if (duplicates_ != UINT16_MAX) {
            duplicates_++;
        }
        sendResponse(f.seq, f.msg, cache_, cache_len_);
        return true;
    }

    /* the payload points into rx_, which stays untouched until the next
     * byte is fed; copy it anyway so the handler may send events */
    uint8_t req[VDM_PAYLOAD_MAX];
    size_t req_len = f.len;
    memcpy(req, f.payload, req_len);
    uint8_t seq = f.seq;
    uint8_t msg = f.msg;

    uint8_t resp[VDM_PAYLOAD_MAX];
    size_t n = handler_.handleRequest(msg, req, req_len, resp, sizeof(resp));
    if (n == 0 || n > sizeof(resp)) {
        resp[0] = 0xFF; /* VDM_ERR_INTERNAL */
        n = 1;
    }
    memcpy(cache_, resp, n);
    cache_len_ = static_cast<uint8_t>(n);
    cache_seq_ = seq;
    cache_msg_ = msg;
    cache_valid_ = true;
    sendResponse(seq, msg, resp, n);
    return true;
}

bool LinkServer::sendEvent(uint8_t msg, const uint8_t *payload, size_t len)
{
    evt_seq_ = static_cast<uint8_t>(evt_seq_ + 1u);
    if (evt_seq_ == 0) {
        evt_seq_ = 1;
    }
    uint8_t wire[VDM_WIRE_MAX];
    size_t n = 0;
    if (!vdm_frame_encode(VDM_KIND_EVENT, evt_seq_, msg, payload, len, wire, sizeof(wire), &n)) {
        return false;
    }
    port_.write(wire, n);
    return true;
}

void LinkServer::clearStats()
{
    rx_.stats = vdm_rx_stats_t{};
    duplicates_ = 0;
}

} // namespace vdm
