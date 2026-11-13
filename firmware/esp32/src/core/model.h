/*
 * Model - everything the ESP32 knows about the STM32 side.
 * Owned by the link task; other tasks work on copies (see app/shared.h).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

#include "vdm/link.h"

namespace vdm
{

enum class LinkState : uint8_t {
    Disconnected, /* no handshake yet, HELLO is sent periodically */
    Connected,
    Incompatible, /* STM32 speaks another protocol version */
    Updating,     /* STM32 firmware update in progress */
};

const char *linkStateName(LinkState s);

struct LinkStats {
    uint32_t requests = 0;
    uint32_t retries = 0;
    uint32_t timeouts = 0; /* requests given up after all retries */
    uint32_t link_ups = 0;
    uint32_t stm_reboots = 0; /* EVT_BOOT while connected */
    uint32_t events = 0;
    uint32_t evt_lost = 0;
    uint32_t hard_resets = 0;
};

struct LogEntry {
    uint32_t ms;
    uint8_t level; /* vdm_log_level_t */
    char text[VDM_LOG_TEXT_MAX + 1];
};

struct Model {
    static constexpr uint8_t kMaxSensors = 96;
    static constexpr uint8_t kLogSize = 16;

    LinkState state = LinkState::Disconnected;
    uint32_t state_since_ms = 0;
    LinkStats stats;
    uint32_t pause_left_s = 0; /* link paused for a failsafe test */

    bool hello_valid = false;
    vdm_hello_t hello{};

    bool valves_valid = false;
    vdm_valve_state_t valves[VDM_MAX_CHANNELS]{};
    bool detail_valid[VDM_MAX_CHANNELS]{};
    vdm_valve_detail_t details[VDM_MAX_CHANNELS]{};
    bool calibration_valid[VDM_MAX_CHANNELS]{};
    vdm_evt_calibration_t calibration[VDM_MAX_CHANNELS]{};

    uint8_t sensor_total = 0;
    vdm_sensor_value_t sensors[kMaxSensors]{};
    uint32_t sensors_ms = 0;
    bool scan_valid = false;
    vdm_evt_scan_done_t scan{};

    bool sys_valid = false;
    vdm_system_config_t sys{};
    bool motor_valid = false;
    vdm_motor_params_t motor{};
    bool diag_valid = false;
    vdm_diag_t diag{};

    LogEntry log[kLogSize]{};
    uint8_t log_head = 0;
    uint8_t log_count = 0;

    void addLog(uint32_t ms, uint8_t level, const char *text, size_t len);
};

} // namespace vdm
