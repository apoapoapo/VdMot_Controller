/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/control.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace vdm
{

namespace
{

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool eligibleMode(uint8_t mode)
{
    switch (mode) {
    case VDM_MODE_IDLE:
    case VDM_MODE_OPENING:
    case VDM_MODE_CLOSING:
    case VDM_MODE_CALIBRATING:
    case VDM_MODE_QUEUED:
        return true;
    default:
        return false;
    }
}

bool isTemperatureKind(uint8_t kind)
{
    return kind == VDM_SENSOR_TEMP_DS18B20 || kind == VDM_SENSOR_TEMP_DS2438;
}

/* a plain number, surrounding white space allowed */
bool parseNumber(const char *s, size_t len, float &out)
{
    char buf[32];
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    char *end = nullptr;
    double d = strtod(buf, &end);
    if (end == buf) {
        return false;
    }
    while (*end != '\0' && isspace(static_cast<unsigned char>(*end))) {
        end++;
    }
    if (*end != '\0' || !isfinite(d)) {
        return false;
    }
    out = static_cast<float>(d);
    return true;
}

/* field of a JSON payload, "a.b" for nested objects */
bool parseJsonField(const char *payload, size_t len, const char *field, float &out)
{
    static DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, payload, len)) {
        return false;
    }
    JsonVariantConst v = doc.as<JsonVariantConst>();
    char key[32];
    const char *p = field;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t n = dot ? static_cast<size_t>(dot - p) : strlen(p);
        if (n == 0 || n >= sizeof(key)) {
            return false;
        }
        memcpy(key, p, n);
        key[n] = '\0';
        v = v[key];
        p += n + (dot ? 1 : 0);
    }
    if (v.is<float>() || v.is<long>()) {
        out = v.as<float>();
        return isfinite(out);
    }
    const char *s = v.as<const char *>();
    return s != nullptr && parseNumber(s, strlen(s), out);
}

} // namespace

const char *sourceStateName(SourceState s)
{
    switch (s) {
    case SourceState::Pending:
        return "pending";
    case SourceState::Ok:
        return "ok";
    case SourceState::Stale:
        return "stale";
    case SourceState::Missing:
        return "missing";
    }
    return "?";
}

const char *zoneActionName(ZoneAction a)
{
    switch (a) {
    case ZoneAction::Off:
        return "off";
    case ZoneAction::Idle:
        return "idle";
    case ZoneAction::Heating:
        return "heating";
    case ZoneAction::Manual:
        return "manual";
    case ZoneAction::Fallback:
        return "fallback";
    case ZoneAction::Hold:
        return "hold";
    }
    return "?";
}

/* ---------------------------------------------------------------------- */
/* PI controller                                                           */
/* ---------------------------------------------------------------------- */

void PiController::start(float error, float position)
{
    float p = kp() * error;
    integral_ = clampf(position - p, 0.0f, 100.0f);
    output_ = clampf(p + integral_, 0.0f, 100.0f);
    active_ = true;
}

void PiController::step(float error, float dt_s)
{
    float p = kp() * error;
    if (p_.ti_s > 0) {
        float i_new = integral_ + p * dt_s / static_cast<float>(p_.ti_s);
        float u = p + i_new;
        /* conditional integration: not while the output is saturated in
         * the direction of the error */
        bool saturated = (u > 100.0f && error > 0.0f) || (u < 0.0f && error < 0.0f);
        if (!saturated) {
            integral_ = clampf(i_new, 0.0f, 100.0f);
        }
    }
    output_ = clampf(p + integral_, 0.0f, 100.0f);
}

void PiController::restore(float integral, float output)
{
    integral_ = clampf(integral, 0.0f, 100.0f);
    output_ = clampf(output, 0.0f, 100.0f);
    active_ = true;
}

/* ---------------------------------------------------------------------- */
/* Engine                                                                  */
/* ---------------------------------------------------------------------- */

ControlEngine::ControlEngine() = default;

