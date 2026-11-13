/*
 * Integration test: ESP32 LinkClient <-> STM32 Controller (real firmware
 * core of firmware/stm32) with valve, 1-Wire and EEPROM simulators,
 * connected by a wire that can drop or corrupt bytes.
 */
#include <stdlib.h>
#include <unity.h>

#include <deque>
#include <memory>

#include "core/link_client.h"

/* STM32 core sources (unity build) */
#include "core/controller.cpp"
#include "core/link_server.cpp"
#include "core/motor_run.cpp"
#include "core/onewire.cpp"
#include "core/sensor_manager.cpp"
#include "core/storage.cpp"
#include "core/valve_manager.cpp"
#include "support/fake_eeprom.h"
#include "support/fake_onewire.h"
#include "support/sim.h"

namespace
{

struct Wire {
    std::deque<uint8_t> q;
    uint32_t error_per_mille = 0;
    uint32_t dropped = 0;
    bool cut = false;
    void put(const uint8_t *d, size_t n)
    {
        for (size_t i = 0; i < n; i++) {
            if (cut)
                continue;
            uint8_t b = d[i];
            if (error_per_mille && (uint32_t)(rand() % 1000) < error_per_mille) {
                if (rand() % 2) {
                    dropped++;
                    continue;
                }
                b ^= (uint8_t)(1u << (rand() % 8));
            }
            q.push_back(b);
        }
    }
};

struct ToStm : vdm::ByteSink {
    Wire w;
    void write(const uint8_t *d, size_t n) override
    {
        w.put(d, n);
    }
};
struct ToEsp : vdm::LinkPort {
    Wire w;
    void write(const uint8_t *d, size_t n) override
    {
        w.put(d, n);
    }
};

struct NoSystem : vdm::SystemPort {
    int resets = 0;
    void reset() override
    {
        resets++;
    }
    vdm::PlatformDiag diag() override
    {
        return {0, 0, 0};
    }
    void clearDiag() override {}
};

const vdm::BoardInfo kInfo = {2, 0, 0, 1, VDM_HW_C2, VDM_MCU_F411, {0}, VDM_CAP_DS18B20, 0};

/* the STM32 side; can be "rebooted" by recreating it */
struct Stm {
    sim::Board board;
    vdm::MotorRun run{board};
    vdm::ValveManager vm{board, run};
    vdm::Storage storage;
    sim::FakeBus bus;
    vdm::SensorManager sensors{bus};
    NoSystem system;
    vdm::Controller ctl;
    Stm(sim::FakeEeprom &e, ToEsp &out, double x0)
        : storage(e), ctl(kInfo, vm, sensors, storage, system, out)
    {
        for (int ch = 2; ch < 12; ch++)
            board.v[ch].connected = false;
        board.v[0].x = x0;
        for (uint32_t i = 0; i < 3; i++) {
            sim::OwDevice d = sim::makeDevice(vdm::onewire::kFamilyDs18b20, 0x40 + i);
            d.temp_mc = 21000 + (int32_t)i * 100;
            bus.devs.push_back(d);
        }
    }
};

struct Rig {
    sim::FakeEeprom eeprom;
    ToStm to_stm;
    ToEsp to_esp;
    std::unique_ptr<Stm> stm;
    vdm::LinkClient client{to_stm};
    uint32_t now = 0;
    bool stm_alive = true;

    Rig()
    {
        reboot(0.3);
    }

    void reboot(double x0)
    {
        double x = stm ? stm->board.v[0].x : x0;
        stm.reset(new Stm(eeprom, to_esp, x));
        stm->ctl.begin(now);
    }

