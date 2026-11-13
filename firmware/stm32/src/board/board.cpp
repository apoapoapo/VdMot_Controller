/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "board/board.h"

#include <Arduino.h>
#include <Wire.h>

#include "board/pins.h"
#include "vdm/link.h"

namespace board
{

/* ---------------------------------------------------------------------- */
/* Motor                                                                   */
/* ---------------------------------------------------------------------- */

void safeOutputs()
{
    for (PinName p : kEnable) {
        digitalWriteFast(p, LOW);
        pinMode(pinNametoDigitalPin(p), OUTPUT);
    }
    digitalWriteFast(kDirection, LOW);
    pinMode(pinNametoDigitalPin(kDirection), OUTPUT);
    digitalWriteFast(kMux, kMuxOdd);
    pinMode(pinNametoDigitalPin(kMux), OUTPUT);
    digitalWriteFast(kPowerEnable, HIGH); /* PSU off */
    pinMode(pinNametoDigitalPin(kPowerEnable), OUTPUT);
}

void Motor::power(bool on)
{
    digitalWriteFast(kPowerEnable, on ? LOW : HIGH);
}

void Motor::select(uint8_t channel, vdm::Dir dir)
{
    bool even = (channel % 2u) == 0;
    digitalWriteFast(kMux, even ? kMuxEven : kMuxOdd);
    digitalWriteFast(kDirection, dir == vdm::Dir::Open ? LOW : HIGH);
}

void Motor::enable(uint8_t channel)
{
    if (channel < VDM_MAX_CHANNELS) {
        digitalWriteFast(kEnable[channel / 2u], HIGH);
    }
}

void Motor::disableAll()
{
    for (PinName p : kEnable) {
        digitalWriteFast(p, LOW);
    }
}

/* ---------------------------------------------------------------------- */
/* Sampling                                                                */
/* ---------------------------------------------------------------------- */

namespace
{

vdm::MotorRun *g_run = nullptr;
HardwareTimer g_timer(TIM11);

void onTimer()
{
    int32_t d =
        static_cast<int32_t>(analogRead(kCurrent)) - static_cast<int32_t>(analogRead(kCurrentRef));
    int32_t ma10 = d * kCurrentUaPerDigit / 100;
    if (ma10 > INT16_MAX) {
        ma10 = INT16_MAX;
    } else if (ma10 < INT16_MIN) {
        ma10 = INT16_MIN;
    }
    g_run->onSample(static_cast<int16_t>(ma10));
}

void onPulse()
{
    g_run->onPulse();
}

} // namespace

void startSampling(vdm::MotorRun &run)
{
    g_run = &run;
    analogReadResolution(12);
    pinMode(kRevPulse, INPUT);
    /* both interrupts at the same preemption priority: they never preempt
     * each other (see core/motor_run.h) */
    attachInterrupt(digitalPinToInterrupt(kRevPulse), onPulse, RISING);
    g_timer.setOverflow(1000, HERTZ_FORMAT);
    g_timer.setInterruptPriority(EXTI_IRQ_PRIO, 0);
    g_timer.attachInterrupt(onTimer);
    g_timer.resume();
}

/* ---------------------------------------------------------------------- */
/* EEPROM                                                                  */
/* ---------------------------------------------------------------------- */

void Eeprom::begin()
{
    Wire.setSCL(kI2cScl);
    Wire.setSDA(kI2cSda);
    Wire.begin();
    Wire.setClock(400000);
}

bool Eeprom::read(uint16_t addr, uint8_t *buf, size_t len)
{
    while (len > 0) {
        size_t n = len > 32 ? 32 : len;
        Wire.beginTransmission(kEepromAddress);
        Wire.write(static_cast<uint8_t>(addr >> 8));
        Wire.write(static_cast<uint8_t>(addr));
        if (Wire.endTransmission(false) != 0) {
            return false;
        }
        if (Wire.requestFrom(kEepromAddress, static_cast<uint8_t>(n)) != n) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            buf[i] = static_cast<uint8_t>(Wire.read());
        }
        buf += n;
        addr = static_cast<uint16_t>(addr + n);
        len -= n;
    }
    return true;
}

bool Eeprom::writePage(uint16_t addr, const uint8_t *buf, size_t len)
{
    Wire.beginTransmission(kEepromAddress);
    Wire.write(static_cast<uint8_t>(addr >> 8));
    Wire.write(static_cast<uint8_t>(addr));
    Wire.write(buf, len);
    return Wire.endTransmission() == 0;
}

