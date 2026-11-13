#include <unity.h>

#include "core/motor_run.h"

using vdm::MotorRun;
using vdm::RunOutcome;
using vdm::RunParams;

namespace
{

struct FakeMotor : vdm::MotorPort {
    int disables = 0;
    void power(bool) override {}
    void select(uint8_t, vdm::Dir) override {}
    void enable(uint8_t) override {}
    void disableAll() override
    {
        disables++;
    }
};

FakeMotor motor;
MotorRun *run;

RunParams params(uint32_t target = 0)
{
    RunParams p{};
    p.target_pulses = target;
    p.ref_current_ma10 = 200;
    p.endstop_factor = 17;
    p.endstop_abs_ma10 = 1000;
    p.inrush_blank_ms = 250;
    p.opencircuit_ma10 = 20;
    p.opencircuit_ms = 500;
    p.timeout_ms = 10000;
    return p;
}

/* feeds `ms` samples of `ma10`, a pulse every `pulse_every` ms (0: none) */
void feed(uint32_t ms, int16_t ma10, uint32_t pulse_every)
{
    for (uint32_t i = 1; i <= ms && !run->done(); i++) {
        if (pulse_every != 0 && i % pulse_every == 0) {
            run->onPulse();
        }
        run->onSample(ma10);
    }
}

void finishCoast()
{
    for (int i = 0; i < 200 && !run->done(); i++) {
        run->onSample(0);
    }
}

} // namespace

void setUp(void)
{
    motor = FakeMotor{};
    run = new MotorRun(motor);
}
void tearDown(void)
{
    delete run;
}

static void test_target_reached_counts_coast_pulses(void)
{
    TEST_ASSERT_TRUE(run->start(params(100)));
    TEST_ASSERT_TRUE(run->active());
    feed(200, 200, 2); /* 100 pulses after 200 ms */
    TEST_ASSERT_TRUE(motor.disables >= 1);
    /* coasting: two more pulses */
    run->onPulse();
    run->onPulse();
    finishCoast();
    TEST_ASSERT_TRUE(run->done());
    vdm::RunResult r = run->result();
    TEST_ASSERT_EQUAL(RunOutcome::TargetReached, r.outcome);
    TEST_ASSERT_EQUAL_UINT32(102, r.pulses);
    TEST_ASSERT_FALSE(run->start(params())); /* not released yet */
    run->release();
    TEST_ASSERT_TRUE(run->start(params()));
}

static void test_endstop_relative_threshold(void)
{
    TEST_ASSERT_TRUE(run->start(params()));
    feed(1000, 200, 3);
    TEST_ASSERT_FALSE(run->done());
    feed(500, 400, 0); /* > 20 mA * 1.7 */
    finishCoast();
    vdm::RunResult r = run->result();
    TEST_ASSERT_EQUAL(RunOutcome::Endstop, r.outcome);
    TEST_ASSERT_UINT16_WITHIN(20, 200, r.mean_ma10);
    TEST_ASSERT_TRUE(r.peak_ma10 > 340);
}

static void test_endstop_negative_current(void)
{
    TEST_ASSERT_TRUE(run->start(params()));
    feed(600, -200, 3);
    feed(500, -400, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::Endstop, run->result().outcome);
}

static void test_inrush_is_blanked(void)
{
    TEST_ASSERT_TRUE(run->start(params()));
    feed(200, 900, 3); /* inrush 90 mA for 200 ms */
    feed(800, 200, 3);
    TEST_ASSERT_FALSE(run->done());
    run->abort();
    feed(2, 200, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::Aborted, run->result().outcome);
}

static void test_absolute_threshold_without_reference(void)
{
    RunParams p = params();
    p.ref_current_ma10 = 0;
    TEST_ASSERT_TRUE(run->start(p));
    feed(1000, 600, 3); /* 60 mA from the start: becomes the reference */
    TEST_ASSERT_FALSE(run->done());
    feed(200, 1100, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::Endstop, run->result().outcome);
}

static void test_bad_reference_cannot_exceed_absolute(void)
{
    RunParams p = params();
    p.ref_current_ma10 = 900; /* 90 mA * 1.7 > 100 mA */
    TEST_ASSERT_TRUE(run->start(p));
    feed(500, 200, 3);
    feed(200, 1050, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::Endstop, run->result().outcome);
}

