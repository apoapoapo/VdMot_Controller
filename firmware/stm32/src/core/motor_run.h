/*
 * MotorRun - real-time part of a single motor movement.
 *
 * The main loop prepares and starts a run; two interrupt sources feed it:
 *   onSample()  1 kHz timer, with the motor current in 0.1 mA (signed)
 *   onPulse()   back-EMF commutation pulse (one per detected revolution step)
 *
 * Both interrupt handlers must run at the same NVIC priority so that they
 * never preempt each other. Ownership of the shared state is handed over
 * through `phase_`:
 *   Idle/Done  -> owned by the main loop
 *   Running/Coasting -> owned by the interrupt handlers
 *
 * The motor is switched off directly from interrupt context (MotorPort::
 * disableAll()) as soon as a stop condition is detected; pulses are still
 * counted for kCoastMs afterwards so the position stays exact.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

#include "core/ports.h"

namespace vdm
{

enum class RunOutcome : uint8_t {
    None,
    TargetReached, /* requested number of pulses counted */
    Endstop,       /* current above the endstop threshold, or motor stalled */
    OpenCircuit,   /* current below the open-circuit threshold */
    Overcurrent,   /* current above the hard limit (short circuit) */
    Timeout,       /* run took longer than allowed */
    NoPulses,      /* current flows, but not a single pulse arrived */
    Aborted,       /* stopped by the main loop */
};

struct RunParams {
    uint32_t target_pulses;    /* 0 = run until the endstop */
    uint16_t ref_current_ma10; /* running current of this direction, 0 = measure it */
    uint8_t endstop_factor;    /* endstop if |I| > ref * factor / 10 */
    uint16_t endstop_abs_ma10; /* absolute endstop threshold */
    uint16_t inrush_blank_ms;
    uint16_t opencircuit_ma10;
    uint16_t opencircuit_ms;
    uint32_t timeout_ms;
};

struct RunResult {
    RunOutcome outcome;
    uint32_t pulses;        /* including pulses counted while coasting */
    uint32_t duration_ms;   /* until the motor was switched off */
    uint16_t mean_ma10;     /* mean |I| after the inrush blanking while the motor turned
                             * (up to the last pulse, so a stall does not count) */
    uint16_t peak_ma10;     /* peak filtered |I| after the inrush blanking */
    uint16_t peak_all_ma10; /* peak filtered |I| over the whole run, inrush included */
};

class MotorRun
{
  public:
    static constexpr int16_t kHardLimitMa10 = 2500; /* 250 mA */
    static constexpr uint8_t kHardLimitSamples = 3;
    static constexpr uint8_t kEndstopSamples = 20;
    /* no pulse for this long while current flows: the motor stalls. That is
     * an endstop if the run expects one (target_pulses == 0) or if pulses
     * were counted before, otherwise the pulse detection is broken. */
    static constexpr uint16_t kStallMs = 1000;
    /* without a reference current the mean of the first samples after the
     * inrush blanking is used */
    static constexpr uint16_t kDynRefSamples = 300;
    static constexpr uint16_t kCoastMs = 100;

    explicit MotorRun(MotorPort &port) : port_(port) {}

    /* --- main loop ----------------------------------------------------- */

    /* Arms a run. The caller enables the motor driver right afterwards.
     * Returns false if a run is still active or not yet released. */
    bool start(const RunParams &params);
    /* Requests a stop; the run then ends with RunOutcome::Aborted. */
    void abort();
    /* True once the run has ended and coasting is over. */
    bool done() const;
    /* True from start() until done(). */
    bool active() const;
    /* Result of the finished run (valid while done()). */
    RunResult result() const;
    /* Returns ownership to the main loop for the next start(). */
    void release();
    /* Live values for status reports while active. */
    uint32_t livePulses() const
    {
        return pulses_;
    }
    int16_t liveCurrentMa10() const;

    /* --- interrupt context --------------------------------------------- */

    void onPulse();
    void onSample(int16_t current_ma10);

  private:
    enum Phase : uint8_t { kIdle, kRunning, kCoasting, kDone };

    void finish(RunOutcome outcome);
    void setThreshold(uint16_t ref_ma10);

    MotorPort &port_;
    RunParams params_{};
    uint16_t threshold_ma10_ = 0;

    volatile uint8_t phase_ = kIdle;
    volatile bool abort_request_ = false;
    volatile bool target_hit_ = false;
    volatile uint32_t pulses_ = 0;
    volatile uint32_t last_pulse_ms_ = 0;
    volatile int32_t filtered_x16_ = 0; /* IIR state, 0.1 mA * 16 */

    /* written by the interrupt handlers only */
    uint32_t elapsed_ms_ = 0;
    uint32_t coast_ms_ = 0;
    uint32_t stop_ms_ = 0;
    uint8_t hard_cnt_ = 0;
    uint8_t endstop_cnt_ = 0;
    uint32_t low_ms_ = 0;
    uint64_t sum_abs_ = 0;
    uint32_t sum_cnt_ = 0;
    uint64_t turn_sum_ = 0; /* sum_abs_/sum_cnt_ at the last pulse */
    uint32_t turn_cnt_ = 0;
    uint16_t peak_ = 0;
    uint16_t peak_all_ = 0;
    RunOutcome outcome_ = RunOutcome::None;
};

} // namespace vdm
