/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/stm32_flasher.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace vdm
{

namespace
{

constexpr uint8_t kAck = 0x79;
constexpr uint8_t kNack = 0x1F;
constexpr uint8_t kSync = 0x7F;
constexpr uint8_t kCmdGetId = 0x02;
constexpr uint8_t kCmdRead = 0x11;
constexpr uint8_t kCmdWrite = 0x31;
constexpr uint8_t kCmdExtErase = 0x44;
constexpr uint8_t kCmdGo = 0x21;
constexpr uint32_t kManualWindowMs = 30000;
constexpr uint32_t kEraseTimeoutMs = 30000;

/* STM32F4 sector layout: 4 x 16 KiB, 1 x 64 KiB, then 128 KiB sectors */
uint32_t sectorEnd(uint16_t sector)
{
    if (sector < 4) {
        return (sector + 1u) * 16384u;
    }
    if (sector == 4) {
        return 131072u;
    }
    return 131072u + (sector - 4u) * 131072u;
}

void report(Stm32Flasher::Progress p, void *ctx, uint8_t percent, const char *phase)
{
    if (p != nullptr) {
        p(ctx, percent, phase);
    }
}

} // namespace

const char *flashResultName(FlashResult r)
{
    switch (r) {
    case FlashResult::Ok:
        return "ok";
    case FlashResult::BadImage:
        return "not an STM32 application image";
    case FlashResult::NoBootloader:
        return "STM32 bootloader does not answer";
    case FlashResult::UnknownChip:
        return "unknown STM32 chip";
    case FlashResult::TooLarge:
        return "image larger than the flash";
    case FlashResult::EraseFailed:
        return "erase failed";
    case FlashResult::WriteFailed:
        return "write failed";
    case FlashResult::VerifyFailed:
        return "verification failed";
    case FlashResult::ReadFailed:
        return "cannot read the image";
    }
    return "?";
}

bool Stm32Flasher::plausibleImage(const uint8_t *head, size_t head_len, size_t size)
{
    if (head_len < 8 || size < 8 || size > 512u * 1024u) {
        return false;
    }
    uint32_t sp =
        head[0] | (head[1] << 8) | (head[2] << 16) | (static_cast<uint32_t>(head[3]) << 24);
    uint32_t pc =
        head[4] | (head[5] << 8) | (head[6] << 16) | (static_cast<uint32_t>(head[7]) << 24);
    bool sp_ok = sp > 0x20000000u && sp <= 0x20020000u;
    bool pc_ok = pc >= kFlashBase && pc < kFlashBase + size && (pc & 1u);
    return sp_ok && pc_ok;
}

void Stm32Flasher::runApplication(BootPort &port)
{
    port.setBoot0(false);
    port.delayMs(10);
    port.setReset(true);
    port.delayMs(50);
    port.setReset(false);
    port.begin(115200, false);
    port.drain();
}

void Stm32Flasher::note(const char *fmt, ...)
{
    size_t used = strlen(trace_);
    if (used + 1 >= sizeof(trace_)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(&trace_[used], sizeof(trace_) - used, fmt, ap);
    va_end(ap);
}

bool Stm32Flasher::probeSync(BootPort &port)
{
    port.drain();
    uint8_t b = kSync;
    port.write(&b, 1);
    int r = port.read(200);
    if (r >= 0 && r != kAck && r != kNack) {
        note("sync:%02X ", r);
    }
    return r == kAck || r == kNack; /* NACK: already synchronised */
}

bool Stm32Flasher::softwareEntry(BootPort &port, bool do_reset, uint32_t settle_ms,
                                 uint32_t interval_ms, int sends, uint32_t boot_ms)
{
    static const char kRequest[] = "DEADBEEF\r\n";
    static const char kAnswer[] = "BEEFIT";
    port.setBoot0(false);
    port.begin(115200, true);
    if (do_reset) {
        port.setReset(true);
        port.delayMs(150);
        port.setReset(false);
    }
    port.delayMs(settle_ms);
    port.drain();
    size_t match = 0;
    size_t received = 0;
    /* the boot window of the application lasts 2 s (v2) or 3 s (legacy) */
    for (int i = 0; i < sends; i++) {
        port.write(reinterpret_cast<const uint8_t *>(kRequest), sizeof(kRequest) - 1);
        int c;
        while ((c = port.read(interval_ms)) >= 0) {
            if (received < 12) {
                note("%02X", c);
            }
            received++;
            match = c == kAnswer[match] ? match + 1 : (c == kAnswer[0] ? 1 : 0);
            if (match == sizeof(kAnswer) - 1) {
                note(" BEEFIT ");
                port.delayMs(boot_ms); /* bootloader start-up */
                for (int s = 0; s < 5; s++) {
                    if (probeSync(port)) {
                        return true;
                    }
                    port.delayMs(200);
                }
                note("no sync; ");
                return false;
            }
        }
    }
    note(" rx %u bytes, no BEEFIT; ", static_cast<unsigned>(received));
    return false;
}

bool Stm32Flasher::boot0Entry(BootPort &port)
{
    static const uint32_t kBauds[] = {115200, 57600};
    note("boot0: ");
    for (uint32_t baud : kBauds) {
        for (int attempt = 0; attempt < 2; attempt++) {
            port.setBoot0(true);
            port.delayMs(10);
            port.setReset(true);
            port.delayMs(50);
            port.setReset(false);
            port.delayMs(150); /* bootloader start-up */
            port.begin(baud, true);
            if (probeSync(port)) {
                return true;
            }
        }
    }
    port.setBoot0(false);
    note("no answer");
    return false;
}

bool Stm32Flasher::go(BootPort &port)
{
    return command(port, kCmdGo) && sendAddress(port, kFlashBase);
}

bool Stm32Flasher::enterBootloader(BootPort &port, Progress progress, void *ctx)
{
    trace_[0] = '\0';
    port.setBoot0(false);
    port.begin(115200, true);
    note("probe: ");
    if (probeSync(port)) {
        note("bootloader active");
        return true;
    }
    /* 1st: timing of the legacy 1.x ESP32 firmware (known to work on the
     * boards), 2nd: faster timing */
    /* 1st: reset by the ESP32, timing of the legacy 1.x ESP32 firmware */
    note("sw: ");
    if (softwareEntry(port, true, 1500, 1000, 4, 1500)) {
        return true;
    }
    /* 2nd: the reset does not reach the STM32 - ask the user to press the
     * RESET button and keep sending the request meanwhile */
    report(progress, ctx, 0, "press RESET on the STM32");
    note("manual: ");
    if (softwareEntry(port, false, 0, 200, static_cast<int>(kManualWindowMs / 200), 1500)) {
        report(progress, ctx, 1, "connecting");
        return true;
    }
    report(progress, ctx, 0, "connecting");
    return boot0Entry(port);
}

bool Stm32Flasher::command(BootPort &port, uint8_t cmd, uint32_t timeout_ms)
{
    uint8_t b[2] = {cmd, static_cast<uint8_t>(~cmd)};
    port.write(b, 2);
    return port.read(timeout_ms) == kAck;
}

bool Stm32Flasher::sendWithChecksum(BootPort &port, const uint8_t *data, size_t len,
                                    uint32_t timeout_ms)
{
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) {
        x ^= data[i];
    }
    port.write(data, len);
    port.write(&x, 1);
    return port.read(timeout_ms) == kAck;
}

bool Stm32Flasher::sendAddress(BootPort &port, uint32_t addr)
{
    uint8_t a[4] = {static_cast<uint8_t>(addr >> 24), static_cast<uint8_t>(addr >> 16),
                    static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr)};
    return sendWithChecksum(port, a, 4, 1000);
}

