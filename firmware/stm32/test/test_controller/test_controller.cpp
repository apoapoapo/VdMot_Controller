#include <string.h>
#include <unity.h>

#include <vector>

#include "../support/fake_eeprom.h"
#include "../support/fake_onewire.h"
#include "../support/sim.h"
#include "core/controller.h"
#include "vdm/codec.h"

namespace
{

struct Frame {
    uint8_t kind, seq, msg;
    std::vector<uint8_t> payload;
};

/* ESP side: decodes everything the STM32 sends */
struct EspLink : vdm::LinkPort {
    vdm_rx_t rx;
    std::vector<Frame> frames;
    EspLink()
    {
        vdm_rx_init(&rx);
    }
    void write(const uint8_t *d, size_t n) override
    {
        for (size_t i = 0; i < n; i++) {
            vdm_frame_t f;
            if (vdm_rx_feed(&rx, d[i], &f)) {
                frames.push_back(
                    {f.kind, f.seq, f.msg, std::vector<uint8_t>(f.payload, f.payload + f.len)});
            }
        }
    }
};

struct FakeSystem : vdm::SystemPort {
    int resets = 0;
    int cleared = 0;
    void reset() override
    {
        resets++;
    }
    vdm::PlatformDiag diag() override
    {
        return {3, 1234, 345};
    }
    void clearDiag() override
    {
        cleared++;
    }
};

const vdm::BoardInfo kInfo = {2,
                              0,
                              0,
                              77,
                              VDM_HW_C2,
                              VDM_MCU_F411,
                              {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
                              0,
                              VDM_RESET_POWER_ON};

struct Rig {
    sim::FakeEeprom &eeprom;
    sim::Board board;
    vdm::MotorRun run{board};
    vdm::ValveManager vm{board, run};
    vdm::Storage storage{eeprom};
    sim::FakeBus bus;
    vdm::SensorManager sensors{bus};
    FakeSystem system;
    EspLink esp;
    vdm::Controller ctl{kInfo, vm, sensors, storage, system, esp};
    uint32_t now = 0;
    uint8_t seq = 1;

    explicit Rig(sim::FakeEeprom &e) : eeprom(e)
    {
        for (int ch = 1; ch < 12; ch++) {
            board.v[ch].connected = false; /* only channel 0 has a valve */
        }
        for (uint32_t i = 0; i < 14; i++) {
            sim::OwDevice d = sim::makeDevice(vdm::onewire::kFamilyDs18b20, 0x100 + i * 3);
            d.temp_mc = 20000 + static_cast<int32_t>(i) * 500;
            bus.devs.push_back(d);
        }
        ctl.begin(now);
    }

    void step()
    {
        int pulses;
        int16_t i = board.step(&pulses);
        for (int k = 0; k < pulses; k++)
            run.onPulse();
        run.onSample(i);
        now++;
        ctl.tick(now);
    }
    void runMs(uint32_t ms)
    {
        for (uint32_t k = 0; k < ms; k++)
            step();
    }

    void sendRaw(uint8_t s, uint8_t msg, const uint8_t *p, size_t n)
    {
        uint8_t wire[VDM_WIRE_MAX];
        size_t wn = 0;
        TEST_ASSERT_TRUE(vdm_frame_encode(VDM_KIND_REQUEST, s, msg, p, n, wire, sizeof(wire), &wn));
        for (size_t i = 0; i < wn; i++)
            ctl.onLinkByte(wire[i], now);
    }

    /* sends a request and returns its response payload */
    std::vector<uint8_t> request(uint8_t msg, const uint8_t *p = nullptr, size_t n = 0)
    {
        uint8_t s = seq++;
        size_t before = esp.frames.size();
        sendRaw(s, msg, p, n);
        for (size_t i = before; i < esp.frames.size(); i++) {
            const Frame &f = esp.frames[i];
            if (f.kind == VDM_KIND_RESPONSE && f.seq == s) {
                TEST_ASSERT_EQUAL_UINT8(msg, f.msg);
                return f.payload;
            }
        }
        TEST_FAIL_MESSAGE("no response");
        return {};
    }

    void hello()
    {
        vdm_hello_req_t h{VDM_PROTO_VERSION, VDM_HELLO_FLAG_RESET_SEQ};
        uint8_t b[2];
        size_t n;
        vdm_enc_hello_req(&h, b, sizeof(b), &n);
        std::vector<uint8_t> r = request(VDM_MSG_HELLO, b, n);
        TEST_ASSERT_EQUAL_UINT8(VDM_OK, r.at(0));
    }

    std::vector<uint8_t> setTarget(uint8_t ch, uint16_t t)
    {
        vdm_set_targets_t st{1, {{ch, t}}};
        uint8_t b[64];
        size_t n;
        vdm_enc_set_targets(&st, b, sizeof(b), &n);
        return request(VDM_MSG_SET_TARGETS, b, n);
    }

    std::vector<uint8_t> mask(uint8_t msg, uint16_t m)
    {
        uint8_t b[2];
        size_t n;
        vdm_enc_mask(m, b, sizeof(b), &n);
        return request(msg, b, n);
    }

    vdm_valve_state_t valveState(uint8_t ch)
    {
        std::vector<uint8_t> r = request(VDM_MSG_GET_VALVE_STATES);
        TEST_ASSERT_EQUAL_UINT8(VDM_OK, r.at(0));
        vdm_valve_states_t s;
        TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_valve_states(&r[1], r.size() - 1, &s, nullptr));
        return s.valves[ch];
    }

    vdm_diag_t diag()
    {
        std::vector<uint8_t> r = request(VDM_MSG_GET_DIAG);
        vdm_diag_t d;
        TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_diag(&r[1], r.size() - 1, &d));
        return d;
    }

