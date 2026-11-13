/*
 * Stm32Flasher - programs the STM32 through its ROM bootloader over the
 * link UART (ST application note AN3155, USART protocol).
 *
 * Entering the bootloader, in this order:
 *  1. the STM32 is already in the bootloader (answers the sync byte)
 *  2. software entry: reset the STM32 (NRST) and send "DEADBEEF" during the
 *     boot window of the running application; it answers "BEEFIT" and
 *     jumps to the ROM bootloader. Both the legacy 1.x and the v2 STM32
 *     firmware support this. BOOT0 is not wired to the ESP32 on the C2/C3
 *     boards, so this is the normal path.
 *     If the reset does not reach the STM32 (seen on some boards), the user
 *     is asked to press the RESET button of the BlackPill while the ESP32
 *     keeps sending the request for 30 s.
 *  3. BOOT0 high + reset: only works if BOOT0 is wired (solder pad next to
 *     the ESP32 connector) or the BOOT0 button is held.
 *
 * After a successful update the new firmware is started with the
 * bootloader's GO command (and a reset), so no reset line is required.
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
    /* What happened while entering the bootloader (for diagnostics). */
    const char *trace() const
    {
        return trace_;
    }

    /* Restarts the STM32 into its application (BOOT0 low, reset pulse). */
    static void runApplication(BootPort &port);

  private:
    bool enterBootloader(BootPort &port, Progress progress, void *ctx);
    bool go(BootPort &port);
    bool probeSync(BootPort &port);
    bool softwareEntry(BootPort &port, bool do_reset, uint32_t settle_ms, uint32_t interval_ms,
                       int sends, uint32_t boot_ms);
    void note(const char *fmt, ...);
    bool boot0Entry(BootPort &port);
    bool command(BootPort &port, uint8_t cmd, uint32_t timeout_ms = 1000);
    bool sendWithChecksum(BootPort &port, const uint8_t *data, size_t len, uint32_t timeout_ms);
    bool sendAddress(BootPort &port, uint32_t addr);
    bool getId(BootPort &port);
    bool erase(BootPort &port, size_t size);
    bool writeBlock(BootPort &port, uint32_t addr, const uint8_t *data, size_t len);
    bool readBlock(BootPort &port, uint32_t addr, uint8_t *data, size_t len);

    char trace_[256] = {};
    uint16_t chip_id_ = 0;
    size_t flash_size_ = 0;
};

} // namespace vdm
