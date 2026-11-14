/*
 * Host tests: temperature sources, zones, PI controller and flow guard,
 * including closed-loop runs against a simple room model.
 */
#include <math.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "core/config_store.h"
#include "core/control.h"

using vdm::ControlEngine;
using vdm::SourceState;
using vdm::ZoneAction;

namespace
{

vdm::Config cfg;
vdm::Model model;
ControlEngine *eng = nullptr;
uint32_t now = 0;
char err[128];

const uint8_t kRom1[8] = {0x28, 0xFF, 1, 2, 3, 4, 5, 6};

void load(const char *json)
{
    TEST_ASSERT_TRUE_MESSAGE(vdm::ConfigStore::fromText(json, cfg, err, sizeof(err)), err);
}

/* all channels connected and idle, targets as the STM32 reports them */
void stmReady(uint16_t target = 0)
{
    model = vdm::Model();
    model.valves_valid = true;
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        model.valves[ch].channel = ch;
        model.valves[ch].mode = VDM_MODE_IDLE;
        model.valves[ch].target = target;
    }
}

/* the STM32 follows the effective targets */
void follow()
{
    for (uint8_t ch = 0; ch < VDM_MAX_CHANNELS; ch++) {
        if (eng->valve(ch).has_target) {
            model.valves[ch].target = eng->valve(ch).effective;
        }
    }
}

void mqtt(const char *topic, const char *payload)
{
    eng->onMqtt(topic, payload, strlen(payload), now);
}

void mqttTemp(float t)
{
    char b[16];
    snprintf(b, sizeof(b), "%.2f", static_cast<double>(t));
    mqtt("room/t", b);
}

void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 1000) {
        now += 1000;
        eng->updateFromModel(model, now);
        eng->tick(now);
        follow();
    }
}

void start(const char *json)
{
    load(json);
    static ControlEngine e;
    e = ControlEngine();
    eng = &e;
    now = 1000;
    eng->configure(cfg, now);
}

const char *kOneRoom = R"({
  "sources": [{"id": "s1", "name": "Room", "type": "mqtt", "topic": "room/t", "max_age_s": 600}],
  "zones": [{"id": "z1", "name": "Room", "setpoint": 21, "control_source": "s1"}],
  "valves": [{"zone": "z1"}],
  "next_source_id": 2, "next_zone_id": 2
})";

/* room: steady temperature 35 °C at 100 % (outside 0 °C), time constant 3 h */
struct Room {
    float t;
    void step(float valve_pct, float dt)
    {
        t += (35.0f * valve_pct / 100.0f - t) * dt / 10800.0f;
    }
};

} // namespace

void setUp()
{
    stmReady();
}
void tearDown() {}

/* --- PI ------------------------------------------------------------------ */

void test_pi_bumpless_and_integral()
{
    vdm::PiController pi;
    pi.configure({2.0f, 1000, 60}); /* kp 50 %/K */
    pi.start(0.5f, 40.0f);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, pi.output());
    TEST_ASSERT_EQUAL_FLOAT(15.0f, pi.integral());
    pi.step(0.5f, 100.0f); /* +25 * 100/1000 = +2.5 */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 17.5f, pi.integral());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.5f, pi.output());
}

void test_pi_anti_windup()
{
    vdm::PiController pi;
    pi.configure({1.0f, 600, 60}); /* kp 100 %/K */
    pi.start(3.0f, 100.0f);        /* far too cold, fully open */
    float i0 = pi.integral();
    for (int k = 0; k < 100; k++) {
        pi.step(3.0f, 60.0f);
    }
    TEST_ASSERT_EQUAL_FLOAT(100.0f, pi.output());
    TEST_ASSERT_EQUAL_FLOAT(i0, pi.integral()); /* no wind-up while saturated */
    pi.step(-0.2f, 60.0f);                      /* overshoot: reacts at once */
    TEST_ASSERT_TRUE(pi.output() < 100.0f);
}

/* --- closed loop --------------------------------------------------------- */