    void step()
    {
        now++;
        if (stm_alive) {
            int pulses;
            int16_t i = stm->board.step(&pulses);
            for (int k = 0; k < pulses; k++)
                stm->run.onPulse();
            stm->run.onSample(i);
            /* ~11 bytes per ms at 115200 baud */
            for (int k = 0; k < 11 && !to_stm.w.q.empty(); k++) {
                stm->ctl.onLinkByte(to_stm.w.q.front(), now);
                to_stm.w.q.pop_front();
            }
            stm->ctl.tick(now);
        } else {
            to_stm.w.q.clear();
        }
        for (int k = 0; k < 11 && !to_esp.w.q.empty(); k++) {
            client.onByte(to_esp.w.q.front(), now);
            to_esp.w.q.pop_front();
        }
        client.tick(now);
    }
    void runMs(uint32_t ms)
    {
        for (uint32_t i = 0; i < ms; i++)
            step();
    }
    const vdm::Model &m() const
    {
        return client.model();
    }
};

} // namespace

void setUp(void)
{
    srand(1);
}
void tearDown(void) {}

static void test_handshake_and_initial_data(void)
{
    Rig r;
    r.runMs(3000);
    TEST_ASSERT_EQUAL(vdm::LinkState::Connected, r.m().state);
    TEST_ASSERT_TRUE(r.m().hello_valid);
    TEST_ASSERT_EQUAL_UINT8(VDM_MCU_F411, r.m().hello.mcu);
    TEST_ASSERT_TRUE(r.m().valves_valid);
    TEST_ASSERT_TRUE(r.m().sys_valid);
    TEST_ASSERT_TRUE(r.m().motor_valid);
    TEST_ASSERT_TRUE(r.m().diag_valid);
    TEST_ASSERT_EQUAL_UINT16(1000, r.m().sys.failsafe_pos);
    TEST_ASSERT_EQUAL_UINT8(3, r.m().sensor_total);
    TEST_ASSERT_EQUAL_INT32(21000, r.m().sensors[0].value);
    /* channel 5 has no valve; it is checked after channel 0 is calibrated */
    TEST_ASSERT_NOT_EQUAL(VDM_MODE_IDLE, r.m().valves[5].mode);
    r.runMs(120000);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_NOT_CONNECTED, r.m().valves[5].mode);
    TEST_ASSERT_EQUAL_UINT32(1, r.m().stats.link_ups);
    TEST_ASSERT_EQUAL_UINT32(0, r.m().stats.timeouts);
}

static void test_target_via_link(void)
{
    Rig r;
    r.runMs(2000);
    TEST_ASSERT_TRUE(r.client.setTarget(0, 650));
    r.runMs(10u * 60u * 1000u);
    const vdm_valve_state_t &s = r.m().valves[0];
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, s.mode);
    TEST_ASSERT_EQUAL_UINT16(650, s.target);
    TEST_ASSERT_UINT16_WITHIN(6, 650, s.actual);
    TEST_ASSERT_DOUBLE_WITHIN(0.006, 0.65, r.stm->board.v[0].x);
    TEST_ASSERT_TRUE(r.m().calibration_valid[0]);
    TEST_ASSERT_TRUE(r.m().detail_valid[0]);
}

static void test_noisy_wire(void)
{
    Rig r;
    r.to_stm.w.error_per_mille = 5;
    r.to_esp.w.error_per_mille = 5;
    r.runMs(3000);
    TEST_ASSERT_TRUE(r.client.setTarget(0, 400));
    TEST_ASSERT_TRUE(r.client.setTarget(1, 300));
    r.runMs(20u * 60u * 1000u);
    TEST_ASSERT_EQUAL(vdm::LinkState::Connected, r.m().state);
    TEST_ASSERT_TRUE(r.m().stats.retries > 0);
    TEST_ASSERT_UINT16_WITHIN(6, 400, r.m().valves[0].actual);
    TEST_ASSERT_UINT16_WITHIN(6, 300, r.m().valves[1].actual);
    TEST_ASSERT_DOUBLE_WITHIN(0.006, 0.40, r.stm->board.v[0].x);
    TEST_ASSERT_DOUBLE_WITHIN(0.006, 0.30, r.stm->board.v[1].x);
    /* every channel was calibrated exactly once despite retransmissions */
    TEST_ASSERT_EQUAL_UINT32(0, r.stm->board.violations);
}

