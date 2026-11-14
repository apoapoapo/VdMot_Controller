/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/link_task.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "app/shared.h"
#include "core/link_client.h"
#include "core/stm32_flasher.h"

namespace app
{

namespace
{

constexpr int kPinStmRx = 5;
constexpr int kPinStmTx = 17;
constexpr int kPinStmReset = 15; /* high = STM32 held in reset */
constexpr int kPinStmBoot0 = 14; /* high = ROM bootloader */
constexpr uint32_t kPublishMs = 100;

HardwareSerial &uart = Serial2;

class UartSink : public vdm::ByteSink
{
  public:
    void write(const uint8_t *data, size_t len) override
    {
        uart.write(data, len);
    }
};

class Stm32BootPort : public vdm::BootPort
{
  public:
    void begin(uint32_t baud, bool even_parity) override
    {
        uart.flush();
        uart.begin(baud, even_parity ? SERIAL_8E1 : SERIAL_8N1, kPinStmRx, kPinStmTx);
    }
    void write(const uint8_t *data, size_t len) override
    {
        uart.write(data, len);
    }
    int read(uint32_t timeout_ms) override
    {
        uint32_t start = millis();
        while (uart.available() == 0) {
            if (millis() - start >= timeout_ms) {
                return -1;
            }
            esp_task_wdt_reset();
            delay(1);
        }
        return uart.read();
    }
    void drain() override
    {
        while (uart.available() > 0) {
            uart.read();
        }
    }
    void setBoot0(bool high) override
    {
        digitalWrite(kPinStmBoot0, high ? HIGH : LOW);
    }
    void setReset(bool asserted) override
    {
        digitalWrite(kPinStmReset, asserted ? HIGH : LOW);
    }
    void delayMs(uint32_t ms) override
    {
        esp_task_wdt_reset();
        delay(ms);
    }
};

class FileImage : public vdm::ImageSource
{
  public:
    explicit FileImage(File &f) : f_(f) {}
    size_t size() const override
    {
        return f_.size();
    }
    bool read(size_t offset, uint8_t *buf, size_t len) override
    {
        return f_.seek(offset) && f_.read(buf, len) == len;
    }

  private:
    File &f_;
};

UartSink sink;
Stm32BootPort boot_port;
vdm::LinkClient *client = nullptr;

void progress(void *, uint8_t percent, const char *phase)
{
    UpdateStatus s = updateStatus();
    s.active = true;
    s.percent = percent;
    strlcpy(s.phase, phase, sizeof(s.phase));
    setUpdateStatus(s);
}

void flashStm32()
{
    UpdateStatus s;
    s.active = true;
    strlcpy(s.phase, "starting", sizeof(s.phase));
    setUpdateStatus(s);

    client->suspend(millis());
    publishModel(client->model(), client->lastError());

    vdm::FlashResult r = vdm::FlashResult::ReadFailed;
    uint16_t chip = 0;
    static char detail[256];
    detail[0] = '\0';
    File f = LittleFS.open(kStm32ImagePath, "r");
    if (f) {
        FileImage img(f);
        vdm::Stm32Flasher flasher;
        r = flasher.flash(boot_port, img, progress, nullptr);
        chip = flasher.chipId();
        strlcpy(detail, flasher.trace(), sizeof(detail));
        f.close();
    }
    if (r == vdm::FlashResult::Ok) {
        LittleFS.remove(kStm32ImagePath);
    }

    s = updateStatus();
    s.active = false;
    s.percent = r == vdm::FlashResult::Ok ? 100 : s.percent;
    strlcpy(s.phase, r == vdm::FlashResult::Ok ? "done" : "failed", sizeof(s.phase));
    const char *chip_name = chip == 0x431   ? "STM32F411"
                            : chip == 0x423 ? "STM32F401xC"
                            : chip == 0x433 ? "STM32F401xE"
                                            : "unknown chip";
    snprintf(s.result, sizeof(s.result), "%s (%s, id 0x%03X)", vdm::flashResultName(r), chip_name,
             chip);
    strlcpy(s.detail, detail, sizeof(s.detail));
    setUpdateStatus(s);
    espLog(r == vdm::FlashResult::Ok ? VDM_LOG_INFO : VDM_LOG_ERROR, "STM32 update: %s", s.result);

    uart.begin(115200, SERIAL_8N1, kPinStmRx, kPinStmTx);
    client->resume(millis());
}

void hardReset()
{
    digitalWrite(kPinStmBoot0, LOW);
    digitalWrite(kPinStmReset, HIGH); /* only reaches the STM32 with jumper X20 set */
    delay(150);
    digitalWrite(kPinStmReset, LOW);
}

void execute(const Command &c)
{
    switch (c.type) {
    case CommandType::SetTarget:
        client->setTarget(c.channel, c.value);
        break;
    case CommandType::SetTargets:
        for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
            if ((c.mask >> ch) & 1u) {
                client->setTarget(ch, c.targets[ch]);
            }
        }
        break;
    case CommandType::Calibrate:
        client->calibrate(c.value);
        break;
    case CommandType::Detect:
        client->detect(c.value);
        break;
    case CommandType::Stop:
        client->stop(c.value);
        break;
    case CommandType::SetChannels:
        client->setChannels(c.value);
        break;
    case CommandType::SensorScan:
        client->sensorScan();
        break;
    case CommandType::SetSystemConfig:
        client->setSystemConfig(c.sys);
        break;
    case CommandType::SetMotorParams:
        client->setMotorParams(c.motor);
        break;
    case CommandType::ResetStm32:
        client->resetStm32();
        break;
    case CommandType::HardResetStm32:
        hardReset();
        break;
    case CommandType::PauseLink:
        client->pause(millis(), static_cast<uint32_t>(c.value) * 1000u);
        break;
    case CommandType::FlashStm32:
        flashStm32();
        break;
    }
}

void linkTask(void *)
{
    esp_task_wdt_add(nullptr);
    static vdm::LinkClient c(sink);
    client = &c;
    uint32_t last_publish = 0;

    for (;;) {
        uint32_t now = millis();
        while (uart.available() > 0) {
            client->onByte(static_cast<uint8_t>(uart.read()), now);
        }
        Command cmd;
        while (receiveCommand(cmd)) {
            execute(cmd);
        }
        client->tick(millis());
        if (client->takeHardResetRequest()) {
            espLog(VDM_LOG_WARN, "STM32 does not answer: reset pulse (needs jumper X20)");
            hardReset();
        }
        if (millis() - last_publish >= kPublishMs) {
            last_publish = millis();
            publishModel(client->model(), client->lastError());
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

} // namespace

void startLinkTask()
{
    pinMode(kPinStmBoot0, OUTPUT);
    digitalWrite(kPinStmBoot0, LOW);
    pinMode(kPinStmReset, OUTPUT);
    digitalWrite(kPinStmReset, LOW); /* release reset */

    uart.setRxBufferSize(2048);
    uart.begin(115200, SERIAL_8N1, kPinStmRx, kPinStmTx);
    xTaskCreatePinnedToCore(linkTask, "link", 8192, nullptr, 5, nullptr, 1);
}

} // namespace app