void ControlEngine::configure(const Config &cfg, uint32_t now_ms)
{
    if (!started_) {
        started_ = true;
        start_ms_ = now_ms;
    }
    cfg_ = &cfg;

    /* keep runtime state by id */
    static SourceRt old_src[kMaxSources];
    static ZoneRt old_zone[kMaxZones];
    for (uint8_t i = 0; i < kMaxSources; i++) {
        old_src[i] = src_[i];
    }
    for (uint8_t i = 0; i < kMaxZones; i++) {
        old_zone[i] = zone_[i];
    }

    for (uint8_t i = 0; i < kMaxSources; i++) {
        src_[i] = SourceRt();
        if (i >= cfg.source_count) {
            continue;
        }
        const SourceConfig &sc = cfg.sources[i];
        for (const SourceRt &o : old_src) {
            if (o.id[0] != '\0' && strcmp(o.id, sc.id) == 0) {
                src_[i] = o;
                break;
            }
        }
        strncpy(src_[i].id, sc.id, kIdSize - 1);
        if (sc.type == SourceType::OneWire) {
            uint8_t rom[8];
            if (parseRom(sc.rom, rom)) {
                if (memcmp(rom, src_[i].rom, 8) != 0) {
                    src_[i].has_value = false; /* another sensor */
                }
                memcpy(src_[i].rom, rom, 8);
            }
        }
    }

    for (uint8_t i = 0; i < kMaxZones; i++) {
        zone_[i] = ZoneRt();
        if (i >= cfg.zone_count) {
            continue;
        }
        const ZoneConfig &zc = cfg.zones[i];
        bool found = false;
        for (const ZoneRt &o : old_zone) {
            if (o.id[0] != '\0' && strcmp(o.id, zc.id) == 0) {
                zone_[i] = o;
                found = true;
                break;
            }
        }
        if (!found) {
            zone_[i].last_mode = zc.mode;
        }
        strncpy(zone_[i].id, zc.id, kIdSize - 1);
        zone_[i].pi.configure(piParams(zc));
    }
}

void ControlEngine::updateFromModel(const Model &m, uint32_t now_ms)
{
    (void)now_ms;
    model_valid_ = m.valves_valid;
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        stm_mode_[ch] = m.valves[ch].mode;
        stm_target_[ch] = m.valves[ch].target;
    }
    if (cfg_ == nullptr) {
        return;
    }
    for (uint8_t i = 0; i < cfg_->source_count; i++) {
        if (cfg_->sources[i].type != SourceType::OneWire) {
            continue;
        }
        SourceRt &s = src_[i];
        for (uint8_t k = 0; k < m.sensor_total && k < Model::kMaxSensors; k++) {
            const vdm_sensor_value_t &v = m.sensors[k];
            if (!isTemperatureKind(v.kind) || memcmp(v.rom, s.rom, 8) != 0 || v.age_s == 0xFFFF) {
                continue;
            }
            /* time of the last successful read, as seen at the fetch */
            uint32_t at = m.sensors_ms - static_cast<uint32_t>(v.age_s) * 1000u;
            if (!s.has_value || static_cast<int32_t>(at - s.value_ms) > 0) {
                s.value_ms = at;
            }
            s.raw = static_cast<float>(v.value) / 1000.0f;
            s.has_value = true;
            break;
        }
    }
}

bool ControlEngine::onMqtt(const char *topic, const char *payload, size_t len, uint32_t now_ms)
{
    if (cfg_ == nullptr) {
        return false;
    }
    bool matched = false;
    for (uint8_t i = 0; i < cfg_->source_count; i++) {
        const SourceConfig &sc = cfg_->sources[i];
        if (sc.type != SourceType::Mqtt || strcmp(sc.topic, topic) != 0) {
            continue;
        }
        matched = true;
        float v;
        bool ok = sc.field[0] == '\0' ? parseNumber(payload, len, v)
                                      : parseJsonField(payload, len, sc.field, v);
        if (ok && v > -55.0f && v < 125.0f) {
            src_[i].raw = v;
            src_[i].has_value = true;
            src_[i].value_ms = now_ms;
        }
    }
    return matched;
}

ControlEngine::Result ControlEngine::setManualTarget(uint8_t ch, uint16_t permille)
{
    if (ch >= VDM_MAX_CHANNELS || permille > 1000) {
        return Result::BadChannel;
    }
    if (cfg_ != nullptr) {
        int zi = cfg_->zoneIndex(cfg_->valves[ch].zone);
        if (zi >= 0 && cfg_->zones[zi].mode == ZoneMode::Control) {
            return Result::Controlled;
        }
    }
    manual_[ch] = permille;
    manual_set_[ch] = true;
    return Result::Ok;
}

const SourceStatus *ControlEngine::sourceById(const char *id) const
{
    if (cfg_ == nullptr) {
        return nullptr;
    }
    int i = cfg_->sourceIndex(id);
    return i < 0 ? nullptr : &src_[i].status;
}

void ControlEngine::evalSource(uint8_t i, uint32_t now_ms)
{
    const SourceConfig &sc = cfg_->sources[i];
    SourceRt &s = src_[i];
    SourceStatus &st = s.status;
    st.has_value = s.has_value;
    if (!s.has_value) {
        st.state =
            now_ms - start_ms_ < kStartupGraceMs ? SourceState::Pending : SourceState::Missing;
        st.age_s = 0;
        return;
    }
    int32_t age_ms = static_cast<int32_t>(now_ms - s.value_ms);
    st.age_s = age_ms > 0 ? static_cast<uint32_t>(age_ms) / 1000u : 0;
    st.value = s.raw + sc.offset;
    st.state =
        (sc.max_age_s != 0 && st.age_s > sc.max_age_s) ? SourceState::Stale : SourceState::Ok;
}