    size_t count(uint8_t kind, uint8_t msg) const
    {
        size_t c = 0;
        for (const Frame &f : esp.frames)
            c += (f.kind == kind && f.msg == msg);
        return c;
    }

    bool settle(uint32_t max_ms = 20u * 60u * 1000u)
    {
        uint32_t quiet = 0;
        for (uint32_t k = 0; k < max_ms; k++) {
            step();
            quiet = vm.motorBusy() ? 0 : quiet + 1;
            if (quiet > 300)
                return true;
        }
        return false;
    }
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_boot_event_until_hello(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.runMs(3500);
    TEST_ASSERT_EQUAL(4, r.count(VDM_KIND_EVENT, VDM_EVT_BOOT));
    TEST_ASSERT_EQUAL(0, r.count(VDM_KIND_EVENT, VDM_EVT_VALVE));

    std::vector<uint8_t> nr = r.request(VDM_MSG_GET_VALVE_STATES);
    TEST_ASSERT_EQUAL_UINT8(VDM_ERR_NOT_READY, nr.at(0));

    vdm_hello_req_t h{VDM_PROTO_VERSION, 0};
    uint8_t b[2];
    size_t n;
    vdm_enc_hello_req(&h, b, sizeof(b), &n);
    std::vector<uint8_t> resp = r.request(VDM_MSG_HELLO, b, n);
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, resp.at(0));
    vdm_hello_t hi;
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_hello(&resp[1], resp.size() - 1, &hi));
    TEST_ASSERT_EQUAL_UINT8(1, hi.proto_version);
    TEST_ASSERT_EQUAL_UINT8(VDM_MCU_F411, hi.mcu);
    TEST_ASSERT_EQUAL_UINT32(1, hi.boot_count);
    TEST_ASSERT_EQUAL_HEX16(vdm_config_crc(&vdm_system_config_default, &vdm_motor_params_default),
                            hi.config_crc);

    r.runMs(3000);
    TEST_ASSERT_EQUAL(4, r.count(VDM_KIND_EVENT, VDM_EVT_BOOT)); /* stopped */
    TEST_ASSERT_TRUE(r.count(VDM_KIND_EVENT, VDM_EVT_VALVE) >= 12);
}

