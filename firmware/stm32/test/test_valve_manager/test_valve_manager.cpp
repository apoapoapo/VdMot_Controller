#include <math.h>
#include <stdlib.h>
#include <unity.h>

#include "../support/sim.h"

using sim::Rig;

void setUp(void) {}
void tearDown(void) {}

static vdm_valve_state_t st(Rig &r, uint8_t ch)
{
    return r.vm.state(ch);
}

/* reported position (0.1 %) vs. mechanical position of the simulated valve */
static void assertPositionMatches(Rig &r, uint8_t ch, double tol_permille)
{
    vdm_valve_state_t s = st(r, ch);
    TEST_ASSERT_NOT_EQUAL(VDM_POS_UNKNOWN, s.actual);
    double mech = r.board.v[ch].x * 1000.0;
    TEST_ASSERT_DOUBLE_WITHIN(tol_permille, mech, (double)s.actual);
}

static void test_boot_detect_calibrate_and_move(void)
{
    Rig r;
    r.vm.setTarget(0, 450);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_UNKNOWN, st(r, 0).mode);
    TEST_ASSERT_TRUE(r.settle());

    vdm_valve_state_t s = st(r, 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, s.mode);
    TEST_ASSERT_TRUE(s.flags & VDM_VFLAG_CALIBRATED);
    TEST_ASSERT_FALSE(s.flags & VDM_VFLAG_POS_UNCERTAIN);
    TEST_ASSERT_UINT16_WITHIN(6, 450, s.actual);
    assertPositionMatches(r, 0, 6);

    const vdm::ChannelPersist &p = r.vm.persist(0);
    /* learned strokes match the simulated (asymmetric) valve, +/- coasting */
    TEST_ASSERT_UINT32_WITHIN(10, 6000, p.open_counts);
    TEST_ASSERT_UINT32_WITHIN(10, 5000, p.close_counts);
    TEST_ASSERT_UINT16_WITHIN(10, 200, p.mean_open_ma10);

    vdm_evt_calibration_t evt;
    TEST_ASSERT_TRUE(r.vm.popCalibrationEvent(evt));
    TEST_ASSERT_EQUAL_UINT8(0, evt.result);
    TEST_ASSERT_EQUAL_UINT8(0, evt.channel);

    uint16_t urgent = 0;
    uint16_t dirty = r.vm.takePersistDirty(&urgent);
    TEST_ASSERT_TRUE(urgent & 1u);
    TEST_ASSERT_TRUE(dirty & 1u);
    TEST_ASSERT_EQUAL_UINT32(0, r.board.violations);
    TEST_ASSERT_FALSE(r.board.psu && r.board.enabled >= 0);
}

static void test_valve_fully_open_at_boot(void)
{
    Rig r;
    r.board.v[0].x = 1.0; /* detection in opening direction finds no pulses */
    r.vm.setTarget(0, 250);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, st(r, 0).mode);
    TEST_ASSERT_UINT16_WITHIN(6, 250, st(r, 0).actual);
    assertPositionMatches(r, 0, 6);
}

static void test_random_moves_do_not_drift(void)
{
    Rig r;
    r.vm.setTarget(0, 0);
    TEST_ASSERT_TRUE(r.settle());
    srand(4242);
    for (int i = 0; i < 150; i++) {
        uint16_t t = (uint16_t)(50 + rand() % 900);
        r.vm.setTarget(0, t);
        TEST_ASSERT_TRUE(r.settle());
        /* error of a single move: rounding + coasting */
        assertPositionMatches(r, 0, 6);
    }
    TEST_ASSERT_EQUAL_UINT32(0, r.board.violations);
    TEST_ASSERT_TRUE(r.vm.persist(0).moves_total >= 140);
}

static void test_endpoints_run_into_endstop(void)
{
    Rig r;
    r.vm.setTarget(0, 1000);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_DOUBLE(1.0, r.board.v[0].x);
    vdm_valve_state_t s = st(r, 0);
    TEST_ASSERT_EQUAL_UINT16(1000, s.actual);
    TEST_ASSERT_TRUE(s.flags & VDM_VFLAG_AT_ENDSTOP);

    uint32_t moves = r.board.v[0].moves;
    r.runMs(5000);
    TEST_ASSERT_EQUAL_UINT32(moves, r.board.v[0].moves); /* no repeated endstop runs */

    r.vm.setTarget(0, 0);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.board.v[0].x);
    TEST_ASSERT_EQUAL_UINT16(0, st(r, 0).actual);
}

