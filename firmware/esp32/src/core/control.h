/*
 * ControlEngine - temperature sources, zones, PI controllers and the flow
 * guard (docs/design/esp32-control-and-integration.md, sections 3 to 6).
 *
 * Hardware independent and single-threaded: the app feeds it with the
 * link model and MQTT values, calls tick() about once per second and sends
 * the effective valve targets to the STM32.
 *
 * Positions: percent for zone outputs, 0.1 % (permille) for valves.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/config.h"
#include "core/model.h"

namespace vdm
{

enum class SourceState : uint8_t {
    Pending, /* no value yet, still within the start-up grace time */
    Ok,
    Stale,   /* no new value for max_age_s */
    Missing, /* never had a value (after the grace time), or not configured */
};
const char *sourceStateName(SourceState s);

struct SourceStatus {
    SourceState state = SourceState::Pending;
    bool has_value = false;
    float value = 0.0f; /* offset applied */
    uint32_t age_s = 0; /* since the last new value */
};

enum class ZoneAction : uint8_t { Off, Idle, Heating, Cooling, Paused, Manual, Fallback, Hold };
const char *zoneActionName(ZoneAction a);

/* what the water delivers to a zone */
enum class Medium : uint8_t { Heat, Cool, None };
const char *mediumName(Medium m);

struct ZoneStatus {
    SourceState source = SourceState::Missing;
    bool has_temperature = false;
    float temperature = 0.0f;
    bool has_output = false; /* false: valves keep their position (start-up) */
    float output = 0.0f;     /* % */
    ZoneAction action = ZoneAction::Hold;
    Medium medium = Medium::Heat;    /* controller in use; None: paused */
    bool has_control_source = false; /* at least one control source configured */
    bool has_supply = false;
    float supply = 0.0f; /* supply temperature used for the zone */
};

struct ValveStatus {
    bool has_target = false;
    uint16_t requested = 0;  /* controller or external command, 0.1 % */
    uint16_t effective = 0;  /* after limits and flow guard, 0.1 % */
    bool controlled = false; /* member of a zone in mode control */
    bool eligible = false;   /* counts for the flow guard */
};

struct FlowStatus {
    bool active = false;       /* targets were raised */
    bool insufficient = false; /* min_total cannot be reached */
    uint16_t total = 0;        /* sum of the effective targets, 0.1 % */
    uint16_t added = 0;        /* raised by, 0.1 % */
};

/* PI controller of one zone, in percent and Kelvin. */
class PiController
{
  public:
    void configure(const PiParams &p)
    {
        p_ = p;
    }
    const PiParams &params() const
    {
        return p_;
    }
    float kp() const
    {
        return 100.0f / p_.xp;
    }
    bool active() const
    {
        return active_;
    }
    float output() const
    {
        return output_;
    }
    float integral() const
    {
        return integral_;
    }
    /* starts bumpless: the output equals `position` as far as possible */
    void start(float error, float position);
    /* one sample; dt in seconds */
    void step(float error, float dt_s);
    void stopController()
    {
        active_ = false;
    }
    void restore(float integral, float output);

  private:
    PiParams p_{1.5f, 900, 60};
    bool active_ = false;
    float integral_ = 0.0f;
    float output_ = 0.0f;
};

/* Controller state kept over an ESP32 restart (RTC RAM). */
struct PiSnapshot {
    char zone_id[kIdSize];
    float integral;
    float output;
    uint8_t medium; /* Medium the controller ran for */
};

class ControlEngine
{
  public:
    static constexpr uint32_t kStartupGraceMs = 120000;
    static constexpr float kOutputDeadband = 1.0f; /* % */
    static constexpr float kHeatingThreshold = 1.0f;
    /* a change of the delivered medium must last this long */
    static constexpr uint32_t kMediumDwellMs = 5UL * 60UL * 1000UL;

    enum class Result : uint8_t { Ok, BadChannel, Controlled, Unknown };