static void test_target_calibration_and_events(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.hello();
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, r.setTarget(0, 420).at(0));
    TEST_ASSERT_TRUE(r.settle());
    vdm_valve_state_t s = r.valveState(0);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, s.mode);
    TEST_ASSERT_UINT16_WITHIN(6, 420, s.actual);
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_NOT_CONNECTED, r.valveState(1).mode);
    TEST_ASSERT_EQUAL(1, r.count(VDM_KIND_EVENT, VDM_EVT_CALIBRATION));
    /* the last EVT_VALVE of channel 0 reports the final state */
    for (size_t i = r.esp.frames.size(); i-- > 0;) {
        const Frame &f = r.esp.frames[i];
        if (f.kind == VDM_KIND_EVENT && f.msg == VDM_EVT_VALVE && f.payload[0] == 0) {
            vdm_valve_state_t ev;
            TEST_ASSERT_EQUAL(
                VDM_OK, vdm_dec_valve_state(f.payload.data(), f.payload.size(), &ev, nullptr));
            TEST_ASSERT_EQUAL_UINT16(s.actual, ev.actual);
            TEST_ASSERT_EQUAL_UINT8(VDM_MODE_IDLE, ev.mode);
            break;
        }
    }
}

static void test_retransmission_is_not_executed_twice(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.hello();
    TEST_ASSERT_TRUE(r.settle());
    size_t cal_before = r.count(VDM_KIND_EVENT, VDM_EVT_CALIBRATION);

    uint8_t b[2];
    size_t n;
    vdm_enc_mask(0x0001, b, sizeof(b), &n);
    r.sendRaw(50, VDM_MSG_CALIBRATE, b, n);
    r.sendRaw(50, VDM_MSG_CALIBRATE, b, n); /* same seq: retry */
    TEST_ASSERT_TRUE(r.settle());
    r.sendRaw(50, VDM_MSG_CALIBRATE, b, n); /* still the same seq */
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL(cal_before + 1, r.count(VDM_KIND_EVENT, VDM_EVT_CALIBRATION));
    TEST_ASSERT_EQUAL_UINT16(2, r.diag().rx_duplicate);
    TEST_ASSERT_EQUAL(3, r.count(VDM_KIND_RESPONSE, VDM_MSG_CALIBRATE));
}

static void test_config_validation_and_crc(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.hello();
    vdm_system_config_t c = vdm_system_config_default;
    uint8_t b[16];
    size_t n;
    vdm_enc_system_config(&c, b, sizeof(b), &n);
    b[6] = 1; /* sensor_interval_s = 1: field 3 out of range */
    b[7] = 0;
    std::vector<uint8_t> resp = r.request(VDM_MSG_SET_SYSTEM_CONFIG, b, n);
    TEST_ASSERT_EQUAL_UINT8(VDM_ERR_RANGE, resp.at(0));
    TEST_ASSERT_EQUAL_UINT8(3, resp.at(1));

    c.failsafe_pos = 500;
    vdm_enc_system_config(&c, b, sizeof(b), &n);
    resp = r.request(VDM_MSG_SET_SYSTEM_CONFIG, b, n);
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, resp.at(0));
    uint16_t crc = (uint16_t)(resp.at(1) | (resp.at(2) << 8));
    TEST_ASSERT_EQUAL_HEX16(vdm_config_crc(&c, &vdm_motor_params_default), crc);

    resp = r.request(VDM_MSG_GET_SYSTEM_CONFIG);
    vdm_system_config_t back;
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_system_config(&resp[1], resp.size() - 1, &back, nullptr));
    TEST_ASSERT_EQUAL_UINT16(500, back.failsafe_pos);
}