void test_closed_loop_reaches_setpoint()
{
    start(kOneRoom);
    Room room{17.0f};
    float max_t = 0.0f;
    for (int s = 0; s < 24 * 3600; s++) {
        if (s % 30 == 0) {
            mqttTemp(room.t);
        }
        run(1000);
        room.step(eng->valve(0).effective / 10.0f, 1.0f);
        if (s > 4 * 3600 && room.t > max_t) {
            max_t = room.t;
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(0.15f, 21.0f, room.t);
    TEST_ASSERT_TRUE(max_t < 21.5f);
    /* steady state: 21/35 = 60 % */
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 60.0f, eng->zone(0).output);
    TEST_ASSERT_EQUAL(ZoneAction::Heating, eng->zone(0).action);
}

void test_floor_profile_closed_loop()
{
    std::string j = kOneRoom;
    j.replace(j.find("\"control_source\""), 0, "\"profile\":\"floor\",");
    start(j.c_str());
    Room room{18.0f};
    for (int s = 0; s < 36 * 3600; s++) {
        if (s % 60 == 0) {
            mqttTemp(room.t);
        }
        run(1000);
        room.step(eng->valve(0).effective / 10.0f, 1.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 21.0f, room.t);
}

void test_output_deadband()
{
    start(kOneRoom);
    stmReady(500);
    mqttTemp(21.0f);
    run(2000);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, eng->zone(0).output); /* bumpless from 50 % */
    mqttTemp(20.99f);                                            /* +0.67 %: below the dead band */
    run(61000);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, eng->zone(0).output);
    mqttTemp(20.9f);
    run(61000);
    TEST_ASSERT_TRUE(eng->zone(0).output > 56.0f);
}

/* --- sources ------------------------------------------------------------- */

void test_startup_hold_then_fallback()
{
    start(kOneRoom);
    stmReady(300);
    run(10000);
    TEST_ASSERT_EQUAL(SourceState::Pending, eng->source(0).state);
    TEST_ASSERT_EQUAL(ZoneAction::Hold, eng->zone(0).action);
    TEST_ASSERT_TRUE(eng->valve(0).has_target);
    TEST_ASSERT_EQUAL(300, eng->valve(0).effective); /* keeps the STM32 target */

    run(ControlEngine::kStartupGraceMs);
    TEST_ASSERT_EQUAL(SourceState::Missing, eng->source(0).state);
    TEST_ASSERT_EQUAL(ZoneAction::Fallback, eng->zone(0).action);
    TEST_ASSERT_EQUAL(1000, eng->valve(0).effective); /* global fallback 100 % */
}

void test_hold_without_link_model()
{
    start(kOneRoom);
    model = vdm::Model(); /* STM32 not connected yet */
    mqttTemp(20.0f);
    run(3000);
    TEST_ASSERT_EQUAL(ZoneAction::Hold, eng->zone(0).action);
    TEST_ASSERT_FALSE(eng->valve(0).has_target);
    stmReady(200);
    run(1000);
    TEST_ASSERT_EQUAL(ZoneAction::Heating, eng->zone(0).action);
}

void test_stale_source_and_recovery()
{
    std::string j = kOneRoom;
    j.replace(j.find("\"control_source\""), 0, "\"fallback_pct\":40,");
    start(j.c_str());
    stmReady(500);
    mqttTemp(20.5f);
    run(5000);
    TEST_ASSERT_EQUAL(SourceState::Ok, eng->source(0).state);

    run(600000); /* max_age 600 s */
    TEST_ASSERT_EQUAL(SourceState::Stale, eng->source(0).state);
    TEST_ASSERT_EQUAL(ZoneAction::Fallback, eng->zone(0).action);
    TEST_ASSERT_EQUAL(400, eng->valve(0).effective); /* zone fallback */
    vdm::PiSnapshot snap[2];
    TEST_ASSERT_EQUAL(1, eng->snapshot(snap, 2));
    float integral_before = snap[0].integral;
    run(3600000);
    eng->snapshot(snap, 2);
    TEST_ASSERT_EQUAL_FLOAT(integral_before, snap[0].integral); /* held while stale */

    mqttTemp(20.5f);
    run(1000);
    TEST_ASSERT_EQUAL(ZoneAction::Heating, eng->zone(0).action);
    eng->snapshot(snap, 2);
    /* integration continues with a capped time step only (2 samples) */
    TEST_ASSERT_TRUE(snap[0].integral - integral_before < 6.0f);
    TEST_ASSERT_TRUE(snap[0].integral > integral_before);
}