    ControlEngine();

    /* (Re)applies the configuration; state of zones and sources with the
     * same id is kept. `cfg` must stay valid until the next call. */
    void configure(const Config &cfg, uint32_t now_ms);

    /* 1-Wire values and valve states from the link model */
    void updateFromModel(const Model &m, uint32_t now_ms);
    /* base topic for MQTT sources without their own topic: they listen on
     * <base>/source/<id>/set */
    void setMqttBase(const char *base);
    /* topic of MQTT source `i` (its own or the default one) */
    const char *mqttTopic(uint8_t i, char *buf, size_t len) const;

    /* value of an MQTT source; true if the topic belongs to a source */
    bool onMqtt(const char *topic, const char *payload, size_t len, uint32_t now_ms);

    /* external valve position (web, MQTT, KNX), only outside of control */
    Result setManualTarget(uint8_t ch, uint16_t permille);

    void tick(uint32_t now_ms);

    const SourceStatus &source(uint8_t i) const
    {
        return src_[i].status;
    }
    const ZoneStatus &zone(uint8_t i) const
    {
        return zone_[i].status;
    }
    const ValveStatus &valve(uint8_t ch) const
    {
        return valve_[ch];
    }
    const FlowStatus &flow() const
    {
        return flow_;
    }
    /* "Office (MQTT), Return (1-Wire, stale)": the zone's control sources
     * with type and, if not ok, their state */
    void controlSourcesText(uint8_t zi, char *buf, size_t len) const;

    /* status of a source by id, nullptr if unknown */
    const SourceStatus *sourceById(const char *id) const;

    /* RTC snapshot of all active controllers; returns the count */
    uint8_t snapshot(PiSnapshot *out, uint8_t max) const;
    void restore(const PiSnapshot *in, uint8_t count);

  private:
    struct SourceRt {
        char id[kIdSize] = "";
        uint8_t rom[8] = {};
        bool has_value = false;
        float raw = 0.0f;
        uint32_t value_ms = 0; /* ESP time of the last new value */
        SourceStatus status;
    };
    struct ZoneRt {
        char id[kIdSize] = "";
        PiController pi;
        bool has_output = false;
        float applied = 0.0f;
        uint32_t sample_ms = 0;
        ZoneMode last_mode = ZoneMode::Control;
        bool medium_init = false;
        Medium medium = Medium::Heat;    /* delivered medium (with dwell) */
        Medium candidate = Medium::Heat; /* waiting for the dwell time */
        uint32_t candidate_ms = 0;
        Medium delivered = Medium::Heat; /* last Heat/Cool seen */
        Medium pi_medium = Medium::Heat; /* medium the controller runs for */
        ZoneStatus status;
    };

    void evalSource(uint8_t i, uint32_t now_ms);
    void evalZone(uint8_t i, uint32_t now_ms);
    Medium evalMedium(uint8_t i, uint32_t now_ms);
    bool zonePosition(uint8_t zi, float &pct) const;
    void evalValves();
    void flowGuard();
    uint16_t fill(uint16_t deficit, bool priority);

    const Config *cfg_ = nullptr;
    char mqtt_base_[96] = "vdmot";
    uint32_t start_ms_ = 0;
    bool started_ = false;

    SourceRt src_[kMaxSources];
    ZoneRt zone_[kMaxZones];
    ValveStatus valve_[VDM_MAX_CHANNELS];

    /* link model */
    bool model_valid_ = false;
    uint8_t stm_mode_[VDM_MAX_CHANNELS] = {};
    uint16_t stm_target_[VDM_MAX_CHANNELS] = {};

    bool manual_set_[VDM_MAX_CHANNELS] = {};
    uint16_t manual_[VDM_MAX_CHANNELS] = {};
    bool was_controlled_[VDM_MAX_CHANNELS] = {};

    FlowStatus flow_;
};

} // namespace vdm
