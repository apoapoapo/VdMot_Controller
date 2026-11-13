/*
 * Valve and motor driver simulator for host tests.
 *
 * Each channel has a mechanical position x in [0, 1]. An opening pulse
 * moves it by 1/open_stroke, a closing pulse by 1/close_stroke, so the
 * strokes are asymmetric like on the real valves. At an endstop the motor
 * draws the endstop current and produces no pulses. After the driver is
 * switched off the motor coasts for a few more pulses.
 *
 * The simulator also checks the driver sequencing rules.
 */
#pragma once

#include <math.h>
#include <unity.h>

#include "core/motor_run.h"
#include "core/ports.h"
#include "core/valve_manager.h"

namespace sim
{

struct Valve {
    bool connected = true;
    bool no_pulses = false;    /* motor turns but the back-EMF circuit sees nothing */
    bool endless = false;      /* broken coupling: never reaches an endstop */
    double x = 0.3;            /* mechanical position */
    double open_stroke = 6000; /* pulses 0 -> 1 */
    double close_stroke = 5000;
    double obstruction = 2.0; /* x where an obstruction stops opening (>1: none) */
    int16_t run_ma10 = 200;
    int16_t end_ma10 = 600;
    double pulses_per_ms = 0.5;
    uint32_t moves = 0;
};

class Board : public vdm::MotorPort
{
  public:
    static constexpr int kChannels = vdm::ValveManager::kChannels;
    Valve v[kChannels];

    bool psu = false;
    int selected = -1;
    vdm::Dir dir = vdm::Dir::Close;
    int enabled = -1;
    uint32_t now = 0;
    uint32_t select_ms = 0;
    uint32_t psu_on_ms = 0;
    int coast_left = 0;
    int coast_ch = -1;
    double acc = 0;
    uint32_t enabled_ms = 0;
    uint32_t violations = 0;

    void power(bool on) override
    {
        if (!on && enabled >= 0) {
            violations++; /* PSU off while a motor runs */
        }
        if (on && !psu) {
            psu_on_ms = now;
        }
        psu = on;
    }
    void select(uint8_t ch, vdm::Dir d) override
    {
        if (enabled >= 0) {
            violations++; /* MUX switched under load */
        }
        selected = ch;
        dir = d;
        select_ms = now;
    }
    void enable(uint8_t ch) override
    {
        if (!psu || static_cast<int>(ch) != selected || now - select_ms < 100 ||
            now - psu_on_ms < 1000) {
            violations++;
        }
        enabled = ch;
        enabled_ms = now;
        acc = 0;
        v[ch].moves++;
    }
    void disableAll() override
    {
        if (enabled >= 0) {
            coast_ch = enabled;
            coast_left = 2;
        }
        enabled = -1;
    }

    bool atEnd(const Valve &s) const
    {
        if (s.endless) {
            return false;
        }
        return dir == vdm::Dir::Open ? (s.x >= 1.0 || s.x >= s.obstruction) : s.x <= 0.0;
    }

    void movePulse(Valve &s)
    {
        if (dir == vdm::Dir::Open) {
            s.x = fmin(fmin(1.0, s.obstruction), s.x + 1.0 / s.open_stroke);
        } else {
            s.x = fmax(0.0, s.x - 1.0 / s.close_stroke);
        }
    }

    /* advances 1 ms; returns the current and the number of pulses */
    int16_t step(int *pulses)
    {
        now++;
        *pulses = 0;
        if (enabled < 0) {
            if (coast_left > 0 && coast_ch >= 0) {
                Valve &s = v[coast_ch];
                if (s.connected && !atEnd(s) && (now % 5) == 0) {
                    movePulse(s);
                    coast_left--;
                    if (!s.no_pulses) {
                        (*pulses)++;
                    }
                }
            }
            return 0;
        }
        Valve &s = v[enabled];
        if (!psu || !s.connected) {
            return 0;
        }
        int16_t i;
        if (atEnd(s)) {
            i = s.end_ma10;
        } else {
            i = now - enabled_ms < 80 ? 900 : s.run_ma10; /* inrush */
            acc += s.pulses_per_ms;
            while (acc >= 1.0) {
                acc -= 1.0;
                movePulse(s);
                if (!s.no_pulses) {
                    (*pulses)++;
                }
                if (atEnd(s)) {
                    break;
                }
            }
        }
        return dir == vdm::Dir::Open ? i : static_cast<int16_t>(-i);
    }
};

struct Rig {
    Board board;
    vdm::MotorRun run{board};
    vdm::ValveManager vm{board, run};
    uint32_t now = 0;

    explicit Rig(uint16_t enabled = 0x0001)
    {
        vdm_motor_params_t p = vdm_motor_params_default;
        p.min_stroke_counts = 2000;
        p.move_timeout_s = 60;
        vm.setMotorParams(p);
        vm.setEnabledMask(enabled);
    }

    void step()
    {
        int pulses;
        int16_t i = board.step(&pulses);
        for (int k = 0; k < pulses; k++) {
            run.onPulse();
        }
        run.onSample(i);
        now++;
        vm.tick(now);
    }

    void runMs(uint32_t ms)
    {
        for (uint32_t k = 0; k < ms; k++) {
            step();
        }
    }

    /* runs until the motor path is idle and no channel has work left */
    bool settle(uint32_t max_ms = 30u * 60u * 1000u)
    {
        uint32_t quiet = 0;
        for (uint32_t k = 0; k < max_ms; k++) {
            step();
            quiet = vm.motorBusy() ? 0 : quiet + 1;
            if (quiet > 200) {
                return true;
            }
        }
        return false;
    }
};

} // namespace sim