static void test_stm_reboot_resends_targets(void)
{
    Rig r;
    r.runMs(2000);
    r.client.setTarget(0, 500);
    r.runMs(5u * 60u * 1000u);
    TEST_ASSERT_UINT16_WITHIN(6, 500, r.m().valves[0].actual);
    /* the target is not stored yet on the STM32 (lazy write), so after a
     * reboot it comes up with its startup target ... */
    r.reboot(0);
    r.runMs(5u * 60u * 1000u);
    TEST_ASSERT_EQUAL_UINT32(1, r.m().stats.stm_reboots);
    TEST_ASSERT_EQUAL(vdm::LinkState::Connected, r.m().state);
    /* ... and the ESP32 sends the user's target again */
    TEST_ASSERT_EQUAL_UINT16(500, r.m().valves[0].target);
    TEST_ASSERT_UINT16_WITHIN(6, 500, r.m().valves[0].actual);
}

static void test_silent_stm_requests_hard_reset(void)
{
    Rig r;
    r.runMs(2000);
    r.stm_alive = false;
    r.runMs(6000); /* noticed with the next poll (detail every 5 s) */
    TEST_ASSERT_EQUAL(vdm::LinkState::Disconnected, r.m().state);
    TEST_ASSERT_FALSE(r.client.takeHardResetRequest());
    r.runMs(25000); /* 30 s after the last answer */
    TEST_ASSERT_TRUE(r.client.takeHardResetRequest());
    r.runMs(60000);
    TEST_ASSERT_FALSE(r.client.takeHardResetRequest()); /* not again so soon */
    r.runMs(5u * 60u * 1000u);
    TEST_ASSERT_TRUE(r.client.takeHardResetRequest());
    r.stm_alive = true;
    r.runMs(3000);
    TEST_ASSERT_EQUAL(vdm::LinkState::Connected, r.m().state);
}

static void test_commands_and_errors(void)
{
    Rig r;
    r.runMs(2000);
    vdm_system_config_t c = r.m().sys;
    c.link_timeout_s = 120;
    TEST_ASSERT_TRUE(r.client.setSystemConfig(c));
    r.runMs(500);
    TEST_ASSERT_EQUAL_UINT16(120, r.m().sys.link_timeout_s);

    TEST_ASSERT_TRUE(r.client.setChannels(0x0001));
    r.runMs(500);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_DISABLED, r.m().valves[3].mode);
    /* a target for a disabled channel is rejected and dropped */
    r.client.setTarget(3, 200);
    r.runMs(500);
    TEST_ASSERT_TRUE(strlen(r.client.lastError()) > 0);
    r.runMs(5000);
    TEST_ASSERT_EQUAL_UINT32(0, r.m().stats.timeouts);

    TEST_ASSERT_TRUE(r.client.sensorScan());
    r.runMs(1000);
    TEST_ASSERT_TRUE(r.m().scan_valid);
    TEST_ASSERT_EQUAL_UINT8(3, r.m().scan.total);
}

static void test_suspend_for_update(void)
{
    Rig r;
    r.runMs(2000);
    r.client.suspend(r.now);
    r.runMs(60000);
    TEST_ASSERT_EQUAL(vdm::LinkState::Updating, r.m().state);
    TEST_ASSERT_FALSE(r.client.takeHardResetRequest());
    r.client.resume(r.now);
    r.runMs(3000);
    TEST_ASSERT_EQUAL(vdm::LinkState::Connected, r.m().state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_handshake_and_initial_data);
    RUN_TEST(test_target_via_link);
    RUN_TEST(test_noisy_wire);
    RUN_TEST(test_stm_reboot_resends_targets);
    RUN_TEST(test_silent_stm_requests_hard_reset);
    RUN_TEST(test_commands_and_errors);
    RUN_TEST(test_suspend_for_update);
    return UNITY_END();
}