static void test_disabled_channel_and_unknown_message(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.hello();
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, r.mask(VDM_MSG_SET_CHANNELS, 0x0001).at(0));
    std::vector<uint8_t> resp = r.setTarget(5, 300);
    TEST_ASSERT_EQUAL_UINT8(VDM_ERR_CHANNEL, resp.at(0));
    TEST_ASSERT_EQUAL_UINT8(0, resp.at(1));
    resp = r.mask(VDM_MSG_CALIBRATE, 0x0006);
    TEST_ASSERT_EQUAL_UINT8(VDM_ERR_CHANNEL, resp.at(0));
    TEST_ASSERT_EQUAL_UINT8(1, resp.at(1));
    TEST_ASSERT_EQUAL_UINT8(VDM_MODE_DISABLED, r.valveState(5).mode);
    resp = r.request(0x7E);
    TEST_ASSERT_EQUAL_UINT8(VDM_ERR_UNKNOWN_MSG, resp.at(0));
}

static void test_failsafe_opens_and_ends_on_request(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.hello();
    vdm_system_config_t c = vdm_system_config_default;
    c.link_timeout_s = 30;
    uint8_t b[16];
    size_t n;
    vdm_enc_system_config(&c, b, sizeof(b), &n);
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, r.request(VDM_MSG_SET_SYSTEM_CONFIG, b, n).at(0));
    r.setTarget(0, 200);
    TEST_ASSERT_TRUE(r.settle());

    r.runMs(31000); /* no requests */
    TEST_ASSERT_TRUE(r.ctl.failsafeActive());
    TEST_ASSERT_TRUE(r.settle());
    TEST_ASSERT_EQUAL_DOUBLE(1.0, r.board.v[0].x); /* fully open */
    TEST_ASSERT_EQUAL(0, r.count(VDM_KIND_EVENT, VDM_EVT_LINK_TIMEOUT));

    vdm_valve_state_t s = r.valveState(0); /* any request ends the failsafe */
    TEST_ASSERT_FALSE(r.ctl.failsafeActive());
    TEST_ASSERT_EQUAL_UINT16(1000, s.target);
    r.runMs(10);
    TEST_ASSERT_EQUAL(1, r.count(VDM_KIND_EVENT, VDM_EVT_LINK_TIMEOUT));
    TEST_ASSERT_FALSE(r.valveState(0).flags & VDM_VFLAG_FAILSAFE);
}

static void test_restart_restores_calibration(void)
{
    sim::FakeEeprom e;
    double x;
    {
        Rig r(e);
        r.hello();
        r.setTarget(0, 600);
        TEST_ASSERT_TRUE(r.settle());
        /* targets are written lazily; keep the link alive meanwhile */
        for (uint32_t t = 0; t < vdm::Controller::kLazyWriteMs + 60000; t += 60000) {
            r.runMs(60000);
            r.request(VDM_MSG_PING);
        }
        x = r.board.v[0].x;
        TEST_ASSERT_TRUE(r.storage.idle());
    }
    Rig r2(e);
    r2.board.v[0].x = x;
    r2.hello();
    TEST_ASSERT_TRUE(r2.settle());
    TEST_ASSERT_EQUAL(0, r2.count(VDM_KIND_EVENT, VDM_EVT_CALIBRATION)); /* reference run only */
    vdm_valve_state_t s = r2.valveState(0);
    TEST_ASSERT_TRUE(s.flags & VDM_VFLAG_CALIBRATED);
    TEST_ASSERT_UINT16_WITHIN(6, 600, s.actual); /* stored target restored */

    std::vector<uint8_t> resp = r2.request(VDM_MSG_GET_VALVE_DETAIL, (const uint8_t[]){0}, 1);
    vdm_valve_detail_t d;
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_valve_detail(&resp[1], resp.size() - 1, &d, nullptr));
    TEST_ASSERT_UINT32_WITHIN(10, 6000, d.open_counts);
}

static void test_reset_flushes_and_resets(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.hello();
    uint8_t b[4];
    size_t n;
    vdm_enc_reset(b, sizeof(b), &n);
    b[0] ^= 1;
    TEST_ASSERT_EQUAL_UINT8(VDM_ERR_RANGE, r.request(VDM_MSG_RESET, b, n).at(0));
    vdm_enc_reset(b, sizeof(b), &n);
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, r.request(VDM_MSG_RESET, b, n).at(0));
    TEST_ASSERT_EQUAL(0, r.system.resets);
    r.step();
    TEST_ASSERT_EQUAL(1, r.system.resets);
    TEST_ASSERT_TRUE(r.storage.idle());
}

