/*
 * Frame layer: builds wire frames and reassembles them from a byte stream.
 *
 *   decoded frame = ver | kind | seq | msg | payload[0..240] | crc_lo | crc_hi
 *   wire          = COBS(decoded frame) | 0x00
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VDM_FRAME_H
#define VDM_FRAME_H

#include "vdm/link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t kind; /* vdm_kind_t */
    uint8_t seq;
    uint8_t msg; /* vdm_msg_t */
    const uint8_t *payload;
    size_t len;
} vdm_frame_t;

/* Builds a complete wire frame including the trailing 0x00.
 * Returns false if the payload is too long, seq is 0 or `out_cap` is too
 * small (VDM_WIRE_MAX is always sufficient). */
bool vdm_frame_encode(uint8_t kind, uint8_t seq, uint8_t msg, const uint8_t *payload, size_t len,
                      uint8_t *out, size_t out_cap, size_t *out_len);

/* Receive statistics; counters saturate at their maximum value. */
typedef struct {
    uint32_t frames_ok;
    uint16_t crc_err;
    uint16_t malformed; /* COBS error, bad length, bad kind, seq 0 */
    uint16_t overflow;  /* more than VDM_ENCODED_MAX bytes without delimiter */
    uint16_t bad_version;
} vdm_rx_stats_t;

typedef struct {
    uint8_t enc[VDM_ENCODED_MAX];
    size_t pos;
    bool overflow;
    uint8_t frame[VDM_FRAME_MAX];
    vdm_rx_stats_t stats;
} vdm_rx_t;

void vdm_rx_init(vdm_rx_t *rx);

/* Feeds one received byte. Returns true when a valid frame has been
 * completed; `*out` then points into `rx` and stays valid until the next
 * call of vdm_rx_feed(). Invalid frames are dropped and counted. */
bool vdm_rx_feed(vdm_rx_t *rx, uint8_t byte, vdm_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VDM_FRAME_H */
