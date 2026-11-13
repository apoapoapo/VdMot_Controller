/*
 * VdMot Controller v2 - ESP32 (WT32-ETH01) firmware
 *
 * Phase 0 skeleton: link bring-up. Sends HELLO to the STM32 once per second
 * and prints the answer on the debug serial port. The real firmware is
 * built in phase 2.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <Arduino.h>

#include "vdm/codec.h"
#include "vdm/frame.h"

namespace
{

constexpr int kPinStmRx = 5;
constexpr int kPinStmTx = 17;
constexpr int kPinStmReset = 15; // high = STM32 held in reset
constexpr int kPinStmBoot0 = 14; // high = ROM bootloader

HardwareSerial &stmLink = Serial2;
vdm_rx_t rx;
uint8_t nextSeq = 1;
uint32_t lastHello = 0;

uint8_t takeSeq()
{
    uint8_t s = nextSeq++;
    if (nextSeq == 0) {
        nextSeq = 1;
    }
    return s;
}

void sendHello()
{
    vdm_hello_req_t req = {VDM_PROTO_VERSION, VDM_HELLO_FLAG_RESET_SEQ};
    uint8_t payload[VDM_HELLO_REQ_LEN];
    uint8_t wire[VDM_WIRE_MAX];
    size_t pn = 0, wn = 0;
    if (vdm_enc_hello_req(&req, payload, sizeof(payload), &pn) &&
        vdm_frame_encode(VDM_KIND_REQUEST, takeSeq(), VDM_MSG_HELLO, payload, pn, wire,
                         sizeof(wire), &wn)) {
        stmLink.write(wire, wn);
    }
}

void handleFrame(const vdm_frame_t &f)
{
    if (f.kind != VDM_KIND_RESPONSE || f.msg != VDM_MSG_HELLO) {
        Serial.printf("link: kind=%u msg=0x%02X len=%u\n", f.kind, f.msg, (unsigned)f.len);
        return;
    }
    uint8_t status, detail;
    const uint8_t *body;
    size_t body_len;
    vdm_hello_t h;
    if (vdm_dec_response(f.payload, f.len, &status, &detail, &body, &body_len) != VDM_OK ||
        status != VDM_OK || vdm_dec_hello(body, body_len, &h) != VDM_OK) {
        Serial.println("link: invalid HELLO response");
        return;
    }
    Serial.printf("link: STM32 fw %u.%u.%u proto %u hw %u mcu %u channels %u\n", h.fw_major,
                  h.fw_minor, h.fw_patch, h.proto_version, h.hw_rev, h.mcu, h.channel_count);
}

} // namespace

void setup()
{
    Serial.begin(115200);

    pinMode(kPinStmBoot0, OUTPUT);
    digitalWrite(kPinStmBoot0, LOW);
    pinMode(kPinStmReset, OUTPUT);
    digitalWrite(kPinStmReset, LOW); // release reset

    stmLink.begin(115200, SERIAL_8N1, kPinStmRx, kPinStmTx);
    vdm_rx_init(&rx);
    Serial.println("VdMot v2 skeleton: link bring-up");
}

void loop()
{
    while (stmLink.available() > 0) {
        vdm_frame_t frame;
        if (vdm_rx_feed(&rx, static_cast<uint8_t>(stmLink.read()), &frame)) {
            handleFrame(frame);
        }
    }
    if (millis() - lastHello >= 1000) {
        lastHello = millis();
        sendHello();
    }
}
