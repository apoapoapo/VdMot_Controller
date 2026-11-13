/*
 * VdMot Controller v2 - STM32 firmware
 *
 * Phase 0 skeleton: puts all motor outputs into a safe state and answers
 * HELLO and PING on the ESP32 espLink. Every other request is answered with
 * ERR_UNKNOWN_MSG. The real firmware is built in phase 1.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <Arduino.h>

#include "vdm/codec.h"
#include "vdm/frame.h"

#ifndef VDM_MCU
#error "VDM_MCU must be defined by the build environment"
#endif
#ifndef VDM_HW_REV
#error "VDM_HW_REV must be defined by the build environment"
#endif

namespace
{

// Pin assignment of hardware revision C2 (see hardware/schematic_c2-sample_V02.pdf)
constexpr uint32_t kPinPowerEnable = PB9; // valve PSU, active low
constexpr uint32_t kPinMux = PB1;         // MUX FETs, C2: high = off
constexpr uint32_t kPinDirection = PA8;
constexpr uint32_t kPinsEnable[] = {PA5, PA6, PA7, PB0, PA15, PB3};
constexpr uint32_t kPinLed = PC13; // active low

auto &espLink = Serial1; // USART1: PA9 TX / PA10 RX

vdm_rx_t rx;

void safeOutputs()
{
    for (uint32_t pin : kPinsEnable) {
        digitalWrite(pin, LOW);
        pinMode(pin, OUTPUT);
    }
    digitalWrite(kPinDirection, LOW);
    pinMode(kPinDirection, OUTPUT);
    digitalWrite(kPinMux, HIGH);
    pinMode(kPinMux, OUTPUT);
    digitalWrite(kPinPowerEnable, HIGH); // PSU off
    pinMode(kPinPowerEnable, OUTPUT);
}

void sendFrame(uint8_t kind, uint8_t seq, uint8_t msg, const uint8_t *payload, size_t len)
{
    uint8_t wire[VDM_WIRE_MAX];
    size_t n = 0;
    if (vdm_frame_encode(kind, seq, msg, payload, len, wire, sizeof(wire), &n)) {
        espLink.write(wire, n);
    }
}

void respond(const vdm_frame_t &req, uint8_t status, const uint8_t *body = nullptr,
             size_t body_len = 0)
{
    uint8_t payload[VDM_PAYLOAD_MAX];
    size_t n = 0;
    if (vdm_enc_response(status, 0, body, body_len, payload, sizeof(payload), &n)) {
        sendFrame(VDM_KIND_RESPONSE, req.seq, req.msg, payload, n);
    }
}

vdm_hello_t helloInfo()
{
    vdm_hello_t h = {};
    h.proto_version = VDM_PROTO_VERSION;
    h.fw_major = FW_VERSION_MAJOR;
    h.fw_minor = FW_VERSION_MINOR;
    h.fw_patch = FW_VERSION_PATCH;
    h.hw_rev = VDM_HW_REV;
    h.mcu = VDM_MCU;
    const uint32_t uid[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
    memcpy(h.mcu_uid, uid, sizeof(h.mcu_uid));
    h.channel_count = VDM_MAX_CHANNELS;
    h.config_crc = vdm_config_crc(&vdm_system_config_default, &vdm_motor_params_default);
    return h;
}

void handleRequest(const vdm_frame_t &req)
{
    switch (req.msg) {
    case VDM_MSG_HELLO: {
        vdm_hello_req_t hr;
        if (vdm_dec_hello_req(req.payload, req.len, &hr) != VDM_OK) {
            respond(req, VDM_ERR_BAD_LEN);
            return;
        }
        vdm_hello_t h = helloInfo();
        uint8_t body[VDM_HELLO_LEN];
        size_t n = 0;
        vdm_enc_hello(&h, body, sizeof(body), &n);
        respond(req, VDM_OK, body, n);
        return;
    }
    case VDM_MSG_PING:
        if (req.len > VDM_PING_MAX) {
            respond(req, VDM_ERR_BAD_LEN);
        } else {
            respond(req, VDM_OK, req.payload, req.len);
        }
        return;
    default:
        respond(req, VDM_ERR_UNKNOWN_MSG);
        return;
    }
}

} // namespace

void setup()
{
    safeOutputs();
    pinMode(kPinLed, OUTPUT);

    espLink.setRx(PA10);
    espLink.setTx(PA9);
    espLink.begin(115200, SERIAL_8N1);
    vdm_rx_init(&rx);
}

void loop()
{
    while (espLink.available() > 0) {
        vdm_frame_t frame;
        if (vdm_rx_feed(&rx, static_cast<uint8_t>(espLink.read()), &frame) &&
            frame.kind == VDM_KIND_REQUEST) {
            handleRequest(frame);
        }
    }
    digitalWrite(kPinLed, (millis() / 500) % 2 ? HIGH : LOW);
}