static void test_diag_and_ping(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.hello();
    const uint8_t data[] = {1, 2, 0, 4};
    std::vector<uint8_t> resp = r.request(VDM_MSG_PING, data, sizeof(data));
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, resp.at(0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, &resp[1], 4);
    vdm_diag_t d = r.diag();
    TEST_ASSERT_EQUAL_UINT16(3, d.uart_hw_err);
    TEST_ASSERT_EQUAL_UINT32(1234, d.loop_max_us);
    TEST_ASSERT_TRUE(d.rx_frames_ok >= 3);
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, r.request(VDM_MSG_CLEAR_DIAG).at(0));
    TEST_ASSERT_EQUAL(1, r.system.cleared);
    TEST_ASSERT_EQUAL_UINT32(1, r.diag().rx_frames_ok);
}

static void test_sensors_events_and_paging(void)
{
    sim::FakeEeprom e;
    Rig r(e);
    r.hello();
    r.runMs(32000); /* scan at boot, cycles at 0 s and 30 s */
    TEST_ASSERT_TRUE(r.count(VDM_KIND_EVENT, VDM_EVT_SENSORS) >= 2);

    std::vector<uint8_t> resp = r.request(VDM_MSG_GET_SENSORS, (const uint8_t[]){0}, 1);
    vdm_sensors_t s;
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_sensors(&resp[1], resp.size() - 1, &s, nullptr));
    TEST_ASSERT_EQUAL_UINT8(14, s.total);
    TEST_ASSERT_EQUAL_UINT8(12, s.count);
    TEST_ASSERT_EQUAL_UINT8(VDM_SSTAT_OK, s.sensors[0].status);
    resp = r.request(VDM_MSG_GET_SENSORS, (const uint8_t[]){12}, 1);
    TEST_ASSERT_EQUAL(VDM_OK, vdm_dec_sensors(&resp[1], resp.size() - 1, &s, nullptr));
    TEST_ASSERT_EQUAL_UINT8(2, s.count);
    TEST_ASSERT_EQUAL_UINT8(12, s.offset);

    /* rescan: busy while requested, then an EVT_SENSOR_SCAN_DONE */
    size_t scans = r.count(VDM_KIND_EVENT, VDM_EVT_SENSOR_SCAN_DONE); /* boot scan */
    TEST_ASSERT_EQUAL_UINT8(VDM_OK, r.request(VDM_MSG_SENSOR_SCAN).at(0));
    TEST_ASSERT_EQUAL_UINT8(VDM_ERR_BUSY, r.request(VDM_MSG_SENSOR_SCAN).at(0));
    r.runMs(500);
    TEST_ASSERT_EQUAL(scans + 1, r.count(VDM_KIND_EVENT, VDM_EVT_SENSOR_SCAN_DONE));
    for (const Frame &f : r.esp.frames) {
        if (f.kind == VDM_KIND_EVENT && f.msg == VDM_EVT_SENSOR_SCAN_DONE) {
            vdm_evt_scan_done_t d;
            TEST_ASSERT_EQUAL(VDM_OK,
                              vdm_dec_evt_scan_done(f.payload.data(), f.payload.size(), &d));
            TEST_ASSERT_EQUAL_UINT8(14, d.total);
            TEST_ASSERT_EQUAL_UINT8(1, d.bus_ok);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_event_until_hello);
    RUN_TEST(test_target_calibration_and_events);
    RUN_TEST(test_retransmission_is_not_executed_twice);
    RUN_TEST(test_config_validation_and_crc);
    RUN_TEST(test_disabled_channel_and_unknown_message);
    RUN_TEST(test_failsafe_opens_and_ends_on_request);
    RUN_TEST(test_restart_restores_calibration);
    RUN_TEST(test_reset_flushes_and_resets);
    RUN_TEST(test_diag_and_ping);
    RUN_TEST(test_sensors_events_and_paging);
    return UNITY_END();
}