bool ControlEngine::zonePosition(uint8_t zi, float &pct) const
{
    float sum = 0.0f;
    int n = 0;
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        const ValveConfig &v = cfg_->valves[ch];
        if (strcmp(v.zone, cfg_->zones[zi].id) != 0) {
            continue;
        }
        uint16_t t;
        if (valve_[ch].has_target) {
            t = valve_[ch].effective;
        } else if (model_valid_ && stm_target_[ch] <= 1000) {
            t = stm_target_[ch];
        } else {
            return false;
        }
        sum += static_cast<float>(t) / 10.0f * 100.0f / static_cast<float>(v.factor_pct);
        n++;
    }
    pct = n > 0 ? clampf(sum / static_cast<float>(n), 0.0f, 100.0f) : 0.0f;
    return true;
}

void ControlEngine::evalZone(uint8_t i, uint32_t now_ms)
{
    const ZoneConfig &zc = cfg_->zones[i];
    ZoneRt &z = zone_[i];
    ZoneStatus &st = z.status;

    const SourceStatus *src = sourceById(zc.control_source);
    st.source = src ? src->state : SourceState::Missing;
    st.has_temperature = src && src->has_value;
    st.temperature = st.has_temperature ? src->value : 0.0f;
    z.last_mode = zc.mode;

    if (zc.mode == ZoneMode::Manual) {
        z.pi.stopController();
        z.has_output = false;
        st.has_output = false;
        st.action = ZoneAction::Manual;
        return;
    }
    if (!zc.heating) {
        z.pi.stopController();
        z.has_output = true;
        z.applied = 0.0f;
        st.has_output = true;
        st.output = 0.0f;
        st.action = ZoneAction::Off;
        return;
    }

    switch (st.source) {
    case SourceState::Ok: {
        float e = zc.setpoint - st.temperature;
        PiParams p = z.pi.params();
        if (!z.pi.active()) {
            float pos;
            if (!zonePosition(i, pos)) {
                break; /* valve positions not known yet: hold */
            }
            z.pi.start(e, pos);
            z.sample_ms = now_ms;
        } else if (now_ms - z.sample_ms >= p.sample_s * 1000u) {
            uint32_t dt = now_ms - z.sample_ms;
            if (dt > 2u * p.sample_s * 1000u) {
                dt = 2u * p.sample_s * 1000u; /* after a pause */
            }
            z.pi.step(e, static_cast<float>(dt) / 1000.0f);
            z.sample_ms = now_ms;
        }
        float u = z.pi.output();
        bool to_limit = (u <= 0.0f && z.applied > 0.0f) || (u >= 100.0f && z.applied < 100.0f);
        if (!z.has_output || fabsf(u - z.applied) >= kOutputDeadband || to_limit) {
            z.applied = u;
        }
        z.has_output = true;
        st.has_output = true;
        st.output = z.applied;
        st.action = z.applied >= kHeatingThreshold ? ZoneAction::Heating : ZoneAction::Idle;
        return;
    }
    case SourceState::Pending:
        if (z.pi.active()) { /* restored after a restart */
            z.has_output = true;
            st.has_output = true;
            st.output = z.applied;
            st.action = z.applied >= kHeatingThreshold ? ZoneAction::Heating : ZoneAction::Idle;
            return;
        }
        break;
    case SourceState::Stale:
    case SourceState::Missing: {
        /* the controller keeps its state and continues when values return */
        int fb = zc.fallback_pct >= 0 ? zc.fallback_pct : cfg_->flow.fallback_pct;
        z.has_output = true;
        z.applied = static_cast<float>(fb);
        st.has_output = true;
        st.output = z.applied;
        st.action = ZoneAction::Fallback;
        return;
    }
    }
    z.has_output = false;
    st.has_output = false;
    st.action = ZoneAction::Hold;
}