static void test_open_circuit_needs_consecutive_low_current(void)
{
    TEST_ASSERT_TRUE(run->start(params()));
    /* alternating low/normal current must not trip */
    for (int k = 0; k < 20; k++) {
        feed(300, 10, 0);
        feed(50, 200, 1);
    }
    TEST_ASSERT_FALSE(run->done());
    feed(600, 5, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::OpenCircuit, run->result().outcome);
}

static void test_no_pulses(void)
{
    TEST_ASSERT_TRUE(run->start(params(100)));
    feed(1300, 200, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::NoPulses, run->result().outcome);
}

static void test_stall_is_an_endstop(void)
{
    /* run to the endstop: stall without any pulse (already at the end) */
    TEST_ASSERT_TRUE(run->start(params()));
    feed(1300, 250, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::Endstop, run->result().outcome);
    run->release();

    /* move with a pulse target: stall after some pulses (obstruction) */
    TEST_ASSERT_TRUE(run->start(params(1000)));
    feed(600, 200, 3);
    feed(1300, 250, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::Endstop, run->result().outcome);
}

static void test_mean_current_excludes_stall(void)
{
    TEST_ASSERT_TRUE(run->start(params()));
    feed(1000, 200, 3); /* turning at 20 mA */
    feed(1300, 250, 0); /* stalled at 25 mA until the stall detection */
    finishCoast();
    vdm::RunResult r = run->result();
    TEST_ASSERT_EQUAL(RunOutcome::Endstop, r.outcome);
    TEST_ASSERT_UINT16_WITHIN(5, 200, r.mean_ma10);
}

static void test_dynamic_reference_without_calibration(void)
{
    RunParams p = params();
    p.ref_current_ma10 = 0;
    TEST_ASSERT_TRUE(run->start(p));
    feed(800, 200, 3); /* reference measured: 20 mA */
    TEST_ASSERT_FALSE(run->done());
    feed(200, 600, 0); /* 60 mA: above 1.7 x 20 mA, below the 100 mA limit */
    finishCoast();
    vdm::RunResult r = run->result();
    TEST_ASSERT_EQUAL(RunOutcome::Endstop, r.outcome);
    TEST_ASSERT_TRUE(r.duration_ms < 800 + 100); /* by current, not by stall */
}

static void test_overcurrent_even_during_blanking(void)
{
    TEST_ASSERT_TRUE(run->start(params()));
    feed(10, 2600, 0);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::Overcurrent, run->result().outcome);
}

static void test_short_spike_is_ignored(void)
{
    TEST_ASSERT_TRUE(run->start(params()));
    feed(500, 200, 3);
    feed(2, 2600, 0); /* 2 samples < kHardLimitSamples */
    feed(500, 200, 3);
    TEST_ASSERT_FALSE(run->done());
}

static void test_timeout(void)
{
    RunParams p = params();
    p.timeout_ms = 2000;
    TEST_ASSERT_TRUE(run->start(p));
    feed(2500, 200, 3);
    finishCoast();
    TEST_ASSERT_EQUAL(RunOutcome::Timeout, run->result().outcome);
    TEST_ASSERT_EQUAL_UINT32(2000, run->result().duration_ms);
}

static void test_pulses_ignored_when_idle(void)
{
    run->onPulse();
    run->onSample(200);
    TEST_ASSERT_FALSE(run->active());
    TEST_ASSERT_TRUE(run->start(params(5)));
    TEST_ASSERT_EQUAL_UINT32(0, run->livePulses());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_target_reached_counts_coast_pulses);
    RUN_TEST(test_endstop_relative_threshold);
    RUN_TEST(test_endstop_negative_current);
    RUN_TEST(test_inrush_is_blanked);
    RUN_TEST(test_absolute_threshold_without_reference);
    RUN_TEST(test_bad_reference_cannot_exceed_absolute);
    RUN_TEST(test_open_circuit_needs_consecutive_low_current);
    RUN_TEST(test_no_pulses);
    RUN_TEST(test_stall_is_an_endstop);
    RUN_TEST(test_dynamic_reference_without_calibration);
    RUN_TEST(test_mean_current_excludes_stall);
    RUN_TEST(test_overcurrent_even_during_blanking);
    RUN_TEST(test_short_spike_is_ignored);
    RUN_TEST(test_timeout);
    RUN_TEST(test_pulses_ignored_when_idle);
    return UNITY_END();
}
