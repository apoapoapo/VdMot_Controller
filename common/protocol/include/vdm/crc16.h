/*
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout).
 * Check value for "123456789" is 0x29B1.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VDM_CRC16_H
#define VDM_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDM_CRC16_INIT 0xFFFFu

uint16_t vdm_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

static inline uint16_t vdm_crc16(const uint8_t *data, size_t len)
{
    return vdm_crc16_update(VDM_CRC16_INIT, data, len);
}

#ifdef __cplusplus
}
#endif

#endif /* VDM_CRC16_H */