void ControlEngine::evalValves()
{
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        const ValveConfig &v = cfg_->valves[ch];
        ValveStatus &s = valve_[ch];
        int zi = cfg_->zoneIndex(v.zone);
        bool controlled = zi >= 0 && cfg_->zones[zi].mode == ZoneMode::Control;
        bool stm_known = model_valid_ && stm_target_[ch] <= 1000;

        /* leaving control: the valve stays where it is */
        if (was_controlled_[ch] && !controlled && s.has_target) {
            manual_[ch] = s.effective;
            manual_set_[ch] = true;
        }
        was_controlled_[ch] = controlled;
        s.controlled = controlled;

        bool has = false;
        int32_t req = 0;
        if (controlled) {
            const ZoneStatus &z = zone_[zi].status;
            if (z.has_output) {
                float pos = z.action == ZoneAction::Fallback
                                ? z.output
                                : z.output * static_cast<float>(v.factor_pct) / 100.0f;
                req = static_cast<int32_t>(lroundf(pos * 10.0f));
                has = true;
            } else if (stm_known) {
                req = stm_target_[ch];
                has = true;
            }
        } else {
            if (!manual_set_[ch] && stm_known) {
                manual_[ch] = stm_target_[ch];
                manual_set_[ch] = true;
            }
            if (manual_set_[ch]) {
                req = manual_[ch];
                has = true;
            }
        }
        if (model_valid_ && stm_mode_[ch] == VDM_MODE_DISABLED) {
            has = false; /* the STM32 rejects targets for disabled channels */
        }
        req = req < 0 ? 0 : (req > 1000 ? 1000 : req);
        int32_t lo = v.min_pct * 10, hi = v.max_pct * 10;
        s.has_target = has;
        s.requested = static_cast<uint16_t>(req);
        s.effective = has ? static_cast<uint16_t>(req < lo ? lo : (req > hi ? hi : req)) : 0;
        s.eligible = has && (!model_valid_ || eligibleMode(stm_mode_[ch]));
    }
}

uint16_t ControlEngine::fill(uint16_t deficit, bool priority)
{
    while (deficit > 0) {
        uint16_t n = 0;
        for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
            const ValveStatus &s = valve_[ch];
            if (s.eligible && cfg_->valves[ch].flow_priority == priority &&
                s.effective < cfg_->valves[ch].max_pct * 10) {
                n++;
            }
        }
        if (n == 0) {
            break;
        }
        uint16_t share = deficit / n > 0 ? static_cast<uint16_t>(deficit / n) : 1;
        for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS && deficit > 0; ch++) {
            ValveStatus &s = valve_[ch];
            uint16_t hi = static_cast<uint16_t>(cfg_->valves[ch].max_pct * 10);
            if (!s.eligible || cfg_->valves[ch].flow_priority != priority || s.effective >= hi) {
                continue;
            }
            uint16_t give = share;
            if (give > hi - s.effective) {
                give = static_cast<uint16_t>(hi - s.effective);
            }
            if (give > deficit) {
                give = deficit;
            }
            s.effective = static_cast<uint16_t>(s.effective + give);
            deficit = static_cast<uint16_t>(deficit - give);
        }
    }
    return deficit;
}

void ControlEngine::flowGuard()
{
    flow_ = FlowStatus();
    uint32_t total = 0;
    for (const ValveStatus &s : valve_) {
        if (s.eligible) {
            total += s.effective;
        }
    }
    uint32_t need = static_cast<uint32_t>(cfg_->flow.min_total_pct) * 10u;
    if (need > 0 && total < need) {
        uint16_t deficit = static_cast<uint16_t>(need - total);
        uint16_t left = fill(deficit, true);
        left = fill(left, false);
        flow_.added = static_cast<uint16_t>(deficit - left);
        flow_.active = flow_.added > 0;
        flow_.insufficient = left > 0;
        total += flow_.added;
    }
    flow_.total = static_cast<uint16_t>(total);
}

void ControlEngine::tick(uint32_t now_ms)
{
    if (cfg_ == nullptr) {
        return;
    }
    for (uint8_t i = 0; i < cfg_->source_count; i++) {
        evalSource(i, now_ms);
    }
    for (uint8_t i = 0; i < cfg_->zone_count; i++) {
        evalZone(i, now_ms);
    }
    evalValves();
    flowGuard();
}

uint8_t ControlEngine::snapshot(PiSnapshot *out, uint8_t max) const
{
    uint8_t n = 0;
    if (cfg_ == nullptr) {
        return 0;
    }
    for (uint8_t i = 0; i < cfg_->zone_count && n < max; i++) {
        const ZoneRt &z = zone_[i];
        if (!z.pi.active()) {
            continue;
        }
        PiSnapshot &s = out[n++];
        memset(&s, 0, sizeof(s));
        strncpy(s.zone_id, z.id, kIdSize - 1);
        s.integral = z.pi.integral();
        s.output = z.applied;
    }
    return n;
}

void ControlEngine::restore(const PiSnapshot *in, uint8_t count)
{
    if (cfg_ == nullptr) {
        return;
    }
    for (uint8_t k = 0; k < count; k++) {
        int i = cfg_->zoneIndex(in[k].zone_id);
        if (i < 0 || cfg_->zones[i].mode != ZoneMode::Control || !cfg_->zones[i].heating) {
            continue;
        }
        ZoneRt &z = zone_[i];
        z.pi.restore(in[k].integral, in[k].output);
        z.applied = z.pi.output();
        z.has_output = true;
        z.sample_ms = start_ms_;
    }
}

} // namespace vdm
