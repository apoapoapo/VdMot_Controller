/*
 * Controller - application layer of the STM32 firmware.
 *
 * Owns the configuration, connects storage, valve manager and link server,
 * handles all requests, generates events and implements the failsafe.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

#include "core/link_server.h"
#include "core/ports.h"
#include "core/sensor_manager.h"
#include "core/storage.h"
#include "core/valve_manager.h"
#include "vdm/link.h"

namespace vdm
{

struct BoardInfo {
    uint8_t fw_major, fw_minor, fw_patch;
    uint32_t fw_build;
    uint8_t hw_rev;
    uint8_t mcu;
    uint8_t mcu_uid[12];
    uint32_t capabilities;
    uint8_t reset_cause;
};

class Controller : public RequestHandler
{
  public:
    static constexpr uint32_t kBootEventMs = 1000;
    static constexpr uint32_t kValveEventMinMs = 100;
    static constexpr uint32_t kLazyWriteMs = 10UL * 60UL * 1000UL;
    static constexpr uint32_t kHourMs = 60UL * 60UL * 1000UL;

    Controller(const BoardInfo &info, ValveManager &valves, SensorManager &sensors,
               Storage &storage, SystemPort &system, LinkPort &link_port);

    /* Loads the stored configuration and starts operation. */
    void begin(uint32_t now_ms);
    /* Feeds one byte received from the ESP32. */
    void onLinkByte(uint8_t byte, uint32_t now_ms);
    /* Main loop. */
    void tick(uint32_t now_ms);

    size_t handleRequest(uint8_t msg, const uint8_t *payload, size_t len, uint8_t *resp,
                         size_t cap) override;

    const vdm_system_config_t &systemConfig() const
    {
        return sys_;
    }
    const vdm_motor_params_t &motorParams() const
    {
        return motor_;
    }
    bool helloDone() const
    {
        return hello_done_;
    }
    bool failsafeActive() const
    {
        return failsafe_active_;
    }
    LinkServer &link()
    {
        return link_;
    }

  private:
    size_t ok(uint8_t *resp, size_t cap, const uint8_t *body = nullptr, size_t body_len = 0);
    size_t error(uint8_t *resp, size_t cap, uint8_t status, uint8_t detail = 0);
    size_t okConfigCrc(uint8_t *resp, size_t cap);
    vdm_hello_t hello() const;
    void saveSystem();
    void applyConfig();
    void sendEvents(uint32_t now_ms);
    bool encodeSensors(uint8_t offset, uint8_t *buf, size_t cap, size_t *len);
    void persist(uint32_t now_ms);
    void endFailsafe();

    BoardInfo info_;
    ValveManager &valves_;
    SensorManager &sensors_;
    Storage &storage_;
    SystemPort &system_;
    LinkServer link_;

    vdm_system_config_t sys_;
    vdm_motor_params_t motor_;
    Counters counters_{};

    uint32_t now_ms_ = 0;
    bool hello_done_ = false;
    uint32_t last_boot_evt_ms_ = 0;
    bool boot_evt_sent_ = false;
    uint32_t last_request_ms_ = 0;
    bool failsafe_active_ = false;
    bool link_timeout_evt_ = false;
    bool reset_pending_ = false;

    uint16_t evt_pending_ = 0;
    uint32_t last_evt_ms_[VDM_MAX_CHANNELS] = {};

    uint16_t saved_targets_[VDM_MAX_CHANNELS] = {};
    uint16_t pre_failsafe_targets_[VDM_MAX_CHANNELS] = {};
    bool targets_dirty_ = false;
    uint32_t targets_dirty_since_ = 0;
    uint16_t channels_dirty_ = 0;
    uint32_t channels_dirty_since_ = 0;
    uint32_t hour_acc_ms_ = 0;
    uint32_t last_tick_ms_ = 0;
};

} // namespace vdm
