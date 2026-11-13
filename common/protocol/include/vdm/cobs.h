/*
 * Consistent Overhead Byte Stuffing (canonical encoding).
 * The encoded data never contains 0x00; the 0x00 delimiter is NOT written
 * by these functions.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VDM_COBS_H
#define VDM_COBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Worst-case encoded size for `len` input bytes. */
#define VDM_COBS_MAX_ENCODED(len) ((len) + ((len) / 254u) + 1u)

/* Encodes `len` bytes. Returns false if `out_cap` is smaller than
 * VDM_COBS_MAX_ENCODED(len). `in` and `out` must not overlap. */
bool vdm_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap, size_t *out_len);

/* Decodes `len` bytes (without delimiter). Returns false on malformed input
 * (empty input, embedded 0x00, block running past the end) or if the result
 * does not fit into `out_cap`. `in` and `out` must not overlap. */
bool vdm_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* VDM_COBS_H */