void test_onewire_source_from_model()
{
    start(R"({
      "sources": [{"id": "s1", "name": "Bath", "rom": "28-FF-01-02-03-04-05-06", "offset": -0.5}],
      "next_source_id": 2})");
    model.sensor_total = 3;
    memcpy(model.sensors[0].rom, kRom1, 8);
    model.sensors[0].kind = VDM_SENSOR_VAD_DS2438; /* same ROM, not a temperature */
    model.sensors[0].value = 5000;
    memcpy(model.sensors[1].rom, kRom1, 8);
    model.sensors[1].kind = VDM_SENSOR_TEMP_DS18B20;
    model.sensors[1].value = 21375;
    model.sensors[1].age_s = 20;
    model.sensors_ms = 50000;
    now = 60000;
    eng->updateFromModel(model, now);
    eng->tick(now);
    TEST_ASSERT_EQUAL(SourceState::Ok, eng->source(0).state);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.875f, eng->source(0).value);
    TEST_ASSERT_EQUAL(30u, eng->source(0).age_s);

    now += 300000; /* no new fetch: older than max_age 300 s */
    eng->tick(now);
    TEST_ASSERT_EQUAL(SourceState::Stale, eng->source(0).state);

    model.sensors[1].age_s = 0xFFFF; /* never read: ignored */
    model.sensors_ms = now;
    eng->updateFromModel(model, now);
    eng->tick(now);
    TEST_ASSERT_EQUAL(SourceState::Stale, eng->source(0).state);
}

void test_mqtt_payloads()
{
    start(R"({
      "sources": [{"id": "s1", "name": "A", "type": "mqtt", "topic": "a"},
                  {"id": "s2", "name": "B", "type": "mqtt", "topic": "b", "field": "temperature"},
                  {"id": "s3", "name": "C", "type": "mqtt", "topic": "b", "field": "x.t"}],
      "next_source_id": 4})");
    mqtt("a", " 21.25 ");
    mqtt("b", R"({"temperature": 19.5, "x": {"t": "18.0"}})");
    run(1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.25f, eng->source(0).value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.5f, eng->source(1).value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 18.0f, eng->source(2).value);

    mqtt("a", "unavailable");
    mqtt("a", "21.5x");
    mqtt("a", "500");
    mqtt("b", "{broken");
    run(1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.25f, eng->source(0).value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.5f, eng->source(1).value);
    TEST_ASSERT_FALSE(eng->onMqtt("other", "1", 1, now));
}

void test_mqtt_default_topic()
{
    start(R"({"sources": [{"id": "s4", "name": "A", "type": "mqtt"}], "next_source_id": 5})");
    eng->setMqttBase("vdmot/vdmot-a1b2c3");
    char b[96];
    TEST_ASSERT_EQUAL_STRING("vdmot/vdmot-a1b2c3/source/s4/set", eng->mqttTopic(0, b, sizeof(b)));
    mqtt("vdmot/vdmot-a1b2c3/source/s4/set", "19.5");
    run(1000);
    TEST_ASSERT_EQUAL(SourceState::Ok, eng->source(0).state);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.5f, eng->source(0).value);
}

void test_mean_of_control_sources()
{
    start(R"({
      "sources": [{"id": "s1", "name": "A", "type": "mqtt", "topic": "a", "max_age_s": 60},
                  {"id": "s2", "name": "B", "type": "mqtt", "topic": "b", "max_age_s": 600}],
      "zones": [{"id": "z1", "name": "Attic", "control_sources": ["s1", "s2"]}],
      "valves": [{"zone": "z1"}],
      "next_source_id": 3, "next_zone_id": 2})");
    mqtt("a", "20.0");
    run(1000);
    TEST_ASSERT_EQUAL(SourceState::Ok, eng->zone(0).source);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, eng->zone(0).temperature); /* only A so far */
    mqtt("b", "21.0");
    run(1000);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.5f, eng->zone(0).temperature);
    run(61000); /* A stale: B alone */
    mqtt("b", "21.0");
    run(1000);
    TEST_ASSERT_EQUAL(SourceState::Ok, eng->zone(0).source);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, eng->zone(0).temperature);
    run(600000); /* both stale */
    TEST_ASSERT_EQUAL(SourceState::Stale, eng->zone(0).source);
    TEST_ASSERT_EQUAL(ZoneAction::Fallback, eng->zone(0).action);
}

