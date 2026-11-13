/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "app/control.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <string.h>

#include "app/settings.h"
#include "app/shared.h"
#include "vdm/crc16.h"

namespace app
{

namespace
{

constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kFailsafeSyncMs = 30000;
constexpr uint32_t kRtcMagic = 0x56444d32; /* "VDM2" */

struct RtcState {
    uint32_t magic;
    uint8_t count;
    vdm::PiSnapshot snaps[vdm::kMaxZones];
    uint16_t crc;
};

RTC_NOINIT_ATTR RtcState g_rtc;

vdm::ControlEngine g_engine;
vdm::Model &g_model = *new vdm::Model(); /* copy of the link model, large: heap */
uint32_t g_last_tick = 0;
bool g_kick = true;
uint32_t g_last_failsafe_sync = 0;
uint16_t g_sent[VDM_MAX_CHANNELS] = {};
uint16_t g_sent_valid = 0;
vdm::ZoneAction g_last_action[vdm::kMaxZones] = {};
bool g_last_insufficient = false;

uint16_t rtcCrc()
{
    return vdm_crc16(reinterpret_cast<const uint8_t *>(&g_rtc), offsetof(RtcState, crc));
}

void saveRtc()
{
    RtcState s;
    memset(&s, 0, sizeof(s));
    s.magic = kRtcMagic;
    s.count = g_engine.snapshot(s.snaps, vdm::kMaxZones);
    g_rtc = s;
    g_rtc.crc = rtcCrc();
}

void restoreRtc()
{
    if (g_rtc.magic != kRtcMagic || g_rtc.count > vdm::kMaxZones || g_rtc.crc != rtcCrc()) {
        return;
    }
    for (uint8_t i = 0; i < g_rtc.count; i++) {
        g_rtc.snaps[i].zone_id[vdm::kIdSize - 1] = '\0';
    }
    g_engine.restore(g_rtc.snaps, g_rtc.count);
    espLog(VDM_LOG_INFO, "controller state of %u zone(s) restored after the restart", g_rtc.count);
}

void logChanges()
{
    const vdm::Config &c = config();
    for (uint8_t i = 0; i < c.zone_count; i++) {
        vdm::ZoneAction a = g_engine.zone(i).action;
        vdm::ZoneAction was = g_last_action[i];
        g_last_action[i] = a;
        if (a == was) {
            continue;
        }
        if (a == vdm::ZoneAction::Fallback) {
            espLog(VDM_LOG_WARN, "zone %s: temperature %s, fallback position", c.zones[i].name,
                   vdm::sourceStateName(g_engine.zone(i).source));
        } else if (was == vdm::ZoneAction::Fallback) {
            espLog(VDM_LOG_INFO, "zone %s: temperature available again", c.zones[i].name);
        }
    }
    bool insufficient = g_engine.flow().insufficient;
    if (insufficient != g_last_insufficient) {
        g_last_insufficient = insufficient;
        espLog(insufficient ? VDM_LOG_WARN : VDM_LOG_INFO,
               insufficient ? "flow guard: minimum total opening cannot be reached"
                            : "flow guard: minimum total opening reached again");
    }
}

void sendTargets()
{
    Command cmd{};
    cmd.type = CommandType::SetTargets;
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        const vdm::ValveStatus &v = g_engine.valve(ch);
        uint16_t bit = static_cast<uint16_t>(1u << ch);
        if (v.has_target && (!(g_sent_valid & bit) || g_sent[ch] != v.effective)) {
            cmd.mask |= bit;
            cmd.targets[ch] = v.effective;
        }
    }
    if (cmd.mask == 0 || !sendCommand(cmd)) {
        return;
    }
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        if ((cmd.mask >> ch) & 1u) {
            g_sent[ch] = cmd.targets[ch];
        }
    }
    g_sent_valid |= cmd.mask;
}

/* the STM32 uses the same fallback position when the ESP32 is gone */
void syncFailsafe(uint32_t now)
{
    uint16_t want = static_cast<uint16_t>(config().flow.fallback_pct * 10u);
    if (!g_model.sys_valid || g_model.state != vdm::LinkState::Connected ||
        g_model.sys.failsafe_pos == want || now - g_last_failsafe_sync < kFailsafeSyncMs) {
        return;
    }
    g_last_failsafe_sync = now;
    Command cmd{};
    cmd.type = CommandType::SetSystemConfig;
    cmd.sys = g_model.sys;
    cmd.sys.failsafe_pos = want;
    if (sendCommand(cmd)) {
        espLog(VDM_LOG_INFO, "STM32 failsafe position set to the fallback position %u %%",
               config().flow.fallback_pct);
    }
}

void evaluate(uint32_t now)
{
    char err[8];
    readModel(g_model, err, sizeof(err));
    g_engine.updateFromModel(g_model, now);
    g_engine.tick(now);
}

} // namespace

void controlBegin()
{
    g_engine.configure(config(), millis());
    restoreRtc();
}

void controlReconfigure()
{
    g_engine.configure(config(), millis());
    g_kick = true;
}

void controlKick()
{
    g_kick = true;
}

void controlLoop()
{
    uint32_t now = millis();
    if (!g_kick && now - g_last_tick < kTickMs) {
        return;
    }
    g_kick = false;
    g_last_tick = now;
    evaluate(now);
    sendTargets();
    syncFailsafe(now);
    logChanges();
    saveRtc();
}

const vdm::ControlEngine &engine()
{
    return g_engine;
}

vdm::ControlEngine::Result controlSetTarget(uint8_t ch, uint16_t permille)
{
    vdm::ControlEngine::Result r = g_engine.setManualTarget(ch, permille);
    if (r == vdm::ControlEngine::Result::Ok) {
        g_kick = true;
    }
    return r;
}

} // namespace app
