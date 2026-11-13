/*
 * Firmware info block embedded in every STM32 v2 image.
 *
 * The ESP32 searches the image for it before flashing and refuses to
 * flash an image built for another chip (a wrong image does not start, and
 * without a working application the STM32 can only be recovered with the
 * BOOT0 button). Images without the block (e.g. legacy 1.x) are accepted.
 *
 * Layout (16 bytes, little-endian):
 *   0  magic "VDMOTFW2" (8 bytes)
 *   8  u16 chip id A   (DBGMCU IDCODE[11:0] the image runs on)
 *  10  u16 chip id B   (second accepted chip id, may equal A)
 *  12  u8  hardware revision (vdm_hw_rev_t)
 *  13  u8  link protocol version
 *  14  u16 reserved (0)
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef VDM_FW_INFO_H
#define VDM_FW_INFO_H

#include <stdint.h>

#define VDM_FW_INFO_MAGIC "VDMOTFW2"
#define VDM_FW_INFO_MAGIC_LEN 8u
#define VDM_FW_INFO_LEN 16u

/* DBGMCU IDCODE device ids */
#define VDM_CHIP_F401XC 0x423u /* STM32F401xB/C */
#define VDM_CHIP_F401XE 0x433u /* STM32F401xD/E */
#define VDM_CHIP_F411XE 0x431u /* STM32F411xC/E */

#endif /* VDM_FW_INFO_H */
