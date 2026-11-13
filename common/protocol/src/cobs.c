/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "vdm/cobs.h"

bool vdm_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_cap < VDM_COBS_MAX_ENCODED(len)) {
        return false;
    }

    size_t code_pos = 0; /* position of the current block's code byte */
    size_t w = 1;
    uint8_t code = 1;

    for (size_t i = 0; i < len; i++) {
        if (in[i] == 0) {
            out[code_pos] = code;
            code_pos = w++;
            code = 1;
        } else {
            out[w++] = in[i];
            code++;
            if (code == 0xFF) {
                out[code_pos] = code;
                code = 1;
                if (i + 1 == len) {
                    /* canonical: no empty trailing block after a full one */
                    *out_len = w;
                    return true;
                }
                code_pos = w++;
            }
        }
    }
    out[code_pos] = code;
    *out_len = w;
    return true;
}

bool vdm_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (len == 0) {
        return false;
    }

    size_t r = 0;
    size_t w = 0;

    while (r < len) {
        uint8_t code = in[r++];
        if (code == 0) {
            return false;
        }
        size_t n = (size_t)code - 1u;
        if (n > len - r || n > out_cap - w) {
            return false;
        }
        for (size_t k = 0; k < n; k++) {
            if (in[r] == 0) {
                return false;
            }
            out[w++] = in[r++];
        }
        if (code != 0xFF && r < len) {
            if (w >= out_cap) {
                return false;
            }
            out[w++] = 0;
        }
    }
    *out_len = w;
    return true;
}
