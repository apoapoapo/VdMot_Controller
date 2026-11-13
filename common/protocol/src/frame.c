/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "vdm/frame.h"

#include <string.h>

#include "vdm/cobs.h"
#include "vdm/crc16.h"

static void inc16(uint16_t *c)
{
    if (*c != UINT16_MAX) {
        (*c)++;
    }
}

bool vdm_frame_encode(uint8_t kind, uint8_t seq, uint8_t msg, const uint8_t *payload, size_t len,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t raw[VDM_FRAME_MAX];

    if (len > VDM_PAYLOAD_MAX || seq == 0 || (len > 0 && payload == NULL)) {
        return false;
    }

    raw[0] = VDM_PROTO_VERSION;
    raw[1] = kind;
    raw[2] = seq;
    raw[3] = msg;
    if (len > 0) {
        memcpy(&raw[VDM_HEADER_LEN], payload, len);
    }
    size_t n = VDM_HEADER_LEN + len;
    uint16_t crc = vdm_crc16(raw, n);
    raw[n++] = (uint8_t)(crc & 0xFFu);
    raw[n++] = (uint8_t)(crc >> 8);

    size_t enc_len;
    if (out_cap < 1u || !vdm_cobs_encode(raw, n, out, out_cap - 1u, &enc_len)) {
        return false;
    }
    out[enc_len] = 0x00;
    *out_len = enc_len + 1u;
    return true;
}

void vdm_rx_init(vdm_rx_t *rx)
{
    memset(rx, 0, sizeof(*rx));
}

static bool rx_complete(vdm_rx_t *rx, vdm_frame_t *out)
{
    size_t n;

    if (!vdm_cobs_decode(rx->enc, rx->pos, rx->frame, sizeof(rx->frame), &n)) {
        inc16(&rx->stats.malformed);
        return false;
    }
    if (n < VDM_FRAME_MIN) {
        inc16(&rx->stats.malformed);
        return false;
    }

    uint16_t crc_rx = (uint16_t)(rx->frame[n - 2] | ((uint16_t)rx->frame[n - 1] << 8));
    if (vdm_crc16(rx->frame, n - VDM_CRC_LEN) != crc_rx) {
        inc16(&rx->stats.crc_err);
        return false;
    }
    if (rx->frame[0] != VDM_PROTO_VERSION) {
        inc16(&rx->stats.bad_version);
        return false;
    }
    uint8_t kind = rx->frame[1];
    if ((kind != VDM_KIND_REQUEST && kind != VDM_KIND_RESPONSE && kind != VDM_KIND_EVENT) ||
        rx->frame[2] == 0) {
        inc16(&rx->stats.malformed);
        return false;
    }

    out->kind = kind;
    out->seq = rx->frame[2];
    out->msg = rx->frame[3];
    out->payload = &rx->frame[VDM_HEADER_LEN];
    out->len = n - VDM_FRAME_MIN;
    if (rx->stats.frames_ok != UINT32_MAX) {
        rx->stats.frames_ok++;
    }
    return true;
}

bool vdm_rx_feed(vdm_rx_t *rx, uint8_t byte, vdm_frame_t *out)
{
    if (byte != 0x00) {
        if (rx->overflow) {
            return false;
        }
        if (rx->pos >= sizeof(rx->enc)) {
            rx->overflow = true;
            inc16(&rx->stats.overflow);
            return false;
        }
        rx->enc[rx->pos++] = byte;
        return false;
    }

    /* delimiter */
    bool ok = false;
    if (!rx->overflow && rx->pos > 0) {
        ok = rx_complete(rx, out);
    }
    rx->pos = 0;
    rx->overflow = false;
    return ok;
}