static void test_small_steps_are_skipped(void)
{
    Rig r;
    r.vm.setTarget(0, 500);
    TEST_ASSERT_TRUE(r.settle());
    uint32_t moves = r.board.v[0].moves;
    r.vm.setTarget(0, (uint16_t)(st(r, 0).actual + 3));
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT32(moves, r.board.v[0].moves);
}

static void test_only_one_motor_at_a_time_and_all_channels_served(void)
{
    Rig r(0x0FFF);
    for (uint8_t ch = 0; ch < 12; ch++) {
        r.board.v[ch].x = 0.1 * (ch % 10);
        r.vm.setTarget(ch, (uint16_t)(100 + ch * 60));
    }
    TEST_ASSERT_TRUE(r.settle(4u * 60u * 60u * 1000u));
    for (uint8_t ch = 0; ch < 12; ch++) {
        TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, st(r, ch).mode);
        assertPositionMatches(r, ch, 6);
    }
    TEST_ASSERT_EQUAL_UINT32(0, r.board.violations);
}

static void test_not_connected_channel(void)
{
    Rig r(0x0003);
    r.board.v[1].connected = false;
    r.vm.setTarget(0, 300);
    r.vm.setTarget(1, 300);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, st(r, 0).mode);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_NOT_CONNECTED, st(r, 1).mode);
    TEST_ASSERT_EQUAL_UINT16(VDM_POS_UNKNOWN, st(r, 1).actual);
    TEST_ASSERT_EQUAL_UINT8(VDM_FAULT_OPEN_CIRCUIT, r.vm.detail(1).last_fault);

    /* plugged in later: found by the re-detection, the first one after 1 min */
    r.board.v[1].connected = true;
    r.runMs(vdm::ValveManager::kRedetectMinMs + 5000);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, st(r, 1).mode);
    assertPositionMatches(r, 1, 6);
    TEST_ASSERT_UINT16_WITHIN(6, 300, st(r, 1).actual);
}

static void test_interference_pulses_without_motor(void)
{
    Rig r(0x0003);
    r.board.v[1].connected = false;
    r.board.v[1].noise_pulses = true; /* the empty channel sees edges on the pulse input */
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_NOT_CONNECTED, st(r, 1).mode);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, st(r, 0).mode);
    vdm_evt_calibration_t evt;
    while (r.vm.popCalibrationEvent(evt)) {
        TEST_ASSERT_EQUAL_UINT8(0, evt.channel); /* no calibration attempt on channel 1 */
    }
}

static void test_redetection_backs_off(void)
{
    Rig r(0x0002);
    r.board.v[1].connected = false;
    TEST_ASSERT_TRUE(r.settle());
    uint32_t moves = r.board.v[1].moves;
    /* checks after 1, 2, 4, 8, 15, 15 ... minutes */
    r.runMs(60u * 60u * 1000u);
    uint32_t checks = r.board.v[1].moves - moves;
    TEST_ASSERT_TRUE(checks >= 6 && checks <= 9);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_NOT_CONNECTED, st(r, 1).mode);
}

static void test_short_stroke_fails_calibration(void)
{
    Rig r;
    r.board.v[0].obstruction = 0.2; /* 1200 pulses < min_stroke 2000 */
    r.board.v[0].x = 0.1;
    r.vm.setTarget(0, 500);
    TEST_ASSERT_TRUE(r.settle());
    vdm_valve_state_t s = st(r, 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_FAULT, s.mode);
    TEST_ASSERT_EQUAL_UINT8(VDM_FAULT_CALIB_FAILED, s.fault);
    vdm_evt_calibration_t evt;
    TEST_ASSERT_TRUE(r.vm.popCalibrationEvent(evt));
    TEST_ASSERT_EQUAL_UINT8(VDM_FAULT_CALIB_FAILED, evt.result);
    TEST_ASSERT_EQUAL_UINT8(2, evt.retries); /* default max_calib_retries */

    /* fixed later: the periodic fault retry recalibrates */
    r.board.v[0].obstruction = 2.0;
    r.runMs(vdm::ValveManager::kFaultRetryMs);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, st(r, 0).mode);
    TEST_ASSERT_UINT16_WITHIN(6, 500, st(r, 0).actual);
}

