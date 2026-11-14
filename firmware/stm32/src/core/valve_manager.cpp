/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/valve_manager.h"

namespace vdm
{

namespace
{

int32_t pulsesToPpm(uint32_t pulses, uint32_t counts)
{
    if (counts == 0) {
        return 0;
    }
    int64_t v = static_cast<int64_t>(pulses) * ValveManager::kFull / counts;
    return v > ValveManager::kFull ? ValveManager::kFull : static_cast<int32_t>(v);
}

uint32_t ppmToPulses(int32_t ppm, uint32_t counts)
{
    int64_t v = static_cast<int64_t>(ppm) * counts / ValveManager::kFull;
    return v < 1 ? 1u : static_cast<uint32_t>(v);
}

int32_t clampPpm(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    return v > ValveManager::kFull ? ValveManager::kFull : v;
}

uint8_t faultFor(RunOutcome o)
{
    switch (o) {
    case RunOutcome::OpenCircuit:
        return VDM_FAULT_OPEN_CIRCUIT;
    case RunOutcome::Overcurrent:
        return VDM_FAULT_OVERCURRENT;
    case RunOutcome::Timeout:
        return VDM_FAULT_TIMEOUT;
    case RunOutcome::NoPulses:
        return VDM_FAULT_NO_PULSES;
    default:
        return VDM_FAULT_NONE;
    }
}

} // namespace

ValveManager::ValveManager(MotorPort &motor, MotorRun &run)
    : motor_(motor), run_(run), motor_params_(vdm_motor_params_default)
{
}

/* ---------------------------------------------------------------------- */
/* Configuration                                                           */
/* ---------------------------------------------------------------------- */

void ValveManager::setRecalibration(uint16_t after_moves, uint8_t after_days)
{
    recal_moves_ = after_moves;
    recal_days_ = after_days;
}

void ValveManager::setEnabledMask(uint16_t mask)
{
    mask &= VDM_CHANNEL_MASK_ALL;
    for (uint8_t ch = 0; ch < kChannels; ch++) {
        bool was = enabled(ch);
        bool now = (mask >> ch) & 1u;
        if (was == now) {
            continue;
        }
        Channel &c = ch_[ch];
        if (now) {
            c.mode = VDM_MODE_UNKNOWN;
            c.want_detect = true;
        } else {
            if (step_ != Step::Idle && job_ch_ == ch) {
                stop(static_cast<uint16_t>(1u << ch));
            }
            c.want_detect = c.want_calib = c.want_reference = false;
            c.retry_armed = false;
        }
        markChanged(ch);
    }
    enabled_mask_ = mask;
}

void ValveManager::setPositionHint(uint8_t ch, uint16_t permille)
{
    if (ch < kChannels) {
        ch_[ch].near_open = permille >= VDM_POS_MAX / 2u;
    }
}

void ValveManager::restore(uint8_t ch, const ChannelPersist &p)
{
    if (ch < kChannels) {
        ch_[ch].p = p;
    }
}

uint16_t ValveManager::takePersistDirty(uint16_t *urgent)
{
    uint16_t d = persist_dirty_;
    if (urgent != nullptr) {
        *urgent = persist_urgent_;
    }
    persist_dirty_ = 0;
    persist_urgent_ = 0;
    return d;
}

/* ---------------------------------------------------------------------- */
/* Commands                                                                */
/* ---------------------------------------------------------------------- */

void ValveManager::setTarget(uint8_t ch, uint16_t target, bool failsafe)
{
    if (ch >= kChannels) {
        return;
    }
    if (target > VDM_POS_MAX) {
        target = VDM_POS_MAX;
    }
    Channel &c = ch_[ch];
    if (c.target != target || c.failsafe != failsafe) {
        c.target = target;
        c.failsafe = failsafe;
        markChanged(ch);
    }
}

void ValveManager::calibrate(uint16_t mask)
{
    for (uint8_t ch = 0; ch < kChannels; ch++) {
        if (((mask >> ch) & 1u) && enabled(ch)) {
            Channel &c = ch_[ch];
            c.want_calib = true;
            if (c.mode == VDM_MODE_NOT_CONNECTED || c.mode == VDM_MODE_UNKNOWN) {
                c.want_detect = true;
            }
            markChanged(ch);
        }
    }
}

void ValveManager::detect(uint16_t mask)
{
    for (uint8_t ch = 0; ch < kChannels; ch++) {
        if (((mask >> ch) & 1u) && enabled(ch)) {
            ch_[ch].want_detect = true;
            markChanged(ch);
        }
    }
}

void ValveManager::stop(uint16_t mask)
{
    for (uint8_t ch = 0; ch < kChannels; ch++) {
        if (!((mask >> ch) & 1u)) {
            continue;
        }
        Channel &c = ch_[ch];
        c.want_detect = c.want_calib = c.want_reference = false;
        if (step_ != Step::Idle && job_ch_ == ch) {
            if (step_ == Step::Running) {
                run_.abort();
                stop_requested_ = true;
            } else {
                /* not moving yet: cancel right away */
                motor_.disableAll();
                step_ = Step::Idle;
                job_ = Job::None;
            }
        }
        if (c.pos_known) {
            c.target = actualPermille(c);
        }
        markChanged(ch);
    }
}

/* ---------------------------------------------------------------------- */
/* Status                                                                  */
/* ---------------------------------------------------------------------- */

uint16_t ValveManager::actualPermille(const Channel &c) const
{
    if (!c.pos_known) {
        return VDM_POS_UNKNOWN;
    }
    return static_cast<uint16_t>((c.pos_ppm + 500) / 1000);
}

int32_t ValveManager::livePpm(uint8_t ch) const
{
    const Channel &c = ch_[ch];
    if (step_ != Step::Running || job_ch_ != ch || !c.pos_known || !c.p.calibrated) {
        return c.pos_ppm;
    }
    uint32_t counts = job_dir_ == Dir::Open ? c.p.open_counts : c.p.close_counts;
    int32_t d = pulsesToPpm(run_.livePulses(), counts);
    return clampPpm(job_dir_ == Dir::Open ? run_start_ppm_ + d : run_start_ppm_ - d);
}

bool ValveManager::wantsMove(const Channel &c) const
{
    if (!c.p.calibrated || !c.pos_known) {
        return false;
    }
    if (c.target == 0) {
        return !(c.at_endstop && c.pos_ppm == 0);
    }
    if (c.target == VDM_POS_MAX) {
        return !(c.at_endstop && c.pos_ppm == kFull);
    }
    int32_t d = static_cast<int32_t>(c.target) * 1000 - c.pos_ppm;
    if (d < 0) {
        d = -d;
    }
    return d >= static_cast<int32_t>(kMinStep) * 1000;
}

bool ValveManager::hasWork(const Channel &c) const
{
    return c.want_detect || c.want_calib || c.want_reference || wantsMove(c);
}

uint8_t ValveManager::reportedMode(uint8_t ch) const
{
    if (!enabled(ch)) {
        return VDM_MODE_DISABLED;
    }
    const Channel &c = ch_[ch];
    if (step_ != Step::Idle && job_ch_ == ch) {
        if (job_ == Job::Calibrate) {
            return VDM_MODE_CALIBRATING;
        }
        if (step_ == Step::Running) {
            return job_dir_ == Dir::Open ? VDM_MODE_OPENING : VDM_MODE_CLOSING;
        }
        return VDM_MODE_QUEUED;
    }
    if (c.mode != VDM_MODE_IDLE) {
        return c.mode;
    }
    return hasWork(c) ? VDM_MODE_QUEUED : VDM_MODE_IDLE;
}

uint8_t ValveManager::flags(uint8_t ch) const
{
    const Channel &c = ch_[ch];
    uint8_t f = 0;
    if (c.p.calibrated) {
        f |= VDM_VFLAG_CALIBRATED;
    }
    if (c.want_calib) {
        f |= VDM_VFLAG_CALIB_PENDING;
    }
    if (!c.pos_known) {
        f |= VDM_VFLAG_POS_UNCERTAIN;
    }
    if (c.at_endstop) {
        f |= VDM_VFLAG_AT_ENDSTOP;
    }
    if (c.failsafe) {
        f |= VDM_VFLAG_FAILSAFE;
    }
    return f;
}

vdm_valve_state_t ValveManager::state(uint8_t ch) const
{
    vdm_valve_state_t s{};
    if (ch >= kChannels) {
        return s;
    }
    const Channel &c = ch_[ch];
    s.channel = ch;
    s.mode = reportedMode(ch);
    s.flags = flags(ch);
    s.fault = c.mode == VDM_MODE_FAULT ? c.fault : static_cast<uint8_t>(VDM_FAULT_NONE);
    s.actual = c.pos_known ? static_cast<uint16_t>((livePpm(ch) + 500) / 1000) : VDM_POS_UNKNOWN;
    s.target = c.target;
    if (step_ == Step::Running && job_ch_ == ch) {
        int16_t i = run_.liveCurrentMa10();
        s.current_ma10 = static_cast<uint16_t>(i < 0 ? -i : i);
    } else {
        s.current_ma10 = c.last_ma10;
    }
    return s;
}

vdm_valve_detail_t ValveManager::detail(uint8_t ch) const
{
    vdm_valve_detail_t d{};
    if (ch >= kChannels) {
        return d;
    }
    const Channel &c = ch_[ch];
    d.state = state(ch);
    d.open_counts = c.p.open_counts;
    d.close_counts = c.p.close_counts;
    d.deadzone_counts =
        static_cast<int32_t>(c.p.close_counts) - static_cast<int32_t>(c.p.open_counts);
    d.calib_age_h = c.p.calibrated ? op_hours_ - c.p.calib_op_hours : 0xFFFFFFFFu;
    d.moves_since_calib = c.p.moves_since_calib;
    d.moves_total = c.p.moves_total;
    d.mean_current_open_ma10 = c.p.mean_open_ma10;
    d.mean_current_close_ma10 = c.p.mean_close_ma10;
    d.peak_current_ma10 = c.peak_ma10;
    d.last_move_counts = c.last_move_counts;
    d.last_move_ms = c.last_move_ms;
    d.calib_retries = c.calib_retries;
    d.last_fault = c.last_fault;
    return d;
}

uint16_t ValveManager::takeChanged()
{
    uint16_t c = changed_;
    changed_ = 0;
    return c;
}

void ValveManager::pushCalibrationEvent(uint8_t ch, uint8_t result, uint32_t open_counts,
                                        uint32_t close_counts, uint8_t retries)
{
    if (cal_count_ == kCalEventQueue) {
        /* drop the oldest */
        cal_head_ = static_cast<uint8_t>((cal_head_ + 1u) % kCalEventQueue);
        cal_count_--;
    }
    uint8_t idx = static_cast<uint8_t>((cal_head_ + cal_count_) % kCalEventQueue);
    cal_events_[idx] = {ch, result, open_counts, close_counts, retries};
    cal_count_++;
}

bool ValveManager::popCalibrationEvent(vdm_evt_calibration_t &evt)
{
    if (cal_count_ == 0) {
        return false;
    }
    evt = cal_events_[cal_head_];
    cal_head_ = static_cast<uint8_t>((cal_head_ + 1u) % kCalEventQueue);
    cal_count_--;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Scheduling                                                              */
/* ---------------------------------------------------------------------- */

ValveManager::Job ValveManager::nextJob(uint8_t ch, uint32_t now_ms)
{
    if (!enabled(ch)) {
        return Job::None;
    }
    Channel &c = ch_[ch];

    if (c.want_detect) {
        return Job::Detect;
    }
    switch (c.mode) {
    case VDM_MODE_UNKNOWN:
        c.want_detect = true;
        return Job::Detect;
    case VDM_MODE_NOT_CONNECTED:
        if (c.retry_armed && static_cast<int32_t>(now_ms - c.retry_at_ms) >= 0) {
            c.retry_armed = false;
            c.want_detect = true;
            return Job::Detect;
        }
        return Job::None;
    case VDM_MODE_FAULT:
        if (c.retry_armed && static_cast<int32_t>(now_ms - c.retry_at_ms) >= 0) {
            c.retry_armed = false;
            c.want_calib = true;
        }
        return c.want_calib ? Job::Calibrate : Job::None;
    default:
        break;
    }

    if (!c.p.calibrated) {
        c.want_calib = true;
    } else if (recal_moves_ != 0 && c.p.moves_since_calib >= recal_moves_) {
        c.want_calib = true;
    } else if (recal_days_ != 0 && op_hours_ - c.p.calib_op_hours >= recal_days_ * 24u) {
        c.want_calib = true;
    }
    if (c.want_calib) {
        return Job::Calibrate;
    }
    if (c.want_reference || !c.pos_known) {
        return Job::Reference;
    }
    return wantsMove(c) ? Job::Move : Job::None;
}

bool ValveManager::setupJob(uint8_t ch, Job job)
{
    Channel &c = ch_[ch];
    job_ = job;
    job_ch_ = ch;
    stop_requested_ = false;

    switch (job) {
    case Job::Detect:
        /* away from the endstop the valve is probably at */
        job_dir_ = (c.near_open != c.detect_second) ? Dir::Close : Dir::Open;
        run_target_pulses_ = kDetectPulses;
        break;
    case Job::Calibrate:
        c.calib_retries = 0;
        cal_phase_ = CalPhase::Home;
        job_dir_ = Dir::Open;
        run_target_pulses_ = 0;
        break;
    case Job::Reference:
        /* to the nearer endstop */
        job_dir_ = c.near_open ? Dir::Open : Dir::Close;
        run_target_pulses_ = 0;
        break;
    case Job::Move:
        if (c.target == 0) {
            job_dir_ = Dir::Close;
            run_target_pulses_ = 0;
        } else if (c.target == VDM_POS_MAX) {
            job_dir_ = Dir::Open;
            run_target_pulses_ = 0;
        } else {
            int32_t d = static_cast<int32_t>(c.target) * 1000 - c.pos_ppm;
            job_dir_ = d > 0 ? Dir::Open : Dir::Close;
            uint32_t counts = d > 0 ? c.p.open_counts : c.p.close_counts;
            run_target_pulses_ = ppmToPulses(d > 0 ? d : -d, counts);
        }
        break;
    case Job::None:
        return false;
    }
    return true;
}

void ValveManager::pickJob(uint32_t now_ms)
{
    for (uint8_t i = 0; i < kChannels; i++) {
        uint8_t ch = static_cast<uint8_t>((rr_ + i) % kChannels);
        Job job = nextJob(ch, now_ms);
        if (job == Job::None || !setupJob(ch, job)) {
            continue;
        }
        rr_ = static_cast<uint8_t>((ch + 1u) % kChannels);
        if (!psu_on_) {
            motor_.power(true);
            psu_on_ = true;
            psu_on_since_ms_ = now_ms;
            step_ = Step::PowerUp;
        } else {
            motor_.select(ch, job_dir_);
            step_since_ms_ = now_ms;
            step_ = Step::Select;
        }
        markChanged(ch);
        return;
    }
}

void ValveManager::beginRun(uint32_t now_ms)
{
    const Channel &c = ch_[job_ch_];
    RunParams p{};
    bool open = job_dir_ == Dir::Open;
    p.target_pulses = run_target_pulses_;
    p.ref_current_ma10 = c.p.calibrated ? (open ? c.p.mean_open_ma10 : c.p.mean_close_ma10) : 0;
    p.endstop_factor =
        open ? motor_params_.endstop_factor_open : motor_params_.endstop_factor_close;
    p.endstop_abs_ma10 = static_cast<uint16_t>(motor_params_.endstop_abs_ma * 10u);
    p.inrush_blank_ms = motor_params_.inrush_blank_ms;
    p.opencircuit_ma10 = motor_params_.opencircuit_ma10;
    p.opencircuit_ms = motor_params_.opencircuit_ms;
    p.timeout_ms = job_ == Job::Detect ? kDetectTimeoutMs : motor_params_.move_timeout_s * 1000UL;

    run_start_ms_ = now_ms;
    run_start_ppm_ = c.pos_ppm;
    last_live_ms_ = now_ms;
    if (!run_.start(p)) {
        /* cannot happen in normal operation; retry on the next tick */
        return;
    }
    motor_.enable(job_ch_);
    step_ = Step::Running;
    markChanged(job_ch_);
}

void ValveManager::tick(uint32_t now_ms)
{
    switch (step_) {
    case Step::Idle:
        pickJob(now_ms);
        if (step_ == Step::Idle && psu_on_ && now_ms - idle_since_ms_ >= kPsuOffDelayMs) {
            motor_.power(false);
            psu_on_ = false;
        }
        break;

    case Step::PowerUp:
        if (now_ms - psu_on_since_ms_ >= kPsuSettleMs) {
            motor_.select(job_ch_, job_dir_);
            step_since_ms_ = now_ms;
            step_ = Step::Select;
        }
        break;

    case Step::Select:
        if (now_ms - step_since_ms_ >= kMuxSettleMs) {
            beginRun(now_ms);
        }
        break;

    case Step::Running:
        if (run_.done()) {
            RunResult r = run_.result();
            run_.release();
            onRunDone(r, now_ms);
        } else if (now_ms - last_live_ms_ >= kLiveEventMs) {
            last_live_ms_ = now_ms;
            markChanged(job_ch_);
        }
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Results                                                                 */
/* ---------------------------------------------------------------------- */

void ValveManager::applyPulses(Channel &c, Dir dir, uint32_t pulses)
{
    if (!c.p.calibrated || !c.pos_known || pulses == 0) {
        return;
    }
    uint32_t counts = dir == Dir::Open ? c.p.open_counts : c.p.close_counts;
    int32_t d = pulsesToPpm(pulses, counts);
    c.pos_ppm = clampPpm(dir == Dir::Open ? c.pos_ppm + d : c.pos_ppm - d);
    c.at_endstop = false;
}

void ValveManager::enterFault(uint8_t ch, uint8_t fault, uint32_t now_ms)
{
    Channel &c = ch_[ch];
    c.mode = VDM_MODE_FAULT;
    c.fault = fault;
    c.last_fault = fault;
    c.pos_known = false;
    c.at_endstop = false;
    c.retry_at_ms = now_ms + kFaultRetryMs;
    c.retry_armed = true;
    markChanged(ch);
}

void ValveManager::enterNotConnected(uint8_t ch, uint32_t now_ms)
{
    Channel &c = ch_[ch];
    c.fault = VDM_FAULT_NONE;
    c.last_fault = VDM_FAULT_OPEN_CIRCUIT;
    c.pos_known = false;
    c.at_endstop = false;
    c.want_detect = false;
    /* back-off while the channel stays empty */
    if (c.mode == VDM_MODE_NOT_CONNECTED && c.redetect_ms != 0) {
        c.redetect_ms = c.redetect_ms * 2u > kRedetectMaxMs ? kRedetectMaxMs : c.redetect_ms * 2u;
    } else {
        c.redetect_ms = kRedetectMinMs;
    }
    c.mode = VDM_MODE_NOT_CONNECTED;
    c.retry_at_ms = now_ms + c.redetect_ms;
    c.retry_armed = true;
    markChanged(ch);
}

void ValveManager::handleFailure(const RunResult &r, uint32_t now_ms)
{
    Channel &c = ch_[job_ch_];
    applyPulses(c, job_dir_, r.pulses);
    if (r.outcome == RunOutcome::OpenCircuit) {
        enterNotConnected(job_ch_, now_ms);
    } else if (r.outcome != RunOutcome::Aborted) {
        enterFault(job_ch_, faultFor(r.outcome), now_ms);
    }
}

void ValveManager::onRunDone(const RunResult &r, uint32_t now_ms)
{
    Channel &c = ch_[job_ch_];
    if (r.mean_ma10 > 0) {
        c.last_ma10 = r.mean_ma10;
    }
    c.peak_ma10 = r.peak_ma10;
    c.last_move_ms = r.duration_ms;
    c.last_move_counts =
        job_dir_ == Dir::Open ? static_cast<int32_t>(r.pulses) : -static_cast<int32_t>(r.pulses);

    bool continue_job = false;
    bool handled = false;
    if (stop_requested_ || r.outcome == RunOutcome::Aborted) {
        applyPulses(c, job_dir_, r.pulses);
        if (job_ == Job::Calibrate) {
            c.want_calib = false;
        }
        if (c.pos_known) {
            c.target = actualPermille(c);
        }
    } else {
        switch (job_) {
        case Job::Detect:
            handled = finishDetect(r);
            break;
        case Job::Move:
            finishMove(r);
            break;
        case Job::Reference:
            finishReference(r);
            break;
        case Job::Calibrate:
            continue_job = finishCalibrationStep(r, now_ms);
            break;
        case Job::None:
            break;
        }
        /* calibration failures are handled by finishCalibrationStep() */
        if (!continue_job && !handled && job_ != Job::Calibrate &&
            faultFor(r.outcome) != VDM_FAULT_NONE) {
            handleFailure(r, now_ms);
        }
    }

    if (continue_job) {
        motor_.select(job_ch_, job_dir_);
        step_since_ms_ = now_ms;
        step_ = Step::Select;
        markChanged(job_ch_);
        return;
    }
    stop_requested_ = false;
    step_ = Step::Idle;
    job_ = Job::None;
    idle_since_ms_ = now_ms;
    markChanged(job_ch_);
}

bool ValveManager::finishDetect(const RunResult &r)
{
    Channel &c = ch_[job_ch_];
    c.want_detect = false;
    bool first = !c.detect_second;
    c.detect_second = false;
    if (r.outcome == RunOutcome::TargetReached && r.peak_all_ma10 < kDetectMinPeakMa10) {
        /* pulses without current: interference, no motor */
        enterNotConnected(job_ch_, run_start_ms_ + r.duration_ms);
        return true;
    }
    if (r.outcome == RunOutcome::TargetReached) {
        c.mode = VDM_MODE_IDLE;
        c.fault = VDM_FAULT_NONE;
        applyPulses(c, job_dir_, r.pulses);
        if (!c.p.calibrated) {
            c.want_calib = true;
        } else if (!c.pos_known) {
            c.want_reference = true;
        }
        return true;
    }
    if (first && (r.outcome == RunOutcome::NoPulses || r.outcome == RunOutcome::Endstop)) {
        /* possibly sitting at that endstop: try the other direction */
        c.want_detect = true;
        c.detect_second = true;
        return true;
    }
    if (r.outcome == RunOutcome::Endstop) {
        /* stalled in both directions without a single pulse */
        enterFault(job_ch_, VDM_FAULT_NO_PULSES, run_start_ms_ + r.duration_ms);
        return true;
    }
    return false; /* other failures are handled by onRunDone() */
}

void ValveManager::finishMove(const RunResult &r)
{
    Channel &c = ch_[job_ch_];
    if (r.outcome == RunOutcome::TargetReached) {
        applyPulses(c, job_dir_, r.pulses);
    } else if (r.outcome == RunOutcome::Endstop) {
        bool expected = (job_dir_ == Dir::Close && c.target == 0) ||
                        (job_dir_ == Dir::Open && c.target == VDM_POS_MAX);
        if (!expected) {
            /* where did we think we were when the endstop was hit? */
            uint32_t counts = job_dir_ == Dir::Open ? c.p.open_counts : c.p.close_counts;
            int32_t d = pulsesToPpm(r.pulses, counts);
            int32_t est = clampPpm(job_dir_ == Dir::Open ? c.pos_ppm + d : c.pos_ppm - d);
            int32_t dist = job_dir_ == Dir::Close ? est : kFull - est;
            if (dist > kBlockedMarginPpm) {
                c.blocked_cnt++;
                c.want_calib = true;
            }
        } else {
            c.blocked_cnt = 0;
        }
        c.pos_ppm = job_dir_ == Dir::Close ? 0 : kFull;
        c.at_endstop = true;
    } else {
        return; /* failure, handled by onRunDone() */
    }

    if (r.outcome == RunOutcome::TargetReached) {
        c.blocked_cnt = 0;
    }
    c.p.moves_total++;
    c.p.moves_since_calib++;
    persist_dirty_ |= static_cast<uint16_t>(1u << job_ch_);

    /* track the running current to follow ageing of the motor; bounded so
     * that a bad run cannot ratchet the endstop threshold up */
    if (r.mean_ma10 > 0 && r.duration_ms > motor_params_.inrush_blank_ms + 500u) {
        uint16_t lo = motor_params_.opencircuit_ma10;
        uint16_t hi = static_cast<uint16_t>(motor_params_.endstop_abs_ma * 5u); /* abs/2 */
        uint16_t m = r.mean_ma10 < lo ? lo : (r.mean_ma10 > hi ? hi : r.mean_ma10);
        uint16_t &ref = job_dir_ == Dir::Open ? c.p.mean_open_ma10 : c.p.mean_close_ma10;
        ref = ref == 0 ? m : static_cast<uint16_t>((ref * 7u + m) / 8u);
    }

    if (c.blocked_cnt >= 2) {
        c.blocked_cnt = 0;
        c.want_calib = false;
        enterFault(job_ch_, VDM_FAULT_BLOCKED, run_start_ms_ + r.duration_ms);
    }
}

void ValveManager::finishReference(const RunResult &r)
{
    Channel &c = ch_[job_ch_];
    if (r.outcome != RunOutcome::Endstop) {
        return; /* failure, handled by onRunDone() */
    }
    bool open = job_dir_ == Dir::Open;
    uint32_t stroke = open ? c.p.open_counts : c.p.close_counts;
    c.want_reference = false;
    c.pos_known = true;
    c.pos_ppm = open ? kFull : 0;
    c.at_endstop = true;
    if (c.p.calibrated && r.pulses > stroke + stroke * 15u / 100u + 200u) {
        c.last_fault = VDM_FAULT_COUNT_MISMATCH;
        c.want_calib = true;
    }
}

bool ValveManager::calibrationFailed(uint8_t fault, uint32_t now_ms)
{
    Channel &c = ch_[job_ch_];
    if (c.calib_retries < motor_params_.max_calib_retries) {
        c.calib_retries++;
        cal_phase_ = CalPhase::Home;
        job_dir_ = Dir::Open;
        run_target_pulses_ = 0;
        return true;
    }
    c.want_calib = false;
    pushCalibrationEvent(job_ch_, fault, 0, 0, c.calib_retries);
    if (fault == VDM_FAULT_OPEN_CIRCUIT) {
        c.want_calib = true; /* calibrate again once it is reconnected */
        enterNotConnected(job_ch_, now_ms);
    } else {
        enterFault(job_ch_, fault, now_ms);
    }
    return false;
}

bool ValveManager::finishCalibrationStep(const RunResult &r, uint32_t now_ms)
{
    Channel &c = ch_[job_ch_];

    if (r.outcome != RunOutcome::Endstop) {
        uint8_t fault = faultFor(r.outcome);
        if (fault == VDM_FAULT_NONE) {
            fault = VDM_FAULT_CALIB_FAILED;
        }
        if (fault == VDM_FAULT_OPEN_CIRCUIT) {
            /* no retries for a missing motor */
            c.calib_retries = motor_params_.max_calib_retries;
        }
        return calibrationFailed(fault, now_ms);
    }

    switch (cal_phase_) {
    case CalPhase::Home:
        cal_phase_ = CalPhase::Close;
        job_dir_ = Dir::Close;
        return true;

    case CalPhase::Close:
        cal_close_counts_ = r.pulses;
        cal_close_mean_ = r.mean_ma10;
        cal_phase_ = CalPhase::Open;
        job_dir_ = Dir::Open;
        return true;

    case CalPhase::Open:
        break;
    }

    uint32_t open_counts = r.pulses;
    if (open_counts < motor_params_.min_stroke_counts ||
        cal_close_counts_ < motor_params_.min_stroke_counts) {
        return calibrationFailed(VDM_FAULT_CALIB_FAILED, now_ms);
    }

    c.p.open_counts = open_counts;
    c.p.close_counts = cal_close_counts_;
    c.p.mean_open_ma10 = r.mean_ma10;
    c.p.mean_close_ma10 = cal_close_mean_;
    c.p.calibrated = 1;
    c.p.calib_op_hours = op_hours_;
    c.p.moves_since_calib = 0;
    c.mode = VDM_MODE_IDLE;
    c.fault = VDM_FAULT_NONE;
    c.want_calib = false;
    c.want_reference = false;
    c.blocked_cnt = 0;
    c.pos_known = true;
    c.pos_ppm = kFull; /* ends at the open endstop */
    c.at_endstop = true;
    persist_dirty_ |= static_cast<uint16_t>(1u << job_ch_);
    persist_urgent_ |= static_cast<uint16_t>(1u << job_ch_);
    pushCalibrationEvent(job_ch_, 0, c.p.open_counts, c.p.close_counts, c.calib_retries);
    return false;
}

} // namespace vdm