void test_reconfigure_keeps_state()
{
    start(kOneRoom);
    mqttTemp(20.0f);
    run(5000);
    float out = eng->zone(0).output;
    /* rename the zone: same id, state kept */
    strcpy(cfg.zones[0].name, "Living room");
    eng->configure(cfg, now);
    run(1000);
    TEST_ASSERT_EQUAL_FLOAT(out, eng->zone(0).output);
    TEST_ASSERT_EQUAL(SourceState::Ok, eng->source(0).state);
}

/* --- zones and valves ---------------------------------------------------- */

void test_heating_off_goes_to_min_position()
{
    std::string j = kOneRoom;
    j.replace(j.find("{\"zone\": \"z1\"}"), 14, R"({"zone": "z1", "min_pct": 15})");
    start(j.c_str());
    mqttTemp(18.0f);
    run(2000);
    TEST_ASSERT_TRUE(eng->valve(0).effective > 150);
    cfg.zones[0].enabled = false;
    run(1000);
    TEST_ASSERT_EQUAL(ZoneAction::Off, eng->zone(0).action);
    TEST_ASSERT_EQUAL(0, eng->valve(0).requested);
    TEST_ASSERT_EQUAL(150, eng->valve(0).effective);
}

void test_factor_and_limits()
{
    start(R"({
      "sources": [{"id": "s1", "name": "Room", "type": "mqtt", "topic": "room/t"}],
      "zones": [{"id": "z1", "name": "Bath", "setpoint": 21, "control_sources": ["s1"]}],
      "valves": [{"zone": "z1"}, {"zone": "z1", "factor_pct": 50}, {"zone": "z1", "max_pct": 30}],
      "next_source_id": 2, "next_zone_id": 2})");
    stmReady(800);
    mqttTemp(21.0f);
    run(1000);
    /* bumpless start from the mean position 800/(.5) ... clamped */
    float out = eng->zone(0).output;
    TEST_ASSERT_EQUAL(lroundf(out * 10), eng->valve(0).effective);
    TEST_ASSERT_EQUAL(lroundf(out * 5), eng->valve(1).effective);
    TEST_ASSERT_EQUAL(300, eng->valve(2).effective);
    TEST_ASSERT_TRUE(eng->valve(2).requested > 300);
}

void test_manual_targets()
{
    start(kOneRoom);
    stmReady(250);
    run(1000);
    /* channel 2 has no zone: controlled directly, starts from the STM32 target */
    TEST_ASSERT_FALSE(eng->valve(1).controlled);
    TEST_ASSERT_EQUAL(250, eng->valve(1).effective);
    TEST_ASSERT_EQUAL(ControlEngine::Result::Ok, eng->setManualTarget(1, 700));
    TEST_ASSERT_EQUAL(ControlEngine::Result::Controlled, eng->setManualTarget(0, 700));
    TEST_ASSERT_EQUAL(ControlEngine::Result::BadChannel, eng->setManualTarget(12, 700));
    TEST_ASSERT_EQUAL(ControlEngine::Result::BadChannel, eng->setManualTarget(1, 1001));
    run(1000);
    TEST_ASSERT_EQUAL(700, eng->valve(1).effective);

    /* control -> manual: the valve stays where the controller left it */
    mqttTemp(19.0f);
    run(2000);
    uint16_t pos = eng->valve(0).effective;
    cfg.zones[0].mode = vdm::ZoneMode::Manual;
    run(1000);
    TEST_ASSERT_EQUAL(ZoneAction::Manual, eng->zone(0).action);
    TEST_ASSERT_EQUAL(pos, eng->valve(0).effective);
    TEST_ASSERT_EQUAL(ControlEngine::Result::Ok, eng->setManualTarget(0, 100));
    run(1000);
    TEST_ASSERT_EQUAL(100, eng->valve(0).effective);

    /* manual -> control: bumpless from the current position */
    cfg.zones[0].mode = vdm::ZoneMode::Control;
    mqttTemp(21.0f);
    run(1000);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, eng->zone(0).output);
}