static void test_obstruction_after_calibration_is_detected(void)
{
    Rig r;
    r.vm.setTarget(0, 100);
    TEST_ASSERT_TRUE(r.settle());
    r.board.v[0].obstruction = 0.5;
    r.vm.setTarget(0, 800);
    TEST_ASSERT_TRUE(r.settle());
    /* the unexpected endstop triggers a recalibration, which now learns the
     * shorter stroke (3000 / 2500 pulses >= min_stroke) */
    vdm_valve_state_t s = st(r, 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, s.mode);
    TEST_ASSERT_UINT32_WITHIN(20, 3000, r.vm.persist(0).open_counts);
    TEST_ASSERT_UINT32_WITHIN(20, 2500, r.vm.persist(0).close_counts);
}

static void test_no_pulses_is_a_fault(void)
{
    Rig r;
    r.board.v[0].no_pulses = true;
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_FAULT, st(r, 0).mode);
    TEST_ASSERT_EQUAL_UINT8(VDM_FAULT_NO_PULSES, st(r, 0).fault);
}

static void test_endstop_without_current_rise_is_found_by_stall(void)
{
    Rig r;
    r.board.v[0].end_ma10 = 200; /* endstop current indistinguishable */
    r.vm.setTarget(0, 300);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, st(r, 0).mode);
    assertPositionMatches(r, 0, 6);
    r.vm.setTarget(0, 0);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT16(0, st(r, 0).actual);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.board.v[0].x);
}

static void test_endless_motor_times_out(void)
{
    Rig r;
    r.vm.setTarget(0, 300);
    TEST_ASSERT_TRUE(r.settle());
    vdm_motor_params_t p = vdm_motor_params_default;
    p.min_stroke_counts = 2000;
    p.move_timeout_s = 20;
    r.vm.setMotorParams(p);
    r.board.v[0].endless = true; /* e.g. broken gear: pulses never stop */
    r.vm.setTarget(0, 0);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_FAULT, st(r, 0).mode);
    TEST_ASSERT_EQUAL_UINT8(VDM_FAULT_TIMEOUT, st(r, 0).fault);
    TEST_ASSERT_EQUAL_UINT32(0, r.board.violations);
}

static void test_restored_calibration_uses_reference_run(void)
{
    vdm::ChannelPersist p;
    {
        Rig r;
        TEST_ASSERT_TRUE(r.settle());
        p = r.vm.persist(0);
    }
    Rig r2(0x0000);
    r2.vm.restore(0, p);
    r2.vm.setTarget(0, 700);
    r2.board.v[0].x = 0.55;
    r2.vm.setEnabledMask(0x0001);
    TEST_ASSERT_TRUE(r2.settle());
    vdm_evt_calibration_t evt;
    TEST_ASSERT_FALSE(r2.vm.popCalibrationEvent(evt)); /* no calibration */
    TEST_ASSERT_UINT16_WITHIN(6, 700, st(r2, 0).actual);
    assertPositionMatches(r2, 0, 6);
}

static void test_reference_goes_to_the_nearer_endstop(void)
{
    vdm::ChannelPersist p;
    {
        Rig r;
        r.vm.setTarget(0, 1000);
        TEST_ASSERT_TRUE(r.settle());
        p = r.vm.persist(0);
    }
    /* restart with the valve still fully open and the stored target 100 % */
    Rig r2(0x0000);
    r2.vm.restore(0, p);
    r2.vm.setTarget(0, 1000);
    r2.vm.setPositionHint(0, 1000);
    r2.board.v[0].x = 1.0;
    r2.vm.setEnabledMask(0x0001);
    double min_x = 1.0;
    for (uint32_t k = 0; k < 120000; k++) {
        r2.step();
        if (r2.board.v[0].x < min_x) {
            min_x = r2.board.v[0].x;
        }
    }
    TEST_ASSERT_TRUE(min_x > 0.98); /* only the detection move, no full stroke */
    vdm_valve_state_t s = st(r2, 0);
    TEST_ASSERT_EQUAL_UINT16(1000, s.actual);
    TEST_ASSERT_TRUE(s.flags & VDM_VFLAG_AT_ENDSTOP);
    vdm_evt_calibration_t evt;
    TEST_ASSERT_FALSE(r2.vm.popCalibrationEvent(evt));
    TEST_ASSERT_EQUAL_UINT32(0, r2.board.violations);
}