bool Eeprom::busy()
{
    /* acknowledge polling: the device NAKs its address during a write cycle */
    Wire.beginTransmission(kEepromAddress);
    return Wire.endTransmission() != 0;
}

/* ---------------------------------------------------------------------- */
/* 1-Wire                                                                  */
/* ---------------------------------------------------------------------- */

namespace
{

inline void owLow()
{
    digitalWriteFast(kOneWire, LOW);
}

inline void owRelease()
{
    digitalWriteFast(kOneWire, HIGH); /* open drain: the pull-up takes over */
}

inline bool owRead()
{
    return digitalReadFast(kOneWire) != 0;
}

} // namespace

void OneWireBus::begin()
{
    owRelease();
    pinMode(pinNametoDigitalPin(kOneWire), OUTPUT_OPEN_DRAIN);
}

vdm::OneWirePort::Reset OneWireBus::reset()
{
    /* the line must be high before a reset, otherwise it is shorted */
    for (int i = 0; i < 125 && !owRead(); i++) {
        delayMicroseconds(2);
    }
    if (!owRead()) {
        return Reset::Short;
    }
    owLow();
    delayMicroseconds(480);
    noInterrupts();
    owRelease();
    delayMicroseconds(70);
    bool presence = !owRead();
    interrupts();
    delayMicroseconds(410);
    return presence ? Reset::Presence : Reset::NoPresence;
}

void OneWireBus::writeBit(bool bit)
{
    noInterrupts();
    owLow();
    if (bit) {
        delayMicroseconds(6);
        owRelease();
        interrupts();
        delayMicroseconds(64);
    } else {
        delayMicroseconds(60);
        owRelease();
        interrupts();
        delayMicroseconds(10);
    }
}

bool OneWireBus::readBit()
{
    noInterrupts();
    owLow();
    delayMicroseconds(6);
    owRelease();
    delayMicroseconds(9);
    bool r = owRead();
    interrupts();
    delayMicroseconds(55);
    return r;
}

/* ---------------------------------------------------------------------- */
/* Link                                                                    */
/* ---------------------------------------------------------------------- */

void Link::begin()
{
    Serial1.setRx(kLinkRx);
    Serial1.setTx(kLinkTx);
    Serial1.begin(115200, SERIAL_8N1);
}

void Link::write(const uint8_t *data, size_t len)
{
    Serial1.write(data, len);
}

int Link::read()
{
    return Serial1.read();
}

/* ---------------------------------------------------------------------- */
/* System                                                                  */
/* ---------------------------------------------------------------------- */

void System::reset()
{
    Serial1.flush();
    safeOutputs();
    NVIC_SystemReset();
}

vdm::PlatformDiag System::diag()
{
    /* UART error counters and the MCU temperature are not available yet */
    return {0, loop_max_us_, 0};
}

void System::clearDiag()
{
    loop_max_us_ = 0;
}

void System::loopTime(uint32_t us)
{
    if (us > loop_max_us_) {
        loop_max_us_ = us;
    }
}

uint8_t readResetCause()
{
    uint32_t csr = RCC->CSR;
    __HAL_RCC_CLEAR_RESET_FLAGS();
    if (csr & RCC_CSR_IWDGRSTF) {
        return VDM_RESET_IWDG;
    }
    if (csr & RCC_CSR_SFTRSTF) {
        return VDM_RESET_SOFTWARE;
    }
    if (csr & RCC_CSR_PORRSTF) {
        return VDM_RESET_POWER_ON;
    }
    if (csr & RCC_CSR_BORRSTF) {
        return VDM_RESET_BROWN_OUT;
    }
    if (csr & RCC_CSR_LPWRRSTF) {
        return VDM_RESET_LOW_POWER;
    }
    if (csr & RCC_CSR_PINRSTF) {
        return VDM_RESET_PIN;
    }
    return VDM_RESET_UNKNOWN;
}

void readUid(uint8_t (&uid)[12])
{
    const uint32_t w[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
    for (int i = 0; i < 3; i++) {
        uid[4 * i + 0] = static_cast<uint8_t>(w[i]);
        uid[4 * i + 1] = static_cast<uint8_t>(w[i] >> 8);
        uid[4 * i + 2] = static_cast<uint8_t>(w[i] >> 16);
        uid[4 * i + 3] = static_cast<uint8_t>(w[i] >> 24);
    }
}

} // namespace board
