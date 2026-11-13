/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/motor_run.h"

#include <atomic>

namespace vdm
{

namespace
{

inline void releaseFence()
{
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

inline uint16_t absMa10(int32_t v)
{
    if (v < 0) {
        v = -v;
    }
    return v > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(v);
}

} // namespace

bool MotorRun::start(const RunParams &params)
{
    if (phase_ != kIdle) {
        return false;
    }
    params_ = params;
    setThreshold(params.ref_current_ma10);

    abort_request_ = false;
    target_hit_ = false;
    pulses_ = 0;
    last_pulse_ms_ = 0;
    filtered_x16_ = 0;
    elapsed_ms_ = 0;
    coast_ms_ = 0;
    stop_ms_ = 0;
    hard_cnt_ = 0;
    endstop_cnt_ = 0;
    low_ms_ = 0;
    sum_abs_ = 0;
    sum_cnt_ = 0;
    peak_ = 0;
    outcome_ = RunOutcome::None;

    releaseFence();
    phase_ = kRunning;
    return true;
}

void MotorRun::setThreshold(uint16_t ref_ma10)
{
    uint32_t thr = params_.endstop_abs_ma10;
    if (ref_ma10 > 0) {
        uint32_t rel = static_cast<uint32_t>(ref_ma10) * params_.endstop_factor / 10u;
        if (rel < thr) {
            thr = rel;
        }
    }
    /* never below the open-circuit threshold, otherwise every run would end
     * as an endstop */
    if (thr <= params_.opencircuit_ma10) {
        thr = params_.opencircuit_ma10 + 1u;
    }
    threshold_ma10_ = static_cast<uint16_t>(thr);
}

void MotorRun::abort()
{
    abort_request_ = true;
}

bool MotorRun::done() const
{
    return phase_ == kDone;
}

bool MotorRun::active() const
{
    uint8_t p = phase_;
    return p == kRunning || p == kCoasting;
}

RunResult MotorRun::result() const
{
    RunResult r{};
    if (phase_ != kDone) {
        return r;
    }
    releaseFence();
    r.outcome = outcome_;
    r.pulses = pulses_;
    r.duration_ms = stop_ms_;
    r.mean_ma10 = sum_cnt_ ? static_cast<uint16_t>(sum_abs_ / sum_cnt_) : 0;
    r.peak_ma10 = peak_;
    return r;
}

void MotorRun::release()
{
    if (phase_ == kDone) {
        phase_ = kIdle;
    }
}

int16_t MotorRun::liveCurrentMa10() const
{
    return static_cast<int16_t>(filtered_x16_ / 16);
}

void MotorRun::finish(RunOutcome outcome)
{
    port_.disableAll();
    outcome_ = outcome;
    stop_ms_ = elapsed_ms_;
    coast_ms_ = 0;
    phase_ = kCoasting;
}

void MotorRun::onPulse()
{
    uint8_t p = phase_;
    if (p != kRunning && p != kCoasting) {
        return;
    }
    uint32_t n = pulses_ + 1u;
    pulses_ = n;
    if (p == kRunning) {
        last_pulse_ms_ = elapsed_ms_;
        if (params_.target_pulses != 0 && n >= params_.target_pulses && !target_hit_) {
            /* switch off right away for accuracy, finish() follows on the
             * next sample */
            port_.disableAll();
            target_hit_ = true;
        }
    }
}

void MotorRun::onSample(int16_t current_ma10)
{
    uint8_t p = phase_;
    if (p == kCoasting) {
        if (++coast_ms_ >= kCoastMs) {
            releaseFence();
            phase_ = kDone;
        }
        return;
    }
    if (p != kRunning) {
        return;
    }

    elapsed_ms_++;

    /* first-order IIR, time constant ~16 ms */
    int32_t f = filtered_x16_;
    f += static_cast<int32_t>(current_ma10) - f / 16;
    filtered_x16_ = f;
    uint16_t filt = absMa10(f / 16);
    uint16_t raw = absMa10(current_ma10);

    if (target_hit_) {
        finish(RunOutcome::TargetReached);
        return;
    }
    if (abort_request_) {
        finish(RunOutcome::Aborted);
        return;
    }

    /* hard limit: active from the very first sample, raw value */
    if (raw >= kHardLimitMa10) {
        if (++hard_cnt_ >= kHardLimitSamples) {
            finish(RunOutcome::Overcurrent);
            return;
        }
    } else {
        hard_cnt_ = 0;
    }

    if (elapsed_ms_ > params_.inrush_blank_ms) {
        sum_abs_ += filt;
        sum_cnt_++;
        if (filt > peak_) {
            peak_ = filt;
        }
        if (params_.ref_current_ma10 == 0 && sum_cnt_ == kDynRefSamples) {
            setThreshold(static_cast<uint16_t>(sum_abs_ / sum_cnt_));
        }

        if (filt > threshold_ma10_) {
            if (++endstop_cnt_ >= kEndstopSamples) {
                finish(RunOutcome::Endstop);
                return;
            }
        } else {
            endstop_cnt_ = 0;
        }

        if (filt < params_.opencircuit_ma10) {
            if (++low_ms_ >= params_.opencircuit_ms) {
                finish(RunOutcome::OpenCircuit);
                return;
            }
        } else {
            low_ms_ = 0;
        }

        uint32_t since =
            elapsed_ms_ -
            (last_pulse_ms_ > params_.inrush_blank_ms ? last_pulse_ms_ : params_.inrush_blank_ms);
        if (since > kStallMs && filt >= params_.opencircuit_ma10) {
            bool stall = params_.target_pulses == 0 || pulses_ > 0;
            finish(stall ? RunOutcome::Endstop : RunOutcome::NoPulses);
            return;
        }
    }

    if (elapsed_ms_ >= params_.timeout_ms) {
        finish(RunOutcome::Timeout);
    }
}

} // namespace vdm