static void test_recalibration_after_moves(void)
{
    Rig r;
    r.vm.setRecalibration(50, 0);
    r.vm.setTarget(0, 200);
    TEST_ASSERT_TRUE(r.settle());
    vdm_evt_calibration_t evt;
    while (r.vm.popCalibrationEvent(evt)) {
    }
    for (int i = 0; i < 50; i++) {
        r.vm.setTarget(0, (uint16_t)(i % 2 ? 200 : 400));
        TEST_ASSERT_TRUE(r.settle());
    }
    TEST_ASSERT_TRUE(r.vm.popCalibrationEvent(evt));
    TEST_ASSERT_TRUE(r.vm.persist(0).moves_since_calib < 5);
    TEST_ASSERT_TRUE(r.vm.persist(0).moves_total >= 50);
}

static void test_stop_aborts_and_keeps_position(void)
{
    Rig r;
    r.vm.setTarget(0, 0);
    TEST_ASSERT_TRUE(r.settle());
    r.vm.setTarget(0, 900);
    r.runMs(1100 + 100 + 4000);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_OPENING, st(r, 0).mode);
    r.vm.stop(0x0001);
    TEST_ASSERT_TRUE(r.settle());
    vdm_valve_state_t s = st(r, 0);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, s.mode);
    TEST_ASSERT_TRUE(s.actual > 100 && s.actual < 900);
    TEST_ASSERT_EQUAL_UINT16(s.actual, s.target);
    assertPositionMatches(r, 0, 6);
}

static void test_disable_and_psu_off(void)
{
    Rig r;
    TEST_ASSERT_TRUE(r.settle());
    r.runMs(vdm::ValveManager::kPsuOffDelayMs + 10);
    TEST_ASSERT_FALSE(r.board.psu);
    r.vm.setEnabledMask(0);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_DISABLED, st(r, 0).mode);
    r.vm.setTarget(0, 800);
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_FALSE(r.board.psu);
}

static void test_failsafe_flag(void)
{
    Rig r;
    r.vm.setTarget(0, 1000, true);
    TEST_ASSERT_TRUE(st(r, 0).flags & VDM_VFLAG_FAILSAFE);
    r.vm.setTarget(0, 1000, false);
    TEST_ASSERT_FALSE(st(r, 0).flags & VDM_VFLAG_FAILSAFE);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_detect_calibrate_and_move);
    RUN_TEST(test_valve_fully_open_at_boot);
    RUN_TEST(test_random_moves_do_not_drift);
    RUN_TEST(test_endpoints_run_into_endstop);
    RUN_TEST(test_small_steps_are_skipped);
    RUN_TEST(test_only_one_motor_at_a_time_and_all_channels_served);
    RUN_TEST(test_not_connected_channel);
    RUN_TEST(test_interference_pulses_without_motor);
    RUN_TEST(test_redetection_backs_off);
    RUN_TEST(test_short_stroke_fails_calibration);
    RUN_TEST(test_obstruction_after_calibration_is_detected);
    RUN_TEST(test_no_pulses_is_a_fault);
    RUN_TEST(test_endstop_without_current_rise_is_found_by_stall);
    RUN_TEST(test_endless_motor_times_out);
    RUN_TEST(test_restored_calibration_uses_reference_run);
    RUN_TEST(test_reference_goes_to_the_nearer_endstop);
    RUN_TEST(test_recalibration_after_moves);
    RUN_TEST(test_stop_aborts_and_keeps_position);
    RUN_TEST(test_disable_and_psu_off);
    RUN_TEST(test_failsafe_flag);
    return UNITY_END();
}