void test_snapshot_restore()
{
    start(kOneRoom);
    mqttTemp(20.0f);
    run(120000);
    vdm::PiSnapshot snap[4];
    uint8_t n = eng->snapshot(snap, 4);
    TEST_ASSERT_EQUAL(1, n);
    float out = eng->zone(0).output;

    /* "restart": new engine, STM32 reports another target */
    start(kOneRoom);
    eng->restore(snap, n);
    stmReady(0);
    run(5000);
    TEST_ASSERT_EQUAL(SourceState::Pending, eng->source(0).state);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, out, eng->zone(0).output);
    TEST_ASSERT_EQUAL(lroundf(out * 10), eng->valve(0).effective);
}

/* --- flow guard ---------------------------------------------------------- */

const char *kFlow = R"({
  "flow": {"min_total_pct": 150},
  "valves": [{"flow_priority": true, "max_pct": 80}, {}, {}, {"max_pct": 20}],
  "next_source_id": 1, "next_zone_id": 1
})";

void test_flow_guard_priority_first()
{
    start(kFlow);
    for (uint8_t ch = 4; ch < VDM_MAX_CHANNELS; ch++) {
        model.valves[ch].mode = VDM_MODE_NOT_CONNECTED;
    }
    run(1000);
    /* 150 %: priority valve up to 80 %, then 70 % evenly over channels 2-4,
     * channel 4 capped at 20 % */
    TEST_ASSERT_TRUE(eng->flow().active);
    TEST_ASSERT_FALSE(eng->flow().insufficient);
    TEST_ASSERT_EQUAL(1500, eng->flow().added);
    TEST_ASSERT_EQUAL(800, eng->valve(0).effective);
    TEST_ASSERT_EQUAL(200, eng->valve(3).effective);
    TEST_ASSERT_EQUAL(250, eng->valve(1).effective);
    TEST_ASSERT_EQUAL(250, eng->valve(2).effective);
    TEST_ASSERT_EQUAL(0, eng->valve(1).requested);
    TEST_ASSERT_EQUAL(1500, eng->flow().total);
}

void test_flow_guard_inactive_when_satisfied()
{
    start(kFlow);
    for (uint8_t ch = 4; ch < VDM_MAX_CHANNELS; ch++) {
        model.valves[ch].mode = VDM_MODE_NOT_CONNECTED;
    }
    eng->setManualTarget(1, 1000);
    eng->setManualTarget(2, 600);
    run(1000);
    TEST_ASSERT_FALSE(eng->flow().active);
    TEST_ASSERT_EQUAL(1600, eng->flow().total);
    TEST_ASSERT_EQUAL(0, eng->valve(0).effective);
}

void test_flow_guard_faulty_valves_do_not_count()
{
    start(kFlow);
    for (uint8_t ch = 4; ch < VDM_MAX_CHANNELS; ch++) {
        model.valves[ch].mode = VDM_MODE_DISABLED;
    }
    eng->setManualTarget(1, 1000);
    eng->setManualTarget(2, 600);
    model.valves[2].mode = VDM_MODE_FAULT;
    run(1000);
    TEST_ASSERT_TRUE(eng->flow().active);
    TEST_ASSERT_FALSE(eng->valve(2).eligible);
    TEST_ASSERT_EQUAL(500, eng->flow().added);
    TEST_ASSERT_EQUAL(500, eng->valve(0).effective);
    TEST_ASSERT_EQUAL(1000, eng->valve(1).effective);

    /* not reachable at all */
    cfg.flow.min_total_pct = 300;
    run(1000);
    TEST_ASSERT_TRUE(eng->flow().insufficient);
    TEST_ASSERT_EQUAL(2000, eng->flow().total);
}