bool Stm32Flasher::getId(BootPort &port)
{
    if (!command(port, kCmdGetId)) {
        return false;
    }
    int n = port.read(200);
    if (n < 1) {
        return false;
    }
    int hi = port.read(200);
    int lo = port.read(200);
    for (int i = 2; i <= n; i++) {
        port.read(200);
    }
    if (hi < 0 || lo < 0 || port.read(200) != kAck) {
        return false;
    }
    chip_id_ = static_cast<uint16_t>((hi << 8) | lo);
    switch (chip_id_) {
    case 0x423: /* STM32F401xB/C */
        flash_size_ = 256u * 1024u;
        return true;
    case 0x433: /* STM32F401xD/E */
    case 0x431: /* STM32F411xC/E */
        flash_size_ = 512u * 1024u;
        return true;
    default:
        flash_size_ = 0;
        return true;
    }
}

bool Stm32Flasher::erase(BootPort &port, size_t size)
{
    uint16_t count = 0;
    while (sectorEnd(count) < size) {
        count++;
    }
    count++; /* sectors 0..count-1 */
    if (!command(port, kCmdExtErase)) {
        return false;
    }
    uint8_t buf[2 + 2 * 8];
    size_t n = 0;
    uint16_t nm1 = static_cast<uint16_t>(count - 1u);
    buf[n++] = static_cast<uint8_t>(nm1 >> 8);
    buf[n++] = static_cast<uint8_t>(nm1);
    for (uint16_t s = 0; s < count; s++) {
        buf[n++] = static_cast<uint8_t>(s >> 8);
        buf[n++] = static_cast<uint8_t>(s);
    }
    return sendWithChecksum(port, buf, n, kEraseTimeoutMs);
}

