/*
 * Stm32Flasher - programs the STM32 through its ROM bootloader over the
 * link UART (ST application note AN3155, USART protocol).
 *
 * The ESP32 drives BOOT0 and NRST, so no button needs to be pressed and
 * the STM32 application does not need to cooperate (a broken firmware can
 * always be replaced).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vdm
{

class BootPort
{
  public:
    virtual ~BootPort() = default;
    virtual void begin(uint32_t baud, bool even_parity) = 0;
    virtual void write(const uint8_t *data, size_t len) = 0;
    /* one byte or -1 after `timeout_ms` */
    virtual int read(uint32_t timeout_ms) = 0;
    virtual void drain() = 0;
    virtual void setBoot0(bool high) = 0;
    virtual void setReset(bool asserted) = 0;
    virtual void delayMs(uint32_t ms) = 0;
};

class ImageSource
{
  public:
    virtual ~ImageSource() = default;
    virtual size_t size() const = 0;
    virtual bool read(size_t offset, uint8_t *buf, size_t len) = 0;
};

enum class FlashResult : uint8_t {
    Ok,
    BadImage,     /* not an STM32 application image */
    NoBootloader, /* no answer to the sync byte */
    UnknownChip,
    TooLarge,
    EraseFailed,
    WriteFailed,
    VerifyFailed,
    ReadFailed, /* image source error */
};

const char *flashResultName(FlashResult r);

class Stm32Flasher
{
  public:
    using Progress = void (*)(void *ctx, uint8_t percent, const char *phase);

    static constexpr uint32_t kFlashBase = 0x08000000u;
    static constexpr size_t kBlock = 256;

    /* Checks the vector table of an application image (stack pointer in
     * SRAM, reset handler in flash). */
    static bool plausibleImage(const uint8_t *head, size_t head_len, size_t size);

    FlashResult flash(BootPort &port, ImageSource &image, Progress progress = nullptr,
                      void *ctx = nullptr);
    uint16_t chipId() const
    {
        return chip_id_;
    }

    /* Restarts the STM32 into its application (BOOT0 low, reset pulse). */
    static void runApplication(BootPort &port);

  private:
    bool sync(BootPort &port);
    bool command(BootPort &port, uint8_t cmd, uint32_t timeout_ms = 1000);
    bool sendWithChecksum(BootPort &port, const uint8_t *data, size_t len, uint32_t timeout_ms);
    bool sendAddress(BootPort &port, uint32_t addr);
    bool getId(BootPort &port);
    bool erase(BootPort &port, size_t size);
    bool writeBlock(BootPort &port, uint32_t addr, const uint8_t *data, size_t len);
    bool readBlock(BootPort &port, uint32_t addr, uint8_t *data, size_t len);

    uint16_t chip_id_ = 0;
    size_t flash_size_ = 0;
};

} // namespace vdm