/* --- heating / cooling from the supply temperature ------------------------ */

const char *kHvac = R"({
  "hvac": {"supply_source": "s9", "supply_margin_k": 2},
  "sources": [{"id": "s1", "name": "Room", "type": "mqtt", "topic": "room/t", "max_age_s": 0},
              {"id": "s2", "name": "Attic", "type": "mqtt", "topic": "attic/t", "max_age_s": 0},
              {"id": "s9", "name": "Supply", "type": "mqtt", "topic": "supply", "max_age_s": 0}],
  "zones": [{"id": "z1", "name": "Room", "setpoint": 21, "cool_setpoint": 24,
             "control_sources": ["s1"]},
            {"id": "z2", "name": "Attic", "setpoint": 21, "cool_setpoint": 24,
             "control_sources": ["s2"]}],
  "valves": [{"zone": "z1"}, {"zone": "z2"}],
  "next_source_id": 10, "next_zone_id": 3
})";

void test_pause_without_heat()
{
    start(kHvac);
    stmReady(400);
    mqtt("supply", "35");
    mqtt("room/t", "20.5");
    run(2000);
    TEST_ASSERT_EQUAL(vdm::Medium::Heat, eng->zone(0).medium);
    TEST_ASSERT_EQUAL(ZoneAction::Heating, eng->zone(0).action);
    vdm::PiSnapshot snap[4];
    eng->snapshot(snap, 4);
    float out = eng->zone(0).output;

    /* the heat pump stops: the pipe cools down to the room temperature */
    mqtt("supply", "21.5");
    run(60000);
    TEST_ASSERT_EQUAL(ZoneAction::Heating, eng->zone(0).action); /* dwell time */
    run(ControlEngine::kMediumDwellMs);
    TEST_ASSERT_EQUAL(ZoneAction::Paused, eng->zone(0).action);
    float i_paused;
    eng->snapshot(snap, 4);
    i_paused = snap[0].integral;
    uint16_t pos = eng->valve(0).effective;
    run(3600000); /* an hour without heat: nothing moves, nothing winds up */
    eng->snapshot(snap, 4);
    TEST_ASSERT_EQUAL_FLOAT(i_paused, snap[0].integral);
    TEST_ASSERT_EQUAL(pos, eng->valve(0).effective);
    (void)out;

    /* heat again */
    mqtt("supply", "30");
    run(ControlEngine::kMediumDwellMs + 1000);
    TEST_ASSERT_EQUAL(ZoneAction::Heating, eng->zone(0).action);
    eng->snapshot(snap, 4);
    TEST_ASSERT_TRUE(snap[0].integral - i_paused < 3.0f); /* no jump after the pause */
}

void test_short_dip_does_not_pause()
{
    start(kHvac);
    mqtt("supply", "35");
    mqtt("room/t", "20.5");
    run(2000);
    mqtt("supply", "21");
    run(120000);
    mqtt("supply", "34");
    run(ControlEngine::kMediumDwellMs);
    TEST_ASSERT_EQUAL(ZoneAction::Heating, eng->zone(0).action);
    /* hysteresis: 22 °C is still "heat" once heating (margin 2, kept down to 1) */
    mqtt("supply", "21.7");
    run(ControlEngine::kMediumDwellMs + 1000);
    TEST_ASSERT_EQUAL(vdm::Medium::Heat, eng->zone(0).medium);
}

void test_cooling_and_mixed_zones()
{
    start(kHvac);
    stmReady(0);
    /* summer: cool supply, attic warm, ground floor cool */
    mqtt("supply", "19");
    mqtt("room/t", "20.5");
    mqtt("attic/t", "26");
    run(2000);
    TEST_ASSERT_EQUAL(vdm::Medium::Cool, eng->zone(1).medium);
    TEST_ASSERT_EQUAL(ZoneAction::Cooling, eng->zone(1).action);
    TEST_ASSERT_EQUAL(ZoneAction::Paused, eng->zone(0).action); /* 19 vs 20.5: nothing */
    TEST_ASSERT_EQUAL(0, eng->valve(0).effective);

    /* the attic controller opens over time (2 K above the cooling setpoint) */
    run(1800000);
    TEST_ASSERT_TRUE(eng->zone(1).output > 50.0f);

    /* attic below its cooling setpoint: closes */
    mqtt("attic/t", "23");
    run(1800000);
    TEST_ASSERT_TRUE(eng->zone(1).output < 5.0f);

    /* ground floor 22 °C: cold water now counts, but 22 < 24: stays closed */
    mqtt("room/t", "22");
    run(ControlEngine::kMediumDwellMs + 61000);
    TEST_ASSERT_EQUAL(vdm::Medium::Cool, eng->zone(0).medium);
    TEST_ASSERT_TRUE(eng->zone(0).output < 1.0f);
}

