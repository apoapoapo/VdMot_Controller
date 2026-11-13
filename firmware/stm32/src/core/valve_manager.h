/*
 * ValveManager - state of all channels and the scheduler for the single
 * motor driver path (only one motor moves at a time).
 *
 * Position model: the position is kept as a fraction of the full stroke in
 * parts per million (0 = closed endstop, 1'000'000 = open endstop). Opening
 * pulses are scaled with the learned opening stroke, closing pulses with the
 * learned closing stroke, so the mechanical asymmetry of the valve does not
 * cause drift. Moves to 0 % and 100 % always run into the endstop and
 * re-reference the position.
 *
 * Jobs per channel, in order of priority:
 *   DETECT     short opening move (a few pulses) to check the connection
 *   CALIBRATE  close to endstop, open to endstop (count), close (count)
 *   REFERENCE  close to endstop to establish the position (after boot)
 *   MOVE       go to the target
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <stdint.h>

#include "core/motor_run.h"
#include "core/ports.h"
#include "vdm/link.h"

namespace vdm
{

/* Calibration and statistics of one channel; persisted in the EEPROM. */
struct ChannelPersist {
    uint32_t open_counts = 0;  /* pulses for 0 -> 100 % */
    uint32_t close_counts = 0; /* pulses for 100 -> 0 % */
    uint16_t mean_open_ma10 = 0;
    uint16_t mean_close_ma10 = 0;
    uint32_t calib_op_hours = 0; /* operating hours at the last calibration */
    uint32_t moves_since_calib = 0;
    uint32_t moves_total = 0;
    uint8_t calibrated = 0;
};

class ValveManager
{
  public:
    static constexpr uint8_t kChannels = VDM_MAX_CHANNELS;
    static constexpr int32_t kFull = 1000000; /* ppm */
    static constexpr uint32_t kPsuSettleMs = 1000;
    static constexpr uint32_t kPsuOffDelayMs = 10000;
    static constexpr uint32_t kMuxSettleMs = 100;
    static constexpr uint16_t kMinStep = 5;       /* 0.5 %: smaller moves are skipped */
    static constexpr uint32_t kDetectPulses = 30; /* ~0.2 % of a stroke */
    /* a detection move only counts if the motor drew at least this current
     * (0.1 mA, inrush included): an empty channel can see interference
     * pulses on the back-EMF input */
    static constexpr uint16_t kDetectMinPeakMa10 = 50;
    static constexpr uint32_t kDetectTimeoutMs = 3000;
    static constexpr uint32_t kRedetectMs = 10UL * 60UL * 1000UL; /* not connected */
    static constexpr uint32_t kFaultRetryMs = 6UL * 60UL * 60UL * 1000UL;
    static constexpr int32_t kBlockedMarginPpm = 150000; /* 15 % */
    static constexpr uint32_t kLiveEventMs = 500;

    ValveManager(MotorPort &motor, MotorRun &run);

    /* --- configuration ------------------------------------------------- */

    void setMotorParams(const vdm_motor_params_t &p)
    {
        motor_params_ = p;
    }
    void setRecalibration(uint16_t after_moves, uint8_t after_days);
    void setEnabledMask(uint16_t mask);
    uint16_t enabledMask() const
    {
        return enabled_mask_;
    }
    void setOperatingHours(uint32_t hours)
    {
        op_hours_ = hours;
    }
    /* Where the valve probably is after a restart (0.1 %, e.g. the last
     * stored target): the reference run goes to the nearer endstop and the
     * detection move goes the other way. */
    void setPositionHint(uint8_t ch, uint16_t permille);

    /* Restores persisted data before the first tick(). */
    void restore(uint8_t ch, const ChannelPersist &p);
    const ChannelPersist &persist(uint8_t ch) const
    {
        return ch_[ch].p;
    }
    /* Channels whose persisted data changed since the last call: `urgent`
     * holds channels with new calibration data (write soon), the return
     * value all changed channels (statistics may be written lazily). */
    uint16_t takePersistDirty(uint16_t *urgent);

    /* --- commands ------------------------------------------------------ */

    /* target in 0.1 % (0..1000); `failsafe` marks the target accordingly */
    void setTarget(uint8_t ch, uint16_t target, bool failsafe = false);
    uint16_t target(uint8_t ch) const
    {
        return ch_[ch].target;
    }
    void calibrate(uint16_t mask);
    void detect(uint16_t mask);
    void stop(uint16_t mask);

    /* --- status -------------------------------------------------------- */