bool Stm32Flasher::writeBlock(BootPort &port, uint32_t addr, const uint8_t *data, size_t len)
{
    if (!command(port, kCmdWrite) || !sendAddress(port, addr)) {
        return false;
    }
    uint8_t buf[1 + kBlock];
    buf[0] = static_cast<uint8_t>(len - 1u);
    memcpy(&buf[1], data, len);
    return sendWithChecksum(port, buf, len + 1u, 1000);
}

bool Stm32Flasher::readBlock(BootPort &port, uint32_t addr, uint8_t *data, size_t len)
{
    if (!command(port, kCmdRead) || !sendAddress(port, addr)) {
        return false;
    }
    uint8_t nm1 = static_cast<uint8_t>(len - 1u);
    uint8_t b[2] = {nm1, static_cast<uint8_t>(~nm1)};
    port.write(b, 2);
    if (port.read(1000) != kAck) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        int v = port.read(200);
        if (v < 0) {
            return false;
        }
        data[i] = static_cast<uint8_t>(v);
    }
    return true;
}

FlashResult Stm32Flasher::flash(BootPort &port, ImageSource &image, Progress progress, void *ctx)
{
    size_t size = image.size();
    uint8_t head[8];
    if (size < sizeof(head) || !image.read(0, head, sizeof(head))) {
        return FlashResult::BadImage;
    }
    if (!plausibleImage(head, sizeof(head), size)) {
        return FlashResult::BadImage;
    }

    report(progress, ctx, 0, "connecting");
    if (!enterBootloader(port, progress, ctx)) {
        runApplication(port);
        return FlashResult::NoBootloader;
    }
    FlashResult result = FlashResult::Ok;
    if (!getId(port) || flash_size_ == 0) {
        result = FlashResult::UnknownChip;
    } else if (size > flash_size_) {
        result = FlashResult::TooLarge;
    }

    if (result == FlashResult::Ok) {
        report(progress, ctx, 2, "erasing");
        if (!erase(port, size)) {
            result = FlashResult::EraseFailed;
        }
    }

    uint8_t block[kBlock];
    uint8_t back[kBlock];
    size_t blocks = (size + kBlock - 1) / kBlock;

    for (size_t i = 0; i < blocks && result == FlashResult::Ok; i++) {
        size_t off = i * kBlock;
        size_t len = size - off < kBlock ? size - off : kBlock;
        if (!image.read(off, block, len)) {
            result = FlashResult::ReadFailed;
            break;
        }
        size_t padded = (len + 3u) & ~static_cast<size_t>(3u);
        memset(&block[len], 0xFF, padded - len);
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            ok = writeBlock(port, kFlashBase + static_cast<uint32_t>(off), block, padded);
        }
        if (!ok) {
            result = FlashResult::WriteFailed;
            break;
        }
        report(progress, ctx, static_cast<uint8_t>(5 + 75 * (i + 1) / blocks), "writing");
    }

    for (size_t i = 0; i < blocks && result == FlashResult::Ok; i++) {
        size_t off = i * kBlock;
        size_t len = size - off < kBlock ? size - off : kBlock;
        if (!image.read(off, block, len)) {
            result = FlashResult::ReadFailed;
            break;
        }
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            ok = readBlock(port, kFlashBase + static_cast<uint32_t>(off), back, len);
        }
        if (!ok || memcmp(block, back, len) != 0) {
            result = FlashResult::VerifyFailed;
            break;
        }
        report(progress, ctx, static_cast<uint8_t>(80 + 20 * (i + 1) / blocks), "verifying");
    }

    if (result == FlashResult::Ok) {
        note(go(port) ? "; go: ok" : "; go: no answer");
    }
    runApplication(port);
    return result;
}

} // namespace vdm