void test_forced_mode()
{
    start(kHvac);
    cfg.hvac.mode = vdm::HvacMode::Heat;
    mqtt("supply", "15");
    mqtt("room/t", "25");
    run(2000);
    TEST_ASSERT_EQUAL(ZoneAction::Paused, eng->zone(0).action); /* cold water, heat wanted */

    cfg.hvac.mode = vdm::HvacMode::Cool;
    cfg.hvac.supply_source[0] = '\0'; /* no supply sensor: cooling, never paused */
    run(2000);
    TEST_ASSERT_EQUAL(vdm::Medium::Cool, eng->zone(0).medium);
    TEST_ASSERT_EQUAL(ZoneAction::Cooling, eng->zone(0).action);
}

void test_zone_supply_source_overrides()
{
    start(kHvac);
    strcpy(cfg.zones[1].supply_source, "s1"); /* attic: "Room" as its supply (test only) */
    eng->configure(cfg, now);
    mqtt("supply", "35");
    mqtt("room/t", "20");
    mqtt("attic/t", "20");
    run(2000);
    TEST_ASSERT_EQUAL(ZoneAction::Heating, eng->zone(0).action);
    TEST_ASSERT_EQUAL(ZoneAction::Paused, eng->zone(1).action);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, eng->zone(1).supply);
}

void test_supply_shown_without_room_temperature()
{
    start(kHvac);
    cfg.zones[0].control_sources[0][0] = '\0';
    mqtt("supply", "33");
    run(ControlEngine::kStartupGraceMs + 1000);
    TEST_ASSERT_EQUAL(ZoneAction::Fallback, eng->zone(0).action);
    TEST_ASSERT_FALSE(eng->zone(0).has_control_source);
    TEST_ASSERT_TRUE(eng->zone(0).has_supply);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 33.0f, eng->zone(0).supply);
    TEST_ASSERT_TRUE(eng->zone(1).has_control_source);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_pi_bumpless_and_integral);
    RUN_TEST(test_pi_anti_windup);
    RUN_TEST(test_closed_loop_reaches_setpoint);
    RUN_TEST(test_floor_profile_closed_loop);
    RUN_TEST(test_output_deadband);
    RUN_TEST(test_startup_hold_then_fallback);
    RUN_TEST(test_hold_without_link_model);
    RUN_TEST(test_stale_source_and_recovery);
    RUN_TEST(test_onewire_source_from_model);
    RUN_TEST(test_mqtt_payloads);
    RUN_TEST(test_mqtt_default_topic);
    RUN_TEST(test_mean_of_control_sources);
    RUN_TEST(test_reconfigure_keeps_state);
    RUN_TEST(test_heating_off_goes_to_min_position);
    RUN_TEST(test_factor_and_limits);
    RUN_TEST(test_manual_targets);
    RUN_TEST(test_snapshot_restore);
    RUN_TEST(test_flow_guard_priority_first);
    RUN_TEST(test_flow_guard_inactive_when_satisfied);
    RUN_TEST(test_flow_guard_faulty_valves_do_not_count);
    RUN_TEST(test_pause_without_heat);
    RUN_TEST(test_short_dip_does_not_pause);
    RUN_TEST(test_cooling_and_mixed_zones);
    RUN_TEST(test_forced_mode);
    RUN_TEST(test_zone_supply_source_overrides);
    RUN_TEST(test_supply_shown_without_room_temperature);
    return UNITY_END();
}
