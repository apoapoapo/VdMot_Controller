/*
 * Shared state between the link task and the web/network task.
 *
 * The link task owns the LinkClient; it publishes a copy of the model
 * every 100 ms. Other tasks only read that copy and send commands
 * through a queue.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <Arduino.h>

#include "core/model.h"

namespace app
{

enum class CommandType : uint8_t {
    SetTarget,
    SetTargets, /* mask + targets[] */
    Calibrate,
    Detect,
    Stop,
    SetChannels,
    SensorScan,
    SetSystemConfig,
    SetMotorParams,
    ResetStm32,     /* RESET request */
    HardResetStm32, /* NRST pulse */
    FlashStm32,     /* image in LittleFS, see kStm32ImagePath */
    PauseLink,      /* value: seconds, 0 = resume */
};

struct Command {
    CommandType type;
    uint8_t channel;
    uint16_t value;
    uint16_t mask;
    uint16_t targets[VDM_MAX_CHANNELS];
    vdm_system_config_t sys;
    vdm_motor_params_t motor;
};

struct UpdateStatus {
    bool active = false;
    uint8_t percent = 0;
    char phase[24] = "";
    char result[64] = "";
    char detail[256] = "";
};

constexpr const char *kStm32ImagePath = "/stm32.bin";

void sharedInit();
bool sendCommand(const Command &c);
bool receiveCommand(Command &c); /* link task, non-blocking */

void publishModel(const vdm::Model &m, const char *last_error);
/* copies the latest model; returns false before the first publish */
bool readModel(vdm::Model &out, char *last_error, size_t err_len);

void setUpdateStatus(const UpdateStatus &s);
UpdateStatus updateStatus();

/* log of the ESP32 itself (control, configuration), shown with the STM32 log */
constexpr uint8_t kEspLogSize = 16;
void espLog(uint8_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* copies the entries, oldest first; returns the count */
uint8_t espLogCopy(vdm::LogEntry *out, uint8_t max);

} // namespace app