    vdm_valve_state_t state(uint8_t ch) const;
    vdm_valve_detail_t detail(uint8_t ch) const;
    /* Channels whose reported state changed (EVT_VALVE), cleared on read. */
    uint16_t takeChanged();
    /* Next calibration result, false if none is pending. */
    bool popCalibrationEvent(vdm_evt_calibration_t &evt);
    bool motorBusy() const
    {
        return step_ != Step::Idle;
    }

    /* --- main loop ----------------------------------------------------- */

    void tick(uint32_t now_ms);

  private:
    enum class Job : uint8_t { None, Detect, Calibrate, Reference, Move };
    enum class Step : uint8_t { Idle, PowerUp, Select, Running };
    enum class CalPhase : uint8_t { Home, Open, Close };

    struct Channel {
        ChannelPersist p;
        uint8_t mode = VDM_MODE_UNKNOWN;
        uint8_t fault = VDM_FAULT_NONE;
        uint8_t last_fault = VDM_FAULT_NONE;
        bool pos_known = false;
        bool at_endstop = false;
        bool failsafe = false;
        int32_t pos_ppm = 0;
        uint16_t target = 0;
        bool want_detect = false;
        bool detect_second = false; /* second attempt in the other direction */
        bool near_open = false;     /* position hint: closer to the open endstop */
        bool want_calib = false;
        bool want_reference = false;
        uint8_t blocked_cnt = 0;
        uint8_t calib_retries = 0;
        uint32_t retry_at_ms = 0;
        bool retry_armed = false;
        uint16_t peak_ma10 = 0;
        uint16_t last_ma10 = 0;
        int32_t last_move_counts = 0;
        uint32_t last_move_ms = 0;
    };

    bool enabled(uint8_t ch) const
    {
        return (enabled_mask_ >> ch) & 1u;
    }
    uint16_t actualPermille(const Channel &c) const;
    int32_t livePpm(uint8_t ch) const;
    uint8_t reportedMode(uint8_t ch) const;
    uint8_t flags(uint8_t ch) const;
    bool wantsMove(const Channel &c) const;
    bool hasWork(const Channel &c) const;
    Job nextJob(uint8_t ch, uint32_t now_ms);
    bool setupJob(uint8_t ch, Job job);
    void pickJob(uint32_t now_ms);
    void beginRun(uint32_t now_ms);
    void onRunDone(const RunResult &r, uint32_t now_ms);
    void finishJob(uint32_t now_ms);
    bool finishDetect(const RunResult &r);
    void finishMove(const RunResult &r);
    void finishReference(const RunResult &r);
    bool finishCalibrationStep(const RunResult &r, uint32_t now_ms);
    bool calibrationFailed(uint8_t fault, uint32_t now_ms);
    void handleFailure(const RunResult &r, uint32_t now_ms);
    void enterFault(uint8_t ch, uint8_t fault, uint32_t now_ms);
    void enterNotConnected(uint8_t ch, uint32_t now_ms);
    void applyPulses(Channel &c, Dir dir, uint32_t pulses);
    void markChanged(uint8_t ch)
    {
        changed_ |= static_cast<uint16_t>(1u << ch);
    }
    void pushCalibrationEvent(uint8_t ch, uint8_t result, uint32_t open_counts,
                              uint32_t close_counts, uint8_t retries);

    MotorPort &motor_;
    MotorRun &run_;
    vdm_motor_params_t motor_params_;
    uint16_t recal_moves_ = 2000;
    uint8_t recal_days_ = 7;
    uint16_t enabled_mask_ = 0;
    uint32_t op_hours_ = 0;

    Channel ch_[kChannels];

    /* active job */
    Step step_ = Step::Idle;
    Job job_ = Job::None;
    uint8_t job_ch_ = 0;
    Dir job_dir_ = Dir::Close;
    CalPhase cal_phase_ = CalPhase::Home;
    uint32_t step_since_ms_ = 0;
    uint32_t run_start_ms_ = 0;
    int32_t run_start_ppm_ = 0;
    uint32_t run_target_pulses_ = 0;
    bool stop_requested_ = false;

    bool psu_on_ = false;
    uint32_t psu_on_since_ms_ = 0;
    uint32_t idle_since_ms_ = 0;
    uint8_t rr_ = 0; /* round-robin start channel */

    uint16_t changed_ = 0;
    uint16_t persist_dirty_ = 0;
    uint16_t persist_urgent_ = 0;
    uint32_t cal_open_counts_ = 0;
    uint16_t cal_open_mean_ = 0;
    uint32_t last_live_ms_ = 0;

    static constexpr uint8_t kCalEventQueue = 4;
    vdm_evt_calibration_t cal_events_[kCalEventQueue];
    uint8_t cal_head_ = 0;
    uint8_t cal_count_ = 0;
};

} // namespace vdm
